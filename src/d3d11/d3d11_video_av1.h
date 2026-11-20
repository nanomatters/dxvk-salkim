#pragma once

#include "d3d11_include.h"
#include "../dxvk/dxvk_video_av1.h"

#include <dxva.h>

namespace dxvk {

  struct D3D11Av1Reference {
    uint8_t pictureId;
    uint8_t orderHint;
    StdVideoAV1GlobalMotion motion;
  };

  bool translateAv1PictureParameters(
    const DXVA_PicParams_AV1&            params,
    const std::vector<DXVA_Tile_AV1>&    tiles,
    const D3D11_VIDEO_DECODER_DESC&      desc,
          std::vector<uint8_t>&          bitstream,
          DxvkAv1PictureParameters&      result,
          std::vector<D3D11Av1Reference>& references);

}
