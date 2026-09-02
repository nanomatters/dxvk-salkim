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
            ID3DLowLatencyDevice*   lowLatencyDevice,
            IDXGIVkSwapChain*       presenter,
      const std::string&            fallbackConfig,
            int32_t                 fpsLowsWindow);

    void render(
            IDXGIVkSwapChainHud*     presenter,
            uint32_t                 surfaceWidth,
            uint32_t                 surfaceHeight);

  private:

    constexpr static size_t MaxVertices = 8192;

    explicit DxgiHud(
            std::string             config,
            std::string             deviceName,
      const Rc<DxvkAdapter>&         adapter,
            ID3DLowLatencyDevice*   lowLatencyDevice,
            IDXGIVkSwapChain*       presenter,
            int32_t                 fpsLowsWindow);

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
