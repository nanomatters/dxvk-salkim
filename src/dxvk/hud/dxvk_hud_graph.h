#pragma once

#include <array>
#include <string>

#include "dxvk_hud_renderer.h"

namespace dxvk::hud {

  struct HudGraphOptions {
    float width = 280.0f;
    float height = 48.0f;
    float history = 3.0f;
    float maximum = 0.0f;
  };

  /**
   * \brief Bounded, renderer-independent time-series graph
   *
   * Callers supply non-negative measurements and timestamps in ns. Late
   * reports can fill history that is still visible without moving newer points.
   * Time buckets retain their minimum and maximum, so reducing the history to
   * screen columns does not average away short spikes or dips.
   */
  class HudGraph {
  public:
    HudGraph(std::string label, std::string unit, uint32_t color,
      const HudGraphOptions& options);

    void addSample(int64_t timeNs, float value);
    void addGap(int64_t timeNs);
    void advance(int64_t timeNs);
    void clear();

    // Allow a separately averaged heading without changing the plotted samples.
    void setValueText(const std::string& text) { m_valueText = text; }

    // Includes the heading. Position is its baseline, as for text HUD items.
    HudPos size() const;
    void render(HudRenderer& renderer, HudPos position) const;

  private:
    static constexpr size_t BucketCount = 128;

    struct Bucket {
      int64_t index = -1;
      float minimum = 0.0f;
      float maximum = 0.0f;
      float last = 0.0f;
      int64_t lastTimeNs = -1;
      bool missing = false;
    };

    std::array<Bucket, BucketCount> m_buckets;
    std::string m_label;
    std::string m_unit;
    uint32_t m_color;
    int32_t m_width;
    int32_t m_height;
    int64_t m_bucketDurationNs;
    int64_t m_timeNs = -1;
    int64_t m_lastTimeNs = -1;
    int64_t m_lastRangeUpdateNs = -1;
    int64_t m_lastTextUpdateNs = -1;
    float m_maximum;
    std::string m_valueText = "--";
    double m_rangeMaximum = 0.0;
    std::string m_rangeText = "0-1.0";

    void updateRange();
  };

}
