// Copyright 2026 Erhan Bilgili

#pragma once

#include <cstdint>
#include <string>

namespace dxvk::hud {

  constexpr uint32_t WineDisplayBackendMask = 0x000000ffu;
  constexpr uint32_t WineDisplayFeedbackDirectScanout = 0x00000800u;

  uint32_t queryWineDisplayFeedback();

  struct HudSystemInfo {
    std::string cpuName;
    std::string wineVersion;
    std::string wineBuild;
    std::string protonBuild;
    std::string displayBackend;

    static const HudSystemInfo& get();

  private:

    HudSystemInfo();

  };

}
