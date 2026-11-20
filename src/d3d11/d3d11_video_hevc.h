#pragma once

#include "d3d11_include.h"

#include "../dxvk/dxvk_video_decoder.h"

#include <dxva.h>

namespace dxvk {

  bool h265LevelSupportsExtent(
          StdVideoH265LevelIdc           level,
          VkExtent2D                     extent);

  bool translateH265PictureParameters(
    const DXVA_PicParams_HEVC&           params,
    const DXVA_Qmatrix_HEVC*             matrix,
    const std::vector<uint8_t>&          bitstream,
    const std::vector<uint32_t>&         sliceOffsets,
          StdVideoH265ProfileIdc         profile,
          StdVideoH265LevelIdc           maxLevel,
          DxvkH265PictureParameters&     result,
          uint8_t                        previousMaxTemporalId = 0);

}
