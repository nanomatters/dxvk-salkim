#include "d3d11_video_h264.h"
#include "d3d11_video_rbsp.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <utility>

namespace dxvk {

  namespace {

    struct H264LevelLimits {
      uint32_t maxFrameMbs;
      uint32_t maxDpbMbs;
    };

    // H.264 Table A-1, indexed by StdVideoH264LevelIdc.
    constexpr H264LevelLimits h264LevelLimits[] = {
      {     99,    396 },  // 1.0
      {    396,    900 },  // 1.1
      {    396,   2376 },  // 1.2
      {    396,   2376 },  // 1.3
      {    396,   2376 },  // 2.0
      {    792,   4752 },  // 2.1
      {   1620,   8100 },  // 2.2
      {   1620,   8100 },  // 3.0
      {   3600,  18000 },  // 3.1
      {   5120,  20480 },  // 3.2
      {   8192,  32768 },  // 4.0
      {   8192,  32768 },  // 4.1
      {   8704,  34816 },  // 4.2
      {  22080, 110400 },  // 5.0
      {  36864, 184320 },  // 5.1
      {  36864, 184320 },  // 5.2
      { 139264, 696320 },  // 6.0
      { 139264, 696320 },  // 6.1
      { 139264, 696320 },  // 6.2
    };

    static_assert(std::size(h264LevelLimits) == STD_VIDEO_H264_LEVEL_IDC_6_2 + 1);

    bool h264LevelSupportsPicture(
            uint32_t                level,
            uint32_t                widthInMbs,
            uint32_t                heightInMbs,
            uint32_t                referenceCount) {
      if (level >= std::size(h264LevelLimits) || !widthInMbs || !heightInMbs)
        return false;

      const auto& limits = h264LevelLimits[level];
      const uint64_t frameMbs = uint64_t(widthInMbs) * heightInMbs;

      return frameMbs <= limits.maxFrameMbs
          && uint64_t(widthInMbs) * widthInMbs <= 8 * limits.maxFrameMbs
          && uint64_t(heightInMbs) * heightInMbs <= 8 * limits.maxFrameMbs
          && frameMbs * referenceCount <= limits.maxDpbMbs;
    }


    StdVideoH264LevelIdc getH264Level(
      const DXVA_PicParams_H264&     params,
            StdVideoH264LevelIdc    maxLevel) {
      if (uint32_t(maxLevel) >= std::size(h264LevelLimits))
        return STD_VIDEO_H264_LEVEL_IDC_INVALID;

      // DXVA omits level_idc. An 8x8 minimum for biprediction implies level
      // 3.1 or higher. The picture size and reference count can raise it further.
      const uint32_t minLevel = params.MinLumaBipredSize8x8Flag
        ? STD_VIDEO_H264_LEVEL_IDC_3_1 : STD_VIDEO_H264_LEVEL_IDC_1_0;

      for (uint32_t level = minLevel; level <= uint32_t(maxLevel); level++) {
        if (h264LevelSupportsPicture(level, params.wFrameWidthInMbsMinus1 + 1u,
                params.wFrameHeightInMbsMinus1 + 1u, params.num_ref_frames))
          return StdVideoH264LevelIdc(level);
      }

      return STD_VIDEO_H264_LEVEL_IDC_INVALID;
    }


    struct H264SliceHeader {
      uint32_t firstMb;
      uint32_t ppsId;
      uint32_t idrPicId;
      uint32_t longTermFrameIndex;
      bool isIntra;
      bool isIdr;
      bool isLongTerm;
    };


    bool skipH264ReferenceListModification(VideoRbspReader& reader, uint32_t refCount) {
      if (!reader.readBits(1))
        return reader.valid();

      for (uint32_t i = 0; i <= refCount && reader.valid(); i++) {
        const uint32_t operation = reader.readUe();
        if (operation == 3)
          return true;

        if (operation > 3)
          return false;

        reader.readUe(); // abs_diff_pic_num_minus1 or long_term_pic_num
      }

      return false;
    }


    bool skipH264PredictionWeights(VideoRbspReader& reader, uint32_t refCountL0, uint32_t refCountL1) {
      if (reader.readUe() > 7 || reader.readUe() > 7)
        return false;

      for (uint32_t refCount : { refCountL0, refCountL1 }) {
        for (uint32_t i = 0; i < refCount && reader.valid(); i++) {
          if (reader.readBits(1)) {
            reader.readUe(); // luma_weight_lX
            reader.readUe(); // luma_offset_lX
          }

          if (reader.readBits(1)) {
            for (uint32_t component = 0; component < 2; component++) {
              reader.readUe(); // chroma_weight_lX
              reader.readUe(); // chroma_offset_lX
            }
          }
        }
      }

      return reader.valid();
    }


    bool parseH264ReferenceMarking(VideoRbspReader& reader, H264SliceHeader& header) {
      if (header.isIdr) {
        reader.readBits(1); // no_output_of_prior_pics_flag
        header.isLongTerm = reader.readBits(1);
        return reader.valid();
      }

      if (!reader.readBits(1)) // adaptive_ref_pic_marking_mode_flag
        return reader.valid();

      while (reader.valid()) {
        const uint32_t operation = reader.readUe();
        if (!operation)
          return reader.valid();

        if (operation > 6)
          return false;

        if (operation == 1 || operation == 3)
          reader.readUe(); // difference_of_pic_nums_minus1

        if (operation == 2 || operation == 3 || operation == 4)
          reader.readUe(); // long-term picture number, frame index, or limit

        if (operation == 6) {
          header.longTermFrameIndex = reader.readUe();
          header.isLongTerm = true;

          if (header.longTermFrameIndex >= 16)
            return false;
        }
      }

      return false;
    }


    bool parseH264SliceHeader(
      const DXVA_PicParams_H264&     params,
      const uint8_t*                data,
            size_t                  size,
            H264SliceHeader&        header) {
      if (size < 5 || data[0] || data[1] || data[2] != 1 || (data[3] & 0x80))
        return false;

      const uint32_t nalType = data[3] & 0x1f;
      const bool isReference = data[3] & 0x60;

      if ((nalType != 1 && nalType != 5) || isReference != bool(params.RefPicFlag))
        return false;

      VideoRbspReader reader(data + 4, data + size);
      header.firstMb = reader.readUe();
      const uint32_t sliceType = reader.readUe();
      const uint32_t sliceClass = sliceType % 5;
      header.ppsId = reader.readUe();
      const uint32_t frameNum = reader.readBits(params.log2_max_frame_num_minus4 + 4);

      if (sliceType > 9 || sliceClass > 2 || header.ppsId > 255
       || frameNum != params.frame_num || !reader.valid())
        return false;

      header.isIdr = nalType == 5;
      header.isIntra = sliceClass == 2;

      if (header.isIdr) {
        if (!header.isIntra || !isReference || frameNum)
          return false;

        header.idrPicId = reader.readUe();

        if (header.idrPicId > 65535)
          return false;
      }

      if (params.pic_order_cnt_type == 0) {
        reader.readBits(params.log2_max_pic_order_cnt_lsb_minus4 + 4);

        if (params.pic_order_present_flag)
          reader.readUe(); // delta_pic_order_cnt_bottom (signed Exp-Golomb)
      }

      if (params.redundant_pic_cnt_present_flag && reader.readUe())
        return false;

      const bool isB = sliceClass == 1;
      if (isB)
        reader.readBits(1); // direct_spatial_mv_pred_flag

      if (!header.isIntra) {
        uint32_t refCountL0 = params.num_ref_idx_l0_active_minus1 + 1u;
        uint32_t refCountL1 = isB ? params.num_ref_idx_l1_active_minus1 + 1u : 0u;

        if (reader.readBits(1)) { // num_ref_idx_active_override_flag
          refCountL0 = reader.readUe() + 1u;
          if (isB)
            refCountL1 = reader.readUe() + 1u;
        }

        if (refCountL0 > 16 || refCountL1 > 16
         || !skipH264ReferenceListModification(reader, refCountL0)
         || (isB && !skipH264ReferenceListModification(reader, refCountL1)))
          return false;

        const bool weighted = isB ? params.weighted_bipred_idc == 1 : params.weighted_pred_flag;
        if (weighted && !skipH264PredictionWeights(reader, refCountL0, refCountL1))
          return false;
      }

      return reader.valid() && (!isReference || parseH264ReferenceMarking(reader, header));
    }


    bool parseH264PictureSlices(
      const DXVA_PicParams_H264&     params,
      const std::vector<uint8_t>&    bitstream,
      const std::vector<uint32_t>&   sliceOffsets,
            H264SliceHeader&        header) {
      const uint64_t macroblockCount = uint64_t(params.wFrameWidthInMbsMinus1 + 1u)
        * (params.wFrameHeightInMbsMinus1 + 1u);
      if (sliceOffsets.empty() || sliceOffsets.size() > macroblockCount)
        return false;

      uint32_t prevMb = 0;
      for (size_t i = 0; i < sliceOffsets.size(); i++) {
        const size_t start = sliceOffsets[i];
        const size_t end = i + 1 < sliceOffsets.size() ? sliceOffsets[i + 1] : bitstream.size();

        if (start >= end || end > bitstream.size())
          return false;

        H264SliceHeader slice = { };
        if (!parseH264SliceHeader(params, bitstream.data() + start, end - start, slice)
         || slice.firstMb >= macroblockCount)
          return false;

        if (!i) {
          if (slice.firstMb)
            return false;

          header = slice;
        } else {
          if (slice.firstMb <= prevMb || slice.ppsId != header.ppsId
           || slice.isIdr != header.isIdr || slice.idrPicId != header.idrPicId
           || slice.isLongTerm != header.isLongTerm || slice.longTermFrameIndex != header.longTermFrameIndex)
            return false;

          header.isIntra &= slice.isIntra;
        }

        prevMb = slice.firstMb;
      }

      return true;
    }


    void translateH264ScalingLists(
      const DXVA_PicParams_H264&     params,
      const DXVA_Qmatrix_H264&       matrix,
            StdVideoH264ScalingLists& lists) {
      lists.scaling_list_present_mask = params.transform_8x8_mode_flag ? 0xff : 0x3f;

      // Configurations using the usual DXVA setting supply scan-order lists.
      // The setting used for the raster-order workaround supplies 0 instead.
      if (params.Reserved16Bits) {
        std::memcpy(lists.ScalingList4x4, matrix.bScalingLists4x4, sizeof(matrix.bScalingLists4x4));

        if (params.transform_8x8_mode_flag)
          std::memcpy(lists.ScalingList8x8, matrix.bScalingLists8x8, sizeof(matrix.bScalingLists8x8));

        return;
      }

      constexpr uint8_t scan4x4[] = {
         0,  1,  4,  8,  5,  2,  3,  6,  9, 12, 13, 10,  7, 11, 14, 15,
      };
      constexpr uint8_t scan8x8[] = {
         0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
        12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
        35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
        58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
      };

      for (size_t i = 0; i < std::size(matrix.bScalingLists4x4); i++) {
        for (size_t j = 0; j < std::size(scan4x4); j++)
          lists.ScalingList4x4[i][j] = matrix.bScalingLists4x4[i][scan4x4[j]];
      }

      if (params.transform_8x8_mode_flag) {
        for (size_t i = 0; i < std::size(matrix.bScalingLists8x8); i++) {
          for (size_t j = 0; j < std::size(scan8x8); j++)
            lists.ScalingList8x8[i][j] = matrix.bScalingLists8x8[i][scan8x8[j]];
        }
      }
    }


    bool translateH264References(
      const DXVA_PicParams_H264&     params,
            DxvkH264PictureParameters& result) {
      for (uint32_t i = 0; i < std::size(params.RefFrameList); i++) {
        const auto& entry = params.RefFrameList[i];
        const uint32_t flags = (params.UsedForReferenceFlags >> (2 * i)) & 3u;

        if (entry.bPicEntry == 0xff || entry.Index7Bits == result.currentPictureId || !flags)
          continue;

        if (flags != 3 || (params.NonExistingFrameFlags & (1u << i))
         || result.referenceCount >= params.num_ref_frames)
          return false;

        for (uint32_t j = 0; j < result.referenceCount; j++) {
          if (result.references[j].pictureId == entry.Index7Bits)
            return false;
        }

        auto& ref = result.references[result.referenceCount++];
        ref.pictureId = entry.Index7Bits;
        ref.info.flags.used_for_long_term_reference = entry.AssociatedFlag;
        ref.info.FrameNum = params.FrameNumList[i];
        ref.info.PicOrderCnt[0] = params.FieldOrderCntList[i][0];
        ref.info.PicOrderCnt[1] = params.FieldOrderCntList[i][1];
      }

      return true;
    }

  }


  bool h264LevelSupportsExtent(
          StdVideoH264LevelIdc      level,
          VkExtent2D                extent) {
    auto widthInMbs = extent.width / 16u + bool(extent.width % 16u);
    auto heightInMbs = extent.height / 16u + bool(extent.height % 16u);
    return h264LevelSupportsPicture(level, widthInMbs, heightInMbs, 0);
  }


  bool translateH264PictureParameters(
    const DXVA_PicParams_H264&       params,
    const DXVA_Qmatrix_H264&         matrix,
    const std::vector<uint8_t>&      bitstream,
    const std::vector<uint32_t>&     sliceOffsets,
          StdVideoH264LevelIdc      maxLevel,
          DxvkH264PictureParameters& result) {
    // DXVA omits the offsets needed to reconstruct a POC type 1 SPS.
    if (!params.frame_mbs_only_flag || params.field_pic_flag || params.MbaffFrameFlag
     || params.CurrPic.AssociatedFlag || params.residual_colour_transform_flag
     || params.chroma_format_idc != 1 || params.bit_depth_luma_minus8
     || params.bit_depth_chroma_minus8 || params.sp_for_switch_flag
     || params.num_slice_groups_minus1 || params.num_ref_frames > 16
     || params.pic_order_cnt_type > 2 || params.pic_order_cnt_type == 1
     || params.log2_max_frame_num_minus4 > 12
     || (params.pic_order_cnt_type == 0 && params.log2_max_pic_order_cnt_lsb_minus4 > 12)
     || params.num_ref_idx_l0_active_minus1 > 15 || params.num_ref_idx_l1_active_minus1 > 15
     || params.weighted_bipred_idc > 2
     || params.pic_init_qp_minus26 < -26 || params.pic_init_qp_minus26 > 25
     || params.pic_init_qs_minus26 < -26 || params.pic_init_qs_minus26 > 25
     || params.chroma_qp_index_offset < -12 || params.chroma_qp_index_offset > 12
     || params.second_chroma_qp_index_offset < -12 || params.second_chroma_qp_index_offset > 12)
      return false;

    const auto level = getH264Level(params, maxLevel);
    if (level == STD_VIDEO_H264_LEVEL_IDC_INVALID)
      return false;

    H264SliceHeader header = { };
    if (!parseH264PictureSlices(params, bitstream, sliceOffsets, header))
      return false;

    DxvkH264PictureParameters translated = { };
    translated.currentPictureId = params.CurrPic.Index7Bits;

    if (!translateH264References(params, translated))
      return false;

    // DXVA omits the SPS ID. A synthesized SPS and its PPS can use any
    // matching ID, while the PPS ID must match the one in the slice headers.
    auto& sps = translated.sps;
    sps.profile_idc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
    sps.level_idc = level;
    sps.chroma_format_idc = STD_VIDEO_H264_CHROMA_FORMAT_IDC_420;
    sps.flags.frame_mbs_only_flag = 1;
    sps.flags.direct_8x8_inference_flag = params.direct_8x8_inference_flag;
    sps.flags.gaps_in_frame_num_value_allowed_flag = 1;
    sps.log2_max_frame_num_minus4 = params.log2_max_frame_num_minus4;
    sps.pic_order_cnt_type = StdVideoH264PocType(params.pic_order_cnt_type);
    if (params.pic_order_cnt_type == 0)
      sps.log2_max_pic_order_cnt_lsb_minus4 = params.log2_max_pic_order_cnt_lsb_minus4;
    sps.max_num_ref_frames = params.num_ref_frames;
    sps.pic_width_in_mbs_minus1 = params.wFrameWidthInMbsMinus1;
    sps.pic_height_in_map_units_minus1 = params.wFrameHeightInMbsMinus1;

    auto& pps = translated.pps;
    pps.pic_parameter_set_id = uint8_t(header.ppsId);
    pps.flags.transform_8x8_mode_flag = params.transform_8x8_mode_flag;
    pps.flags.redundant_pic_cnt_present_flag = params.redundant_pic_cnt_present_flag;
    pps.flags.constrained_intra_pred_flag = params.constrained_intra_pred_flag;
    pps.flags.deblocking_filter_control_present_flag = params.deblocking_filter_control_present_flag;
    pps.flags.weighted_pred_flag = params.weighted_pred_flag;
    pps.flags.bottom_field_pic_order_in_frame_present_flag = params.pic_order_present_flag;
    pps.flags.entropy_coding_mode_flag = params.entropy_coding_mode_flag;
    pps.flags.pic_scaling_matrix_present_flag = 1;
    pps.num_ref_idx_l0_default_active_minus1 = params.num_ref_idx_l0_active_minus1;
    pps.num_ref_idx_l1_default_active_minus1 = params.num_ref_idx_l1_active_minus1;
    pps.weighted_bipred_idc = StdVideoH264WeightedBipredIdc(params.weighted_bipred_idc);
    pps.pic_init_qp_minus26 = params.pic_init_qp_minus26;
    pps.pic_init_qs_minus26 = params.pic_init_qs_minus26;
    pps.chroma_qp_index_offset = params.chroma_qp_index_offset;
    pps.second_chroma_qp_index_offset = params.second_chroma_qp_index_offset;
    translateH264ScalingLists(params, matrix, translated.scalingLists);

    auto& picture = translated.picture;
    picture.flags.is_intra = header.isIntra;
    picture.flags.IdrPicFlag = header.isIdr;
    picture.flags.is_reference = params.RefPicFlag;
    picture.pic_parameter_set_id = uint8_t(header.ppsId);
    picture.frame_num = params.frame_num;
    picture.idr_pic_id = uint16_t(header.idrPicId);
    picture.PicOrderCnt[0] = params.CurrFieldOrderCnt[0];
    picture.PicOrderCnt[1] = params.CurrFieldOrderCnt[1];

    auto& current = translated.currentReference;
    current.flags.used_for_long_term_reference = header.isLongTerm;
    current.FrameNum = header.isLongTerm ? uint16_t(header.longTermFrameIndex) : params.frame_num;
    current.PicOrderCnt[0] = params.CurrFieldOrderCnt[0];
    current.PicOrderCnt[1] = params.CurrFieldOrderCnt[1];

    translated.sliceOffsets = sliceOffsets;
    result = std::move(translated);
    return true;
  }

}
