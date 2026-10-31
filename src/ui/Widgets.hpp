// QPanel — widget layer.
// Rounded chassis with Hyprland-style gradient borders (focused panel glows),
// metadata panel, browser tree, 1:1 half-block cover canvas, CAVA spectrum,
// transport deck + scrub bar. Renders into the term (qpanel) cell grid.
#pragma once
#include <array>
#include <string>
#include <vector>

#include "../term/Terminal.hpp"
#include "../ui/Palette.hpp"
#include "../media/Media.hpp"
#include "../media/ChafaRender.hpp"
#include "../fs/Library.hpp"

namespace qp {

struct MetaState {
    TrackMeta meta;
    std::string playingPath;   // absolute path of currently loaded track
    bool loaded = false;
    bool computingBpm = false;
};

// widget-local cell for scratch compositing. `cont` marks the right half of a
// two-column glyph; blitters skip those cells (the lead already emitted it).
struct WCell {
    char32_t ch = U' '; Color fg = NONE; Color bg = NONE; bool cont = false;
};

struct CoverState {
    bool loading = false;
    CoverPixels src;          // decoded square-crop-ready source
    CoverPixels rendered;     // resized for current canvas (cache)
    int renderedCols = 0;     // canvas cols when rendered == src fit
    // chafa cell cache: keyed by (artCols, artRows, engine, symbols)
    std::vector<ArtCell> artCells;
    int artCols = 0, artRows = 0;
    bool artDirty = true;
    CoverPixels artSrc;          // higher-res square crop fed to chafa
    int artSrcCols = 0;
};

struct DeckState {
    bool playing = false;
    double pos = 0, dur = 0;
    float vol = 0.85f;
    int repeat = 0;   // 0 off, 1 all, 2 one
    bool shuffle = false;
};

enum class Focus { Metadata, Browser, Art, Deck };

namespace W {

// `active` (0..1) lets a non-focused dock still read as "lit" — used by the
// spectrum panel, which owns the browser dock while the visualizer is up even
// if the click focus sits on another dock.
void chassis(const Rect& r, const Palette& P, bool focused, const std::string& title,
             const std::string& suffix = "", float active = 0.f);
void truncText(int x, int y, int maxCols, const std::string& s, Color c);

void metadata(const Rect& r, const Palette& P, bool focused, const MetaState& m,
              const DeckState& d);

// returns mapping row -> node index for hit tests (or -1)
void browser(const Rect& r, const Palette& P, bool focused, const Library& lib,
             int selected, int scroll, const MetaState& m);
int  browserRowToNode(const Rect& r, const Library& lib, int selected, int top, int rowY);

void cover(const Rect& r, const Palette& P, bool focused, CoverState& c, const TrackMeta& m);

// hitboxes are reported in absolute terminal coords for the mouse dispatcher
struct DeckHits {
    Rect prev, play, next, scrub, volume, shuffle, repeat;
    bool scrollInside = false;
};
// withPlay=false means the play/pause button lives in its own panel (player())
void deck(const Rect& r, const Palette& P, bool focused, const DeckState& d, DeckHits& hits,
          bool withPlay = true);
// the square play column: ONE button, no extra frame
void player(const Rect& r, const Palette& P, bool focused, const DeckState& d, DeckHits& hits);
float deckScrubToSeconds(const Rect& deckRect, const DeckHits& h, const DeckState& d, int mx);
float deckVolumeAt(const DeckHits& h, int mx);

void spectrum(const Rect& r, const Palette& P, bool focused,
              const std::array<float, 32>& bars, const DeckState& d,
              const std::vector<float>* wave = nullptr);

// crossfade blending: draws tree and spectrum into scratch, then dithers
// result by t in [0,1] into the live canvas. Scratch is (rows * cols) cells.
void crossfadeBrowser(const Rect& r, const Palette& P, bool focused,
                      const Library& lib, int selected, int top, const MetaState& m,
                      const DeckState& d, const std::array<float, 32>& bars,
                      float t, std::vector<WCell>& scratchA,
                      std::vector<WCell>& scratchB);

} // namespace W
} // namespace qp
