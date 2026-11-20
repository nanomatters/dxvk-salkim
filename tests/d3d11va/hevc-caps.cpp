#include <initguid.h>
#include <d3d11.h>
#include <dxva.h>
#include <wrl/client.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

static void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

static void checkProfile(ID3D11VideoDevice* device, const GUID& profile, DXGI_FORMAT format,
                         const char* name, bool expected) {
  UINT found = 0;
  for (UINT i = 0; i < device->GetVideoDecoderProfileCount(); i++) {
    GUID id;
    require(SUCCEEDED(device->GetVideoDecoderProfile(i, &id)), "GetVideoDecoderProfile failed");
    found += id == profile;
  }
  std::printf("%s: %u profile(s), expected %u\n", name, found, unsigned(expected));
  require(found == unsigned(expected), "Incorrect profile enumeration");

  BOOL supported = FALSE;
  HRESULT hr = device->CheckVideoDecoderFormat(&profile, format, &supported);
  require(expected == (SUCCEEDED(hr) && supported), "Incorrect format support");


  D3D11_VIDEO_DECODER_DESC desc = { profile, 320, 192, format };
  UINT count = 0;
  hr = device->GetVideoDecoderConfigCount(&desc, &count);
  require(expected == (SUCCEEDED(hr) && count != 0), "Incorrect decoder config count");

  // A usable HEVC VLD decoder must accept unencrypted raw bitstreams.
  bool haveConfig = false;
  for (UINT i = 0; i < count; i++) {
    D3D11_VIDEO_DECODER_CONFIG config = { };
    require(SUCCEEDED(device->GetVideoDecoderConfig(&desc, i, &config)), "GetVideoDecoderConfig failed");
    if (config.ConfigBitstreamRaw == 1 && config.guidConfigBitstreamEncryption == DXVA_NoEncrypt) {
      ComPtr<ID3D11VideoDecoder> decoder;
      require(SUCCEEDED(device->CreateVideoDecoder(&desc, &config, &decoder)), "CreateVideoDecoder failed");
      haveConfig = true;
      break;
    }
  }
  require(expected == haveConfig, "Incorrect unencrypted short-slice decoder availability");

  const std::array invalidFormats = {
    DXGI_FORMAT_R8G8B8A8_UNORM, format == DXGI_FORMAT_NV12 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12,
  };
  for (auto invalid : invalidFormats) {
    supported = FALSE;
    hr = device->CheckVideoDecoderFormat(&profile, invalid, &supported);
    require(FAILED(hr) || !supported, "Unexpected decoder output format");
    desc.OutputFormat = invalid;
    count = 0;
    hr = device->GetVideoDecoderConfigCount(&desc, &count);
    require(FAILED(hr) || count == 0, "Incorrect config count for unsupported output");
  }
}

int main(int argc, char** argv) {
  bool main8 = false, main10 = false;
  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "main"))
      main8 = true;
    else if (!std::strcmp(argv[i], "main10"))
      main10 = true;
    else {
      std::fputs("Usage: hevc-caps.exe [main] [main10]\n", stderr);
      return 2;
    }
  }

  try {
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11VideoDevice> video;
    require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, nullptr)), "D3D11CreateDevice failed");
    require(SUCCEEDED(device.As(&video)), "ID3D11VideoDevice unavailable");
    checkProfile(video.Get(), DXVA_ModeHEVC_VLD_Main, DXGI_FORMAT_NV12, "Main/NV12", main8);
    checkProfile(video.Get(), DXVA_ModeHEVC_VLD_Main10, DXGI_FORMAT_P010, "Main 10/P010", main10);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
