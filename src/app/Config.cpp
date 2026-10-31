#include "Config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace qp {

Config& cfg() {
    static Config c;
    return c;
}

namespace {

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(uint8_t(c)));
    return s;
}

bool parseBool(const std::string& v, bool& out) {
    std::string s = lower(trim(v));
    if (s == "1" || s == "true" || s == "yes" || s == "on") { out = true; return true; }
    if (s == "0" || s == "false" || s == "no" || s == "off") { out = false; return true; }
    return false;
}

bool parseColor(const std::string& v, int& out) {
    std::string s = trim(v);
    if (!s.empty() && s[0] == '#') s = s.substr(1);
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
    if (s.size() != 6) return false;
    for (char c : s) if (!std::isxdigit(uint8_t(c))) return false;
    out = int(std::strtol(s.c_str(), nullptr, 16));
    return true;
}

// "#RRGGBB" with a 3-digit shorthand, used by the skeleton writer
std::string hexOf(Color c) {
    char b[16];
    std::snprintf(b, sizeof b, "#%06X", unsigned(c) & 0xFFFFFFu);
    return b;
}

std::string getEnv(const char* k) {
    const char* v = std::getenv(k);
    return v ? std::string(v) : std::string();
}

} // namespace

std::string Config::defaultPath() {
    std::string p = getEnv("QPANEL_CONFIG");
    if (!p.empty()) return p;
    std::string xdg = getEnv("XDG_CONFIG_HOME");
    if (xdg.empty()) {
        const char* home = std::getenv("HOME");
        if (home) xdg = std::string(home) + "/.config";
    }
    if (!xdg.empty()) return xdg + "/qpanel/config.ini";
    return "config.ini";
}

bool Config::load(const std::string& path, std::vector<std::string>* warnings) {
    sourcePath = path;
    std::ifstream in(path);
    if (!in) return false;

    auto warn = [&](const std::string& m) { if (warnings) warnings->push_back(m); };
    auto bad = [&](const std::string& key, const std::string& val) {
        warn("config: bad value for " + key + ": '" + val + "' (ignored)");
    };

    std::string section = "general";
    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        std::string s = trim(line);
        if (s.empty() || s[0] == '#' || s[0] == ';') continue;
        if (s.front() == '[') {
            size_t close = s.find(']');
            if (close == std::string::npos) { warn("config: unterminated section on line " + std::to_string(lineno)); continue; }
            section = lower(trim(s.substr(1, close - 1)));
            continue;
        }
        size_t eq = s.find('=');
        if (eq == std::string::npos) { warn("config: missing '=' on line " + std::to_string(lineno)); continue; }
        std::string key = lower(trim(s.substr(0, eq)));
        std::string val = trim(s.substr(eq + 1));
        // strip a trailing inline comment: '#'/';' only counts when preceded by
        // whitespace, so "#RRGGBB" (no leading space) survives intact.
        for (size_t i = 1; i < val.size(); ++i) {
            if ((val[i] == '#' || val[i] == ';') && (val[i - 1] == ' ' || val[i - 1] == '\t')) {
                val = trim(val.substr(0, i));
                break;
            }
        }
        if (key.empty()) { warn("config: empty key on line " + std::to_string(lineno)); continue; }

        auto asInt = [&](int& dst, int lo, int hi) {
            try {
                int v = std::stoi(val);
                if (v < lo || v > hi) return false;
                dst = v;
                return true;
            } catch (...) { return false; }
        };
        auto asFloat = [&](float& dst, float lo, float hi) {
            try {
                float v = std::stof(val);
                if (v < lo || v > hi) return false;
                dst = v;
                return true;
            } catch (...) { return false; }
        };

        if (section == "general") {
            if (key == "theme") theme = val;
            else if (key == "icons") icons = val;
            else if (key == "volume") { if (!asFloat(volume, 0.f, 1.f)) bad(key, val); }
            else if (key == "idle_timeout") { if (!asFloat(idleTimeout, 1.f, 3600.f)) bad(key, val); }
            else warn("config: unknown key [" + section + "] " + key);
        } else if (section == "layout") {
            if (key == "gap_out") { if (!asInt(gapOut, 0, 8)) bad(key, val); }
            else if (key == "gap_in") { if (!asInt(gapIn, 0, 8)) bad(key, val); }
            else if (key == "pad_x") { if (!asInt(padX, 0, 12)) bad(key, val); }
            else if (key == "pad_y") { if (!asInt(padY, 0, 8)) bad(key, val); }
            else if (key == "shadow") { if (!parseBool(val, shadow)) bad(key, val); }
            else if (key == "art_panel") { if (!parseBool(val, artPanel)) bad(key, val); }
            else if (key == "art_split") { if (!asInt(artSplit, 20, 80)) bad(key, val); }
            else if (key == "art_fit") { if (!parseBool(val, artFit)) bad(key, val); }
            else if (key == "border") {
                std::string b = lower(val);
                if (b == "rounded") border = BorderStyle::Rounded;
                else if (b == "single" || b == "plain") border = BorderStyle::Single;
                else if (b == "double") border = BorderStyle::Double;
                else if (b == "heavy" || b == "thick") border = BorderStyle::Heavy;
                else if (b == "ascii") border = BorderStyle::Ascii;
                else if (b == "none") border = BorderStyle::None;
                else bad(key, val);
            }
            else warn("config: unknown key [" + section + "] " + key);
        } else if (section == "art" || section == "chafa") {
            if (key == "engine") {
                std::string v = lower(val);
                if (v == "chafa") art.engine = ArtEngine::Chafa;
                else if (v == "block" || v == "half" || v == "builtin") art.engine = ArtEngine::Block;
                else bad(key, val);
            } else if (key == "symbols") {
                std::string v = lower(val);
                if (v == "all") art.symbols = ChafaSymbols::All;
                else if (v == "block" || v == "half") art.symbols = ChafaSymbols::Block;
                else if (v == "sextant") art.symbols = ChafaSymbols::Sextant;
                else if (v == "braille") art.symbols = ChafaSymbols::Braille;
                else if (v == "ascii") art.symbols = ChafaSymbols::Ascii;
                else if (v == "ramp") art.symbols = ChafaSymbols::Ramp;
                else bad(key, val);
            } else if (key == "colors" || key == "mode") {
                std::string v = lower(val);
                if (v == "truecolor" || v == "24" || v == "24bit") art.mode = ChafaMode::TrueColor;
                else if (v == "256") art.mode = ChafaMode::Indexed256;
                else if (v == "16") art.mode = ChafaMode::Indexed16;
                else bad(key, val);
            } else if (key == "dither") {
                std::string v = lower(val);
                if (v == "none" || v == "off") art.dither = ChafaDither::None;
                else if (v == "ordered") art.dither = ChafaDither::Ordered;
                else if (v == "diffusion" || v == "fs" || v == "atkinson") art.dither = ChafaDither::Diffusion;
                else bad(key, val);
            } else if (key == "fallback") { artFallback = val; }
            else if (key == "ramp") {
                // leading space is implied; keep it to visible glyphs
                if (val.size() >= 2 && val.size() <= 24) art.ramp = val;
                else bad(key, val);
            }
            else if (key == "fg_only") { if (!parseBool(val, art.fgOnly)) bad(key, val); }
            else if (key == "gamma") { if (!asFloat(art.gamma, 0.1f, 4.f)) bad(key, val); }
            else warn("config: unknown key [" + section + "] " + key);
        } else if (section == "colors" || section == "colour" || section == "palette") {
            int v = -1;
            if (!parseColor(val, v)) { bad(key, val); continue; }
            if (key == "accent_a") cAccentA = v;
            else if (key == "accent_b") cAccentB = v;
            else if (key == "frame") cFrame = v;
            else if (key == "frame_hi") cFrameHi = v;
            else if (key == "surface") cSurface = v;
            else if (key == "text") cText = v;
            else if (key == "dim") cDim = v;
            else if (key == "label") cLabel = v;
            else if (key == "ok") cOk = v;
            else if (key == "warn") cWarn = v;
            else if (key == "err") cErr = v;
            else warn("config: unknown colour key " + key);
        } else {
            warn("config: unknown section [" + section + "]");
        }
    }

    // clamp sanity after parsing (values may have come from several keys)
    gapOut = std::clamp(gapOut, 0, 8);
    gapIn = std::clamp(gapIn, 0, 8);
    return true;
}

Palette Config::palette() const {
    const Palette& base = Palette::byName(theme);
    Palette p = base;
    if (cAccentA >= 0) p.accentA = Color(cAccentA);
    if (cAccentB >= 0) p.accentB = Color(cAccentB);
    if (cFrame   >= 0) p.frame   = Color(cFrame);
    if (cFrameHi >= 0) p.frameHi = Color(cFrameHi);
    if (cSurface >= 0) p.surface = Color(cSurface);
    if (cText    >= 0) p.text    = Color(cText);
    if (cDim     >= 0) p.dim     = Color(cDim);
    if (cLabel   >= 0) p.label   = Color(cLabel);
    if (cOk      >= 0) p.ok      = Color(cOk);
    if (cWarn    >= 0) p.warn    = Color(cWarn);
    if (cErr     >= 0) p.err     = Color(cErr);
    p.name = base.name;
    return p;
}

std::string Config::skeleton() {
    Config d;
    const Palette& p = d.palette();
    std::ostringstream o;
    o <<
"# QPanel configuration — every key is optional.\n"
"# Values here are the defaults; command-line flags still win.\n"
"# Lookup: --config PATH > $QPANEL_CONFIG > $XDG_CONFIG_HOME/qpanel/config.ini\n"
"#         > ~/.config/qpanel/config.ini > ./config.ini\n"
"# Comments use '#' or ';' on their own line (no inline comments: '#' starts a\n"
"# hex colour).\n"
"\n"
"[general]\n"
"# palette: hyper | catppuccin-mocha | catppuccin-macchiato | catppuccin-latte\n"
"#          | tokyo-night | rose-pine | gruvbox-material\n"
"theme = " << d.theme << "\n"
"# glyph tier: unicode (default) | ascii | nerd\n"
"icons = " << d.icons << "\n"
"volume = " << d.volume << "            # 0.0 .. 1.0\n"
"idle_timeout = " << d.idleTimeout << "       # seconds before the visualizer fades in\n"
"\n"
"[layout]\n"
"# Hyprland-style geometry, in terminal cells.\n"
"gap_out = " << d.gapOut << "             # margin around the whole UI (gap_in = even frame)\n"
"gap_in = " << d.gapIn << "              # space between panels\n"
"# cover-art column: false gives the tree/spectrum the full width\n"
"art_panel = " << (d.artPanel ? "true" : "false") << "\n"
"art_split = " << d.artSplit << "            # left column width, percent (20..80)\n"
"art_fit = " << (d.artFit ? "true" : "false") << "              # 1:1 square crop of the cover\n"
"pad_x = " << d.padX << "               # extra left/right padding inside panels\n"
"pad_y = " << d.padY << "               # extra top padding inside panels\n"
"# border: rounded | single | double | heavy | ascii | none\n"
"border = rounded\n"
"shadow = " << (d.shadow ? "true" : "false") << "            # soft drop shadow in the gaps\n"
"\n"
"[art]\n"
"# engine: chafa (default, needs libchafa at build time) | block (built-in)\n"
"engine = chafa\n"
"# symbols: block (half-blocks, best font coverage) | ascii (punctuation ramp)\n"
"#          | ramp (classic .:-=+*#%@) | sextant | braille | all\n"
"symbols = block\n"
"# colors: truecolor | 256 | 16\n"
"colors = truecolor\n"
"# dither: none | ordered | diffusion\n"
"dither = none\n"
"# cover shown when a track has no art: default (assets/default-cover.*)\n"
"# | none | /path/to/image.jpg\n"
"fallback = " << d.artFallback << "\n"
"# density ladder for symbols = ascii|ramp (a leading space is implied)\n"
"ramp = " << d.art.ramp << "\n"
"fg_only = false\n"
"gamma = 1.0\n"
"\n"
"[colors]\n"
"# Optional per-theme overrides. Delete a line to inherit from [general] theme.\n"
"accent_a = " << hexOf(p.accentA) << "\n"
"accent_b = " << hexOf(p.accentB) << "\n"
"frame    = " << hexOf(p.frame) << "\n"
"frame_hi = " << hexOf(p.frameHi) << "\n"
"surface  = " << hexOf(p.surface) << "\n"
"text     = " << hexOf(p.text) << "\n"
"dim      = " << hexOf(p.dim) << "\n"
"label    = " << hexOf(p.label) << "\n"
"ok       = " << hexOf(p.ok) << "\n"
"warn     = " << hexOf(p.warn) << "\n"
"err      = " << hexOf(p.err) << "\n";
    return o.str();
}

} // namespace qp
