// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Voice input's recording (docs/voice-input.md): the default microphone through WASAPI in shared
// mode, converted by Windows to 16 kHz mono, on a thread of its own. Every 32 ms window goes to
// the speech detector (voice_recognizer.h) and the level meter of the overlay; the recording
// stays in memory only.
#ifndef CXXIME_SERVER_VOICE_RECORDER_H_
#define CXXIME_SERVER_VOICE_RECORDER_H_

#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include <cxxime/voice_recognizer.h>

class VoiceRecorder {
public:
    enum class Error {
        kNone,
        kNoDevice,  // no microphone
        kDenied,    // Windows' privacy settings do not let desktop apps use the microphone
        kFailed,
    };
    struct Listener {
        // Each window: its level (0..1, for the overlay) and whether someone is speaking.
        std::function<void(float level, bool speaking)> level;
        // The microphone could not be opened or stopped working (recording has ended).
        std::function<void(Error)> error;
        // The recording reached its longest length (recording has ended).
        std::function<void()> limit;
    };

    VoiceRecorder() = default;
    ~VoiceRecorder();
    VoiceRecorder(const VoiceRecorder&) = delete;
    VoiceRecorder& operator=(const VoiceRecorder&) = delete;

    // Starts recording on the thread. `detector` (reset here) is used by that thread until
    // stop(); the listener is called from it.
    void start(cxxime::voice::SpeechDetector* detector, int max_seconds, Listener listener);
    // Ends recording: the samples (16 kHz, [-1, 1]) and whether the detector heard speech.
    std::vector<float> stop(bool* heard);
    bool recording() const { return thread_.joinable(); }

private:
    void run(int max_seconds);
    void deliver(const float* samples, std::size_t count, std::size_t max_samples);

    std::thread thread_;
    std::atomic<bool> stop_{false};
    cxxime::voice::SpeechDetector* detector_ = nullptr;
    Listener listener_;
    std::mutex mutex_;
    std::vector<float> samples_;
    std::vector<float> window_;  // samples not yet a whole VAD window
    bool limit_reached_ = false;
};

#endif  // CXXIME_SERVER_VOICE_RECORDER_H_
