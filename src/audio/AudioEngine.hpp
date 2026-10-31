// QPanel — audio playback subsystem (miniaudio).
// f32 stereo @ native 44.1k, lock-free ring between decode thread and device,
// position/seek/pause/volume management, and a mono PCM tap for the DSP core.
#pragma once
#include <miniaudio.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace qp {

class AudioEngine {
public:
    bool init();
    void shutdown();

    // Loads a track. Returns total duration in seconds (0 on failure).
    double load(const std::string& path);
    void unload();

    void play();
    void pause();
    void toggle();
    void seekTo(double seconds);
    void seekBy(double delta);

    double position() const;   // seconds
    double duration() const { return durSec_; }
    bool playing() const { return playing_; }
    bool loaderFailed() const { return failed_; }
    bool pollEof() { return eof_.exchange(false); }
    void clearEof() { eof_ = false; }

    void setVolume(float v01) { vol_ = v01 < 0.f ? 0.f : v01 > 1.f ? 1.f : v01; }
    float volume() const { return vol_; }

    int channels() const { return 2; }
    int sampleRate() const { return 44100; }

    // ---- DSP tap (downmixed mono, SPSC) ----
    static constexpr size_t TAP_SIZE = 1 << 15;
    // Push is done by device callback; read by DSP thread.
    size_t tapRead(float* dst, size_t frames) {
        size_t rd = tapRd_.load(std::memory_order_relaxed);
        size_t wr = tapWr_.load(std::memory_order_acquire);
        size_t avail = (wr + TAP_SIZE - rd) % TAP_SIZE;
        if (avail < frames) return 0;
        if (rd + frames < TAP_SIZE)
            std::memcpy(dst, tap_.data() + rd, frames * sizeof(float));
        else {
            size_t first = TAP_SIZE - rd;
            std::memcpy(dst, tap_.data() + rd, first * sizeof(float));
            std::memcpy(dst + first, tap_.data(), (frames - first) * sizeof(float));
        }
        tapRd_.store((rd + frames) % TAP_SIZE, std::memory_order_release);
        return frames;
    }
    size_t tapAvail() const {
        size_t rd = tapRd_.load(std::memory_order_relaxed);
        size_t wr = tapWr_.load(std::memory_order_acquire);
        return (wr + TAP_SIZE - rd) % TAP_SIZE;
    }
    void tapReset() {
        tapWr_ = tapRd_ = 0;
    }
    bool tapSlow() const { return tapAvail() > TAP_SIZE / 2; } // reader fell behind: drain
    // drop all but the newest `keep` frames — use is private
    void tapTrimToLatest(size_t keep) {
        size_t rd = tapRd_.load(std::memory_order_relaxed);
        size_t wr = tapWr_.load(std::memory_order_acquire);
        size_t avail = (wr + TAP_SIZE - rd) % TAP_SIZE;
        if (avail > keep) tapRd_.store((wr - keep + TAP_SIZE) % TAP_SIZE, std::memory_order_release);
    }

private:
    static void deviceCb(ma_device* dev, void* out, const void* in, ma_uint32 frames);
    void decodeLoop();

    ma_device dev_{};
    ma_decoder dec_{};
    bool decOpen_ = false;
    ma_pcm_rb rb_{};
    bool rbReady_ = false;
    std::thread decodeThread_;
    std::atomic<bool> running_{false};

    std::atomic<bool> playing_{false};
    std::atomic<bool> eof_{false};
    std::atomic<bool> failed_{false};
    std::atomic<bool> decodeDone_{false};
    std::atomic<uint64_t> seekTarget_{UINT64_MAX}; // frame index
    std::atomic<uint64_t> posFrames_{0};
    std::atomic<float> vol_{0.85f};
    uint64_t totalFrames_ = 0;
    double durSec_ = 0.0;
    std::string path_;

    std::vector<float> scratch_; // decode staging buffer
    std::vector<float> tap_{};
    std::atomic<size_t> tapWr_{0}, tapRd_{0};
};

} // namespace qp
