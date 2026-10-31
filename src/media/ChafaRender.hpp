// QPanel — chafa art engine.
// Renders decoded RGB cover pixels into terminal cells using libchafa when it
// is available at build time; the built-in half-block renderer stays as the
// fallback so a machine without chafa still builds and runs.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../ui/Palette.hpp"

namespace qp {

enum class ArtEngine  { Block, Chafa };
enum class ChafaSymbols { All, Block, Sextant, Braille, Ascii, Ramp };
enum class ChafaMode  { TrueColor, Indexed256, Indexed16 };
enum class ChafaDither { None, Ordered, Diffusion };

struct ArtConfig {
    ArtEngine    engine  = ArtEngine::Chafa;   // falls back to Block if unavailable
    ChafaSymbols symbols = ChafaSymbols::Block;  // widest font support; 'all' is opt-in
    ChafaMode    mode    = ChafaMode::TrueColor;
    ChafaDither  dither  = ChafaDither::None;
    bool         fgOnly  = false;
    float        gamma   = 1.0f;
    // density ladder for the built-in ASCII renderer (symbols = ascii|ramp).
    // A leading space is always implied, so keep this to visible glyphs.
    std::string  ramp    = ".:-=+*#%@";
};

ArtConfig& artConfig();
bool chafaAvailable();
const char* chafaBackendName();

// One rendered art cell. `ch` may be a wide glyph; `bg` is meaningful only
// when hasBg (chafa leaves background untouched otherwise, so transparency
// survives in the panels around the cover).
struct ArtCell {
    char32_t ch = U' ';
    Color fg = NONE;
    Color bg = NONE;
    bool hasBg = false;
};

// rgb: w*h*3 bytes. Returns rows*cols cells in row-major order; empty when
// chafa is unavailable or the input is degenerate.
std::vector<ArtCell> chafaRender(const uint8_t* rgb, int w, int h, int cols, int rows);

// Built-in ASCII ramp renderer. chafa can only use glyphs it has coverage data
// for (a bare "ascii" map collapses to ~2 usable chars), so ASCII art is drawn
// here: average each cell, take luminance, pick a glyph from `ramp`, colour it
// with the cell's average colour.
std::vector<ArtCell> asciiArtRender(const uint8_t* rgb, int w, int h, int cols,
                                    int rows, const std::string& ramp);

} // namespace qp
