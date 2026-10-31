// QPanel — GlyphTheme: 3-tier icon strategy (never tofu, never emoji).
//  - safe unicode (default): U+25A0–U+25FF shapes + U+2500–U+257F box drawing.
//    All monochrome, all exactly 1 cell wide in virtually every terminal font.
//  - pure ascii: raw TTYs / serial / zero unicode font.
//  - nerd fonts: opt-in via config (--icons=nerd) for Hyprland riced setups.
#pragma once
#include <string_view>

namespace qp {

struct GlyphSet {
    std::string_view play;
    std::string_view pause;
    std::string_view seek_back;
    std::string_view seek_fwd;
    std::string_view dir_open;
    std::string_view dir_closed;
    std::string_view audio_file;
    std::string_view file_junk;      // unplayable file
    std::string_view scrub_fill;
    std::string_view scrub_empty;
    std::string_view scrub_head;
    std::string_view shuffle;
    std::string_view repeat;
    std::string_view vol_label;
    std::string_view note;           // cover placeholder
    std::string_view selected_marker;
    std::string_view frame_v;        // chassis vertical
    std::string_view frame_h;
    std::string_view frame_tl, frame_tr, frame_bl, frame_br;
};

// Safe Unicode DEFAULT (36 years of font support, 1-cell wide, theme-colored)
inline constexpr GlyphSet UnicodeGlyphs {
    .play          = "▶",
    .pause         = "▮▮",
    .seek_back     = "◀◀",
    .seek_fwd      = "▶▶",
    .dir_open      = "▾",
    .dir_closed    = "▸",
    .audio_file    = "•",
    .file_junk     = "-",
    .scrub_fill    = "━",
    .scrub_empty   = "─",
    .scrub_head    = "●",
    .shuffle       = "⇄",
    .repeat        = "↻",
    .vol_label     = "VOL",
    .note          = "♪",
    .selected_marker = "▶",   // prepended for the playing/selected track row
    .frame_v = "│", .frame_h = "─",
    .frame_tl = "╭", .frame_tr = "╮", .frame_bl = "╰", .frame_br = "╯",
};

// Pure ASCII fail-safe (TTY / SSH / fc-0)
inline constexpr GlyphSet AsciiGlyphs {
    .play          = ">",
    .pause         = "||",
    .seek_back     = "<<",
    .seek_fwd      = ">>",
    .dir_open      = "-",
    .dir_closed    = "+",
    .audio_file    = "*",
    .file_junk     = "x",
    .scrub_fill    = "=",
    .scrub_empty   = "-",
    .scrub_head    = "o",
    .shuffle       = "[S]",
    .repeat        = "[R]",
    .vol_label     = "V:",
    .note          = "^",
    .selected_marker = ">",
    .frame_v = "|", .frame_h = "-",
    .frame_tl = "+", .frame_tr = "+", .frame_bl = "+", .frame_br = "+",
};

// Nerd Fonts opt-in (icons = "nerd")
inline constexpr GlyphSet NerdGlyphs {
    .play          = "󰐊",
    .pause         = "󰏤",
    .seek_back     = "󰒮",
    .seek_fwd      = "󰒭",
    .dir_open      = "󰉖",
    .dir_closed    = "󰉋",
    .audio_file    = "󰎆",
    .file_junk     = "󱚩",
    .scrub_fill    = "━",
    .scrub_empty   = "─",
    .scrub_head    = "󰝥",
    .shuffle       = "󰒝",
    .repeat        = "󰑖",
    .vol_label     = "󰕾",
    .note          = "󰎈",
    .selected_marker = "󰐊",
    .frame_v = "│", .frame_h = "─",
    .frame_tl = "╭", .frame_tr = "╮", .frame_bl = "╰", .frame_br = "╯",
};

// active theme (set once at boot by App; read-only afterwards)
class GlyphTheme {
public:
    static const GlyphSet& get() { return *cur_; }
    static int set(const std::string& mode) {
        if (mode == "ascii")  { cur_ = &AsciiGlyphs;  return 0; }
        if (mode == "nerd")   { cur_ = &NerdGlyphs;   return 0; }
        if (mode == "unicode") { cur_ = &UnicodeGlyphs; return 0; }
        return -1;
    }
    static const char* name() {
        if (cur_ == &AsciiGlyphs) return "ascii";
        if (cur_ == &NerdGlyphs) return "nerd";
        return "unicode";
    }
private:
    inline static const GlyphSet* cur_ = &UnicodeGlyphs;
};

} // namespace qp
