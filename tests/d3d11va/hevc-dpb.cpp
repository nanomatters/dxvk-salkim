#include <initguid.h>
#include <d3d11.h>
#include <dxva.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;

constexpr UINT width = 128, height = 96;

// The four VCL NAL units in cra-dpb-growth.hevc, in decode order. Offsets
// include the three-byte start code. Hashes are FNV-1a of software-decoded NV12.
constexpr struct {
  UINT offset, size;
  int poc;
  USHORT rpsBits;
  uint32_t hash;
} frames[] = {
  {   84, 2227, 0,  0, 0x3c2c840f }, // IDR
  { 2396, 2809, 4,  2, 0x3f426858 }, // CRA, or BLA_N_LP after changing the NAL type
  { 5206, 1457, 6,  8, 0x3162fa7a }, // P
  { 6664, 1318, 5, 10, 0xd5890de2 }, // B
};

static void check(HRESULT hr, const char* name) {
  if (FAILED(hr)) {
    std::fprintf(stderr, "%s failed: %#010lx\n", name, static_cast<unsigned long>(hr));
    throw std::runtime_error("D3D11 call failed");
  }
}

static DXVA_PicParams_HEVC pictureParams(UINT i) {
  DXVA_PicParams_HEVC p = { };
  p.PicWidthInMinCbsY = width / 8;
  p.PicHeightInMinCbsY = height / 8;
  p.chroma_format_idc = 1;
  p.log2_diff_max_min_luma_coding_block_size = 3;
  p.log2_diff_max_min_transform_block_size = 3;
  p.log2_max_pic_order_cnt_lsb_minus4 = 4;
  p.sps_max_dec_pic_buffering_minus1 = i == 0 ? 1 : 3;
  p.strong_intra_smoothing_enabled_flag = 1;
  p.sign_data_hiding_enabled_flag = 1;
  p.cu_qp_delta_enabled_flag = 1;
  p.diff_cu_qp_delta_depth = 1;
  p.weighted_pred_flag = 1;
  p.pps_loop_filter_across_slices_enabled_flag = 1;
  p.CurrPicOrderCntVal = frames[i].poc;
  p.CurrPic.bPicEntry = i;
  p.StatusReportFeedbackNumber = i + 1;
  p.IntraPicFlag = p.IrapPicFlag = i < 2;
  p.IdrPicFlag = i == 0;
  p.wNumBitsForShortTermRPSInSlice = frames[i].rpsBits;

  std::memset(p.RefPicList, 0xff, sizeof(p.RefPicList));
  std::memset(p.RefPicSetStCurrBefore, 0xff, sizeof(p.RefPicSetStCurrBefore));
  std::memset(p.RefPicSetStCurrAfter, 0xff, sizeof(p.RefPicSetStCurrAfter));
  std::memset(p.RefPicSetLtCurr, 0xff, sizeof(p.RefPicSetLtCurr));
  if (i >= 2) {
    p.RefPicList[0].bPicEntry = 1;
    p.PicOrderCntValList[0] = frames[1].poc;
    p.RefPicSetStCurrBefore[0] = 0;
  }
  if (i == 3) {
    p.RefPicList[1].bPicEntry = 2;
    p.PicOrderCntValList[1] = frames[2].poc;
    p.RefPicSetStCurrAfter[0] = 1;
  }
  return p;
}

static D3D11_VIDEO_DECODER_BUFFER_DESC fillBuffer(ID3D11VideoContext* context,
    ID3D11VideoDecoder* decoder, D3D11_VIDEO_DECODER_BUFFER_TYPE type, const void* data, UINT size) {
  void* buffer = nullptr;
  UINT capacity = 0;
  check(context->GetDecoderBuffer(decoder, type, &capacity, &buffer), "GetDecoderBuffer");
  if (capacity < size || !buffer) {
    check(context->ReleaseDecoderBuffer(decoder, type), "ReleaseDecoderBuffer");
    throw std::runtime_error("Decoder buffer too small");
  }
  std::memcpy(buffer, data, size);
  check(context->ReleaseDecoderBuffer(decoder, type), "ReleaseDecoderBuffer");
  D3D11_VIDEO_DECODER_BUFFER_DESC desc = { };
  desc.BufferType = type;
  desc.DataSize = size;
  return desc;
}

static void run(ID3D11Device* device, ID3D11DeviceContext* context, std::ifstream& fixture, bool bla) {
  ComPtr<ID3D11VideoDevice> videoDevice;
  ComPtr<ID3D11VideoContext> videoContext;
  check(device->QueryInterface(IID_PPV_ARGS(&videoDevice)), "Query ID3D11VideoDevice");
  check(context->QueryInterface(IID_PPV_ARGS(&videoContext)), "Query ID3D11VideoContext");

  const D3D11_VIDEO_DECODER_DESC desc = { DXVA_ModeHEVC_VLD_Main, width, height, DXGI_FORMAT_NV12 };
  D3D11_VIDEO_DECODER_CONFIG config = { };
  UINT count = 0;
  check(videoDevice->GetVideoDecoderConfigCount(&desc, &count), "GetVideoDecoderConfigCount");
  for (UINT i = 0; i < count; i++) {
    check(videoDevice->GetVideoDecoderConfig(&desc, i, &config), "GetVideoDecoderConfig");
    if (config.ConfigBitstreamRaw == 1 && config.guidConfigBitstreamEncryption == DXVA_NoEncrypt)
      break;
  }
  if (config.ConfigBitstreamRaw != 1 || config.guidConfigBitstreamEncryption != DXVA_NoEncrypt)
    throw std::runtime_error("No unencrypted HEVC Main VLD configuration");

  ComPtr<ID3D11VideoDecoder> decoder;
  check(videoDevice->CreateVideoDecoder(&desc, &config, &decoder), "CreateVideoDecoder");
  D3D11_TEXTURE2D_DESC image = { };
  image.Width = width;
  image.Height = height;
  image.MipLevels = 1;
  image.ArraySize = std::max<UINT>(std::size(frames), config.ConfigMinRenderTargetBuffCount);
  image.Format = DXGI_FORMAT_NV12;
  image.SampleDesc.Count = 1;
  image.BindFlags = D3D11_BIND_DECODER;
  ComPtr<ID3D11Texture2D> output, staging;
  check(device->CreateTexture2D(&image, nullptr, &output), "Create NV12 output");
  image.ArraySize = 1;
  image.BindFlags = 0;
  image.Usage = D3D11_USAGE_STAGING;
  image.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  check(device->CreateTexture2D(&image, nullptr, &staging), "Create NV12 staging texture");

  for (UINT i = 0; i < std::size(frames); i++) {
    const auto& frame = frames[i];
    std::vector<uint8_t> bitstream((frame.size + 127u) & ~127u);
    fixture.seekg(frame.offset);
    if (!fixture.read(reinterpret_cast<char*>(bitstream.data()), frame.size)
     || std::memcmp(bitstream.data(), "\x00\x00\x01", 3))
      throw std::runtime_error("Expected the cra-dpb-growth.hevc fixture");

    // This CRA has an empty RPS and no leading pictures. Changing only its
    // NAL type to BLA_N_LP preserves the expected decoded pixels.
    if (i == 1) {
      if (bitstream[3] != 21u << 1)
        throw std::runtime_error("Expected a CRA picture");
      if (bla)
        bitstream[3] = 18u << 1;
    }

    D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC viewDesc = { };
    viewDesc.DecodeProfile = desc.Guid;
    viewDesc.ViewDimension = D3D11_VDOV_DIMENSION_TEXTURE2D;
    viewDesc.Texture2D.ArraySlice = i;
    ComPtr<ID3D11VideoDecoderOutputView> view;
    check(videoDevice->CreateVideoDecoderOutputView(output.Get(), &viewDesc, &view), "Create output view");
    check(videoContext->DecoderBeginFrame(decoder.Get(), view.Get(), 0, nullptr), "DecoderBeginFrame");
    const auto params = pictureParams(i);
    const DXVA_Slice_HEVC_Short slice = { 0, UINT(bitstream.size()), 0 };
    const D3D11_VIDEO_DECODER_BUFFER_DESC buffers[] = {
      fillBuffer(videoContext.Get(), decoder.Get(), D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, &params, sizeof(params)),
      fillBuffer(videoContext.Get(), decoder.Get(), D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL, &slice, sizeof(slice)),
      fillBuffer(videoContext.Get(), decoder.Get(), D3D11_VIDEO_DECODER_BUFFER_BITSTREAM, bitstream.data(), slice.SliceBytesInBuffer),
    };
    check(videoContext->SubmitDecoderBuffers(decoder.Get(), std::size(buffers), buffers), "SubmitDecoderBuffers");
    check(videoContext->DecoderEndFrame(decoder.Get()), "DecoderEndFrame");
  }

  for (UINT i = 0; i < std::size(frames); i++) {
    context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, output.Get(), i, nullptr);
    D3D11_MAPPED_SUBRESOURCE map = { };
    check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map), "Map NV12 readback");
    uint32_t hash = 2166136261u;
    for (UINT y = 0; y < height + height / 2; y++) {
      const auto* row = static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch;
      for (UINT x = 0; x < width; x++)
        hash = (hash ^ row[x]) * 16777619u;
    }
    context->Unmap(staging.Get(), 0);
    std::printf("%s frame %u: %08x (expected %08x)\n", bla ? "BLA" : "CRA", i, hash, frames[i].hash);
    if (hash != frames[i].hash)
      throw std::runtime_error("Decoded pixels differ from software");
  }
}

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fputs("Usage: hevc-dpb.exe cra-dpb-growth.hevc\n", stderr);
    return 2;
  }
  try {
    std::ifstream fixture(argv[1], std::ios::binary);
    if (!fixture)
      throw std::runtime_error("Cannot open HEVC fixture");
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
      D3D11_SDK_VERSION, &device, nullptr, &context), "D3D11CreateDevice");
    run(device.Get(), context.Get(), fixture, false);
    run(device.Get(), context.Get(), fixture, true);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
