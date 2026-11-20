#include "d3d11_video_av1.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace dxvk {

  namespace {

    uint32_t ceilLog2(uint32_t n) {
      uint32_t bits = 0;
      while (n > 1) {
        n = (n + 1) >> 1;
        ++bits;
      }
      return bits;
    }

    // DXVA supplies entropy-coded tiles and resolved parameters. Drivers may
    // also read the frame header OBU from the Vulkan bitstream buffer.
    class Av1Bits {
    public:
      void put(uint32_t n, uint32_t value) {
        if (n > 32 || (n < 32 && value >= (1u << n))) {
          valid = false;
          return;
        }
        for (uint32_t i = n; i; --i) {
          if (!bitOffset)
            bytes.push_back(0);
          bytes.back() |= ((value >> (i - 1)) & 1) << (7 - bitOffset);
          bitOffset = (bitOffset + 1) & 7;
        }
      }

      void flag(bool value) { put(1, value); }

      void signedBits(uint32_t n, int v) {
        const int limit = 1 << (n - 1);
        if (v < -limit || v >= limit)
          valid = false;
        put(n, uint32_t(v) & ((1u << n) - 1));
      }

      void delta(int v) {
        flag(v != 0);
        if (v)
          signedBits(7, v);
      }

      void ns(uint32_t n, uint32_t v) {
        if (!n || v >= n) {
          valid = false;
          return;
        }
        if (n == 1)
          return;
        const uint32_t w = ceilLog2(n);
        const uint32_t m = (1u << w) - n;
        put(v < m ? w - 1 : w, v < m ? v : v + m);
      }

      void subexp(uint32_t count, uint32_t value) {
        uint32_t base = 0;
        for (uint32_t i = 0; ; ++i) {
          const uint32_t bits = i ? 2 + i : 3;
          const uint32_t step = 1u << bits;
          if (count <= base + 3 * step) {
            ns(count - base, value - base);
            return;
          }
          const bool more = value >= base + step;
          flag(more);
          if (!more) {
            put(bits, value - base);
            return;
          }
          base += step;
        }
      }

      bool valid = true;
      std::vector<uint8_t> bytes;
    private:
      uint32_t bitOffset = 0;
    };

    void appendObuHeader(std::vector<uint8_t>& dst, uint8_t type, uint32_t size) {
      dst.push_back(uint8_t((type << 3) | 2)); // obu_has_size_field
      do {
        uint8_t b = size & 0x7f;
        size >>= 7;
        dst.push_back(b | (size ? 0x80 : 0));
      } while (size);
    }

    bool isIntra(StdVideoAV1FrameType type) {
      return type == STD_VIDEO_AV1_FRAME_TYPE_KEY || type == STD_VIDEO_AV1_FRAME_TYPE_INTRA_ONLY;
    }

    void setSequence(const DXVA_PicParams_AV1& p, DxvkAv1PictureParameters& a) {
      auto& s = a.sequence;
      s.seq_profile = STD_VIDEO_AV1_PROFILE_MAIN;
      s.max_frame_width_minus_1 = p.max_width - 1;
      s.max_frame_height_minus_1 = p.max_height - 1;
      s.frame_width_bits_minus_1 = std::max(1u, ceilLog2(p.max_width)) - 1;
      s.frame_height_bits_minus_1 = std::max(1u, ceilLog2(p.max_height)) - 1;
      s.order_hint_bits_minus_1 = p.order_hint_bits ? p.order_hint_bits - 1 : 0;
      s.seq_force_screen_content_tools = STD_VIDEO_AV1_SELECT_SCREEN_CONTENT_TOOLS;
      s.seq_force_integer_mv = STD_VIDEO_AV1_SELECT_INTEGER_MV;
      s.flags.use_128x128_superblock = p.coding.use_128x128_superblock;
      s.flags.enable_filter_intra = p.coding.filter_intra;
      s.flags.enable_intra_edge_filter = p.coding.intra_edge_filter;
      s.flags.enable_interintra_compound = p.coding.interintra_compound;
      s.flags.enable_masked_compound = p.coding.masked_compound;
      s.flags.enable_warped_motion = 1;
      s.flags.enable_dual_filter = p.coding.dual_filter;
      s.flags.enable_order_hint = p.order_hint_bits != 0;
      s.flags.enable_jnt_comp = p.coding.jnt_comp;
      s.flags.enable_ref_frame_mvs = p.coding.enable_ref_frame_mvs;
      s.flags.enable_superres = 1;
      s.flags.enable_cdef = p.coding.cdef;
      s.flags.enable_restoration = p.coding.restoration;
      s.flags.film_grain_params_present = p.coding.film_grain;
      a.color.flags.separate_uv_delta_q = 1;
      a.color.BitDepth = p.bitdepth;
      a.color.subsampling_x = a.color.subsampling_y = 1;
      a.color.color_primaries = STD_VIDEO_AV1_COLOR_PRIMARIES_UNSPECIFIED;
      a.color.transfer_characteristics = STD_VIDEO_AV1_TRANSFER_CHARACTERISTICS_UNSPECIFIED;
      a.color.matrix_coefficients = STD_VIDEO_AV1_MATRIX_COEFFICIENTS_UNSPECIFIED;
    }

    bool setPicture(const DXVA_PicParams_AV1& p, DxvkAv1PictureParameters& a) {
      auto& pic = a.picture;
      auto& f = pic.flags;
      pic.frame_type = StdVideoAV1FrameType(p.format.frame_type);
      f.error_resilient_mode = pic.frame_type == STD_VIDEO_AV1_FRAME_TYPE_SWITCH
        || (pic.frame_type == STD_VIDEO_AV1_FRAME_TYPE_KEY && p.format.show_frame);
      f.disable_cdf_update = p.coding.disable_cdf_update;
      f.use_superres = p.coding.superres;
      f.allow_screen_content_tools = p.coding.screen_content_tools;
      f.is_filter_switchable = p.interp_filter == STD_VIDEO_AV1_INTERPOLATION_FILTER_SWITCHABLE;
      f.force_integer_mv = isIntra(pic.frame_type) || p.coding.integer_mv;
      f.frame_size_override_flag = pic.frame_type == STD_VIDEO_AV1_FRAME_TYPE_SWITCH
        || p.width != p.max_width || p.height != p.max_height;
      f.allow_intrabc = p.coding.intrabc;
      f.allow_high_precision_mv = p.coding.high_precision_mv;
      f.is_motion_mode_switchable = p.coding.switchable_motion_mode;
      f.use_ref_frame_mvs = p.coding.use_ref_frame_mvs;
      f.disable_frame_end_update_cdf = p.coding.disable_frame_end_update_cdf;
      f.allow_warped_motion = p.coding.warped_motion;
      f.reduced_tx_set = p.coding.reduced_tx_set;
      f.reference_select = p.coding.reference_mode;
      f.skip_mode_present = p.coding.skip_mode;
      f.delta_q_present = p.quantization.delta_q_present;
      f.delta_lf_present = p.loop_filter.delta_lf_present;
      f.delta_lf_multi = p.loop_filter.delta_lf_multi;
      f.segmentation_enabled = p.segmentation.enabled;
      f.segmentation_update_map = p.segmentation.update_map;
      f.segmentation_temporal_update = p.segmentation.temporal_update;
      f.segmentation_update_data = p.segmentation.update_data;
      f.apply_grain = p.film_grain.apply_grain;
      pic.OrderHint = p.order_hint;
      pic.primary_ref_frame = isIntra(pic.frame_type) ? STD_VIDEO_AV1_PRIMARY_REF_NONE : p.primary_ref_frame;
      pic.interpolation_filter = StdVideoAV1InterpolationFilter(p.interp_filter);
      pic.TxMode = StdVideoAV1TxMode(p.coding.tx_mode);
      pic.delta_q_res = p.quantization.delta_q_res;
      pic.delta_lf_res = p.loop_filter.delta_lf_res;
      pic.coded_denom = f.use_superres ? p.superres_denom - 9 : 0;
      a.extent = {p.width, p.height};
      a.currentPictureId = p.CurrPicTextureIndex;
      a.referenceNames.fill(0xff);
      std::copy_n(p.RefFrameMapTextureIndex, 8, a.referenceMap.begin());
      if (pic.primary_ref_frame > 7 || p.interp_filter > 4 || p.coding.tx_mode > 2
       || (!isIntra(pic.frame_type) && !p.coding.reference_frame_update)
       || (f.apply_grain && !p.coding.film_grain))
        return false;
      auto& q = a.quantization;
      q.base_q_idx = p.quantization.base_qindex;
      q.DeltaQYDc = p.quantization.y_dc_delta_q;
      q.DeltaQUDc = p.quantization.u_dc_delta_q;
      q.DeltaQUAc = p.quantization.u_ac_delta_q;
      q.DeltaQVDc = p.quantization.v_dc_delta_q;
      q.DeltaQVAc = p.quantization.v_ac_delta_q;
      q.flags.diff_uv_delta = q.DeltaQUDc != q.DeltaQVDc || q.DeltaQUAc != q.DeltaQVAc;
      q.flags.using_qmatrix = p.quantization.qm_y != 0xff;
      if (q.flags.using_qmatrix) {
        q.qm_y = p.quantization.qm_y;
        q.qm_u = p.quantization.qm_u;
        q.qm_v = p.quantization.qm_v;
      }
      for (unsigned i = 0; i < 8; i++) {
        a.segmentation.FeatureEnabled[i] = p.segmentation.feature_mask[i].mask;
        std::copy_n(p.segmentation.feature_data[i], 8, a.segmentation.FeatureData[i]);
        a.cdef.cdef_y_pri_strength[i] = p.cdef.y_strengths[i].primary;
        a.cdef.cdef_y_sec_strength[i] = p.cdef.y_strengths[i].secondary;
        a.cdef.cdef_uv_pri_strength[i] = p.cdef.uv_strengths[i].primary;
        a.cdef.cdef_uv_sec_strength[i] = p.cdef.uv_strengths[i].secondary;
        a.loopFilter.loop_filter_ref_deltas[i] = p.loop_filter.ref_deltas[i];
        a.motion.gm_params[i][2] = a.motion.gm_params[i][5] = 1 << 16;
      }
      a.cdef.cdef_damping_minus_3 = p.cdef.damping;
      a.cdef.cdef_bits = p.cdef.bits;
      auto& lf = a.loopFilter;
      lf.flags.loop_filter_delta_enabled = p.loop_filter.mode_ref_delta_enabled;
      lf.flags.loop_filter_delta_update = p.loop_filter.mode_ref_delta_update;
      lf.update_ref_delta = lf.flags.loop_filter_delta_update ? 0xff : 0;
      lf.update_mode_delta = lf.flags.loop_filter_delta_update ? 3 : 0;
      lf.loop_filter_sharpness = p.loop_filter.sharpness_level;
      std::copy_n(p.loop_filter.mode_deltas, 2, lf.loop_filter_mode_deltas);
      lf.loop_filter_level[0] = p.loop_filter.filter_level[0];
      lf.loop_filter_level[1] = p.loop_filter.filter_level[1];
      lf.loop_filter_level[2] = p.loop_filter.filter_level_u;
      lf.loop_filter_level[3] = p.loop_filter.filter_level_v;
      for (unsigned i = 0; i < 3; ++i) {
        unsigned type = p.loop_filter.frame_restoration_type[i];
        if (type > 3)
          return false;
        a.restoration.FrameRestorationType[i] = StdVideoAV1FrameRestorationType(type);
        f.UsesLr |= type != 0;
        f.usesChromaLr |= i && type;
      }
      if (f.UsesLr) {
        for (unsigned i = 0; i < 3; ++i) {
          const uint32_t log2size = p.loop_filter.log2_restoration_unit_size[i];
          if (log2size < 5 || log2size > 8)
            return false;
          a.restoration.LoopRestorationSize[i] = log2size - 5;
        }
      }
      if (!isIntra(pic.frame_type)) {
        for (unsigned i = 0; i < 7; i++) {
          auto& ref = p.frame_refs[i];
          if (ref.Index >= 8 || a.referenceMap[ref.Index] == 0xff)
            return false;
          a.referenceNames[i] = a.referenceMap[ref.Index];
          a.motion.GmType[i + 1] = ref.wmtype;
          if (ref.wmtype)
            std::copy_n(ref.wmmat, 6, a.motion.gm_params[i + 1]);
        }
      }
      return true;
    }

    bool setGrain(const DXVA_PicParams_AV1& p, StdVideoAV1FilmGrain& grain) {
      const auto& g = p.film_grain;
      if (g.num_y_points > 14 || g.num_cb_points > 10 || g.num_cr_points > 10
       || ((g.chroma_scaling_from_luma || !g.num_y_points) && (g.num_cb_points || g.num_cr_points))
       || g.cb_offset < 0 || g.cb_offset > 511 || g.cr_offset < 0 || g.cr_offset > 511)
        return false;
      grain.flags.chroma_scaling_from_luma = g.chroma_scaling_from_luma;
      grain.flags.overlap_flag = g.overlap_flag;
      grain.flags.clip_to_restricted_range = g.clip_to_restricted_range;
      grain.flags.update_grain = 1;
      grain.grain_scaling_minus_8 = g.scaling_shift_minus8;
      grain.ar_coeff_lag = g.ar_coeff_lag;
      grain.ar_coeff_shift_minus_6 = g.ar_coeff_shift_minus6;
      grain.grain_scale_shift = g.grain_scale_shift;
      grain.grain_seed = g.grain_seed;
      grain.num_y_points = g.num_y_points;
      grain.num_cb_points = g.num_cb_points;
      grain.num_cr_points = g.num_cr_points;
      auto copyPoints = [](auto& values, auto& scales, const auto& src, unsigned count) {
        for (unsigned i = 0; i < count; i++) {
          if (i && src[i][0] <= src[i - 1][0])
            return false;
          values[i] = src[i][0];
          scales[i] = src[i][1];
        }
        return true;
      };
      if (!copyPoints(grain.point_y_value, grain.point_y_scaling, g.scaling_points_y, g.num_y_points)
       || !copyPoints(grain.point_cb_value, grain.point_cb_scaling, g.scaling_points_cb, g.num_cb_points)
       || !copyPoints(grain.point_cr_value, grain.point_cr_scaling, g.scaling_points_cr, g.num_cr_points))
        return false;
      std::memcpy(grain.ar_coeffs_y_plus_128, g.ar_coeffs_y, sizeof(g.ar_coeffs_y));
      std::memcpy(grain.ar_coeffs_cb_plus_128, g.ar_coeffs_cb, sizeof(g.ar_coeffs_cb));
      std::memcpy(grain.ar_coeffs_cr_plus_128, g.ar_coeffs_cr, sizeof(g.ar_coeffs_cr));
      grain.cb_mult = g.cb_mult;
      grain.cb_luma_mult = g.cb_luma_mult;
      grain.cr_mult = g.cr_mult;
      grain.cr_luma_mult = g.cr_luma_mult;
      grain.cb_offset = g.cb_offset;
      grain.cr_offset = g.cr_offset;
      return true;
    }

    // Each Vulkan decode gives the driver the actual pictures for all seven
    // reference names. Choose legal indices and a refresh mask for the encoded
    // header, retaining distinct pictures and reusing duplicate aliases first.
    struct HeaderReferences {
      std::array<uint8_t, 7> indices = { };
      const D3D11Av1Reference* primary = nullptr;
      bool skipAllowed = false;
    };

    bool setReferences(DxvkAv1PictureParameters& a, const DXVA_PicParams_AV1& p,
        const std::vector<D3D11Av1Reference>& refs, HeaderReferences& h) {
      auto& pic = a.picture;
      const bool intra = isIntra(pic.frame_type);
      if (refs.empty() && pic.frame_type != STD_VIDEO_AV1_FRAME_TYPE_KEY)
        return false;
      if (!intra) {
        for (unsigned i = 0; i < h.indices.size(); ++i) {
          auto it = std::find_if(refs.begin(), refs.end(), [&](const auto& r) { return r.pictureId == a.referenceNames[i]; });
          if (it == refs.end())
            return false;
          h.indices[i] = uint8_t(it - refs.begin());
          pic.OrderHints[i + 1] = it->orderHint;
          if (i == pic.primary_ref_frame)
            h.primary = &*it;
        }
      }
      if (!refs.empty() && pic.flags.reference_select && p.order_hint_bits && !intra) {
        const int range = 1 << p.order_hint_bits;
        auto dist = [&](int x, int y) {
          return ((x - y + range / 2) & (range - 1)) - range / 2;
        };
        int forward = -1, backward = -1;
        for (int i = 1; i <= 7; ++i) {
          int hint = pic.OrderHints[i];
          int d = dist(hint, pic.OrderHint);
          if (d < 0 && (forward < 0 || dist(hint, pic.OrderHints[forward]) > 0))
            forward = i;
          if (d > 0 && (backward < 0 || dist(hint, pic.OrderHints[backward]) < 0))
            backward = i;
        }
        if (forward >= 0 && backward < 0) {
          for (int i = 1; i <= 7; ++i) {
            int hint = pic.OrderHints[i];
            if (dist(hint, pic.OrderHints[forward]) < 0 &&
                (backward < 0 || dist(hint, pic.OrderHints[backward]) > 0))
              backward = i;
          }
        }
        if (forward >= 0 && backward >= 0) {
          h.skipAllowed = true;
          pic.SkipModeFrame[0] = std::min(forward, backward);
          pic.SkipModeFrame[1] = std::max(forward, backward);
        }
      }

      if (pic.flags.skip_mode_present && !h.skipAllowed)
        return false;
      if ((pic.frame_type == STD_VIDEO_AV1_FRAME_TYPE_KEY && p.format.show_frame)
       || pic.frame_type == STD_VIDEO_AV1_FRAME_TYPE_SWITCH) {
        pic.refresh_frame_flags = 0xff;
      } else {
        size_t slot = 0;
        unsigned best = 4;
        for (size_t i = 0; i < refs.size(); i++) {
          const uint8_t id = refs[i].pictureId;
          const bool expired = std::find(a.referenceMap.begin(), a.referenceMap.end(), id) == a.referenceMap.end();
          const bool alias = std::any_of(refs.begin(), refs.begin() + i, [&](const auto& r) { return r.pictureId == id; });
          const bool named = std::find(a.referenceNames.begin(), a.referenceNames.end(), id) != a.referenceNames.end();
          unsigned rank = expired ? 0 : alias ? 1 : !named ? 2 : 3;
          if (rank < best) {
            best = rank;
            slot = i;
          }
        }
        pic.refresh_frame_flags = uint8_t(1u << slot);
      }
      return true;
    }

    bool writeTiles(Av1Bits& out, const DXVA_PicParams_AV1& p, DxvkAv1PictureParameters& a) {
      const unsigned sb = p.coding.use_128x128_superblock ? 128 : 64;
      const unsigned width = (p.width * 8 + p.superres_denom / 2) / p.superres_denom;
      const unsigned cols = (width + sb - 1) / sb, rows = (p.height + sb - 1) / sb;
      const unsigned maxWidth = 4096 / sb, maxArea = 4096 * 2304 / (sb * sb);
      const unsigned minColsLog = ceilLog2((cols + maxWidth - 1) / maxWidth);
      const unsigned minTilesLog = std::max(minColsLog, ceilLog2((cols * rows + maxArea - 1) / maxArea));
      const unsigned colLog = ceilLog2(p.tiles.cols), rowLog = ceilLog2(p.tiles.rows);
      const unsigned maxColLog = ceilLog2(std::min(cols, 64u)), maxRowLog = ceilLog2(std::min(rows, 64u));
      const unsigned minRowLog = minTilesLog > colLog ? minTilesLog - colLog : 0;

      // Uniform spacing permits some large tiles that the explicit grid's
      // maximum-area rule cannot represent.
      auto uniformAxis = [](unsigned total, unsigned log, unsigned count, const auto& sizes) {
        unsigned step = (total + (1u << log) - 1) >> log;
        if ((total + step - 1) / step != count)
          return false;
        for (unsigned i = 0; i < count; ++i)
          if (sizes[i] != std::min(step, total - i * step))
            return false;
        return true;
      };
      const bool uniform = colLog >= minColsLog && colLog <= maxColLog && rowLog >= minRowLog && rowLog <= maxRowLog
        && uniformAxis(cols, colLog, p.tiles.cols, p.tiles.widths) && uniformAxis(rows, rowLog, p.tiles.rows, p.tiles.heights);
      a.tiles.flags.uniform_tile_spacing_flag = uniform;
      unsigned widest = 0;
      a.tiles.TileCols = p.tiles.cols;
      a.tiles.TileRows = p.tiles.rows;
      a.tiles.context_update_tile_id = p.tiles.context_update_id;
      a.tiles.tile_size_bytes_minus_1 = 0;
      out.flag(uniform);
      if (uniform) {
        auto increment = [&](unsigned value, unsigned min, unsigned max) {
          for (unsigned i = min; i < value; i++)
            out.flag(true);
          if (value < max)
            out.flag(false);
        };
        increment(colLog, minColsLog, maxColLog);
        increment(rowLog, minRowLog, maxRowLog);
      }
      unsigned start = 0;
      for (unsigned i = 0; i < p.tiles.cols; ++i) {
        unsigned widthInSb = p.tiles.widths[i];
        if (!widthInSb || widthInSb > std::min(maxWidth, cols - start))
          return false;
        a.tileColumns[i] = uint16_t(start * (sb / 4));
        a.tileWidths[i] = widthInSb - 1;
        if (!uniform)
          out.ns(std::min(maxWidth, cols - start), widthInSb - 1);
        start += widthInSb;
        widest = std::max(widest, widthInSb);
      }
      if (start != cols)
        return false;
      a.tileColumns[p.tiles.cols] = uint16_t(2 * ((width + 7) >> 3));
      const unsigned area = minTilesLog ? ((cols * rows) >> (minTilesLog + 1)) : (cols * rows);
      const unsigned maxHeight = uniform ? rows : std::max(1u, area / widest);
      start = 0;
      for (unsigned i = 0; i < p.tiles.rows; ++i) {
        unsigned heightInSb = p.tiles.heights[i];
        if (!heightInSb || heightInSb > std::min(maxHeight, rows - start))
          return false;
        a.tileRows[i] = uint16_t(start * (sb / 4));
        a.tileHeights[i] = heightInSb - 1;
        if (!uniform)
          out.ns(std::min(maxHeight, rows - start), heightInSb - 1);
        start += heightInSb;
      }
      if (start != rows)
        return false;
      a.tileRows[p.tiles.rows] = uint16_t(2 * ((p.height + 7) >> 3));
      if (p.tiles.cols * p.tiles.rows > 1) {
        out.put(colLog + rowLog, p.tiles.context_update_id);
        out.put(2, a.tiles.tile_size_bytes_minus_1);
      }
      return true;
    }

    bool writeQuantization(Av1Bits& out, const DxvkAv1PictureParameters& a) {
      const auto& q = a.quantization;
      out.put(8, q.base_q_idx);
      out.delta(q.DeltaQYDc);
      out.flag(q.flags.diff_uv_delta);
      out.delta(q.DeltaQUDc);
      out.delta(q.DeltaQUAc);
      if (q.flags.diff_uv_delta) {
        out.delta(q.DeltaQVDc);
        out.delta(q.DeltaQVAc);
      }
      out.flag(q.flags.using_qmatrix);
      if (q.flags.using_qmatrix) {
        out.put(4, q.qm_y);
        out.put(4, q.qm_u);
        out.put(4, q.qm_v);
      }

      const auto& pic = a.picture;
      const auto& f = pic.flags;
      out.flag(f.segmentation_enabled);
      if (f.segmentation_enabled) {
        if (pic.primary_ref_frame != STD_VIDEO_AV1_PRIMARY_REF_NONE) {
          out.flag(f.segmentation_update_map);
          if (f.segmentation_update_map)
            out.flag(f.segmentation_temporal_update);
          out.flag(f.segmentation_update_data);
        }
        if (pic.primary_ref_frame == STD_VIDEO_AV1_PRIMARY_REF_NONE || f.segmentation_update_data) {
          for (unsigned i = 0; i < 8; ++i) {
            for (unsigned j = 0; j < 8; ++j) {
              const bool enabled = a.segmentation.FeatureEnabled[i] & (1 << j);
              out.flag(enabled);
              if (enabled && j < 6) {
                int value = a.segmentation.FeatureData[i][j];
                if (j == 5)
                  out.put(3, value);
                else
                  out.signedBits(j ? 7 : 9, value);
              }
            }
          }
        }
      }
      if (q.base_q_idx)
        out.flag(f.delta_q_present);
      if (f.delta_q_present) {
        out.put(2, pic.delta_q_res);
        if (!f.allow_intrabc)
          out.flag(f.delta_lf_present);
        if (f.delta_lf_present) {
          out.put(2, pic.delta_lf_res);
          out.flag(f.delta_lf_multi);
        }
      }

      if (q.DeltaQYDc || q.DeltaQUAc || q.DeltaQUDc || q.DeltaQVAc || q.DeltaQVDc)
        return false;
      for (unsigned i = 0; i < 8; ++i) {
        const bool altQ = f.segmentation_enabled && (a.segmentation.FeatureEnabled[i] & 1);
        if (q.base_q_idx + (altQ ? a.segmentation.FeatureData[i][0] : 0) > 0)
          return false;
      }
      return true; // CodedLossless
    }

    void writeFilters(Av1Bits& out, const DxvkAv1PictureParameters& a, bool lossless) {
      auto& pic = a.picture;
      auto& f = pic.flags;
      if (!lossless && !f.allow_intrabc) {
        const auto& lf = a.loopFilter;
        out.put(6, lf.loop_filter_level[0]);
        out.put(6, lf.loop_filter_level[1]);
        if (lf.loop_filter_level[0] || lf.loop_filter_level[1]) {
          out.put(6, lf.loop_filter_level[2]);
          out.put(6, lf.loop_filter_level[3]);
        }
        out.put(3, lf.loop_filter_sharpness);
        out.flag(lf.flags.loop_filter_delta_enabled);
        if (lf.flags.loop_filter_delta_enabled) {
          out.flag(lf.flags.loop_filter_delta_update);
          if (lf.flags.loop_filter_delta_update) {
            for (int x : lf.loop_filter_ref_deltas) {
              out.flag(true);
              out.signedBits(7, x);
            }
            for (int x : lf.loop_filter_mode_deltas) {
              out.flag(true);
              out.signedBits(7, x);
            }
          }
        }
        if (a.sequence.flags.enable_cdef) {
          const auto& c = a.cdef;
          out.put(2, c.cdef_damping_minus_3);
          out.put(2, c.cdef_bits);
          for (unsigned i = 0; i < (1u << c.cdef_bits); i++) {
            out.put(4, c.cdef_y_pri_strength[i]);
            out.put(2, c.cdef_y_sec_strength[i]);
            out.put(4, c.cdef_uv_pri_strength[i]);
            out.put(2, c.cdef_uv_sec_strength[i]);
          }
        }
      }

      if ((!lossless || f.use_superres) && !f.allow_intrabc && a.sequence.flags.enable_restoration) {
        // restoration_type literals are NONE, SWITCHABLE, WIENER, SGRPROJ.
        constexpr uint8_t syntax[] = {0, 2, 3, 1};
        for (auto type : a.restoration.FrameRestorationType)
          out.put(2, syntax[type]);
        if (f.UsesLr) {
          unsigned luma = a.restoration.LoopRestorationSize[0];
          unsigned minLuma = a.sequence.flags.use_128x128_superblock ? 2 : 1;
          if (luma < minLuma || luma > 3) {
            out.valid = false;
            return;
          }
          for (unsigned n = minLuma; n < 3; ++n) {
            out.flag(luma > n);
            if (luma == n)
              break;
          }
          if (f.usesChromaLr) {
            const unsigned chroma = a.restoration.LoopRestorationSize[1];
            if (chroma != a.restoration.LoopRestorationSize[2] || (luma != chroma && luma != chroma + 1))
              out.valid = false;
            out.flag(luma != chroma);
          }
        }
      }
    }

    bool writeGlobalMotion(Av1Bits& out, const DxvkAv1PictureParameters& a, const D3D11Av1Reference* primary) {
      if (isIntra(a.picture.frame_type))
        return true;

      for (unsigned ref = 1; ref < 8; ref++) {
        const unsigned type = a.motion.GmType[ref];
        if (type > 3)
          return false;
        out.flag(type != 0);
        if (!type)
          continue;
        out.flag(type == 2);
        if (type != 2)
          out.flag(type == 1);

        // Warp coefficients use 16 fractional bits. Encode their signed
        // subexponential deltas relative to the primary reference's model.
        auto coefficient = [&](unsigned k) {
          const bool translation = k < 2 && type == 1;
          const int lowPrecision = !a.picture.flags.allow_high_precision_mv;
          const int absBits = translation ? 9 - lowPrecision : 12;
          const int fractBits = k >= 2 ? 15 : translation ? 3 - lowPrecision : 6;
          const int step = 1 << (16 - fractBits);
          const int identity = (k == 2 || k == 5) ? 1 << 16 : 0;
          const int bound = 1 << absBits;
          const int count = 2 * bound + 1;
          const int64_t raw = a.motion.gm_params[ref][k];
          const int64_t previous = primary ? primary->motion.gm_params[ref][k] : identity;
          if ((raw - identity) % step)
            return false;

          int64_t value = (raw - identity) / step + bound;
          int64_t center = ((previous - identity) >> (16 - fractBits)) + bound;
          if (value < 0 || value >= count || center < 0 || center >= count)
            return false;
          if (2 * center > count) {
            value = count - 1 - value;
            center = count - 1 - center;
          }
          uint32_t code = value > 2 * center ? value
                        : value >= center ? 2 * (value - center) : 2 * (center - value) - 1;
          out.subexp(count, code);
          return true;
        };
        if (type >= 2 && (!coefficient(2) || !coefficient(3)))
          return false;
        if (type == 3 && (!coefficient(4) || !coefficient(5)))
          return false;
        if (!coefficient(0) || !coefficient(1))
          return false;
      }
      return out.valid;
    }

    void writeGrain(Av1Bits& out, const DXVA_PicParams_AV1& p) {
      if (!p.coding.film_grain || (!p.format.show_frame && !p.format.showable_frame))
        return;
      const auto& g = p.film_grain;
      out.flag(g.apply_grain);
      if (!g.apply_grain)
        return;
      out.put(16, g.grain_seed);
      if (p.format.frame_type == STD_VIDEO_AV1_FRAME_TYPE_INTER)
        out.flag(true); // DXVA supplies the resolved parameters even for inherited grain.

      auto points = [&](unsigned count, const auto& values) {
        out.put(4, count);
        for (unsigned i = 0; i < count; i++) {
          out.put(8, values[i][0]);
          out.put(8, values[i][1]);
        }
      };
      points(g.num_y_points, g.scaling_points_y);
      out.flag(g.chroma_scaling_from_luma);
      if (!g.chroma_scaling_from_luma && g.num_y_points) {
        points(g.num_cb_points, g.scaling_points_cb);
        points(g.num_cr_points, g.scaling_points_cr);
      }
      out.put(2, g.scaling_shift_minus8);
      out.put(2, g.ar_coeff_lag);
      const unsigned luma = 2 * g.ar_coeff_lag * (g.ar_coeff_lag + 1);
      const unsigned chroma = luma + !!g.num_y_points;
      if (g.num_y_points) {
        for (unsigned i = 0; i < luma; i++)
          out.put(8, g.ar_coeffs_y[i]);
      }
      if (g.num_cb_points || g.chroma_scaling_from_luma) {
        for (unsigned i = 0; i < chroma; i++)
          out.put(8, g.ar_coeffs_cb[i]);
      }
      if (g.num_cr_points || g.chroma_scaling_from_luma) {
        for (unsigned i = 0; i < chroma; i++)
          out.put(8, g.ar_coeffs_cr[i]);
      }
      out.put(2, g.ar_coeff_shift_minus6);
      out.put(2, g.grain_scale_shift);
      if (g.num_cb_points) {
        out.put(8, g.cb_mult);
        out.put(8, g.cb_luma_mult);
        out.put(9, g.cb_offset);
      }
      if (g.num_cr_points) {
        out.put(8, g.cr_mult);
        out.put(8, g.cr_luma_mult);
        out.put(9, g.cr_offset);
      }
      out.flag(g.overlap_flag);
      out.flag(g.clip_to_restricted_range);
    }

    bool writeFrameHeader(Av1Bits& out, const DXVA_PicParams_AV1& p, DxvkAv1PictureParameters& a,
        const HeaderReferences& h, const std::vector<D3D11Av1Reference>& refs) {
      const auto& pic = a.picture;
      const auto& f = pic.flags;
      const bool intra = isIntra(pic.frame_type);
      const bool fullRefresh = (pic.frame_type == STD_VIDEO_AV1_FRAME_TYPE_KEY && p.format.show_frame)
        || pic.frame_type == STD_VIDEO_AV1_FRAME_TYPE_SWITCH;
      out.flag(false); // show_existing_frame
      out.put(2, pic.frame_type);
      out.flag(p.format.show_frame);
      if (!p.format.show_frame)
        out.flag(p.format.showable_frame);
      if (!fullRefresh)
        out.flag(f.error_resilient_mode);
      out.flag(f.disable_cdf_update);
      out.flag(f.allow_screen_content_tools);
      if (f.allow_screen_content_tools)
        out.flag(f.force_integer_mv);
      if (pic.frame_type != STD_VIDEO_AV1_FRAME_TYPE_SWITCH)
        out.flag(f.frame_size_override_flag);
      out.put(p.order_hint_bits, p.order_hint);
      if (!intra && !f.error_resilient_mode)
        out.put(3, pic.primary_ref_frame);
      if (!fullRefresh)
        out.put(8, pic.refresh_frame_flags);
      if ((!intra || pic.refresh_frame_flags != 0xff) && f.error_resilient_mode && p.order_hint_bits)
        for (unsigned i = 0; i < 8; i++)
          out.put(p.order_hint_bits, i < refs.size() ? refs[i].orderHint : 0);
      if (!intra) {
        if (p.order_hint_bits)
          out.flag(false); // frame_refs_short_signaling
        for (uint8_t index : h.indices)
          out.put(3, index);
        if (f.frame_size_override_flag && !f.error_resilient_mode)
          out.put(7, 0); // write explicit dimensions instead of inheriting a reference extent
      }
      if (f.frame_size_override_flag) {
        out.put(a.sequence.frame_width_bits_minus_1 + 1, p.width - 1);
        out.put(a.sequence.frame_height_bits_minus_1 + 1, p.height - 1);
      }
      out.flag(f.use_superres);
      if (f.use_superres)
        out.put(3, pic.coded_denom);
      out.flag(false); // render_and_frame_size_different
      if (intra) {
        if (f.allow_screen_content_tools && !f.use_superres)
          out.flag(f.allow_intrabc);
      } else {
        if (!f.force_integer_mv)
          out.flag(f.allow_high_precision_mv);
        out.flag(f.is_filter_switchable);
        if (!f.is_filter_switchable)
          out.put(2, pic.interpolation_filter);
        out.flag(f.is_motion_mode_switchable);
        if (a.sequence.flags.enable_ref_frame_mvs && !f.error_resilient_mode)
          out.flag(f.use_ref_frame_mvs);
      }
      if (!f.disable_cdf_update)
        out.flag(f.disable_frame_end_update_cdf);
      if (!writeTiles(out, p, a))
        return false;
      bool lossless = writeQuantization(out, a);
      writeFilters(out, a, lossless);
      if (!lossless)
        out.flag(pic.TxMode == STD_VIDEO_AV1_TX_MODE_SELECT);
      if (!intra)
        out.flag(f.reference_select);
      if (h.skipAllowed)
        out.flag(f.skip_mode_present);
      if (!intra && !f.error_resilient_mode)
        out.flag(f.allow_warped_motion);
      out.flag(f.reduced_tx_set);
      if (!writeGlobalMotion(out, a, h.primary))
        return false;
      writeGrain(out, p);
      out.flag(true); // trailing_one_bit for OBU_FRAME_HEADER
      return out.valid;
    }

  }


  bool translateAv1PictureParameters(
    const DXVA_PicParams_AV1& p, const std::vector<DXVA_Tile_AV1>& tiles,
    const D3D11_VIDEO_DECODER_DESC& desc, std::vector<uint8_t>& bitstream,
    DxvkAv1PictureParameters& result, std::vector<D3D11Av1Reference>& refs) {
    if (p.seq_profile || !p.format.subsampling_x || !p.format.subsampling_y || p.format.mono_chrome
     || !p.width || !p.height || p.width > p.max_width || p.height > p.max_height
     || p.max_width > 65536 || p.max_height > 65536 || p.width > desc.SampleWidth || p.height > desc.SampleHeight
     || (desc.OutputFormat == DXGI_FORMAT_NV12 ? p.bitdepth != 8 : desc.OutputFormat != DXGI_FORMAT_P010 || p.bitdepth != 10)
     || !p.tiles.cols || !p.tiles.rows || p.tiles.cols > 64 || p.tiles.rows > 64 || tiles.size() > 512
     || tiles.size() != size_t(p.tiles.cols) * p.tiles.rows || p.tiles.context_update_id >= tiles.size()
     || p.CurrPicTextureIndex == 0xff || p.order_hint_bits > 8 || p.primary_ref_frame > 7
     || (p.coding.superres ? p.superres_denom < 9 || p.superres_denom > 16 : p.superres_denom != 8)
     || (!refs.empty() && (refs.size() < 8 || refs.size() > 16)))
      return false;

    result = { };
    auto& a = result;
    setSequence(p, a);
    if (!setPicture(p, a) || (p.film_grain.apply_grain && !setGrain(p, a.grain)))
      return false;
    // DXVA names the source surfaces but does not provide refresh_frame_flags.
    // Keep the eight logical indices used by previous reconstructed headers
    // when possible. Vulkan binds the seven named references explicitly, so a
    // surviving DXVA surface may also be rebound to its actual index.
    std::vector<D3D11Av1Reference> slots(refs.begin(), refs.begin() + std::min<size_t>(8, refs.size()));
    auto byId = [](const auto& table, uint8_t id) {
      return std::find_if(table.begin(), table.end(), [id](const auto& r) { return r.pictureId == id; });
    };
    if (!isIntra(a.picture.frame_type) && std::any_of(a.referenceNames.begin(), a.referenceNames.end(),
        [&](uint8_t id) { return byId(slots, id) == slots.end(); })) {
      slots.clear();
      for (uint8_t id : a.referenceMap) {
        auto it = byId(refs, id);
        slots.push_back(it == refs.end() ? D3D11Av1Reference{0xff, 0, {}} : *it);
      }
    }

    HeaderReferences h;
    if (!setReferences(a, p, slots, h))
      return false;
    Av1Bits frame;
    if (!writeFrameHeader(frame, p, a, h, slots))
      return false;

    std::vector<uint8_t> buffer;
    appendObuHeader(buffer, 3, frame.bytes.size()); // OBU_FRAME_HEADER
    buffer.insert(buffer.end(), frame.bytes.begin(), frame.bytes.end());
    a.sliceOffsets.reserve(tiles.size());
    a.tileSizes.reserve(tiles.size());

    // One tile per tile group avoids inheriting the client's packing or
    // needing tile-size fields between tile payloads.
    uint64_t end = 0;
    const unsigned tileBits = ceilLog2(p.tiles.cols) + ceilLog2(p.tiles.rows);
    for (size_t i = 0; i < tiles.size(); i++) {
      const auto& tile = tiles[i];
      if (tile.anchor_frame != 0xff || tile.row != i / p.tiles.cols || tile.column != i % p.tiles.cols
       || !tile.DataSize || tile.DataOffset < end)
        return false;
      end = uint64_t(tile.DataOffset) + tile.DataSize;
      if (end > bitstream.size() || uint64_t(buffer.size()) + tile.DataSize + 12 > UINT32_MAX)
        return false;
      Av1Bits group;
      if (tileBits) {
        group.flag(true); // tile_start_and_end_present_flag
        group.put(tileBits, i);
        group.put(tileBits, i);
      }
      appendObuHeader(buffer, 4, uint32_t(group.bytes.size() + tile.DataSize)); // OBU_TILE_GROUP
      buffer.insert(buffer.end(), group.bytes.begin(), group.bytes.end());
      a.sliceOffsets.push_back(uint32_t(buffer.size()));
      a.tileSizes.push_back(tile.DataSize);
      buffer.insert(buffer.end(), bitstream.data() + tile.DataOffset, bitstream.data() + end);
    }
    if (slots.empty())
      slots.resize(8, D3D11Av1Reference{0xff, 0, {}});
    for (unsigned i = 0; i < 8; i++) {
      if (a.picture.refresh_frame_flags & (1u << i))
        slots[i] = {a.currentPictureId, a.picture.OrderHint, a.motion};
      else if (slots[i].pictureId == a.currentPictureId)
        slots[i].pictureId = 0xff; // DXVA reused this surface for a new picture.
    }

    // Remember surfaces still in DXVA's map even when the reconstructed
    // refresh replaced their final alias. A later picture can still use them.
    if (a.picture.refresh_frame_flags != 0xff) {
      for (uint8_t id : a.referenceMap) {
        if (id == 0xff || byId(slots, id) != slots.end())
          continue;
        auto it = byId(refs, id);
        if (it != refs.end())
          slots.push_back(*it);
      }
    }
    refs = std::move(slots);
    bitstream = std::move(buffer);
    return true;
  }

}
