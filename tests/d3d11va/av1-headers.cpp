#include "d3d11_video_av1.h"

#include <cstdio>

// Each captured submission starts with its buffer lengths and decoder description.
struct FrameRecord {
  uint32_t tileCount;
  uint32_t bitstreamSize;
  uint32_t width;
  uint32_t height;
  uint32_t format;
};

static_assert(sizeof(FrameRecord) == 20);
static_assert(sizeof(DXVA_PicParams_AV1) == 912);
static_assert(sizeof(DXVA_Tile_AV1) == 16);

int main() {
  std::vector<dxvk::D3D11Av1Reference> references;
  // Vulkan carries the sequence parameters separately. This sequence OBU
  // describes the test clip using the sequence flags chosen by DXVK.
  const uint8_t sequence[] = { 0x0a, 0x0a, 0x00, 0x00, 0x00, 0x03, 0xb7, 0xff, 0xf3, 0x6b, 0xee, 0x07 };
  const uint8_t delimiter[] = { 0x12, 0x00 };

  for (unsigned int i = 0; i < 3; ++i) {
    FrameRecord record;
    DXVA_PicParams_AV1 picture;
    if (std::fread(&record, sizeof(record), 1, stdin) != 1
     || record.tileCount != 4 || record.bitstreamSize > 4096
     || std::fread(&picture, sizeof(picture), 1, stdin) != 1)
      return 1;

    std::vector<DXVA_Tile_AV1> tiles(record.tileCount);
    std::vector<uint8_t> bitstream(record.bitstreamSize);
    if (std::fread(tiles.data(), sizeof(tiles[0]), tiles.size(), stdin) != tiles.size()
     || std::fread(bitstream.data(), 1, bitstream.size(), stdin) != bitstream.size())
      return 1;

    D3D11_VIDEO_DECODER_DESC desc = { };
    desc.SampleWidth = record.width;
    desc.SampleHeight = record.height;
    desc.OutputFormat = DXGI_FORMAT(record.format);
    dxvk::DxvkAv1PictureParameters params;
    if (!dxvk::translateAv1PictureParameters(picture, tiles, desc, bitstream, params, references)) {
      std::fprintf(stderr, "DXVK rejected picture %u\n", i);
      return 1;
    }

    std::fwrite(delimiter, 1, sizeof(delimiter), stdout);
    if (!i)
      std::fwrite(sequence, 1, sizeof(sequence), stdout);
    std::fwrite(bitstream.data(), 1, bitstream.size(), stdout);
  }

  return std::fgetc(stdin) != EOF || std::ferror(stdin) || std::fflush(stdout) || std::ferror(stdout);
}
