#include "App.hpp"
#include "Config.hpp"
#include "../ui/Vector.hpp"

#include <cmath>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace qp {

using namespace std::chrono_literals;

App::App(const std::string& rootDir) {
    P_ = cfg().palette();
    GlyphTheme::set(cfg().icons);
    rootDir_ = rootDir;
    deckState_.vol = cfg().volume;
    idleTimeout_ = cfg().idleTimeout;
    running_ = true;
    worker_ = std::thread(&App::workerLoop, this);
    audio_.init();
    // the engine owns the gain; deckState_.vol is only a mirror refreshed each
    // frame from audio_.volume(), so the configured start volume must be set here
    audio_.setVolume(cfg().volume);
}

void App::shutdown() {
    if (running_.exchange(false)) {
        dsp_.stop();
        jobsM_.lock();
        { std::lock_guard<std::mutex> g(jobsM_, std::adopt_lock); jobs_.clear(); }
        audio_.shutdown();
        if (worker_.joinable()) worker_.join();
    }
}

void App::post(std::function<void()> fn) {
    std::lock_guard<std::mutex> g(jobsM_);
    jobs_.push_back(std::move(fn));
}

void App::postUi(std::function<void()> fn) {
    std::lock_guard<std::mutex> g(evM_);
    uiQ_.push_back(std::move(fn));
}

void App::drainUi() {
    std::deque<std::function<void()>> ready;
    {
        std::lock_guard<std::mutex> g(evM_);
        ready.swap(uiQ_);
    }
    for (auto& f : ready) f();
}

void App::workerLoop() {
    while (running_) {
        std::function<void()> job;
        {
            std::lock_guard<std::mutex> g(jobsM_);
            if (!jobs_.empty()) {
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
        }
        if (!job) { std::this_thread::sleep_for(6ms); continue; }
        job();
    }
}


// ---------------------------------------------------------------------------
// fallback cover: used when a track has no embedded art and no dir art.
// Resolution: [art] fallback = path | none | default (assets/default-cover.*
// in the config dir, then next to the binary, then ./assets).
// ---------------------------------------------------------------------------
namespace {

std::string exeDir() {
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    return std::filesystem::path(buf).parent_path().string();
}

std::string resolveFallbackPath(const std::string& spec) {
    std::string f = spec;
    if (f.empty() || f == "none" || f == "off") return {};
    if (f != "default") {
        if (f.rfind("~/", 0) == 0) {
            const char* home = std::getenv("HOME");
            if (home) f = std::string(home) + f.substr(1);
        }
        return f;
    }
    std::vector<std::string> dirs;
    if (const char* x = std::getenv("XDG_CONFIG_HOME")) dirs.push_back(std::string(x) + "/qpanel/assets");
    if (const char* h = std::getenv("HOME")) dirs.push_back(std::string(h) + "/.config/qpanel/assets");
    std::string ex = exeDir();
    if (!ex.empty()) {
        dirs.push_back(ex + "/assets");
        dirs.push_back(ex + "/../assets");
    }
    dirs.push_back("assets");
    for (const auto& d : dirs)
        for (const char* ext : { ".jpg", ".jpeg", ".png", ".bmp" }) {
            std::error_code ec;
            std::string p = d + "/default-cover" + ext;
            if (std::filesystem::is_regular_file(p, ec)) return p;
        }
    return {};
}

const CoverPixels& fallbackCover(const std::string& spec) {
    static std::string cachedSpec;
    static CoverPixels cached;
    if (spec != cachedSpec) {           // re-resolve on reload / spec change
        cachedSpec = spec;
        cached = {};
        std::string p = resolveFallbackPath(spec);
        if (!p.empty()) cached = Media::readImageFile(p);
    }
    return cached;
}

} // namespace

// ---------------------------------------------------------------------------
// live config reload (ctrl+9 / ctrl+0)
// Re-reads the same file into a FRESH Config (so deleted keys fall back to
// defaults), then re-applies everything that is cached in the app.
// ---------------------------------------------------------------------------
void App::reloadConfig() {
    const std::string path = cfg().sourcePath.empty() ? Config::defaultPath() : cfg().sourcePath;
    Config fresh;
    std::vector<std::string> warnings;
    const bool ok = fresh.load(path, &warnings);
    cfg() = fresh;

    P_ = cfg().palette();
    GlyphTheme::set(cfg().icons);
    artConfig() = cfg().art;
    audio_.setVolume(cfg().volume);
    idleTimeout_ = cfg().idleTimeout;

    // art settings may have changed: drop the rendered cover so it re-renders
    coverState_.artDirty = true;
    coverState_.artCells.clear();
    coverState_.artSrcCols = 0;

    for (auto& w : warnings) std::fprintf(stderr, "qpanel: %s\n", w.c_str());
    if (!ok)                toast_ = "NO CONFIG";
    else if (!warnings.empty()) toast_ = "CFG " + std::to_string(warnings.size()) + " WARN";
    else                    toast_ = "CFG RELOADED";
    toastUntil_ = Clock::now() + std::chrono::milliseconds(2500);
}

// ---------------------------------------------------------------------------
// layout — proportions per the 92x28 spec, flexing to actual size
// ---------------------------------------------------------------------------
App::Layout App::layout() {
    int W = term.width(), H = term.height();
    const int OUT = std::max(0, cfg().gapOut);
    const int GAP = std::max(0, cfg().gapIn);
    const int innerW = std::max(40, W - OUT * 2);
    const int innerH = std::max(12, H - OUT * 2);
    const int deckH = evaluateTier(W, H) == DisplayTier::Expanded && H >= 36 ? 12 : 9;

    Layout L;

    // ---- bottom row: CONTROLS (wide) | PLAY (square, always its own dock) ---
    // A cell is 1x2, so a visually square dock is w = 2*h.
    int playW = std::clamp(2 * deckH, 14, std::max(14, innerW / 3));
    if (innerW - playW - GAP < 34) playW = 0;          // no room: keep inline
    const int playH = deckH;
    L.deck = Rect{ OUT, OUT + innerH - playH,
                   innerW - (playW ? playW + GAP : 0), playH };
    L.play = playW ? Rect{ OUT + innerW - playW, OUT + innerH - playH, playW, playH }
                   : Rect{};

    // ---- top row: [meta / browser] | [ART square] --------------------------
    const int topH = std::max(6, innerH - playH - GAP);

    if (!cfg().artPanel) {
        int metaH0 = std::clamp(topH * 38 / 100, 10, 16);
        L.meta    = Rect{ OUT, OUT, innerW, std::min(metaH0, topH - 8) };
        L.browser = Rect{ OUT, L.meta.y + L.meta.h + GAP, innerW,
                          std::max(6, topH - L.meta.h - GAP) };
        L.art = Rect{};
        return L;
    }

    // ART keeps a 1:1 dock (w = 2*h); whatever is left makes the tree column
    // wider — that reads better than a fixed 46% split.
    int artW = std::clamp(2 * topH, 20, std::max(20, innerW / 2));
    int leftW = std::max(24, innerW - artW - GAP);
    int metaH = std::clamp(topH * 38 / 100, 10, 16);
    if (metaH > topH - GAP - 6) metaH = std::max(6, topH - GAP - 6);

    L.meta    = Rect{ OUT, OUT, leftW, metaH };
    L.browser = Rect{ OUT, OUT + metaH + GAP, leftW, std::max(6, topH - metaH - GAP) };
    L.art     = Rect{ OUT + leftW + GAP, OUT, artW, topH };
    return L;
}

// ---------------------------------------------------------------------------
// playback
// ---------------------------------------------------------------------------
void App::applyTrack(const std::string& path) {
    meta_ = MetaState{};
    meta_.playingPath = path;
    coverState_ = CoverState{};
    coverState_.loading = true;
    deckState_.dur = 0.0;
    deckState_.pos = 0.0;
    deckState_.playing = false;

    const std::string fbSpec = cfg().artFallback;   // captured on the UI thread
    post([this, path, fbSpec] {
        TrackMeta t = Media::readMeta(path);
        CoverPixels cp = Media::readCover(path, std::filesystem::path(path).parent_path().string());
        if (!cp.valid()) cp = fallbackCover(fbSpec);  // asset default when the file has no art
        postUi([this, path, t, cp] {
            meta_.meta = t;
            meta_.loaded = true;
            meta_.playingPath = path;
            coverState_.src = cp;
            coverState_.loading = false;
            coverState_.renderedCols = 0;
            double dur = audio_.load(path);   // decoder swap on UI thread
            deckState_.dur = dur;
            deckState_.playing = true;
            for (int i = 0; i < lib_.count(); ++i)
                if (!lib_.at(i).isDir && lib_.at(i).path.string() == path) { selectNode(i, false); break; }
        });
    });
}

void App::playNode(int idx) {
    if (idx < 0 || idx >= lib_.count()) return;
    const TreeNode n = lib_.at(idx);
    if (n.isDir) {
        if (n.children.empty()) {
            lib_.ref(idx).loading = true;
            std::string dir = n.path.string();
            post([this, idx, dir] {
                auto entries = lib_.scan(idx);
                postUi([this, idx, entries = std::move(entries)] {
                    lib_.graft(idx, entries);
                    if (!lib_.at(idx).children.empty())
                        selected_ = lib_.at(idx).children.front();
                });
            });
        } else {
            lib_.ref(idx).expanded = !lib_.ref(idx).expanded;
        }
        return;
    }
    if (!n.playable()) return;
    applyTrack(n.path.string());
}

// ---------------------------------------------------------------------------
// tree
// ---------------------------------------------------------------------------
void App::selectNode(int idx, bool reveal) {
    selected_ = idx;
    if (!reveal || idx < 0) return;
    // open parents + ensure visible
    int i = idx;
    while (i >= 0) { int p = lib_.at(i).parent; if (p >= 0) lib_.ref(p).expanded = true; i = p; }
}

void App::scrollTree(int dy) {
    Rect b = layout().browser;
    int h = b.h - 3;
    int total = lib_.visibleCount();
    top_ = std::clamp(top_ + dy, 0, std::max(0, total - h));
}

// ---------------------------------------------------------------------------
// input
// ---------------------------------------------------------------------------
// arrow/whitelist key codes live in Terminal.hpp (VKey) so the parser and the
// dispatcher can never drift apart.

void App::handleInput(const Input& ev) {
    // While the visualizer is on the front:
    //   * a click in the browser/spectrum panel dismisses it (and nothing else),
    //   * a click on METADATA / ART moves the focus highlight there and keeps
    //     the visualizer up (those docks never hide it),
    //   * the transport (CONTROLS dock + PLAY panel) stays LIVE - you can pause
    //     or change volume without losing the visualizer,
    //   * wheel/motion on anything else is dropped before touching the timer.
    if (fade_ > 0.001f || fadeTarget_ > 0.001f) {
        const Layout L = layout();
        if (ev.kind == Input::Key) { lastInput_ = Clock::now(); onKey(ev.key); return; }
        if (ev.kind != Input::MouseEv) return;

        const bool inTransport = L.deck.contains(ev.m.x, ev.m.y) ||
                                 (L.play.w > 0 && L.play.contains(ev.m.x, ev.m.y));
        const bool dragging = volDrag_ || scrubbing_;
        if (inTransport || dragging) {
            onMouse(ev.m);              // works on the visualizer, timer untouched
            return;
        }
        // A press outside the transport decides what the front panel shows:
        // BROWSER = back to the tree (animated dismiss); any other dock keeps
        // the visualizer up and just moves the focus highlight there.
        if (ev.m.type == Mouse::Press) {
            lastInput_ = Clock::now();
            if (L.browser.contains(ev.m.x, ev.m.y)) {
                vizHide();                       // back to the tree, animated
                focus_ = Focus::Browser;
            } else if (L.meta.contains(ev.m.x, ev.m.y)) {
                focus_ = Focus::Metadata;
                vizShow();                       // assert the visualizer
            } else if (L.art.w > 0 && L.art.contains(ev.m.x, ev.m.y)) {
                focus_ = Focus::Art;
                vizShow();
            }
        }
        return;
    }
    lastInput_ = Clock::now();
    if (ev.kind == Input::Key) onKey(ev.key);
    else if (ev.kind == Input::MouseEv) onMouse(ev.m);
}

void App::onKey(int key) {
    // While the idle visualizer is up, the first keypress only sends it away
    // (animated) — acting on the hidden tree in the same keystroke is a
    // surprise, e.g. Enter playing a track you could not see.
    if (key == VK_RELOAD) { reloadConfig(); return; }  // ctrl+9 / ctrl+0
    if (key != 'q' && key != 'Q' && (fade_ > 0.001f || fadeTarget_ > 0.001f)) { vizHide(); return; }
    switch (key) {
        case 'q': case 'Q':
            quitting_ = true;
            break;
        case ' ': case 'p': case 'P':
            fadeTarget_ = 0.f;
            audio_.toggle();
            break;
        case VK_LEFT:  audio_.seekBy(-10.0); break;
        case VK_RIGHT: audio_.seekBy(10.0);  break;
        case VK_UP: {
            auto rows = lib_.visible(top_, layout().browser.h - 3);
            if (rows.empty()) break;
            int pos = -1;
            for (size_t i = 0; i < rows.size(); ++i) if (rows[i] == selected_) { pos = int(i); break; }
            if (pos < 0) selected_ = rows.front();
            else if (pos == 0) { if (top_ > 0) { top_--; selected_ = rows.front(); } }
            else selected_ = rows[size_t(pos) - 1];
            break;
        }
        case VK_DOWN: {
            auto rows = lib_.visible(top_, layout().browser.h - 3);
            if (rows.empty()) break;
            int pos = -1;
            for (size_t i = 0; i < rows.size(); ++i) if (rows[i] == selected_) { pos = int(i); break; }
            if (pos < 0) selected_ = rows.front();
            else if (pos + 2 > int(rows.size())) { top_ = std::min(top_ + 1, std::max(0, lib_.visibleCount() - (int(rows.size())))); selected_ = rows.back(); }
            else selected_ = rows[size_t(pos + 1)];
            break;
        }
        case '\n': case '\r':
            if (selected_ >= 0) playNode(selected_);
            break;
        case 's': case 'S': deckState_.shuffle = !deckState_.shuffle; break;
        case 'r': case 'R': deckState_.repeat = (deckState_.repeat + 1) % 3; break;
        case '+': case '=': audio_.setVolume(audio_.volume() + 0.05f); break;
        case '-': case '_': audio_.setVolume(audio_.volume() - 0.05f); break;
        default: break;
    }
}

void App::onMouse(const Mouse& m) {
    Layout L = layout();

    // While the visualizer is on the front — fully shown OR mid-animation in
    // either direction — the pointer must not reach the widgets underneath.
    // The ONE accepted gesture is a click inside the browser/spectrum panel,
    // which sends it away. Everything else (deck buttons, wheel, drags) is
    // swallowed until the animation has finished.
    const bool vizUp = fade_ > 0.001f || fadeTarget_ > 0.001f;
    if (vizUp) {
        const bool inTransport = L.deck.contains(m.x, m.y) ||
                                 (L.play.w > 0 && L.play.contains(m.x, m.y));
        if (m.type == Mouse::Press && L.browser.contains(m.x, m.y)) { vizHide(); return; }
        // transport + ongoing drags keep working over the visualizer
        if (!(inTransport || volDrag_ || scrubbing_)) return;
        if (m.type == Mouse::WheelUp || m.type == Mouse::WheelDown) return;
    }

    if (m.type == Mouse::WheelUp || m.type == Mouse::WheelDown) {
        // hovering the volume bar: the wheel controls loudness, not scroll
        if (hits_.volume.w > 0 && hits_.volume.contains(m.x, m.y)) {
            float step = (m.type == Mouse::WheelUp) ? 0.05f : -0.05f;
            audio_.setVolume(audio_.volume() + step);
            return;
        }
        scrollTree(m.type == Mouse::WheelUp ? -3 : 3);
        return;
    }

    if (m.type == Mouse::Press) {
        if (L.browser.contains(m.x, m.y)) {
            focus_ = Focus::Browser;
            int node = W::browserRowToNode(L.browser, lib_, selected_, top_, m.y);
            if (node < 0) return;
            static Clock::time_point lastClick = Clock::now() - 10s;
            static int lastNode = -1;
            auto now = Clock::now();
            bool dbl = lastNode == node &&
                       std::chrono::duration<double>(now - lastClick).count() < 0.4;
            lastClick = now; lastNode = node;
            selected_ = node;

            const TreeNode& n = lib_.at(node);
            if (n.isDir) {
                if (n.expanded && !dbl) lib_.ref(node).expanded = false; // single click collapse
                else if (dbl) playNode(node);                            // double click open
                else if (!n.children.empty()) lib_.ref(node).expanded = true;
                else playNode(node); // loads + expands async
            } else if (n.playable()) {
                applyTrack(n.path.string()); // click == play (decoded audio)
            }
        } else if (L.play.w > 0 && L.play.contains(m.x, m.y)) {
            focus_ = Focus::Deck;
            if (hits_.play.contains(m.x, m.y)) audio_.toggle();
        } else if (L.deck.contains(m.x, m.y)) {
            focus_ = Focus::Deck;
            if (hits_.play.contains(m.x, m.y)) audio_.toggle();
            else if (hits_.prev.contains(m.x, m.y)) audio_.seekBy(-10.0);
            else if (hits_.next.contains(m.x, m.y)) audio_.seekBy(10.0);
            else if (hits_.shuffle.contains(m.x, m.y)) deckState_.shuffle = !deckState_.shuffle;
            else if (hits_.repeat.contains(m.x, m.y)) deckState_.repeat = (deckState_.repeat + 1) % 3;
            else if (hits_.volume.contains(m.x, m.y)) {
                float v = W::deckVolumeAt(hits_, m.x);
                if (v >= 0) audio_.setVolume(v);
                volDrag_ = true;                 // keep tracking the drag
            } else if (hits_.scrub.contains(m.x, m.y)) {
                audio_.seekTo(W::deckScrubToSeconds(L.deck, hits_, deckState_, m.x));
                scrubbing_ = true;
            }
        } else if (L.meta.contains(m.x, m.y)) {
            // clicking a dock that is not the tree both focuses it (its chassis
            // gets the gradient + glow) and brings the visualizer forward.
            focus_ = Focus::Metadata;
            vizShow();
        } else if (L.art.w > 0 && L.art.contains(m.x, m.y)) {
            focus_ = Focus::Art;
            vizShow();
        } else {
            // gaps / background: same thing, focus stays where it was
            vizShow();
        }
        return;
    }

    if (m.type == Mouse::Motion && volDrag_ && hits_.volume.w > 0) {
        float v = W::deckVolumeAt(hits_, m.x);
        if (v >= 0) audio_.setVolume(v);
        return;
    }

    if (m.type == Mouse::Motion && scrubbing_ && hits_.scrub.w > 1) {
        static Clock::time_point last = Clock::now() - 1s;
        auto now = Clock::now();
        if (std::chrono::duration<double>(now - last).count() > 0.25) {
            last = now;
            if (m.y >= hits_.scrub.y - 1 && m.y <= hits_.scrub.y + 1)
                audio_.seekTo(W::deckScrubToSeconds(L.deck, hits_, deckState_, m.x));
        }
        return;
    }

    if (m.type == Mouse::Release) {
        // Only a drag that STARTED inside the scrub bar commits a seek on
        // release. Otherwise a plain button click (e.g. [10s >>] at the far
        // right) would snap to bar-relative 0:00 / duration — spec misuse.
        if (scrubbing_ && hits_.scrub.w > 1)
            audio_.seekTo(W::deckScrubToSeconds(L.deck, hits_, deckState_, m.x));
        scrubbing_ = false;
        volDrag_ = false;
    }
}

// ---------------------------------------------------------------------------
// main loop
// ---------------------------------------------------------------------------
void App::run() {
    auto& t = term;
    if (!t.start()) {
        std::fprintf(stderr, "qpanel: stdin/stdout must be a terminal\n");
        return;
    }
    dsp_.start(&audio_, float(audio_.sampleRate()));
    lib_.setRoot(rootDir_);

    auto prev = Clock::now();
    while (!quitting_) {
        auto now = Clock::now();
        float dt = std::chrono::duration<float>(now - prev).count();
        prev = now;

        Input ev;
        while (t.poll(ev)) {
            if (ev.kind == Input::Quit) { quitting_ = true; break; }
            handleInput(ev);
        }
        if (t.resized()) t.refreshSize();
        drainUi();

        // EOF -> next track
        if (audio_.pollEof()) {
            std::vector<int> files;
            for (int i = 0; i < lib_.count(); ++i)
                if (!lib_.at(i).isDir && lib_.at(i).playable()) files.push_back(i);
            if (!files.empty()) {
                int cur = -1;
                for (size_t i = 0; i < files.size(); ++i)
                    if (lib_.at(files[i]).path.string() == meta_.playingPath) { cur = int(i); break; }
                if (deckState_.repeat == 2 && cur >= 0)
                    applyTrack(lib_.at(files[size_t(cur)]).path.string());
                else {
                    int next;
                    if (deckState_.shuffle) next = files[size_t(std::rand()) % files.size()];
                    else if (cur + 1 < int(files.size())) next = files[size_t(cur) + 1];
                    else if (deckState_.repeat == 1) next = files.front();
                    else {
                        deckState_.playing = false;
                        deckState_.pos = deckState_.dur;
                        goto render;
                    }
                    applyTrack(lib_.at(next).path.string());
                }
            }
        }

    render:;
        deckState_.playing = audio_.playing();
        deckState_.pos = audio_.position();
        deckState_.dur = audio_.duration();
        deckState_.vol = audio_.volume();

        Layout L = layout();

        // idle detector + fade timeline (spec: 8s timeout)
        double sinceInput = std::chrono::duration<double>(now - lastInput_).count();
        if (!idle_ && sinceInput > idleTimeout_) { idle_ = true; fadeTarget_ = 1.f; }
        if (idle_ && sinceInput <= idleTimeout_) { idle_ = false; fadeTarget_ = 0.f; }
        if (std::fabs(fadeTarget_ - fade_) > 0.001f) {
            float speed = fadeTarget_ > fade_ ? 1.5f : 5.f; // slow morph in, quick morph out
            if (fadeTarget_ > fade_) fade_ = std::min(fadeTarget_, fade_ + dt * speed);
            else fade_ = std::max(fadeTarget_, fade_ - dt * speed);
        }
        // ALWAYS recompute the eased value: when input set fade_ and fadeTarget_
        // to the same 0 this used to be skipped, so fadeEased_ stayed at 1.0 and
        // the render kept showing (and routing to) the spectrum forever.
        fadeEased_ = fade_ * fade_ * (3.f - 2.f * fade_); // smoothstep

        auto bars = dsp_.snapshot();
        W::chassis(L.meta, P_, focus_ == Focus::Metadata, "METADATA");
        // while the visualizer owns the panel it reads as a lit window even when
        // the click focus sits on another dock: an unfocused frame (#1E2740)
        // under the bright bars looks like a dead black bar.
        W::chassis(L.browser, P_, focus_ == Focus::Browser,
                   fadeEased_ > 0.5f ? "SPECTRUM" : "BROWSER",
                   fadeEased_ > 0.5f ? "-- IDLE" : "",
                   fadeEased_ * 0.85f);
        W::chassis(L.art, P_, focus_ == Focus::Art, "ART", "[1:1]");
        W::chassis(L.deck, P_, focus_ == Focus::Deck, "CONTROLS",
                   toastUntil_ > now ? toast_ : std::string());
        if (L.play.w > 0) W::chassis(L.play, P_, focus_ == Focus::Deck, "PLAY");

        W::metadata(L.meta, P_, focus_ == Focus::Metadata, meta_, deckState_);
        if (fadeEased_ <= 0.001f) {
            W::browser(L.browser, P_, focus_ == Focus::Browser, lib_, selected_, top_, meta_);
        } else if (fadeEased_ >= 0.999f) {
            auto wave = dsp_.waveSnapshot();
            W::spectrum(L.browser, P_, focus_ == Focus::Browser, bars, deckState_, &wave);
        } else {
            W::crossfadeBrowser(L.browser, P_, focus_ == Focus::Browser, lib_,
                                selected_, top_, meta_, deckState_, bars,
                                fadeEased_, scratchA_, scratchB_);
        }
        W::cover(L.art, P_, focus_ == Focus::Art, coverState_, meta_.meta);
        W::deck(L.deck, P_, focus_ == Focus::Deck, deckState_, hits_, L.play.w == 0);
        if (L.play.w > 0) W::player(L.play, P_, focus_ == Focus::Deck, deckState_, hits_);

        t.flush();

        // frame pacing ~60fps
        auto target = prev + 16666us;
        if (now < target) std::this_thread::sleep_until(target);
    }

    dsp_.stop();
    audio_.shutdown();
    t.stop();
}

} // namespace qp
