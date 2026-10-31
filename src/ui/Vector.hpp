// QPanel — vector layer: terminal sub-pixel rasterization.
//  - DisplayTier: responsive layout breakpoints (<80 compact, 80-120 standard,
//    >120x36 expanded => multi-cell vector geometry, btop/notcurses-style).
//  - BrailleCanvas: virtual 2x4 dot framebuffer (ISO 11548-1 dot table) with
//    Bresenham lines — Lissajous curves, oscilloscopes, giant vector icons.
//  - BigClock: 3x5 cell raster digit font for the elapsed-time readout.
//  - BigGlyphs: multi-row vector transport buttons for cinema mode.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "Palette.hpp"
#include "../term/Terminal.hpp"

namespace qp {

enum class DisplayTier { Compact, Standard, Expanded };

inline DisplayTier evaluateTier(int cols, int rows) {
    if (cols < 85 || rows < 24) return DisplayTier::Compact;
    if (cols > 130 && rows > 36) return DisplayTier::Expanded;
    return DisplayTier::Standard;
}

// ---------------------------------------------------------------------------
// BrailleCanvas — sub-pixel virtual framebuffer
// ---------------------------------------------------------------------------
class BrailleCanvas {
public:
    BrailleCanvas(size_t cell_width, size_t cell_height)
        : cols_(cell_width), rows_(cell_height),
          width_(cell_width * 2), height_(cell_height * 4),
          grid_(cols_ * rows_, 0) {}

    void clear() { grid_.assign(grid_.size(), 0); }

    void set_pixel(int x, int y) {
        if (x < 0 || x >= width_ || y < 0 || y >= height_) return;
        int cell_x = x / 2;                 // cell coords
        int cell_y = y / 4;
        int dot_x = x % 2;
        int dot_y = y % 4;

        // Braille dot offset table (ISO 11548-1)
        static const int dot_map[4][2] = {
            {0x01, 0x08},
            {0x02, 0x10},
            {0x04, 0x20},
            {0x40, 0x80}
        };
        grid_[size_t(cell_y) * cols_ + cell_x] |= uint8_t(dot_map[dot_y][dot_x]);
    }

    void unset_pixel(int x, int y) {
        if (x < 0 || x >= width_ || y < 0 || y >= height_) return;
        int cell_x = x / 2, cell_y = y / 4, dot_x = x % 2, dot_y = y % 4;
        static const int dot_map[4][2] = {
            {0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80}
        };
        grid_[size_t(cell_y) * cols_ + cell_x] &= uint8_t(~dot_map[dot_y][dot_x]);
    }

    void draw_line(int x0, int y0, int x1, int y1) { // Bresenham
        int dx = x1 - x0 < 0 ? -(x1 - x0) : x1 - x0, sx = x0 < x1 ? 1 : -1;
        int dy = y1 - y0 < 0 ? -(y1 - y0) : y1 - y0; dy = -dy; int sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        for (;;) {
            set_pixel(x0, y0);
            if (x0 == x1 && y0 == y1) break;
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }

    void draw_circle(int cx, int cy, int r) {
        int x = r, y = 0, err = 1 - r;
        while (x >= y) {
            set_pixel(cx + x, cy + y); set_pixel(cx + y, cy + x);
            set_pixel(cx - y, cy + x); set_pixel(cx - x, cy + y);
            set_pixel(cx - x, cy - y); set_pixel(cx - y, cy - x);
            set_pixel(cx + y, cy - x); set_pixel(cx + x, cy - y);
            if (err <= 0) { y++; err += 2 * y + 1; }
            if (err > 0)  { x--; err -= 2 * x + 1; }
        }
    }

    int width() const { return width_; }
    int height() const { return height_; }
    int cols() const { return int(cols_); }
    int rows() const { return int(rows_); }

    char32_t glyph(int cx, int cy) const {
        if (cx < 0 || cy < 0 || cx >= int(cols_) || cy >= int(rows_)) return U' ';
        return char32_t(0x2800 + grid_[size_t(cy) * cols_ + cx]);
    }

    // blit into the term grid (skips blank cells to preserve bg)
    void render(int termX, int termY, Color fg) const {
        for (size_t ir = 0; ir < rows_; ++ir)
            for (size_t ic = 0; ic < cols_; ++ic) {
                uint8_t d = grid_[ir * cols_ + ic];
                if (!d) continue;
                term.setCell(termX + int(ic), termY + int(ir), char32_t(0x2800 + d), fg, NONE);
            }
    }

    void fadeRender(int termX, int termY, Color fg, float alpha) const {
        for (size_t ir = 0; ir < rows_; ++ir)
            for (size_t ic = 0; ic < cols_; ++ic) {
                uint8_t d = grid_[ir * cols_ + ic];
                if (!d) continue;
                term.blendCell(termX + int(ic), termY + int(ir), char32_t(0x2800 + d), fg, alpha);
            }
    }

private:
    size_t cols_, rows_;
    int width_, height_;
    std::vector<uint8_t> grid_;
};

// ---------------------------------------------------------------------------
// BigClock — 3x5 cell raster font (quadrant-safe solid fills)
// ---------------------------------------------------------------------------
inline const char* const* bigDigit(int d) {
    static const char* const D[10][5] = {
        { "███", "█ █", "█ █", "█ █", "███" }, // 0
        { " █ ", "██ ", " █ ", " █ ", "███" }, // 1
        { "███", "  █", "███", "█  ", "███" }, // 2
        { "███", "  █", "███", "  █", "███" }, // 3
        { "█ █", "█ █", "███", "  █", "  █" }, // 4
        { "███", "█  ", "███", "  █", "███" }, // 5
        { "███", "█  ", "███", "█ █", "███" }, // 6
        { "███", "  █", " █ ", " █ ", " █ " }, // 7
        { "███", "█ █", "███", "█ █", "███" }, // 8
        { "███", "█ █", "███", "  █", "███" }, // 9
    };
    return (d >= 0 && d < 10) ? D[d] : D[0];
}

inline int bigClockWidth(int chars) { return chars * 4 - 1; } // 3 cols + 1 gap

// draws string of digits/':'/space at (x,y) top-left; returns width used
inline int drawBigClock(int x, int y, const std::string& txt, Color c, Color base) {
    int cx = x;
    for (char ch : txt) {
        if (ch == ':') {
            for (int r = 0; r < 5; ++r) term.setCell(cx, y + r, (r == 1 || r == 3) ? U'●' : U' ', (r==1||r==3)? c : base, NONE);
            cx += 1;
        } else if (ch >= '0' && ch <= '9') {
            auto g = bigDigit(ch - '0');
            for (int r = 0; r < 5; ++r) {
                // decode the UTF-8 row to codepoints (row is 3 cells wide)
                std::u32string u;
                for (unsigned char b : std::string_view(g[r])) {
                    if (b < 0x80) u.push_back(char32_t(b));
                    else if ((b & 0xC0) != 0x80) u.push_back(char32_t(b & 0x1F));
                    else u.back() = (u.back() << 6) | char32_t(b & 0x3F);
                }
                for (int k = 0; k < 3; ++k) {
                    bool solid = (k < int(u.size())) && u[size_t(k)] == U'█';
                    term.setCell(cx + k, y + r, solid ? U'█' : U' ', solid ? c : base, NONE);
                }
            }
            cx += 3;
        } else { cx += 3; }
        cx += 1; // gap
    }
    return cx - x;
}

// ---------------------------------------------------------------------------
// BigGlyphs — multi-row vector transport art for Expanded tier
// ---------------------------------------------------------------------------
struct BigGlyphs {
    static constexpr int ROWS = 5;
    static const char* const* play() {
        static const char* const P[ROWS] = {
            "▄██▄    ",
            "██████▄ ",
            "████████▄",
            "██████▀ ",
            "▀██▀    ",
        };
        return P;
    }
    static const char* const* pause() {
        static const char* const P[ROWS] = {
            "██  ███",
            "██  ███",
            "██  ███",
            "██  ███",
            "██  ███",
        };
        return P;
    }
};

} // namespace qp
