#pragma once

#include <array>
#include <vector>

#include "d3d11_texture.h"

#include "../dxvk/hud/dxvk_hud.h"

#include "../dxvk/dxvk_latency.h"
#include "../dxvk/dxvk_swapchain_blitter.h"

#include "../util/sync/sync_signal.h"

namespace dxvk {
  
  class D3D11Device;
  class D3D11DXGIDevice;
        class D3D11SwapChain : public ComObject<IDXGIVkSwapChain3>,
                         public IWineDXGICompositionDmabufExport {
    constexpr static uint32_t DefaultFrameLatency = 1;
  public:

    D3D11SwapChain(
            D3D11DXGIDevice*          pContainer,
            D3D11Device*              pDevice,
            IDXGIVkSurfaceFactory*    pSurfaceFactory,
      const DXGI_SWAP_CHAIN_DESC1*    pDesc,
            bool                      IsComposition);
    
    ~D3D11SwapChain();

    ULONG STDMETHODCALLTYPE AddRef();

    ULONG STDMETHODCALLTYPE Release();

    HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID                    riid,
            void**                    ppvObject);

    HRESULT STDMETHODCALLTYPE GetDesc(
            DXGI_SWAP_CHAIN_DESC1*    pDesc);

    HRESULT STDMETHODCALLTYPE GetAdapter(
            REFIID                    riid,
            void**                    ppvObject);
    
    HRESULT STDMETHODCALLTYPE GetDevice(
            REFIID                    riid,
            void**                    ppDevice);
    
    HRESULT STDMETHODCALLTYPE GetImage(
            UINT                      BufferId,
            REFIID                    riid,
            void**                    ppBuffer);

    UINT STDMETHODCALLTYPE GetImageIndex();

    UINT STDMETHODCALLTYPE GetFrameLatency();

    HANDLE STDMETHODCALLTYPE GetFrameLatencyEvent();

    HRESULT STDMETHODCALLTYPE ChangeProperties(
      const DXGI_SWAP_CHAIN_DESC1*    pDesc,
      const UINT*                     pNodeMasks,
            IUnknown* const*          ppPresentQueues);

    HRESULT STDMETHODCALLTYPE SetPresentRegion(
      const RECT*                     pRegion);

    HRESULT STDMETHODCALLTYPE SetGammaControl(
            UINT                      NumControlPoints,
      const DXGI_RGB*                 pControlPoints);

    HRESULT STDMETHODCALLTYPE SetFrameLatency(
            UINT                      MaxLatency);

    HRESULT STDMETHODCALLTYPE Present(
            UINT                      SyncInterval,
            UINT                      PresentFlags,
      const DXGI_PRESENT_PARAMETERS*  pPresentParameters);

    UINT STDMETHODCALLTYPE CheckColorSpaceSupport(
            DXGI_COLOR_SPACE_TYPE     ColorSpace);

    HRESULT STDMETHODCALLTYPE SetColorSpace(
            DXGI_COLOR_SPACE_TYPE     ColorSpace);

    HRESULT STDMETHODCALLTYPE SetHDRMetaData(
      const DXGI_VK_HDR_METADATA*     pMetaData);

    void STDMETHODCALLTYPE GetLastPresentCount(
            UINT64*                   pLastPresentCount);

    void STDMETHODCALLTYPE GetFrameStatistics(
            DXGI_VK_FRAME_STATISTICS* pFrameStatistics);

    void STDMETHODCALLTYPE SetTargetFrameRate(
            double                    FrameRate);

    HRESULT STDMETHODCALLTYPE SetHwndDmabufPresentMode(
            BOOL                      Enable);

    HRESULT STDMETHODCALLTYPE GetCompositionDmabuf(
      const wine_dxgi_dcomp_dmabuf_host_caps* pCaps,
            UINT                              ExpectedPresentCount,
            wine_dxgi_dmabuf_desc*           pDesc,
            int*                              pDmabufFd,
            int*                              pAcquireSyncFd);

    HRESULT STDMETHODCALLTYPE ReleaseCompositionDmabuf(
            UINT64                            ReleaseToken,
            UINT                              ReleaseFlags);

    HRESULT STDMETHODCALLTYPE PoisonCompositionDmabufRing(
            UINT                              CapFeedbackGen,
            UINT                              HostOrphanSeq);

  private:

    enum BindingIds : uint32_t {
      Image = 0,
      Gamma = 1,
    };

    Com<D3D11DXGIDevice, false> m_dxgiDevice;
    
    D3D11Device*              m_parent;
    Com<IDXGIVkSurfaceFactory> m_surfaceFactory;

    DXGI_SWAP_CHAIN_DESC1     m_desc;

    Rc<DxvkDevice>            m_device;
    Rc<Presenter>             m_presenter;

    Rc<DxvkSwapchainBlitter>  m_blitter;
    Rc<DxvkLatencyTracker>    m_latency;

    small_vector<Com<D3D11Texture2D, false>, 4> m_backBuffers;

    uint64_t                  m_frameId      = DXGI_MAX_SWAP_CHAIN_BUFFERS;
    uint32_t                  m_frameLatency = DefaultFrameLatency;
    uint32_t                  m_frameLatencyCap = 0;
    bool                      m_isComposition = false;
        bool                      m_hWndDmabufPresent = false;
    HANDLE                    m_frameLatencyEvent = nullptr;
    Rc<sync::CallbackFence>   m_frameLatencySignal;

    VkColorSpaceKHR           m_colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;

    double                    m_targetFrameRate = 0.0;

    dxvk::mutex               m_frameStatisticsLock;
    DXGI_VK_FRAME_STATISTICS  m_frameStatistics = { };

    Rc<hud::HudLatencyItem>   m_latencyHud;

    struct DCompExportImage {
      Rc<DxvkImage> image = nullptr;
      uint32_t imageId = 0u;
      uint64_t releaseToken = 0u;
      bool busy = false;
                        bool foreignOwned = false;
    };

    struct DCompExportRing {
      std::array<DCompExportImage, 3> images = { };
      uint32_t generation = 0u;
      uint32_t feedbackGen = 0u;
      uint32_t width = 0u;
      uint32_t height = 0u;
      uint32_t fourcc = 0u;
      uint64_t modifier = 0u;
      uint32_t nextImage = 0u;
      uint32_t consecutiveBacklog = 0u;
      bool valid = false;
    };

    dxvk::mutex               m_dcompExportLock;
    dxvk::condition_variable  m_dcompExportCond;
    DCompExportRing           m_dcompExportRing;
    uint64_t                  m_dcompExportNextReleaseToken = 0u;
    uint32_t                  m_dcompExportPendingDescFlags = 0u;
    uint32_t                  m_dcompExportHostOrphanSeq = 0u;

    Rc<DxvkImageView> GetBackBufferView();

    bool SupportsDCompExport() const;

    bool HasBusyDCompExportImagesLocked() const;

    HRESULT SelectDCompExportFormat(
      const wine_dxgi_dcomp_dmabuf_host_caps* pCaps,
            VkFormat                          SourceFormat,
            uint32_t*                         pFourcc,
            std::vector<uint64_t>*            pModifiers) const;

    HRESULT EnsureDCompExportRing(
      const wine_dxgi_dcomp_dmabuf_host_caps* pCaps,
      const Rc<DxvkImage>&                    SourceImage,
            DCompExportRing*                  pRing);

        void ResetDCompExportRingLocked();

    void PoisonDCompExportRingLocked(
            uint32_t                          HostOrphanSeq);

    void RestoreDCompExportDescFlagsLocked(
            uint32_t                          DescFlags,
            uint32_t                          HostOrphanSeq);

    HRESULT PresentImage(UINT SyncInterval);

        HRESULT PresentHwndDmabuf(UINT SyncInterval);

    HRESULT PresentComposition();

    void RotateBackBuffers(D3D11ImmediateContext* ctx);

    void CreateFrameLatencyEvent();

    void CreatePresenter();

    void CreateBackBuffers();

    void CreateBlitter();

    void DestroyFrameLatencyEvent();

    void DestroyLatencyTracker();

    void SyncFrameLatency();

    uint32_t GetActualFrameLatency();

    VkSurfaceFormatKHR GetSurfaceFormat(DXGI_FORMAT Format);

    Com<D3D11ReflexDevice> GetReflexDevice();

    std::string GetApiName() const;

  };

}