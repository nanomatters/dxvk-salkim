# HUD font atlas

The embedded font, **Wineland HUD Mono**, is an ASCII distance-field conversion
of Share Tech Mono Regular by Carrois Type Design / Ralph du Carrois. The
conversion has its own name because the original reserves the name "Share".
The font data remains under SIL OFL 1.1; see the font section of DXVK's LICENSE.

The atlas contains U+0020 through U+007E in a single R8 texture. It uses a
32-pixel em, a rounded 17-pixel fixed advance, and a five-texel signed-distance
falloff with the outline at 0.5. Glyph rectangles include a zero border for
bilinear filtering. Both HUD rendering paths consume the same data, including
when drawing smaller text. No font loader or rasterizer is needed at runtime.

Main labels and values use size 20; units and secondary information use size
14. Both are multiplied by the HUD's existing `scale` setting. Mixed-size
values share a baseline, with 24-pixel spacing between main text rows.

## Regeneration

Run these commands from this directory with a C++17 compiler and the system
`stb/stb_truetype.h` header (version 1.26). This is an offline maintainer tool,
not a normal DXVK build dependency. The source font need not be installed.

```sh
font_tmp=$(mktemp -d)
curl -fL https://raw.githubusercontent.com/google/fonts/main/ofl/sharetechmono/ShareTechMono-Regular.ttf -o "$font_tmp/font.ttf" &&
printf '%s  %s\n' 9ceab1f87414829af259c0f537573ae03ef7dd3147c0b27a36a1a0beb6732677 "$font_tmp/font.ttf" | sha256sum -c - &&
c++ -std=c++17 -O2 generate_font.cpp -o "$font_tmp/generate_font" &&
"$font_tmp/generate_font" "$font_tmp/font.ttf" > "$font_tmp/dxvk_hud_font.cpp"
```

Only replace `dxvk_hud_font.cpp` after successful generation and inspection of
the result. The hash check deliberately rejects a changed upstream font.
The generator expects trusted input and rejects missing ASCII glyphs or a
non-monospaced advance. It packs glyphs deterministically and emits compile-time
checks for the glyph count and texture byte count.
