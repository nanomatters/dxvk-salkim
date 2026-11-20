#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

#include "d3d11_context_imm.h"
#include "d3d11_video.h"
#include "d3d11_video_h264.h"

#include "../dxvk/dxvk_h264.h"

namespace dxvk {

  namespace {

    constexpr GUID H264Profile = { 0x1b81be68, 0xa0c7, 0x11d3, { 0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5 } };
    constexpr GUID NoEncryption = { 0x1b81bed0, 0xa0c7, 0x11d3, { 0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5 } };

    bool supportsH264Desc(D3D11Device* device, const D3D11_VIDEO_DECODER_DESC& desc) {
      if (!DxvkH264Decoder::supports(device->GetDXVKDevice().ptr()) || desc.Guid != H264Profile
       || desc.OutputFormat != DXGI_FORMAT_NV12
       || !desc.SampleWidth || !desc.SampleHeight
       || ((desc.SampleWidth | desc.SampleHeight) & 1u))
        return false;

      const auto& video = device->GetDXVKDevice()->properties().h264Decode;
      const auto& caps = video.capabilities;

      if (desc.SampleWidth > caps.maxCodedExtent.width
       || desc.SampleHeight > caps.maxCodedExtent.height)
        return false;

      VkExtent2D codedExtent = {
        align(desc.SampleWidth, 16u),
        align(desc.SampleHeight, 16u),
      };

      return codedExtent.width >= caps.minCodedExtent.width
          && codedExtent.height >= caps.minCodedExtent.height
          && codedExtent.width <= caps.maxCodedExtent.width
          && codedExtent.height <= caps.maxCodedExtent.height
          && h264LevelSupportsExtent(video.maxLevelIdc, codedExtent);
    }

    bool belongsToDevice(ID3D11DeviceChild* child, ID3D11Device* device) {
      if (!child)
        return false;

      Com<ID3D11Device> owner;
      child->GetDevice(&owner);
      return owner == device;
    }

    enum class DecoderBuffer : uint32_t {
      Picture, Matrix, Slice, Bitstream, Count,
    };

    DecoderBuffer getDecoderBuffer(D3D11_VIDEO_DECODER_BUFFER_TYPE type) {
      switch (type) {
        case D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS:         return DecoderBuffer::Picture;
        case D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX: return DecoderBuffer::Matrix;
        case D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL:              return DecoderBuffer::Slice;
        case D3D11_VIDEO_DECODER_BUFFER_BITSTREAM:                  return DecoderBuffer::Bitstream;
        default:                                                   return DecoderBuffer::Count;
      }
    }

    struct DecoderBufferStorage {
      enum class State { Available, Acquired, Released };

      State                state = State::Available;
      std::vector<uint8_t> bytes;
    };

    struct DecoderFrameData {
      DXVA_PicParams_H264   picture = { };
      DXVA_Qmatrix_H264     matrix = { };
      bool                  havePicture = false;
      bool                  haveMatrix = false;
      bool                  choppedSlice = false;
      std::vector<uint8_t>  bitstream;
      std::vector<uint32_t> offsets;
    };

    class D3D11VideoDecoderOutputView final : public D3D11DeviceChild<ID3D11VideoDecoderOutputView> {
    public:
      D3D11VideoDecoderOutputView(D3D11Device* device, ID3D11Resource* resource, D3D11CommonTexture* texture,
        const D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC& desc)
      : D3D11DeviceChild(device), m_resource(resource), m_texture(texture), m_desc(desc),
        m_destructionNotifier(this) { }

      HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object)
          return E_POINTER;

        *object = nullptr;

        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild)
         || riid == __uuidof(ID3D11View) || riid == __uuidof(ID3D11VideoDecoderOutputView)) {
          *object = ref(this);
          return S_OK;
        }

        if (riid == __uuidof(ID3DDestructionNotifier)) {
          *object = ref(&m_destructionNotifier);
          return S_OK;
        }

        return E_NOINTERFACE;
      }

      void STDMETHODCALLTYPE GetResource(ID3D11Resource** resource) override {
        if (resource)
          *resource = m_resource.ref();
      }

      void STDMETHODCALLTYPE GetDesc(D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC* desc) override {
        if (desc)
          *desc = m_desc;
      }

      D3D11CommonTexture* texture() const {
        return m_texture;
      }

      UINT layer() const {
        return m_desc.Texture2D.ArraySlice;
      }

    private:
      Com<ID3D11Resource>                 m_resource;
      D3D11CommonTexture*                m_texture;
      D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC m_desc;
      D3DDestructionNotifier             m_destructionNotifier;
    };


    class D3D11VideoDecoder final : public D3D11DeviceChild<ID3D11VideoDecoder> {
    public:
      D3D11VideoDecoder(D3D11Device* device, const D3D11_VIDEO_DECODER_DESC& desc, const D3D11_VIDEO_DECODER_CONFIG& config)
      : D3D11DeviceChild(device), m_desc(desc), m_config(config),
        m_extent{ align(desc.SampleWidth, 16u), align(desc.SampleHeight, 16u) },
        m_backend(new DxvkH264Decoder(device->GetDXVKDevice().ptr(), m_extent)),
        m_destructionNotifier(this) { }

      HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object)
          return E_POINTER;

        *object = nullptr;

        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild)
         || riid == __uuidof(ID3D11VideoDecoder)) {
          *object = ref(this);
          return S_OK;
        }

        if (riid == __uuidof(ID3DDestructionNotifier)) {
          *object = ref(&m_destructionNotifier);
          return S_OK;
        }

        return E_NOINTERFACE;
      }

      HRESULT STDMETHODCALLTYPE GetCreationParameters(D3D11_VIDEO_DECODER_DESC* desc, D3D11_VIDEO_DECODER_CONFIG* config) override {
        if (!desc || !config)
          return E_INVALIDARG;

        *desc = m_desc;
        *config = m_config;
        return S_OK;
      }

      HRESULT STDMETHODCALLTYPE GetDriverHandle(HANDLE* driverHandle) override {
        if (driverHandle)
          *driverHandle = nullptr;
        return E_NOTIMPL;
      }

      HRESULT beginFrame(D3D11VideoDecoderOutputView* output) {
        if (m_decodeFailed.load(std::memory_order_relaxed))
          return E_FAIL;

        if (m_output)
          return E_INVALIDARG;

        const auto& desc = *output->texture()->Desc();

        if (desc.Width < m_desc.SampleWidth || desc.Height < m_desc.SampleHeight)
          return E_INVALIDARG;

        m_output = output;
        return S_OK;
      }

      HRESULT acquireBuffer(DecoderBuffer type, UINT* size, void** data) {
        if (!m_output || type == DecoderBuffer::Count)
          return E_INVALIDARG;

        auto& storage = m_buffers[uint32_t(type)];

        if (storage.state == DecoderBufferStorage::State::Acquired)
          return E_INVALIDARG;

        try {
          if (storage.bytes.empty())
            storage.bytes.resize(getBufferSize(type));
        } catch (const std::bad_alloc&) {
          return E_OUTOFMEMORY;
        }

        storage.state = DecoderBufferStorage::State::Acquired;
        *size = UINT(storage.bytes.size());
        *data = storage.bytes.data();
        return S_OK;
      }

      HRESULT releaseBuffer(DecoderBuffer type) {
        if (!m_output || type == DecoderBuffer::Count)
          return E_INVALIDARG;

        auto& storage = m_buffers[uint32_t(type)];

        if (storage.state != DecoderBufferStorage::State::Acquired)
          return E_INVALIDARG;

        storage.state = DecoderBufferStorage::State::Released;
        return S_OK;
      }

      HRESULT submitBuffers(const D3D11_VIDEO_DECODER_BUFFER_DESC* buffers, UINT count);

      HRESULT endFrame(DxvkH264PictureParameters& params, std::vector<uint8_t>& bytes,
        Com<D3D11VideoDecoderOutputView>& output) {
        if (!m_output)
          return E_INVALIDARG;

        for (const auto& buffer : m_buffers) {
          if (buffer.state == DecoderBufferStorage::State::Acquired)
            return E_INVALIDARG;
        }

        output = std::move(m_output);
        auto frame = std::exchange(m_frame, { });

        for (auto& buffer : m_buffers)
          buffer.state = DecoderBufferStorage::State::Available;

        if (m_decodeFailed.load(std::memory_order_relaxed))
          return E_FAIL;

        if (!frame.havePicture || !frame.haveMatrix || frame.choppedSlice || frame.offsets.empty())
          return E_INVALIDARG;

        const uint32_t codedWidth = 16u * (uint32_t(frame.picture.wFrameWidthInMbsMinus1) + 1u);
        const uint32_t codedHeight = 16u * (uint32_t(frame.picture.wFrameHeightInMbsMinus1) + 1u);

        if (codedWidth != m_extent.width || codedHeight != m_extent.height)
          return E_INVALIDARG;

        // The Vulkan decoder backends expect a three-byte Annex B prefix.
        size_t size = 0;
        for (size_t i = 0; i < frame.offsets.size(); i++) {
          size_t start = frame.offsets[i];
          size_t end = i + 1 < frame.offsets.size() ? frame.offsets[i + 1] : frame.bitstream.size();

          if (end - start >= 4 && !std::memcmp(frame.bitstream.data() + start, "\0\0\0\1", 4))
            start++;

          frame.offsets[i] = uint32_t(size);
          std::memmove(frame.bitstream.data() + size, frame.bitstream.data() + start, end - start);
          size += end - start;
        }
        frame.bitstream.resize(size);

        const auto maxLevel = m_parent->GetDXVKDevice()->properties().h264Decode.maxLevelIdc;
        if (!translateH264PictureParameters(frame.picture, frame.matrix, frame.bitstream, frame.offsets, maxLevel, params))
          return E_INVALIDARG;

        bytes = std::move(frame.bitstream);
        return S_OK;
      }

      void decode(DxvkContext* ctx, const DxvkH264PictureParameters& params, const std::vector<uint8_t>& bytes,
        const Rc<DxvkImage>& image, uint32_t layer) {
        if (m_decodeFailed.load(std::memory_order_relaxed))
          return;

        try {
          auto frame = m_backend->decode(params, bytes);
          if (frame.image)
            ctx->copyH264Frame(frame, image, layer, { m_desc.SampleWidth, m_desc.SampleHeight });
        } catch (const DxvkError& error) {
          m_decodeFailed.store(true, std::memory_order_relaxed);
          Logger::err(error.message());
        } catch (const std::exception& error) {
          m_decodeFailed.store(true, std::memory_order_relaxed);
          Logger::err(error.what());
        }
      }

    private:
      size_t getBufferSize(DecoderBuffer type) const {
        const size_t pixels = size_t(m_extent.width) * m_extent.height;

        switch (type) {
          case DecoderBuffer::Picture:   return sizeof(DXVA_PicParams_H264);
          case DecoderBuffer::Matrix:    return sizeof(DXVA_Qmatrix_H264);
          case DecoderBuffer::Slice:     return (pixels / 256u) * sizeof(DXVA_Slice_H264_Short);
          case DecoderBuffer::Bitstream: return std::max<size_t>(1u << 20, pixels + pixels / 2u);
          default:                       return 0u;
        }
      }

      D3D11_VIDEO_DECODER_DESC             m_desc;
      D3D11_VIDEO_DECODER_CONFIG           m_config;
      VkExtent2D                          m_extent;
      Rc<DxvkH264Decoder>                  m_backend;
      Com<D3D11VideoDecoderOutputView>     m_output;
      DecoderFrameData                    m_frame;
      std::array<DecoderBufferStorage, uint32_t(DecoderBuffer::Count)> m_buffers;
      std::atomic<bool>                   m_decodeFailed = { false };
      D3DDestructionNotifier              m_destructionNotifier;
    };


    HRESULT D3D11VideoDecoder::submitBuffers(const D3D11_VIDEO_DECODER_BUFFER_DESC* buffers, UINT count) {
      if (!m_output || !count || count > m_buffers.size())
        return E_INVALIDARG;

      struct BufferData {
        const uint8_t* bytes;
        size_t         size;
      };

      std::array<BufferData, uint32_t(DecoderBuffer::Count)> data = { };

      for (UINT i = 0; i < count; i++) {
        const auto& desc = buffers[i];
        const auto type = getDecoderBuffer(desc.BufferType);

        if (type == DecoderBuffer::Count || desc.DataOffset || !desc.DataSize
         || desc.pIV || desc.IVSize || desc.PartialEncryption)
          return E_INVALIDARG;

        const auto idx = uint32_t(type);
        const auto& storage = m_buffers[idx];

        if (data[idx].size || storage.state != DecoderBufferStorage::State::Released
         || desc.DataSize > storage.bytes.size())
          return E_INVALIDARG;

        data[idx] = { storage.bytes.data(), desc.DataSize };
      }

      const auto picture = data[uint32_t(DecoderBuffer::Picture)];
      const auto matrix = data[uint32_t(DecoderBuffer::Matrix)];
      const auto slices = data[uint32_t(DecoderBuffer::Slice)];
      const auto bitstream = data[uint32_t(DecoderBuffer::Bitstream)];

      if ((picture.size && (m_frame.havePicture || picture.size != sizeof(m_frame.picture)))
       || (matrix.size && (m_frame.haveMatrix || matrix.size != sizeof(m_frame.matrix)))
       || !!slices.size != !!bitstream.size || slices.size % sizeof(DXVA_Slice_H264_Short))
        return E_INVALIDARG;

      bool choppedSlice = m_frame.choppedSlice;
      size_t byteCount = m_frame.bitstream.size();
      size_t sliceCount = m_frame.offsets.size();
      size_t previousEnd = 0u;

      // Validate the entire batch before updating the active frame.
      for (size_t offset = 0; offset < slices.size; offset += sizeof(DXVA_Slice_H264_Short)) {
        DXVA_Slice_H264_Short slice;
        std::memcpy(&slice, slices.bytes + offset, sizeof(slice));

        const size_t start = slice.BSNALunitDataLocation;
        const size_t size = slice.SliceBytesInBuffer;
        const bool startsSlice = !(slice.wBadSliceChopping & 2u);

        if (slice.wBadSliceChopping > 3u || startsSlice == choppedSlice
         || start < previousEnd || start > bitstream.size
         || !size || size > bitstream.size - start
         || size > std::numeric_limits<uint32_t>::max() - byteCount)
          return E_INVALIDARG;

        previousEnd = start + size;
        byteCount += size;
        sliceCount += startsSlice;
        choppedSlice = !!(slice.wBadSliceChopping & 1u);
      }

      try {
        m_frame.bitstream.reserve(byteCount);
        m_frame.offsets.reserve(sliceCount);
      } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
      } catch (const std::length_error&) {
        return E_OUTOFMEMORY;
      }

      if (picture.size) {
        std::memcpy(&m_frame.picture, picture.bytes, sizeof(m_frame.picture));
        m_frame.havePicture = true;
      }

      if (matrix.size) {
        std::memcpy(&m_frame.matrix, matrix.bytes, sizeof(m_frame.matrix));
        m_frame.haveMatrix = true;
      }

      for (size_t offset = 0; offset < slices.size; offset += sizeof(DXVA_Slice_H264_Short)) {
        DXVA_Slice_H264_Short slice;
        std::memcpy(&slice, slices.bytes + offset, sizeof(slice));

        if (!(slice.wBadSliceChopping & 2u))
          m_frame.offsets.push_back(uint32_t(m_frame.bitstream.size()));

        const auto* bytes = bitstream.bytes + slice.BSNALunitDataLocation;
        m_frame.bitstream.insert(m_frame.bitstream.end(), bytes, bytes + slice.SliceBytesInBuffer);
      }

      m_frame.choppedSlice = choppedSlice;

      for (UINT i = 0; i < count; i++)
        m_buffers[uint32_t(getDecoderBuffer(buffers[i].BufferType))].state = DecoderBufferStorage::State::Available;

      return S_OK;
    }

  }


  HRESULT STDMETHODCALLTYPE D3D11VideoDevice::CreateVideoDecoder(
    const D3D11_VIDEO_DECODER_DESC*   desc,
    const D3D11_VIDEO_DECODER_CONFIG* config,
          ID3D11VideoDecoder**       decoder) {
    InitReturnPtr(decoder);

    if (!desc || !config || !decoder || !supportsH264Desc(m_device, *desc)
     || config->ConfigBitstreamRaw != 2u || config->ConfigDecoderSpecific
     || config->guidConfigBitstreamEncryption != NoEncryption
     || config->guidConfigMBcontrolEncryption != NoEncryption
     || config->guidConfigResidDiffEncryption != NoEncryption)
      return E_INVALIDARG;

    try {
      *decoder = ref(new D3D11VideoDecoder(m_device, *desc, *config));
      return S_OK;
    } catch (const std::bad_alloc&) {
      return E_OUTOFMEMORY;
    } catch (const DxvkError& error) {
      Logger::err(error.message());
      return E_FAIL;
    } catch (const std::exception& error) {
      Logger::err(error.what());
      return E_FAIL;
    }
  }


  HRESULT STDMETHODCALLTYPE D3D11VideoDevice::CreateVideoDecoderOutputView(
          ID3D11Resource*                       resource,
    const D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC*  desc,
          ID3D11VideoDecoderOutputView**        view) {
    InitReturnPtr(view);

    if (!desc || desc->ViewDimension != D3D11_VDOV_DIMENSION_TEXTURE2D
     || desc->DecodeProfile != H264Profile || !DxvkH264Decoder::supports(m_device->GetDXVKDevice().ptr())
     || !belongsToDevice(resource, m_device))
      return E_INVALIDARG;

    D3D11_RESOURCE_DIMENSION dimension;
    resource->GetType(&dimension);

    if (dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
      return E_INVALIDARG;

    auto texture = GetCommonTexture(resource);
    const auto& textureDesc = *texture->Desc();

    if (textureDesc.Format != DXGI_FORMAT_NV12 || textureDesc.MipLevels != 1
     || textureDesc.SampleDesc.Count != 1 || textureDesc.Usage != D3D11_USAGE_DEFAULT
     || !(textureDesc.BindFlags & D3D11_BIND_DECODER)
     || desc->Texture2D.ArraySlice >= textureDesc.ArraySize)
      return E_INVALIDARG;

    if (!view)
      return S_OK;

    try {
      *view = ref(new D3D11VideoDecoderOutputView(m_device, resource, texture, *desc));
      return S_OK;
    } catch (const std::bad_alloc&) {
      return E_OUTOFMEMORY;
    }
  }


  UINT STDMETHODCALLTYPE D3D11VideoDevice::GetVideoDecoderProfileCount() {
    return DxvkH264Decoder::supports(m_device->GetDXVKDevice().ptr()) ? 1u : 0u;
  }


  HRESULT STDMETHODCALLTYPE D3D11VideoDevice::GetVideoDecoderProfile(
          UINT  index,
          GUID* profile) {
    if (!profile || index >= GetVideoDecoderProfileCount())
      return E_INVALIDARG;

    *profile = H264Profile;
    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D11VideoDevice::CheckVideoDecoderFormat(
    const GUID*       profile,
          DXGI_FORMAT format,
          BOOL*       supported) {
    if (!supported)
      return E_INVALIDARG;

    *supported = FALSE;

    if (!profile || *profile != H264Profile || !DxvkH264Decoder::supports(m_device->GetDXVKDevice().ptr()))
      return E_INVALIDARG;

    *supported = format == DXGI_FORMAT_NV12;
    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D11VideoDevice::GetVideoDecoderConfigCount(
    const D3D11_VIDEO_DECODER_DESC* desc,
          UINT*                    count) {
    if (!desc || !count)
      return E_INVALIDARG;

    *count = supportsH264Desc(m_device, *desc) ? 1u : 0u;
    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D11VideoDevice::GetVideoDecoderConfig(
    const D3D11_VIDEO_DECODER_DESC*   desc,
          UINT                       index,
          D3D11_VIDEO_DECODER_CONFIG* config) {
    if (!desc || !config || index || !supportsH264Desc(m_device, *desc))
      return E_INVALIDARG;

    *config = { };
    config->guidConfigBitstreamEncryption = NoEncryption;
    config->guidConfigMBcontrolEncryption = NoEncryption;
    config->guidConfigResidDiffEncryption = NoEncryption;
    config->ConfigBitstreamRaw = 2u;
    config->ConfigResidDiffAccelerator = 1u;
    config->ConfigHostInverseScan = 1u;
    config->ConfigSpecificIDCT = 2u;
    config->ConfigMinRenderTargetBuffCount = 1u;
    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D11VideoContext::GetDecoderBuffer(
          ID3D11VideoDecoder*             decoder,
          D3D11_VIDEO_DECODER_BUFFER_TYPE type,
          UINT*                          size,
          void**                         data) {
    D3D10DeviceLock lock = m_ctx->LockContext();

    if (!size || !data)
      return E_INVALIDARG;

    *size = 0u;
    *data = nullptr;

    if (!belongsToDevice(decoder, m_ctx->m_parent))
      return E_INVALIDARG;

    return static_cast<D3D11VideoDecoder*>(decoder)->acquireBuffer(getDecoderBuffer(type), size, data);
  }


  HRESULT STDMETHODCALLTYPE D3D11VideoContext::ReleaseDecoderBuffer(
          ID3D11VideoDecoder*             decoder,
          D3D11_VIDEO_DECODER_BUFFER_TYPE type) {
    D3D10DeviceLock lock = m_ctx->LockContext();

    if (!belongsToDevice(decoder, m_ctx->m_parent))
      return E_INVALIDARG;

    return static_cast<D3D11VideoDecoder*>(decoder)->releaseBuffer(getDecoderBuffer(type));
  }


  HRESULT STDMETHODCALLTYPE D3D11VideoContext::DecoderBeginFrame(
          ID3D11VideoDecoder*             decoder,
          ID3D11VideoDecoderOutputView*   view,
          UINT                            keySize,
    const void*                           key) {
    D3D10DeviceLock lock = m_ctx->LockContext();

    if (keySize || key || !belongsToDevice(decoder, m_ctx->m_parent)
     || !belongsToDevice(view, m_ctx->m_parent))
      return E_INVALIDARG;

    return static_cast<D3D11VideoDecoder*>(decoder)->beginFrame(static_cast<D3D11VideoDecoderOutputView*>(view));
  }


  HRESULT STDMETHODCALLTYPE D3D11VideoContext::DecoderEndFrame(
          ID3D11VideoDecoder* decoder) {
    D3D10DeviceLock lock = m_ctx->LockContext();

    if (!belongsToDevice(decoder, m_ctx->m_parent))
      return E_INVALIDARG;

    Com<D3D11VideoDecoder, false> videoDecoder = static_cast<D3D11VideoDecoder*>(decoder);
    Com<D3D11VideoDecoderOutputView> output;
    DxvkH264PictureParameters params = { };
    std::vector<uint8_t> bytes;

    try {
      HRESULT hr = videoDecoder->endFrame(params, bytes, output);

      if (FAILED(hr))
        return hr;

      m_ctx->EmitCs([
        cDecoder = std::move(videoDecoder),
        cImage   = output->texture()->GetImage(),
        cLayer   = output->layer(),
        cParams  = std::move(params),
        cBytes   = std::move(bytes)
      ] (DxvkContext* ctx) {
        cDecoder->decode(ctx, cParams, cBytes, cImage, cLayer);
      });

      return S_OK;
    } catch (const std::bad_alloc&) {
      return E_OUTOFMEMORY;
    } catch (const DxvkError& error) {
      Logger::err(error.message());
      return E_FAIL;
    } catch (const std::exception& error) {
      Logger::err(error.what());
      return E_FAIL;
    }
  }


  HRESULT STDMETHODCALLTYPE D3D11VideoContext::SubmitDecoderBuffers(
          ID3D11VideoDecoder*                   decoder,
          UINT                                  count,
    const D3D11_VIDEO_DECODER_BUFFER_DESC*       descs) {
    D3D10DeviceLock lock = m_ctx->LockContext();

    if (!descs || !belongsToDevice(decoder, m_ctx->m_parent))
      return E_INVALIDARG;

    return static_cast<D3D11VideoDecoder*>(decoder)->submitBuffers(descs, count);
  }

}
