#include "Widgets.hpp"
#include "Glyphs.hpp"
#include "Vector.hpp"
#include "Text.hpp"
#include "../app/Config.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>

namespace qp {
namespace W {

// ---------------------------------------------------------------------------
// shared helpers
// ---------------------------------------------------------------------------
namespace {

// decode is width/control-safe: invalid bytes -> U+FFFD, C0/C1 -> space
std::u32string decode(const std::string& s) { return decodeUtf8(s); }

struct TermSink {
    void set(int x, int y, char32_t ch, Color fg, Color bg) { term.setCell(x, y, ch, fg, bg); }
};

// draws by display column, never by codepoint index
void putTrunc(int x, int y, int maxCols, const std::string& s, Color c, Color bg = NONE) {
    if (maxCols <= 0) return;
    static thread_local std::u32string u;
    u = decode(s);
    drawTextTrunc(TermSink{}, x, y, maxCols, u, c, bg);
}

// widget-local compositing surface (same wide-glyph contract as Terminal:
// the lead cell holds the codepoint, the trailing column is cont with ch==0)
struct Canvas {
    int x = 0, y = 0, w = 0, h = 0;
    std::vector<WCell> cells;
    void init(int X, int Y, int W, int H) {
        x = X; y = Y; w = W; h = H;
        if (w > 0 && h > 0) cells.assign(size_t(w) * h, WCell{});
    }
    bool inside(int X, int Y) const { return X >= x && Y >= y && X < x + w && Y < y + h; }
    size_t idx(int X, int Y) const { return size_t(Y - y) * w + size_t(X - x); }
    void detach(int X, int Y) {
        if (!inside(X, Y)) return;
        WCell& c = cells[idx(X, Y)];
        if (c.cont) { if (inside(X - 1, Y)) cells[idx(X - 1, Y)] = WCell{}; }
        else if (c.ch && cpWidth(c.ch) == 2 && inside(X + 1, Y)) cells[idx(X + 1, Y)] = WCell{};
        c = WCell{};
    }
    void set(int X, int Y, char32_t ch, Color fg, Color bg) {
        if (!inside(X, Y)) return;
        if (ch == 0) {                       // explicit continuation marker
            WCell& cc = cells[idx(X, Y)];
            if (!cc.cont && cc.ch && cpWidth(cc.ch) == 2 && inside(X + 1, Y))
                cells[idx(X + 1, Y)] = WCell{};
            cc.ch = 0; cc.fg = fg; cc.bg = bg; cc.cont = true;
            return;
        }
        if (ch < 0x20 || ch == 0x7F) ch = U' ';
        detach(X, Y);
        WCell& c = cells[idx(X, Y)];
        c.ch = ch; c.fg = fg; c.bg = bg; c.cont = false;
        if (cpWidth(ch) == 2) {
            if (!inside(X + 1, Y)) { c.ch = U' '; return; }
            detach(X + 1, Y);
            WCell& n = cells[idx(X + 1, Y)];
            n.ch = 0; n.fg = fg; n.bg = bg; n.cont = true;
        }
    }
    int text(int X, int Y, int maxCols, const std::u32string& u, Color fg, Color bg) {
        return drawText(*this, X, Y, maxCols, u, fg, bg);
    }
    int textTrunc(int X, int Y, int maxCols, const std::u32string& u, Color fg, Color bg) {
        return drawTextTrunc(*this, X, Y, maxCols, u, fg, bg);
    }
    const WCell& at(int X, int Y) const {
        return cells[idx(X, Y)];
    }
};

std::string mmss(double sec) {
    if (sec < 0) sec = 0;
    int t = int(sec), h = t / 3600, mn = (t % 3600) / 60, s = t % 60;
    char b[16];
    if (h > 0) std::snprintf(b, sizeof b, "%d:%02d:%02d", h, mn, s);
    else std::snprintf(b, sizeof b, "%02d:%02d", mn, s);
    return b;
}

// ---- config-driven frame + interior geometry -------------------------------
struct BorderGlyphs {
    char32_t tl, tr, bl, br, h, v;
};

BorderGlyphs borderGlyphs() {
    switch (cfg().border) {
        case BorderStyle::Single: return { U'┌', U'┐', U'└', U'┘', U'─', U'│' };
        case BorderStyle::Double: return { U'╔', U'╗', U'╚', U'╝', U'═', U'║' };
        case BorderStyle::Heavy:  return { U'┏', U'┓', U'┗', U'┛', U'━', U'┃' };
        case BorderStyle::Ascii:  return { U'+', U'+', U'+', U'+', U'-', U'|' };
        case BorderStyle::Rounded:
        case BorderStyle::None:
        default:                  return { U'╭', U'╮', U'╰', U'╯', U'─', U'│' };
    }
}

// Interior content rect: inside the frame, below the title row, then inset by
// [layout] pad_x / pad_y. Every widget draws inside this, so padding is one
// definition rather than per-widget fudging.
Rect contentRect(const Rect& r) {
    const Config& c = cfg();
    if (c.border == BorderStyle::None) return Rect{ r.x, r.y, r.w, r.h };
    int px = std::clamp(c.padX, 0, std::max(0, (r.w - 4) / 2));
    int py = std::clamp(c.padY, 0, std::max(0, r.h - 5));
    return Rect{ r.x + 1 + px, r.y + 2 + py,
                 std::max(1, r.w - 2 - 2 * px), std::max(1, r.h - 3 - py) };
}

} // namespace

// ---------------------------------------------------------------------------
// chassis: rounded corners + gradient border (Hyprland glow when focused)
// ---------------------------------------------------------------------------
void chassis(const Rect& r, const Palette& P, bool focused, const std::string& title,
             const std::string& suffix, float active) {
    const GlyphSet& G = GlyphTheme::get();
    if (r.w < 6 || r.h < 3) return;
    if (cfg().border == BorderStyle::None) return;      // frameless layout
    int x2 = r.x + r.w - 1, y2 = r.y + r.h - 1;
    const BorderGlyphs g = borderGlyphs();
    const bool ascii = cfg().border == BorderStyle::Ascii;

    // `glow` is the single "how alive is this border" knob: 1 when focused,
    // `active` when the dock is lit for another reason (spectrum owns it),
    // 0 for a plain inactive frame.
    const float glow = focused ? 1.f : std::clamp(active, 0.f, 1.f);

    // Hyprland col.active_border = A -> B 45deg: the sweep is diagonal, so the
    // blend parameter follows (dx + dy), not just x.
    auto borderCol = [&](int x, int y) -> Color {
        if (glow <= 0.001f) return P.frame;
        float t = float((x - r.x) + (y - r.y)) / float(std::max(1, r.w + r.h - 2));
        float bend = t * t * (3.f - 2.f * t); // hermite ease
        Color c = grad(P, bend);
        return glow >= 0.999f ? c : mix(P.frame, c, glow);
    };

    // soft drop shadow in the gaps outside the panel (Hyprland shadows = true).
    // Skip the outermost screen row/col: there the "shadow" just drew a dotted
    // line along the terminal edge, which read as noise.
    Color shadow = mix(P.surface, P.frame, 0.55f);
    const char32_t shGlyph = ascii ? U'.' : U'░';
    if (cfg().shadow) {
        if (x2 + 1 < term.width() - 1) {
            for (int y = r.y + 1; y <= y2; y++)
                term.blendCell(x2 + 1, y, shGlyph, shadow, 0.30f);
        }
        if (y2 + 1 < term.height() - 1) {
            for (int x = r.x + 1; x <= x2 && x < term.width() - 1; x++)
                term.blendCell(x, y2 + 1, shGlyph, shadow, 0.30f);
        }
    }

    for (int x = r.x + 1; x < x2; x++) {
        term.setCell(x, r.y, g.h, borderCol(x, r.y), NONE);
        term.setCell(x, y2, g.h, borderCol(x, y2), NONE);
    }
    for (int y = r.y + 1; y < y2; y++) {
        term.setCell(r.x, y, g.v, borderCol(r.x, y), NONE);
        term.setCell(x2, y, g.v, borderCol(x2, y), NONE);
    }
    // corners: focused panels glow (accent pushed toward the text colour)
    Color cTL = borderCol(r.x, r.y),   cTR = borderCol(x2, r.y);
    Color cBL = borderCol(r.x, y2),    cBR = borderCol(x2, y2);
    if (glow > 0.001f) {
        cTL = mix(cTL, P.text, 0.25f * glow); cTR = mix(cTR, P.text, 0.25f * glow);
        cBL = mix(cBL, P.text, 0.25f * glow); cBR = mix(cBR, P.text, 0.25f * glow);
    }
    term.setCell(r.x, r.y, g.tl, cTL, NONE);
    term.setCell(x2, r.y, g.tr, cTR, NONE);
    term.setCell(r.x, y2, g.bl, cBL, NONE);
    term.setCell(x2, y2, g.br, cBR, NONE);

    // title tab: "─ TITLE ─" + hyper dotted tail
    Color tc = mix(P.label, mix(P.accentA, P.text, 0.15f), glow);
    std::string head = title + (suffix.empty() ? "" : " " + suffix);
    std::u32string hu = decode(head);
    if (r.w - 2 > widthOf(hu) + 4) {
        int tx = r.x + 2;
        term.setCell(tx, r.y, U' ', tc, NONE);
        drawText(TermSink{}, tx + 1, r.y, x2 - tx - 1, hu, tc, NONE);
        term.setCell(tx + 1 + widthOf(hu), r.y, U' ', tc, NONE);
        for (int x = tx + widthOf(hu) + 2; x < x2; x++) {
            if (glow > 0.9f)   // only the truly focused dock gets the conf-file tail
                term.setCell(x, r.y, ascii ? U'.' : U'∙', mix(borderCol(x, r.y), P.text, 0.10f), NONE);
            else
                term.setCell(x, r.y, g.h, P.frame, NONE);
        }
    }
    (void)G;
}

// ---------------------------------------------------------------------------
// metadata top-left
// ---------------------------------------------------------------------------
void metadata(const Rect& r, const Palette& P, bool focused, const MetaState& m,
              const DeckState& d) {
    (void)focused;
    const Rect in = contentRect(r);
    if (in.h < 3 || in.w < 20) return;
    int cx = in.x + 2;
    int labelW = 7;
    int valX = cx + labelW + 2;
    int valW = in.x + in.w - 1 - valX;
    if (valW < 8) valW = 8;

    bool has = m.loaded;
    struct Row { const char* label; std::string value; Color col; };
    std::vector<Row> rows;
    rows.push_back({ "Track",  has ? m.meta.title  : "--", has ? P.text : P.dim });
    rows.push_back({ "Artist", has ? m.meta.artist : "--", has ? P.text : P.dim });
    rows.push_back({ "Album",  has ? m.meta.album  : "--", has ? P.text : P.dim });
    rows.push_back({ "Genre",  has ? m.meta.genre  : "--", has ? P.text : P.dim });
    if (!has)                 rows.push_back({ "BPM", "-- BPM", P.dim });
    else if (m.computingBpm)  rows.push_back({ "BPM", "… analyzing", P.accentA });
    else if (m.meta.bpm < 0)  rows.push_back({ "BPM", "-- BPM", P.dim });
    else {
        char v[24]; std::snprintf(v, sizeof v, "%.1f BPM", m.meta.bpm);
        rows.push_back({ "BPM", v, P.accentA });
    }
    rows.push_back({ "Format", has ? m.meta.statusVerbose : "--", P.dim });

    char volb[8]; std::snprintf(volb, sizeof volb, "%d%%", int(d.vol * 100.f + 0.5f));
    std::string st = d.dur > 0 ? (d.playing ? "PLAYING" : "PAUSED")
                               : (m.loaded ? "LOADED" : "IDLE");
    Color sc = d.dur > 0 ? (d.playing ? P.ok : P.warn) : P.dim;
    rows.push_back({ "Status", "[" + st + "]   Vol: " + volb, sc });

    // short dock (gap_out eats a row at 92x28): drop Format, then Album —
    // BPM and Status are the ones you actually read while playing
    if (in.h < int(rows.size()) && rows.size() > 6) rows.erase(rows.begin() + 5);
    if (in.h < int(rows.size()) && rows.size() > 5) rows.erase(rows.begin() + 2);

    // centre the block: spare rows become an even margin top and bottom
    const int n = std::min<int>(in.h, int(rows.size()));
    const int y = in.y + std::max(0, (in.h - n) / 2);
    for (int i = 0; i < n; ++i) {
        putTrunc(cx, y + i, labelW, rows[size_t(i)].label, P.label, NONE);
        term.setCell(cx + labelW, y + i, U':', P.dimmer, NONE);   // one gap before the value
        putTrunc(valX, y + i, valW, rows[size_t(i)].value, rows[size_t(i)].col, NONE);
    }
}

// ---------------------------------------------------------------------------
// browser tree (paints into a Canvas so the crossfade can dither between
// the tree and the spectrum without restyling)
// ---------------------------------------------------------------------------
static void paintTree(Canvas& cv, const Palette& P, bool focused, const Library& lib,
                      int selected, int top, const std::string& playingPath) {
    (void)focused;
    if (cv.w <= 0 || cv.h <= 0) return;
    auto rows = lib.visible(top, cv.h);
    for (int i = 0; i < int(rows.size()); ++i) {
        int idx = rows[size_t(i)];
        const TreeNode& n = lib.at(idx);
        int indent = lib.depth(idx);
        // top level starts on the same column as METADATA's labels (content
        // inset 2) so the two stacked docks line up; each level indents by 2
        int x = cv.x + (indent > 0 ? indent * 2 : 1);
        int y = cv.y + i;
        bool isSel = idx == selected;
        bool isPlaying = !playingPath.empty() && !n.isDir && n.path.string() == playingPath;

        Color c;
        char32_t glyph;
        if (n.isDir) {
            glyph = n.expanded ? U'▾' : U'▸';
            c = isSel ? P.accentA : P.dim;
        } else if (n.loading) {
            glyph = U'…'; c = P.dim;
        } else if (!n.playable()) {
            glyph = U'•'; c = P.dimmer;
        } else {
            glyph = U'•';
            c = isSel ? P.accentA : P.text;
        }
        if (isPlaying) c = P.accentB;
        cv.set(x, y, glyph, c, NONE);
        if (isPlaying) cv.set(x - 1, y, U'►', P.accentB, NONE);

        std::string name = n.name;
        if (n.isDir) name = "[D] " + name;
        if (n.loading) name = "[LOAD] " + name;
        // width-aware and ellipsised: CJK/katakana names consume two columns each
        cv.textTrunc(x + 1, y, cv.x + cv.w - 1 - (x + 1), decode(name), c, NONE);
    }
}

void browser(const Rect& r, const Palette& P, bool focused, const Library& lib,
             int selected, int scroll, const MetaState& m) {
    if (r.w < 12 || r.h < 3) return;
    const Rect in = contentRect(r);
    Canvas cv;
    cv.init(in.x, in.y, in.w, in.h);
    paintTree(cv, P, focused, lib, selected, scroll, m.playingPath);
    for (int y = 0; y < cv.h; ++y)
        for (int x = 0; x < cv.w; ++x) {
            const WCell& c = cv.cells[size_t(y) * cv.w + x];
            if (c.cont) continue;                       // right half of a wide glyph
            if (c.ch != U' ' || c.fg != NONE || c.bg != NONE)
                term.setCell(in.x + x, in.y + y, c.ch, c.fg, c.bg);
        }
}

int browserRowToNode(const Rect& r, const Library& lib, int selected, int top, int rowY) {
    const Rect in = contentRect(r);
    int contentTop = in.y;
    int h = in.h;
    if (rowY < contentTop || rowY >= contentTop + h) return -1;
    (void)selected;
    auto rows = lib.visible(top, h);
    int i = rowY - contentTop;
    if (i < 0 || i >= int(rows.size())) return -1;
    return rows[size_t(i)];
}

// ---------------------------------------------------------------------------
// cover art: 1:1 half-block pixel canvas (▀ with fg=top px, bg=bottom px)
// ---------------------------------------------------------------------------
void cover(const Rect& r, const Palette& P, bool focused, CoverState& c, const TrackMeta& m) {
    (void)m; (void)focused;
    const Rect in = contentRect(r);
    if (in.w < 6 || in.h < 3) return;
    int availW = in.w - 1;
    int availH = in.h;
    if (availW < 4 || availH < 2) return;

    int rowsN = std::min(availH, availW / 2);
    int colsN = rowsN * 2;
    if (colsN < 4 || rowsN < 2) return;

    int ox = in.x + (in.w - colsN) / 2;
    int oy = in.y + (availH - rowsN) / 2;

    if (c.src.valid()) {
        if (c.renderedCols != colsN || !c.rendered.valid()) {
            c.rendered = Media::squareResize(c.src, colsN);
            c.renderedCols = colsN;
        }
    } else {
        c.rendered = {};
    }

    // ---- chafa engine (customizable symbols + truecolor, fallback below) ----
    const ArtConfig& ac = artConfig();
    const bool asciiArt = ac.symbols == ChafaSymbols::Ascii || ac.symbols == ChafaSymbols::Ramp;
    if (ac.engine == ArtEngine::Chafa && (chafaAvailable() || asciiArt) && c.src.valid()) {
        if (c.artCells.empty() || c.artCols != colsN || c.artRows != rowsN || c.artDirty) {
            // feed chafa a HIGHER-res crop (8 px per cell): handing it the
            // 1-px-per-cell bitmap made it render a blurry upscale.
            int sample = std::min(colsN * 8, 512);
            if (c.artSrcCols != sample || !c.artSrc.valid()) {
                // art_fit = true crops to a square; false keeps the source
                // aspect and lets chafa letterbox inside the panel.
                c.artSrc = cfg().artFit ? Media::squareResize(c.src, sample) : c.src;
                c.artSrcCols = sample;
            }
            const CoverPixels& feed = c.artSrc.valid() ? c.artSrc : c.src;
            // ASCII art is drawn by the built-in ramp renderer: chafa only uses
            // glyphs it has coverage data for, which collapses a punctuation
            // ramp to two characters.
            c.artCells = asciiArt
                ? asciiArtRender(feed.px.data(), feed.w, feed.h, colsN, rowsN, ac.ramp)
                : chafaRender(feed.px.data(), feed.w, feed.h, colsN, rowsN);
            c.artCols = colsN; c.artRows = rowsN; c.artDirty = false;
        }
        if (int(c.artCells.size()) >= colsN * rowsN) {
            for (int ry = 0; ry < rowsN; ++ry) {
                int rx = 0;
                while (rx < colsN) {
                    const ArtCell& a = c.artCells[size_t(ry) * size_t(colsN) + size_t(rx)];
                    if (a.ch == 0) { ++rx; continue; }
                    term.setCell(ox + rx, oy + ry, a.ch, a.fg, a.hasBg ? a.bg : NONE);
                    int adv = cpWidth(a.ch);
                    if (adv < 1) adv = 1;
                    rx += adv;
                }
            }
            return;
        }
    }

    if (c.rendered.valid()) {
        const auto& px = c.rendered.px;
        for (int ry = 0; ry < rowsN; ++ry)
            for (int rx = 0; rx < colsN; ++rx) {
                size_t t = (size_t(ry * 2) * colsN + rx) * 3;
                size_t b = (size_t(ry * 2 + 1) * colsN + rx) * 3;
                Color fg = (Color(px[t]) << 16) | (Color(px[t + 1]) << 8) | Color(px[t + 2]);
                Color bg = (Color(px[b]) << 16) | (Color(px[b + 1]) << 8) | Color(px[b + 2]);
                term.setCell(ox + rx, oy + ry, U'▀', fg, bg);
            }
        return;
    }

    // placeholder card
    for (int y = 0; y < rowsN; ++y)
        for (int x = 0; x < colsN; ++x) {
            float g = float(y) / float(rowsN);
            if (rowsN == 1) g = 0;
            Color c2 = mix(P.surface, mix(P.accentA, P.accentB, g), 0.20f + 0.06f * float(x) / std::max(1, colsN));
            bool border = (x == 0 || y == 0 || x == colsN - 1 || y == rowsN - 1);
            if (border) c2 = mix(c2, P.frameHi, 0.55f);
            term.setCell(ox + x, oy + y, U' ', c2, c2);
        }
    if (rowsN >= 5 && colsN >= 8) {
        Color cc = mix(P.accentA, P.accentB, 0.5f);
        char32_t art = c.loading ? U'…' : U'♪';
        term.setCell(ox + colsN / 2, oy + rowsN / 2, art, cc, mix(P.surface, P.accentA, 0.10f));
        if (c.loading)
            putTrunc(ox + colsN / 2 - 3, oy + rowsN / 2 + 1, std::min(7, colsN - 2), "reading", cc,
                     mix(P.surface, P.accentA, 0.10f));
    }
}

// ---------------------------------------------------------------------------
// transport deck + scrub bar
// ---------------------------------------------------------------------------
void deck(const Rect& r, const Palette& P, bool focused, const DeckState& d, DeckHits& h,
          bool withPlay) {
    const GlyphSet& G = GlyphTheme::get();
    h = DeckHits{};
    if (r.h < 5 || r.w < 30) return;
    const Rect q = contentRect(r);

    // ---------- CINEMA MODE (Expanded tier): vector buttons + big clock ------
    // q.w threshold used to be 96, which the 46% left column made unreachable
    // until ~178 columns — so Expanded decks silently fell back to text. The
    // cinema band needs ~70 columns; anything wider than that gets it.
    if (evaluateTier(term.width(), term.height()) == DisplayTier::Expanded && r.h >= 12 && r.w >= 72) {
        // one content inset for the whole deck: INL..INR are the first/last
        // usable columns, so every row starts and ends on the same gutter
        const int INL = q.x + 2;
        const int INR = q.x + q.w - 3;
        int ry = q.y + q.h - 1;          // status row (last interior row)
        int sy = ry - 1;                 // scrub row, one blank row above status
        // big clock: 5-row band, centred in the free rows above the scrub
        int clockY = q.y + std::max(0, ((sy - 1) - q.y + 1 - BigGlyphs::ROWS) / 2);

        std::string cl = mmss(d.pos);
        if (d.dur > 0 && cl.size() < 5) cl = "0" + cl; // pad mm:ss
        drawBigClock(q.x + (q.w - 11 - 1 - bigClockWidth(int(cl.size()))) / 2, clockY, cl,
                     mix(P.accentA, P.accentB, float(d.pos > 0 ? std::fmod(d.pos, 8.0) / 8.0 : 0.f)), P.surface);

        // seek flanks: single-line left zone
        auto cw = [](const std::string& s) { return widthOf(s); };
        std::string pl = "[ " + std::string(G.seek_back) + " 10s ]";
        putTrunc(INL, clockY + 2, cw(pl), pl, P.dim, NONE);
        h.prev = Rect{ INL, clockY, cw(pl), BigGlyphs::ROWS };
        std::string nl = "[ 10s " + std::string(G.seek_fwd) + " ]";
        int nx = INR - cw(nl) + 1;
        putTrunc(nx, clockY + 2, cw(nl), nl, P.dim, NONE);
        h.next = Rect{ nx, clockY, cw(nl), BigGlyphs::ROWS };

        // scrub row (identical gutter math to the status row)
        putTrunc(INL, sy, 6, mmss(d.pos), P.accentA, NONE);
        putTrunc(INR - 5, sy, 6, d.dur > 0 ? mmss(d.dur) : std::string("--:--"), P.dim, NONE);
        int barX = INL + 7;
        int barLen = (INR - 7) - barX + 1;
        if (barLen > 3) {
            h.scrub = Rect{ barX, sy, barLen, 1 };
            float frac = d.dur > 0 ? float(d.pos / d.dur) : 0.f;
            frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
            int knob = int(frac * float(barLen - 1) + 0.5f);
            Color knobC = mix(P.accentA, P.accentB, 0.5f);
            for (int i = 0; i < barLen; ++i) {
                float t = float(i) / float(barLen - 1);
                if (i == knob) term.setCell(barX + i, sy, U'●', knobC, NONE);
                else if (i < knob) term.setCell(barX + i, sy, U'━', mix(P.accentA, P.accentB, t * 0.8f), NONE);
                else term.setCell(barX + i, sy, U'─', P.dimmer, NONE);
            }
        }

        // status row
        std::string shufL = std::string(G.shuffle) + " SHUF";
        std::u32string su = decode(shufL);
        drawText(TermSink{}, INL, ry, q.x + q.w - 2 - INL, su, d.shuffle ? P.accentB : P.dim, NONE);
        h.shuffle = Rect{ INL, ry, widthOf(su) + 1, 1 };
        const char* reps[3] = { "OFF", "ALL", "ONE" };
        std::string repL = std::string(G.repeat) + " REP:" + reps[std::clamp(d.repeat, 0, 2)];
        int repX = INL + widthOf(su) + 3;
        std::u32string ru = decode(repL);
        drawText(TermSink{}, repX, ry, q.x + q.w - 2 - repX, ru, d.repeat ? P.accentA : P.dim, NONE);
        h.repeat = Rect{ repX - 1, ry, widthOf(ru) + 2, 1 };

        int blocks = 8;
        char vt[8]; std::snprintf(vt, sizeof vt, "%3d%%", int(d.vol * 100.f + 0.5f));
        const int vtW = widthOf(vt);
        const int vtX = INR - vtW + 1;          // value ends on the right gutter
        int vbX = vtX - 1 - blocks;
        int lblW = widthOf(G.vol_label);
        putTrunc(vbX - lblW - 1, ry, lblW, std::string(G.vol_label), P.label, NONE);
        int fill = int(d.vol * float(blocks) + 0.5f);
        for (int i = 0; i < blocks; ++i) {
            bool on = i < fill;
            float t = float(i) / float(blocks - 1);
            term.setCell(vbX + i, ry, on ? U'▇' : U'░', on ? mix(P.accentA, P.accentB, t) : P.dimmer, NONE);
        }
        putTrunc(vtX, ry, vtW, vt, P.dim, NONE);
        h.volume = Rect{ vbX, ry, blocks, 1 };
        return;
    }

    // ---------- STANDARD MODE ----------
    // same 2-cell gutter as the cinema deck and the metadata/tree content
    int cx = q.x + 2;
    int x2 = q.x + q.w - 3;

    std::string prevL = "[ " + std::string(G.seek_back) + " 10s ]";
    std::string playL = d.playing ? "[ " + std::string(G.pause) + " PAUSE ]"
                                  : "[ " + std::string(G.play) + " PLAY ]";
    std::string nextL = "[ 10s " + std::string(G.seek_fwd) + " ]";
    // Flat inline row - no frame (the play button has its own panel now).
    int gap = 2;
    std::vector<std::pair<std::string, Color>> btns;
    if (withPlay) {
        btns.push_back({ prevL, P.dim });
        btns.push_back({ playL, d.playing ? P.accentA : P.ok });
        btns.push_back({ nextL, P.dim });
    } else {
        btns.push_back({ prevL, P.dim });
        btns.push_back({ nextL, P.dim });
    }
    int totalW = gap * int(btns.size() - 1);
    for (auto& b : btns) totalW += widthOf(b.first);
    if (totalW > q.w - 4) gap = 1;
    totalW = gap * int(btns.size() - 1);
    for (auto& b : btns) totalW += widthOf(b.first);
    int bx = q.x + (q.w - std::min(totalW, q.w - 4)) / 2 + 1;
    // centre the controls block vertically (the deck is tall; the status row
    // owns the last line)
    int by = q.y + std::max(0, (q.h - 1 - 3) / 2);
    for (size_t i = 0; i < btns.size(); ++i) {
        int bw = widthOf(btns[i].first);
        putTrunc(bx, by, q.w - (bx - q.x) - 1, btns[i].first, btns[i].second, NONE);
        bool isPrev = (i == 0);
        bool isPlay = withPlay && i == 1;
        bool isNext = (!withPlay && i == 1) || (withPlay && i == 2);
        Rect box = Rect{ bx, by, bw, 1 };
        if (isPrev) h.prev = box;
        else if (isPlay) h.play = box;
        else if (isNext) h.next = box;
        bx += bw + gap;
    }
    int sy = by + 2;

    // scrub row
    if (sy >= q.y + q.h - 1) return;
    putTrunc(cx, sy, 6, mmss(d.pos), P.accentA, NONE);
    putTrunc(x2 - 5, sy, 6, d.dur > 0 ? mmss(d.dur) : std::string("--:--"), P.dim, NONE);
    int barX = cx + 7;
    int barLen = (x2 - 6) - barX + 1;
    if (barLen > 3) {
        h.scrub = Rect{ barX, sy, barLen, 1 };
        float frac = d.dur > 0 ? float(d.pos / d.dur) : 0.f;
        frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
        int knob = int(frac * float(barLen - 1) + 0.5f);
        if (d.dur <= 0) knob = 0;
        Color knobC = mix(P.accentA, P.accentB, 0.5f);
        for (int i = 0; i < barLen; ++i) {
            float t = float(i) / float(barLen - 1);
            if (i == knob) term.setCell(barX + i, sy, U'●', knobC, NONE);
            else if (i < knob) term.setCell(barX + i, sy, U'━', mix(P.accentA, P.accentB, t * 0.8f), NONE);
            else term.setCell(barX + i, sy, U'─', P.dimmer, NONE);
        }
    }

    // status row
    int ry = q.y + q.h - 1;
    if (ry <= sy) return;
    int vx = cx;
    std::string shufL = std::string(G.shuffle) + " SHUF";
    std::u32string su = decode(shufL);
    drawText(TermSink{}, vx, ry, x2 - vx, su, d.shuffle ? P.accentB : P.dim, NONE);
    if (d.shuffle) term.setCell(vx - 1, ry, U'●', P.accentB, NONE);
    h.shuffle = Rect{ vx - 1, ry, widthOf(su) + 2, 1 };

    const char* reps[3] = { "OFF", "ALL", "ONE" };
    std::string repL = std::string(G.repeat) + " REP:" + reps[std::clamp(d.repeat, 0, 2)];
    int repX = vx + widthOf(su) + 3;
    std::u32string ru = decode(repL);
    drawText(TermSink{}, repX, ry, x2 - repX, ru, d.repeat ? P.accentA : P.dim, NONE);
    h.repeat = Rect{ repX - 1, ry, widthOf(ru) + 2, 1 };

    // volume bar right-aligned on the same gutter as the duration
    int blocks = 6;
    std::string vl = std::string(G.vol_label) + " ";
    std::u32string vu = decode(vl);
    char vt[8]; std::snprintf(vt, sizeof vt, "%3d%%", int(d.vol * 100.f + 0.5f));
    const int vtW = widthOf(vt);
    const int vtX = x2 - vtW + 1;          // value ends on the right gutter
    int vbX = vtX - 1 - blocks;            // bar, one blank cell before the value
    int viX = vbX - widthOf(vu);
    if (viX > repX + widthOf(ru) + 2) {
        drawText(TermSink{}, viX, ry, x2 - viX + 1, vu, P.label, NONE);
        int fill = int(d.vol * float(blocks) + 0.5f);
        for (int i = 0; i < blocks; ++i) {
            bool on = i < fill;
            float t = float(i) / float(blocks - 1);
            Color c = on ? mix(P.accentA, P.accentB, t) : P.dimmer;
            term.setCell(vbX + i, ry, on ? U'▇' : U'░', c, NONE);
        }
        drawText(TermSink{}, vtX, ry, vtW, decode(vt), P.dim, NONE);
        h.volume = Rect{ vbX, ry, blocks, 1 };
    }
}

// ---------------------------------------------------------------------------
// play column: the whole panel is one button
// ---------------------------------------------------------------------------
void player(const Rect& r, const Palette& P, bool focused, const DeckState& d, DeckHits& h) {
    if (r.w < 5 || r.h < 4) return;
    const Rect q = contentRect(r);
    if (q.w < 3 || q.h < 2) return;

    auto art = d.playing ? BigGlyphs::pause() : BigGlyphs::play();
    const int artW = widthOf(art[0]);
    const int artH = BigGlyphs::ROWS;
    Color c = d.playing ? mix(P.accentA, P.accentB, 0.5f) : P.ok;
    if (focused) c = mix(c, P.text, 0.20f);

    if (artW + 2 <= q.w && artH <= q.h) {
        int ox = q.x + (q.w - artW) / 2;
        int oy = q.y + (q.h - artH) / 2;

        // silhouette mask in COLUMNS (the art rows are multi-byte: never index
        // them by byte or codepoint, walk them with cpWidth)
        std::vector<char> mask(size_t(artH) * size_t(artW), 0);
        auto inMask = [&](int ry, int rx) -> char {
            return mask[size_t(ry) * size_t(artW) + size_t(rx)];
        };
        for (int row = 0; row < artH; ++row) {
            std::u32string u = decode(art[row]);
            int col = 0;
            for (char32_t ch2 : u) {
                int gw = cpWidth(ch2);
                if (gw < 1) continue;
                if (ch2 != U' ' && col < artW) mask[size_t(row) * size_t(artW) + size_t(col)] = 1;
                col += gw;
            }
        }

        // drop shadow: the silhouette shifted one cell up-right (light from
        // below-left), painted first so the glyphs sit on top of it. The shadow
        // reuses the art's OWN glyphs (▄/█/▀) so it hugs the shape instead of
        // showing the blocky steps a full '█' stamp produced. Clipped to the
        // dock interior - at small sizes the art starts on the first content
        // row and the shadow would otherwise smear the title tab.
        const Color sh = mix(P.surface, P.frame, 0.55f);
        for (int row = 0; row < artH; ++row) {
            std::u32string u = decode(art[row]);
            int col = 0;
            for (char32_t ch2 : u) {
                int gw = cpWidth(ch2);
                if (gw < 1) continue;
                if (ch2 != U' ') {
                    int sx = ox + col + 1, sy = oy + row - 1;
                    int mx = col + 1, my = row - 1;
                    bool covered = (mx >= 0 && mx < artW && my >= 0 && my < artH && inMask(my, mx));
                    if (!covered && sx >= q.x && sy >= q.y &&
                        sx < q.x + q.w && sy < q.y + q.h)
                        term.setCell(sx, sy, ch2, sh, NONE);
                }
                col += gw;
            }
        }

        for (int row = 0; row < artH; ++row) {
            std::u32string u = decode(art[row]);
            int col = 0;
            for (char32_t ch2 : u) {
                int gw = cpWidth(ch2);
                if (gw < 1) continue;
                if (ch2 != U' ') term.setCell(ox + col, oy + row, ch2, c, NONE);
                col += gw;
            }
        }
    } else {
        std::string lbl = d.playing ? "PAUSE" : "PLAY";
        int w = widthOf(lbl);
        if (w > q.w) { lbl = d.playing ? "||" : ">"; w = widthOf(lbl); }
        putTrunc(q.x + (q.w - w) / 2, q.y + q.h / 2, w, lbl, c, NONE);
    }
    h.play = Rect{ q.x, q.y, q.w, q.h };   // the whole panel is the target
}

float deckScrubToSeconds(const Rect&, const DeckHits& h, const DeckState& d, int mx) {
    if (h.scrub.w <= 1 || d.dur <= 0) return d.pos;
    int k = std::clamp(mx - h.scrub.x, 0, h.scrub.w - 1);
    return double(k) / double(h.scrub.w - 1) * d.dur;
}

float deckVolumeAt(const DeckHits& h, int mx) {
    if (h.volume.w <= 0) return -1.f;
    int k = std::clamp(mx - h.volume.x, 0, h.volume.w - 1);
    return float(k) / float(h.volume.w - 1);
}

// ---------------------------------------------------------------------------
// CAVA spectrum (8-step block ramp + bass/mid/treble ruler)
// ---------------------------------------------------------------------------
static const char32_t B8[9] = { U' ', U'▁', U'▂', U'▃', U'▄', U'▅', U'▆', U'▇', U'█' };

// bars may bleed over the panel's upper border (hyper overlap):
// painter anchors bars to the ground row; `barMax` rows available
// (caller adds overflow rows to the canvas and blends those cells).
static void paintSpectrum(Canvas& cv, const Palette& P, const std::array<float, 32>& bars,
                          int barMax, int rulerIdx) {
    (void)barMax; (void)rulerIdx;
    if (cv.w <= 0 || cv.h <= 1) return;
    // Layout of the dock, bottom up:
    //   row cv.h-1  -> Bass / Mid / Treble axis labels (right above the wall)
    //   row cv.h-2  -> the bars' baseline; everything above is signal
    const int labelRow = cv.h - 1;
    const int ground = cv.h - 2;
    const int heights = cv.h - 1;
    // Bars MUST span the whole width. The old `step = w / nbars` truncated
    // (90 cols / 32 bands -> step 2 -> bars stopped at col 64), which read as
    // "the treble third is missing". Distribute proportionally instead.
    int nbars = std::clamp(cv.w / 2, 2, 32);

    for (int b = 0; b < nbars; ++b) {
        float v;
        if (nbars == 32) v = bars[size_t(b)];
        else {
            float srcPos = float(b) * 32.f / float(nbars);
            int i0 = int(srcPos), i1 = std::min(i0 + 1, 31);
            float f = srcPos - float(i0);
            v = bars[size_t(i0)] * (1.f - f) + bars[size_t(i1)] * f;
        }
        float hgt = v * float(heights);
        int full = std::clamp(int(hgt), 0, ground + 1);
        float frac = hgt - float(full);
        Color col = mix(P.accentA, P.accentB, float(b) / float(std::max(1, nbars - 1)));
        // each band owns the full column range [x0,x1): bars touch, no gaps,
        // and the width scales with the terminal instead of staying 1 cell.
        int x0 = cv.x + int((long long)b * cv.w / nbars);
        int x1 = cv.x + int((long long)(b + 1) * cv.w / nbars);
        if (x1 <= x0) x1 = x0 + 1;
        for (int x = x0; x < x1 && x < cv.x + cv.w; ++x) {
            for (int ry = 0; ry < full; ++ry)
                cv.set(x, cv.y + ground - ry, U'█', col, NONE);
            if (frac > 0.05f && full <= ground) {
                int gi = std::clamp(int(frac * 8.f), 1, 7);
                cv.set(x, cv.y + ground - full, B8[gi], col, NONE);
            }
        }
    }
    // Axis labels on the baseline, painted last so they stay readable over the
    // bars (they used to be drawn first and got covered by signal).
    {
        static const std::u32string LB = decode("Bass"), LM = decode("Mid"), LT = decode("Treble");
        Color fg = mix(P.dimmer, P.text, 0.70f);
        Color bgl = mix(P.surface, 0x000000, 0.20f);
        int ly = cv.y + labelRow;
        auto tag = [&](const std::u32string& t, int x) {
            for (size_t i = 0; i < t.size() && x + int(i) < cv.x + cv.w; ++i)
                cv.set(x + int(i), ly, t[i], fg, bgl);
        };
        tag(LB, cv.x);
        tag(LM, cv.x + cv.w / 2 - int(LM.size()) / 2);
        tag(LT, cv.x + cv.w - int(LT.size()));
    }

}

void spectrum(const Rect& r, const Palette& P, bool focused,
              const std::array<float, 32>& bars, const DeckState& d,
              const std::vector<float>* wave) {
    (void)focused;
    if (r.w < 10 || r.h < 5) return;
    const Rect p = contentRect(r);

    // ---- overflow: bars bleed over the upper border while playing ----
    // (contentRect already accounts for the frame + title row: no extra +2, or
    // the whole visualizer slides down and the ruler lands outside the panel)
    int interiorTop = p.y;
    int rows = p.h;                       // interior incl ruler row
    int scopeRows = 0;
    if (wave && !wave->empty() && p.w > 130 && rows > 8) { scopeRows = 3; rows -= 3; }
    bool ov = d.playing && rows > 6 && scopeRows == 0;
    int ext = ov ? 1 : 0;                     // ONE row of bleed over the frame
    int contentH = rows;
    int barMax = contentH - 1 + ext;
    int rulerIdx = contentH - 1;

    // Canvas for the bars. `rows` already had scopeRows removed above, so the
    // canvas must NOT subtract them again - doing that pushed the whole
    // spectrum (bars AND labels) three rows off the bottom wall on wide
    // terminals, which is the "bars are spaced from the bottom" bug. Start the
    // canvas below the oscilloscope strip and keep `rows` (+ext bleed) rows.
    Canvas cv;
    // same 2-cell gutter as every other dock's content: bars start on the
    // column the tree bullets / metadata labels use and stop one short of the
    // right frame, instead of running frame to frame
    const int gut = (p.w >= 12) ? 2 : 0;
    const int cw2 = std::max(4, p.w - 2 * gut);
    cv.init(p.x + gut, interiorTop + scopeRows - ext, cw2, contentH + ext);
    paintSpectrum(cv, P, bars, barMax, rulerIdx);

    // braille oscilloscope strip (Expanded tier): poly-line of latest mono samples
    if (scopeRows) {
        BrailleCanvas bc(static_cast<size_t>(cw2), static_cast<size_t>(scopeRows));
        float mid = float(bc.height() / 2);
        float amp = float(bc.height()) / 2.f - 1.f;
        int lastX = -1, lastY = int(mid);
        for (size_t i = 0; i < wave->size(); ++i) {
            float f = wave->data()[i];
            float s = f * 2.2f;
            if (s > 1) s = 1;
            if (s < -1) s = -1;
            int x = int(float(i) * float(bc.width() - 1) / float(std::max<size_t>(1, wave->size() - 1)));
            int y = int(mid - s * amp);
            y = std::clamp(y, 0, bc.height() - 1);
            if (lastX >= 0 && (x != lastX || y != lastY)) bc.draw_line(lastX, lastY, x, y);
            else bc.set_pixel(x, y);
            lastX = x; lastY = y;
        }
        for (int x = 0; x < bc.width(); x += (bc.width() / 12 > 0 ? bc.width() / 12 : 1))
            bc.set_pixel(x, bc.height() / 2);
        bc.render(p.x + gut, p.y, mix(P.accentA, P.accentB, 0.35f));
    }

    // blit canvas back (diff-buffer picks the churn up per-frame)
    for (int y = 0; y < cv.h; ++y)
        for (int x = 0; x < cv.w; ++x) {
            const WCell& c = cv.cells[size_t(y) * cv.w + x];
            if (c.cont) continue;
            if (c.ch == U' ' && c.fg == NONE && c.bg == NONE) continue;
            int ty = cv.y + y;
            if (ty < interiorTop) {
                // above the panel content: bleed over the frame line only, and
                // never over text (the title tab lives on that row).
                char32_t under = term.peek(cv.x + x, ty);
                bool frameCell = (under == U'─' || under == U'∙' || under == U'╭' ||
                                  under == U'╮' || under == U' ' || under == 0);
                if (!frameCell) continue;
                float a = 0.85f;
                term.blendCell(cv.x + x, ty, c.ch, c.fg, a);
            } else {
                term.setCell(cv.x + x, ty, c.ch, c.fg, c.bg);
            }
        }
}

// ---------------------------------------------------------------------------
// idle crossfade: ordered-dither blend of tree -> spectrum, eased t in [0,1]
// ---------------------------------------------------------------------------
void crossfadeBrowser(const Rect& r, const Palette& P, bool focused,
                      const Library& lib, int selected, int top, const MetaState& m,
                      const DeckState& d, const std::array<float, 32>& bars,
                      float t, std::vector<WCell>& scratchA,
                      std::vector<WCell>& scratchB) {
    (void)d; (void)m;
    if (r.w < 12 || r.h < 6) return;
    const Rect in = contentRect(r);
    Canvas A, B;
    A.init(in.x, in.y, in.w, in.h);
    B.init(A.x, A.y, A.w, A.h);
    (void)scratchA; (void)scratchB; // kept for API stability; canvases are local

    paintTree(A, P, focused, lib, selected, top, m.playingPath);
    paintSpectrum(B, P, bars, B.h - 2, B.h - 1);

    static const int bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
    for (int y = 0; y < A.h; ++y)
        for (int x = 0; x < A.w; ++x) {
            const WCell& a = A.cells[size_t(y) * A.w + x];
            const WCell& b = B.cells[size_t(y) * B.w + x];
            float thr = (float(bayer[(y & 3) * 4 + (x & 3)]) + 0.5f) / 16.f;
            bool useSpec = t > thr;
            Color fg = useSpec ? mix(a.fg, b.fg, std::min(1.f, t + 0.35f))
                               : mix(b.fg, a.fg, std::min(1.f, (1.f - t) + 0.2f));
            term.setCell(A.x + x, A.y + y, useSpec ? b.ch : a.ch, fg, NONE);
        }
}

} // namespace W
} // namespace qp
