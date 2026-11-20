#include "d3d11_video_hevc.h"
#include "d3d11_video_rbsp.h"

#include "../util/util_bit.h"
#include "../util/util_math.h"

#include <algorithm>
#include <cstring>
#include <iterator>

namespace dxvk {

  namespace {

    struct H265LevelLimits {
      uint32_t pictureSamples;
      uint16_t sliceSegments;
      uint8_t tileColumns;
      uint8_t tileRows;
    };

    // H.265 Annex A, in StdVideoH265LevelIdc order. DXVA omits the signalled level.
    constexpr H265LevelLimits h265Levels[] = {
      {    36864,  16,  1,  1 }, {   122880,  16,  1,  1 }, {   245760,  20,  1,  1 },
      {   552960,  30,  2,  2 }, {   983040,  40,  3,  3 }, {  2228224,  75,  5,  5 },
      {  2228224,  75,  5,  5 }, {  8912896, 200, 10, 11 }, {  8912896, 200, 10, 11 },
      {  8912896, 200, 10, 11 }, { 35651584, 600, 20, 22 }, { 35651584, 600, 20, 22 },
      { 35651584, 600, 20, 22 },
    };
    static_assert(std::size(h265Levels) == STD_VIDEO_H265_LEVEL_IDC_6_2 + 1);

    StdVideoH265LevelIdc getH265Level(
      const DXVA_PicParams_HEVC& p, VkExtent2D extent, size_t slices, StdVideoH265LevelIdc maxLevel) {
      if (uint32_t(maxLevel) >= std::size(h265Levels))
        return STD_VIDEO_H265_LEVEL_IDC_INVALID;

      const uint64_t samples = uint64_t(extent.width) * extent.height;
      for (uint32_t i = 0; i <= uint32_t(maxLevel); i++) {
        const auto& limit = h265Levels[i];
        const uint32_t dpbSlots = samples <= limit.pictureSamples / 4 ? 16
                                : samples <= limit.pictureSamples / 2 ? 12
                                : samples <= limit.pictureSamples * 3 / 4 ? 8 : 6;
        if (h265LevelSupportsExtent(StdVideoH265LevelIdc(i), extent)
         && p.sps_max_dec_pic_buffering_minus1 < dpbSlots && slices <= limit.sliceSegments
         && (!p.tiles_enabled_flag || (p.num_tile_columns_minus1 < limit.tileColumns
                                   && p.num_tile_rows_minus1 < limit.tileRows)))
          return StdVideoH265LevelIdc(i);
      }
      return STD_VIDEO_H265_LEVEL_IDC_INVALID;
    }


    bool usesSliceLocalRps(VideoRbspReader& reader, const DXVA_PicParams_HEVC& p, bool first, uint32_t ctbCount) {
      if (!first) {
        bool dependent = p.dependent_slice_segments_enabled_flag && reader.readBits(1);
        uint32_t address = reader.readBits(32u - bit::lzcnt(ctbCount - 1));
        if (!reader.valid() || !address || address >= ctbCount)
          return false;
        if (dependent)
          return true; // Inherits the preceding independent slice header.
      }

      reader.readBits(p.num_extra_slice_header_bits);
      uint32_t sliceType = reader.readUe();
      if (sliceType > 2 || (p.IrapPicFlag && sliceType != 2))
        return false;
      if (p.output_flag_present_flag)
        reader.readBits(1);

      if (!p.IdrPicFlag) {
        reader.readBits(p.log2_max_pic_order_cnt_lsb_minus4 + 4);
        if (reader.readBits(1)) // short_term_ref_pic_set_sps_flag
          return false;
        if (p.num_short_term_ref_pic_sets && reader.readBits(1)) // inter_ref_pic_set_prediction_flag
          return false;
      }
      return reader.valid();
    }


    // DXVA omits the PPS ID, temporal ID, and reference flag. All slices of
    // a progressive picture must identify the same layer, NAL type, and PPS.
    bool parseH265Slices(
      const DXVA_PicParams_HEVC& p, const std::vector<uint8_t>& bytes, const std::vector<uint32_t>& offsets,
      uint32_t& ppsId, uint32_t& temporalId, bool& isReference) {
      uint32_t ctbSizeInMinCbs = 1u << p.log2_diff_max_min_luma_coding_block_size;
      uint32_t ctbCount = divCeil(uint32_t(p.PicWidthInMinCbsY), ctbSizeInMinCbs)
                        * divCeil(uint32_t(p.PicHeightInMinCbsY), ctbSizeInMinCbs);
      if (offsets.empty() || offsets.size() > ctbCount)
        return false;

      uint32_t pictureNalType = 0;
      for (size_t i = 0; i < offsets.size(); i++) {
        const size_t start = offsets[i];
        const size_t end = i + 1 < offsets.size() ? offsets[i + 1] : bytes.size();
        if (start >= end || end > bytes.size() || end - start < 6)
          return false;

        const uint8_t* data = bytes.data() + start;
        if (data[0] || data[1] || data[2] != 1 || (data[3] & 0x81) || (data[4] & 0xf8) || !(data[4] & 7))
          return false;

        uint32_t nalType = data[3] >> 1;
        bool irap = nalType >= 16 && nalType <= 21;
        if ((!irap && nalType > 9) || irap != bool(p.IrapPicFlag) || (nalType == 19 || nalType == 20) != bool(p.IdrPicFlag))
          return false;

        VideoRbspReader reader(data + 5, bytes.data() + end);
        if (reader.readBits(1) != (i == 0)) // first_slice_segment_in_pic_flag
          return false;

        if (irap)
          reader.readBits(1); // no_output_of_prior_pics_flag

        uint32_t id = reader.readUe();
        if (id > 63 || !usesSliceLocalRps(reader, p, i == 0, ctbCount))
          return false;

        const uint32_t tid = (data[4] & 7) - 1;
        if (!i) {
          ppsId = id;
          temporalId = tid;
          pictureNalType = nalType;
          isReference = irap || (nalType & 1);
        } else if (id != ppsId || tid != temporalId || nalType != pictureNalType) {
          return false;
        }
      }

      return true;
    }


    bool translateH265References(const DXVA_PicParams_HEVC& p, DxvkH265PictureParameters& result) {
      uint8_t indices[std::size(p.RefPicList)];
      std::fill(std::begin(indices), std::end(indices), uint8_t(0xff));
      std::array<bool, 128> seen = { };
      seen[p.CurrPic.Index7Bits] = true;

      for (size_t i = 0; i < std::size(p.RefPicList); i++) {
        const auto entry = p.RefPicList[i];
        if (entry.bPicEntry == 0xff)
          continue;
        if (seen[entry.Index7Bits])
          return false;

        seen[entry.Index7Bits] = true;
        indices[i] = uint8_t(result.referenceCount);
        auto& ref = result.references[result.referenceCount++];
        ref.pictureId = entry.Index7Bits;
        ref.info.PicOrderCntVal = p.PicOrderCntValList[i];
        ref.info.flags.used_for_long_term_reference = entry.AssociatedFlag;
      }

      // The DXVA arrays index RefPicList. Compact holes here; the backend
      // converts these reference indices to the actual Vulkan DPB slots.
      bool used[std::size(p.RefPicList)] = { };
      auto translateSet = [&](const auto& src, auto& dst, bool longTerm) {
        for (size_t i = 0; i < std::size(src); i++) {
          uint8_t idx = src[i];
          if (idx == 0xff) {
            dst[i] = 0xff;
            continue;
          }
          if (idx >= std::size(indices) || indices[idx] == 0xff || used[idx]
           || bool(p.RefPicList[idx].AssociatedFlag) != longTerm)
            return false;

          used[idx] = true;
          dst[i] = indices[idx];
        }
        return true;
      };

      return result.referenceCount <= p.sps_max_dec_pic_buffering_minus1
          && translateSet(p.RefPicSetStCurrBefore, result.picture.RefPicSetStCurrBefore, false)
          && translateSet(p.RefPicSetStCurrAfter, result.picture.RefPicSetStCurrAfter, false)
          && translateSet(p.RefPicSetLtCurr, result.picture.RefPicSetLtCurr, true);
    }

  }


  bool h265LevelSupportsExtent(StdVideoH265LevelIdc level, VkExtent2D extent) {
    if (uint32_t(level) >= std::size(h265Levels) || !extent.width || !extent.height)
      return false;

    const uint32_t limit = h265Levels[level].pictureSamples;
    return uint64_t(extent.width) * extent.height <= limit
        && uint64_t(extent.width) * extent.width <= 8u * limit
        && uint64_t(extent.height) * extent.height <= 8u * limit;
  }


  bool translateH265PictureParameters(
    const DXVA_PicParams_HEVC& p, const DXVA_Qmatrix_HEVC* matrix, const std::vector<uint8_t>& bitstream,
    const std::vector<uint32_t>& sliceOffsets, StdVideoH265ProfileIdc profile, StdVideoH265LevelIdc maxLevel,
    DxvkH265PictureParameters& result, uint8_t previousMaxTemporalId) {
    if ((profile != STD_VIDEO_H265_PROFILE_IDC_MAIN && profile != STD_VIDEO_H265_PROFILE_IDC_MAIN_10)
     || p.chroma_format_idc != 1 || p.separate_colour_plane_flag
     || p.bit_depth_chroma_minus8 != p.bit_depth_luma_minus8
     || p.bit_depth_luma_minus8 != (profile == STD_VIDEO_H265_PROFILE_IDC_MAIN_10 ? 2 : 0)
     || p.CurrPic.AssociatedFlag || p.sps_max_dec_pic_buffering_minus1 >= 16
     || p.log2_max_pic_order_cnt_lsb_minus4 > 12 || p.log2_min_luma_coding_block_size_minus3 > 3
     || p.log2_min_luma_coding_block_size_minus3 + p.log2_diff_max_min_luma_coding_block_size > 3
     || p.log2_min_transform_block_size_minus2 + p.log2_diff_max_min_transform_block_size > 3
     || p.num_ref_idx_l0_default_active_minus1 > 14 || p.num_ref_idx_l1_default_active_minus1 > 14
     || previousMaxTemporalId > 6 || (p.scaling_list_enabled_flag && !matrix))
      return false;

    // The slice parser rejects lookup and prediction from SPS tables, since DXVA
    // does not supply their entries. Preserve the short-term count because it
    // determines whether the inline set contains a prediction flag.
    if (p.num_short_term_ref_pic_sets > STD_VIDEO_H265_MAX_SHORT_TERM_REF_PIC_SETS || p.num_long_term_ref_pics_sps)
      return false;

    const uint32_t minCb = 8u << p.log2_min_luma_coding_block_size_minus3;
    const VkExtent2D extent = { p.PicWidthInMinCbsY * minCb, p.PicHeightInMinCbsY * minCb };
    auto level = getH265Level(p, extent, sliceOffsets.size(), maxLevel);
    if (level == STD_VIDEO_H265_LEVEL_IDC_INVALID)
      return false;

    uint32_t ppsId = 0, temporalId = 0;
    bool isReference = false;
    if (!parseH265Slices(p, bitstream, sliceOffsets, ppsId, temporalId, isReference))
      return false;

    result = { };
    auto& sps = result.sps;
    auto& pps = result.pps;
    auto& pic = result.picture;
    result.profileTierLevel.general_profile_idc = profile;
    result.profileTierLevel.general_level_idc = level;
    result.profileTierLevel.flags.general_progressive_source_flag = 1;
    result.profileTierLevel.flags.general_frame_only_constraint_flag = 1;
    const uint32_t maxTemporalId = p.IdrPicFlag ? temporalId : std::max<uint32_t>(previousMaxTemporalId, temporalId);
    result.vps.vps_max_sub_layers_minus1 = maxTemporalId;
    result.vps.flags.vps_temporal_id_nesting_flag = !maxTemporalId;
    sps.sps_max_sub_layers_minus1 = maxTemporalId;
    sps.flags.sps_temporal_id_nesting_flag = !maxTemporalId;
    for (uint32_t i = 0; i <= maxTemporalId; i++) {
      result.dpbMgr.max_dec_pic_buffering_minus1[i] = p.sps_max_dec_pic_buffering_minus1;
      result.dpbMgr.max_num_reorder_pics[i] = p.NoPicReorderingFlag ? 0 : p.sps_max_dec_pic_buffering_minus1;
    }

    sps.chroma_format_idc = STD_VIDEO_H265_CHROMA_FORMAT_IDC_420;
    sps.pic_width_in_luma_samples = extent.width;
    sps.pic_height_in_luma_samples = extent.height;
    sps.bit_depth_luma_minus8 = p.bit_depth_luma_minus8;
    sps.bit_depth_chroma_minus8 = p.bit_depth_chroma_minus8;
    sps.num_short_term_ref_pic_sets = p.num_short_term_ref_pic_sets;
    sps.log2_max_pic_order_cnt_lsb_minus4 = p.log2_max_pic_order_cnt_lsb_minus4;
    sps.log2_min_luma_coding_block_size_minus3 = p.log2_min_luma_coding_block_size_minus3;
    sps.log2_diff_max_min_luma_coding_block_size = p.log2_diff_max_min_luma_coding_block_size;
    sps.log2_min_luma_transform_block_size_minus2 = p.log2_min_transform_block_size_minus2;
    sps.log2_diff_max_min_luma_transform_block_size = p.log2_diff_max_min_transform_block_size;
    sps.max_transform_hierarchy_depth_inter = p.max_transform_hierarchy_depth_inter;
    sps.max_transform_hierarchy_depth_intra = p.max_transform_hierarchy_depth_intra;
    sps.flags.amp_enabled_flag = p.amp_enabled_flag;
    sps.flags.sample_adaptive_offset_enabled_flag = p.sample_adaptive_offset_enabled_flag;
    sps.flags.pcm_enabled_flag = p.pcm_enabled_flag;
    sps.flags.pcm_loop_filter_disabled_flag = p.pcm_loop_filter_disabled_flag;
    sps.pcm_sample_bit_depth_luma_minus1 = p.pcm_sample_bit_depth_luma_minus1;
    sps.pcm_sample_bit_depth_chroma_minus1 = p.pcm_sample_bit_depth_chroma_minus1;
    sps.log2_min_pcm_luma_coding_block_size_minus3 = p.log2_min_pcm_luma_coding_block_size_minus3;
    sps.log2_diff_max_min_pcm_luma_coding_block_size = p.log2_diff_max_min_pcm_luma_coding_block_size;
    sps.flags.long_term_ref_pics_present_flag = p.long_term_ref_pics_present_flag;
    sps.flags.sps_temporal_mvp_enabled_flag = p.sps_temporal_mvp_enabled_flag;
    sps.flags.strong_intra_smoothing_enabled_flag = p.strong_intra_smoothing_enabled_flag;
    sps.flags.scaling_list_enabled_flag = p.scaling_list_enabled_flag;
    sps.flags.sps_scaling_list_data_present_flag = p.scaling_list_enabled_flag;
    if (p.scaling_list_enabled_flag) {
      // The effective DXVA matrices and the StdVideo matrices both contain
      // the same scaling factors in diagonal scan order.
      auto& scaling = result.scalingLists;
      std::memcpy(scaling.ScalingList4x4, matrix->ucScalingLists0, sizeof(scaling.ScalingList4x4));
      std::memcpy(scaling.ScalingList8x8, matrix->ucScalingLists1, sizeof(scaling.ScalingList8x8));
      std::memcpy(scaling.ScalingList16x16, matrix->ucScalingLists2, sizeof(scaling.ScalingList16x16));
      std::memcpy(scaling.ScalingList32x32, matrix->ucScalingLists3, sizeof(scaling.ScalingList32x32));
      std::memcpy(scaling.ScalingListDCCoef16x16, matrix->ucScalingListDCCoefSizeID2, sizeof(scaling.ScalingListDCCoef16x16));
      std::memcpy(scaling.ScalingListDCCoef32x32, matrix->ucScalingListDCCoefSizeID3, sizeof(scaling.ScalingListDCCoef32x32));
    }

    // PPS IDs occur in the slice header. The VPS and SPS IDs only occur
    // in parameter-set NAL units, which DXVA does not submit.
    pps.pps_pic_parameter_set_id = ppsId;
    pps.flags.dependent_slice_segments_enabled_flag = p.dependent_slice_segments_enabled_flag;
    pps.flags.output_flag_present_flag = p.output_flag_present_flag;
    pps.num_extra_slice_header_bits = p.num_extra_slice_header_bits;
    pps.flags.sign_data_hiding_enabled_flag = p.sign_data_hiding_enabled_flag;
    pps.flags.cabac_init_present_flag = p.cabac_init_present_flag;
    pps.num_ref_idx_l0_default_active_minus1 = p.num_ref_idx_l0_default_active_minus1;
    pps.num_ref_idx_l1_default_active_minus1 = p.num_ref_idx_l1_default_active_minus1;
    pps.init_qp_minus26 = p.init_qp_minus26;
    pps.flags.constrained_intra_pred_flag = p.constrained_intra_pred_flag;
    pps.flags.transform_skip_enabled_flag = p.transform_skip_enabled_flag;
    pps.flags.cu_qp_delta_enabled_flag = p.cu_qp_delta_enabled_flag;
    pps.diff_cu_qp_delta_depth = p.diff_cu_qp_delta_depth;
    pps.pps_cb_qp_offset = p.pps_cb_qp_offset;
    pps.pps_cr_qp_offset = p.pps_cr_qp_offset;
    pps.flags.pps_slice_chroma_qp_offsets_present_flag = p.pps_slice_chroma_qp_offsets_present_flag;
    pps.flags.weighted_pred_flag = p.weighted_pred_flag;
    pps.flags.weighted_bipred_flag = p.weighted_bipred_flag;
    pps.flags.transquant_bypass_enabled_flag = p.transquant_bypass_enabled_flag;
    pps.flags.tiles_enabled_flag = p.tiles_enabled_flag;
    pps.flags.entropy_coding_sync_enabled_flag = p.entropy_coding_sync_enabled_flag;
    if (p.tiles_enabled_flag) {
      pps.num_tile_columns_minus1 = p.num_tile_columns_minus1;
      pps.num_tile_rows_minus1 = p.num_tile_rows_minus1;
      pps.flags.uniform_spacing_flag = p.uniform_spacing_flag;
      pps.flags.loop_filter_across_tiles_enabled_flag = p.loop_filter_across_tiles_enabled_flag;
      if (!p.uniform_spacing_flag) {
        std::copy_n(p.column_width_minus1, p.num_tile_columns_minus1, pps.column_width_minus1);
        std::copy_n(p.row_height_minus1, p.num_tile_rows_minus1, pps.row_height_minus1);
      }
    }
    pps.flags.pps_loop_filter_across_slices_enabled_flag = p.pps_loop_filter_across_slices_enabled_flag;
    pps.flags.deblocking_filter_control_present_flag = p.deblocking_filter_override_enabled_flag
                                                   || p.pps_deblocking_filter_disabled_flag
                                                   || p.pps_beta_offset_div2 || p.pps_tc_offset_div2;
    pps.flags.deblocking_filter_override_enabled_flag = p.deblocking_filter_override_enabled_flag;
    pps.flags.pps_deblocking_filter_disabled_flag = p.pps_deblocking_filter_disabled_flag;
    pps.pps_beta_offset_div2 = p.pps_beta_offset_div2;
    pps.pps_tc_offset_div2 = p.pps_tc_offset_div2;
    pps.flags.lists_modification_present_flag = p.lists_modification_present_flag;
    pps.log2_parallel_merge_level_minus2 = p.log2_parallel_merge_level_minus2;
    pps.flags.slice_segment_header_extension_present_flag = p.slice_segment_header_extension_present_flag;

    pic.flags.IrapPicFlag = p.IrapPicFlag;
    pic.flags.IdrPicFlag = p.IdrPicFlag;
    pic.flags.IsReference = isReference;
    pic.pps_pic_parameter_set_id = ppsId;
    pic.PicOrderCntVal = p.CurrPicOrderCntVal;
    // No supported inline set is predicted from another set, so
    // NumDeltaPocsOfRefRpsIdx and short_term_ref_pic_set_sps_flag stay zero.
    pic.NumBitsForSTRefPicSetInSlice = p.IdrPicFlag ? 0 : p.wNumBitsForShortTermRPSInSlice;
    result.currentReference.PicOrderCntVal = p.CurrPicOrderCntVal;
    result.currentPictureId = p.CurrPic.Index7Bits;
    if (!translateH265References(p, result))
      return false;

    result.sliceOffsets = sliceOffsets;
    return true;
  }

}
