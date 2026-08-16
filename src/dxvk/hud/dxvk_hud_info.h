#pragma once

#include <string>

namespace dxvk::hud {

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
