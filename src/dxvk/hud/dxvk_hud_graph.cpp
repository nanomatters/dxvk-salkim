#include "dxvk_hud_graph.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dxvk::hud {

  namespace {
    float bounded(float value, float fallback, float lo, float hi) {
      return std::isfinite(value) ? std::clamp(value, lo, hi) : fallback;
    }

    std::string formatGraphValue(double value) {
      char text[32];
      std::snprintf(text, sizeof(text), value < 10000.0 ? "%.1f" : "%.2g", value);
      return text;
    }
  }

  HudGraph::HudGraph(std::string label, std::string unit, uint32_t color,
    const HudGraphOptions& options)
  : m_label(std::move(label)), m_unit(std::move(unit)), m_color(color),
    m_width(int32_t(bounded(options.width, 280.0f, 180.0f, 640.0f))),
    m_height(int32_t(bounded(options.height, 48.0f, 24.0f, 160.0f))),
    m_bucketDurationNs(int64_t(bounded(options.history, 3.0f, 1.0f, 30.0f)
      * 1'000'000'000.0 / BucketCount)),
    m_maximum(bounded(options.maximum, 0.0f, 0.0f, 1'000'000.0f)) {
    if (m_maximum > 0.0f) {
      m_rangeMaximum = m_maximum;
      m_rangeText = "0-" + formatGraphValue(m_rangeMaximum);
    }
  }


  void HudGraph::addSample(int64_t timeNs, float value) {
    if (timeNs < 0 || !std::isfinite(value) || value < 0.0f)
      return;

    advance(timeNs);
    int64_t index = timeNs / m_bucketDurationNs;
    if (index <= m_timeNs / m_bucketDurationNs - int64_t(BucketCount))
      return;

    auto& bucket = m_buckets[index % BucketCount];
    if (bucket.index != index) {
      bucket = { };
      bucket.index = index;
    }
    if (bucket.lastTimeNs < 0) {
      bucket.minimum = bucket.maximum = bucket.last = value;
      bucket.lastTimeNs = timeNs;
    } else {
      bucket.minimum = std::min(bucket.minimum, value);
      bucket.maximum = std::max(bucket.maximum, value);
      if (timeNs >= bucket.lastTimeNs) {
        bucket.last = value;
        bucket.lastTimeNs = timeNs;
      }
    }

    if (timeNs >= m_lastTimeNs) {
      m_lastTimeNs = timeNs;
      if (m_lastTextUpdateNs < 0 || timeNs - m_lastTextUpdateNs >= 500'000'000) {
        m_valueText = formatGraphValue(value) + m_unit;
        m_lastTextUpdateNs = timeNs;
      }
    }

    // Grow immediately if needed, but check for shrinkage only four times per
    // second. Fixed ranges need neither the history scan nor axis formatting.
    if (!m_maximum && (m_lastRangeUpdateNs < 0 || value > m_rangeMaximum ||
        m_timeNs - m_lastRangeUpdateNs >= 250'000'000))
      updateRange();
  }


  void HudGraph::addGap(int64_t timeNs) {
    if (timeNs < 0)
      return;
    advance(timeNs);
    int64_t index = timeNs / m_bucketDurationNs;
    if (index <= m_timeNs / m_bucketDurationNs - int64_t(BucketCount))
      return;

    auto& bucket = m_buckets[index % BucketCount];
    if (bucket.index != index) {
      bucket = { };
      bucket.index = index;
    }
    bucket.missing = true;

    if (timeNs >= m_lastTimeNs) {
      m_lastTimeNs = timeNs;
      m_lastTextUpdateNs = -1;
      m_valueText = "--";
    }
  }


  void HudGraph::advance(int64_t timeNs) {
    m_timeNs = std::max(m_timeNs, timeNs);
    if (m_lastTimeNs >= 0 && m_lastTimeNs / m_bucketDurationNs <=
        m_timeNs / m_bucketDurationNs - int64_t(BucketCount)) {
      m_valueText = "--";
      m_lastTextUpdateNs = -1;
    }
  }


  void HudGraph::clear() {
    m_buckets = { };
    m_timeNs = m_lastTimeNs = m_lastRangeUpdateNs = m_lastTextUpdateNs = -1;
    m_valueText = "--";
    m_rangeMaximum = m_maximum;
    m_rangeText = "0-" + formatGraphValue(m_maximum > 0.0f ? m_maximum : 1.0f);
  }


  HudPos HudGraph::size() const {
    return { m_width, int32_t(HudFontSize) + 6 + m_height };
  }


  void HudGraph::updateRange() {
    double maximum = 1.0;
    int64_t newest = m_timeNs / m_bucketDurationNs;

    for (const auto& bucket : m_buckets) {
      if (bucket.lastTimeNs >= 0 && bucket.index > newest - int64_t(BucketCount))
        maximum = std::max(maximum, double(bucket.maximum));
    }

    // Use fine steps (10 FPS in the hundreds), not the coarse 1/2/5 ladder.
    // Keep headroom when growing and hysteresis when shrinking to avoid jitter.
    double step = std::pow(10.0, std::floor(std::log10(maximum)) - 1.0);
    double target = std::ceil(maximum * 1.05 / step) * step;
    if (maximum > m_rangeMaximum || target < m_rangeMaximum * 0.75) {
      m_rangeMaximum = target;
      m_rangeText = "0-" + formatGraphValue(m_rangeMaximum);
    }
    m_lastRangeUpdateNs = m_timeNs;
  }


  void HudGraph::render(HudRenderer& renderer, HudPos position) const {
    double maximum = m_rangeMaximum > 0.0 ? m_rangeMaximum : 1.0;
    renderer.drawText(HudSmallFontSize, position, m_color, m_label + ": " + m_valueText);

    renderer.drawText(HudSmallFontSize,
      { position.x + m_width - int32_t(renderer.textWidth(HudSmallFontSize, m_rangeText)), position.y },
      0xffccccccu, m_rangeText);

    int32_t top = position.y + 6;
    for (int32_t y : { top, top + m_height / 2, top + m_height - 1 })
      renderer.drawRect({ position.x, y }, { m_width, 1 }, 0x50808080u);

    if (m_lastTimeNs < 0)
      return;

    double pixelsPerUnit = (m_height - 1) / maximum;
    auto mapY = [&] (float v) {
      return top + int32_t((m_height - 1) - std::clamp(double(v) * pixelsPerUnit,
        0.0, double(m_height - 1)) + 0.5);
    };

    int64_t newest = m_timeNs / m_bucketDurationNs;
    size_t previousColumn = BucketCount;
    float previous = 0.0f;
    HudPos runPosition = {}, runSize = {};

    auto flushRun = [&] {
      if (runSize.x)
        renderer.drawRect(runPosition, runSize, m_color);
      runSize = {};
    };

    auto drawColumn = [&] (size_t i, float lo, float hi) {
      int32_t x0 = int32_t(i * m_width / BucketCount);
      int32_t x1 = int32_t((i + 1) * m_width / BucketCount);
      int32_t y0 = mapY(hi), y1 = mapY(lo);
      // Adjacent columns with identical pixel coverage are one rectangle.
      // This also reduces vertices and copies in the D3D12 HUD path.
      if (runSize.x && runPosition.y == y0 && runSize.y == y1 - y0 + 1)
        runSize.x += x1 - x0;
      else {
        flushRun();
        runPosition = { position.x + x0, y0 };
        runSize = { x1 - x0, y1 - y0 + 1 };
      }
    };

    for (size_t i = 0; i < BucketCount; i++) {
      int64_t index = newest - int64_t(BucketCount - 1 - i);
      if (index < 0)
        continue;
      const auto& bucket = m_buckets[index % BucketCount];
      if (bucket.index != index)
        continue;
      // A missing report breaks its whole bucket, even if valid reports share
      // it. Merely empty buckets between measured frames are not missing data.
      if (bucket.missing) {
        flushRun();
        previousColumn = BucketCount;
        continue;
      }

      float lo = bucket.minimum, hi = bucket.maximum;
      if (previousColumn != BucketCount) {
        // Connect existing points across empty time buckets. This is drawing
        // interpolation only. The measured history and its extrema stay intact.
        float start = previous;
        for (size_t j = previousColumn + 1; j < i; j++) {
          float fraction = float(j - previousColumn) / float(i - previousColumn);
          float value = start + (bucket.last - start) * fraction;
          drawColumn(j, std::min(previous, value), std::max(previous, value));
          previous = value;
        }
        lo = std::min(lo, previous);
        hi = std::max(hi, previous);
      }
      drawColumn(i, lo, hi);
      previous = bucket.last;
      previousColumn = i;
    }
    flushRun();
  }


  void HudRenderer::drawGraph(HudPos position, const HudGraph& graph) {
    graph.render(*this, position);
  }

}
