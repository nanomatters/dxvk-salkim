#include "dxgi_factory.h"
#include "dxgi_output.h"
#include "dxgi_swapchain.h"

#include "../util/util_misc.h"

#include <atomic>
#include <d3d12.h>

namespace dxvk {

  namespace {

    constexpr UINT WineDxgiPresentDirtyRectsVersion = 1;
    constexpr UINT WineDxgiPresentDirtyRectsMax = 16;
    constexpr UINT WineDxgiDmabufDirtyRectsMax = 7;

    const GUID WineDxgiPresentDirtyRectsGuid = {
      0x5f78c2d4, 0x4e9a, 0x4f4b,
      { 0x9d, 0x5e, 0x91, 0xd4, 0xa6, 0x0f, 0x37, 0x42 }
    };

    struct WineDxgiPresentDirtyRects {
      UINT version;
      UINT present_count;
      UINT width;
      UINT height;
      UINT dirty_count;
      RECT dirty_rects[WineDxgiPresentDirtyRectsMax];
    };

    static_assert(sizeof(WineDxgiPresentDirtyRects) == 276,
      "Wine DXGI dirty rect metadata layout must match Wine's dxgi_dcomp.h");

    WineDxgiPresentDirtyRects initWineDxgiPresentDirtyInfo(
            UINT                      width,
            UINT                      height) {
      WineDxgiPresentDirtyRects dirtyInfo = { };
      dirtyInfo.version = WineDxgiPresentDirtyRectsVersion;
      dirtyInfo.width = width;
      dirtyInfo.height = height;
      return dirtyInfo;
    }


    bool isRectEmpty(const RECT& rect) {
      return rect.left >= rect.right || rect.top >= rect.bottom;
    }


    bool clipRectToSwapChain(
            RECT*                     rect,
            UINT                      width,
            UINT                      height) {
      rect->left   = std::max<LONG>(rect->left, 0);
      rect->top    = std::max<LONG>(rect->top, 0);
      rect->right  = std::min<LONG>(rect->right,  LONG(width));
      rect->bottom = std::min<LONG>(rect->bottom, LONG(height));

      return !isRectEmpty(*rect);
    }


    void unionRect(
            RECT*                     dst,
      const RECT&                     src) {
      if (isRectEmpty(*dst)) {
        *dst = src;
        return;
      }

      dst->left   = std::min(dst->left,   src.left);
      dst->top    = std::min(dst->top,    src.top);
      dst->right  = std::max(dst->right,  src.right);
      dst->bottom = std::max(dst->bottom, src.bottom);
    }


    bool getWineDxgiPresentDirtyRects(
      const DXGI_PRESENT_PARAMETERS*  pPresentParameters,
            UINT                      width,
            UINT                      height,
            WineDxgiPresentDirtyRects* dirtyInfo) {
      RECT bounds = { 0, 0, 0, 0 };
      bool overflow = false;

      *dirtyInfo = initWineDxgiPresentDirtyInfo(width, height);

      if (!pPresentParameters
       || !pPresentParameters->DirtyRectsCount
       || !pPresentParameters->pDirtyRects)
        return false;

      if (pPresentParameters->pScrollRect || pPresentParameters->pScrollOffset)
        return false;

      for (UINT i = 0; i < pPresentParameters->DirtyRectsCount; i++) {
        RECT dirtyRect = pPresentParameters->pDirtyRects[i];

        if (!clipRectToSwapChain(&dirtyRect, width, height))
          continue;

        unionRect(&bounds, dirtyRect);

        if (!overflow && dirtyInfo->dirty_count < WineDxgiPresentDirtyRectsMax)
          dirtyInfo->dirty_rects[dirtyInfo->dirty_count++] = dirtyRect;
        else
          overflow = true;
      }

      if (!dirtyInfo->dirty_count)
        return false;

      if (overflow) {
        dirtyInfo->dirty_count = 1;
        dirtyInfo->dirty_rects[0] = bounds;
      }

      return true;
    }


    bool wineDxgiDmabufDirtyRectFits(
      const RECT&                     rect) {
      return rect.left   >= 0 && rect.top    >= 0
          && rect.right  >= 0 && rect.bottom >= 0
          && rect.left   <= 0xffff && rect.top    <= 0xffff
          && rect.right  <= 0xffff && rect.bottom <= 0xffff;
    }


    void setWineDxgiDmabufDirtyRect(
            wine_dxgi_dmabuf_desc*    desc,
            UINT                      index,
      const RECT&                     rect) {
      desc->dirty_rects[index][0] = UINT16(rect.left);
      desc->dirty_rects[index][1] = UINT16(rect.top);
      desc->dirty_rects[index][2] = UINT16(rect.right);
      desc->dirty_rects[index][3] = UINT16(rect.bottom);
    }


    bool setWineDxgiDmabufDirtyRects(
            wine_dxgi_dmabuf_desc*    desc,
      const WineDxgiPresentDirtyRects& dirtyInfo) {
      if (dirtyInfo.version != WineDxgiPresentDirtyRectsVersion
       || dirtyInfo.present_count != desc->present_count
       || dirtyInfo.width != desc->width
       || dirtyInfo.height != desc->height
       || !dirtyInfo.dirty_count
       || dirtyInfo.dirty_count > WineDxgiPresentDirtyRectsMax)
        return false;

      if (dirtyInfo.dirty_count > WineDxgiDmabufDirtyRectsMax) {
        RECT bounds = dirtyInfo.dirty_rects[0];

        for (UINT i = 1; i < dirtyInfo.dirty_count; i++)
          unionRect(&bounds, dirtyInfo.dirty_rects[i]);

        if (isRectEmpty(bounds) || !wineDxgiDmabufDirtyRectFits(bounds))
          return false;

        setWineDxgiDmabufDirtyRect(desc, 0, bounds);
        desc->dirty_count = 1u;
        return true;
      }

      for (UINT i = 0; i < dirtyInfo.dirty_count; i++) {
        const RECT& rect = dirtyInfo.dirty_rects[i];

        if (isRectEmpty(rect) || !wineDxgiDmabufDirtyRectFits(rect))
          return false;
      }

      for (UINT i = 0; i < dirtyInfo.dirty_count; i++)
        setWineDxgiDmabufDirtyRect(desc, i, dirtyInfo.dirty_rects[i]);

      desc->dirty_count = dirtyInfo.dirty_count;
      return true;
    }


    using PFN_NtUserGetHwndDmabufCaps = UINT (WINAPI *) (HWND, void*, void*, UINT, UINT*);
    using PFN_NtUserPublishHwndDmabuf = UINT (WINAPI *) (HWND, int, int, const void*, UINT*);

    struct WineHwndDmabufFuncs {
      PFN_NtUserGetHwndDmabufCaps getCaps = nullptr;
      PFN_NtUserPublishHwndDmabuf publish = nullptr;
      bool resolved = false;
    };

    WineHwndDmabufFuncs& getWineHwndDmabufFuncs() {
      static WineHwndDmabufFuncs funcs;

      if (funcs.resolved)
        return funcs;

      funcs.resolved = true;
      HMODULE win32u = ::GetModuleHandleW(L"win32u.dll");

      if (!win32u)
        return funcs;

      funcs.getCaps = reinterpret_cast<PFN_NtUserGetHwndDmabufCaps>(
        ::GetProcAddress(win32u, "NtUserGetHwndDmabufCaps"));
      funcs.publish = reinterpret_cast<PFN_NtUserPublishHwndDmabuf>(
        ::GetProcAddress(win32u, "NtUserPublishHwndDmabuf"));
      return funcs;
    }


    bool getWineHwndDmabufCaps(
            HWND                              hwnd,
            wine_dxgi_dcomp_dmabuf_host_caps* caps,
            std::vector<wine_dxgi_dcomp_dmabuf_format_modifier>* formatModifiers) {
      WineHwndDmabufFuncs& funcs = getWineHwndDmabufFuncs();

      if (!funcs.getCaps || !funcs.publish)
        return false;

      UINT formatModifierCount = 0u;
      UINT status = funcs.getCaps(hwnd, caps, nullptr, 0u, &formatModifierCount);

      if (status != WINE_HWND_DMABUF_OK || !formatModifierCount)
        return false;

      formatModifiers->resize(formatModifierCount);
      status = funcs.getCaps(hwnd, caps, formatModifiers->data(), formatModifierCount, &formatModifierCount);

      return status == WINE_HWND_DMABUF_OK
          && caps->format_modifiers
          && caps->format_modifier_count;
    }


    wine_hwnd_dmabuf_desc convertWineHwndDmabufDesc(
      const wine_dxgi_dmabuf_desc&            desc) {
      wine_hwnd_dmabuf_desc hwndDesc = { };

      hwndDesc.version = WINE_HWND_DMABUF_DESC_VERSION_V1;
      hwndDesc.flags = desc.desc_flags;
      hwndDesc.width = desc.width;
      hwndDesc.height = desc.height;
      hwndDesc.fourcc = desc.fourcc;
      hwndDesc.stride = desc.stride;
      hwndDesc.offset = desc.offset;
      hwndDesc.frame_seq = desc.frame_seq;
      hwndDesc.ring_generation = desc.ring_generation;
      hwndDesc.image_id = desc.image_id;
      hwndDesc.sync_fd_kind = desc.sync_fd_kind;
      hwndDesc.dirty_count = desc.dirty_count;
      std::memcpy(hwndDesc.dirty_rects, desc.dirty_rects, sizeof(hwndDesc.dirty_rects));
      hwndDesc.modifier = desc.modifier;
      hwndDesc.producer_unique_id = desc.producer_unique_id;
      hwndDesc.sync_timeline_point = desc.sync_timeline_point;
      hwndDesc.release_token = desc.release_token;
      hwndDesc.dxgi_format = desc.dxgi_format;
      hwndDesc.alpha_mode = desc.alpha_mode;
      hwndDesc.color_space = desc.color_space;
      hwndDesc.hdr_metadata_type = desc.hdr_metadata_type;
      hwndDesc.hdr_metadata = desc.hdr_metadata;
      return hwndDesc;
    }

  }
  
  DxgiSwapChain::DxgiSwapChain(
          DxgiFactory*                pFactory,
          IDXGIVkSwapChain*           pPresenter,
          HWND                        hWnd,
    const DXGI_SWAP_CHAIN_DESC1*      pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*  pFullscreenDesc,
          IDXGIOutput*                pRestrictToOutput,
          IUnknown*                   pDevice)
  : m_factory   (pFactory),
    m_restrictOutput(pRestrictToOutput),
    m_window    (hWnd),
    m_desc      (*pDesc),
    m_descFs    (*pFullscreenDesc),
    m_presentId (0u),
    m_presenter (pPresenter),
    m_monitor   (nullptr),
    m_is_d3d12(SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&Com<ID3D12CommandQueue>())))),
    m_destructionNotifier(static_cast<IDXGISwapChain4*>(this)) {

    if (m_window) {
      m_monitor = wsi::getWindowMonitor(m_window);
    } else if (m_restrictOutput != nullptr) {
      DXGI_OUTPUT_DESC outputDesc;

      if (SUCCEEDED(m_restrictOutput->GetDesc(&outputDesc)))
        m_monitor = outputDesc.Monitor;
    }

    if (FAILED(m_presenter->GetAdapter(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(&m_adapter))))
      throw DxvkError("DXGI: Failed to get adapter for present device");

    // Query updated interface versions from presenter, this
    // may fail e.g. with older vkd3d-proton builds.
    m_presenter->QueryInterface(__uuidof(IDXGIVkSwapChain1), reinterpret_cast<void**>(&m_presenter1));
    m_presenter->QueryInterface(__uuidof(IDXGIVkSwapChain2), reinterpret_cast<void**>(&m_presenter2));
    m_presenter->QueryInterface(__uuidof(IDXGIVkSwapChain3), reinterpret_cast<void**>(&m_presenter3));

    m_frameRateOption = m_factory->GetOptions()->maxFrameRate;

    // Query monitor info form DXVK's DXGI factory, if available
    m_factory->QueryInterface(__uuidof(IDXGIVkMonitorInfo), reinterpret_cast<void**>(&m_monitorInfo));
    
    // Apply initial window mode and fullscreen state
    if (!m_descFs.Windowed && FAILED(EnterFullscreenMode(nullptr)))
      throw DxvkError("DXGI: Failed to set initial fullscreen state");

    // Ensure that RGBA16 swap chains are scRGB if supported
    UpdateColorSpace(m_desc.Format, m_colorSpace);

    // Somewhat hacky way to determine whether to forward the
    // display refresh rate in windowed mode even with a sync
    // interval of 1.
    if (!m_is_d3d12) {
      auto instance = pFactory->GetDXVKInstance();
      m_hasLatencyControl = instance->options().latencySleep == Tristate::True;
    }
  }
  
  
  DxgiSwapChain::~DxgiSwapChain() {
    if (!m_descFs.Windowed)
      RestoreDisplayMode(m_monitor);

    // Decouple swap chain from monitor if necessary
    DXGI_VK_MONITOR_DATA* monitorInfo = nullptr;
    
    if (SUCCEEDED(AcquireMonitorData(m_monitor, &monitorInfo))) {
      if (monitorInfo->pSwapChain == this)
        monitorInfo->pSwapChain = nullptr;
      
      ReleaseMonitorData();
    }
  }


  ULONG STDMETHODCALLTYPE DxgiSwapChain::AddRef() {
    return DxgiObject<IDXGISwapChain4>::AddRef();
  }


  ULONG STDMETHODCALLTYPE DxgiSwapChain::Release() {
    return DxgiObject<IDXGISwapChain4>::Release();
  }

  HRESULT STDMETHODCALLTYPE DxgiSwapChain::QueryInterface(REFIID riid, void** ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;
    
    if (riid == __uuidof(IUnknown)
     || riid == __uuidof(IDXGIObject)
     || riid == __uuidof(IDXGIDeviceSubObject)
     || riid == __uuidof(IDXGISwapChain)
     || riid == __uuidof(IDXGISwapChain1)
     || riid == __uuidof(IDXGISwapChain2)
     || riid == __uuidof(IDXGISwapChain3)
     || riid == __uuidof(IDXGISwapChain4)) {
      *ppvObject = ref(static_cast<IDXGISwapChain4*>(this));
      return S_OK;
    }

    if (riid == __uuidof(ID3DDestructionNotifier)) {
      *ppvObject = ref(&m_destructionNotifier);
      return S_OK;
    }

    if (riid == __uuidof(IWineDXGICompositionDmabufExport)) {
      Com<IWineDXGICompositionDmabufExport> presenterExport;

      if (FAILED(m_presenter->QueryInterface(riid, reinterpret_cast<void**>(&presenterExport))))
        return E_NOINTERFACE;

      *ppvObject = ref(static_cast<IWineDXGICompositionDmabufExport*>(this));
      return S_OK;
    }
    
    if (logQueryInterfaceError(__uuidof(IDXGISwapChain), riid)) {
      Logger::warn("DxgiSwapChain::QueryInterface: Unknown interface query");
      Logger::warn(str::format(riid));
    }

    return E_NOINTERFACE;
  }
  
  
  HRESULT STDMETHODCALLTYPE DxgiSwapChain::GetParent(REFIID riid, void** ppParent) {
    return m_factory->QueryInterface(riid, ppParent);
  }
  
  
  HRESULT STDMETHODCALLTYPE DxgiSwapChain::GetDevice(REFIID riid, void** ppDevice) {
    return m_presenter->GetDevice(riid, ppDevice);
  }
  
  
  HRESULT STDMETHODCALLTYPE DxgiSwapChain::GetBuffer(UINT Buffer, REFIID riid, void** ppSurface) {
    return m_presenter->GetImage(Buffer, riid, ppSurface);
  }


  UINT STDMETHODCALLTYPE DxgiSwapChain::GetCurrentBackBufferIndex() {
    return m_presenter->GetImageIndex();
  }
  
  
  HRESULT STDMETHODCALLTYPE DxgiSwapChain::GetContainingOutput(IDXGIOutput** ppOutput) {
    InitReturnPtr(ppOutput);

    if (ppOutput == nullptr)
      return E_INVALIDARG;
    
    if (!m_window || !wsi::isWindow(m_window)) {
      if (m_restrictOutput != nullptr) {
        *ppOutput = m_restrictOutput.ref();
        return S_OK;
      }

      return DXGI_ERROR_INVALID_CALL;
    }
    
    Com<IDXGIOutput1> output;

    if (m_target == nullptr) {
      HRESULT hr = GetOutputFromMonitor(wsi::getWindowMonitor(m_window), &output);

      if (FAILED(hr))
        return hr;
    } else {
      output = m_target;
    }

    *ppOutput = output.ref();
    return S_OK;
  }
  
  
  HRESULT STDMETHODCALLTYPE DxgiSwapChain::GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc) {
    if (!pDesc)
      return E_INVALIDARG;
    
    pDesc->BufferDesc.Width     = m_desc.Width;
    pDesc->BufferDesc.Height    = m_desc.Height;
    pDesc->BufferDesc.RefreshRate = m_descFs.RefreshRate;
    pDesc->BufferDesc.Format    = m_desc.Format;
    pDesc->BufferDesc.ScanlineOrdering = m_descFs.ScanlineOrdering;
    pDesc->BufferDesc.Scaling   = m_descFs.Scaling;
    pDesc->SampleDesc           = m_desc.SampleDesc;
    pDesc->BufferUsage          = m_desc.BufferUsage;
    pDesc->BufferCount          = m_desc.BufferCount;
    pDesc->OutputWindow         = m_window;
    pDesc->Windowed             = m_descFs.Windowed;
    pDesc->SwapEffect           = m_desc.SwapEffect;
    pDesc->Flags                = m_desc.Flags;
    return S_OK;
  }
  
  
  HRESULT STDMETHODCALLTYPE DxgiSwapChain::GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc) {
    if (pDesc == nullptr)
      return E_INVALIDARG;
    
    *pDesc = m_desc;
    return S_OK;
  }
  
  
  HRESULT STDMETHODCALLTYPE DxgiSwapChain::GetBackgroundColor(
          DXGI_RGBA*                pColor) {
    Logger::err("DxgiSwapChain::GetBackgroundColor: Not implemented");
    return E_NOTIMPL;
  }
  
  
  HRESULT STDMETHODCALLTYPE DxgiSwapChain::GetRotation(
          DXGI_MODE_ROTATION*       pRotation) {
    Logger::err("DxgiSwapChain::GetRotation: Not implemented");
    return E_NOTIMPL;
  }
  
  
  HRESULT STDMETHODCALLTYPE DxgiSwapChain::GetRestrictToOutput(
          IDXGIOutput**             ppRestrictToOutput) {
    InitReturnPtr(ppRestrictToOutput);

    if (ppRestrictToOutput == nullptr)
      return E_INVALIDARG;
    
    *ppRestrictToOutput = m_restrictOutput.ref();
    return S_OK;
  }
  
  
  HRESULT STDMETHODCALLTYPE DxgiSwapChain::GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats) {
    std::lock_guard<dxvk::recursive_mutex> lock(m_lockWindow);

    if (!pStats)
      return E_INVALIDARG;

    static bool s_errorShown = false;

    if (!std::exchange(s_errorShown, true))
      Logger::warn("DxgiSwapChain::GetFrameStatistics: Frame statistics may be inaccurate");

    // Populate frame statistics with local present count and current time
    auto t1Counter = dxvk::high_resolution_clock::get_counter();

    DXGI_VK_FRAME_STATISTICS frameStatistics = { };
    frameStatistics.PresentCount = m_presentId;
    frameStatistics.PresentQPCTime = t1Counter;

    if (m_presenter1 != nullptr)
      m_presenter1->GetFrameStatistics(&frameStatistics);

    // Fill in actual DXGI statistics, using monitor data to help compute
    // vblank counts if possible. This is not fully accurate, especially on
    // displays with variable refresh rates, but it's the best we can do.
    DXGI_VK_MONITOR_DATA* monitorData = nullptr;

    pStats->PresentCount          = frameStatistics.PresentCount;
    pStats->PresentRefreshCount   = 0;
    pStats->SyncRefreshCount      = 0;
    pStats->SyncQPCTime.QuadPart  = frameStatistics.PresentQPCTime;
    pStats->SyncGPUTime.QuadPart  = 0;

    if (SUCCEEDED(AcquireMonitorData(m_monitor, &monitorData))) {
      auto refreshPeriod = computeRefreshPeriod(
        monitorData->LastMode.RefreshRate.Numerator,
        monitorData->LastMode.RefreshRate.Denominator);

      auto t0 = dxvk::high_resolution_clock::get_time_from_counter(monitorData->FrameStats.SyncQPCTime.QuadPart);
      auto t1 = dxvk::high_resolution_clock::get_time_from_counter(t1Counter);
      auto t2 = dxvk::high_resolution_clock::get_time_from_counter(frameStatistics.PresentQPCTime);

      pStats->PresentRefreshCount = m_presenter1 != nullptr
        ? monitorData->FrameStats.SyncRefreshCount + computeRefreshCount(t0, t2, refreshPeriod)
        : monitorData->FrameStats.PresentRefreshCount;
      pStats->SyncRefreshCount = monitorData->FrameStats.SyncRefreshCount + computeRefreshCount(t0, t1, refreshPeriod);

      ReleaseMonitorData();
    }

    // Docs say that DISJOINT is returned on the first call and around
    // mode changes. Just make this swap chain state for now.
    HRESULT hr = S_OK;

    if (std::exchange(m_frameStatisticsDisjoint, false))
      hr = DXGI_ERROR_FRAME_STATISTICS_DISJOINT;

    return hr;
  }
  
  
  HRESULT STDMETHODCALLTYPE DxgiSwapChain::GetFullscreenState(
          BOOL*         pFullscreen,
          IDXGIOutput** ppTarget) {
    HRESULT hr = S_OK;

    if (m_window && !m_is_d3d12 && !m_descFs.Windowed && wsi::isOccluded(m_window))
      SetFullscreenState(FALSE, nullptr);
    if (pFullscreen != nullptr)
      *pFullscreen = !m_descFs.Windowed;
    
    if (ppTarget != nullptr)
      *ppTarget = m_target.ref();

    return hr;
  }
  
  
  HRESULT STDMETHODCALLTYPE DxgiSwapChain::GetFullscreenDesc(
          DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc) {
    if (pDesc == nullptr)
      return E_INVALIDARG;
    
    *pDesc = m_descFs;
    return S_OK;
  }
  
  
  HRESULT STDMETHODCALLTYPE DxgiSwapChain::GetHwnd(
          HWND*                     pHwnd) {
    if (pHwnd == nullptr)
      return E_INVALIDARG;
    
    *pHwnd = m_window;
    return S_OK;
  }
  
  
  HRESULT STDMETHODCALLTYPE DxgiSwapChain::GetCoreWindow(
          REFIID                    refiid,
          void**                    ppUnk) {
    InitReturnPtr(ppUnk);
    
    Logger::err("DxgiSwapChain::GetCoreWindow: Not implemented");
    return E_NOTIMPL;
  }
  
  
  HRESULT STDMETHODCALLTYPE DxgiSwapChain::GetLastPresentCount(UINT* pLastPresentCount) {
    if (pLastPresentCount == nullptr)
      return E_INVALIDARG;

    UINT64 presentId = m_presentId;

    if (m_presenter1 != nullptr)
      m_presenter1->GetLastPresentCount(&presentId);

    *pLastPresentCount = UINT(presentId);
    return S_OK;
  }
  
  
  BOOL STDMETHODCALLTYPE DxgiSwapChain::IsTemporaryMonoSupported() {
    // This seems to be related to stereo 3D display
    // modes, which we don't support at the moment
    return FALSE;
  }
  
  
  HRESULT STDMETHODCALLTYPE DxgiSwapChain::Present(UINT SyncInterval, UINT Flags) {
    return PresentBase(SyncInterval, Flags, nullptr);
  }

  HRESULT STDMETHODCALLTYPE DxgiSwapChain::Present1(
          UINT                      SyncInterval,
          UINT                      PresentFlags,
    const DXGI_PRESENT_PARAMETERS*  pPresentParameters) {

    return PresentBase(SyncInterval, PresentFlags, pPresentParameters);
  }

  HRESULT STDMETHODCALLTYPE DxgiSwapChain::PresentBase(
          UINT                      SyncInterval,
          UINT                      PresentFlags,
    const DXGI_PRESENT_PARAMETERS*  pPresentParameters) {

    if (SyncInterval > 4)
      return DXGI_ERROR_INVALID_CALL;

    if (m_window && (m_desc.SwapEffect == DXGI_SWAP_EFFECT_DISCARD || m_desc.SwapEffect == DXGI_SWAP_EFFECT_SEQUENTIAL) && wsi::isMinimized(m_window))
      return DXGI_STATUS_OCCLUDED;
    bool occluded = m_window && !m_descFs.Windowed && wsi::isOccluded(m_window) && !wsi::isMinimized(m_window);

    auto options = m_factory->GetOptions();

    if (options->syncInterval >= 0)
      SyncInterval = options->syncInterval;

    UpdateGlobalHDRState();

    if (!(PresentFlags & DXGI_PRESENT_TEST))
      UpdateTargetFrameRate(SyncInterval);

    std::lock_guard<dxvk::recursive_mutex> lockWin(m_lockWindow);
    HRESULT hr = S_OK;
    WineDxgiPresentDirtyRects presentDirtyInfo = initWineDxgiPresentDirtyInfo(
      m_desc.Width, m_desc.Height);
    UINT publishedPresentCount = 0u;
    uint32_t presentDiagId = UINT32_MAX;

    if (m_window && !m_is_d3d12) {
      static std::atomic<uint32_t> presentAttemptCounter = 0u;
      presentDiagId = presentAttemptCounter.fetch_add(1u, std::memory_order_relaxed) + 1u;

      if (presentDiagId <= 16u) {
        Logger::info(str::format("DxgiSwapChain::PresentBase: begin id=", presentDiagId,
          " sync=", SyncInterval,
          " flags=", PresentFlags,
          " nextPresentId=", m_presentId + 1,
          " occluded=", occluded));
      }
    }

    if (!m_window || wsi::isWindow(m_window)) {
      std::lock_guard<dxvk::mutex> lockBuf(m_lockBuffer);

      if (!(PresentFlags & DXGI_PRESENT_TEST))
        getWineDxgiPresentDirtyRects(pPresentParameters,
          m_desc.Width, m_desc.Height, &presentDirtyInfo);

      if (!(PresentFlags & DXGI_PRESENT_TEST) && m_presenter3) {
        wine_dxgi_dcomp_dmabuf_host_caps caps = { };
        std::vector<wine_dxgi_dcomp_dmabuf_format_modifier> formatModifiers;
        BOOL hwndDmabufPresent = m_window && !m_is_d3d12
          && getWineHwndDmabufCaps(m_window, &caps, &formatModifiers);

        m_presenter3->SetHwndDmabufPresentMode(hwndDmabufPresent);
      }

      hr = m_presenter->Present(SyncInterval, PresentFlags, nullptr);

      if (presentDiagId <= 16u) {
        Logger::info(str::format("DxgiSwapChain::PresentBase: after presenter id=", presentDiagId,
          " hr=", hr));
      }

      if (!(PresentFlags & DXGI_PRESENT_TEST) && hr == S_OK) {
        UINT presentCount = 0;

        if (SUCCEEDED(GetLastPresentCount(&presentCount))) {
          presentDirtyInfo.present_count = presentCount;
          this->SetPrivateData(WineDxgiPresentDirtyRectsGuid, sizeof(presentDirtyInfo), &presentDirtyInfo);
          publishedPresentCount = presentCount;

          if (presentDiagId <= 16u) {
            Logger::info(str::format("DxgiSwapChain::PresentBase: counted id=", presentDiagId,
              " presentCount=", presentCount));
          }
        }
      }
    }

    if (PresentFlags & DXGI_PRESENT_TEST)
      return hr == S_OK && occluded ? DXGI_STATUS_OCCLUDED : hr;

    if (hr == S_OK) {

      m_presentId += 1;

      // Update monitor frame statistics. This is not consistent with swap chain
      // frame statistics at all, but we want to ensure that all presents become
      // visible to the IDXGIOutput in case applications rely on that behaviour.
      DXGI_VK_MONITOR_DATA* monitorData = nullptr;

      if (SUCCEEDED(AcquireMonitorData(m_monitor, &monitorData))) {
        auto refreshPeriod = computeRefreshPeriod(
          monitorData->LastMode.RefreshRate.Numerator,
          monitorData->LastMode.RefreshRate.Denominator);

        auto t0 = dxvk::high_resolution_clock::get_time_from_counter(monitorData->FrameStats.SyncQPCTime.QuadPart);
        auto t1 = dxvk::high_resolution_clock::now();

        monitorData->FrameStats.PresentCount += 1;
        monitorData->FrameStats.PresentRefreshCount = monitorData->FrameStats.SyncRefreshCount + computeRefreshCount(t0, t1, refreshPeriod);
        ReleaseMonitorData();
      }
      if (occluded) {
        if (!(PresentFlags & DXGI_PRESENT_TEST))
          SetFullscreenState(FALSE, nullptr);
        hr = DXGI_STATUS_OCCLUDED;
      }
    }

    if (hr == S_OK && publishedPresentCount)
      PublishHwndDmabuf(publishedPresentCount);

    if (m_window && !m_is_d3d12) {
      static std::atomic<uint32_t> loggedPresentDiagnostics = 0u;
      uint32_t diagIndex = loggedPresentDiagnostics.fetch_add(1u, std::memory_order_relaxed);

      if (diagIndex < 16u) {
        Logger::info(str::format("DxgiSwapChain::PresentBase: hr=", hr,
          " sync=", SyncInterval,
          " flags=", PresentFlags,
          " presentId=", m_presentId,
          " publishedPresentCount=", publishedPresentCount,
          " occluded=", occluded));
      }
    }

    return hr;
  }
  
  
  HRESULT STDMETHODCALLTYPE DxgiSwapChain::ResizeBuffers(
          UINT                      BufferCount,
          UINT                      Width,
          UINT                      Height,
          DXGI_FORMAT               NewFormat,
          UINT                      SwapChainFlags) {
    return ResizeBuffers1(BufferCount, Width, Height,
      NewFormat, SwapChainFlags, nullptr, nullptr);
  }


  HRESULT STDMETHODCALLTYPE DxgiSwapChain::ResizeBuffers1(
          UINT                      BufferCount,
          UINT                      Width,
          UINT                      Height,
          DXGI_FORMAT               Format,
          UINT                      SwapChainFlags,
    const UINT*                     pCreationNodeMask,
          IUnknown* const*          ppPresentQueue) {
    if (m_window && !wsi::isWindow(m_window))
      return DXGI_ERROR_INVALID_CALL;

    constexpr UINT PreserveFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    if ((m_desc.Flags & PreserveFlags) != (SwapChainFlags & PreserveFlags))
      return DXGI_ERROR_INVALID_CALL;
    
    std::lock_guard<dxvk::mutex> lock(m_lockBuffer);
    m_desc.Width  = Width;
    m_desc.Height = Height;
    
    if (m_window) {
      wsi::getWindowSize(m_window,
        m_desc.Width  ? nullptr : &m_desc.Width,
        m_desc.Height ? nullptr : &m_desc.Height);
    }
    
    if (BufferCount != 0)
      m_desc.BufferCount = BufferCount;
    
    if (Format != DXGI_FORMAT_UNKNOWN)
      m_desc.Format = Format;
    
    HRESULT hr = m_presenter->ChangeProperties(&m_desc, pCreationNodeMask, ppPresentQueue);

    if (FAILED(hr))
      return hr;

    UpdateColorSpace(m_desc.Format, m_colorSpace);
    return hr;
  }


  HRESULT STDMETHODCALLTYPE DxgiSwapChain::ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters) {
    std::lock_guard<dxvk::recursive_mutex> lock(m_lockWindow);

    if (!pNewTargetParameters)
      return DXGI_ERROR_INVALID_CALL;
    
    if (!m_window || !wsi::isWindow(m_window))
      return DXGI_ERROR_INVALID_CALL;

    // Promote display mode
    DXGI_MODE_DESC1 newDisplayMode = { };
    newDisplayMode.Width = pNewTargetParameters->Width;
    newDisplayMode.Height = pNewTargetParameters->Height;
    newDisplayMode.RefreshRate = pNewTargetParameters->RefreshRate;
    newDisplayMode.Format = pNewTargetParameters->Format;
    newDisplayMode.ScanlineOrdering = pNewTargetParameters->ScanlineOrdering;
    newDisplayMode.Scaling = pNewTargetParameters->Scaling;

    // Update the swap chain description
    if (newDisplayMode.RefreshRate.Numerator != 0)
      m_descFs.RefreshRate = newDisplayMode.RefreshRate;
    
    m_descFs.ScanlineOrdering = newDisplayMode.ScanlineOrdering;
    m_descFs.Scaling          = newDisplayMode.Scaling;
    
    if (m_descFs.Windowed) {
      wsi::resizeWindow(
        m_window, &m_windowState,
        newDisplayMode.Width,
        newDisplayMode.Height);
    } else {
      Com<IDXGIOutput1> output;
      
      if (FAILED(GetOutputFromMonitor(m_monitor, &output))) {
        Logger::err("DXGI: ResizeTarget: Failed to query containing output");
        return E_FAIL;
      }

      RECT bounds = { };
      wsi::getDesktopCoordinates(m_monitor, &bounds);

      uint32_t width = 0u;
      uint32_t height = 0u;

      wsi::getWindowSize(m_window, &width, &height);

      // Window bounds were changed behind our back, update saved state
      if (uint32_t(bounds.right - bounds.left) != width || uint32_t(bounds.bottom - bounds.top) != height)
        wsi::saveWindowState(m_window, &m_windowState, false);

      ChangeDisplayMode(output.ptr(), &newDisplayMode);
      wsi::updateFullscreenWindow(m_monitor, m_window, false);
    }

    return S_OK;
  }
  
  
  HRESULT STDMETHODCALLTYPE DxgiSwapChain::SetFullscreenState(
          BOOL          Fullscreen,
          IDXGIOutput*  pTarget) {
    std::lock_guard<dxvk::recursive_mutex> lock(m_lockWindow);

    if (!Fullscreen && pTarget)
      return DXGI_ERROR_INVALID_CALL;

    Com<IDXGIOutput1> target;

    if (pTarget) {
      DXGI_OUTPUT_DESC desc;

      pTarget->QueryInterface(IID_PPV_ARGS(&target));
      target->GetDesc(&desc);

      if (!m_descFs.Windowed && Fullscreen && m_monitor != desc.Monitor) {
        HRESULT hr = this->LeaveFullscreenMode();
        if (FAILED(hr))
          return hr;
      }
    }

    if (m_descFs.Windowed && Fullscreen)
      return this->EnterFullscreenMode(target.ptr());
    else if (!m_descFs.Windowed && !Fullscreen)
      return this->LeaveFullscreenMode();
    
    return S_OK;
  }
  
  
  HRESULT STDMETHODCALLTYPE DxgiSwapChain::SetBackgroundColor(
    const DXGI_RGBA*                pColor) {
    Logger::err("DxgiSwapChain::SetBackgroundColor: Not implemented");
    return E_NOTIMPL;
  }
  
  
  HRESULT STDMETHODCALLTYPE DxgiSwapChain::SetRotation(
          DXGI_MODE_ROTATION        Rotation) {

    if (Rotation == DXGI_MODE_ROTATION_IDENTITY)
      return S_OK;

    Logger::err(str::format("DxgiSwapChain::SetRotation(", Rotation,"): Not implemented"));
    return E_NOTIMPL;
  }
  
  
  HANDLE STDMETHODCALLTYPE DxgiSwapChain::GetFrameLatencyWaitableObject() {
    if (!(m_desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))
      return nullptr;

    return m_presenter->GetFrameLatencyEvent();
  }


  HRESULT STDMETHODCALLTYPE DxgiSwapChain::GetMatrixTransform(
          DXGI_MATRIX_3X2_F*        pMatrix) {
    // We don't support composition swap chains
    Logger::err("DxgiSwapChain::GetMatrixTransform: Not supported");
    return DXGI_ERROR_INVALID_CALL;
  }

  
  HRESULT STDMETHODCALLTYPE DxgiSwapChain::GetMaximumFrameLatency(
          UINT*                     pMaxLatency) {
    if (!(m_desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))
      return DXGI_ERROR_INVALID_CALL;

    std::lock_guard<dxvk::recursive_mutex> lock(m_lockWindow);
    *pMaxLatency = m_presenter->GetFrameLatency();
    return S_OK;
  }

  
  HRESULT STDMETHODCALLTYPE DxgiSwapChain::GetSourceSize(
          UINT*                     pWidth,
          UINT*                     pHeight) {
    // TODO implement properly once supported
    if (pWidth)  *pWidth  = m_desc.Width;
    if (pHeight) *pHeight = m_desc.Height;
    return S_OK;
  }

  
  HRESULT STDMETHODCALLTYPE DxgiSwapChain::SetMatrixTransform(
    const DXGI_MATRIX_3X2_F*        pMatrix) {
    // We don't support composition swap chains
    Logger::err("DxgiSwapChain::SetMatrixTransform: Not supported");
    return DXGI_ERROR_INVALID_CALL;
  }

  
  HRESULT STDMETHODCALLTYPE DxgiSwapChain::SetMaximumFrameLatency(
          UINT                      MaxLatency) {
    if (!(m_desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))
      return DXGI_ERROR_INVALID_CALL;

    std::lock_guard<dxvk::recursive_mutex> lock(m_lockWindow);
    return m_presenter->SetFrameLatency(MaxLatency);
  }


  HRESULT STDMETHODCALLTYPE DxgiSwapChain::SetSourceSize(
          UINT                      Width,
          UINT                      Height) {
    if (Width  == 0 || Width  > m_desc.Width
     || Height == 0 || Height > m_desc.Height)
      return E_INVALIDARG;

    std::lock_guard<dxvk::mutex> lock(m_lockBuffer);

    RECT region = { 0, 0, LONG(Width), LONG(Height) };
    return m_presenter->SetPresentRegion(&region);
  }
  

  HRESULT STDMETHODCALLTYPE DxgiSwapChain::CheckColorSpaceSupport(
          DXGI_COLOR_SPACE_TYPE           ColorSpace,
          UINT*                           pColorSpaceSupport) {
    if (!pColorSpaceSupport)
      return E_INVALIDARG;

    std::lock_guard<dxvk::mutex> lock(m_lockBuffer);

    if (ValidateColorSpaceSupport(m_desc.Format, ColorSpace))
      *pColorSpaceSupport = DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT;
    else
      *pColorSpaceSupport = 0;

    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE DxgiSwapChain::SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace) {
    std::lock_guard<dxvk::mutex> lock(m_lockBuffer);

    if (!ValidateColorSpaceSupport(m_desc.Format, ColorSpace))
      return E_INVALIDARG;

    // Write back color space if setting it up succeeded. This way, we preserve
    // the current color space even if the swap chain temporarily switches to a
    // back buffer format which does not support it.
    HRESULT hr = UpdateColorSpace(m_desc.Format, ColorSpace);

    if (SUCCEEDED(hr))
      m_colorSpace = ColorSpace;

    return hr;
  }

  
  HRESULT STDMETHODCALLTYPE DxgiSwapChain::SetHDRMetaData(
          DXGI_HDR_METADATA_TYPE    Type,
          UINT                      Size,
          void*                     pMetaData) {
    if (Size && !pMetaData)
      return E_INVALIDARG;

    DXGI_VK_HDR_METADATA metadata = { Type };

    switch (Type) {
      case DXGI_HDR_METADATA_TYPE_NONE:
        break;

      case DXGI_HDR_METADATA_TYPE_HDR10:
        if (Size != sizeof(DXGI_HDR_METADATA_HDR10))
          return E_INVALIDARG;

        metadata.HDR10 = *static_cast<const DXGI_HDR_METADATA_HDR10*>(pMetaData);
        break;

      default:
        Logger::err(str::format("DXGI: Unsupported HDR metadata type: ", Type));
        return E_INVALIDARG;
    }

    std::lock_guard<dxvk::mutex> lock(m_lockBuffer);
    HRESULT hr = m_presenter->SetHDRMetaData(&metadata);

    if (SUCCEEDED(hr))
      m_hdrMetadata = metadata;

    return hr;
  }
  
  
  HRESULT STDMETHODCALLTYPE DxgiSwapChain::SetGammaControl(
          UINT                      NumPoints,
    const DXGI_RGB*                 pGammaCurve) {
    std::lock_guard<dxvk::mutex> lockBuf(m_lockBuffer);
    return m_presenter->SetGammaControl(NumPoints, pGammaCurve);
  }


  void DxgiSwapChain::PublishHwndDmabuf(UINT PresentCount) {
    if (!m_window || m_is_d3d12)
      return;

    if (PresentCount <= 8u)
      Logger::info(str::format("DxgiSwapChain::PublishHwndDmabuf: begin present=", PresentCount));

    WineHwndDmabufFuncs& funcs = getWineHwndDmabufFuncs();

    if (!funcs.publish)
    {
      if (PresentCount <= 8u)
        Logger::info(str::format("DxgiSwapChain::PublishHwndDmabuf: unavailable present=", PresentCount));
      return;
    }

    wine_dxgi_dcomp_dmabuf_host_caps caps = { };
    std::vector<wine_dxgi_dcomp_dmabuf_format_modifier> formatModifiers;

    if (!getWineHwndDmabufCaps(m_window, &caps, &formatModifiers))
    {
      if (PresentCount <= 8u)
        Logger::info(str::format("DxgiSwapChain::PublishHwndDmabuf: no caps present=", PresentCount));
      return;
    }

    wine_dxgi_dmabuf_desc dxgiDesc = { };
    int dmabufFd = -1;
    int acquireSyncFd = -1;
    HRESULT hr = GetCompositionDmabuf(&caps, PresentCount, &dxgiDesc, &dmabufFd, &acquireSyncFd);

    if (FAILED(hr))
    {
      if (PresentCount <= 8u)
        Logger::info(str::format("DxgiSwapChain::PublishHwndDmabuf: export skipped present=", PresentCount,
          " hr=", hr));
      return;
    }

    wine_hwnd_dmabuf_desc hwndDesc = convertWineHwndDmabufDesc(dxgiDesc);
    UINT frameSeq = 0u;
    UINT status = funcs.publish(m_window, dmabufFd, acquireSyncFd, &hwndDesc, &frameSeq);

    if (status == WINE_HWND_DMABUF_OK) {
      if (frameSeq <= 8u) {
        Logger::info(str::format("DxgiSwapChain::PublishHwndDmabuf: published frameSeq=", frameSeq,
          " present=", dxgiDesc.present_count, " releaseToken=", dxgiDesc.release_token,
          " generation=", dxgiDesc.ring_generation));
      }

      if (m_hwndDmabufPendingReleaseToken)
        ReleaseCompositionDmabuf(m_hwndDmabufPendingReleaseToken, WINE_DXGI_DMABUF_RELEASE_OK);

      m_hwndDmabufPendingReleaseToken = dxgiDesc.release_token;
      return;
    }

    Logger::warn(str::format("DxgiSwapChain::PublishHwndDmabuf: NtUserPublishHwndDmabuf failed status=", status,
      " present=", dxgiDesc.present_count, " releaseToken=", dxgiDesc.release_token,
      " generation=", dxgiDesc.ring_generation));

    ReleaseCompositionDmabuf(dxgiDesc.release_token,
      status == WINE_HWND_DMABUF_INVALID_ARGS
        ? WINE_DXGI_DMABUF_RELEASE_FAILED
        : WINE_DXGI_DMABUF_RELEASE_DROPPED);
  }


  HRESULT STDMETHODCALLTYPE DxgiSwapChain::GetCompositionDmabuf(
    const wine_dxgi_dcomp_dmabuf_host_caps* pCaps,
          UINT                              ExpectedPresentCount,
          wine_dxgi_dmabuf_desc*            pDesc,
          int*                              pDmabufFd,
          int*                              pAcquireSyncFd) {
    Com<IWineDXGICompositionDmabufExport> presenterExport;
    HRESULT hr = m_presenter->QueryInterface(__uuidof(IWineDXGICompositionDmabufExport),
      reinterpret_cast<void**>(&presenterExport));

    if (FAILED(hr))
      return hr;

    std::lock_guard<dxvk::mutex> lockBuf(m_lockBuffer);

    hr = presenterExport->GetCompositionDmabuf(pCaps,
      ExpectedPresentCount, pDesc, pDmabufFd, pAcquireSyncFd);

    if (SUCCEEDED(hr) && pDesc) {
      if (pDesc->version >= WINE_DXGI_DMABUF_DESC_VERSION_V2) {
        pDesc->dxgi_format = m_desc.Format;
        pDesc->alpha_mode = m_desc.AlphaMode == DXGI_ALPHA_MODE_UNSPECIFIED
          ? DXGI_ALPHA_MODE_IGNORE
          : m_desc.AlphaMode;
        pDesc->color_space = m_colorSpace;
        pDesc->hdr_metadata_type = m_hdrMetadata.Type;

        if (m_hdrMetadata.Type == DXGI_HDR_METADATA_TYPE_HDR10)
          pDesc->hdr_metadata = m_hdrMetadata.HDR10;
      }

      WineDxgiPresentDirtyRects dirtyInfo = { };
      UINT dirtyInfoSize = sizeof(dirtyInfo);

      if (SUCCEEDED(this->GetPrivateData(WineDxgiPresentDirtyRectsGuid, &dirtyInfoSize, &dirtyInfo)))
        setWineDxgiDmabufDirtyRects(pDesc, dirtyInfo);
    }

    return hr;
  }


  HRESULT STDMETHODCALLTYPE DxgiSwapChain::ReleaseCompositionDmabuf(
          UINT64                            ReleaseToken,
          UINT                              ReleaseFlags) {
    Com<IWineDXGICompositionDmabufExport> presenterExport;
    HRESULT hr = m_presenter->QueryInterface(__uuidof(IWineDXGICompositionDmabufExport),
      reinterpret_cast<void**>(&presenterExport));

    if (FAILED(hr))
      return hr;

    return presenterExport->ReleaseCompositionDmabuf(ReleaseToken, ReleaseFlags);
  }


  HRESULT STDMETHODCALLTYPE DxgiSwapChain::PoisonCompositionDmabufRing(
          UINT                              CapFeedbackGen,
          UINT                              HostOrphanSeq) {
    Com<IWineDXGICompositionDmabufExport> presenterExport;
    HRESULT hr = m_presenter->QueryInterface(__uuidof(IWineDXGICompositionDmabufExport),
      reinterpret_cast<void**>(&presenterExport));

    if (FAILED(hr))
      return hr;

    return presenterExport->PoisonCompositionDmabufRing(CapFeedbackGen, HostOrphanSeq);
  }

  HRESULT DxgiSwapChain::EnterFullscreenMode(IDXGIOutput1* pTarget) {
    if (m_ModeChangeInProgress) {
      Logger::warn("Nested EnterFullscreenMode");
      return DXGI_STATUS_MODE_CHANGE_IN_PROGRESS;
    }
    scoped_bool in_progress(m_ModeChangeInProgress);

    Com<IDXGIOutput1> output = pTarget;

    if (!m_window || !wsi::isWindow(m_window))
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    
    if (output == nullptr) {
      if (FAILED(GetOutputFromMonitor(wsi::getWindowMonitor(m_window), &output))) {
        Logger::err("DXGI: EnterFullscreenMode: Cannot query containing output");
        return E_FAIL;
      }
    }

    DXGI_MODE_DESC1 displayMode = { };
    displayMode.Width            = m_desc.Width;
    displayMode.Height           = m_desc.Height;
    displayMode.RefreshRate      = m_descFs.RefreshRate;
    displayMode.Format           = m_desc.Format;
    // Ignore these two, games usually use them wrong and we don't
    // support any scaling modes except UNSPECIFIED anyway.
    displayMode.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
    displayMode.Scaling          = DXGI_MODE_SCALING_UNSPECIFIED;
    
    if (FAILED(ChangeDisplayMode(output.ptr(), &displayMode))) {
      Logger::err("DXGI: EnterFullscreenMode: Failed to change display mode");
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    }
    
    // Update swap chain description
    m_descFs.Windowed = FALSE;
    
    // Move the window so that it covers the entire output
    bool modeSwitch = (m_desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH) != 0u;

    DXGI_OUTPUT_DESC desc;
    output->GetDesc(&desc);

    wsi::saveWindowState(m_window, &m_windowState, true);

    if (!wsi::enterFullscreenMode(desc.Monitor, m_window, &m_windowState, modeSwitch)) {
      Logger::err("DXGI: EnterFullscreenMode: Failed to enter fullscreen mode");
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    }
    
    m_monitor = desc.Monitor;
    m_target  = std::move(output);

    // Apply current gamma curve of the output
    DXGI_VK_MONITOR_DATA* monitorInfo = nullptr;

    if (SUCCEEDED(AcquireMonitorData(m_monitor, &monitorInfo))) {
      if (!monitorInfo->pSwapChain)
        monitorInfo->pSwapChain = this;
      
      SetGammaControl(DXGI_VK_GAMMA_CP_COUNT, monitorInfo->GammaCurve.GammaCurve);
      ReleaseMonitorData();
    }

    return S_OK;
  }
  
  
  HRESULT DxgiSwapChain::LeaveFullscreenMode() {
    if (m_ModeChangeInProgress) {
      Logger::warn("Nested LeaveFullscreenMode");
      return DXGI_STATUS_MODE_CHANGE_IN_PROGRESS;
    }
    scoped_bool in_progress(m_ModeChangeInProgress);

    if (FAILED(RestoreDisplayMode(m_monitor)))
      Logger::warn("DXGI: LeaveFullscreenMode: Failed to restore display mode");
    
    // Reset gamma control and decouple swap chain from monitor
    DXGI_VK_MONITOR_DATA* monitorInfo = nullptr;

    if (SUCCEEDED(AcquireMonitorData(m_monitor, &monitorInfo))) {
      if (monitorInfo->pSwapChain == this)
        monitorInfo->pSwapChain = nullptr;
      
      SetGammaControl(0, nullptr);
      ReleaseMonitorData();
    }

    // Restore internal state
    m_descFs.Windowed = TRUE;
    m_target  = nullptr;
    m_monitor = m_window ? wsi::getWindowMonitor(m_window) : nullptr;
    
    if (!m_window || !wsi::isWindow(m_window))
      return S_OK;
    
    if (!wsi::leaveFullscreenMode(m_window, &m_windowState)) {
      Logger::err("DXGI: LeaveFullscreenMode: Failed to exit fullscreen mode");
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    }
    wsi::restoreWindowState(m_window, &m_windowState, true);
    
    return S_OK;
  }
  
  
  HRESULT DxgiSwapChain::ChangeDisplayMode(
          IDXGIOutput1*           pOutput,
    const DXGI_MODE_DESC1*        pDisplayMode) {
    if (!pOutput)
      return DXGI_ERROR_INVALID_CALL;
    
    // Find a mode that the output supports
    DXGI_OUTPUT_DESC outputDesc;
    pOutput->GetDesc(&outputDesc);
    
    DXGI_MODE_DESC1 preferredMode = *pDisplayMode;
    DXGI_MODE_DESC1 selectedMode = { };

    if (!(m_desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH)) {
      preferredMode.Width = 0;
      preferredMode.Height = 0;
    }

    if (preferredMode.Format == DXGI_FORMAT_UNKNOWN)
      preferredMode.Format = m_desc.Format;
    
    HRESULT hr = pOutput->FindClosestMatchingMode1(
      &preferredMode, &selectedMode, nullptr);

    if (FAILED(hr)) {
      Logger::err(str::format(
        "DXGI: Failed to query closest mode:",
        "\n  Format: ", preferredMode.Format,
        "\n  Mode:   ", preferredMode.Width, "x", preferredMode.Height,
          "@", preferredMode.RefreshRate.Numerator / std::max(preferredMode.RefreshRate.Denominator, 1u)));
      return hr;
    }

    if (!selectedMode.RefreshRate.Denominator)
      selectedMode.RefreshRate.Denominator = 1;

    if (!wsi::setWindowMode(outputDesc.Monitor, m_window, &m_windowState, ConvertDisplayMode(selectedMode)))
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;

    DXGI_VK_MONITOR_DATA* monitorData = nullptr;

    if (SUCCEEDED(AcquireMonitorData(outputDesc.Monitor, &monitorData))) {
      auto refreshPeriod = computeRefreshPeriod(
        monitorData->LastMode.RefreshRate.Numerator,
        monitorData->LastMode.RefreshRate.Denominator);

      auto t1Counter = dxvk::high_resolution_clock::get_counter();

      auto t0 = dxvk::high_resolution_clock::get_time_from_counter(monitorData->FrameStats.SyncQPCTime.QuadPart);
      auto t1 = dxvk::high_resolution_clock::get_time_from_counter(t1Counter);

      monitorData->FrameStats.SyncRefreshCount += computeRefreshCount(t0, t1, refreshPeriod);
      monitorData->FrameStats.SyncQPCTime.QuadPart = t1Counter;
      monitorData->LastMode = selectedMode;
      ReleaseMonitorData();
    }

    m_frameRateRefresh = double(selectedMode.RefreshRate.Numerator)
                       / double(selectedMode.RefreshRate.Denominator);
    return S_OK;
  }
  
  
  HRESULT DxgiSwapChain::RestoreDisplayMode(HMONITOR hMonitor) {
    if (!hMonitor)
      return DXGI_ERROR_INVALID_CALL;
    
    if (!wsi::restoreDisplayMode())
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;

    m_frameRateRefresh = 0.0;
    return S_OK;
  }
  
  
  HRESULT DxgiSwapChain::GetSampleCount(UINT Count, VkSampleCountFlagBits* pCount) const {
    switch (Count) {
      case  1: *pCount = VK_SAMPLE_COUNT_1_BIT;  return S_OK;
      case  2: *pCount = VK_SAMPLE_COUNT_2_BIT;  return S_OK;
      case  4: *pCount = VK_SAMPLE_COUNT_4_BIT;  return S_OK;
      case  8: *pCount = VK_SAMPLE_COUNT_8_BIT;  return S_OK;
      case 16: *pCount = VK_SAMPLE_COUNT_16_BIT; return S_OK;
    }
    
    return E_INVALIDARG;
  }


  HRESULT DxgiSwapChain::GetOutputFromMonitor(
          HMONITOR                  Monitor,
          IDXGIOutput1**            ppOutput) {
    if (!ppOutput)
      return DXGI_ERROR_INVALID_CALL;

    Com<IDXGIOutput> output;

    for (uint32_t i = 0; SUCCEEDED(m_adapter->EnumOutputs(i, &output)); i++) {
      DXGI_OUTPUT_DESC outputDesc;
      output->GetDesc(&outputDesc);
      
      if (outputDesc.Monitor == Monitor)
        return output->QueryInterface(IID_PPV_ARGS(ppOutput));
      
      output = nullptr;
    }
    
    return DXGI_ERROR_NOT_FOUND;
  }


  HRESULT DxgiSwapChain::AcquireMonitorData(
          HMONITOR                hMonitor,
          DXGI_VK_MONITOR_DATA**  ppData) {
    if (m_monitorInfo == nullptr || !hMonitor)
      return E_NOINTERFACE;

    HRESULT hr = m_monitorInfo->AcquireMonitorData(hMonitor, ppData);

    if (FAILED(hr) && HasLiveReferences()) {
      // We may need to initialize a DXGI output to populate monitor data.
      // If acquiring monitor data has failed previously, do not try again.
      if (hMonitor == m_monitor && !m_monitorHasOutput)
        return E_NOINTERFACE;

      Com<IDXGIOutput1> output;

      if (SUCCEEDED(GetOutputFromMonitor(hMonitor, &output)))
        hr = m_monitorInfo->AcquireMonitorData(hMonitor, ppData);
    }

    if (hMonitor == m_monitor)
      m_monitorHasOutput = SUCCEEDED(hr);

    return hr;
  }

  
  void DxgiSwapChain::ReleaseMonitorData() {
    if (m_monitorInfo != nullptr)
      m_monitorInfo->ReleaseMonitorData();
  }


  void DxgiSwapChain::UpdateGlobalHDRState() {
    // Update the global HDR state if called from the legacy NVAPI
    // interfaces, etc.

    auto state = m_factory->GlobalHDRState();
    if (m_globalHDRStateSerial != state.Serial) {
      SetColorSpace1(state.ColorSpace);

      switch (state.Metadata.Type) {
        case DXGI_HDR_METADATA_TYPE_NONE:
          SetHDRMetaData(DXGI_HDR_METADATA_TYPE_NONE, 0, nullptr);
          break;
        case DXGI_HDR_METADATA_TYPE_HDR10:
          SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(state.Metadata.HDR10), reinterpret_cast<void*>(&state.Metadata.HDR10));
          break;
        default:
          Logger::err(str::format("DXGI: Unsupported HDR metadata type (global): ", state.Metadata.Type));
          break;
      }

      m_globalHDRStateSerial = state.Serial;
    }
  }


  bool DxgiSwapChain::ValidateColorSpaceSupport(
          DXGI_FORMAT             Format,
          DXGI_COLOR_SPACE_TYPE   ColorSpace) {
    // RGBA16 swap chains are treated as scRGB even on SDR displays,
    // and regular sRGB is not exposed when this format is used.
    if (Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
      return ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;

    // For everything else, we will always expose plain sRGB
    if (ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709)
      return true;

    // Only expose HDR10 color space if HDR option is enabled
    if (ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
      return m_factory->GetOptions()->enableHDR && m_presenter->CheckColorSpaceSupport(ColorSpace);

    return false;
  }


  HRESULT DxgiSwapChain::UpdateColorSpace(
          DXGI_FORMAT             Format,
          DXGI_COLOR_SPACE_TYPE   ColorSpace) {
    // Don't do anything if the explicitly sepected color space
    // is compatible with the back buffer format already
    if (!ValidateColorSpaceSupport(Format, ColorSpace)) {
      ColorSpace = Format == DXGI_FORMAT_R16G16B16A16_FLOAT
        ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
        : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    }

    // Ensure that we pick a supported color space. This is relevant for
    // mapping scRGB to sRGB on SDR setups, matching Windows behaviour.
    if (!m_presenter->CheckColorSpaceSupport(ColorSpace))
      ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;

    HRESULT hr = m_presenter->SetColorSpace(ColorSpace);

    // If this was a colorspace other than our current one,
    // punt us into that one on the DXGI output.
    if (SUCCEEDED(hr))
      m_monitorInfo->PuntColorSpace(ColorSpace);

    return hr;
  }


  void DxgiSwapChain::UpdateTargetFrameRate(
          UINT                    SyncInterval) {
    if (m_presenter2 == nullptr)
      return;

    // Engage the frame limiter with large sync intervals even in windowed
    // mode since we want to avoid double-presenting to the swap chain.
    if (SyncInterval != m_frameRateSyncInterval && m_descFs.Windowed) {
      bool engageLimiter = (SyncInterval > 1u) || (SyncInterval && m_hasLatencyControl);

      m_frameRateSyncInterval = SyncInterval;
      m_frameRateRefresh = 0.0f;

      if (engageLimiter && m_window && wsi::isWindow(m_window)) {
        wsi::WsiMode mode = { };

        if (wsi::getCurrentDisplayMode(wsi::getWindowMonitor(m_window), &mode)) {
          if (mode.refreshRate.numerator && mode.refreshRate.denominator) {
            m_frameRateRefresh = double(mode.refreshRate.numerator)
                               / double(mode.refreshRate.denominator);
          }
        }
      }
    } else if (!m_descFs.Windowed) {
      // Reset tracking when in fullscreen mode
      m_frameRateSyncInterval = 0;
    }

    // Use a negative number to indicate that the limiter should only
    // be engaged if the target frame rate is actually exceeded
    double frameRate = m_frameRateOption;

    if (frameRate != -1.0) {
      if (SyncInterval && frameRate == 0.0)
        frameRate = -m_frameRateRefresh / double(SyncInterval);

      if (m_frameRateLimit != frameRate) {
        m_frameRateLimit = frameRate;
        m_presenter2->SetTargetFrameRate(frameRate);
      }
    }
  }

}
