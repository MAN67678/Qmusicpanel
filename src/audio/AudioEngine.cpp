#include "AudioEngine.hpp"
#include <cstring>
#include <chrono>

namespace qp {

using namespace std::chrono_literals;

bool AudioEngine::init() {
    tap_.assign(TAP_SIZE, 0.f);
    if (ma_pcm_rb_init(ma_format_f32, 2, 16384, nullptr, nullptr, &rb_) != MA_SUCCESS)
        return false;
    rbReady_ = true;
    ma_device_config dc = ma_device_config_init(ma_device_type_playback);
    dc.playback.format = ma_format_f32;
    dc.playback.channels = 2;
    dc.sampleRate = 44100;
    dc.dataCallback = &AudioEngine::deviceCb;
    dc.pUserData = this;
    return ma_device_init(nullptr, &dc, &dev_) == MA_SUCCESS;
}

void AudioEngine::shutdown() {
    running_ = false;
    if (decodeThread_.joinable()) decodeThread_.join();
    ma_device_uninit(&dev_);
    if (decOpen_) { ma_decoder_uninit(&dec_); decOpen_ = false; }
    // the PCM ring is malloc'd by miniaudio and owns 128 KB: uninit it or
    // LeakSanitizer reports a direct leak of the ring's backing buffer at exit.
    if (rbReady_) { ma_pcm_rb_uninit(&rb_); rbReady_ = false; }
}

double AudioEngine::load(const std::string& path) {
    failed_ = false;
    eof_ = false;
    decodeDone_ = false;
    posFrames_ = 0;
    seekTarget_ = UINT64_MAX;

    playing_ = false; // device cb will emit silence while we swap
    if (decodeThread_.joinable()) { running_ = false; decodeThread_.join(); }
    if (decOpen_) { ma_decoder_uninit(&dec_); decOpen_ = false; }

    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 2, 44100);
    ma_result r = ma_decoder_init_file(path.c_str(), &cfg, &dec_);
    if (r != MA_SUCCESS) {
        failed_ = true;
        running_ = true;
        decodeThread_ = std::thread(&AudioEngine::decodeLoop, this);
        return 0.0;
    }
    decOpen_ = true;
    durSec_ = 0.0;
    totalFrames_ = 0;
    ma_uint64 len = 0;
    if (ma_decoder_get_length_in_pcm_frames(&dec_, &len) == MA_SUCCESS && len > 0) {
        totalFrames_ = len;
        durSec_ = double(len) / 44100.0;
    }
    ma_pcm_rb_reset(&rb_);
    tapReset();
    decodeDone_ = false;
    running_ = true;
    playing_ = true;
    decodeThread_ = std::thread(&AudioEngine::decodeLoop, this);
    (void)ma_device_start(&dev_);
    return durSec_;
}

void AudioEngine::unload() {
    pause();
    if (decodeThread_.joinable()) { running_ = false; decodeThread_.join(); }
    if (decOpen_) { ma_decoder_uninit(&dec_); decOpen_ = false; }
    durSec_ = 0.0; totalFrames_ = 0;
}

void AudioEngine::play() {
    if (!decOpen_) return;
    if (eof_) seekTo(0.0);
    eof_ = false;
    playing_ = true;
    (void)ma_device_start(&dev_);
}
void AudioEngine::pause() { playing_ = false; }
void AudioEngine::toggle() { if (playing_) pause(); else play(); }

void AudioEngine::seekTo(double seconds) {
    if (!decOpen_ || totalFrames_ == 0) return;
    uint64_t frame = (durSec_ > 0.0)
        ? uint64_t(std::min<uint64_t>(uint64_t(seconds * 44100.0), totalFrames_ - 1))
        : 0;
    if (seconds <= 0) frame = 0;
    if (frame >= totalFrames_) frame = totalFrames_ - 1;
    seekTarget_ = frame;
    posFrames_ = frame;
    decodeDone_ = false;
    eof_ = false;
}
void AudioEngine::seekBy(double delta) { seekTo(position() + delta); }

double AudioEngine::position() const {
    uint64_t f = posFrames_.load();
    if (totalFrames_ && f > totalFrames_) f = totalFrames_;
    return double(f) / 44100.0;
}

// ---- decode thread: decoder -> ma_pcm_rb ----
void AudioEngine::decodeLoop() {
    while (running_) {
        if (!decOpen_ || !playing_) { std::this_thread::sleep_for(4ms); continue; }

        uint64_t target = seekTarget_.exchange(UINT64_MAX);
        if (target != UINT64_MAX) {
            ma_decoder_seek_to_pcm_frame(&dec_, (ma_uint64)target);
            ma_pcm_rb_reset(&rb_);
            decodeDone_ = false;
            continue;
        }

        if (decodeDone_) {
            // drained all buffered audio and finished -> EOF
            if (ma_pcm_rb_available_read(&rb_) == 0) {
                eof_ = true;
                std::this_thread::sleep_for(20ms);
            } else {
                std::this_thread::sleep_for(5ms);
            }
            continue;
        }

        ma_uint32 wrAvail = ma_pcm_rb_available_write(&rb_);
        if (wrAvail == 0) { std::this_thread::sleep_for(3ms); continue; }

        ma_uint32 chunk = wrAvail < 4096 ? wrAvail : 4096;
        void* ptr = nullptr;
        if (ma_pcm_rb_acquire_write(&rb_, &chunk, &ptr) != MA_SUCCESS || chunk == 0) {
            std::this_thread::sleep_for(3ms);
            continue;
        }
        ma_uint64 got = 0;
        ma_result r = ma_decoder_read_pcm_frames(&dec_, ptr, chunk, &got);
        if (r != MA_SUCCESS || got == 0) {
            ma_pcm_rb_commit_write(&rb_, 0);
            decodeDone_ = true;
            continue;
        }
        ma_pcm_rb_commit_write(&rb_, (ma_uint32)got);
        if (got < chunk) decodeDone_ = true;
    }
}

// ---- device callback: pcm ring -> output (+ mono tap + volume) ----
void AudioEngine::deviceCb(ma_device* dev, void* out, const void*, ma_uint32 frames) {
    auto* self = static_cast<AudioEngine*>(dev->pUserData);
    float* dst = static_cast<float*>(out);
    size_t nframes = size_t(frames);

    if (!self->playing_.load()) {
        std::memset(dst, 0, nframes * 2 * sizeof(float));
        return;
    }

    ma_uint32 got = 0;
    if (ma_pcm_rb_available_read(&self->rb_) > 0) {
        ma_uint32 want = (ma_uint32)frames;
        void* p = nullptr;
        got = want;
        if (ma_pcm_rb_acquire_read(&self->rb_, &got, &p) == MA_SUCCESS && got > 0) {
            std::memcpy(dst, p, size_t(got) * 2 * sizeof(float));
            ma_pcm_rb_commit_read(&self->rb_, got);
        } else {
            got = 0;
        }
    }

    // underrun tail
    if (size_t(got) < nframes)
        std::memset(dst + size_t(got) * 2, 0, (nframes - size_t(got)) * 2 * sizeof(float));

    // sample-accurate-ish position
    self->posFrames_ += got;

    // volume gain MUST be applied to the output, not just the visualizer tap:
    // this is the only place audio leaves the engine.
    float vol = self->vol_.load(std::memory_order_relaxed);
    if (vol < 1.f)
        for (size_t i = 0; i < size_t(got) * 2; i++) dst[i] *= vol;

    // mono downmix tap (drop-on-full), SPSC with DSP thread
    size_t avail = self->tapAvail();
    size_t space = self->TAP_SIZE - 1 - avail;
    if (space > 0) {
        size_t push = nframes < space ? nframes : space;
        size_t wr = self->tapWr_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < push; i++) {
            float l = dst[i * 2], r0 = dst[i * 2 + 1];
            self->tap_[(wr + i) % self->TAP_SIZE] = (l + r0) * 0.5f * vol;
        }
        self->tapWr_.store((wr + push) % self->TAP_SIZE, std::memory_order_release);
    }
}

} // namespace qp
