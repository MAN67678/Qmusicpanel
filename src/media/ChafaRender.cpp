#include "ChafaRender.hpp"

#include <algorithm>
#include <cmath>

#ifdef QP_HAVE_CHAFA
#include <chafa.h>
#endif

namespace qp {

ArtConfig& artConfig() {
    static ArtConfig cfg;
    return cfg;
}

std::vector<ArtCell> asciiArtRender(const uint8_t* rgb, int w, int h, int cols,
                                    int rows, const std::string& rampIn) {
    std::vector<ArtCell> out;
    if (!rgb || w <= 0 || h <= 0 || cols <= 0 || rows <= 0) return out;
    std::string ramp = " " + (rampIn.empty() ? std::string(".:-=+*#%@") : rampIn);
    std::vector<char32_t> glyphs;              // decode as UTF-8 if needed
    for (size_t i = 0; i < ramp.size();) {
        uint8_t c0 = uint8_t(ramp[i]);
        int need = c0 < 0x80 ? 1 : (c0 >> 5) == 0x6 ? 2 : (c0 >> 4) == 0xE ? 3 : 4;
        char32_t cp = c0 < 0x80 ? c0 : 0;
        if (need > 1 && i + size_t(need) <= ramp.size()) {
            cp = c0 & (need == 2 ? 0x1F : need == 3 ? 0x0F : 0x07);
            for (int k = 1; k < need; k++) cp = (cp << 6) | (uint8_t(ramp[i + size_t(k)]) & 0x3F);
        }
        glyphs.push_back(cp ? cp : U' ');
        i += size_t(need);
    }
    if (glyphs.size() < 2) glyphs = { U' ', U'#' };
    out.resize(size_t(cols) * size_t(rows));     // (pass 2 writes here)

    // pass 1: average each cell, remember its luminance
    struct CellAvg { int r, g, b; float lum; };
    std::vector<CellAvg> avg(size_t(cols) * size_t(rows));
    std::vector<bool> valid(size_t(cols) * size_t(rows), false);
    float lo = 1.f, hi = 0.f;
    for (int cy = 0; cy < rows; cy++) {
        int y0 = cy * h / rows, y1 = std::max(y0 + 1, (cy + 1) * h / rows);
        for (int cx = 0; cx < cols; cx++) {
            int x0 = cx * w / cols, x1 = std::max(x0 + 1, (cx + 1) * w / cols);
            uint64_t sr = 0, sg = 0, sb = 0, n = 0;
            for (int y = y0; y < y1 && y < h; y++)
                for (int x = x0; x < x1 && x < w; x++) {
                    size_t t = (size_t(y) * size_t(w) + size_t(x)) * 3;
                    sr += rgb[t]; sg += rgb[t + 1]; sb += rgb[t + 2]; n++;
                }
            if (!n) continue;
            int r = int(sr / n), g = int(sg / n), b = int(sb / n);
            float lum = (0.2126f * float(r) + 0.7152f * float(g) + 0.0722f * float(b)) / 255.f;
            size_t idx = size_t(cy) * size_t(cols) + size_t(cx);
            avg[idx] = { r, g, b, lum };
            valid[idx] = true;
            lo = std::min(lo, lum);
            hi = std::max(hi, lum);
        }
    }

    // pass 2: contrast-stretch so a bright or a dark cover both use the whole
    // ladder (without this a white-background cover is a wall of '@')
    float span = std::max(0.02f, hi - lo);
    for (int cy = 0; cy < rows; cy++)
        for (int cx = 0; cx < cols; cx++) {
            size_t idx = size_t(cy) * size_t(cols) + size_t(cx);
            ArtCell& cell = out[idx];
            if (!valid[idx]) { cell.ch = U' '; cell.fg = NONE; continue; }
            const CellAvg& a = avg[idx];
            float t = (a.lum - lo) / span;
            t = std::pow(std::clamp(t, 0.f, 1.f), 0.85f);        // slight mid lift
            size_t gi = size_t(t * float(glyphs.size() - 1) + 0.5f);
            cell.ch = glyphs[std::min(gi, glyphs.size() - 1)];
            cell.fg = Color(a.r << 16 | a.g << 8 | a.b);
            cell.bg = NONE;
            cell.hasBg = false;
        }
    return out;
}

#ifdef QP_HAVE_CHAFA

bool chafaAvailable() { return true; }

const char* chafaBackendName() { return "libchafa"; }

// chafa's own "ascii" class is ASCII *including* letters and digits, which
// renders as scattered text ("A F g J k L P R y Z 4 5 7 9"). For art we want a
// punctuation-only density ramp, so the ASCII set is rebuilt from ranges.
static void applySymbols(ChafaSymbolMap* map, ChafaSymbols s) {
    if (!map) return;
    switch (s) {
        // cell geometry is 1x2, so half-blocks + solid + space are the exact
        // match (and the widest font coverage); quadrant/sextant glyphs imply
        // square subpixels and are opt-in.
        case ChafaSymbols::Block:
            chafa_symbol_map_add_by_tags(map, ChafaSymbolTags(
                CHAFA_SYMBOL_TAG_HALF | CHAFA_SYMBOL_TAG_SOLID | CHAFA_SYMBOL_TAG_SPACE));
            return;
        case ChafaSymbols::Sextant:
            chafa_symbol_map_add_by_tags(map, ChafaSymbolTags(
                CHAFA_SYMBOL_TAG_SEXTANT | CHAFA_SYMBOL_TAG_BLOCK | CHAFA_SYMBOL_TAG_HALF));
            return;
        case ChafaSymbols::Braille:
            chafa_symbol_map_add_by_tags(map, ChafaSymbolTags(
                CHAFA_SYMBOL_TAG_BRAILLE | CHAFA_SYMBOL_TAG_DOT));
            return;
        case ChafaSymbols::All:
            chafa_symbol_map_add_by_tags(map, ChafaSymbolTags(CHAFA_SYMBOL_TAG_ALL));
            return;
        case ChafaSymbols::Ramp:
            // coarser ladder: punctuation minus brackets/braces/pipe/tilde
            chafa_symbol_map_apply_selectors(map, "ascii-alnum", nullptr);
            chafa_symbol_map_remove_by_range(map, '[', '`');
            chafa_symbol_map_remove_by_range(map, '{', '~');
            return;
        case ChafaSymbols::Ascii:
        default:
            // MODERN ASCII LOOK. chafa's "ascii" class includes letters and
            // digits ("A F g J k L P R y Z 4 5 7 9"), which reads as random
            // text scattered over the cover. Subtracting "alnum" leaves a
            // punctuation-only density ramp ( ~22 glyphs) — proper ASCII art.
            chafa_symbol_map_apply_selectors(map, "ascii-alnum", nullptr);
            return;
    }
}

std::vector<ArtCell> chafaRender(const uint8_t* rgb, int w, int h, int cols, int rows) {
    std::vector<ArtCell> out;
    if (!rgb || w <= 0 || h <= 0 || cols <= 0 || rows <= 0) return out;
    const ArtConfig& ac = artConfig();

    ChafaCanvasConfig* cfg = chafa_canvas_config_new();
    if (!cfg) return out;
    chafa_canvas_config_set_geometry(cfg, cols, rows);
    chafa_canvas_config_set_cell_geometry(cfg, 1, 2);   // one cell = 1x2 pixels
    switch (ac.mode) {
        case ChafaMode::Indexed256: chafa_canvas_config_set_canvas_mode(cfg, CHAFA_CANVAS_MODE_INDEXED_256); break;
        case ChafaMode::Indexed16:  chafa_canvas_config_set_canvas_mode(cfg, CHAFA_CANVAS_MODE_INDEXED_16);  break;
        case ChafaMode::TrueColor:
        default:                    chafa_canvas_config_set_canvas_mode(cfg, CHAFA_CANVAS_MODE_TRUECOLOR);   break;
    }
    chafa_canvas_config_set_color_space(cfg, CHAFA_COLOR_SPACE_RGB);
    switch (ac.dither) {
        case ChafaDither::Ordered:   chafa_canvas_config_set_dither_mode(cfg, CHAFA_DITHER_MODE_ORDERED);   break;
        case ChafaDither::Diffusion: chafa_canvas_config_set_dither_mode(cfg, CHAFA_DITHER_MODE_DIFFUSION); break;
        case ChafaDither::None:
        default:                    chafa_canvas_config_set_dither_mode(cfg, CHAFA_DITHER_MODE_NONE);     break;
    }
    chafa_canvas_config_set_fg_only_enabled(cfg, ac.fgOnly ? TRUE : FALSE);

    ChafaSymbolMap* map = chafa_symbol_map_new();
    if (map) {
        applySymbols(map, ac.symbols);
        chafa_canvas_config_set_symbol_map(cfg, map);
    }

    ChafaCanvas* canvas = chafa_canvas_new(cfg);
    if (!canvas) {
        if (map) chafa_symbol_map_unref(map);
        chafa_canvas_config_unref(cfg);
        return out;
    }
    chafa_canvas_draw_all_pixels(canvas, CHAFA_PIXEL_RGB8, rgb, w, h, w * 3);

    out.resize(size_t(cols) * size_t(rows));
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            gunichar gc = chafa_canvas_get_char_at(canvas, x, y);
            gint fg = -1, bg = -1;
            chafa_canvas_get_colors_at(canvas, x, y, &fg, &bg);
            ArtCell& c = out[size_t(y) * size_t(cols) + size_t(x)];
            c.ch = gc ? char32_t(gc) : U' ';
            if (fg >= 0) c.fg = Color(guint32(fg) & 0xFFFFFFu);
            if (bg >= 0 && (guint32(bg) & 0xFFFFFFu) != 0) { c.bg = Color(guint32(bg) & 0xFFFFFFu); c.hasBg = true; }
        }
    }

    if (map) chafa_symbol_map_unref(map);
    chafa_canvas_unref(canvas);
    chafa_canvas_config_unref(cfg);
    return out;
}

#else  // ---- no chafa at build time: builtin renderer only ----

bool chafaAvailable() { return false; }
const char* chafaBackendName() { return "builtin"; }
std::vector<ArtCell> chafaRender(const uint8_t*, int, int, int, int) { return {}; }

#endif

} // namespace qp
