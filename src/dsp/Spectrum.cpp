#include "Spectrum.hpp"
#include "../audio/AudioEngine.hpp"

#include <fftw3.h>
#include <algorithm>
#include <cmath>
#include <chrono>

namespace qp {

using namespace std::chrono_literals;

void Spectrum::start(AudioEngine* eng, float sr) {
    eng_ = eng;
    sr_ = sr;
    hist_.clear();
    bars_.fill(0.f);
    fall_.fill(0.f);
    run_ = true;
    th_ = std::thread(&Spectrum::run, this);
}

void Spectrum::stop() {
    run_ = false;
    if (th_.joinable()) th_.join();
}

void Spectrum::run() {
    static float win[FFT_N];
    for (int i = 0; i < FFT_N; i++)
        win[i] = 0.5f * (1.f - std::cos(2.f * float(M_PI) * i / (FFT_N - 1)));

    float* in = fftwf_alloc_real(FFT_N);
    fftwf_complex* out = fftwf_alloc_complex(FFT_N / 2 + 1);
    fftwf_plan plan = fftwf_plan_dft_r2c_1d(FFT_N, in, out, FFTW_MEASURE);

    std::vector<float> buf(FFT_N, 0.f);
    std::vector<float> bandPower(BARS, 0.f);

    // log-spaced bin edges: 30 Hz .. 16 kHz
    float fmin = 30.f, fmax = 16000.f;
    int binEdges[BARS + 1];
    for (int b = 0; b <= BARS; b++) {
        float f = fmin * std::pow(fmax / fmin, float(b) / BARS);
        int bin = int(f / sr_ * FFT_N);
        if (bin < 1) bin = 1;
        if (bin > FFT_N / 2) bin = FFT_N / 2;
        if (b > 0 && bin <= binEdges[b - 1]) bin = binEdges[b - 1] + 1;
        if (bin > FFT_N / 2) bin = FFT_N / 2;
        binEdges[b] = bin;
    }

    const int BW = BARS;
    auto next = std::chrono::steady_clock::now();

    while (run_) {
        next += 33ms; // ~30Hz analysis
        std::this_thread::sleep_until(next);

        // drain tap into history ring
        size_t avail = eng_->tapAvail();
        if (avail == 0) {
            // decay bars gently when no audio
            std::lock_guard<std::mutex> g(m_);
            for (int b = 0; b < BARS; b++) {
                bars_[b] *= 0.94f;
                if (bars_[b] < 0.001f) bars_[b] = 0.f;
            }
            continue;
        }
        if (eng_->tapSlow()) eng_->tapTrimToLatest(FFT_N); // fell behind: keep newest

        std::vector<float> fresh;
        // append what we can to rolling history
        while (true) {
            float tmp[1024];
            size_t got = eng_->tapRead(tmp, 1024);
            if (got == 0) break;
            for (size_t i = 0; i < got; i++) {
                if (hist_.size() >= FFT_N) {
                    std::memmove(hist_.data(), hist_.data() + 1, (FFT_N - 1) * sizeof(float));
                    hist_.back() = tmp[i];
                } else {
                    hist_.push_back(tmp[i]);
                }
                fresh.push_back(tmp[i]);
            }
            if (avail > 4096) continue;
            break;
        }
        // snap latest waveform for the 'cinema' oscilloscope
        if (!fresh.empty()) {
            std::lock_guard<std::mutex> g(m_);
            wave_.insert(wave_.end(), fresh.begin(), fresh.end());
            size_t keep = 384;
            if (wave_.size() > keep)
                wave_.erase(wave_.begin(), wave_.begin() + long(wave_.size() - keep));
        }
        if (hist_.size() < size_t(FFT_N)) continue;

        // window + fft
        for (int i = 0; i < FFT_N; i++) in[i] = hist_[i] * win[i];
        fftwf_execute(plan);

        // band power
        for (int b = 0; b < BARS; b++) {
            int lo = binEdges[b], hi = binEdges[b + 1];
            if (hi <= lo) hi = lo + 1;
            // PEAK, not average: the log-spaced top bands span hundreds of FFT
            // bins while the bottom ones span one or two, so averaging diluted
            // the treble to nothing and the right third of the visualizer read
            // as dead. A peak keeps every band honest.
            float peak = 0.f;
            for (int k = lo; k < hi; k++) {
                float re = out[k][0], im = out[k][1];
                float p2 = re * re + im * im;
                if (p2 > peak) peak = p2;
            }
            bandPower[b] = peak;
        }

        // QPANEL_DSP_DEBUG=1 prints the 32 normalized band levels once a second
        static const bool dbg = std::getenv("QPANEL_DSP_DEBUG") != nullptr;
        static int dbgTick = 0;

        // Display normalization. A per-band auto gain made every bar sit at
        // the same height (each band pinned to its own peak), and no gain at all
        // left the treble invisible: music falls off ~20-50x from mids to the
        // top octave. So: normalize against a slow GLOBAL peak (natural bass ->
        // treble slope, no band flattening) and lift the quiet end with a power
        // curve so the treble still dances.
        float frameMax = 0.f;
        for (int b = 0; b < BARS; b++) {
            float raw = std::sqrt(bandPower[b]) * 0.05f;
            agc_[b] = raw;
            if (raw > frameMax) frameMax = raw;
        }
        globPeak_ = std::max(frameMax, globPeak_ * 0.9990f);   // ~3s decay
        float ref = std::max(0.012f, globPeak_);

        float newBars[BW];
        for (int b = 0; b < BARS; b++) {
            float rel = std::clamp(agc_[b] / ref, 0.f, 1.f);
            float v = std::pow(rel, 0.65f) * 0.95f;             // lift the top end
            if (agc_[b] < 0.0015f) v = 0.f;                      // absolute noise floor
            // (a RELATIVE gate zeroed whole bass/mid bands whenever the global
            //  peak sat elsewhere in the spectrum)
            newBars[b] = v;
        }

        if (dbg && (++dbgTick % 30) == 0) {
            std::fprintf(stderr, "dsp:");
            for (int b = 0; b < BARS; b += 2) std::fprintf(stderr, " %.2f", newBars[b]);
            std::fprintf(stderr, "\n");
        }

        // CAVA-style physics: instant rise (clamped), accelerating gravity fall
        {
            std::lock_guard<std::mutex> g(m_);
            for (int b = 0; b < BARS; b++) {
                float& v = bars_[b];
                float t = newBars[b];
                if (t >= v || t > 0.985f) {
                    v = t;
                    fall_[b] = 0.f;
                } else {
                    fall_[b] += 0.9f + 1.7f * fall_[b]; // gravity accel
                    if (fall_[b] > 12.f) fall_[b] = 12.f;
                    v -= fall_[b] * 0.016f;
                    if (v < t) { v = t; fall_[b] = 0.f; }
                    if (v < 0.f) v = 0.f;
                }
            }
        }
    }

    fftwf_destroy_plan(plan);
    fftwf_free(in);
    fftwf_free(out);
}

} // namespace qp
