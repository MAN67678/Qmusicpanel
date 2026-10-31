// QPanel — DSP core.
// Reads the mono PCM tap, FFTs a sliding window, maps bins onto 30Hz..16kHz
// log bands, and applies CAVA-style physics (fast rise, accelerating gravity
// falloff). Publishes a bar snapshot consumed by the UI thread.
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace qp {

struct AudioEngine;

class Spectrum {
public:
    static constexpr int BARS = 32;
    static constexpr int FFT_N = 2048;

    void start(AudioEngine* eng, float sampleRate);
    void stop();

    // thread-safe copy of current bar heights in [0,1]
    std::array<float, BARS> snapshot() const {
        std::lock_guard<std::mutex> g(m_);
        return bars_;
    }

    // latest mono samples for the braille oscilloscope (Expanded tier)
    std::vector<float> waveSnapshot() const {
        std::lock_guard<std::mutex> g(m_);
        return wave_;
    }

private:
    void run();
    AudioEngine* eng_ = nullptr;
    float sr_ = 44100;
    std::thread th_;
    std::atomic<bool> run_{false};
    mutable std::mutex m_;
    std::array<float, BARS> bars_{};
    std::vector<float> wave_;
    std::vector<float> hist_;
    std::array<float, BARS> fall_{};
    std::array<float, BARS> agc_{};   // per-band raw level (scratch)
    float globPeak_ = 0.f;            // slow global reference for the display
};

} // namespace qp
