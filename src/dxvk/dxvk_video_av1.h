#pragma once

#include <array>
#include <vector>

#include "../vulkan/vulkan_loader.h"

namespace dxvk {

  struct DxvkAv1SessionParameters {
    StdVideoAV1SequenceHeader sequence = { };
    StdVideoAV1ColorConfig color = { };

    StdVideoAV1SequenceHeader sequenceHeader() const {
      auto result = sequence;
      result.pColorConfig = &color;
      return result;
    }
  };

  struct DxvkAv1PictureParameters : DxvkAv1SessionParameters {
    VkExtent2D extent = { };
    uint8_t currentPictureId = 0;
    std::array<uint8_t, 7> referenceNames = { };
    std::array<uint8_t, 8> referenceMap = { };
    uint32_t frameHeaderOffset = 0;
    std::vector<uint32_t> sliceOffsets;
    std::vector<uint32_t> tileSizes;
    StdVideoDecodeAV1PictureInfo picture = { };
    StdVideoAV1TileInfo tiles = { };
    std::array<uint16_t, 65> tileColumns = { }, tileRows = { };
    std::array<uint16_t, 64> tileWidths = { }, tileHeights = { };
    StdVideoAV1Quantization quantization = { };
    StdVideoAV1Segmentation segmentation = { };
    StdVideoAV1LoopFilter loopFilter = { };
    StdVideoAV1CDEF cdef = { };
    StdVideoAV1LoopRestoration restoration = { };
    StdVideoAV1GlobalMotion motion = { };
    StdVideoAV1FilmGrain grain = { };

    StdVideoDecodeAV1PictureInfo pictureInfo(StdVideoAV1TileInfo& tileInfo) const {
      tileInfo = tiles;
      tileInfo.pMiColStarts = tileColumns.data();
      tileInfo.pMiRowStarts = tileRows.data();
      tileInfo.pWidthInSbsMinus1 = tileWidths.data();
      tileInfo.pHeightInSbsMinus1 = tileHeights.data();

      auto result = picture;
      result.pTileInfo = &tileInfo;
      result.pQuantization = &quantization;
      result.pSegmentation = &segmentation;
      result.pLoopFilter = &loopFilter;
      result.pCDEF = &cdef;
      result.pLoopRestoration = &restoration;
      result.pGlobalMotion = &motion;
      result.pFilmGrain = picture.flags.apply_grain ? &grain : nullptr;
      return result;
    }
  };

}
