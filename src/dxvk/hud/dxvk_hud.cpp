#include "dxvk_hud.h"

namespace dxvk::hud {
  
  Hud::Hud(
    const Rc<DxvkDevice>&     device,
          ID3DLowLatencyDevice* lowLatencyDevice)
  : m_device        (device),
    m_renderer      (device),
    m_hudItems      (device) {
    addItem<HudVersionItem>("version", -1);
    addItem<HudDeviceInfoItem>("devinfo", -1, m_device);
    m_hudItems.addSystemInfoItems();
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
    addItem<HudGpuLoadItem>("gpuload", -1, device);
    addItem<HudCompilerActivityItem>("compiler", -1, device);
    m_hudItems.addReflexItems(lowLatencyDevice);
  }


  Hud::~Hud() {
    
  }


  void Hud::update() {
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
          ID3DLowLatencyDevice* lowLatencyDevice) {
    return new Hud(device, lowLatencyDevice);
  }
  
}
