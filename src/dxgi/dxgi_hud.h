// Copyright 2026 Erhan Bilgili

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "dxgi_interfaces.h"

#include "../dxvk/hud/dxvk_hud_item.h"

namespace dxvk {

  class DxgiHud : public hud::HudRenderer {

  public:

    static std::unique_ptr<DxgiHud> create(
            IDXGIAdapter*           adapter,
      const std::string&            fallbackConfig);

    void render(
            IDXGIVkSwapChainHud*     presenter);

  private:

    constexpr static size_t MaxVertices = 4096;

    explicit DxgiHud(
            std::string             config,
            std::string             deviceName);

    hud::HudItemSet                  m_hudItems;
    std::vector<DXGI_VK_HUD_VERTEX> m_vertices;
    bool                             m_failed = false;

    void drawText(
            uint32_t                size,
            hud::HudPos             position,
            uint32_t                color,
      const std::string&            text) override;

  };

}
