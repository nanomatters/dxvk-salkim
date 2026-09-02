#include "dxvk_hud_item.h"
#include "dxvk_hud_info.h"

#include "../dxvk_presenter.h"

#include "../../dxgi/dxgi_interfaces.h"

#include <hud_chunk_frag_background.h>
#include <hud_chunk_frag_visualize.h>
#include <hud_chunk_vert_background.h>
#include <hud_chunk_vert_visualize.h>

#include <hud_frame_time_eval.h>

#include <hud_graph_frag.h>
#include <hud_graph_vert.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iomanip>
#include <limits>
#include <version.h>

namespace dxvk::hud {

  namespace {

    struct HudReflexMetricInfo {
      const char* option;
      const char* label;
    };

    constexpr std::array<HudReflexMetricInfo, size_t(HudReflexMetric::Count)> HudReflexMetrics = {{
      { "reflex.id",       "Reflex frame:" },
      { "reflex.interval", "Simulation interval:" },
      { "reflex.input",    "Input sample:" },
      { "reflex.sim",      "Simulation end:" },
      { "reflex.submit",   "Submit start/end:" },
      { "reflex.present",  "Present start/end:" },
      { "reflex.driver",   "Driver start/end:" },
      { "reflex.queue",    "OS queue start/end:" },
      { "reflex.gpu",      "GPU start/end:" },
      { "reflex.active",   "GPU active:" },
      { "reflex.frame",    "GPU frame:" },
      { "reflex.camera",   "Camera constructed:" },
      { "reflex.copy",     "Cross adapter copy:" },
      { "reflex.ai",       "AI frame:" },
    }};

    struct HudPresentTelemetryMetricInfo {
      const char* option;
      const char* label;
    };

    constexpr std::array<HudPresentTelemetryMetricInfo,
      size_t(HudPresentTelemetryMetric::Count)> HudPresentTelemetryMetrics = {{
      { "latency.queue",    "Queue operations:" },
      { "latency.display",  "Display queue:" },
      { "latency.present",  "Present latency:" },
      { "latency.interval", "Display interval:" },
    }};

    constexpr std::array<HudTelemetryMetricInfo,
      size_t(HudGpuTelemetryMetric::Count)> HudGpuTelemetryMetrics = {{
      { "gpu.name",     "GPU:"          },
      { "gpu.driver",   "GPU driver:"   },
      { "gpu.power",    "GPU power:"    },
      { "gpu.temp",     "GPU temp:"     },
      { "gpu.load",     "GPU load:"     },
      { "gpu.clock",    "GPU clock:"    },
      { "gpu.memclock", "Memory clock:" },
      { "gpu.vram",     "VRAM:"         },
      { "gpu.memload",  "Memory load:"  },
      { "gpu.pcie",     "PCIe:"         },
    }};

    constexpr std::array<HudTelemetryMetricInfo,
      size_t(HudCpuTelemetryMetric::Count)> HudCpuTelemetryMetrics = {{
      { "cpu.name",  "CPU:"       },
      { "cpu.power", "CPU power:" },
      { "cpu.temp",  "CPU temp:"  },
      { "cpu.load",  "CPU load:"  },
      { "cpu.clock", "CPU clock:" },
    }};

    constexpr uint32_t HudReflexLabelColor = 0xff60ff60u;
    constexpr uint32_t HudPresentTelemetryLabelColor = 0xff40ffffu;
    constexpr uint32_t HudGpuTelemetryLabelColor = 0xffffff40u;
    constexpr uint32_t HudCpuTelemetryLabelColor = 0xffff8040u;
    constexpr uint32_t HudTelemetryValueColor = 0xffffffffu;


    size_t reflexMetricIndex(HudReflexMetric metric) {
      return size_t(metric);
    }


    size_t presentTelemetryMetricIndex(HudPresentTelemetryMetric metric) {
      return size_t(metric);
    }


    std::string formatNanoseconds(uint64_t durationNs) {
      return str::format(durationNs / 1'000'000, ".",
        std::setfill('0'), std::setw(3), durationNs / 1'000 % 1'000, " ms");
    }


    std::string formatReflexMicroseconds(uint64_t durationUs) {
      return str::format(durationUs / 1000, ".",
        std::setfill('0'), std::setw(3), durationUs % 1000, " ms");
    }


    std::string formatReflexDuration(uint64_t durationUs) {
      return durationUs ? formatReflexMicroseconds(durationUs) : "--";
    }


    std::string formatReflexSpan(uint64_t startUs, uint64_t endUs) {
      return startUs && endUs >= startUs
        ? formatReflexMicroseconds(endUs - startUs)
        : "--";
    }


    std::string formatReflexOffset(uint64_t markerUs, uint64_t originUs) {
      if (!markerUs || !originUs)
        return "--";

      if (markerUs >= originUs)
        return str::format("+", formatReflexMicroseconds(markerUs - originUs));

      return str::format("-", formatReflexMicroseconds(originUs - markerUs));
    }


    std::string formatReflexRange(
            uint64_t              startUs,
            uint64_t              endUs,
            uint64_t              originUs) {
      if (!startUs || !endUs || endUs < startUs)
        return "--";

      return str::format(formatReflexOffset(startUs, originUs), " / ",
        formatReflexOffset(endUs, originUs));
    }


    bool isCompleteReflexReport(const D3D_LOW_LATENCY_FRAME_REPORT& report) {
      return report.frameID
          && report.simStartTime
          && report.gpuRenderStartTime
          && report.gpuRenderEndTime
          && report.gpuRenderEndTime >= report.gpuRenderStartTime;
    }


    bool isOrderedReflexPair(
      const D3D_LOW_LATENCY_FRAME_REPORT& previous,
      const D3D_LOW_LATENCY_FRAME_REPORT& current) {
      return isCompleteReflexReport(previous)
          && isCompleteReflexReport(current)
          && previous.frameID < current.frameID
          && previous.simStartTime <= current.simStartTime
          && previous.gpuRenderEndTime <= current.gpuRenderEndTime;
    }

    class HudTextLayoutRenderer final : public HudRenderer {

    public:

      void drawText(
              uint32_t            size,
              HudPos              position,
              uint32_t            color,
        const std::string&        text) override {
        if (!text.empty())
          m_draws.push_back({ size, position, color, text, m_line });
      }

      void setLine(size_t line) {
        m_line = line;
      }

      void replay(
              HudRenderer&        renderer,
              uint32_t            surfaceWidth,
              uint32_t            surfaceHeight,
              float               scale,
              bool                horizontal,
              bool                center,
              bool                bottom) const {
        constexpr int64_t gap = 24;

        if (m_draws.empty())
          return;

        bool haveWidth = surfaceWidth && std::isfinite(scale) && scale > 0.0f;
        int64_t logicalWidth = haveWidth
          ? int64_t(double(surfaceWidth) / scale)
          : 0;
        bool haveHeight = surfaceHeight && std::isfinite(scale) && scale > 0.0f;
        int64_t logicalHeight = haveHeight
          ? int64_t(double(surfaceHeight) / scale)
          : 0;

        if (!horizontal) {
          int64_t left = m_draws.front().position.x;
          int64_t right = left;
          int64_t lastBaseline = m_draws.front().position.y;

          for (const auto& draw : m_draws) {
            left = std::min<int64_t>(left, draw.position.x);
            right = std::max<int64_t>(right,
              int64_t(draw.position.x) + getTextWidth(draw));
            lastBaseline = std::max<int64_t>(lastBaseline, draw.position.y);
          }

          int64_t x = center && haveWidth
            ? std::max<int64_t>(8, (logicalWidth - (right - left)) / 2)
            : 8;
          int64_t yOffset = bottom && haveHeight
            ? logicalHeight - 20 - lastBaseline
            : 0;

          for (const auto& draw : m_draws) {
            int64_t drawX = x + int64_t(draw.position.x) - left;
            int64_t drawY = int64_t(draw.position.y) + yOffset;
            HudPos position = {
              int32_t(std::clamp(drawX,
                int64_t(std::numeric_limits<int32_t>::min()),
                int64_t(std::numeric_limits<int32_t>::max()))),
              int32_t(std::clamp(drawY,
                int64_t(std::numeric_limits<int32_t>::min()),
                int64_t(std::numeric_limits<int32_t>::max()))),
            };

            renderer.drawText(draw.size, position, draw.color, draw.text);
          }

          return;
        }

        constexpr int64_t lineHeight = 20;
        size_t lastLine = m_draws.back().line;

        for (size_t first = 0; first < m_draws.size();) {
          size_t line = m_draws[first].line;
          size_t lineEnd = first;
          int64_t totalWidth = -gap;

          while (lineEnd < m_draws.size() && m_draws[lineEnd].line == line) {
            int64_t left, right;
            lineEnd = getLineBounds(lineEnd, &left, &right);
            totalWidth += right - left + gap;
          }

          int64_t x = center && haveWidth
            ? std::max<int64_t>(8, (logicalWidth - totalWidth) / 2)
            : 8;
          int64_t lineOffset = int64_t(bottom ? lastLine - line : line);
          int64_t y = bottom && haveHeight
            ? logicalHeight - 20 - lineOffset * lineHeight
            : 24 + lineOffset * lineHeight;

          for (size_t i = first; i < lineEnd;) {
            int64_t left, right;
            size_t end = getLineBounds(i, &left, &right);

            for (size_t j = i; j < end; j++) {
              int64_t drawX = x + int64_t(m_draws[j].position.x) - left;
              HudPos position = {
                int32_t(std::clamp(drawX,
                  int64_t(std::numeric_limits<int32_t>::min()),
                  int64_t(std::numeric_limits<int32_t>::max()))),
                int32_t(std::clamp(y,
                  int64_t(std::numeric_limits<int32_t>::min()),
                  int64_t(std::numeric_limits<int32_t>::max()))),
              };

              renderer.drawText(m_draws[j].size, position,
                m_draws[j].color, m_draws[j].text);
            }

            x += right - left + gap;
            i = end;
          }

          first = lineEnd;
        }
      }

    private:

      struct Draw {
        uint32_t size;
        HudPos position;
        uint32_t color;
        std::string text;
        size_t line;
      };

      std::vector<Draw> m_draws;
      size_t m_line = 0;

      int64_t getTextWidth(const Draw& draw) const {
        return textWidth(draw.size, draw.text);
      }

      size_t getLineBounds(
              size_t              first,
              int64_t*            pLeft,
              int64_t*            pRight) const {
        int32_t y = m_draws[first].position.y;
        int64_t left = m_draws[first].position.x;
        int64_t right = left;
        size_t end = first;

        while (end < m_draws.size() &&
               m_draws[end].line == m_draws[first].line &&
               m_draws[end].position.y == y) {
          left = std::min<int64_t>(left, m_draws[end].position.x);
          right = std::max<int64_t>(right,
            int64_t(m_draws[end].position.x) + getTextWidth(m_draws[end]));
          end += 1;
        }

        *pLeft = left;
        *pRight = right;
        return end;
      }

    };

  }

  HudItem::~HudItem() {

  }


  void HudItem::update(dxvk::high_resolution_clock::time_point time) {
    // Do nothing by default. Some items won't need this.
  }


  HudItemSet::HudItemSet(const Rc<DxvkDevice>& device)
  : HudItemSet([&] {
    std::string configStr = env::getEnvVar("DXVK_HUD");

    if (configStr.empty())
      configStr = device->config().hud;

    return configStr;
  }(), device->config().hudFpsLowsWindow) { }


  HudItemSet::HudItemSet(std::string configStr, int32_t fpsLowsWindow) {
    m_fpsLowsWindowNs = int64_t(std::clamp(fpsLowsWindow, 1, 30)) * 1'000'000'000;

    std::string::size_type pos = 0;
    std::string::size_type end = 0;
    std::string::size_type mid = 0;

    while (pos < configStr.size()) {
      end = configStr.find(',', pos);
      mid = configStr.find('=', pos);

      if (end == std::string::npos)
        end = configStr.size();

      if (mid != std::string::npos && mid < end) {
        m_options.insert({
          configStr.substr(pos,     mid - pos),
          configStr.substr(mid + 1, end - mid - 1) });
      } else {
        std::string token = configStr.substr(pos, end - pos);
        m_enabled.insert(token);
        m_layoutTokens.push_back(token);

        if (token == "newline")
          m_hasNewline = true;
      }

      pos = end + 1;
    }

    if (m_enabled.find("full") != m_enabled.end())
      m_enableFull = true;

    if (m_enabled.find("1") != m_enabled.end()) {
      m_enabled.insert("devinfo");
      m_enabled.insert("fps");
    }

    m_renderOptions.scale = std::clamp(
      getOption<float>("scale", 1.0f), 0.25f, 4.0f);
    m_renderOptions.opacity = std::clamp(
      getOption<float>("opacity", 1.0f), 0.1f, 1.0f);
    m_renderOptions.horizontal = m_enabled.find("horizontal") != m_enabled.end();
    m_renderOptions.center = m_enabled.find("center") != m_enabled.end();
    m_renderOptions.bottom = m_enabled.find("bottom") != m_enabled.end();
  }


  HudItemSet::~HudItemSet() {

  }


  void HudItemSet::addSystemInfoItems() {
    if (m_enableFull) {
      add<HudSystemInfoItem>("systeminfo", -1, HudSystemInfoItem::All);
    } else if (isEnabled("systeminfo")) {
      add<HudSystemInfoItem>("systeminfo", -1, HudSystemInfoItem::System);
      add<HudSystemInfoItem>("wine", -1, HudSystemInfoItem::Wine);
    } else {
      add<HudSystemInfoItem>("proton", -1, HudSystemInfoItem::Proton);
      add<HudSystemInfoItem>("wine", -1, HudSystemInfoItem::Wine);
      add<HudSystemInfoItem>("winsys", -1, HudSystemInfoItem::Display);
    }
  }


  void HudItemSet::addGpuTelemetryItems(
    const Rc<DxvkAdapter>& adapter) {
    uint32_t requested = 0;
    bool group = isEnabled("gpu");
    bool info = group || isEnabled("gpu.name") || isEnabled("gpu.driver");

    if (!adapter)
      return;

    if (group || isEnabled("gpu.power"))
      requested |= D3DKMT_WINE_GPU_TELEMETRY_POWER
                |  D3DKMT_WINE_GPU_TELEMETRY_POWER_LIMIT;
    if (group || isEnabled("gpu.temp"))
      requested |= D3DKMT_WINE_GPU_TELEMETRY_TEMPERATURE;
    if (group || isEnabled("gpu.load"))
      requested |= D3DKMT_WINE_GPU_TELEMETRY_UTILIZATION;
    if (group || isEnabled("gpu.clock"))
      requested |= D3DKMT_WINE_GPU_TELEMETRY_CLOCK;
    if (group || isEnabled("gpu.memclock"))
      requested |= D3DKMT_WINE_GPU_TELEMETRY_MEMORY_CLOCK;
    if (group || isEnabled("gpu.vram"))
      requested |= D3DKMT_WINE_GPU_TELEMETRY_VRAM;
    if (group || isEnabled("gpu.memload"))
      requested |= D3DKMT_WINE_GPU_TELEMETRY_MEMORY_UTIL;
    if (group || isEnabled("gpu.pcie"))
      requested |= D3DKMT_WINE_GPU_TELEMETRY_PCIE;
    if (!info && !requested)
      return;

    Rc<HudTelemetryData> data = new HudGpuTelemetryData(adapter, requested);

    if (group) {
      add<HudTelemetryItem>("gpu", -1, data, HudGpuTelemetryMetrics.data(),
        HudGpuTelemetryMetrics.size(), HudGpuTelemetryMetrics.size(), HudGpuTelemetryLabelColor);
      return;
    }

    for (size_t i = 0; i < HudGpuTelemetryMetrics.size(); i++) {
      add<HudTelemetryItem>(HudGpuTelemetryMetrics[i].option, -1, data,
        HudGpuTelemetryMetrics.data(), HudGpuTelemetryMetrics.size(), i, HudGpuTelemetryLabelColor);
    }
  }


  void HudItemSet::addCpuTelemetryItems(
    const Rc<DxvkAdapter>& adapter) {
    uint32_t requested = 0;
    bool group = isEnabled("cpu");
    bool info = group || isEnabled("cpu.name");

    if (group || isEnabled("cpu.power"))
      requested |= D3DKMT_WINE_CPU_TELEMETRY_POWER
                |  D3DKMT_WINE_CPU_TELEMETRY_POWER_LIMIT;
    if (group || isEnabled("cpu.temp"))
      requested |= D3DKMT_WINE_CPU_TELEMETRY_TEMPERATURE;
    if (group || isEnabled("cpu.load"))
      requested |= D3DKMT_WINE_CPU_TELEMETRY_UTILIZATION;
    if (group || isEnabled("cpu.clock"))
      requested |= D3DKMT_WINE_CPU_TELEMETRY_CLOCK;
    if (!info && !requested)
      return;

    Rc<HudTelemetryData> data = new HudCpuTelemetryData(adapter, requested);

    if (group) {
      add<HudTelemetryItem>("cpu", -1, data, HudCpuTelemetryMetrics.data(),
        HudCpuTelemetryMetrics.size(), HudCpuTelemetryMetrics.size(), HudCpuTelemetryLabelColor);
      return;
    }

    for (size_t i = 0; i < HudCpuTelemetryMetrics.size(); i++) {
      add<HudTelemetryItem>(HudCpuTelemetryMetrics[i].option, -1, data,
        HudCpuTelemetryMetrics.data(), HudCpuTelemetryMetrics.size(), i, HudCpuTelemetryLabelColor);
    }
  }


  void HudItemSet::addReflexItems(
          ID3DLowLatencyDevice* lowLatencyDevice) {
    if (!lowLatencyDevice)
      return;

    if (isEnabled("reflex")) {
      Rc<HudReflexData> data = new HudReflexData(lowLatencyDevice);
      add<HudReflexItem>("reflex", -1, data, HudReflexMetric::Count);
      return;
    }

    bool enabled = false;

    for (const auto& metric : HudReflexMetrics)
      enabled |= isEnabled(metric.option);

    if (!enabled)
      return;

    Rc<HudReflexData> data = new HudReflexData(lowLatencyDevice);

    for (size_t i = 0; i < HudReflexMetrics.size(); i++) {
      add<HudReflexItem>(HudReflexMetrics[i].option, -1,
        data, HudReflexMetric(i));
    }
  }


  template<typename T>
  static void addPresentTelemetrySource(
          HudItemSet&             items,
          T*                      presenter) {
    if (!presenter)
      return;

    bool group = items.isEnabled("present_latency");
    bool enabled = group;

    for (const auto& metric : HudPresentTelemetryMetrics)
      enabled |= items.isEnabled(metric.option);

    if (!enabled)
      return;

    Rc<HudPresentTelemetryData> data = new HudPresentTelemetryData(presenter);

    if (group) {
      items.add<HudPresentTelemetryItem>("present_latency", -1,
        data, HudPresentTelemetryMetric::Count);
      return;
    }

    for (size_t i = 0; i < HudPresentTelemetryMetrics.size(); i++) {
      items.add<HudPresentTelemetryItem>(HudPresentTelemetryMetrics[i].option, -1,
        data, HudPresentTelemetryMetric(i));
    }
  }


  void HudItemSet::addPresentTelemetryItems(
    const Rc<Presenter>& presenter) {
    addPresentTelemetrySource(*this, presenter.ptr());
  }


  void HudItemSet::addPresentTelemetryItems(
          IDXGIVkSwapChainPresentTelemetry* presenter) {
    addPresentTelemetrySource(*this, presenter);
  }


  bool HudItemSet::presentTelemetryEnabled() const {
    if (isEnabled("present_latency"))
      return true;

    for (const auto& metric : HudPresentTelemetryMetrics) {
      if (isEnabled(metric.option))
        return true;
    }

    return false;
  }


  HudReflexData::HudReflexData(
          ID3DLowLatencyDevice* lowLatencyDevice)
  : m_lowLatencyDevice(lowLatencyDevice) {
    m_lowLatencyDevice->AddRef();
    m_values.fill("--");
  }


  HudReflexData::~HudReflexData() {
    m_lowLatencyDevice->Release();
  }


  void HudReflexData::update(
          dxvk::high_resolution_clock::time_point time) {
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      time - m_lastUpdate).count();

    if (m_lastUpdate.time_since_epoch().count() && elapsed < UpdateInterval)
      return;

    m_lastUpdate = time;

    D3D_LOW_LATENCY_RESULTS results = { };
    results.version = uint32_t(sizeof(results)) | (1u << 16);

    if (FAILED(m_lowLatencyDevice->GetLatencyInfo(&results))) {
      m_values.fill("--");
      return;
    }

    const auto& report = results.frameReports[63];
    const auto& previous = results.frameReports[62];

    if (!isOrderedReflexPair(previous, report)) {
      m_values.fill("--");
      return;
    }

    m_values[reflexMetricIndex(HudReflexMetric::FrameId)] = str::format(report.frameID);
    m_values[reflexMetricIndex(HudReflexMetric::SimulationInterval)] =
      formatReflexSpan(previous.simStartTime, report.simStartTime);
    m_values[reflexMetricIndex(HudReflexMetric::Input)] =
      formatReflexOffset(report.inputSampleTime, report.simStartTime);
    m_values[reflexMetricIndex(HudReflexMetric::Simulation)] =
      formatReflexOffset(report.simEndTime, report.simStartTime);
    m_values[reflexMetricIndex(HudReflexMetric::Submit)] =
      formatReflexRange(report.renderSubmitStartTime,
        report.renderSubmitEndTime, report.simStartTime);
    m_values[reflexMetricIndex(HudReflexMetric::Present)] =
      formatReflexRange(report.presentStartTime,
        report.presentEndTime, report.simStartTime);
    m_values[reflexMetricIndex(HudReflexMetric::Driver)] =
      formatReflexRange(report.driverStartTime,
        report.driverEndTime, report.simStartTime);
    m_values[reflexMetricIndex(HudReflexMetric::Queue)] =
      formatReflexRange(report.osRenderQueueStartTime,
        report.osRenderQueueEndTime, report.simStartTime);
    m_values[reflexMetricIndex(HudReflexMetric::Gpu)] =
      formatReflexRange(report.gpuRenderStartTime,
        report.gpuRenderEndTime, report.simStartTime);
    m_values[reflexMetricIndex(HudReflexMetric::GpuActive)] =
      formatReflexDuration(report.gpuActiveRenderTimeUs);
    m_values[reflexMetricIndex(HudReflexMetric::GpuFrame)] =
      formatReflexDuration(report.gpuFrameTimeUs);
    m_values[reflexMetricIndex(HudReflexMetric::Camera)] =
      formatReflexOffset(report.cameraConstructedTime, report.simStartTime);
    m_values[reflexMetricIndex(HudReflexMetric::Copy)] =
      formatReflexDuration(report.crossAdapterCopyTimeUs);
    m_values[reflexMetricIndex(HudReflexMetric::Ai)] =
      formatReflexDuration(report.aiFrameTimeUs);
  }


  const std::string& HudReflexData::value(
          HudReflexMetric        metric) const {
    return m_values[reflexMetricIndex(metric)];
  }


  HudReflexItem::HudReflexItem(
    const Rc<HudReflexData>& data,
          HudReflexMetric    metric)
  : m_data(data), m_metric(metric) { }


  void HudReflexItem::update(
          dxvk::high_resolution_clock::time_point time) {
    m_data->update(time);
  }


  HudPos HudReflexItem::render(
    const Rc<DxvkCommandList>&ctx,
    const HudPipelineKey&     key,
    const HudOptions&         options,
          HudRenderer&        renderer,
          HudPos              position) {
    size_t first = m_metric == HudReflexMetric::Count
      ? 0
      : reflexMetricIndex(m_metric);
    size_t end = m_metric == HudReflexMetric::Count
      ? HudReflexMetrics.size()
      : first + 1;
    uint32_t valueOffset = 0;

    for (size_t i = first; i < end; i++)
      valueOffset = std::max(valueOffset,
        renderer.textWidth(16, HudReflexMetrics[i].label));

    valueOffset += renderer.textWidth(16, " ");

    for (size_t i = first; i < end; i++) {
      position.y += i == first ? 16 : 20;
      renderer.drawText(16, position, HudReflexLabelColor,
        HudReflexMetrics[i].label);
      renderer.drawText(16, { position.x + int32_t(valueOffset), position.y },
        HudTelemetryValueColor, m_data->value(HudReflexMetric(i)));
    }

    position.y += 8;
    return position;
  }


  HudPresentTelemetryData::HudPresentTelemetryData(
          Presenter* presenter)
  : m_presenter(presenter) {
    m_presenter->setPresentTelemetryEnabled(true);
    m_values.fill("--");
  }


  HudPresentTelemetryData::HudPresentTelemetryData(
          IDXGIVkSwapChainPresentTelemetry* presenter)
  : m_dxgiPresenter(presenter) {
    m_dxgiPresenter->AddRef();
    m_dxgiPresenter->SetEnabled(TRUE);
    m_values.fill("--");
  }


  HudPresentTelemetryData::~HudPresentTelemetryData() {
    if (m_presenter)
      m_presenter->setPresentTelemetryEnabled(false);

    if (m_dxgiPresenter) {
      m_dxgiPresenter->SetEnabled(FALSE);
      m_dxgiPresenter->Release();
    }
  }


  void HudPresentTelemetryData::update(
          dxvk::high_resolution_clock::time_point time) {
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      time - m_lastUpdate).count();

    if (m_lastUpdate.time_since_epoch().count() && elapsed < UpdateInterval)
      return;

    m_lastUpdate = time;
    m_values.fill("--");

    DXGI_VK_PRESENT_TELEMETRY data = { };
    data.StructSize = sizeof(data);

    if (m_presenter) {
      PresenterTelemetry telemetry = { };

      if (!m_presenter->getPresentTelemetry(telemetry))
        return;

      data.ValidFields = telemetry.validFields;
      data.PresentId = telemetry.presentId;
      data.QueueDurationNs = telemetry.queueDurationNs;
      data.DisplayDurationNs = telemetry.displayDurationNs;
      data.PresentDurationNs = telemetry.presentDurationNs;
      data.DisplayIntervalNs = telemetry.displayIntervalNs;
      data.CompletionStage = telemetry.completionStage;
    } else if (FAILED(m_dxgiPresenter->GetData(&data))) {
      return;
    }

    if (data.ValidFields & DXGI_VK_PRESENT_TELEMETRY_QUEUE)
      m_values[presentTelemetryMetricIndex(HudPresentTelemetryMetric::Queue)] =
        formatNanoseconds(data.QueueDurationNs);
    if (data.ValidFields & DXGI_VK_PRESENT_TELEMETRY_DISPLAY)
      m_values[presentTelemetryMetricIndex(HudPresentTelemetryMetric::Display)] =
        formatNanoseconds(data.DisplayDurationNs);
    if (data.ValidFields & DXGI_VK_PRESENT_TELEMETRY_PRESENT)
      m_values[presentTelemetryMetricIndex(HudPresentTelemetryMetric::Present)] =
        formatNanoseconds(data.PresentDurationNs);
    if (data.ValidFields & DXGI_VK_PRESENT_TELEMETRY_INTERVAL)
      m_values[presentTelemetryMetricIndex(HudPresentTelemetryMetric::Interval)] =
        formatNanoseconds(data.DisplayIntervalNs);
  }


  const std::string& HudPresentTelemetryData::value(
          HudPresentTelemetryMetric metric) const {
    return m_values[presentTelemetryMetricIndex(metric)];
  }


  HudPresentTelemetryItem::HudPresentTelemetryItem(
    const Rc<HudPresentTelemetryData>& data,
          HudPresentTelemetryMetric   metric)
  : m_data(data), m_metric(metric) { }


  void HudPresentTelemetryItem::update(
          dxvk::high_resolution_clock::time_point time) {
    m_data->update(time);
  }


  HudPos HudPresentTelemetryItem::render(
    const Rc<DxvkCommandList>&ctx,
    const HudPipelineKey&     key,
    const HudOptions&         options,
          HudRenderer&        renderer,
          HudPos              position) {
    size_t first = m_metric == HudPresentTelemetryMetric::Count
      ? 0
      : presentTelemetryMetricIndex(m_metric);
    size_t end = m_metric == HudPresentTelemetryMetric::Count
      ? HudPresentTelemetryMetrics.size()
      : first + 1;
    uint32_t valueOffset = 0;

    for (size_t i = first; i < end; i++)
      valueOffset = std::max(valueOffset,
        renderer.textWidth(16, HudPresentTelemetryMetrics[i].label));

    valueOffset += renderer.textWidth(16, " ");

    for (size_t i = first; i < end; i++) {
      position.y += i == first ? 16 : 20;
      renderer.drawText(16, position, HudPresentTelemetryLabelColor,
        HudPresentTelemetryMetrics[i].label);
      renderer.drawText(16, { position.x + int32_t(valueOffset), position.y },
        HudTelemetryValueColor, m_data->value(HudPresentTelemetryMetric(i)));
    }

    position.y += 8;
    return position;
  }


  void HudItemSet::orderItems() {
    if (m_itemsOrdered)
      return;

    std::vector<bool> added(m_items.size());
    std::vector<size_t> order;
    std::vector<size_t> lines;
    size_t line = 0;

    order.reserve(m_items.size());
    lines.reserve(m_items.size());

    auto appendItem = [&] (size_t index) {
      if (!added[index]) {
        order.push_back(index);
        lines.push_back(line);
        added[index] = true;
      }
    };

    auto appendNamedItems = [&] (const std::string& name) {
      for (size_t i = 0; i < m_items.size(); i++) {
        if (m_itemNames[i] == name)
          appendItem(i);
      }
    };

    for (const auto& token : m_layoutTokens) {
      if (token == "newline") {
        line += 1;
      } else if (token == "1") {
        appendNamedItems("devinfo");
        appendNamedItems("fps");
      } else if (token == "full") {
        for (size_t i = 0; i < m_items.size(); i++)
          appendItem(i);
      } else {
        appendNamedItems(token);
      }
    }

    for (size_t i = 0; i < m_items.size(); i++)
      appendItem(i);

    std::vector<std::string> orderedNames;
    std::vector<Rc<HudItem>> orderedItems;

    orderedNames.reserve(m_items.size());
    orderedItems.reserve(m_items.size());

    for (size_t index : order) {
      orderedNames.push_back(std::move(m_itemNames[index]));
      orderedItems.push_back(std::move(m_items[index]));
    }

    m_itemNames = std::move(orderedNames);
    m_items = std::move(orderedItems);
    m_itemLines = std::move(lines);
    m_itemsOrdered = true;
  }


  void HudItemSet::update() {
    orderItems();

    auto time = dxvk::high_resolution_clock::now();

    for (const auto& item : m_items)
      item->update(time);
  }


  void HudItemSet::render(
      const Rc<DxvkCommandList>&ctx,
      const HudPipelineKey&     key,
      const HudOptions&         options,
            HudRenderer&        renderer,
            uint32_t            surfaceWidth,
            uint32_t            surfaceHeight) {
    orderItems();

    if (!options.horizontal && !options.center && !options.bottom) {
      HudPos position = { 8, 8 };

      for (const auto& item : m_items)
        position = item->render(ctx, key, options, renderer, position);

      return;
    }

    HudTextLayoutRenderer layout;
    HudPos position = options.horizontal ? HudPos{ 0, 0 } : HudPos{ 8, 8 };
    size_t line = std::numeric_limits<size_t>::max();

    for (size_t i = 0; i < m_items.size(); i++) {
      if (m_items[i]->supportsHorizontalLayout()) {
        if (options.horizontal && m_hasNewline) {
          if (line != m_itemLines[i]) {
            line = m_itemLines[i];
            position = { 0, 0 };
          }

          layout.setLine(line);
        }

        position = m_items[i]->render(ctx, key, options, layout, position);
      }
    }

    layout.replay(renderer, surfaceWidth, surfaceHeight, options.scale,
      options.horizontal, options.center, options.bottom);
    position = { 8, 8 };

    for (const auto& item : m_items) {
      if (!item->supportsHorizontalLayout())
        position = item->render(ctx, key, options, renderer, position);
    }
  }


  void HudItemSet::render(
          HudRenderer&        renderer,
          uint32_t            surfaceWidth,
          uint32_t            surfaceHeight) {
    render(Rc<DxvkCommandList>(), HudPipelineKey(),
      m_renderOptions, renderer, surfaceWidth, surfaceHeight);
  }


  void HudItemSet::parseOption(const std::string& str, float& value) {
    try {
      value = std::stof(str);
    } catch (const std::invalid_argument&) {
      return;
    }
  }


  HudGpuTelemetryData::HudGpuTelemetryData(
    const Rc<DxvkAdapter>& adapter,
          uint32_t         requested)
  : m_adapter  (adapter),
    m_requested(requested) {
    auto info = m_adapter->info();
    std::string driverVersion = info.driverInfo;

    m_values.fill("--");
    m_values[size_t(HudGpuTelemetryMetric::Name)] = info.deviceName;

    if (driverVersion.empty())
      driverVersion = DxvkDeviceCapabilities::decodeDriverVersion(
        info.driverId, info.driverVersion).toString();

    if (*info.driverName && !driverVersion.empty())
      m_values[size_t(HudGpuTelemetryMetric::Driver)] =
        str::format(info.driverName, " ", driverVersion);
    else if (*info.driverName)
      m_values[size_t(HudGpuTelemetryMetric::Driver)] = info.driverName;
    else if (!driverVersion.empty())
      m_values[size_t(HudGpuTelemetryMetric::Driver)] = std::move(driverVersion);

    if (m_requested)
      update(dxvk::high_resolution_clock::now());
  }


  void HudGpuTelemetryData::update(
          dxvk::high_resolution_clock::time_point time) {
    if (!m_supported || !m_requested)
      return;

    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(time - m_lastUpdate);

    if (elapsed.count() < UpdateInterval)
      return;

    m_lastUpdate = time;

    D3DKMT_WINE_GPU_TELEMETRY data = { };
    D3DKMT_QUERYADAPTERINFO query = { };

    for (size_t i = size_t(HudGpuTelemetryMetric::Power); i < m_values.size(); i++)
      m_values[i] = "--";

    data.Requested = m_requested;
    query.hAdapter = m_adapter ? m_adapter->kmtLocal() : 0;
    query.Type = KMTQAITYPE_WINE_GPU_TELEMETRY;
    query.pPrivateDriverData = &data;
    query.PrivateDriverDataSize = sizeof(data);

    NTSTATUS status = query.hAdapter
      ? D3DKMTQueryAdapterInfo(&query)
      : D3DKMT_STATUS_NOT_IMPLEMENTED;

    if (status >= 0) {
      std::string power = "--";
      std::string powerLimit = "--";

      if (data.Valid & D3DKMT_WINE_GPU_TELEMETRY_POWER) {
        uint64_t deciwatts = (data.PowerMicrowatts + 50'000) / 100'000;
        power = str::format(deciwatts / 10, ".", deciwatts % 10);
      }

      if (data.Valid & D3DKMT_WINE_GPU_TELEMETRY_POWER_LIMIT) {
        uint64_t deciwatts = (uint64_t(data.PowerLimitMilliwatts) + 50) / 100;
        powerLimit = str::format(deciwatts / 10, ".", deciwatts % 10);
      }

      if (m_requested & D3DKMT_WINE_GPU_TELEMETRY_POWER)
        m_values[size_t(HudGpuTelemetryMetric::Power)] =
          str::format(power, " / ", powerLimit, " W");

      if (data.Valid & D3DKMT_WINE_GPU_TELEMETRY_TEMPERATURE) {
        m_values[size_t(HudGpuTelemetryMetric::Temperature)] =
          str::format(data.TemperatureDeciCelsius / 10, ".",
          data.TemperatureDeciCelsius % 10, " C");
      }

      if (data.Valid & D3DKMT_WINE_GPU_TELEMETRY_UTILIZATION)
        m_values[size_t(HudGpuTelemetryMetric::Utilization)] =
          str::format(data.UtilizationPercent, "%");

      if (data.Valid & D3DKMT_WINE_GPU_TELEMETRY_CLOCK)
        m_values[size_t(HudGpuTelemetryMetric::GraphicsClock)] =
          str::format((uint64_t(data.GraphicsClockKHz) + 500) / 1000, " MHz");

      if (data.Valid & D3DKMT_WINE_GPU_TELEMETRY_MEMORY_CLOCK)
        m_values[size_t(HudGpuTelemetryMetric::MemoryClock)] =
          str::format((uint64_t(data.MemoryClockKHz) + 500) / 1000, " MHz");

      if (data.Valid & D3DKMT_WINE_GPU_TELEMETRY_VRAM) {
        uint64_t usedMiB = (data.VramUsedBytes + (1u << 19)) >> 20;
        uint64_t totalMiB = (data.VramTotalBytes + (1u << 19)) >> 20;
        m_values[size_t(HudGpuTelemetryMetric::Vram)] =
          str::format(usedMiB, " / ", totalMiB, " MiB");
      }

      if (data.Valid & D3DKMT_WINE_GPU_TELEMETRY_MEMORY_UTIL)
        m_values[size_t(HudGpuTelemetryMetric::MemoryUtilization)] =
          str::format(data.MemoryUtilizationPercent, "%");

      if (data.Valid & D3DKMT_WINE_GPU_TELEMETRY_PCIE) {
        std::string value = str::format("Gen ", data.PcieGeneration, " x", data.PcieWidth);

        if (data.PcieMaxGeneration && data.PcieMaxWidth)
          value = str::format(value, " / Gen ", data.PcieMaxGeneration, " x", data.PcieMaxWidth);

        m_values[size_t(HudGpuTelemetryMetric::Pcie)] = std::move(value);
      }
    } else if (status == D3DKMT_STATUS_NOT_IMPLEMENTED) {
      m_supported = false;
    }
  }


  const std::string& HudGpuTelemetryData::value(size_t metric) const {
    return m_values[metric];
  }


  HudCpuTelemetryData::HudCpuTelemetryData(
    const Rc<DxvkAdapter>& adapter,
          uint32_t         requested)
  : m_adapter  (adapter),
    m_requested(requested) {
    m_values.fill("--");

    const auto& cpuName = HudSystemInfo::get().cpuName;
    if (!cpuName.empty())
      m_values[size_t(HudCpuTelemetryMetric::Name)] = cpuName;

    if (m_requested)
      update(dxvk::high_resolution_clock::now());
  }


  void HudCpuTelemetryData::update(
          dxvk::high_resolution_clock::time_point time) {
    if (!m_supported || !m_requested)
      return;

    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(time - m_lastUpdate);

    if (elapsed.count() < UpdateInterval)
      return;

    m_lastUpdate = time;

    D3DKMT_WINE_CPU_TELEMETRY data = { };
    D3DKMT_QUERYADAPTERINFO query = { };

    for (size_t i = size_t(HudCpuTelemetryMetric::Power); i < m_values.size(); i++)
      m_values[i] = "--";

    data.Requested = m_requested;
    query.hAdapter = m_adapter ? m_adapter->kmtLocal() : 0;
    query.Type = KMTQAITYPE_WINE_CPU_TELEMETRY;
    query.pPrivateDriverData = &data;
    query.PrivateDriverDataSize = sizeof(data);

    NTSTATUS status = query.hAdapter
      ? D3DKMTQueryAdapterInfo(&query)
      : D3DKMT_STATUS_NOT_IMPLEMENTED;

    if (status >= 0) {
      std::string power = "--";
      std::string powerLimit = "--";

      if (data.Valid & D3DKMT_WINE_CPU_TELEMETRY_POWER) {
        uint64_t deciwatts = (data.PowerMicrowatts + 50'000) / 100'000;
        power = str::format(deciwatts / 10, ".", deciwatts % 10);
      }

      if (data.Valid & D3DKMT_WINE_CPU_TELEMETRY_POWER_LIMIT) {
        uint64_t deciwatts = (uint64_t(data.PowerLimitMilliwatts) + 50) / 100;
        powerLimit = str::format(deciwatts / 10, ".", deciwatts % 10);
      }

      if (m_requested & D3DKMT_WINE_CPU_TELEMETRY_POWER)
        m_values[size_t(HudCpuTelemetryMetric::Power)] =
          str::format(power, " / ", powerLimit, " W");

      if (data.Valid & D3DKMT_WINE_CPU_TELEMETRY_TEMPERATURE) {
        m_values[size_t(HudCpuTelemetryMetric::Temperature)] =
          str::format(data.TemperatureDeciCelsius / 10, ".",
          data.TemperatureDeciCelsius % 10, " C");
      }

      if (data.Valid & D3DKMT_WINE_CPU_TELEMETRY_UTILIZATION)
        m_values[size_t(HudCpuTelemetryMetric::Utilization)] =
          str::format(data.UtilizationPercent, "%");

      if (data.Valid & D3DKMT_WINE_CPU_TELEMETRY_CLOCK) {
        uint64_t average = (uint64_t(data.AverageClockKHz) + 500) / 1000;

        if (data.MaximumClockKHz) {
          uint64_t maximum = (uint64_t(data.MaximumClockKHz) + 500) / 1000;
          m_values[size_t(HudCpuTelemetryMetric::Clock)] =
            str::format(average, " / ", maximum, " MHz");
        } else {
          m_values[size_t(HudCpuTelemetryMetric::Clock)] =
            str::format(average, " MHz");
        }
      }
    } else if (status == D3DKMT_STATUS_NOT_IMPLEMENTED) {
      m_supported = false;
    }
  }


  const std::string& HudCpuTelemetryData::value(size_t metric) const {
    return m_values[metric];
  }


  HudTelemetryItem::HudTelemetryItem(
    const Rc<HudTelemetryData>& data,
    const HudTelemetryMetricInfo* metrics,
          size_t                metricCount,
          size_t                metric,
          uint32_t              labelColor)
  : m_data       (data),
    m_metrics    (metrics),
    m_metricCount(metricCount),
    m_metric     (metric),
    m_labelColor (labelColor) { }


  void HudTelemetryItem::update(
          dxvk::high_resolution_clock::time_point time) {
    m_data->update(time);
  }


  HudPos HudTelemetryItem::render(
    const Rc<DxvkCommandList>&ctx,
    const HudPipelineKey&     key,
    const HudOptions&         options,
          HudRenderer&        renderer,
          HudPos              position) {
    size_t first = m_metric == m_metricCount
      ? 0
      : m_metric;
    size_t end = m_metric == m_metricCount
      ? m_metricCount
      : first + 1;
    uint32_t valueOffset = 0;

    for (size_t i = first; i < end; i++)
      valueOffset = std::max(valueOffset,
        renderer.textWidth(16, m_metrics[i].label));

    valueOffset += renderer.textWidth(16, " ");

    for (size_t i = first; i < end; i++) {
      position.y += 20;
      renderer.drawText(16, position, m_labelColor, m_metrics[i].label);
      renderer.drawText(16, { position.x + int32_t(valueOffset), position.y },
        HudTelemetryValueColor, m_data->value(i));
    }

    position.y += 8;
    return position;
  }


  HudSystemInfoItem::HudSystemInfoItem(uint32_t fields) {
    const auto& info = HudSystemInfo::get();

    if ((fields & Cpu) && !info.cpuName.empty())
      m_lines.push_back({ "CPU: ", info.cpuName });

    if ((fields & Proton) && !info.protonBuild.empty())
      m_lines.push_back({ "Proton: ", info.protonBuild });

    if ((fields & Wine) && !info.wineVersion.empty()) {
      std::string wine = info.wineVersion;

      if (!info.wineBuild.empty())
        wine = str::format(wine, " (", info.wineBuild, ")");

      m_lines.push_back({ "Wine: ", std::move(wine) });
    }

    if ((fields & Display) && !info.displayBackend.empty())
      m_lines.push_back({ "Window system: ", info.displayBackend });
  }


  HudPos HudSystemInfoItem::render(
    const Rc<DxvkCommandList>&ctx,
    const HudPipelineKey&     key,
    const HudOptions&         options,
          HudRenderer&        renderer,
          HudPos              position) {
    for (const auto& line : m_lines) {
      position.y += 20;
      renderer.drawText(16, position, 0xff40ffffu, line.label);
      renderer.drawText(16,
        { position.x + int32_t(renderer.textWidth(16, line.label)), position.y },
        0xffffffffu, line.value);
    }

    position.y += 8;
    return position;
  }


  HudPos HudVersionItem::render(
    const Rc<DxvkCommandList>&ctx,
    const HudPipelineKey&     key,
    const HudOptions&         options,
          HudRenderer&        renderer,
          HudPos              position) {
    position.y += 16;
    renderer.drawText(16, position, 0xffffffffu, "DXVK " DXVK_VERSION);

    position.y += 8;
    return position;
  }


  HudClientApiItem::HudClientApiItem(std::string api)
  : m_api(std::move(api)) {

  }


  HudClientApiItem::~HudClientApiItem() {

  }


  HudPos HudClientApiItem::render(
    const Rc<DxvkCommandList>&ctx,
    const HudPipelineKey&     key,
    const HudOptions&         options,
          HudRenderer&        renderer,
          HudPos              position) {
    std::lock_guard lock(m_mutex);

    position.y += 16;
    renderer.drawText(16, position, 0xffffffffu, m_api);

    position.y += 8;
    return position;
  }


  HudDeviceInfoItem::HudDeviceInfoItem(const Rc<DxvkDevice>& device) {
    const auto& props = device->properties();

    std::string driverInfo = props.vk12.driverInfo;

    if (driverInfo.empty())
      driverInfo = props.driverVersion.toString();

    m_deviceName = props.core.properties.deviceName;
    m_driverName = str::format("Driver:  ", props.vk12.driverName);
    m_driverVer = str::format("Version: ", driverInfo);
  }


  HudDeviceInfoItem::HudDeviceInfoItem(
          std::string         deviceName,
          std::string         driverName,
          std::string         driverVersion)
  : m_deviceName(std::move(deviceName)),
    m_driverName(std::move(driverName)),
    m_driverVer (std::move(driverVersion)) { }


  HudDeviceInfoItem::~HudDeviceInfoItem() {

  }


  HudPos HudDeviceInfoItem::render(
    const Rc<DxvkCommandList>&ctx,
    const HudPipelineKey&     key,
    const HudOptions&         options,
          HudRenderer&        renderer,
          HudPos              position) {
    position.y += 16;
    renderer.drawText(16, position, 0xffffffffu, m_deviceName);

    if (!m_driverName.empty()) {
      position.y += 24;
      renderer.drawText(16, position, 0xffffffffu, m_driverName);
    }

    if (!m_driverVer.empty()) {
      position.y += 20;
      renderer.drawText(16, position, 0xffffffffu, m_driverVer);
    }

    position.y += 8;
    return position;
  }


  HudFpsItem::HudFpsItem() { }
  HudFpsItem::~HudFpsItem() { }


  void HudFpsItem::update(dxvk::high_resolution_clock::time_point time) {
    m_frameCount += 1;

    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(time - m_lastUpdate);

    if (elapsed.count() >= UpdateInterval) {
      int64_t fps = (10'000'000ll * m_frameCount) / elapsed.count();

      m_frameRate = str::format(fps / 10, ".", fps % 10);
      m_frameCount = 0;
      m_lastUpdate = time;
    }
  }


  HudPos HudFpsItem::render(
    const Rc<DxvkCommandList>&ctx,
    const HudPipelineKey&     key,
    const HudOptions&         options,
          HudRenderer&        renderer,
          HudPos              position) {
    position.y += 16;

    renderer.drawText(16, position, 0xff4040ffu, "FPS:");
    renderer.drawText(16, { position.x + 60, position.y },
      0xffffffffu, m_frameRate);

    position.y += 8;
    return position;
  }


  HudFpsLowItem::HudFpsLowItem(int64_t windowDurationNs)
  : m_windowDurationNs(windowDurationNs),
    m_samples(MaxSamples) {
    m_scratch.reserve(MaxSamples);
  }


  void HudFpsLowItem::update(dxvk::high_resolution_clock::time_point time) {
    int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
      time.time_since_epoch()).count();

    if (!m_haveLastSample) {
      m_lastSampleTime = now;
      m_lastUpdateTime = now;
      m_haveLastSample = true;
      return;
    }

    int64_t duration = now - m_lastSampleTime;
    m_lastSampleTime = now;

    if (duration <= 0)
      return;

    if (duration >= m_windowDurationNs) {
      m_sampleHead = 0;
      m_sampleCount = 0;
      m_lastUpdateTime = now;
      m_onePercent = "--";
      m_pointOnePercent = "--";
      return;
    }

    addSample(now, duration);

    if (now - m_lastUpdateTime >= UpdateIntervalNs) {
      updateRates();
      m_lastUpdateTime = now;
    }
  }


  void HudFpsLowItem::addSample(int64_t endTime, int64_t duration) {
    int64_t cutoff = endTime - m_windowDurationNs;

    while (m_sampleCount && m_samples[m_sampleHead].endTime <= cutoff) {
      m_sampleHead = (m_sampleHead + 1) % MaxSamples;
      m_sampleCount -= 1;
    }

    size_t index;

    if (m_sampleCount < MaxSamples) {
      index = (m_sampleHead + m_sampleCount) % MaxSamples;
      m_sampleCount += 1;
    } else {
      index = m_sampleHead;
      m_sampleHead = (m_sampleHead + 1) % MaxSamples;
    }

    m_samples[index] = { endTime, duration };
  }


  void HudFpsLowItem::updateRates() {
    m_scratch.clear();

    for (size_t i = 0; i < m_sampleCount; i++)
      m_scratch.push_back(m_samples[(m_sampleHead + i) % MaxSamples].duration);

    if (m_scratch.empty()) {
      m_onePercent = "--";
      m_pointOnePercent = "--";
      return;
    }

    size_t onePercentCount = (m_scratch.size() + 99) / 100;
    auto onePercentEnd = m_scratch.begin() + onePercentCount;

    if (onePercentEnd != m_scratch.end())
      std::nth_element(m_scratch.begin(), onePercentEnd,
        m_scratch.end(), std::greater<int64_t>());

    m_onePercent = formatRate(m_scratch, onePercentCount);

    size_t pointOnePercentCount = (m_scratch.size() + 999) / 1000;
    auto pointOnePercentEnd = m_scratch.begin() + pointOnePercentCount;

    if (pointOnePercentEnd != onePercentEnd)
      std::nth_element(m_scratch.begin(), pointOnePercentEnd,
        onePercentEnd, std::greater<int64_t>());

    m_pointOnePercent = formatRate(m_scratch, pointOnePercentCount);
  }


  std::string HudFpsLowItem::formatRate(
    const std::vector<int64_t>& samples,
          size_t                count) {
    uint64_t duration = 0;

    for (size_t i = 0; i < count; i++)
      duration += uint64_t(samples[i]);

    uint64_t rate = duration
      ? (10'000'000'000ull * count + duration / 2) / duration
      : 0;

    return str::format(rate / 10, ".", rate % 10);
  }


  HudPos HudFpsLowItem::render(
    const Rc<DxvkCommandList>&ctx,
    const HudPipelineKey&     key,
    const HudOptions&         options,
          HudRenderer&        renderer,
          HudPos              position) {
    int32_t valueOffset = int32_t(renderer.textWidth(16, "0.1% low: "));

    position.y += 16;
    renderer.drawText(16, position, 0xff4040ffu, "1% low:");
    renderer.drawText(16, { position.x + valueOffset, position.y },
      0xffffffffu, m_onePercent);

    position.y += 20;
    renderer.drawText(16, position, 0xff4040ffu, "0.1% low:");
    renderer.drawText(16, { position.x + valueOffset, position.y },
      0xffffffffu, m_pointOnePercent);

    position.y += 8;
    return position;
  }


  HudFrameTimeItem::HudFrameTimeItem(const Rc<DxvkDevice>& device, HudVulkanRenderer* renderer)
  : m_device            (device),
    m_renderer          (renderer),
    m_gfxPipelineLayout (createPipelineLayout()) {
    createComputePipeline(*renderer);
  }


  HudFrameTimeItem::~HudFrameTimeItem() {
    auto vk = m_device->vkd();

    for (const auto& p : m_gfxPipelines)
      vk->vkDestroyPipeline(vk->device(), p.second, nullptr);

    vk->vkDestroyPipeline(vk->device(), m_computePipeline, nullptr);
  }


  HudPos HudFrameTimeItem::render(
    const Rc<DxvkCommandList>&ctx,
    const HudPipelineKey&     key,
    const HudOptions&         options,
          HudRenderer&        renderer,
          HudPos              position) {
    if (!m_gpuBuffer)
      createResources(ctx);

    HudPos minPos = {  12, -128 };
    HudPos maxPos = { 162, -128 };

    HudPos graphPos = { 8, -120 };
    HudPos graphSize = { NumDataPoints, 80 };

    uint32_t dataPoint = m_nextDataPoint++;

    processFrameTimes(ctx, key, *m_renderer,
      dataPoint, minPos, maxPos);

    drawFrameTimeGraph(ctx, key, *m_renderer,
      dataPoint, graphPos, graphSize);

    if (m_nextDataPoint >= NumDataPoints)
      m_nextDataPoint = 0u;

    return position;
  }


  void HudFrameTimeItem::processFrameTimes(
    const Rc<DxvkCommandList>&ctx,
    const HudPipelineKey&     key,
          HudVulkanRenderer&  renderer,
          uint32_t            dataPoint,
          HudPos              minPos,
          HudPos              maxPos) {
    if (unlikely(m_device->debugFlags().test(DxvkDebugFlag::Capture))) {
      ctx->cmdBeginDebugUtilsLabel(DxvkCmdBuffer::InitBuffer,
        vk::makeLabel(0xf0c0dc, "HUD frame time processing"));
    }

    // Write current time stamp to the buffer
    DxvkResourceBufferInfo slice = m_gpuBuffer->getSliceInfo();
    std::pair<VkQueryPool, uint32_t> query = m_query->getQuery();

    ctx->cmdResetQueryPool(DxvkCmdBuffer::InitBuffer,
      query.first, query.second, 1);

    ctx->cmdWriteTimestamp(DxvkCmdBuffer::InitBuffer,
      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      query.first, query.second);

    ctx->cmdCopyQueryPoolResults(DxvkCmdBuffer::InitBuffer,
      query.first, query.second, 1, slice.buffer,
      slice.offset + (dataPoint & 1u) * sizeof(uint64_t), sizeof(uint64_t),
      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

    VkMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

    VkDependencyInfo depInfo = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    depInfo.memoryBarrierCount = 1u;
    depInfo.pMemoryBarriers = &barrier;

    ctx->cmdPipelineBarrier(DxvkCmdBuffer::InitBuffer, &depInfo);

    // Process contents of the buffer and write out text draws
    auto bufferLayout = computeBufferLayout();

    auto drawParamBuffer = m_gpuBuffer->getSliceInfo(
      bufferLayout.drawParamOffset, bufferLayout.drawParamSize);
    auto drawInfoBuffer = m_gpuBuffer->getSliceInfo(
      bufferLayout.drawInfoOffset, bufferLayout.drawInfoSize);

    std::array<DxvkDescriptorWrite, 4u> descriptors = { };
    descriptors[0u].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptors[0u].buffer = m_gpuBuffer->getSliceInfo(0, bufferLayout.timestampSize);

    descriptors[1u].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptors[1u].buffer = drawParamBuffer;

    descriptors[2u].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptors[2u].buffer = drawInfoBuffer;

    descriptors[3u].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    descriptors[3u].descriptor = m_textWrView->getDescriptor(false);

    ComputePushConstants pushConstants = { };
    pushConstants.msPerTick = m_device->properties().core.properties.limits.timestampPeriod / 1000000.0f;
    pushConstants.dataPoint = dataPoint;
    pushConstants.textPosMinX = minPos.x + 48;
    pushConstants.textPosMinY = minPos.y;
    pushConstants.textPosMaxX = maxPos.x + 48;
    pushConstants.textPosMaxY = maxPos.y;

    ctx->cmdBindPipeline(DxvkCmdBuffer::InitBuffer,
      VK_PIPELINE_BIND_POINT_COMPUTE, m_computePipeline);

    ctx->bindResources(DxvkCmdBuffer::InitBuffer,
      m_computePipelineLayout, descriptors.size(), descriptors.data(),
      sizeof(pushConstants), &pushConstants);

    ctx->cmdDispatch(DxvkCmdBuffer::InitBuffer, 1, 1, 1);

    barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.srcStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = m_gpuBuffer->info().access;
    barrier.dstStageMask = m_gpuBuffer->info().stages;

    ctx->cmdPipelineBarrier(DxvkCmdBuffer::InitBuffer, &depInfo);

    // Display the min/max numbers
    renderer.drawText(12, minPos, 0xff4040ff, "min:");
    renderer.drawText(12, maxPos, 0xff4040ff, "max:");

    renderer.drawTextIndirect(ctx, key, drawParamBuffer,
      drawInfoBuffer, m_textRdView, 2u);

    if (unlikely(m_device->debugFlags().test(DxvkDebugFlag::Capture)))
      ctx->cmdEndDebugUtilsLabel(DxvkCmdBuffer::InitBuffer);

    // Make sure GPU resources are being kept alive as necessary
    ctx->track(m_gpuBuffer, DxvkAccess::Write);
    ctx->track(m_query);
  }


  void HudFrameTimeItem::drawFrameTimeGraph(
    const Rc<DxvkCommandList>&ctx,
    const HudPipelineKey&     key,
          HudVulkanRenderer&  renderer,
          uint32_t            dataPoint,
          HudPos              graphPos,
          HudPos              graphSize) {
    DxvkDescriptorWrite descriptorWrite;
    descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptorWrite.buffer = m_gpuBuffer->getSliceInfo(0u, computeBufferLayout().timestampSize);

    RenderPushConstants pushConstants = { };
    pushConstants.hud = renderer.getPushConstants();
    pushConstants.x = graphPos.x;
    pushConstants.y = graphPos.y;
    pushConstants.w = graphSize.x;
    pushConstants.h = graphSize.y;
    pushConstants.frameIndex = dataPoint;

    ctx->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer,
      VK_PIPELINE_BIND_POINT_GRAPHICS, getPipeline(renderer, key));

    ctx->bindResources(DxvkCmdBuffer::ExecBuffer,
      m_gfxPipelineLayout, 1u, &descriptorWrite,
      sizeof(pushConstants), &pushConstants);

    ctx->cmdDraw(4, 1, 0, 0);

    ctx->track(m_gpuBuffer, DxvkAccess::Read);
  }


  void HudFrameTimeItem::createResources(
    const Rc<DxvkCommandList>&ctx) {
    auto bufferLayout = computeBufferLayout();

    DxvkBufferCreateInfo bufferInfo = { };
    bufferInfo.size = bufferLayout.totalSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT
                     | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                     | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
                     | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                     | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT
                     | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
    bufferInfo.stages = VK_PIPELINE_STAGE_TRANSFER_BIT
                      | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT
                      | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                      | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT
                      | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    bufferInfo.access = VK_ACCESS_TRANSFER_READ_BIT
                      | VK_ACCESS_TRANSFER_WRITE_BIT
                      | VK_ACCESS_INDIRECT_COMMAND_READ_BIT
                      | VK_ACCESS_SHADER_READ_BIT
                      | VK_ACCESS_SHADER_WRITE_BIT;
    bufferInfo.debugName = "HUD frame time data";

    m_gpuBuffer = m_device->createBuffer(bufferInfo, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);

    DxvkBufferViewKey textViewInfo = { };
    textViewInfo.format = VK_FORMAT_R8_UINT;
    textViewInfo.offset = bufferLayout.textOffset;
    textViewInfo.size = bufferLayout.textSize;
    textViewInfo.usage = VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
    m_textWrView = m_gpuBuffer->createView(textViewInfo);

    textViewInfo.usage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
    m_textRdView = m_gpuBuffer->createView(textViewInfo);

    // Zero-init buffer so we don't display random garbage at the start
    DxvkResourceBufferInfo bufferSlice = m_gpuBuffer->getSliceInfo();

    ctx->cmdFillBuffer(DxvkCmdBuffer::InitBuffer,
      bufferSlice.buffer, bufferSlice.offset, bufferSlice.size, 0u);

    VkMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    barrier.dstAccessMask = m_gpuBuffer->info().access;
    barrier.dstStageMask = m_gpuBuffer->info().stages;

    VkDependencyInfo depInfo = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    depInfo.memoryBarrierCount = 1u;
    depInfo.pMemoryBarriers = &barrier;

    ctx->cmdPipelineBarrier(DxvkCmdBuffer::InitBuffer, &depInfo);
    ctx->track(m_gpuBuffer, DxvkAccess::Write);

    m_query = m_device->createRawQuery(VK_QUERY_TYPE_TIMESTAMP);
  }


  void HudFrameTimeItem::createComputePipeline(
          HudVulkanRenderer&  renderer) {
    static const std::array<DxvkDescriptorSetLayoutBinding, 4> bindings = {{
      { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,       1, VK_SHADER_STAGE_COMPUTE_BIT },
      { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,       1, VK_SHADER_STAGE_COMPUTE_BIT },
      { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,       1, VK_SHADER_STAGE_COMPUTE_BIT },
      { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
    }};

    m_computePipelineLayout = m_device->createBuiltInPipelineLayout(0u,
      VK_SHADER_STAGE_COMPUTE_BIT, sizeof(ComputePushConstants),
      bindings.size(), bindings.data());

    m_computePipeline = m_device->createBuiltInComputePipeline(m_computePipelineLayout,
      util::DxvkBuiltInShaderStage(hud_frame_time_eval, nullptr));
  }


  const DxvkPipelineLayout* HudFrameTimeItem::createPipelineLayout() {
    static const std::array<DxvkDescriptorSetLayoutBinding, 1> bindings = {{
      { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT },
    }};

    return m_device->createBuiltInPipelineLayout(0u,
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
      sizeof(RenderPushConstants), bindings.size(), bindings.data());
  }


  VkPipeline HudFrameTimeItem::getPipeline(
          HudVulkanRenderer&  renderer,
    const HudPipelineKey&     key) {
    auto entry = m_gfxPipelines.find(key);

    if (entry != m_gfxPipelines.end())
      return entry->second;

    VkPipeline pipeline = createPipeline(renderer, key);
    m_gfxPipelines.insert({ key, pipeline });
    return pipeline;
  }


  VkPipeline HudFrameTimeItem::createPipeline(
          HudVulkanRenderer&  renderer,
    const HudPipelineKey&     key) {
    auto vk = m_device->vkd();

    HudSpecConstants specConstants = renderer.getSpecConstants(key);
    VkSpecializationInfo specInfo = renderer.getSpecInfo(&specConstants);

    VkPipelineInputAssemblyStateCreateInfo iaState = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    iaState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

    VkPipelineColorBlendAttachmentState cbAttachment = { };
    cbAttachment.blendEnable = VK_TRUE;
    cbAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cbAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cbAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    cbAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cbAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cbAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    cbAttachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    util::DxvkBuiltInGraphicsState state = { };
    state.vs = util::DxvkBuiltInShaderStage(hud_graph_vert, nullptr);
    state.fs = util::DxvkBuiltInShaderStage(hud_graph_frag, &specInfo);
    state.iaState = &iaState;
    state.colorFormats[0] = key.format;
    state.cbAttachment = &cbAttachment;

    return m_device->createBuiltInGraphicsPipeline(m_gfxPipelineLayout, state);
  }


  HudFrameTimeItem::BufferLayout HudFrameTimeItem::computeBufferLayout() {
    struct ComputeTimestampBuffer {
      std::array<uint64_t, 2u> timestamps;
      std::array<float, NumDataPoints> intervals;
      float avgMs;
      float minMs;
      float maxMs;
    };

    BufferLayout result = { };
    result.timestampSize = align(sizeof(ComputeTimestampBuffer), 256u);
    result.drawInfoOffset = result.timestampSize;
    result.drawInfoSize = align(sizeof(HudTextDrawInfo) * NumTextDraws, 256u);
    result.drawParamOffset = result.drawInfoOffset + result.drawInfoSize;
    result.drawParamSize = align(sizeof(VkDrawIndirectCommand) * NumTextDraws, 256u);
    result.textOffset = result.drawParamOffset + result.drawParamSize;
    result.textSize = 256u;
    result.totalSize = result.textOffset + result.textSize;
    return result;
  }




  HudSubmissionStatsItem::HudSubmissionStatsItem(const Rc<DxvkDevice>& device)
  : m_device(device) {

  }


  HudSubmissionStatsItem::~HudSubmissionStatsItem() {

  }


  void HudSubmissionStatsItem::update(dxvk::high_resolution_clock::time_point time) {
    DxvkStatCounters counters = m_device->getStatCounters();
    
    uint64_t currSubmitCount = counters.getCtr(DxvkStatCounter::QueueSubmitCount);
    uint64_t currSyncCount = counters.getCtr(DxvkStatCounter::GpuSyncCount);
    uint64_t currSyncTicks = counters.getCtr(DxvkStatCounter::GpuSyncTicks);

    m_maxSubmitCount = std::max(m_maxSubmitCount, currSubmitCount - m_prevSubmitCount);
    m_maxSyncCount = std::max(m_maxSyncCount, currSyncCount - m_prevSyncCount);
    m_maxSyncTicks = std::max(m_maxSyncTicks, currSyncTicks - m_prevSyncTicks);

    m_prevSubmitCount = currSubmitCount;
    m_prevSyncCount = currSyncCount;
    m_prevSyncTicks = currSyncTicks;

    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(time - m_lastUpdate);

    if (elapsed.count() >= UpdateInterval) {
      m_submitString = str::format(m_maxSubmitCount);

      uint64_t syncTicks = m_maxSyncTicks / 100;

      m_syncString = m_maxSyncCount
        ? str::format(m_maxSyncCount, " (", (syncTicks / 10), ".", (syncTicks % 10), " ms)")
        : str::format(m_maxSyncCount);

      m_maxSubmitCount = 0;
      m_maxSyncCount = 0;
      m_maxSyncTicks = 0;

      m_lastUpdate = time;
    }
  }


  HudPos HudSubmissionStatsItem::render(
    const Rc<DxvkCommandList>&ctx,
    const HudPipelineKey&     key,
    const HudOptions&         options,
          HudRenderer&        renderer,
          HudPos              position) {
    position.y += 16;
    renderer.drawText(16, position, 0xff4080ff, "Queue submissions:");
    renderer.drawText(16, { position.x + 228, position.y }, 0xffffffffu, m_submitString);

    position.y += 20;
    renderer.drawText(16, position, 0xff4080ff, "Queue syncs:");
    renderer.drawText(16, { position.x + 228, position.y }, 0xffffffffu, m_syncString);

    position.y += 8;
    return position;
  }


  HudDrawCallStatsItem::HudDrawCallStatsItem(const Rc<DxvkDevice>& device)
  : m_device(device) {

  }


  HudDrawCallStatsItem::~HudDrawCallStatsItem() {

  }


  void HudDrawCallStatsItem::update(dxvk::high_resolution_clock::time_point time) {
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(time - m_lastUpdate);

    DxvkStatCounters counters = m_device->getStatCounters();
    auto diffCounters = counters.diff(m_prevCounters);

    if (elapsed.count() >= UpdateInterval) {
      m_drawCallCount   = diffCounters.getCtr(DxvkStatCounter::CmdDrawCalls);
      m_drawCount       = diffCounters.getCtr(DxvkStatCounter::CmdDrawsMerged) + m_drawCallCount;
      m_dispatchCount   = diffCounters.getCtr(DxvkStatCounter::CmdDispatchCalls);
      m_renderPassCount = diffCounters.getCtr(DxvkStatCounter::CmdRenderPassCount);
      m_barrierCount    = diffCounters.getCtr(DxvkStatCounter::CmdBarrierCount);

      m_lastUpdate = time;
    }

    m_prevCounters = counters;
  }


  HudPos HudDrawCallStatsItem::render(
    const Rc<DxvkCommandList>&ctx,
    const HudPipelineKey&     key,
    const HudOptions&         options,
          HudRenderer&        renderer,
          HudPos              position) {
    std::string drawCount = m_drawCount > m_drawCallCount
      ? str::format(m_drawCallCount, " (", m_drawCount, ")")
      : str::format(m_drawCallCount);

    position.y += 16;
    renderer.drawText(16, position, 0xffff8040, "Draw calls:");
    renderer.drawText(16, { position.x + 192, position.y }, 0xffffffffu, drawCount);
    
    position.y += 20;
    renderer.drawText(16, position, 0xffff8040, "Dispatch calls:");
    renderer.drawText(16, { position.x + 192, position.y }, 0xffffffffu, str::format(m_dispatchCount));
    
    position.y += 20;
    renderer.drawText(16, position, 0xffff8040, "Render passes:");
    renderer.drawText(16, { position.x + 192, position.y }, 0xffffffffu, str::format(m_renderPassCount));
    
    position.y += 20;
    renderer.drawText(16, position, 0xffff8040, "Barriers:");
    renderer.drawText(16, { position.x + 192, position.y }, 0xffffffffu, str::format(m_barrierCount));
    
    position.y += 8;
    return position;
  }


  HudPipelineStatsItem::HudPipelineStatsItem(const Rc<DxvkDevice>& device)
  : m_device(device) {

  }


  HudPipelineStatsItem::~HudPipelineStatsItem() {

  }


  void HudPipelineStatsItem::update(dxvk::high_resolution_clock::time_point time) {
    DxvkStatCounters counters = m_device->getStatCounters();

    m_graphicsPipelines = counters.getCtr(DxvkStatCounter::PipeCountGraphics);
    m_graphicsLibraries = counters.getCtr(DxvkStatCounter::PipeCountLibrary);
    m_computePipelines  = counters.getCtr(DxvkStatCounter::PipeCountCompute);
  }


  HudPos HudPipelineStatsItem::render(
    const Rc<DxvkCommandList>&ctx,
    const HudPipelineKey&     key,
    const HudOptions&         options,
          HudRenderer&        renderer,
          HudPos              position) {
    position.y += 16;
    renderer.drawText(16, position, 0xffff40ff, "Graphics pipelines:");
    renderer.drawText(16, { position.x + 240, position.y }, 0xffffffffu, str::format(m_graphicsPipelines));

    if (m_graphicsLibraries) {
      position.y += 20;
      renderer.drawText(16, position, 0xffff40ff, "Graphics shaders:");
      renderer.drawText(16, { position.x + 240, position.y }, 0xffffffffu, str::format(m_graphicsLibraries));
    }

    position.y += 20;
    renderer.drawText(16, position, 0xffff40ff, "Compute shaders:");
    renderer.drawText(16, { position.x + 240, position.y }, 0xffffffffu, str::format(m_computePipelines));

    position.y += 8;
    return position;
  }


  HudDescriptorStatsItem::HudDescriptorStatsItem(const Rc<DxvkDevice>& device)
  : m_device(device) {

  }


  HudDescriptorStatsItem::~HudDescriptorStatsItem() {

  }


  void HudDescriptorStatsItem::update(dxvk::high_resolution_clock::time_point time) {
    uint64_t ticks = std::chrono::duration_cast<std::chrono::microseconds>(time - m_lastUpdate).count();

    DxvkStatCounters counters = m_device->getStatCounters();

    if (ticks >= UpdateInterval) {
      uint64_t busyTicks = counters.getCtr(DxvkStatCounter::DescriptorCopyBusyTicks);

      m_copyThreadLoad = uint32_t(double(100.0 * (busyTicks - m_copyThreadBusyTicks)) / ticks);
      m_copyThreadBusyTicks = busyTicks;

      m_descriptorSetCountDisplay = m_descriptorSetCountMax;
      m_descriptorSetCountMax = 0u;

      m_descriptorHeapUsed = m_descriptorHeapMax;
      m_descriptorHeapMax = 0u;

      m_lastUpdate = time;
    }

    auto descriptorSetCount  = counters.getCtr(DxvkStatCounter::DescriptorSetCount);
    m_descriptorPoolCount = counters.getCtr(DxvkStatCounter::DescriptorPoolCount);

    m_descriptorHeapCount = counters.getCtr(DxvkStatCounter::DescriptorHeapCount);
    m_descriptorHeapAlloc = counters.getCtr(DxvkStatCounter::DescriptorHeapSize);

    m_descriptorSetCountMax = std::max(m_descriptorSetCountMax, descriptorSetCount - m_descriptorSetCount);
    m_descriptorSetCount = descriptorSetCount;

    auto descriptorHeapUsed = counters.getCtr(DxvkStatCounter::DescriptorHeapUsed);
    m_descriptorHeapMax = std::max(descriptorHeapUsed - m_descriptorHeapPrev, m_descriptorHeapMax);
    m_descriptorHeapPrev = descriptorHeapUsed;
  }


  HudPos HudDescriptorStatsItem::render(
    const Rc<DxvkCommandList>&ctx,
    const HudPipelineKey&     key,
    const HudOptions&         options,
          HudRenderer&        renderer,
          HudPos              position) {
    if (m_descriptorPoolCount) {
      position.y += 16;
      renderer.drawText(16, position, 0xff8040ff, "Descriptor pools:");
      renderer.drawText(16, { position.x + 216, position.y }, 0xffffffffu, str::format(m_descriptorPoolCount));

      position.y += 20;
      renderer.drawText(16, position, 0xff8040ff, "Descriptor sets:");
      renderer.drawText(16, { position.x + 216, position.y }, 0xffffffffu, str::format(m_descriptorSetCountDisplay));
    }

    if (m_descriptorHeapAlloc) {
      position.y += 16;
      renderer.drawText(16, position, 0xff8040ff, "Descriptor heaps:");
      renderer.drawText(16, { position.x + 216, position.y }, 0xffffffffu, str::format(m_descriptorHeapCount, " (", m_descriptorHeapAlloc >> 20, " MB)"));

      position.y += 20;
      renderer.drawText(16, position, 0xff8040ff, "Descriptor usage:");
      renderer.drawText(16, { position.x + 216, position.y }, 0xffffffffu, str::format(m_descriptorHeapUsed >> 10, " kB"));

      position.y += 20;
      renderer.drawText(16, position, 0xff8040ff, "Copy worker:");
      renderer.drawText(16, { position.x + 216, position.y }, 0xffffffffu, str::format(m_copyThreadLoad, "%"));
    }

    position.y += 8;
    return position;
  }


  HudMemoryStatsItem::HudMemoryStatsItem(const Rc<DxvkDevice>& device)
  : m_device(device), m_memory(device->adapter()->memoryProperties()) {

  }


  HudMemoryStatsItem::~HudMemoryStatsItem() {

  }


  void HudMemoryStatsItem::update(dxvk::high_resolution_clock::time_point time) {
    for (uint32_t i = 0; i < m_memory.memoryHeapCount; i++)
      m_heaps[i] = m_device->getMemoryStats(i);
  }


  HudPos HudMemoryStatsItem::render(
    const Rc<DxvkCommandList>&ctx,
    const HudPipelineKey&     key,
    const HudOptions&         options,
          HudRenderer&        renderer,
          HudPos              position) {
    for (uint32_t i = 0; i < m_memory.memoryHeapCount; i++) {
      bool isDeviceLocal = m_memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;

      uint64_t memUsedMib = m_heaps[i].memoryUsed >> 20;
      uint64_t memAllocatedMib = m_heaps[i].memoryAllocated >> 20;
      uint64_t percentage = m_heaps[i].memoryBudget
        ? (100u * m_heaps[i].memoryAllocated) / m_heaps[i].memoryBudget
        : 0u;

      std::string label = str::format(isDeviceLocal ? "Vidmem" : "Sysmem", " heap ", i, ": ");
      std::string text  = str::format(std::setfill(' '), std::setw(5), memAllocatedMib, " MB (", percentage, "%) ",
        std::setw(5 + (percentage < 10 ? 1 : 0) + (percentage < 100 ? 1 : 0)), memUsedMib, " MB used");

      position.y += 16;
      renderer.drawText(16, position, 0xff40ffffu, label);
      renderer.drawText(16, { position.x + 168, position.y }, 0xffffffffu, text);

      position.y += 4;
    }

    position.y += 4;
    return position;
  }




  HudMemoryDetailsItem::HudMemoryDetailsItem(
    const Rc<DxvkDevice>&     device,
          HudVulkanRenderer*  renderer)
  : m_device          (device),
    m_renderer        (renderer),
    m_pipelineLayout  (createPipelineLayout()) {

  }


  HudMemoryDetailsItem::~HudMemoryDetailsItem() {
    auto vk = m_device->vkd();

    for (const auto& p : m_pipelines) {
      vk->vkDestroyPipeline(vk->device(), p.second.background, nullptr);
      vk->vkDestroyPipeline(vk->device(), p.second.visualize, nullptr);
    }
  }


  void HudMemoryDetailsItem::update(dxvk::high_resolution_clock::time_point time) {
    uint64_t ticks = std::chrono::duration_cast<std::chrono::microseconds>(time - m_lastUpdate).count();

    if (ticks >= UpdateInterval) {
      m_cacheStats = m_device->getMemoryAllocationStats(m_stats);
      m_displayCacheStats |= m_cacheStats.requestCount != 0u;

      m_lastUpdate = time;
    }
  }


  HudPos HudMemoryDetailsItem::render(
    const Rc<DxvkCommandList>&ctx,
    const HudPipelineKey&     key,
    const HudOptions&         options,
          HudRenderer&        renderer,
          HudPos              position) {
    // Layout, align the entire element to the bottom right.
    int32_t x = -564;
    int32_t y = -20;

    if (m_displayCacheStats) {
      uint32_t hitCount = m_cacheStats.requestCount - m_cacheStats.missCount;
      uint32_t hitRate = (100 * hitCount) / std::max(m_cacheStats.requestCount, 1u);

      std::string cacheStr = str::format("Cache: ", m_cacheStats.size >> 10, " kB (", hitRate, "% hit)");
      renderer.drawText(14, { x, y }, 0xffffffffu, cacheStr);

      y -= 24;
    }

    for (uint32_t i = m_stats.memoryTypes.size(); i; i--) {
      const auto& type = m_stats.memoryTypes.at(i - 1);

      if (!type.chunkCount)
        continue;

      // Compute layout and gather memory stats
      DxvkMemoryStats stats = { };

      int32_t w = 0;
      int32_t h = 0;

      for (uint32_t j = 0; j < type.chunkCount; j++) {
        const auto& chunk = m_stats.chunks.at(type.chunkIndex + j);
        stats.memoryAllocated += chunk.capacity;
        stats.memoryUsed += chunk.used;

        int32_t chunkWidth = (chunk.pageCount + 15u) / 16u + 2;

        bool separate = j && m_stats.chunks.at(type.chunkIndex + j - 1u).mapped != chunk.mapped;

        if (x + w + chunkWidth > -8 || separate) {
          w = 0;
          h += 34;
        }

        w += chunkWidth + 6;
      }

      if (w)
        h += 34;

      y -= h;

      w = 0;
      h = 8;

      // Draw individual chunks
      for (uint32_t j = 0; j < type.chunkCount; j++) {
        const auto& chunk = m_stats.chunks.at(type.chunkIndex + j);

        // Default VRAM, blue
        uint32_t color = 0xff804020u;

        if (type.properties.propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) {
          // Cached memory, green
          color = 0xff208020u;
        } else if (!(type.properties.propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
          if (!chunk.mapped) {
            // Fallback allocation, grey
            color = 0xff201c18u;
          } else {
            // Uncached memory, red
            color = 0xff202080u;
          }
        } else if (chunk.mapped) {
          // Host-visible VRAM, yellow
          color = 0xff208080u;
        }

        int32_t chunkWidth = (chunk.pageCount + 15u) / 16u + 2;

        bool separate = j && m_stats.chunks.at(type.chunkIndex + j - 1u).mapped != chunk.mapped;

        if (x + w + chunkWidth > -8 || separate) {
          w = 0;
          h += 34;
        }

        drawChunk({ x + w, y + h }, { chunkWidth, 24 }, color, chunk);

        w += chunkWidth + 6;
      }

      // Render descriptive text
      std::string headline = str::format("Mem type ", (i - 1), " [", type.properties.heapIndex, "]: ",
        type.chunkCount, " chunk", type.chunkCount != 1u ? "s" : "", " (", (stats.memoryAllocated >> 20u), " MB, ",
        ((stats.memoryUsed >= (1u << 20u)) ? stats.memoryUsed >> 20 : stats.memoryUsed >> 10),
        (stats.memoryUsed >= (1u << 20u) ? " MB" : " kB"), " used)");

      renderer.drawText(14, { x, y }, 0xffffffffu, headline);

      y -= 24;
    }

    flushDraws(ctx, key, options, *m_renderer);
    return position;
  }


  void HudMemoryDetailsItem::drawChunk(
          HudPos            pos,
          HudPos            size,
          uint32_t          color,
    const DxvkMemoryChunkStats& chunk) {
    auto& draw = m_drawInfos.emplace_back();
    draw.x = pos.x;
    draw.y = pos.y;
    draw.w = size.x;
    draw.h = size.y;
    draw.pageMask = chunk.pageMaskOffset;
    draw.pageCountAndActiveBit = chunk.pageCount;
    draw.color = color;

    if (chunk.active)
      draw.pageCountAndActiveBit |= 1u << 15;
  }


  void HudMemoryDetailsItem::flushDraws(
    const Rc<DxvkCommandList>&ctx,
    const HudPipelineKey&     key,
    const HudOptions&         options,
          HudVulkanRenderer&  renderer) {
    if (m_drawInfos.empty())
      return;

    PipelinePair pipelines = getPipeline(renderer, key);

    // Update relevant buffers
    DxvkResourceBufferInfo drawDescriptor = { };
    DxvkResourceBufferInfo dataDescriptor = { };

    updateDataBuffer(ctx, drawDescriptor, dataDescriptor);

    // Bind resources
    std::array<DxvkDescriptorWrite, 2u> descriptors = { };
    descriptors[0u].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptors[0u].buffer = drawDescriptor;

    descriptors[1u].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptors[1u].buffer = dataDescriptor;

    HudPushConstants pushConstants = renderer.getPushConstants();

    ctx->bindResources(DxvkCmdBuffer::ExecBuffer,
      m_pipelineLayout, descriptors.size(), descriptors.data(),
      sizeof(pushConstants), &pushConstants);

    // Draw background first, then the actual usage info. The pipeline
    // layout is the same for both pipelines, so don't rebind resources.
    ctx->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer,
      VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.background);

    ctx->cmdDraw(4, m_drawInfos.size(), 0, 0);

    ctx->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer,
      VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.visualize);
    ctx->cmdDraw(4, m_drawInfos.size(), 0, 0);

    // Track data buffer lifetime
    ctx->track(m_dataBuffer, DxvkAccess::Read);

    m_drawInfos.clear();
  }


  void HudMemoryDetailsItem::updateDataBuffer(
    const Rc<DxvkCommandList>&ctx,
          DxvkResourceBufferInfo& drawDescriptor,
          DxvkResourceBufferInfo& dataDescriptor) {
    size_t drawInfoSize = m_drawInfos.size() * sizeof(DrawInfo);
    size_t drawInfoSizeAligned = align(drawInfoSize, 256u);

    size_t chunkDataSize = m_stats.pageMasks.size() * sizeof(uint32_t);
    size_t chunkDataSizeAligned = align(chunkDataSize, 256u);

    size_t bufferSize = align(drawInfoSizeAligned + chunkDataSizeAligned, 2048u);

    if (!m_dataBuffer || m_dataBuffer->info().size < bufferSize) {
      DxvkBufferCreateInfo bufferInfo;
      bufferInfo.size = bufferSize;
      bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
      bufferInfo.access = VK_ACCESS_SHADER_READ_BIT;
      bufferInfo.stages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
      bufferInfo.debugName = "HUD memory data";

      m_dataBuffer = m_device->createBuffer(bufferInfo,
        VK_MEMORY_HEAP_DEVICE_LOCAL_BIT |
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    } else {
      // Ensure we can update the buffer without overriding live data
      auto allocation = m_dataBuffer->assignStorage(m_dataBuffer->allocateStorage());
      ctx->track(std::move(allocation));
    }

    // Update draw infos and pad unused area with zeroes
    std::memcpy(m_dataBuffer->mapPtr(0), m_drawInfos.data(), drawInfoSize);
    std::memset(m_dataBuffer->mapPtr(drawInfoSize), 0, drawInfoSizeAligned - drawInfoSize);

    // Update chunk data and pad with zeroes
    std::memcpy(m_dataBuffer->mapPtr(drawInfoSizeAligned), m_stats.pageMasks.data(), chunkDataSize);
    std::memset(m_dataBuffer->mapPtr(drawInfoSizeAligned + chunkDataSize), 0, chunkDataSizeAligned - chunkDataSize);

    // Write back descriptors
    drawDescriptor = m_dataBuffer->getSliceInfo(0u, drawInfoSizeAligned);
    dataDescriptor = m_dataBuffer->getSliceInfo(drawInfoSizeAligned, chunkDataSizeAligned);
  }


  const DxvkPipelineLayout* HudMemoryDetailsItem::createPipelineLayout() {
    static const std::array<DxvkDescriptorSetLayoutBinding, 2> bindings = {{
      { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT   },
      { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT },
    }};

    return m_device->createBuiltInPipelineLayout(0u,
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
      sizeof(HudPushConstants), bindings.size(), bindings.data());
  }


  HudMemoryDetailsItem::PipelinePair HudMemoryDetailsItem::createPipeline(
          HudVulkanRenderer&  renderer,
    const HudPipelineKey&     key) {
    auto vk = m_device->vkd();

    HudSpecConstants specConstants = renderer.getSpecConstants(key);
    VkSpecializationInfo specInfo = renderer.getSpecInfo(&specConstants);

    PipelinePair pipelines = { };

    VkPipelineInputAssemblyStateCreateInfo iaState = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    iaState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

    VkPipelineColorBlendAttachmentState cbAttachment = { };
    cbAttachment.blendEnable = VK_TRUE;
    cbAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cbAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cbAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    cbAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cbAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cbAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    cbAttachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    util::DxvkBuiltInGraphicsState state = { };
    state.iaState = &iaState;
    state.colorFormats[0] = key.format;
    state.cbAttachment = &cbAttachment;

    state.vs = util::DxvkBuiltInShaderStage(hud_chunk_vert_background, nullptr);
    state.fs = util::DxvkBuiltInShaderStage(hud_chunk_frag_background, &specInfo);

    pipelines.background = m_device->createBuiltInGraphicsPipeline(m_pipelineLayout, state);

    state.vs = util::DxvkBuiltInShaderStage(hud_chunk_vert_visualize, nullptr);
    state.fs = util::DxvkBuiltInShaderStage(hud_chunk_frag_visualize, &specInfo);

    pipelines.visualize = m_device->createBuiltInGraphicsPipeline(m_pipelineLayout, state);
    return pipelines;
  }


  HudMemoryDetailsItem::PipelinePair HudMemoryDetailsItem::getPipeline(
          HudVulkanRenderer&  renderer,
    const HudPipelineKey&     key) {
    auto entry = m_pipelines.find(key);

    if (entry != m_pipelines.end())
      return entry->second;

    PipelinePair pipeline = createPipeline(renderer, key);
    m_pipelines.insert({ key, pipeline });
    return pipeline;
  }




  HudCsThreadItem::HudCsThreadItem(const Rc<DxvkDevice>& device)
  : m_device(device) {

  }


  HudCsThreadItem::~HudCsThreadItem() {

  }


  void HudCsThreadItem::update(dxvk::high_resolution_clock::time_point time) {
    uint64_t ticks = std::chrono::duration_cast<std::chrono::microseconds>(time - m_lastUpdate).count();

    // Capture the maximum here since it's more useful to
    // identify stutters than using any sort of average
    DxvkStatCounters counters = m_device->getStatCounters();
    uint64_t currCsSyncCount = counters.getCtr(DxvkStatCounter::CsSyncCount);
    uint64_t currCsSyncTicks = counters.getCtr(DxvkStatCounter::CsSyncTicks);

    m_maxCsSyncCount = std::max(m_maxCsSyncCount, currCsSyncCount - m_prevCsSyncCount);
    m_maxCsSyncTicks = std::max(m_maxCsSyncTicks, currCsSyncTicks - m_prevCsSyncTicks);

    m_prevCsSyncCount = currCsSyncCount;
    m_prevCsSyncTicks = currCsSyncTicks;

    m_updateCount++;

    if (ticks >= UpdateInterval) {
      uint64_t currCsChunks = counters.getCtr(DxvkStatCounter::CsChunkCount);
      uint64_t diffCsChunks = (currCsChunks - m_prevCsChunks) / m_updateCount;
      m_prevCsChunks = currCsChunks;

      uint64_t syncTicks = m_maxCsSyncTicks / 100;

      m_csChunkString = str::format(diffCsChunks);
      m_csSyncString = m_maxCsSyncCount
        ? str::format(m_maxCsSyncCount, " (", (syncTicks / 10), ".", (syncTicks % 10), " ms)")
        : str::format(m_maxCsSyncCount);

      uint64_t currCsIdleTicks = counters.getCtr(DxvkStatCounter::CsIdleTicks);

      m_diffCsIdleTicks = currCsIdleTicks - m_prevCsIdleTicks;
      m_prevCsIdleTicks = currCsIdleTicks;

      uint64_t busyTicks = ticks > m_diffCsIdleTicks
        ? uint64_t(ticks - m_diffCsIdleTicks)
        : uint64_t(0);

      m_csLoadString = str::format((100 * busyTicks) / ticks, "%");

      m_maxCsSyncCount = 0;
      m_maxCsSyncTicks = 0;

      m_updateCount = 0;
      m_lastUpdate = time;
    }
  }


  HudPos HudCsThreadItem::render(
    const Rc<DxvkCommandList>&ctx,
    const HudPipelineKey&     key,
    const HudOptions&         options,
          HudRenderer&        renderer,
          HudPos              position) {
    position.y += 16;
    renderer.drawText(16, position, 0xff40ff40, "CS chunks:");
    renderer.drawText(16, { position.x + 132, position.y }, 0xffffffffu, m_csChunkString);

    position.y += 20;
    renderer.drawText(16, position, 0xff40ff40, "CS syncs:");
    renderer.drawText(16, { position.x + 132, position.y }, 0xffffffffu, m_csSyncString);

    position.y += 20;
    renderer.drawText(16, position, 0xff40ff40, "CS load:");
    renderer.drawText(16, { position.x + 132, position.y }, 0xffffffffu, m_csLoadString);

    position.y += 8;
    return position;
  }


  HudGpuLoadItem::HudGpuLoadItem(const Rc<DxvkDevice>& device)
  : m_device(device) {

  }


  HudGpuLoadItem::~HudGpuLoadItem() {

  }


  void HudGpuLoadItem::update(dxvk::high_resolution_clock::time_point time) {
    uint64_t ticks = std::chrono::duration_cast<std::chrono::microseconds>(time - m_lastUpdate).count();

    if (ticks >= UpdateInterval) {
      DxvkStatCounters counters = m_device->getStatCounters();
      uint64_t currGpuIdleTicks = counters.getCtr(DxvkStatCounter::GpuIdleTicks);

      m_diffGpuIdleTicks = currGpuIdleTicks - m_prevGpuIdleTicks;
      m_prevGpuIdleTicks = currGpuIdleTicks;

      uint64_t busyTicks = ticks > m_diffGpuIdleTicks
        ? uint64_t(ticks - m_diffGpuIdleTicks)
        : uint64_t(0);

      m_gpuLoadString = str::format((100 * busyTicks) / ticks, "%");
      m_lastUpdate = time;
    }
  }


  HudPos HudGpuLoadItem::render(
    const Rc<DxvkCommandList>&ctx,
    const HudPipelineKey&     key,
    const HudOptions&         options,
          HudRenderer&        renderer,
          HudPos              position) {
    position.y += 16;
    renderer.drawText(16, position, 0xff408040u, "GPU:");
    renderer.drawText(16, { position.x + 60, position.y }, 0xffffffffu, m_gpuLoadString);

    position.y += 8;
    return position;
  }


  HudCompilerActivityItem::HudCompilerActivityItem(const Rc<DxvkDevice>& device)
  : m_device(device) {

  }


  HudCompilerActivityItem::~HudCompilerActivityItem() {

  }


  void HudCompilerActivityItem::update(dxvk::high_resolution_clock::time_point time) {
    DxvkStatCounters counters = m_device->getStatCounters();

    m_tasksDone = counters.getCtr(DxvkStatCounter::PipeTasksDone);
    m_tasksTotal = counters.getCtr(DxvkStatCounter::PipeTasksTotal);

    bool doShow = m_tasksDone < m_tasksTotal;

    if (!doShow)
      m_timeDone = time;

    if (!m_show) {
      m_timeShown = time;
      m_showPercentage = false;
    } else {
      auto durationShown = std::chrono::duration_cast<std::chrono::milliseconds>(time - m_timeShown);
      auto durationWorking = std::chrono::duration_cast<std::chrono::milliseconds>(time - m_timeDone);

      if (!doShow) {
        m_offset = m_tasksTotal;

        // Ensure the item stays up long enough to be legible
        doShow = durationShown.count() <= MinShowDuration;
      }

      if (!m_showPercentage) {
        // Don't show percentage if it's just going to be stuck at 99%
        // because the workers are not being fed tasks fast enough
        m_showPercentage = durationWorking.count() >= (MinShowDuration / 5)
                        && (computePercentage() < 50);
      }
    }

    m_show = doShow;
  }


  HudPos HudCompilerActivityItem::render(
    const Rc<DxvkCommandList>&ctx,
    const HudPipelineKey&     key,
    const HudOptions&         options,
          HudRenderer&        renderer,
          HudPos              position) {
    if (m_show) {
      std::string string = "Compiling shaders...";

      if (m_showPercentage)
        string = str::format(string, " (", computePercentage(), "%)");

      renderer.drawText(16, { position.x, -20 }, 0xffffffffu, string);
    }

    return position;
  }


  uint32_t HudCompilerActivityItem::computePercentage() const {
    if (m_offset == m_tasksTotal)
      return 100;

    return (uint32_t(m_tasksDone - m_offset) * 100)
         / (uint32_t(m_tasksTotal - m_offset));
  }



  HudLatencyItem::HudLatencyItem() {

  }


  HudLatencyItem::~HudLatencyItem() {

  }


  void HudLatencyItem::accumulateStats(const DxvkLatencyStats& stats) {
    std::lock_guard lock(m_mutex);

    if (stats.frameLatency.count()) {
      m_accumStats.frameLatency += stats.frameLatency;
      m_accumStats.sleepDuration += stats.sleepDuration;

      m_accumFrames += 1u;
    } else {
      m_accumStats = { };
      m_accumFrames = 0u;
    }
  }


  void HudLatencyItem::update(dxvk::high_resolution_clock::time_point time) {
    uint64_t ticks = std::chrono::duration_cast<std::chrono::microseconds>(time - m_lastUpdate).count();

    if (ticks >= UpdateInterval) {
      std::lock_guard lock(m_mutex);

      if (m_accumFrames) {
        uint32_t latency = (m_accumStats.frameLatency / m_accumFrames).count() / 100u;
        uint32_t sleep = (m_accumStats.sleepDuration / m_accumFrames).count() / 100u;

        m_latencyString = str::format(latency / 10, ".", latency % 10, " ms");
        m_sleepString = str::format(sleep / 10, ".", sleep % 10, " ms");

        m_accumStats = { };
        m_accumFrames = 0u;

        m_invalidUpdates = 0u;
      } else {
        m_latencyString = "--";
        m_sleepString = "--";

        if (m_invalidUpdates < MaxInvalidUpdates)
          m_invalidUpdates += 1u;
      }

      m_lastUpdate = time;
    }
  }


  HudPos HudLatencyItem::render(
    const Rc<DxvkCommandList>&ctx,
    const HudPipelineKey&     key,
    const HudOptions&         options,
          HudRenderer&        renderer,
          HudPos              position) {
    if (m_invalidUpdates >= MaxInvalidUpdates)
      return position;

    position.y += 16;

    renderer.drawText(16, position, 0xffff60a0u, "Latency: ");
    renderer.drawText(16, { position.x + 108, position.y }, 0xffffffffu, m_latencyString);

    position.y += 20;

    renderer.drawText(16, position, 0xffff60a0u, "Sleep: ");
    renderer.drawText(16, { position.x + 108, position.y }, 0xffffffffu, m_sleepString);

    position.y += 8;
    return position;
  }

}
