// QPanel — text metrics: UTF-8 decode, display width, sanitizing.
// One cell is one terminal column, NOT one codepoint: East-Asian wide glyphs
// occupy two columns, combining marks zero. Everything that draws text must
// advance by cpWidth(), never by codepoint count — otherwise a CJK title
// shifts the rest of the row and bleeds through the chassis border.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace qp {

// East-Asian Wide/Fullwidth ranges (UAX #11) + emoji presentation blocks.
static inline bool isWideCp(char32_t cp) {
    return (cp >= 0x1100 && cp <= 0x115F) ||   // Hangul Jamo
           (cp >= 0x2E80 && cp <= 0x303E) ||   // CJK radicals / punctuation
           (cp >= 0x3041 && cp <= 0x33FF) ||   // kana, CJK compat
           (cp >= 0x3400 && cp <= 0x4DBF) ||   // CJK ext A
           (cp >= 0x4E00 && cp <= 0x9FFF) ||   // CJK unified
           (cp >= 0xA000 && cp <= 0xA4CF) ||   // Yi
           (cp >= 0xA960 && cp <= 0xA97F) ||   // Hangul Jamo ext A
           (cp >= 0xAC00 && cp <= 0xD7A3) ||   // Hangul syllables
           (cp >= 0xF900 && cp <= 0xFAFF) ||   // CJK compat ideographs
           (cp >= 0xFE10 && cp <= 0xFE19) ||   // vertical forms
           (cp >= 0xFE30 && cp <= 0xFE6F) ||   // CJK compat forms
           (cp >= 0xFF00 && cp <= 0xFF60) ||   // fullwidth forms
           (cp >= 0xFFE0 && cp <= 0xFFE6) ||   // fullwidth signs
           (cp >= 0x1B000 && cp <= 0x1B2FF) || // kana ext
           (cp >= 0x1F000 && cp <= 0x1FAFF) || // emoji / pictographs
           (cp >= 0x20000 && cp <= 0x3FFFD);   // CJK ext B..F
}

static inline bool isZeroWidthCp(char32_t cp) {
    return (cp >= 0x0300 && cp <= 0x036F) ||   // combining diacritics
           (cp >= 0x0483 && cp <= 0x0489) ||
           (cp >= 0x0591 && cp <= 0x05BD) ||
           (cp >= 0x0610 && cp <= 0x061A) ||
           (cp >= 0x064B && cp <= 0x065F) ||
           (cp >= 0x0900 && cp <= 0x0903) ||
           (cp >= 0x0E31 && cp <= 0x0E3A) ||
           (cp >= 0x1AB0 && cp <= 0x1AFF) ||
           (cp >= 0x1DC0 && cp <= 0x1DFF) ||
           (cp >= 0x200B && cp <= 0x200F) ||   // ZWSP/ZWJ/ZWNJ
           (cp >= 0x2060 && cp <= 0x2064) ||
           (cp >= 0x20D0 && cp <= 0x20FF) ||
           (cp >= 0xFE00 && cp <= 0xFE0F) ||   // variation selectors
           (cp >= 0xFE20 && cp <= 0xFE2F) ||
           (cp >= 0xE0100 && cp <= 0xE01EF);
}

// Terminal columns this codepoint occupies: 0, 1 or 2.
static inline int cpWidth(char32_t cp) {
    if (cp < 0x20 || cp == 0x7F) return 1;            // sanitized to space
    if (cp >= 0x80 && cp <= 0x9F) return 1;           // C1 controls
    if (isZeroWidthCp(cp)) return 0;
    if (isWideCp(cp)) return 2;
    return 1;
}

// Decode UTF-8 into codepoints. Invalid sequences become U+FFFD so a bad tag
// or filename can never emit raw bytes into the terminal stream.
static inline std::u32string decodeUtf8(std::string_view s) {
    std::u32string u;
    u.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        uint8_t c0 = uint8_t(s[i]);
        int need = c0 < 0x80 ? 1
                 : (c0 >> 5) == 0x6 ? 2
                 : (c0 >> 4) == 0xE ? 3
                 : (c0 >> 3) == 0x1E ? 4 : 0;
        bool bad = false;
        if (need == 0) { bad = true; }
        else if (i + size_t(need) > s.size()) { bad = true; }
        else {
            char32_t cp = c0 & (need == 1 ? 0x7F : need == 2 ? 0x1F : need == 3 ? 0x0F : 0x07);
            for (int k = 1; k < need; k++) {
                uint8_t ck = uint8_t(s[i + size_t(k)]);
                if ((ck & 0xC0) != 0x80) { bad = true; break; }
                cp = (cp << 6) | (ck & 0x3F);
            }
            if (!bad) {
                // sanitize: no control/escape bytes ever reach the terminal
                if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F)) cp = U' ';
                u += cp;
            }
        }
        if (bad) u += char32_t(0xFFFD);
        i += size_t(need > 0 ? need : 1);
    }
    return u;
}

// Columns a decoded run occupies.
static inline int widthOf(const std::u32string& u) {
    int w = 0;
    for (char32_t c : u) w += cpWidth(c);
    return w;
}

static inline int widthOf(std::string_view s) { return widthOf(decodeUtf8(s)); }

// Write a run into a cell sink, advancing by display width. The sink must
// provide set(int x, int y, char32_t ch, Color fg, Color bg); continuation
// columns are filled with ch=0 so blitters can skip them.
template <typename Sink, typename Col>
static inline int drawText(Sink&& sink, int x, int y, int maxCols,
                           const std::u32string& u, Col fg, Col bg) {
    if (maxCols <= 0) return 0;
    int used = 0;
    for (char32_t c : u) {
        int w = cpWidth(c);
        if (w == 0) continue;                       // combining: dropped
        if (used + w > maxCols) break;
        sink.set(x + used, y, c, fg, bg);
        if (w == 2) sink.set(x + used + 1, y, char32_t(0), fg, bg);
        used += w;
    }
    return used;
}

// Draw with an ellipsis when the run does not fit.
template <typename Sink, typename Col>
static inline int drawTextTrunc(Sink&& sink, int x, int y, int maxCols,
                                const std::u32string& u, Col fg, Col bg) {
    if (maxCols <= 0) return 0;
    if (widthOf(u) <= maxCols) return drawText(std::forward<Sink>(sink), x, y, maxCols, u, fg, bg);
    if (maxCols == 1) { sink.set(x, y, U'…', fg, bg); return 1; }
    int used = 0;
    for (char32_t c : u) {
        int w = cpWidth(c);
        if (w == 0) continue;
        if (used + w > maxCols - 1) break;
        sink.set(x + used, y, c, fg, bg);
        if (w == 2) sink.set(x + used + 1, y, char32_t(0), fg, bg);
        used += w;
    }
    sink.set(x + used, y, U'…', fg, bg);
    return used + 1;
}

} // namespace qp
