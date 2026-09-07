// Offline atlas generator; not part of the DXVK build.
// Requires stb_truetype 1.26. Only use trusted input fonts.
// c++ -std=c++17 -O2 generate_font.cpp -o generate_font
// ./generate_font ShareTechMono-Regular.ttf > dxvk_hud_font.cpp

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb/stb_truetype.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

struct Glyph {
  int code, x = 0, y = 0, w = 0, h = 0, ox = 0, oy = 0;
  std::vector<unsigned char> pixels;
};

int main(int argc, char** argv) try {
  if (argc != 2)
    throw std::runtime_error("Usage: generate_font ShareTechMono-Regular.ttf");

  std::ifstream input(argv[1], std::ios::binary);
  std::vector<unsigned char> data((std::istreambuf_iterator<char>(input)), {});
  stbtt_fontinfo font = {};
  if (data.empty() || !stbtt_InitFont(&font, data.data(), 0))
    throw std::runtime_error("Cannot read TrueType font");

  constexpr int size = 32, falloff = 5, padding = falloff + 1;
  constexpr int width = 512, first = 32, count = 95;
  const float scale = stbtt_ScaleForMappingEmToPixels(&font, size);
  int advance, bearing;
  stbtt_GetCodepointHMetrics(&font, first, &advance, &bearing);

  std::array<Glyph, count> glyphs;
  std::array<int, count> order;
  for (int i = 0; i < count; i++) {
    auto& glyph = glyphs[i];
    glyph.code = first + i;
    order[i] = i;
    int glyphAdvance;
    stbtt_GetCodepointHMetrics(&font, glyph.code, &glyphAdvance, &bearing);
    if (!stbtt_FindGlyphIndex(&font, glyph.code) || glyphAdvance != advance)
      throw std::runtime_error("Expected printable ASCII with a fixed advance");

    // Both HUD shaders use 0.5 at the outline and a five-texel falloff.
    auto* sdf = stbtt_GetCodepointSDF(&font, scale, glyph.code, padding,
      128, 127.5f / falloff, &glyph.w, &glyph.h, &glyph.ox, &glyph.oy);
    if (!sdf) {
      if (glyph.code != ' ')
        throw std::runtime_error("Could not generate glyph SDF");
      glyph.w = glyph.h = 2 * padding;
      glyph.ox = glyph.oy = -padding;
      glyph.pixels.resize(glyph.w * glyph.h);
    } else {
      glyph.pixels.assign(sdf, sdf + glyph.w * glyph.h);
      stbtt_FreeSDF(sdf, font.userdata);
    }
    glyph.ox = -glyph.ox;
    glyph.oy = -glyph.oy;
    if (glyph.w > width)
      throw std::runtime_error("Glyph exceeds atlas width");
  }

  // Deterministic height-sorted shelves. Each glyph includes a zero border.
  std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
    return glyphs[a].h > glyphs[b].h;
  });
  int x = 0, y = 0, rowHeight = 0;
  for (int i : order) {
    auto& glyph = glyphs[i];
    if (x + glyph.w > width) {
      x = 0;
      y += rowHeight;
      rowHeight = 0;
    }
    glyph.x = x;
    glyph.y = y;
    x += glyph.w;
    rowHeight = std::max(rowHeight, glyph.h);
  }
  const int height = y + rowHeight;
  std::vector<unsigned char> atlas(width * height);
  for (const auto& glyph : glyphs) {
    for (int row = 0; row < glyph.h; row++)
      std::copy_n(glyph.pixels.data() + row * glyph.w, glyph.w,
        atlas.data() + (glyph.y + row) * width + glyph.x);
  }

  std::printf("#include \"dxvk_hud_font.h\"\n\n"
    "namespace dxvk::hud {\n\n"
    "  // Wineland HUD Mono: an ASCII SDF conversion of Share Tech Mono.\n"
    "  // Copyright (c) 2012, Carrois Type Design, Ralph du Carrois\n"
    "  // (post@carrois.com www.carrois.com), with Reserved Font Name 'Share'.\n"
    "  // Font data is licensed under SIL OFL 1.1; see LICENSE.\n"
    "  // Generated with generate_font.cpp; see README.font.md.\n"
    "  const HudGlyph g_hudFontGlyphs[] = {\n");
  for (const auto& glyph : glyphs) {
    // Numeric code points avoid special escaping for quote and backslash.
    std::printf("    {%d, %d, %d, %d, %d, %d, %d},\n", glyph.code,
      glyph.x, glyph.y, glyph.w, glyph.h, glyph.ox, glyph.oy);
  }
  std::printf("  };\n\n  const uint8_t g_hudFontImage[] = {\n");
  for (size_t i = 0; i < atlas.size(); i++) {
    if (i % 50 == 0)
      std::printf("    ");
    std::printf("0x%02x,", atlas[i]);
    if (i % 50 == 49 || i + 1 == atlas.size())
      std::printf("\n");
  }
  std::printf("  };\n\n"
    "  const HudFont g_hudFont = { %d, %d, %d, %d, %d, %d, g_hudFontGlyphs, g_hudFontImage };\n\n"
    "  static_assert(sizeof(g_hudFontImage) == %d * %d);\n"
    "  static_assert(sizeof(g_hudFontGlyphs) / sizeof(HudGlyph) == %d);\n\n}\n",
    size, width, height, falloff, int(std::lround(advance * scale)), count,
    width, height, count);
  return std::ferror(stdout) ? 1 : 0;
} catch (const std::exception& error) {
  std::fprintf(stderr, "%s\n", error.what());
  return 1;
}
