// QPanel — app state controller.
// Owns the render loop (60fps), input dispatch (SGR mouse hit-mapping),
// idle detector + crossfade timeline, background worker pool for
// library scans / metadata / cover art, and playback orchestration.
#pragma once
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../term/Terminal.hpp"
#include "../ui/Palette.hpp"
#include "../ui/Widgets.hpp"
#include "../ui/Glyphs.hpp"
#include "../audio/AudioEngine.hpp"
#include "../dsp/Spectrum.hpp"
#include "../fs/Library.hpp"
#include "../media/Media.hpp"

namespace qp {

class App {
public:
    App(const std::string& rootDir);
    ~App() { shutdown(); }

    void run();
    void shutdown();

private:
    using Clock = std::chrono::steady_clock;
    struct Job { std::function<void()> fn; };

    // ---- rendering ----
    struct Layout { Rect meta, browser, art, deck, play; };
    Layout layout();

    // ---- input ----
    void handleInput(const Input& ev);
    void onKey(int key);
    void onMouse(const Mouse& m);
    void playNode(int idx);

    // ---- workers ----
    void post(std::function<void()> fn);
    void workerLoop();
    void loadMetaAsync(const std::string& path);
    void loadDirAsync(int nodeIdx);

    // ---- misc ----
    void applyTrack(const std::string& path);
    void reloadConfig();                    // Ctrl+R / Ctrl+Shift+R
    void vizShow() { idle_ = false; fadeTarget_ = 1.f; }  // click => spectrum
    void vizHide() { idle_ = false; fadeTarget_ = 0.f; }  // click => tree
    void scrollTree(int dy);
    void selectNode(int idx, bool reveal);

    Palette P_;
    AudioEngine audio_;
    Spectrum dsp_;
    Library lib_;
    MetaState meta_;
    CoverState coverState_;
    DeckState deckState_;
    W::DeckHits hits_{};    Focus focus_ = Focus::Deck;

    int selected_ = -1;      // node index tree selection
    int top_ = 0;            // tree scroll
    float fade_ = 0.f;       // 0=tree, 1=spectrum
    float fadeTarget_ = 0.f;
    float fadeEased_ = 0.f;
    bool idle_ = false;
    Clock::time_point lastInput_ = Clock::now();

    std::string rootDir_;
    std::vector<WCell> scratchA_, scratchB_;

    // workers (cpu jobs) + ui queue (results published to render thread)
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::mutex jobsM_;
    std::deque<std::function<void()>> jobs_;
    std::mutex evM_;
    std::deque<std::function<void()>> uiQ_;   // drained on main/UI thread
    void postUi(std::function<void()> fn);
    void drainUi();

    bool scrubbing_ = false;
    bool volDrag_ = false;
    bool quitting_ = false;
    float idleTimeout_ = 8.f;   // [general] idle_timeout
    std::string toast_;                    // transient chassis-suffix notice
    Clock::time_point toastUntil_{};
};

} // namespace qp
