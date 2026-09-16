#pragma once

// A plain RGB pixel buffer with text and rectangle drawing.
//
// The debugger renders its whole interface into one of these, which SDL then
// presents. Keeping the drawing independent of the window means the interface
// can also be dumped to a PNG and inspected without a display -- which is how
// it gets tested.

#include "host/font8x8.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gba::host {

struct Rgb {
    uint8_t r, g, b;
};

// A restrained palette: this is a debugger, and colour should mean something.
inline constexpr Rgb kBg{0x12, 0x14, 0x18};
inline constexpr Rgb kPanel{0x1B, 0x1E, 0x24};
inline constexpr Rgb kText{0xC8, 0xCC, 0xD4};
inline constexpr Rgb kDim{0x6A, 0x71, 0x7E};
inline constexpr Rgb kAccent{0x6E, 0xA8, 0xFF};
inline constexpr Rgb kWarn{0xE8, 0xA3, 0x3D};
inline constexpr Rgb kChanged{0x8F, 0xD4, 0x6C};

class Canvas {
  public:
    Canvas(uint32_t width, uint32_t height)
        : w_(width), h_(height), px_(size_t(width) * height * 3, 0) {}

    uint32_t width() const { return w_; }
    uint32_t height() const { return h_; }
    const std::vector<uint8_t>& pixels() const { return px_; }

    void clear(Rgb c) {
        for (size_t i = 0; i < px_.size(); i += 3) {
            px_[i] = c.r;
            px_[i + 1] = c.g;
            px_[i + 2] = c.b;
        }
    }

    void rect(int x, int y, int w, int h, Rgb c) {
        for (int yy = y; yy < y + h; ++yy) {
            if (yy < 0 || yy >= int(h_)) continue;
            for (int xx = x; xx < x + w; ++xx) {
                if (xx < 0 || xx >= int(w_)) continue;
                const size_t i = (size_t(yy) * w_ + xx) * 3;
                px_[i] = c.r;
                px_[i + 1] = c.g;
                px_[i + 2] = c.b;
            }
        }
    }

    // A one-pixel outline, for panel edges.
    void frame(int x, int y, int w, int h, Rgb c) {
        rect(x, y, w, 1, c);
        rect(x, y + h - 1, w, 1, c);
        rect(x, y, 1, h, c);
        rect(x + w - 1, y, 1, h, c);
    }

    void text(int x, int y, const std::string& s, Rgb c) {
        int cx = x;
        for (char ch : s) {
            if (ch == '\n') { y += 10; cx = x; continue; }
            const auto u = static_cast<unsigned char>(ch);
            if (u >= 32 && u < 127) {
                const uint8_t* glyph = kFont8x8[u - 32];
                for (int row = 0; row < 8; ++row)
                    for (int col = 0; col < 6; ++col)
                        if (glyph[row] & (1u << col)) rect(cx + col, y + row, 1, 1, c);
            }
            cx += 6;
        }
    }

    // Draws the emulator's BGR555 framebuffer, scaled by an integer factor.
    void blitFramebuffer(int x, int y, const std::vector<uint32_t>& fb, uint32_t fbW,
                         uint32_t fbH, int scale) {
        for (uint32_t sy = 0; sy < fbH; ++sy)
            for (uint32_t sx = 0; sx < fbW; ++sx) {
                const uint32_t i = sy * fbW + sx;
                const uint32_t c = (fb[i >> 1] >> ((i & 1) * 16)) & 0xFFFF;
                const uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
                rect(x + int(sx) * scale, y + int(sy) * scale, scale, scale,
                     {uint8_t((r << 3) | (r >> 2)), uint8_t((g << 3) | (g >> 2)),
                      uint8_t((b << 3) | (b >> 2))});
            }
    }

  private:
    uint32_t w_, h_;
    std::vector<uint8_t> px_;
};

}  // namespace gba::host
