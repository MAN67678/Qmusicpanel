// QPanel — configuration.
// A plain INI file (no dependencies). Every key is optional: the defaults below
// reproduce the built-in look, and command-line flags override the file.
//
//   lookup order:  --config <path>  >  $QPANEL_CONFIG  >
//                  $XDG_CONFIG_HOME/qpanel/config.ini  >  ~/.config/qpanel/config.ini
//                  >  ./config.ini
#pragma once
#include <string>
#include <vector>

#include "../media/ChafaRender.hpp"
#include "../ui/Palette.hpp"

namespace qp {

enum class BorderStyle { Rounded, Single, Double, Heavy, Ascii, None };

struct Config {
    // [general]
    std::string theme = "hyper";
    std::string icons = "unicode";      // unicode | ascii | nerd
    float volume = 0.85f;               // 0..1 starting volume
    float idleTimeout = 8.0f;           // seconds of no input -> visualizer

    // [layout]
    int gapOut = 1;                     // screen margin in cells (same as gap_in => even frame)
    int gapIn = 1;                      // gap between panels (gaps_in)
    int padX = 0;                       // extra horizontal interior padding
    int padY = 0;                       // extra vertical interior padding
    BorderStyle border = BorderStyle::Rounded;
    bool shadow = true;                 // drop shadow into the gaps

    // art panel (the cover column)
    bool artPanel = true;               // false = tree/spectrum gets the full width
    int  artSplit = 46;                 // left column width %
    bool artFit = true;                 // square 1:1 crop (false = letterbox)

    // [art] — chafa engine
    ArtConfig art;
    // cover used when a track has no embedded/dir art: "default" resolves
    // assets/default-cover.* (config dir, then ./assets, then next to the
    // binary), "none" disables it, anything else is an explicit path.
    std::string artFallback = "default";

    // [colors] — optional per-key palette overrides (-1 = not set, so #000000
    // is a legitimate value)
    int cAccentA = -1, cAccentB = -1, cFrame = -1, cFrameHi = -1, cSurface = -1;
    int cText = -1, cDim = -1, cLabel = -1, cOk = -1, cWarn = -1, cErr = -1;

    std::string sourcePath;             // file actually loaded (for reload)

    // parse; returns false if the file could not be read (defaults stay).
    // warnings collects unknown keys / bad values (reported to stderr).
    bool load(const std::string& path, std::vector<std::string>* warnings = nullptr);

    // effective palette = named palette + [colors] overrides
    Palette palette() const;

    static std::string defaultPath();
    static std::string skeleton();      // commented sample file
};

Config& cfg();

} // namespace qp
