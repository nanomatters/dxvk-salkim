#include <initguid.h>
#include <d3d11.h>
#include <dxva.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <stdexcept>

#include "h264_fixture.h"

using Microsoft::WRL::ComPtr;

static void check(HRESULT hr, const char* operation) {
  if (FAILED(hr)) {
    std::fprintf(stderr, "%s failed: %#010lx\n", operation, static_cast<unsigned long>(hr));
    throw std::runtime_error("D3D11 call failed");
  }
}

static bool supportsH264(ID3D11VideoDevice* device) {
  UINT count = device->GetVideoDecoderProfileCount();
  std::printf("Decoder profiles: %u\n", count);

  for (UINT i = 0; i < count; i++) {
    GUID profile;
    check(device->GetVideoDecoderProfile(i, &profile), "GetVideoDecoderProfile");
    if (profile == DXVA_ModeH264_VLD_NoFGT)
      return true;
  }

  return false;
}

static D3D11_VIDEO_DECODER_CONFIG selectConfig(
        ID3D11VideoDevice*              device,
  const D3D11_VIDEO_DECODER_DESC&       desc) {
  BOOL supported = FALSE;
  UINT count = 0;
  check(device->CheckVideoDecoderFormat(&desc.Guid, desc.OutputFormat, &supported),
        "CheckVideoDecoderFormat");

  if (!supported)
    throw std::runtime_error("H.264 NV12 is unavailable");

  check(device->GetVideoDecoderConfigCount(&desc, &count), "GetVideoDecoderConfigCount");

  for (UINT i = 0; i < count; i++) {
    D3D11_VIDEO_DECODER_CONFIG config = { };
    check(device->GetVideoDecoderConfig(&desc, i, &config), "GetVideoDecoderConfig");

    if (config.ConfigBitstreamRaw == 2
     && config.guidConfigBitstreamEncryption == DXVA_NoEncrypt)
      return config;
  }

  throw std::runtime_error("No unencrypted H.264 short-slice configuration");
}

static D3D11_VIDEO_DECODER_BUFFER_DESC submitBuffer(
        ID3D11VideoContext*             context,
        ID3D11VideoDecoder*             decoder,
        D3D11_VIDEO_DECODER_BUFFER_TYPE type,
  const void*                          source,
        UINT                           size) {
  void* buffer = nullptr;
  UINT capacity = 0;

  check(context->GetDecoderBuffer(decoder, type, &capacity, &buffer), "GetDecoderBuffer");

  if (!buffer || capacity < size) {
    check(context->ReleaseDecoderBuffer(decoder, type), "ReleaseDecoderBuffer");
    throw std::runtime_error("Decoder buffer is too small");
  }

  std::memcpy(buffer, source, size);
  check(context->ReleaseDecoderBuffer(decoder, type), "ReleaseDecoderBuffer");

  D3D11_VIDEO_DECODER_BUFFER_DESC desc = { };
  desc.BufferType = type;
  desc.DataSize = size;
  return desc;
}

static uint32_t readback(
        ID3D11DeviceContext*           context,
        ID3D11Texture2D*                output,
        ID3D11Texture2D*                staging,
        UINT                           slice) {
  context->CopySubresourceRegion(staging, 0, 0, 0, 0, output, slice, nullptr);

  D3D11_MAPPED_SUBRESOURCE mapped = { };
  check(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped), "Map NV12 readback");

  // Hash the visible Y and interleaved UV bytes, without row padding.
  uint32_t hash = 2166136261u;
  for (UINT y = 0; y < fixtureHeight + fixtureHeight / 2; y++) {
    const auto* row = static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch;
    for (UINT x = 0; x < fixtureWidth; x++)
      hash = (hash ^ row[x]) * 16777619u;
  }

  context->Unmap(staging, 0);
  return hash;
}

static void decode(
        ID3D11Device*                  device,
        ID3D11DeviceContext*           context,
        ID3D11VideoDevice*              videoDevice,
        bool                           recoverMissingReference,
        bool                           growDpb) {
  ComPtr<ID3D11VideoContext> videoContext;
  check(context->QueryInterface(IID_PPV_ARGS(&videoContext)), "Query ID3D11VideoContext");

  const D3D11_VIDEO_DECODER_DESC desc = {
    DXVA_ModeH264_VLD_NoFGT, fixtureWidth, fixtureHeight, DXGI_FORMAT_NV12
  };

  const auto config = selectConfig(videoDevice, desc);
  ComPtr<ID3D11VideoDecoder> decoder;
  check(videoDevice->CreateVideoDecoder(&desc, &config, &decoder), "CreateVideoDecoder");

  D3D11_VIDEO_DECODER_DESC reportedDesc = { };
  D3D11_VIDEO_DECODER_CONFIG reportedConfig = { };
  check(decoder->GetCreationParameters(&reportedDesc, &reportedConfig), "GetCreationParameters");
  if (reportedDesc.Guid != desc.Guid || reportedDesc.OutputFormat != desc.OutputFormat
   || reportedDesc.SampleWidth != desc.SampleWidth || reportedDesc.SampleHeight != desc.SampleHeight
   || reportedConfig.ConfigBitstreamRaw != config.ConfigBitstreamRaw)
    throw std::runtime_error("Decoder returned different creation parameters");

  constexpr UINT recoveryViewCount = 6;
  constexpr UINT growthViewCount = 4;
  const UINT viewCount = recoverMissingReference ? recoveryViewCount
    : growDpb ? growthViewCount : std::size(fixtureFrames);

  D3D11_TEXTURE2D_DESC textureDesc = { };
  textureDesc.Width = fixtureWidth;
  textureDesc.Height = fixtureHeight;
  textureDesc.MipLevels = 1;
  textureDesc.ArraySize = std::max<UINT>(viewCount, config.ConfigMinRenderTargetBuffCount);
  textureDesc.Format = DXGI_FORMAT_NV12;
  textureDesc.SampleDesc.Count = 1;
  textureDesc.Usage = D3D11_USAGE_DEFAULT;
  textureDesc.BindFlags = D3D11_BIND_DECODER;

  ComPtr<ID3D11Texture2D> output;
  check(device->CreateTexture2D(&textureDesc, nullptr, &output), "Create NV12 decoder texture");

  textureDesc.ArraySize = 1;
  textureDesc.Usage = D3D11_USAGE_STAGING;
  textureDesc.BindFlags = 0;
  textureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

  ComPtr<ID3D11Texture2D> staging;
  check(device->CreateTexture2D(&textureDesc, nullptr, &staging), "Create NV12 staging texture");

  DXVA_Qmatrix_H264 matrix;
  std::memset(&matrix, 16, sizeof(matrix));
  ComPtr<ID3D11VideoDecoderOutputView> views[recoveryViewCount];

  for (UINT i = 0; i < viewCount; i++) {
    D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC viewDesc = { };
    viewDesc.DecodeProfile = desc.Guid;
    viewDesc.ViewDimension = D3D11_VDOV_DIMENSION_TEXTURE2D;
    viewDesc.Texture2D.ArraySlice = i;
    check(videoDevice->CreateVideoDecoderOutputView(output.Get(), &viewDesc, &views[i]),
          "CreateVideoDecoderOutputView");
  }

  UINT feedbackNumber = 0;
  const auto submitFrame = [&](UINT frameIndex, UINT surface, UINT reference, UINT referenceCount = 1) {
    const auto& frame = fixtureFrames[frameIndex];
    const DXVA_Slice_H264_Short slice = { 0, frame.size, 0 };
    auto parameters = fixturePictureParameters(frameIndex);
    parameters.CurrPic.Index7Bits = surface;
    parameters.num_ref_frames = referenceCount;
    parameters.StatusReportFeedbackNumber = ++feedbackNumber;
    if (frameIndex == 1)
      parameters.RefFrameList[0].Index7Bits = reference;

    check(videoContext->DecoderBeginFrame(decoder.Get(), views[surface].Get(), 0, nullptr), "DecoderBeginFrame");
    const D3D11_VIDEO_DECODER_BUFFER_DESC buffers[] = {
      submitBuffer(videoContext.Get(), decoder.Get(), D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS,
                   &parameters, sizeof(parameters)),
      submitBuffer(videoContext.Get(), decoder.Get(), D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX,
                   &matrix, sizeof(matrix)),
      submitBuffer(videoContext.Get(), decoder.Get(), D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL,
                   &slice, sizeof(slice)),
      submitBuffer(videoContext.Get(), decoder.Get(), D3D11_VIDEO_DECODER_BUFFER_BITSTREAM,
                   frame.data, frame.size),
    };

    check(videoContext->SubmitDecoderBuffers(decoder.Get(), std::size(buffers), buffers),
          "SubmitDecoderBuffers");
    check(videoContext->DecoderEndFrame(decoder.Get()), "DecoderEndFrame");
  };

  const auto checkFrame = [&](UINT frameIndex, UINT surface) {
    const uint32_t hash = readback(context, output.Get(), staging.Get(), surface);
    const uint32_t expected = fixtureFrames[frameIndex].expectedHash;
    std::printf("Frame %u NV12 hash: %08x (expected %08x)\n", frameIndex, hash, expected);
    if (hash != expected)
      throw std::runtime_error("Decoded pixels differ from software");
  };

  const auto submitPair = [&](UINT idrSurface, UINT pSurface, UINT referenceCount = 1) {
    submitFrame(0, idrSurface, 0, referenceCount);
    submitFrame(1, pSurface, idrSurface, referenceCount);
  };

  const auto checkPair = [&](UINT idrSurface, UINT pSurface) {
    checkFrame(0, idrSurface);
    checkFrame(1, pSurface);
  };

  if (growDpb) {
    for (UINT referenceCount = 2; referenceCount <= 3; referenceCount++) {
      std::printf("Increase the decoded picture buffer limit to %u references\n", referenceCount);
      // Queue both pairs before readback so the second IDR can replace the DPB
      // while the first pair's output copies are still in flight.
      submitPair(0, 1);
      submitPair(2, 3, referenceCount);
      checkPair(0, 1);
      checkPair(2, 3);
    }
    return;
  }

  submitPair(0, 1);
  checkPair(0, 1);

  if (recoverMissingReference) {
    constexpr UINT missingReference = 31;

    std::puts("Recover from a missing reference after a valid IDR and P frame");
    submitFrame(1, 0, missingReference);
    // Read back the destination to wait for the queued decode and verify it was untouched.
    checkFrame(0, 0);
    submitPair(2, 3);
    checkPair(2, 3);

    std::puts("Recover from a missing reference on the first frame");
    decoder.Reset();
    check(videoDevice->CreateVideoDecoder(&desc, &config, &decoder), "CreateVideoDecoder");
    feedbackNumber = 0;
    submitFrame(1, 0, missingReference);
    checkFrame(0, 0);
    submitPair(4, 5);
    checkPair(4, 5);
  }
}

int main(int argc, char** argv) {
  UINT flags = 0;
  bool expectUnsupported = false;
  bool recoverMissingReference = false;
  bool growDpb = false;

  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "--video-support")) {
      flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    } else if (!std::strcmp(argv[i], "--expect-unsupported")) {
      expectUnsupported = true;
    } else if (!std::strcmp(argv[i], "--recover-missing-reference")) {
      recoverMissingReference = true;
    } else if (!std::strcmp(argv[i], "--grow-dpb")) {
      growDpb = true;
    } else {
      std::fputs("Usage: h264.exe [--video-support] [--expect-unsupported | --recover-missing-reference | --grow-dpb]\n",
        stderr);
      return 2;
    }
  }

  if (unsigned(expectUnsupported) + unsigned(recoverMissingReference) + unsigned(growDpb) > 1) {
    std::fputs("Choose one of --expect-unsupported, --recover-missing-reference or --grow-dpb.\n", stderr);
    return 2;
  }

  try {
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
          flags, &level, 1, D3D11_SDK_VERSION,
          &device, nullptr, &context), "D3D11CreateDevice");

    ComPtr<ID3D11VideoDevice> videoDevice;
    check(device->QueryInterface(IID_PPV_ARGS(&videoDevice)), "Query ID3D11VideoDevice");

    const bool supported = supportsH264(videoDevice.Get());
    if (expectUnsupported) {
      if (supported)
        throw std::runtime_error("Unexpected H.264 decoder profile");
      std::puts("H.264 decoder profile is absent as expected");
    } else {
      if (!supported)
        throw std::runtime_error("H.264 decoder profile is unavailable");
      decode(device.Get(), context.Get(), videoDevice.Get(), recoverMissingReference, growDpb);
    }
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
