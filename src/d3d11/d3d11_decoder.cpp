#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <variant>
#include <vector>

#include "d3d11_context_imm.h"
#include "d3d11_video.h"
#include "d3d11_video_av1.h"
#include "d3d11_video_h264.h"
#include "d3d11_video_hevc.h"

#include "../dxvk/dxvk_video_decoder.h"

namespace dxvk {

  namespace {

    struct DecoderProfile {
      GUID guid;
      DxvkVideoProfile codec;
      DXGI_FORMAT format;
      UINT rawBitstream;
    };

    constexpr GUID Av1Profile0 = { 0xb8be4ccb, 0xcf53, 0x46ba, { 0x8d, 0x59, 0xd6, 0xb8, 0xa6, 0xda, 0x5d, 0x2a } };
    constexpr DecoderProfile profiles[] = {
      { { 0x1b81be68, 0xa0c7, 0x11d3, { 0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5 } },
        DxvkVideoProfile::H264, DXGI_FORMAT_NV12, 2 },
      { { 0x5b11d51b, 0x2f4c, 0x4452, { 0xbc, 0xc3, 0x09, 0xf2, 0xa1, 0x16, 0x0c, 0xc0 } },
        DxvkVideoProfile::HevcMain, DXGI_FORMAT_NV12, 1 },
      { { 0x107af0e0, 0xef1a, 0x4d19, { 0xab, 0xa8, 0x67, 0xa1, 0x63, 0x07, 0x3d, 0x13 } },
        DxvkVideoProfile::HevcMain10, DXGI_FORMAT_P010, 1 },
      { Av1Profile0, DxvkVideoProfile::Av1Main, DXGI_FORMAT_NV12, 1 },
      { Av1Profile0, DxvkVideoProfile::Av1Main10, DXGI_FORMAT_P010, 1 },
    };
    constexpr GUID NoEncryption = { 0x1b81bed0, 0xa0c7, 0x11d3, { 0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5 } };

    bool isAv1(DxvkVideoProfile codec) {
      return codec == DxvkVideoProfile::Av1Main || codec == DxvkVideoProfile::Av1Main10;
    }

    // AV1 Annex A. Levels that differ only in rate have the same picture limits.
    bool av1LevelSupports(StdVideoAV1Level maxLevel, VkExtent2D size, uint32_t tileCols = 1, uint32_t tileRows = 1) {
      static constexpr struct {
        StdVideoAV1Level level;
        uint32_t samples, width, height, tiles, columns;
      } limits[] = {
        { STD_VIDEO_AV1_LEVEL_2_0,   147456,  2048, 1152,   8,  4 },
        { STD_VIDEO_AV1_LEVEL_2_1,   278784,  2816, 1584,   8,  4 },
        { STD_VIDEO_AV1_LEVEL_3_0,   665856,  4352, 2448,  16,  6 },
        { STD_VIDEO_AV1_LEVEL_3_1,  1065024,  5504, 3096,  16,  6 },
        { STD_VIDEO_AV1_LEVEL_4_0,  2359296,  6144, 3456,  32,  8 },
        { STD_VIDEO_AV1_LEVEL_5_0,  8912896,  8192, 4352,  64,  8 },
        { STD_VIDEO_AV1_LEVEL_6_0, 35651584, 16384, 8704, 128, 16 },
      };

      for (const auto& limit : limits) {
        if (limit.level <= maxLevel && size.width <= limit.width && size.height <= limit.height
         && uint64_t(size.width) * size.height <= limit.samples && tileCols <= limit.columns
         && uint64_t(tileRows) * tileCols <= limit.tiles)
          return true;
      }
      return false;
    }

    const DecoderProfile* findProfile(D3D11Device* device, const GUID& guid, DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN) {
      for (const auto& profile : profiles) {
        if (profile.guid == guid && (format == DXGI_FORMAT_UNKNOWN || format == profile.format)
         && DxvkVideoDecoder::supports(device->GetDXVKDevice().ptr(), profile.codec))
          return &profile;
      }
      return nullptr;
    }

    bool getSessionExtent(D3D11Device* device, const DecoderProfile& profile,
        const D3D11_VIDEO_DECODER_DESC& desc, VkExtent2D& extent) {
      if (desc.OutputFormat != profile.format || !desc.SampleWidth || !desc.SampleHeight
       || ((desc.SampleWidth | desc.SampleHeight) & 1u))
        return false;

      const auto& props = device->GetDXVKDevice()->properties();
      if (isAv1(profile.codec)) {
        const auto& av1 = props.av1Decode[profile.codec == DxvkVideoProfile::Av1Main10];
        const auto& caps = av1.capabilities;
        extent = { desc.SampleWidth, desc.SampleHeight };
        return extent.width >= caps.minCodedExtent.width && extent.width <= caps.maxCodedExtent.width
            && extent.height >= caps.minCodedExtent.height && extent.height <= caps.maxCodedExtent.height
            && av1LevelSupports(av1.maxLevel, extent);
      }

      const bool h264 = profile.codec == DxvkVideoProfile::H264;
      const auto& hevc = props.hevcDecode[profile.codec == DxvkVideoProfile::HevcMain10];
      const auto& caps = h264 ? props.h264Decode.capabilities : hevc.capabilities;
      if (desc.SampleWidth > caps.maxCodedExtent.width || desc.SampleHeight > caps.maxCodedExtent.height)
        return false;

      const uint32_t block = h264 ? 16u : 8u;
      extent = { align(desc.SampleWidth, block), align(desc.SampleHeight, block) };
      if (extent.width < caps.minCodedExtent.width || extent.height < caps.minCodedExtent.height
       || extent.width > caps.maxCodedExtent.width || extent.height > caps.maxCodedExtent.height)
        return false;

      if (h264)
        return h264LevelSupportsExtent(props.h264Decode.maxLevelIdc, extent);

      if (!h265LevelSupportsExtent(hevc.maxLevelIdc, extent))
        return false;

      // The HEVC minimum coding block size arrives with the picture parameters.
      // Reserve room for its padding and use the actual SPS extent at decode.
      extent.width = std::min(align(desc.SampleWidth, 64u), caps.maxCodedExtent.width);
      extent.height = std::min(align(desc.SampleHeight, 64u), caps.maxCodedExtent.height);
      return true;
    }

    const DecoderProfile* findConfig(D3D11Device* device, const D3D11_VIDEO_DECODER_DESC& desc, VkExtent2D& extent) {
      const auto* profile = findProfile(device, desc.Guid, desc.OutputFormat);
      return profile && getSessionExtent(device, *profile, desc, extent) ? profile : nullptr;
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

    using PictureParameters = std::variant<DxvkH264PictureParameters, DxvkH265PictureParameters, DxvkAv1PictureParameters>;

    // DXVA uses the same offset, size and chopping fields in both short-slice records.
    static_assert(sizeof(DXVA_Slice_HEVC_Short) == sizeof(DXVA_Slice_H264_Short));
    static_assert(offsetof(DXVA_Slice_HEVC_Short, BSNALunitDataLocation) == offsetof(DXVA_Slice_H264_Short, BSNALunitDataLocation));
    static_assert(offsetof(DXVA_Slice_HEVC_Short, SliceBytesInBuffer) == offsetof(DXVA_Slice_H264_Short, SliceBytesInBuffer));
    static_assert(offsetof(DXVA_Slice_HEVC_Short, wBadSliceChopping) == offsetof(DXVA_Slice_H264_Short, wBadSliceChopping));

    struct DecoderFrameData {
      union {
        DXVA_PicParams_H264 h264;
        DXVA_PicParams_HEVC hevc;
        DXVA_PicParams_AV1  av1;
      } picture = { };
      union {
        DXVA_Qmatrix_H264 h264;
        DXVA_Qmatrix_HEVC hevc;
      } matrix = { };
      bool                  havePicture = false;
      bool                  haveMatrix = false;
      bool                  choppedSlice = false;
      std::vector<uint8_t>  bitstream;
      std::vector<uint32_t> offsets;
      std::vector<DXVA_Tile_AV1> tiles;
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

      const GUID& profile() const {
        return m_desc.DecodeProfile;
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
      D3D11VideoDecoder(D3D11Device* device, const D3D11_VIDEO_DECODER_DESC& desc, const D3D11_VIDEO_DECODER_CONFIG& config,
        DxvkVideoProfile profile, VkExtent2D extent)
      : D3D11DeviceChild(device), m_desc(desc), m_config(config),
        m_profile(profile), m_extent(extent),
        m_backend(new DxvkVideoDecoder(device->GetDXVKDevice().ptr(), profile, extent)),
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

        if (output->profile() != m_desc.Guid || desc.Format != m_desc.OutputFormat
         || desc.Width < m_desc.SampleWidth || desc.Height < m_desc.SampleHeight)
          return E_INVALIDARG;

        m_output = output;
        return S_OK;
      }

      HRESULT acquireBuffer(DecoderBuffer type, UINT* size, void** data) {
        if (!m_output || uint32_t(type) >= m_buffers.size() || (isAv1(m_profile) && type == DecoderBuffer::Matrix))
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
        if (!m_output || uint32_t(type) >= m_buffers.size())
          return E_INVALIDARG;

        auto& storage = m_buffers[uint32_t(type)];

        if (storage.state != DecoderBufferStorage::State::Acquired)
          return E_INVALIDARG;

        storage.state = DecoderBufferStorage::State::Released;
        return S_OK;
      }

      HRESULT submitBuffers(const D3D11_VIDEO_DECODER_BUFFER_DESC* buffers, UINT count);

      HRESULT endFrame(PictureParameters& params, std::vector<uint8_t>& bytes,
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

        if (!frame.havePicture)
          return E_INVALIDARG;

        if (isAv1(m_profile)) {
          const auto& av1 = frame.picture.av1;
          const auto& caps = m_parent->GetDXVKDevice()->properties().av1Decode[m_profile == DxvkVideoProfile::Av1Main10];
          if (!av1LevelSupports(caps.maxLevel, { av1.width, av1.height }, av1.tiles.cols, av1.tiles.rows)
           || !translateAv1PictureParameters(av1, frame.tiles, m_desc, frame.bitstream,
                params.emplace<DxvkAv1PictureParameters>(), m_av1References))
            return E_INVALIDARG;
          bytes = std::move(frame.bitstream);
          return S_OK;
        }

        if (frame.choppedSlice || frame.offsets.empty())
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

        const auto& props = m_parent->GetDXVKDevice()->properties();
        if (m_profile == DxvkVideoProfile::H264) {
          const auto& pic = frame.picture.h264;
          if (!frame.haveMatrix || 16u * (pic.wFrameWidthInMbsMinus1 + 1u) != m_extent.width
           || 16u * (pic.wFrameHeightInMbsMinus1 + 1u) != m_extent.height
           || !translateH264PictureParameters(pic, frame.matrix.h264, frame.bitstream, frame.offsets,
                 props.h264Decode.maxLevelIdc, params.emplace<DxvkH264PictureParameters>()))
            return E_INVALIDARG;
        } else {
          const bool main10 = m_profile == DxvkVideoProfile::HevcMain10;
          const auto& caps = props.hevcDecode[main10];
          auto& hevc = params.emplace<DxvkH265PictureParameters>();
          if (!translateH265PictureParameters(frame.picture.hevc, frame.haveMatrix ? &frame.matrix.hevc : nullptr,
                frame.bitstream, frame.offsets, main10 ? STD_VIDEO_H265_PROFILE_IDC_MAIN_10 : STD_VIDEO_H265_PROFILE_IDC_MAIN,
                caps.maxLevelIdc, hevc, m_hevcMaxTemporalId))
            return E_INVALIDARG;

          if (hevc.sps.pic_width_in_luma_samples < std::max(m_desc.SampleWidth, caps.capabilities.minCodedExtent.width)
           || hevc.sps.pic_height_in_luma_samples < std::max(m_desc.SampleHeight, caps.capabilities.minCodedExtent.height)
           || hevc.sps.pic_width_in_luma_samples > m_extent.width || hevc.sps.pic_height_in_luma_samples > m_extent.height)
            return E_INVALIDARG;

          m_hevcMaxTemporalId = hevc.sps.sps_max_sub_layers_minus1;
        }

        bytes = std::move(frame.bitstream);
        return S_OK;
      }

      void decode(DxvkContext* ctx, const PictureParameters& params, const std::vector<uint8_t>& bytes,
        const Rc<DxvkImage>& image, uint32_t layer) {
        if (m_decodeFailed.load(std::memory_order_relaxed))
          return;

        try {
          auto frame = std::visit([&] (const auto& picture) { return m_backend->decode(picture, bytes); }, params);
          if (frame.image) {
            VkExtent2D extent = { m_desc.SampleWidth, m_desc.SampleHeight };
            // AV1 can change picture size within a decoder. Copy complete chroma samples.
            if (const auto* av1 = std::get_if<DxvkAv1PictureParameters>(&params))
              extent = { align(av1->extent.width, 2u), align(av1->extent.height, 2u) };
            ctx->copyVideoFrame(frame, image, layer, extent);
          }
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
          case DecoderBuffer::Picture:
            if (isAv1(m_profile))
              return sizeof(DXVA_PicParams_AV1);
            return m_profile == DxvkVideoProfile::H264 ? sizeof(DXVA_PicParams_H264) : sizeof(DXVA_PicParams_HEVC);
          case DecoderBuffer::Matrix:    return m_profile == DxvkVideoProfile::H264 ? sizeof(DXVA_Qmatrix_H264) : sizeof(DXVA_Qmatrix_HEVC);
          case DecoderBuffer::Slice:
            return isAv1(m_profile) ? 64u * 64u * sizeof(DXVA_Tile_AV1) : (pixels / 256u) * sizeof(DXVA_Slice_H264_Short);
          case DecoderBuffer::Bitstream:
            return std::max<size_t>(1u << 20, m_desc.OutputFormat == DXGI_FORMAT_P010 ? 3u * pixels : pixels + pixels / 2u);
          default:                       return 0u;
        }
      }

      D3D11_VIDEO_DECODER_DESC             m_desc;
      D3D11_VIDEO_DECODER_CONFIG           m_config;
      DxvkVideoProfile                    m_profile;
      VkExtent2D                          m_extent;
      Rc<DxvkVideoDecoder>                  m_backend;
      Com<D3D11VideoDecoderOutputView>     m_output;
      DecoderFrameData                    m_frame;
      uint8_t                             m_hevcMaxTemporalId = 0;
      std::vector<D3D11Av1Reference>      m_av1References;
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

      const bool av1 = isAv1(m_profile);
      const size_t recordSize = av1 ? sizeof(DXVA_Tile_AV1) : sizeof(DXVA_Slice_H264_Short);
      if ((picture.size && (m_frame.havePicture || picture.size != getBufferSize(DecoderBuffer::Picture)))
       || (matrix.size && (av1 || m_frame.haveMatrix || matrix.size != getBufferSize(DecoderBuffer::Matrix)))
       || !!slices.size != !!bitstream.size || slices.size % recordSize)
        return E_INVALIDARG;

      bool choppedSlice = m_frame.choppedSlice;
      size_t byteCount = m_frame.bitstream.size();
      size_t sliceCount = m_frame.offsets.size();
      size_t previousEnd = 0u;

      if (av1) {
        const size_t tileCount = slices.size / recordSize;
        if (tileCount + m_frame.tiles.size() > 64u * 64u
         || bitstream.size > std::numeric_limits<uint32_t>::max() - byteCount)
          return E_INVALIDARG;

        // DXVA offsets are relative to this submitted bitstream buffer.
        // Keep the buffer intact when applications submit several tile groups.
        for (size_t i = 0; i < tileCount; i++) {
          DXVA_Tile_AV1 tile;
          std::memcpy(&tile, slices.bytes + i * recordSize, sizeof(tile));
          if (!tile.DataSize || tile.DataOffset < previousEnd || tile.DataOffset > bitstream.size
           || tile.DataSize > bitstream.size - tile.DataOffset)
            return E_INVALIDARG;
          previousEnd = size_t(tile.DataOffset) + tile.DataSize;
        }

        try {
          m_frame.tiles.reserve(m_frame.tiles.size() + tileCount);
          m_frame.bitstream.reserve(byteCount + bitstream.size);
        } catch (const std::bad_alloc&) {
          return E_OUTOFMEMORY;
        } catch (const std::length_error&) {
          return E_OUTOFMEMORY;
        }

        if (picture.size) {
          std::memcpy(&m_frame.picture.av1, picture.bytes, picture.size);
          m_frame.havePicture = true;
        }
        for (size_t i = 0; i < tileCount; i++) {
          DXVA_Tile_AV1 tile;
          std::memcpy(&tile, slices.bytes + i * recordSize, sizeof(tile));
          tile.DataOffset += uint32_t(byteCount);
          m_frame.tiles.push_back(tile);
        }
        if (bitstream.size)
          m_frame.bitstream.insert(m_frame.bitstream.end(), bitstream.bytes, bitstream.bytes + bitstream.size);
        for (UINT i = 0; i < count; i++)
          m_buffers[uint32_t(getDecoderBuffer(buffers[i].BufferType))].state = DecoderBufferStorage::State::Available;
        return S_OK;
      }

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
        if (m_profile == DxvkVideoProfile::H264)
          std::memcpy(&m_frame.picture.h264, picture.bytes, picture.size);
        else
          std::memcpy(&m_frame.picture.hevc, picture.bytes, picture.size);
        m_frame.havePicture = true;
      }

      if (matrix.size) {
        if (m_profile == DxvkVideoProfile::H264)
          std::memcpy(&m_frame.matrix.h264, matrix.bytes, matrix.size);
        else
          std::memcpy(&m_frame.matrix.hevc, matrix.bytes, matrix.size);
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

    VkExtent2D extent;
    const auto* profile = desc ? findConfig(m_device, *desc, extent) : nullptr;
    if (!profile || !config || !decoder || config->ConfigBitstreamRaw != profile->rawBitstream || config->ConfigDecoderSpecific
     || config->guidConfigBitstreamEncryption != NoEncryption
     || config->guidConfigMBcontrolEncryption != NoEncryption
     || config->guidConfigResidDiffEncryption != NoEncryption)
      return E_INVALIDARG;

    try {
      *decoder = ref(new D3D11VideoDecoder(m_device, *desc, *config, profile->codec, extent));
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
     || !belongsToDevice(resource, m_device))
      return E_INVALIDARG;

    D3D11_RESOURCE_DIMENSION dimension;
    resource->GetType(&dimension);

    if (dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
      return E_INVALIDARG;

    auto texture = GetCommonTexture(resource);
    const auto& textureDesc = *texture->Desc();

    if (!findProfile(m_device, desc->DecodeProfile, textureDesc.Format) || textureDesc.MipLevels != 1
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
    return std::count_if(std::begin(profiles), std::end(profiles), [&] (const DecoderProfile& profile) {
      return findProfile(m_device, profile.guid) == &profile;
    });
  }


  HRESULT STDMETHODCALLTYPE D3D11VideoDevice::GetVideoDecoderProfile(
          UINT  index,
          GUID* profile) {
    if (!profile)
      return E_INVALIDARG;

    for (const auto& entry : profiles) {
      if (findProfile(m_device, entry.guid) == &entry && !index--) {
        *profile = entry.guid;
        return S_OK;
      }
    }
    return E_INVALIDARG;
  }


  HRESULT STDMETHODCALLTYPE D3D11VideoDevice::CheckVideoDecoderFormat(
    const GUID*       profile,
          DXGI_FORMAT format,
          BOOL*       supported) {
    if (!supported)
      return E_INVALIDARG;

    *supported = FALSE;

    const auto* entry = profile ? findProfile(m_device, *profile) : nullptr;
    if (!entry)
      return E_INVALIDARG;

    *supported = format != DXGI_FORMAT_UNKNOWN && findProfile(m_device, *profile, format);
    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D11VideoDevice::GetVideoDecoderConfigCount(
    const D3D11_VIDEO_DECODER_DESC* desc,
          UINT*                    count) {
    if (!desc || !count)
      return E_INVALIDARG;

    VkExtent2D extent;
    *count = findConfig(m_device, *desc, extent) ? 1u : 0u;
    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D11VideoDevice::GetVideoDecoderConfig(
    const D3D11_VIDEO_DECODER_DESC*   desc,
          UINT                       index,
          D3D11_VIDEO_DECODER_CONFIG* config) {
    VkExtent2D extent;
    const auto* profile = desc ? findConfig(m_device, *desc, extent) : nullptr;
    if (!profile || !config || index)
      return E_INVALIDARG;

    *config = { };
    config->guidConfigBitstreamEncryption = NoEncryption;
    config->guidConfigMBcontrolEncryption = NoEncryption;
    config->guidConfigResidDiffEncryption = NoEncryption;
    config->ConfigBitstreamRaw = profile->rawBitstream;
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
    PictureParameters params = { };
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
