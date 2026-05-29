#include "d3d11_context_imm.h"
#include "d3d11_device.h"
#include "d3d11_swapchain.h"

#include "../dxvk/dxvk_latency_builtin.h"

#include "../util/util_win32_compat.h"

#include <atomic>
#include <cstdint>
#include <cstring>

namespace dxvk {

  static uint16_t MapGammaControlPoint(float x) {
    if (x < 0.0f) x = 0.0f;
    if (x > 1.0f) x = 1.0f;
    return uint16_t(65535.0f * x);
  }

  static VkColorSpaceKHR ConvertColorSpace(DXGI_COLOR_SPACE_TYPE colorspace) {
    switch (colorspace) {
      case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:    return VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020: return VK_COLOR_SPACE_HDR10_ST2084_EXT;
      case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:    return VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
      default:
        Logger::warn(str::format("DXGI: ConvertColorSpace: Unknown colorspace ", colorspace));
        return VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    }
  }

  static VkXYColorEXT ConvertXYColor(const UINT16 (&dxgiColor)[2]) {
    return VkXYColorEXT{ float(dxgiColor[0]) / 50000.0f, float(dxgiColor[1]) / 50000.0f };
  }

  static float ConvertMaxLuminance(UINT dxgiLuminance) {
    return float(dxgiLuminance);
  }

  static float ConvertMinLuminance(UINT dxgiLuminance) {
    return float(dxgiLuminance) * 0.0001f;
  }

  static float ConvertLevel(UINT16 dxgiLevel) {
    return float(dxgiLevel);
  }

  static VkHdrMetadataEXT ConvertHDRMetadata(const DXGI_HDR_METADATA_HDR10& dxgiMetadata) {
    VkHdrMetadataEXT vkMetadata = { VK_STRUCTURE_TYPE_HDR_METADATA_EXT };
    vkMetadata.displayPrimaryRed         = ConvertXYColor(dxgiMetadata.RedPrimary);
    vkMetadata.displayPrimaryGreen       = ConvertXYColor(dxgiMetadata.GreenPrimary);
    vkMetadata.displayPrimaryBlue        = ConvertXYColor(dxgiMetadata.BluePrimary);
    vkMetadata.whitePoint                = ConvertXYColor(dxgiMetadata.WhitePoint);
    vkMetadata.maxLuminance              = ConvertMaxLuminance(dxgiMetadata.MaxMasteringLuminance);
    vkMetadata.minLuminance              = ConvertMinLuminance(dxgiMetadata.MinMasteringLuminance);
    vkMetadata.maxContentLightLevel      = ConvertLevel(dxgiMetadata.MaxContentLightLevel);
    vkMetadata.maxFrameAverageLightLevel = ConvertLevel(dxgiMetadata.MaxFrameAverageLightLevel);
    return vkMetadata;
  }

  constexpr uint32_t DCompDrmFormat(char a, char b, char c, char d) {
    return uint32_t(a) | (uint32_t(b) << 8) | (uint32_t(c) << 16) | (uint32_t(d) << 24);
  }

  constexpr size_t WineDxgiDmabufDescV1Size = offsetof(wine_dxgi_dmabuf_desc, dxgi_format);
  constexpr uint32_t DCompDrmFormatAbgr8888 = DCompDrmFormat('A', 'B', '2', '4');
  constexpr uint32_t DCompDrmFormatArgb8888 = DCompDrmFormat('A', 'R', '2', '4');
  constexpr uint32_t DCompDrmFormatXbgr8888 = DCompDrmFormat('X', 'B', '2', '4');
  constexpr uint32_t DCompDrmFormatXrgb8888 = DCompDrmFormat('X', 'R', '2', '4');
  constexpr uint32_t DCompDrmFormatAbgr2101010 = DCompDrmFormat('A', 'B', '3', '0');
  constexpr uint32_t DCompDrmFormatXbgr2101010 = DCompDrmFormat('X', 'B', '3', '0');
  constexpr uint32_t DCompDrmFormatAbgr16161616F = DCompDrmFormat('A', 'B', '4', 'H');
  constexpr uint32_t DCompDrmFormatXbgr16161616F = DCompDrmFormat('X', 'B', '4', 'H');
  constexpr uint64_t DCompDrmFormatModInvalid = 0x00ffffffffffffffull;

  static DXGI_ALPHA_MODE NormalizeDCompAlphaMode(DXGI_ALPHA_MODE alphaMode) {
    return alphaMode == DXGI_ALPHA_MODE_UNSPECIFIED
      ? DXGI_ALPHA_MODE_IGNORE
      : alphaMode;
  }

  D3D11SwapChain::D3D11SwapChain(
          D3D11DXGIDevice*        pContainer,
          D3D11Device*            pDevice,
          IDXGIVkSurfaceFactory*  pSurfaceFactory,
    const DXGI_SWAP_CHAIN_DESC1*  pDesc,
          bool                    IsComposition)
  : m_dxgiDevice(pContainer),
    m_parent(pDevice), 
    m_surfaceFactory(pSurfaceFactory),
    m_desc(*pDesc),
    m_device(pDevice->GetDXVKDevice()),
    m_frameLatencyCap(pDevice->GetOptions()->maxFrameLatency),
    m_isComposition(IsComposition) {
    CreateFrameLatencyEvent();
    CreatePresenter();
    CreateBackBuffers();
    CreateBlitter();
  }


  D3D11SwapChain::~D3D11SwapChain() {
    // Avoids hanging when in this state, see comment
    // in DxvkDevice::~DxvkDevice.
    if (this_thread::isInModuleDetachment())
      return;

    {
      std::lock_guard<dxvk::mutex> lock(m_dcompExportLock);
      ResetDCompExportRingLocked();
    }

    m_presenter->destroyResources();
    
    DestroyFrameLatencyEvent();
    DestroyLatencyTracker();
  }


  ULONG STDMETHODCALLTYPE D3D11SwapChain::AddRef() {
    return ComObject<IDXGIVkSwapChain3>::AddRef();
  }


  ULONG STDMETHODCALLTYPE D3D11SwapChain::Release() {
    return ComObject<IDXGIVkSwapChain3>::Release();
  }

  HRESULT STDMETHODCALLTYPE D3D11SwapChain::QueryInterface(
          REFIID                  riid,
          void**                  ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    InitReturnPtr(ppvObject);

    if (riid == __uuidof(IUnknown)
     || riid == __uuidof(IDXGIVkSwapChain)
     || riid == __uuidof(IDXGIVkSwapChain1)
     || riid == __uuidof(IDXGIVkSwapChain2)
     || riid == __uuidof(IDXGIVkSwapChain3)) {
      *ppvObject = ref(static_cast<IDXGIVkSwapChain3*>(this));
      return S_OK;
    }

    if (riid == __uuidof(IWineDXGICompositionDmabufExport)) {
      if (!SupportsDCompExport())
        return E_NOINTERFACE;

      *ppvObject = ref(static_cast<IWineDXGICompositionDmabufExport*>(this));
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(IDXGIVkSwapChain), riid)) {
      Logger::warn("D3D11SwapChain::QueryInterface: Unknown interface query");
      Logger::warn(str::format(riid));
    }

    return E_NOINTERFACE;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::GetDesc(
          DXGI_SWAP_CHAIN_DESC1*    pDesc) {
    *pDesc = m_desc;
    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::GetAdapter(
          REFIID                    riid,
          void**                    ppvObject) {
    return m_dxgiDevice->GetParent(riid, ppvObject);
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::GetDevice(
          REFIID                    riid,
          void**                    ppDevice) {
    return m_dxgiDevice->QueryInterface(riid, ppDevice);
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::GetImage(
          UINT                      BufferId,
          REFIID                    riid,
          void**                    ppBuffer) {
    InitReturnPtr(ppBuffer);

    if (BufferId >= m_backBuffers.size()) {
      Logger::err("D3D11: GetImage: Invalid buffer ID");
      return DXGI_ERROR_UNSUPPORTED;
    }

    if (m_isComposition && BufferId == m_backBuffers.size() - 1u)
      BufferId = 0u;

    return m_backBuffers[BufferId]->QueryInterface(riid, ppBuffer);
  }


  UINT STDMETHODCALLTYPE D3D11SwapChain::GetImageIndex() {
    return 0;
  }


  UINT STDMETHODCALLTYPE D3D11SwapChain::GetFrameLatency() {
    return m_frameLatency;
  }


  HANDLE STDMETHODCALLTYPE D3D11SwapChain::GetFrameLatencyEvent() {
    HANDLE result = nullptr;
    HANDLE processHandle = GetCurrentProcess();

    if (!DuplicateHandle(processHandle, m_frameLatencyEvent,
        processHandle, &result, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
      Logger::err("DxgiSwapChain::GetFrameLatencyWaitableObject: DuplicateHandle failed");
      return nullptr;
    }

    return result;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::ChangeProperties(
    const DXGI_SWAP_CHAIN_DESC1*    pDesc,
    const UINT*                     pNodeMasks,
          IUnknown* const*          ppPresentQueues) {
    if (m_desc.Format != pDesc->Format)
      m_presenter->setSurfaceFormat(GetSurfaceFormat(pDesc->Format));

    if (m_desc.Width != pDesc->Width || m_desc.Height != pDesc->Height)
      m_presenter->setSurfaceExtent({ pDesc->Width, pDesc->Height });

    m_desc = *pDesc;

    {
      std::lock_guard<dxvk::mutex> lock(m_dcompExportLock);
      ResetDCompExportRingLocked();
    }

    CreateBackBuffers();
    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::SetPresentRegion(
    const RECT*                     pRegion) {
    // TODO implement
    return E_NOTIMPL;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::SetGammaControl(
          UINT                      NumControlPoints,
    const DXGI_RGB*                 pControlPoints) {
    bool isIdentity = true;

    if (NumControlPoints > 1) {
      std::array<DxvkGammaCp, 1025> cp;

      if (NumControlPoints > cp.size())
        return E_INVALIDARG;
      
      for (uint32_t i = 0; i < NumControlPoints; i++) {
        uint16_t identity = MapGammaControlPoint(float(i) / float(NumControlPoints - 1));

        cp[i].r = MapGammaControlPoint(pControlPoints[i].Red);
        cp[i].g = MapGammaControlPoint(pControlPoints[i].Green);
        cp[i].b = MapGammaControlPoint(pControlPoints[i].Blue);
        cp[i].a = 0;

        isIdentity &= cp[i].r == identity
                   && cp[i].g == identity
                   && cp[i].b == identity;
      }

      if (!isIdentity)
        m_blitter->setGammaRamp(NumControlPoints, cp.data());
    }

    if (isIdentity)
      m_blitter->setGammaRamp(0, nullptr);

    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::SetFrameLatency(
          UINT                      MaxLatency) {
    if (MaxLatency == 0 || MaxLatency > DXGI_MAX_SWAP_CHAIN_BUFFERS)
      return DXGI_ERROR_INVALID_CALL;

    if (m_frameLatencyEvent) {
      // Windows DXGI does not seem to handle the case where the new maximum
      // latency is less than the current value, and some games relying on
      // this behaviour will hang if we attempt to decrement the semaphore.
      // Thus, only increment the semaphore as necessary.
      if (MaxLatency > m_frameLatency)
        ReleaseSemaphore(m_frameLatencyEvent, MaxLatency - m_frameLatency, nullptr);
    }

    m_frameLatency = MaxLatency;
    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::Present(
          UINT                      SyncInterval,
          UINT                      PresentFlags,
    const DXGI_PRESENT_PARAMETERS*  pPresentParameters) {
    HRESULT hr = S_OK;

    if (m_device->getDeviceStatus() != VK_SUCCESS)
      hr = DXGI_ERROR_DEVICE_RESET;

    if (PresentFlags & DXGI_PRESENT_TEST) {
      if (hr != S_OK)
        return hr;

      if (m_isComposition)
        return S_OK;

      VkResult status = m_presenter->checkSwapChainStatus();
      return status == VK_SUCCESS ? S_OK : DXGI_STATUS_OCCLUDED;
    }

    if (hr != S_OK) {
      SyncFrameLatency();
      return hr;
    }

    try {
      hr = m_hWndDmabufPresent ? PresentHwndDmabuf(SyncInterval)
        : m_isComposition ? PresentComposition() : PresentImage(SyncInterval);
    } catch (const DxvkError& e) {
      Logger::err(e.message());
      hr = E_FAIL;
    }

    // Ensure to synchronize and release the frame latency semaphore
    // even if presentation failed with STATUS_OCCLUDED, or otherwise
    // applications using the semaphore may deadlock. This works because
    // we do not increment the frame ID in those situations.
    SyncFrameLatency();

    // Ignore latency stuff if presentation failed
    DxvkLatencyStats latencyStats = { };

    if (hr == S_OK && m_latency) {
      latencyStats = m_latency->getStatistics(m_frameId);
      m_latency->sleepAndBeginFrame(m_frameId + 1, std::abs(m_targetFrameRate));
    }

    if (m_latencyHud)
      m_latencyHud->accumulateStats(latencyStats);

    return hr;
  }


  UINT STDMETHODCALLTYPE D3D11SwapChain::CheckColorSpaceSupport(
          DXGI_COLOR_SPACE_TYPE     ColorSpace) {
    UINT supportFlags = 0;

    VkColorSpaceKHR vkColorSpace = ConvertColorSpace(ColorSpace);

    if (m_presenter->supportsColorSpace(vkColorSpace))
      supportFlags |= DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT;

    return supportFlags;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::SetColorSpace(
          DXGI_COLOR_SPACE_TYPE     ColorSpace) {
    VkColorSpaceKHR colorSpace = ConvertColorSpace(ColorSpace);

    if (!m_presenter->supportsColorSpace(colorSpace))
      return E_INVALIDARG;

    m_colorSpace = colorSpace;

    m_presenter->setSurfaceFormat(GetSurfaceFormat(m_desc.Format));
    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::SetHDRMetaData(
    const DXGI_VK_HDR_METADATA*     pMetaData) {
    // For some reason this call always seems to succeed on Windows
    if (pMetaData->Type == DXGI_HDR_METADATA_TYPE_HDR10)
      m_presenter->setHdrMetadata(ConvertHDRMetadata(pMetaData->HDR10));

    return S_OK;
  }


  void STDMETHODCALLTYPE D3D11SwapChain::GetLastPresentCount(
          UINT64*                   pLastPresentCount) {
    *pLastPresentCount = UINT64(m_frameId - DXGI_MAX_SWAP_CHAIN_BUFFERS);
  }


  void STDMETHODCALLTYPE D3D11SwapChain::GetFrameStatistics(
          DXGI_VK_FRAME_STATISTICS* pFrameStatistics) {
    std::lock_guard<dxvk::mutex> lock(m_frameStatisticsLock);
    *pFrameStatistics = m_frameStatistics;
  }


  void STDMETHODCALLTYPE D3D11SwapChain::SetTargetFrameRate(
          double                    FrameRate) {
    m_targetFrameRate = FrameRate;

    if (m_presenter != nullptr)
      m_presenter->setFrameRateLimit(m_targetFrameRate, GetActualFrameLatency());
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::SetHwndDmabufPresentMode(
          BOOL                      Enable) {
    bool enable = !!Enable;

    if (m_hWndDmabufPresent != enable) {
      Logger::info(str::format("D3D11SwapChain::SetHwndDmabufPresentMode: ", enable ? "enabled" : "disabled"));
      m_hWndDmabufPresent = enable;
    }

    return S_OK;
  }


  bool D3D11SwapChain::SupportsDCompExport() const {
    return m_device->features().khrExternalMemoryFd
        && m_device->features().extExternalMemoryDmaBuf
        && m_device->features().extImageDrmFormatModifier;
  }


  bool D3D11SwapChain::HasBusyDCompExportImagesLocked() const {
    for (const auto& image : m_dcompExportRing.images) {
      if (image.busy)
        return true;
    }

    return false;
  }


  HRESULT D3D11SwapChain::SelectDCompExportFormat(
    const wine_dxgi_dcomp_dmabuf_host_caps* pCaps,
          VkFormat                          SourceFormat,
          uint32_t*                         pFourcc,
          std::vector<uint64_t>*            pModifiers) const {
    uint32_t requiredFourcc = 0u;

    DXGI_ALPHA_MODE alphaMode = NormalizeDCompAlphaMode(m_desc.AlphaMode);
    bool useAlpha = false;

    switch (alphaMode) {
      case DXGI_ALPHA_MODE_PREMULTIPLIED:
        useAlpha = true;
        break;

      case DXGI_ALPHA_MODE_IGNORE:
        break;

      default:
        return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    }

    switch (SourceFormat) {
      case VK_FORMAT_R8G8B8A8_UNORM:
      case VK_FORMAT_R8G8B8A8_SRGB:
        requiredFourcc = useAlpha ? DCompDrmFormatAbgr8888 : DCompDrmFormatXbgr8888;
        break;

      case VK_FORMAT_B8G8R8A8_UNORM:
      case VK_FORMAT_B8G8R8A8_SRGB:
        requiredFourcc = useAlpha ? DCompDrmFormatArgb8888 : DCompDrmFormatXrgb8888;
        break;

      case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        requiredFourcc = useAlpha ? DCompDrmFormatAbgr2101010 : DCompDrmFormatXbgr2101010;
        break;

      case VK_FORMAT_R16G16B16A16_SFLOAT:
        requiredFourcc = useAlpha ? DCompDrmFormatAbgr16161616F : DCompDrmFormatXbgr16161616F;
        break;

      default:
        return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    }

    pModifiers->clear();

    for (uint32_t i = 0; i < pCaps->format_modifier_count; i++) {
      const auto& entry = pCaps->format_modifiers[i];

      if (entry.fourcc != requiredFourcc)
        continue;
      if (entry.modifier == DCompDrmFormatModInvalid)
        continue;

      pModifiers->push_back(entry.modifier);
    }

    if (pModifiers->empty())
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;

    *pFourcc = requiredFourcc;
    return S_OK;
  }


  HRESULT D3D11SwapChain::EnsureDCompExportRing(
    const wine_dxgi_dcomp_dmabuf_host_caps* pCaps,
    const Rc<DxvkImage>&                    SourceImage,
          DCompExportRing*                  pRing) {
    if (!m_device->features().khrExternalMemoryFd
     || !m_device->features().extExternalMemoryDmaBuf
     || !m_device->features().extImageDrmFormatModifier)
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;

    if (!pCaps || !pCaps->format_modifiers || !pCaps->format_modifier_count)
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;

    const auto& sourceInfo = SourceImage->info();
    uint32_t fourcc = 0u;
    uint32_t width = sourceInfo.extent.width;
    uint32_t height = sourceInfo.extent.height;
    std::vector<uint64_t> modifiers;
    HRESULT hr = SelectDCompExportFormat(pCaps, sourceInfo.format, &fourcc, &modifiers);

    if (FAILED(hr)) {
      Logger::warn(str::format("D3D11SwapChain::EnsureDCompExportRing: no compatible host format, hr ", hr));
      return hr;
    }

    if (pRing->valid
     && pRing->feedbackGen == pCaps->feedback_gen
     && pRing->width == width
     && pRing->height == height
     && pRing->fourcc == fourcc)
      return S_OK;

    bool hadRing = pRing->valid || pRing->generation;

    std::vector<uint64_t> exportableModifiers;
    exportableModifiers.reserve(modifiers.size());

    for (uint64_t modifier : modifiers) {
      DxvkFormatQuery formatQuery = { };
      formatQuery.format = sourceInfo.format;
      formatQuery.type = VK_IMAGE_TYPE_2D;
      formatQuery.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
      formatQuery.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
      formatQuery.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
      formatQuery.hasDrmFormatModifier = VK_TRUE;
      formatQuery.drmFormatModifier = modifier;

      auto limits = m_device->getFormatLimits(formatQuery);

      if (limits && (limits->externalFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT))
        exportableModifiers.push_back(modifier);
    }

    modifiers = std::move(exportableModifiers);

    if (modifiers.empty()) {
      Logger::warn(str::format("D3D11SwapChain::EnsureDCompExportRing: no exportable modifiers for fourcc ", fourcc));
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    }

    // Prefer a LINEAR export when the compositor offers it. Tiled/block-linear
    // modifiers need an explicit external queue-family handoff before the
    // compositor can sample them coherently, and a linear image avoids that
    // path entirely.
    bool hasLinear = false;
    for (uint64_t modifier : modifiers) {
      if (modifier == 0u /* DRM_FORMAT_MOD_LINEAR */) { hasLinear = true; break; }
    }
    if (hasLinear)
      modifiers = { 0u };

    if (HasBusyDCompExportImagesLocked()) {
      PoisonDCompExportRingLocked(m_dcompExportHostOrphanSeq);
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    }

    if (hadRing)
      m_dcompExportPendingDescFlags |= WINE_DXGI_DMABUF_DESC_RING_POISONED;

    DCompExportRing next = { };
    next.generation = pRing->generation + 1u;
    next.feedbackGen = pCaps->feedback_gen;
    next.width = width;
    next.height = height;
    next.fourcc = fourcc;
    next.valid = true;

    Logger::info(str::format("D3D11SwapChain::EnsureDCompExportRing: creating export ring fourcc=", fourcc,
      " size=", width, "x", height,
      " modifierCandidates=", modifiers.size(), " generation=", next.generation));

    for (uint32_t i = 0; i < next.images.size(); i++) {
      DxvkImageCreateInfo imageInfo;
      imageInfo.type = VK_IMAGE_TYPE_2D;
      imageInfo.format = sourceInfo.format;
      imageInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
      imageInfo.extent = { width, height, 1u };
      imageInfo.numLayers = 1u;
      imageInfo.mipLevels = 1u;
      imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
      imageInfo.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
      imageInfo.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
      imageInfo.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
      imageInfo.layout = VK_IMAGE_LAYOUT_GENERAL;
      imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      imageInfo.shared = VK_TRUE;
      imageInfo.sharing.mode = DxvkSharedHandleMode::Export;
      imageInfo.sharing.type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
      imageInfo.drmFormatModifierCount = modifiers.size();
      imageInfo.drmFormatModifiers = modifiers.data();
      imageInfo.debugName = "Wine DComp dmabuf export image";

      try {
        next.images[i].image = m_device->createImage(imageInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      } catch (const DxvkError&) {
        Logger::warn(str::format("D3D11SwapChain::EnsureDCompExportRing: failed to create export image ", i));
        return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
      }

      VkImageDrmFormatModifierPropertiesEXT modifierProps = { VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT };
      VkImage image = next.images[i].image->storage()->getImageInfo().image;

      if (m_device->vkd()->vkGetImageDrmFormatModifierPropertiesEXT(m_device->vkd()->device(), image, &modifierProps) != VK_SUCCESS)
        return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;

      if (!i)
        next.modifier = modifierProps.drmFormatModifier;
      else if (next.modifier != modifierProps.drmFormatModifier)
        return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;

      next.images[i].imageId = i + 1u;
    }

    *pRing = std::move(next);
    return S_OK;
  }


  void D3D11SwapChain::ResetDCompExportRingLocked() {
    uint32_t generation = m_dcompExportRing.generation;
    bool hadRing = m_dcompExportRing.valid || m_dcompExportRing.generation;

    if (hadRing)
      m_dcompExportPendingDescFlags |= WINE_DXGI_DMABUF_DESC_RING_POISONED;

    if (HasBusyDCompExportImagesLocked()) {
      m_dcompExportRing.valid = false;
      m_dcompExportCond.notify_all();
      return;
    }

    m_dcompExportRing = DCompExportRing();
    m_dcompExportRing.generation = generation;
    m_dcompExportCond.notify_all();
  }


  void D3D11SwapChain::PoisonDCompExportRingLocked(uint32_t HostOrphanSeq) {
    m_dcompExportHostOrphanSeq = HostOrphanSeq;
    m_dcompExportPendingDescFlags |= WINE_DXGI_DMABUF_DESC_RING_POISONED;
    m_dcompExportRing.valid = false;
    m_dcompExportCond.notify_all();
  }


  void D3D11SwapChain::RestoreDCompExportDescFlagsLocked(
          uint32_t                          DescFlags,
          uint32_t                          HostOrphanSeq) {
    if ((DescFlags & WINE_DXGI_DMABUF_DESC_RING_POISONED)
     && !(m_dcompExportPendingDescFlags & WINE_DXGI_DMABUF_DESC_RING_POISONED))
      m_dcompExportHostOrphanSeq = HostOrphanSeq;

    m_dcompExportPendingDescFlags |= DescFlags;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::GetCompositionDmabuf(
    const wine_dxgi_dcomp_dmabuf_host_caps* pCaps,
          UINT                              ExpectedPresentCount,
          wine_dxgi_dmabuf_desc*           pDesc,
          int*                              pDmabufFd,
          int*                              pAcquireSyncFd) {
    if (!pDesc || !pDmabufFd || !pAcquireSyncFd) {
      Logger::warn("D3D11SwapChain::GetCompositionDmabuf: invalid output pointer");
      return E_INVALIDARG;
    }

    *pDmabufFd = -1;
    *pAcquireSyncFd = -1;
    bool writeExtendedDesc = pCaps && pCaps->version >= WINE_DXGI_DCOMP_DMABUF_HOST_CAPS_VERSION_V2;
    std::memset(pDesc, 0, writeExtendedDesc ? sizeof(*pDesc) : WineDxgiDmabufDescV1Size);

    if (!SupportsDCompExport())
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;

    uint64_t presentCount64 = m_frameId - DXGI_MAX_SWAP_CHAIN_BUFFERS;

    if (ExpectedPresentCount && ExpectedPresentCount <= 8u) {
      Logger::info(str::format("D3D11SwapChain::GetCompositionDmabuf: begin expected=", ExpectedPresentCount,
        " actual=", UINT(presentCount64)));
    }

    if (ExpectedPresentCount && ExpectedPresentCount > UINT(presentCount64)) {
      Logger::warn(str::format("D3D11SwapChain::GetCompositionDmabuf: present mismatch expected=", ExpectedPresentCount,
        " actual=", UINT(presentCount64)));
      return DXGI_ERROR_WAS_STILL_DRAWING;
    }

    if (ExpectedPresentCount && ExpectedPresentCount < UINT(presentCount64))
      Logger::warn(str::format("D3D11SwapChain::GetCompositionDmabuf: exporting newer present expected=", ExpectedPresentCount,
        " actual=", UINT(presentCount64)));

    uint32_t sourceBuffer = !m_isComposition && m_backBuffers.size() > 1u
      ? m_backBuffers.size() - 1u
      : 0u;
    Rc<DxvkImage> sourceImage = GetCommonTexture(m_backBuffers[sourceBuffer].ptr())->GetImage();
    Rc<DxvkImage> exportImage = nullptr;
    uint32_t imageId = 0u;
    uint32_t ringGeneration = 0u;
    uint32_t capFeedbackGen = 0u;
    uint32_t hostOrphanSeq = 0u;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t fourcc = 0u;
    uint32_t descFlags = 0u;
    uint64_t modifier = 0u;
    uint64_t releaseToken = 0u;
    uint32_t externalQueueFamily = VK_QUEUE_FAMILY_EXTERNAL;
    bool acquireExternalOwnership = false;
    bool requiresExternalQueueTransfer = false;

    {
      std::unique_lock<dxvk::mutex> lock(m_dcompExportLock);
      HRESULT hr = EnsureDCompExportRing(pCaps, sourceImage, &m_dcompExportRing);

      if (FAILED(hr)) {
        Logger::warn(str::format("D3D11SwapChain::GetCompositionDmabuf: export ring unavailable, hr ", hr));
        return hr;
      }

      auto findFreeImage = [&] () -> DCompExportImage* {
        for (uint32_t i = 0; i < m_dcompExportRing.images.size(); i++) {
          uint32_t index = (m_dcompExportRing.nextImage + i) % m_dcompExportRing.images.size();
          auto& image = m_dcompExportRing.images[index];

          if (!image.busy) {
            m_dcompExportRing.nextImage = (index + 1u) % m_dcompExportRing.images.size();
            return &image;
          }
        }

        return nullptr;
      };

      DCompExportImage* ringImage = findFreeImage();

      if (!ringImage) {
        m_dcompExportCond.wait_for(lock, std::chrono::milliseconds(1));
        ringImage = findFreeImage();
      }

      if (!ringImage) {
        ++m_dcompExportRing.consecutiveBacklog;
        Logger::warn(str::format("D3D11SwapChain::GetCompositionDmabuf: export ring backlog, consecutive=",
          m_dcompExportRing.consecutiveBacklog));
        return DXGI_ERROR_WAS_STILL_DRAWING;
      }

      m_dcompExportRing.consecutiveBacklog = 0u;

      releaseToken = ++m_dcompExportNextReleaseToken;
      if (!releaseToken)
        releaseToken = ++m_dcompExportNextReleaseToken;
      ringImage->busy = true;
      ringImage->releaseToken = releaseToken;

      exportImage = ringImage->image;
      imageId = ringImage->imageId;
      ringGeneration = m_dcompExportRing.generation;
      capFeedbackGen = m_dcompExportRing.feedbackGen;
      hostOrphanSeq = m_dcompExportHostOrphanSeq;
      width = m_dcompExportRing.width;
      height = m_dcompExportRing.height;
      fourcc = m_dcompExportRing.fourcc;
      modifier = m_dcompExportRing.modifier;
      requiresExternalQueueTransfer = modifier != 0u;
      externalQueueFamily = m_device->features().extQueueFamilyForeign
        ? VK_QUEUE_FAMILY_FOREIGN_EXT
        : VK_QUEUE_FAMILY_EXTERNAL;
      acquireExternalOwnership = requiresExternalQueueTransfer && ringImage->foreignOwned;
      ringImage->foreignOwned = false;
      descFlags = m_dcompExportPendingDescFlags;
      m_dcompExportPendingDescFlags = 0u;
    }

    auto immediateContext = m_parent->GetContext();

    {
      auto immediateContextLock = immediateContext->LockContext();

      VkImageSubresourceLayers subresource = { };
      subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      subresource.mipLevel = 0u;
      subresource.baseArrayLayer = 0u;
      subresource.layerCount = 1u;

      VkExtent3D extent = { width, height, 1u };

      immediateContext->EmitCs([
        cDstImage = exportImage,
        cSrcImage = sourceImage,
        cSubresource = subresource,
        cExtent = extent,
        cAcquireExternalOwnership = acquireExternalOwnership,
        cRequiresExternalQueueTransfer = requiresExternalQueueTransfer,
        cExternalQueueFamily = externalQueueFamily
      ] (DxvkContext* ctx) {
        if (cAcquireExternalOwnership) {
          ctx->acquireExternalImageQueueFamily(
            cDstImage, cExternalQueueFamily, cDstImage->info().layout);
        }

        ctx->initImage(cDstImage, VK_IMAGE_LAYOUT_UNDEFINED);
        ctx->copyImage(cDstImage, cSubresource, VkOffset3D(), cSrcImage, cSubresource, VkOffset3D(), cExtent);

        if (cRequiresExternalQueueTransfer) {
          ctx->releaseExternalImageQueueFamily(
            cDstImage, cExternalQueueFamily, cDstImage->info().layout);
        }
      });

      immediateContext->ExecuteFlush(GpuFlushType::ExplicitFlush, nullptr, true);
    }

    /* ExecuteFlush only submits the copy (its Synchronize flag is a no-op
     * outside 11on12 devices). Wait for the copy into the export image to
     * retire on the GPU before exporting the dmabuf; otherwise the compositor
     * can sample a partially written image, which shows as a black/garbled
     * frame on NVIDIA where implicit dmabuf synchronization is unreliable. */
    m_device->waitForResource(*exportImage, DxvkAccess::Write);

    VkSubresourceLayout layout = { };
    /* DRM-format-modifier images require a memory-plane aspect here, not
     * COLOR: VK_IMAGE_ASPECT_COLOR_BIT yields undefined rowPitch/offset for a
     * VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT image (VUID-vkGetImageSubresourceLayout-tiling).
     * The export ring negotiates single-plane modifiers, so plane 0 is correct. */
    VkImageSubresource subresource = { VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, 0u, 0u };
    DxvkResourceMemoryInfo memoryInfo = exportImage->storage()->getMemoryInfo();
    VkImage image = exportImage->storage()->getImageInfo().image;

    m_device->vkd()->vkGetImageSubresourceLayout(m_device->vkd()->device(), image, &subresource, &layout);

    int fd = exportImage->sharedFd(VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);

    if (fd < 0) {
      Logger::warn(str::format("D3D11SwapChain::GetCompositionDmabuf: shared fd export failed token=", releaseToken));
      std::lock_guard<dxvk::mutex> lock(m_dcompExportLock);

      RestoreDCompExportDescFlagsLocked(descFlags, hostOrphanSeq);

      for (auto& image : m_dcompExportRing.images) {
        if (image.releaseToken != releaseToken)
          continue;

        image.releaseToken = 0u;
        image.busy = false;
        image.foreignOwned = false;
        ResetDCompExportRingLocked();
        break;
      }

      return E_FAIL;
    }

    pDesc->version = writeExtendedDesc
      ? WINE_DXGI_DMABUF_DESC_VERSION_V2
      : WINE_DXGI_DMABUF_DESC_VERSION_V1;
    pDesc->desc_flags = descFlags;
    pDesc->present_count = UINT(presentCount64);
    pDesc->cap_feedback_gen = capFeedbackGen;
    pDesc->host_orphan_seq = hostOrphanSeq;
    pDesc->producer_pid = GetCurrentProcessId();
    pDesc->ring_generation = ringGeneration;
    pDesc->image_id = imageId;
    pDesc->width = width;
    pDesc->height = height;
    pDesc->fourcc = fourcc;
    pDesc->stride = UINT(layout.rowPitch);
    pDesc->offset = UINT(memoryInfo.offset + layout.offset);
    pDesc->sync_fd_kind = WINE_DXGI_DMABUF_SYNC_NONE;
    pDesc->producer_unique_id = reinterpret_cast<uintptr_t>(this);
    pDesc->modifier = modifier;
    pDesc->release_token = releaseToken;
    pDesc->sync_timeline_point = 0u;
    pDesc->frame_seq = UINT(presentCount64);
    pDesc->dirty_count = 0u;

    if (writeExtendedDesc) {
      pDesc->dxgi_format = m_desc.Format;
      pDesc->alpha_mode = NormalizeDCompAlphaMode(m_desc.AlphaMode);
    }

    *pDmabufFd = fd;

    if (requiresExternalQueueTransfer) {
      std::lock_guard<dxvk::mutex> lock(m_dcompExportLock);

      for (auto& image : m_dcompExportRing.images) {
        if (image.releaseToken != releaseToken)
          continue;

        image.foreignOwned = true;
        break;
      }
    }

    if (pDesc->frame_seq <= 8u) {
      Logger::info(str::format("D3D11SwapChain::GetCompositionDmabuf: exported dmabuf token=", releaseToken,
        " present=", UINT(presentCount64), " image=", imageId, " generation=", ringGeneration,
        " size=", width, "x", height, " fourcc=", fourcc, " modifier=", modifier));
    }

    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::ReleaseCompositionDmabuf(
          UINT64                            ReleaseToken,
          UINT                              ReleaseFlags) {
    if (!ReleaseToken)
      return E_INVALIDARG;

    std::lock_guard<dxvk::mutex> lock(m_dcompExportLock);

    for (auto& image : m_dcompExportRing.images) {
      if (image.releaseToken != ReleaseToken)
        continue;

      image.releaseToken = 0u;
      image.busy = false;

      if (ReleaseFlags & (WINE_DXGI_DMABUF_RELEASE_FAILED | WINE_DXGI_DMABUF_RELEASE_ORPHANED))
        ResetDCompExportRingLocked();
      else
        m_dcompExportCond.notify_all();

      return S_OK;
    }

    return E_INVALIDARG;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::PoisonCompositionDmabufRing(
          UINT                              CapFeedbackGen,
          UINT                              HostOrphanSeq) {
    std::lock_guard<dxvk::mutex> lock(m_dcompExportLock);

    if (m_dcompExportRing.feedbackGen != CapFeedbackGen)
      return S_OK;

    PoisonDCompExportRingLocked(HostOrphanSeq);
    return S_OK;
  }

  Rc<DxvkImageView> D3D11SwapChain::GetBackBufferView() {
    Rc<DxvkImage> image = GetCommonTexture(m_backBuffers[0].ptr())->GetImage();

    DxvkImageViewKey key;
    key.viewType = VK_IMAGE_VIEW_TYPE_2D;
    key.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    key.format = image->info().format;
    key.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
    key.mipIndex = 0u;
    key.mipCount = 1u;
    key.layerIndex = 0u;
    key.layerCount = 1u;

    return image->createView(key);
  }


  HRESULT D3D11SwapChain::PresentImage(UINT SyncInterval) {
    uint64_t nextPresentCount = m_frameId - DXGI_MAX_SWAP_CHAIN_BUFFERS + 1u;

    if (nextPresentCount <= 8u) {
      Logger::info(str::format("D3D11SwapChain::PresentImage: begin present=", nextPresentCount,
        " sync=", SyncInterval));
    }

    // Flush pending rendering commands before
    auto immediateContext = m_parent->GetContext();
    auto immediateContextLock = immediateContext->LockContext();

    immediateContext->EndFrame(m_latency);
    immediateContext->ExecuteFlush(GpuFlushType::ExplicitFlush, nullptr, true);

    if (nextPresentCount <= 8u)
      Logger::info(str::format("D3D11SwapChain::PresentImage: after flush present=", nextPresentCount));

    m_presenter->setSyncInterval(SyncInterval);

    // Presentation semaphores and WSI swap chain image
    if (m_latency)
      m_latency->notifyCpuPresentBegin(m_frameId + 1u);

    PresenterSync sync;
    Rc<DxvkImage> backBuffer;

    if (nextPresentCount <= 8u)
      Logger::info(str::format("D3D11SwapChain::PresentImage: before acquire present=", nextPresentCount));

    VkResult status = m_presenter->acquireNextImage(sync, backBuffer);

    if (nextPresentCount <= 8u)
      Logger::info(str::format("D3D11SwapChain::PresentImage: after acquire present=", nextPresentCount,
        " status=", status));

    if (status != VK_SUCCESS && m_latency)
      m_latency->discardTimings();

    if (status < 0)
      return E_FAIL;

    if (status == VK_NOT_READY)
      return DXGI_STATUS_OCCLUDED;

    m_frameId += 1;

    // Present from CS thread so that we don't
    // have to synchronize with it first.
    DxvkImageViewKey viewInfo = { };
    viewInfo.viewType   = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.usage      = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    viewInfo.format     = backBuffer->info().format;
    viewInfo.aspects    = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.mipIndex   = 0u;
    viewInfo.mipCount   = 1u;
    viewInfo.layerIndex = 0u;
    viewInfo.layerCount = 1u;

    immediateContext->EmitCs([
      cDevice         = m_device,
      cBlitter        = m_blitter,
      cBackBuffer     = backBuffer->createView(viewInfo),
      cSwapImage      = GetBackBufferView(),
      cSync           = sync,
      cPresenter      = m_presenter,
      cLatency        = m_latency,
      cColorSpace     = m_colorSpace,
      cFrameId        = m_frameId
    ] (DxvkContext* ctx) {
      // Update back buffer color space as necessary
      if (cSwapImage->image()->info().colorSpace != cColorSpace) {
        DxvkImageUsageInfo usage = { };
        usage.colorSpace = cColorSpace;

        ctx->ensureImageCompatibility(cSwapImage->image(), usage);
      }

      // Blit the D3D back buffer onto the actual Vulkan
      // swap chain and render the HUD if we have one.
      auto contextObjects = ctx->beginExternalRendering();

      cBlitter->present(contextObjects,
        cBackBuffer, VkRect2D(),
        cSwapImage, VkRect2D());

      // Submit current command list and present
      ctx->synchronizeWsi(cSync);
      ctx->flushCommandList(nullptr, nullptr);

      cDevice->presentImage(cPresenter, cLatency, cFrameId, nullptr);
    });

    if (m_backBuffers.size() > 1u)
      RotateBackBuffers(immediateContext);

    immediateContext->FlushCsChunk();

    if (nextPresentCount <= 8u)
      Logger::info(str::format("D3D11SwapChain::PresentImage: after FlushCsChunk present=", nextPresentCount,
        " frameId=", m_frameId - DXGI_MAX_SWAP_CHAIN_BUFFERS));

    if (m_latency) {
      m_latency->notifyCpuPresentEnd(m_frameId);

      if (m_latency->needsAutoMarkers()) {
        immediateContext->EmitCs([
          cLatency = m_latency,
          cFrameId = m_frameId
        ] (DxvkContext* ctx) {
          ctx->beginLatencyTracking(cLatency, cFrameId + 1u);
        });
      }
    }

    return S_OK;
  }


  HRESULT D3D11SwapChain::PresentHwndDmabuf(UINT SyncInterval) {
    uint64_t nextPresentCount = m_frameId - DXGI_MAX_SWAP_CHAIN_BUFFERS + 1u;

    if (nextPresentCount <= 8u) {
      Logger::info(str::format("D3D11SwapChain::PresentHwndDmabuf: begin present=", nextPresentCount,
        " sync=", SyncInterval));
    }

    auto immediateContext = m_parent->GetContext();
    auto immediateContextLock = immediateContext->LockContext();

    immediateContext->EndFrame(m_latency);
    immediateContext->ExecuteFlush(GpuFlushType::ExplicitFlush, nullptr, true);

    if (nextPresentCount <= 8u)
      Logger::info(str::format("D3D11SwapChain::PresentHwndDmabuf: after flush present=", nextPresentCount));

    if (m_latency)
      m_latency->notifyCpuPresentBegin(m_frameId + 1u);

    m_frameId += 1;

    if (m_backBuffers.size() > 1u)
      RotateBackBuffers(immediateContext);

    immediateContext->FlushCsChunk();

    if (nextPresentCount <= 8u)
      Logger::info(str::format("D3D11SwapChain::PresentHwndDmabuf: after FlushCsChunk present=", nextPresentCount,
        " frameId=", m_frameId - DXGI_MAX_SWAP_CHAIN_BUFFERS));

    if (m_latency) {
      m_latency->notifyCpuPresentEnd(m_frameId);

      if (m_latency->needsAutoMarkers()) {
        immediateContext->EmitCs([
          cLatency = m_latency,
          cFrameId = m_frameId
        ] (DxvkContext* ctx) {
          ctx->beginLatencyTracking(cLatency, cFrameId + 1u);
        });
      }
    }

    immediateContext->SynchronizeCsThread(DxvkCsThread::SynchronizeAll);
    m_frameLatencySignal->signal(m_frameId);

    if (nextPresentCount <= 8u)
      Logger::info(str::format("D3D11SwapChain::PresentHwndDmabuf: after SynchronizeCsThread present=", nextPresentCount));

    return S_OK;
  }


  HRESULT D3D11SwapChain::PresentComposition() {
    auto immediateContext = m_parent->GetContext();

    {
      auto immediateContextLock = immediateContext->LockContext();

      immediateContext->EndFrame(nullptr);
      immediateContext->ExecuteFlush(GpuFlushType::ExplicitFlush, nullptr, true);

      m_frameId += 1;
      immediateContext->FlushCsChunk();
    }

    immediateContext->SynchronizeCsThread(DxvkCsThread::SynchronizeAll);

    m_frameLatencySignal->signal(m_frameId);

    return S_OK;
  }


  void D3D11SwapChain::RotateBackBuffers(D3D11ImmediateContext* ctx) {
    small_vector<Rc<DxvkImage>, 4> images;

    for (uint32_t i = 0; i < m_backBuffers.size(); i++)
      images.push_back(GetCommonTexture(m_backBuffers[i].ptr())->GetImage());

    ctx->EmitCs([
      cImages = std::move(images)
    ] (DxvkContext* ctx) {
      auto allocation = cImages[0]->storage();

      for (size_t i = 0u; i + 1 < cImages.size(); i++) {
        ctx->invalidateImage(cImages[i], cImages[i + 1]->storage(),
          cImages[i + 1]->info().layout);
      }

      ctx->invalidateImage(cImages[cImages.size() - 1u],
        std::move(allocation), cImages[0]->info().layout);
    });
  }


  void D3D11SwapChain::CreateFrameLatencyEvent() {
    m_frameLatencySignal = new sync::CallbackFence(m_frameId);

    if (m_desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
      m_frameLatencyEvent = CreateSemaphore(nullptr, m_frameLatency, DXGI_MAX_SWAP_CHAIN_BUFFERS, nullptr);
  }


  void D3D11SwapChain::CreatePresenter() {
    PresenterDesc presenterDesc = { };
    presenterDesc.deferSurfaceCreation = m_parent->GetOptions()->deferSurfaceCreation;

    m_presenter = new Presenter(m_device, m_frameLatencySignal, presenterDesc, [
      cAdapter  = m_device->adapter(),
      cFactory  = m_surfaceFactory
    ] (VkSurfaceKHR* surface) {
      return cFactory->CreateSurface(
        cAdapter->vki()->instance(),
        cAdapter->handle(), surface);
    });

    m_presenter->setSurfaceFormat(GetSurfaceFormat(m_desc.Format));
    m_presenter->setSurfaceExtent({ m_desc.Width, m_desc.Height });
    m_presenter->setFrameRateLimit(m_targetFrameRate, GetActualFrameLatency());

    if (m_isComposition)
      return;

    m_latency = m_device->createLatencyTracker(m_presenter);

    Com<D3D11ReflexDevice> reflex = GetReflexDevice();
    reflex->RegisterLatencyTracker(m_latency);
  }


  void D3D11SwapChain::CreateBackBuffers() {
    // Explicitly destroy current swap image before
    // creating a new one to free up resources
    m_backBuffers.clear();

    bool sequential = m_isComposition ||
              m_desc.SwapEffect == DXGI_SWAP_EFFECT_SEQUENTIAL ||
              m_desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    uint32_t backBufferCount = sequential ? m_desc.BufferCount : 1u;

    // Create new back buffer
    D3D11_COMMON_TEXTURE_DESC desc;
    desc.Width              = std::max(m_desc.Width,  1u);
    desc.Height             = std::max(m_desc.Height, 1u);
    desc.Depth              = 1;
    desc.MipLevels          = 1;
    desc.ArraySize          = 1;
    desc.Format             = m_desc.Format;
    desc.SampleDesc         = m_desc.SampleDesc;
    desc.Usage              = D3D11_USAGE_DEFAULT;
    desc.BindFlags          = 0;
    desc.CPUAccessFlags     = 0;
    desc.MiscFlags          = 0;
    desc.TextureLayout      = D3D11_TEXTURE_LAYOUT_UNDEFINED;

    if (m_desc.BufferUsage & DXGI_USAGE_RENDER_TARGET_OUTPUT)
      desc.BindFlags |= D3D11_BIND_RENDER_TARGET;

    if (SupportsDCompExport())
      desc.BindFlags |= D3D11_BIND_RENDER_TARGET;

    if (m_desc.BufferUsage & DXGI_USAGE_SHADER_INPUT)
      desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;

    if (m_desc.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS)
      desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    
    if (m_desc.Flags & DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE)
      desc.MiscFlags |= D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
    
    DXGI_USAGE dxgiUsage = DXGI_USAGE_BACK_BUFFER;

    for (uint32_t i = 0; i < backBufferCount; i++) {
      if (m_desc.SwapEffect == DXGI_SWAP_EFFECT_DISCARD
       || m_desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD)
         dxgiUsage |= DXGI_USAGE_DISCARD_ON_PRESENT;

      m_backBuffers.push_back(new D3D11Texture2D(
        m_parent, static_cast<IDXGIVkSwapChain2*>(this), &desc, dxgiUsage));

      dxgiUsage |= DXGI_USAGE_READ_ONLY;
    }

    small_vector<Rc<DxvkImage>, 4> images;

    for (uint32_t i = 0; i < backBufferCount; i++)
      images.push_back(GetCommonTexture(m_backBuffers[i].ptr())->GetImage());

    // Initialize images so that we can use them. Clearing
    // to black prevents garbled output for the first frame.
    m_parent->GetContext()->InjectCs(DxvkCsQueue::HighPriority, [
      cImages = std::move(images)
    ] (DxvkContext* ctx) {
      for (size_t i = 0; i < cImages.size(); i++) {
        ctx->setDebugName(cImages[i], str::format("Back buffer ", i).c_str());
        ctx->initImage(cImages[i], VK_IMAGE_LAYOUT_UNDEFINED);
      }
    });
  }


  void D3D11SwapChain::CreateBlitter() {
    Rc<hud::Hud> hud = hud::Hud::createHud(m_device);

    if (hud) {
      hud->addItem<hud::HudClientApiItem>("api", 1, GetApiName());

      if (m_latency)
        m_latencyHud = hud->addItem<hud::HudLatencyItem>("latency", 4);
    }

    m_blitter = new DxvkSwapchainBlitter(m_device, std::move(hud));
  }


  void D3D11SwapChain::DestroyFrameLatencyEvent() {
    CloseHandle(m_frameLatencyEvent);
  }


  void D3D11SwapChain::DestroyLatencyTracker() {
    if (!m_latency)
      return;

    // Need to make sure the context stops using
    // the tracker for submissions
    m_parent->GetContext()->InjectCs(DxvkCsQueue::Ordered, [
      cLatency = m_latency
    ] (DxvkContext* ctx) {
      ctx->endLatencyTracking(cLatency);
    });

    Com<D3D11ReflexDevice> reflex = GetReflexDevice();
    reflex->UnregisterLatencyTracker(m_latency);
  }


  void D3D11SwapChain::SyncFrameLatency() {
    // Wait for the sync event so that we respect the maximum frame latency
    m_frameLatencySignal->wait(m_frameId - GetActualFrameLatency());

    m_frameLatencySignal->setCallback(m_frameId, [this,
      cFrameId           = m_frameId,
      cFrameLatencyEvent = m_frameLatencyEvent
    ] () {
      if (cFrameLatencyEvent)
        ReleaseSemaphore(cFrameLatencyEvent, 1, nullptr);

      std::lock_guard<dxvk::mutex> lock(m_frameStatisticsLock);
      m_frameStatistics.PresentCount = cFrameId - DXGI_MAX_SWAP_CHAIN_BUFFERS;
      m_frameStatistics.PresentQPCTime = dxvk::high_resolution_clock::get_counter();
    });
  }


  uint32_t D3D11SwapChain::GetActualFrameLatency() {
    // DXGI does not seem to implicitly synchronize waitable swap chains,
    // so in that case we should just respect the user config. For regular
    // swap chains, pick the latency from the DXGI device.
    uint32_t maxFrameLatency = DXGI_MAX_SWAP_CHAIN_BUFFERS;

    if (!(m_desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))
      m_dxgiDevice->GetMaximumFrameLatency(&maxFrameLatency);

    if (m_frameLatencyCap)
      maxFrameLatency = std::min(maxFrameLatency, m_frameLatencyCap);

    maxFrameLatency = std::min(maxFrameLatency, m_desc.BufferCount);
    return maxFrameLatency;
  }


  VkSurfaceFormatKHR D3D11SwapChain::GetSurfaceFormat(DXGI_FORMAT Format) {
    switch (Format) {
      default:
        Logger::warn(str::format("D3D11SwapChain: Unexpected format: ", m_desc.Format));
        [[fallthrough]];

      case DXGI_FORMAT_R8G8B8A8_UNORM:
      case DXGI_FORMAT_B8G8R8A8_UNORM:
        return { VK_FORMAT_R8G8B8A8_UNORM, m_colorSpace };

      case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return { VK_FORMAT_R8G8B8A8_SRGB, m_colorSpace };

      case DXGI_FORMAT_R10G10B10A2_UNORM:
        return { VK_FORMAT_A2B10G10R10_UNORM_PACK32, m_colorSpace };

      case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return { VK_FORMAT_R16G16B16A16_SFLOAT, m_colorSpace };
    }
  }


  Com<D3D11ReflexDevice> D3D11SwapChain::GetReflexDevice() {
    Com<ID3DLowLatencyDevice> llDevice;
    m_parent->QueryInterface(__uuidof(ID3DLowLatencyDevice), reinterpret_cast<void**>(&llDevice));

    return static_cast<D3D11ReflexDevice*>(llDevice.ptr());
  }


  std::string D3D11SwapChain::GetApiName() const {
    Com<IDXGIDXVKDevice> device;
    m_parent->QueryInterface(__uuidof(IDXGIDXVKDevice), reinterpret_cast<void**>(&device));

    uint32_t apiVersion = device->GetAPIVersion();
    uint32_t featureLevel = m_parent->GetFeatureLevel();

    uint32_t flHi = (featureLevel >> 12);
    uint32_t flLo = (featureLevel >> 8) & 0x7;

    bool is11On12 = m_parent->Is11on12Device();

    return str::format("D3D", apiVersion, (is11On12 ? "On12" : ""), " FL", flHi, "_", flLo);
  }

}
