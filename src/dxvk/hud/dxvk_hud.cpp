#include <algorithm>
#include <cstring>

#include "dxvk_hud.h"

namespace dxvk::hud {
  
  Hud::Hud(
    const Rc<DxvkDevice>& device)
  : m_device        (device),
    m_renderer      (device),
    m_hudItems      (device) {
    // Retrieve and sanitize options
    m_options.scale = std::clamp(m_hudItems.getOption<float>("scale", 1.0f), 0.25f, 4.0f);
    m_options.opacity = std::clamp(m_hudItems.getOption<float>("opacity", 1.0f), 0.1f, 1.0f);
    m_options.horizontal = m_hudItems.hasFlag("horizontal");
    m_options.center = m_hudItems.hasFlag("center");

    addItem<HudVersionItem>("version", -1);
    addItem<HudDeviceInfoItem>("devinfo", -1, m_device);

    if (m_hudItems.isEnabled("systeminfo")) {
      addItem<HudSystemInfoItem>("systeminfo", -1, HudSystemInfoItem::All);
    } else {
      addItem<HudSystemInfoItem>("cpu", -1, HudSystemInfoItem::Cpu);
      addItem<HudSystemInfoItem>("proton", -1, HudSystemInfoItem::Proton);
      addItem<HudSystemInfoItem>("wine", -1, HudSystemInfoItem::Wine);
      addItem<HudSystemInfoItem>("winsys", -1, HudSystemInfoItem::WinSys);
    }

    uint32_t gpuInfo = 0;
    if (m_hudItems.isEnabled("gpuname") && !m_hudItems.isEnabled("devinfo"))
      gpuInfo |= HudGpuInfoItem::Name;
    if (m_hudItems.isEnabled("gputemp"))
      gpuInfo |= HudGpuInfoItem::Temperature;
    if (m_hudItems.isEnabled("gpupower"))
      gpuInfo |= HudGpuInfoItem::Power;

    if (gpuInfo) {
      const char* name = gpuInfo & HudGpuInfoItem::Name ? "gpuname"
        : gpuInfo & HudGpuInfoItem::Temperature ? "gputemp" : "gpupower";
      addItem<HudGpuInfoItem>(name, -1, m_device, gpuInfo);
    }

    addItem<HudFpsItem>("fps", -1);
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

    m_renderer.beginFrame(ctx, dstView, m_options);
    m_hudItems.render(ctx, key, m_options, m_renderer);
    m_renderer.flushDraws(ctx, dstView, m_options);
    m_renderer.endFrame(ctx);
  }


  Rc<Hud> Hud::createHud(const Rc<DxvkDevice>& device) {
    return new Hud(device);
  }
  
}
