#include "dxvk_hud.h"
#include "dxvk_hud_info.h"

namespace dxvk::hud {
  
  Hud::Hud(
    const Rc<DxvkDevice>&     device,
          ID3DLowLatencyDevice* lowLatencyDevice,
    const Rc<Presenter>&      presenter)
  : m_device        (device),
    m_hasDxgiColorSpace(presenter != nullptr),
    m_renderer      (device),
    m_hudItems      (device) {
    addItem<HudVersionItem>("version", -1);
    addItem<HudDeviceInfoItem>("devinfo", -1, m_device);
    m_systemInfo = m_hudItems.addSystemInfoItems();
    m_hudItems.addCpuTelemetryItems(device->adapter());
    m_hudItems.addGpuTelemetryItems(device->adapter());
    addItem<HudFpsItem>("fps", -1);
    addItem<HudFpsLowItem>("fps_lows", -1, m_hudItems.fpsLowsWindowNs());
    addItem<HudFrameTimeItem>("frametimes", -1, device, &m_renderer);
    addItem<HudSubmissionStatsItem>("submissions", -1, device);
    addItem<HudDrawCallStatsItem>("drawcalls", -1, device);
    addItem<HudPipelineStatsItem>("pipelines", -1, device);
    addItem<HudDescriptorStatsItem>("descriptors", -1, device);
    addItem<HudMemoryStatsItem>("memory", -1, device);
    addItem<HudMemoryDetailsItem>("allocations", -1, device, &m_renderer);
    addItem<HudCsThreadItem>("cs", -1, device);
    if (!m_hudItems.isEnabled("gpu") || m_hudItems.isExplicitlyEnabled("gpuload"))
      addItem<HudGpuLoadItem>("gpuload", -1, device);
    addItem<HudCompilerActivityItem>("compiler", -1, device);
    m_hudItems.addReflexItems(lowLatencyDevice);
    m_hudItems.addPresentTelemetryItems(presenter);
  }


  Hud::~Hud() {
    
  }


  void Hud::update(VkColorSpaceKHR colorSpace) {
    auto now = dxvk::high_resolution_clock::now();

    if (m_systemInfo && now >= m_nextPresentationUpdate) {
      bool directScanout = queryWineDisplayFeedback()
        & WineDisplayFeedbackDirectScanout;

      HudPresentationColorSpace hudColorSpace = HudPresentationColorSpace::Sdr;
      if (m_hasDxgiColorSpace) {
        if (colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT)
          hudColorSpace = HudPresentationColorSpace::Hdr10;
        else if (colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT)
          hudColorSpace = HudPresentationColorSpace::ScRgb;
      }

      m_systemInfo->setPresentationStatus(hudColorSpace, directScanout);
      m_nextPresentationUpdate = now + std::chrono::seconds(1);
    }

    m_hudItems.update();
  }


  void Hud::render(
    const Rc<DxvkCommandList>&ctx,
    const Rc<DxvkImageView>&  dstView) {
    if (empty())
      return;

    auto key = m_renderer.getPipelineKey(dstView);
    const auto& options = m_hudItems.options();
    VkExtent3D surfaceSize = dstView->mipLevelExtent(0u);

    m_renderer.beginFrame(ctx, dstView, options);
    m_hudItems.render(ctx, key, options, m_renderer,
      surfaceSize.width, surfaceSize.height);
    m_renderer.flushDraws(ctx, dstView, options);
    m_renderer.endFrame(ctx);
  }


  Rc<Hud> Hud::createHud(
    const Rc<DxvkDevice>&     device,
          ID3DLowLatencyDevice* lowLatencyDevice,
    const Rc<Presenter>&      presenter) {
    return new Hud(device, lowLatencyDevice, presenter);
  }
  
}
