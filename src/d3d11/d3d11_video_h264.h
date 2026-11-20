#pragma once

#include "d3d11_include.h"

#include "../dxvk/dxvk_video_decoder.h"

#include <dxva.h>

namespace dxvk {

  bool h264LevelSupportsExtent(
          StdVideoH264LevelIdc          level,
          VkExtent2D                    extent);

  bool translateH264PictureParameters(
    const DXVA_PicParams_H264&           params,
    const DXVA_Qmatrix_H264&             matrix,
    const std::vector<uint8_t>&          bitstream,
    const std::vector<uint32_t>&         sliceOffsets,
          StdVideoH264LevelIdc          maxLevel,
          DxvkH264PictureParameters&    result);

}
