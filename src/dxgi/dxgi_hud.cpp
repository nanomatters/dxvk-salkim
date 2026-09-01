// Copyright 2026 Erhan Bilgili

#include "dxgi_hud.h"

#include <algorithm>
#include <array>

#include "../dxvk/hud/dxvk_hud_font.h"

namespace dxvk {

  std::unique_ptr<DxgiHud> DxgiHud::create(
          IDXGIAdapter*           adapter,
    const std::string&            fallbackConfig) {
    std::string configString = env::getEnvVar("DXVK_HUD");

    if (configString.empty())
      configString = fallbackConfig;

    if (configString.empty())
      return nullptr;

    std::string deviceName = "D3D12 device";

    if (adapter) {
      DXGI_ADAPTER_DESC desc = { };

      if (SUCCEEDED(adapter->GetDesc(&desc)))
        deviceName = str::fromws(desc.Description);
    }

    auto result = std::unique_ptr<DxgiHud>(
      new DxgiHud(std::move(configString), std::move(deviceName)));

    if (result->m_hudItems.empty())
      return nullptr;

    Logger::info("DXGI HUD: VKD3D renderer enabled");
    return result;
  }


  DxgiHud::DxgiHud(
          std::string             config,
          std::string             deviceName)
  : m_hudItems(std::move(config)) {
    m_hudItems.add<hud::HudVersionItem>("version", -1);
    m_hudItems.add<hud::HudDeviceInfoItem>("devinfo", -1,
      std::move(deviceName), std::string(), std::string());
    m_hudItems.add<hud::HudFpsItem>("fps", -1);
    m_hudItems.add<hud::HudClientApiItem>("api", 1, "D3D12");
    m_vertices.reserve(MaxVertices);
  }


  void DxgiHud::render(
          IDXGIVkSwapChainHud*     presenter,
          uint32_t                 surfaceWidth,
          uint32_t                 surfaceHeight) {
    if (m_failed)
      return;

    m_hudItems.update();
    m_vertices.clear();
    m_hudItems.render(*this, surfaceWidth, surfaceHeight);

    const auto& font = hud::g_hudFont;
    DXGI_VK_HUD_DATA data = { };
    data.StructSize = sizeof(data);
    data.VertexCount = uint32_t(m_vertices.size());
    data.pVertices = m_vertices.data();
    data.Scale = m_hudItems.options().scale;
    data.Opacity = m_hudItems.options().opacity;
    data.FontWidth = font.width;
    data.FontHeight = font.height;
    data.FontDataSize = font.width * font.height;
    data.pFontData = font.texture;

    HRESULT hr = presenter->SetHudData(&data);

    if (FAILED(hr)) {
      Logger::warn(str::format("DXGI HUD: IDXGIVkSwapChainHud::SetHudData failed with error ", hr));
      m_failed = true;
    }
  }


  void DxgiHud::drawText(
          uint32_t                size,
          hud::HudPos             position,
          uint32_t                color,
    const std::string&            text) {
    if (text.empty() || m_vertices.size() >= MaxVertices)
      return;

    const auto& font = hud::g_hudFont;
    float sizeFactor = float(size) / float(font.size);

    for (size_t i = 0; i < text.size() && MaxVertices - m_vertices.size() >= 6; i++) {
      uint32_t codePoint = uint8_t(text[i]);
      const hud::HudGlyph* glyph = nullptr;

      for (uint32_t j = 0; j < font.charCount; j++) {
        if (font.glyphs[j].codePoint == codePoint) {
          glyph = &font.glyphs[j];
          break;
        }
      }

      if (!glyph) {
        for (uint32_t j = 0; j < font.charCount; j++) {
          if (font.glyphs[j].codePoint == '?') {
            glyph = &font.glyphs[j];
            break;
          }
        }
      }

      if (!glyph)
        continue;

      float x0 = float(position.x) + sizeFactor
        * (float(font.advance * i) - float(glyph->originX));
      float y0 = float(position.y) - sizeFactor * float(glyph->originY);
      float x1 = x0 + sizeFactor * float(glyph->w);
      float y1 = y0 + sizeFactor * float(glyph->h);
      float u0 = float(glyph->x);
      float v0 = float(glyph->y);
      float u1 = float(glyph->x + glyph->w);
      float v1 = float(glyph->y + glyph->h);

      const std::array<DXGI_VK_HUD_VERTEX, 6> quad = {{
        { { x0, y0 }, { u0, v0 }, color, 0 },
        { { x1, y0 }, { u1, v0 }, color, 0 },
        { { x0, y1 }, { u0, v1 }, color, 0 },
        { { x0, y1 }, { u0, v1 }, color, 0 },
        { { x1, y0 }, { u1, v0 }, color, 0 },
        { { x1, y1 }, { u1, v1 }, color, 0 },
      }};

      m_vertices.insert(m_vertices.end(), quad.begin(), quad.end());
    }
  }

}
