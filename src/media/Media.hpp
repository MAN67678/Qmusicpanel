// QPanel — asset & metadata pipeline.
// TagLib extraction (ID3v2 / Xiph / FLAC / MP4) incl. BPM + embedded art,
// directory-art fallback, stb decode & center-crop/resize to canvas pixels.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace qp {

struct TrackMeta {
    std::string title, artist, album, genre;
    std::string statusVerbose;   // "FLAC 24-bit / 44.1 kHz"
    double bpm = -1.0;           // -1 => unknown
    double duration = 0.0;
    std::string err;
};

struct CoverPixels {
    int w = 0, h = 0, ch = 3;    // RGB
    std::vector<uint8_t> px;     // w*h*3
    bool valid() const { return w > 0 && h > 0 && !px.empty(); }
};

class Media {
public:
    static bool playable(const std::string& path);

    // tags + BPM;_sep builds the "status"/format line
    static TrackMeta readMeta(const std::string& path);

    // embedded art (largest pic) else directory fallback; returns decoded RGB
    static CoverPixels readCover(const std::string& path, const std::string& dirFallbackHint);

    // decode a standalone image file (jpg/png/bmp/gif) — used for fallback art
    static CoverPixels readImageFile(const std::string& path);

    // center-crop to square then resize to w x w pixels (sRGB)
    static CoverPixels squareResize(const CoverPixels& src, int px);

private:
    static CoverPixels decode(const std::vector<uint8_t>& blob);
};

} // namespace qp
