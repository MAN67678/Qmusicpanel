// QPanel — "hyper land" theme system.
// TrueColor palettes (Catppuccin, Tokyo Night, Rose Pine, Gruvbox Material),
// default-terminal background for compositor transparency, gradient borders.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace qp {

using Color = uint32_t; // 0xRRGGBB
constexpr Color NONE = 0xFFFFFFFF; // terminal default (transparency-friendly)

static inline Color mix(Color a, Color b, float t);

struct Palette {
    // identity
    const char* name;

    // base
    Color bg;        // UNUSED at runtime — panels keep term default (NONE)
    Color surface;   // translucent-feel panel bg (slightly darker than default)
    Color frame;     // inactive border
    Color frameHi;   // focused border base (gradient mixes frameHi -> mixHi)

    // hyper accents
    Color accentA;   // gradient start (e.g. cyan)
    Color accentB;   // gradient end (e.g. green)
    Color text;
    Color dim;
    Color label;
    Color ok;
    Color warn;
    Color err;
    Color gold;
    Color dimmer = 0x2C3A55;

    // UNorm sets below replicate popular "rice" palettes ----------------------
    // Catppuccin Mocha
    static Palette catppuccinMocha() {
        return { "catppuccin-mocha",
            0x1E1E2E, 0x181825, 0x313244, 0x45475A,
            0x89B4FA, 0xA6E3A1, 0xCDD6F4, 0x6C7086, 0x9399B2,
            0xA6E3A1, 0xFAB387, 0xF38BA8, 0xF9E2AF };
    }
    // Catppuccin Macchiato
    static Palette catppuccinMacchiato() {
        return { "catppuccin-macchiato",
            0x24273A, 0x1E2030, 0x363A4F, 0x494D64,
            0x8AADF4, 0xA6DA95, 0xCAD3F5, 0x6E738D, 0x939AB7,
            0xA6DA95, 0xF5A97F, 0xED8796, 0xF5A97F };
    }
    // Tokyo Night
    static Palette tokyoNight() {
        return { "tokyo-night",
            0x1A1B26, 0x16161E, 0x292E42, 0x3B4261,
            0x7DCFFF, 0x9ECE6A, 0xC0CAF5, 0x565F89, 0x737AA2,
            0x9ECE6A, 0xE0AF68, 0xF7768E, 0xE0AF68 };
    }
    // Rosé Pine
    static Palette rosePine() {
        return { "rose-pine",
            0x191724, 0x1F1D2E, 0x26233A, 0x3F3A52,
            0xC4A7E7, 0x9CCFD8, 0xE0DEF4, 0x6E6A86, 0x908CAA,
            0x31748F, 0xF6C177, 0xEBBCBA, 0xF6C177 };
    }
    // Gruvbox Material Dark
    static Palette gruvbox() {
        return { "gruvbox-material",
            0x282828, 0x282828, 0x3C3836, 0x504945,
            0x83A598, 0xB8BB26, 0xEAE9A6, 0xA89984, 0x928374,
            0xB8BB26, 0xD7A878, 0xEA6962, 0xD8A657 };
    }
    // "Hyper" default — neon cyan->violet->pink gradient land
    static Palette hyper() {
        return { "hyper",
            0x0A0E1A, 0x0D1220, 0x1E2740, 0x2C3A66,
            0x22D3EE, 0x7C64EE, 0xE2E8F0, 0x5B6B87, 0x8FA1C4,
            0x34D399, 0xFBBF24, 0xF472B6, 0xFBBF24 };
    }
    static Palette latte() {
        return { "catppuccin-latte",
            0xEFF1F5, 0xE6E9EF, 0xDCE0E8, 0xCCD0DA,
            0x1E66F5, 0x40A02B, 0x4C4F69, 0x8C8FA1, 0x7C7F93,
            0x40A02B, 0xDF8E1D, 0xD20F39, 0xDF8E1D };
    }

    static const std::vector<Palette>& all() {
        static std::vector<Palette> v = {
            hyper(), catppuccinMocha(), catppuccinMacchiato(), latte(),
            tokyoNight(), rosePine(), gruvbox()
        };
        return v;
    }
    static const Palette& byName(const std::string& n) {
        for (auto& p : all()) if (n == p.name) return p;
        return all()[0];
    }
};

// gradient border: frameHi -> accentA -> accentB across a border run
static inline Color grad(const Palette& P, float t01) {
    if (t01 < 0.5f) return mix(P.frameHi, P.accentA, t01 * 2.f);
    return mix(P.accentA, P.accentB, (t01 - 0.5f) * 2.f);
}

// approximate "translucent surface": blend palette surface at low alpha
// over the default bg. Real transparency comes from emitting bg=NONE.
static inline Color surfaceMix(const Palette& P) { return P.surface; }

static inline Color mix(Color a, Color b, float t) {
    if (a == NONE && b == NONE) return NONE;
    if (a == NONE) a = 0x0A0E1A;
    if (b == NONE) b = 0x0A0E1A;
    int r = int(float(a >> 16 & 255) + t * (float(b >> 16 & 255) - float(a >> 16 & 255)));
    int g = int(float(a >> 8  & 255) + t * (float(b >> 8  & 255) - float(a >> 8  & 255)));
    int bl = int(float(a & 255)      + t * (float(b & 255)      - float(a & 255)));
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (bl < 0) bl = 0;
    if (bl > 255) bl = 255;
    return (Color(r) << 16) | (Color(g) << 8) | Color(bl);
}

} // namespace qp
