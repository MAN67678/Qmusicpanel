#include "Media.hpp"

#include <fstream>
#include <iterator>

#include <taglib/tag.h>
#include <taglib/fileref.h>
#include <taglib/tpropertymap.h>
#include <taglib/mpegfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/id3v2frame.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/flacfile.h>
#include <taglib/xiphcomment.h>
#include <taglib/oggfile.h>
#include <taglib/vorbisfile.h>
#include <taglib/mp4file.h>
#include <taglib/mp4item.h>

#include <stb_image.h>
#include <stb_image_resize2.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace qp {
namespace fs = std::filesystem;

static bool iext(const std::string& p, const char* ext) {
    auto s = fs::path(p).extension().string();
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
    return s == ext;
}

bool Media::playable(const std::string& p) {
    for (auto* e : { ".mp3", ".flac", ".ogg", ".oga", ".opus", ".wav", ".m4a", ".aac", ".mp4", ".it", ".xm", ".mod", ".s3m" }) {
        if (iext(p, e)) return true;
    }
    return false;
}

// ---------- tags ----------
TrackMeta Media::readMeta(const std::string& path) {
    TrackMeta m;
    TagLib::FileRef ref(path.c_str());
    if (ref.isNull()) { m.err = "unreadable"; return m; }

    if (auto* t = ref.tag()) {
        m.title  = t->title().to8Bit(true);
        m.artist = t->artist().to8Bit(true);
        m.album  = t->album().to8Bit(true);
        m.genre  = t->genre().to8Bit(true);
        // TagLib 2: BPM lives in the property map (TBPM / BPM / FPMC…)
        TagLib::PropertyMap props = t->properties();
        if (props.contains("BPM")) {
            const TagLib::StringList& l = props["BPM"];
            if (!l.isEmpty()) m.bpm = std::stod(l.front().to8Bit(true));
        }
        if (m.bpm <= 0) m.bpm = -1.0;
    }
    if (m.title.empty())
        m.title = fs::path(path).stem().string();
    if (m.artist.empty()) m.artist = "--";
    if (m.album.empty())  m.album  = "--";
    if (m.genre.empty())  m.genre  = "--";

    if (auto* ap = ref.audioProperties()) {
        int kbps = ap->bitrate();
        int hz = ap->sampleRate();
        int ch = ap->channels();
        if (iext(path, ".flac")) {
            auto* fp = dynamic_cast<TagLib::FLAC::Properties*>(ref.audioProperties());
            int bits = fp ? fp->bitsPerSample() : 16;
            char buf[64];
            std::snprintf(buf, sizeof buf, "FLAC %d-bit / %d.%d kHz", bits, hz / 1000, (hz % 1000) / 100);
            m.statusVerbose = buf;
        } else if (iext(path, ".mp3")) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "MP3 %d kbps / %d.%d kHz", kbps, hz / 1000, (hz % 1000) / 100);
            m.statusVerbose = buf;
        } else if (iext(path, ".m4a") || iext(path, ".mp4") || iext(path, ".aac")) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "AAC %d kbps / %d.%d kHz", kbps, hz / 1000, (hz % 1000) / 100);
            m.statusVerbose = buf;
        } else {
            char buf[80];
            const char* tag = iext(path, ".wav") ? "WAV" : "OGG";
            std::snprintf(buf, sizeof buf, "%s %dkbps %dch %d.%dkHz", tag, kbps, ch, hz / 1000, (hz % 1000) / 100);
            m.statusVerbose = buf;
        }
    }
    return m;
}

// ---------- decode ----------
CoverPixels Media::decode(const std::vector<uint8_t>& blob) {
    CoverPixels c;
    int w, h, n;
    stbi_uc* data = stbi_load_from_memory(blob.data(), int(blob.size()), &w, &h, &n, 3);
    if (!data) return c;
    c.w = w; c.h = h; c.ch = 3;
    c.px.assign(data, data + size_t(w) * h * 3);
    stbi_image_free(data);
    return c;
}

// ---------- embedded art ----------
static bool id3Cover(const std::string& path, std::vector<uint8_t>& out) {
    TagLib::MPEG::File f(path.c_str());
    if (!f.isValid()) return false;
    auto* id3 = f.ID3v2Tag(false);
    if (!id3) return false;
    std::vector<uint8_t> best;
    auto frameList = id3->frameListMap()["APIC"];
    for (auto* fr : frameList) {
        auto* pic = dynamic_cast<TagLib::ID3v2::AttachedPictureFrame*>(fr);
        if (!pic) continue;
        auto bv = pic->picture();
        std::vector<uint8_t> cur(bv.begin(), bv.end());
        if (cur.size() > best.size()) best = std::move(cur);
    }
    if (!best.empty()) { out = std::move(best); return true; }
    return false;
}

static bool flacCover(const std::string& path, std::vector<uint8_t>& out) {
    TagLib::FLAC::File f(path.c_str());
    if (!f.isValid()) return false;
    std::vector<uint8_t> best;
    for (auto* p : f.pictureList()) {
        auto bv = p->data();
        std::vector<uint8_t> cur(bv.begin(), bv.end());
        if (cur.size() > best.size()) best = std::move(cur);
    }
    if (!best.empty()) { out = std::move(best); return true; }
    return false;
}

static bool oggCover(const std::string& path, std::vector<uint8_t>& out) {
    TagLib::Vorbis::File f(path.c_str());
    if (!f.isValid()) return false;
    auto* xc = f.tag();
    if (!xc) return false;
    std::vector<uint8_t> best;
    for (auto* p : xc->pictureList()) {
        auto bv = p->data();
        std::vector<uint8_t> cur(bv.begin(), bv.end());
        if (cur.size() > best.size()) best = std::move(cur);
    }
    if (!best.empty()) { out = std::move(best); return true; }
    return false;
}

static bool mp4Cover(const std::string& path, std::vector<uint8_t>& out) {
    TagLib::MP4::File f(path.c_str());
    if (!f.isValid()) return false;
    auto* tag = f.tag();
    if (!tag) return false;
    const auto& items = tag->itemMap();
    if (!items.contains("covr")) return false;
    auto lst = items["covr"].toCoverArtList();
    std::vector<uint8_t> best;
    for (auto& ca : lst) {
        auto bv = ca.data();
        std::vector<uint8_t> cur(bv.begin(), bv.end());
        if (cur.size() > best.size()) best = std::move(cur);
    }
    if (!best.empty()) { out = std::move(best); return true; }
    return false;
}

CoverPixels Media::readCover(const std::string& path, const std::string& dirHint) {
    std::vector<uint8_t> blob;
    if (iext(path, ".mp3")) id3Cover(path, blob);
    else if (iext(path, ".flac")) flacCover(path, blob);
    else if (iext(path, ".ogg") || iext(path, ".oga")) oggCover(path, blob);
    else if (iext(path, ".m4a") || iext(path, ".mp4")) mp4Cover(path, blob);

    // FALLBACK: directory art (cover.jpg/png, folder.*, front.*)
    if (blob.empty()) {
        namespace fs2 = std::filesystem;
        fs::path dir = !dirHint.empty() ? fs::path(dirHint) : fs::path(path).parent_path();
        for (const char* stem : { "cover", "folder", "front", "album", "art" }) {
            for (const char* ext : { ".jpg", ".jpeg", ".png", ".bmp" }) {
                fs::path p = dir / (std::string(stem) + ext);
                std::error_code ec;
                if (fs2::exists(p, ec)) {
                    FILE* fp = std::fopen(p.string().c_str(), "rb");
                    if (fp) {
                        std::vector<uint8_t> buf;
                        uint8_t chunk[65536];
                        size_t r;
                        while ((r = std::fread(chunk, 1, sizeof chunk, fp)) > 0) buf.insert(buf.end(), chunk, chunk + r);
                        std::fclose(fp);
                        if (!buf.empty()) { blob = std::move(buf); break; }
                    }
                }
            }
            if (!blob.empty()) break;
        }
    }

    if (blob.empty()) return {};
    return decode(blob);
}

// ---------- resize ----------
CoverPixels Media::readImageFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::vector<uint8_t> blob((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    if (blob.empty()) return {};
    return decode(blob);
}

CoverPixels Media::squareResize(const CoverPixels& src, int px) {
    if (!src.valid() || px <= 0) return {};
    int side = std::min(src.w, src.h);
    int cx0 = (src.w - side) / 2, cy0 = (src.h - side) / 2;

    // materialize center-cropped square
    std::vector<uint8_t> sq(size_t(side) * side * 3);
    for (int y = 0; y < side; y++)
        std::memcpy(sq.data() + size_t(y) * side * 3,
                    src.px.data() + (size_t(cy0 + y) * src.w + cx0) * 3,
                    size_t(side) * 3);

    CoverPixels o;
    o.w = o.h = px; o.ch = 3;
    o.px.resize(size_t(px) * px * 3);
    if (side == px) { o.px = sq; return o; }
    stbir_resize_uint8_srgb(sq.data(), side, side, 0,
                            o.px.data(), px, px, 0, STBIR_RGB);
    if (o.px.empty()) return {};
    return o;
}

} // namespace qp
