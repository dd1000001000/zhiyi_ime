// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Voice input (docs/voice-input.md), one recording at a time for the session whose voice key
// started it:
//
//   idle --voice key--> listening --voice key / stop button / longest length--> recognizing
//        --> done (the client takes the text: VOICE_STATUS) --> idle
//
// While listening and recognizing the session's keys do not reach the input box
// (Engine::set_voice_capture). Esc, the client's VOICE_CANCEL, another session getting the focus
// or the session ending drop it. A recording without speech (the VAD heard nothing) or without
// text shows a message and ends. The model is loaded on first use and unloaded after a while
// unused; recording, loading and recognizing run on threads of their own, never on the IPC
// thread.
#ifndef CXXIME_SERVER_VOICE_SERVICE_H_
#define CXXIME_SERVER_VOICE_SERVICE_H_

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <cxxime/config.h>
#include <cxxime/voice_recognizer.h>

#include "voice_overlay.h"
#include "voice_recorder.h"

class VoiceService {
public:
    // What the service needs of the sessions.
    struct Sessions {
        std::function<bool(uint32_t id, bool capturing)> set_capture;
        std::function<uint64_t(uint32_t id)> capabilities;
        // Chinese punctuation on (else the text's punctuation is made ASCII).
        std::function<bool(uint32_t id)> chinese_punct;
    };

    VoiceService();
    ~VoiceService();

    bool start(Sessions sessions, const std::shared_ptr<const cxxime::Config>& config);
    void stop();
    void update_config(const std::shared_ptr<const cxxime::Config>& config);

    // The voice key of session `id`: starts recording, or ends it.
    void toggle(uint32_t id);
    // Esc while recording, or the client's VOICE_CANCEL.
    void cancel(uint32_t id);
    void focus_in(uint32_t id);   // another session's recording is dropped
    void focus_out(uint32_t id);  // this session's is
    void session_ended(uint32_t id);

    // kVoice* (ipc_protocol.h) for session `id`.
    uint32_t state(uint32_t id);
    // VOICE_STATUS: as state(), and when done the text, once (the session's keys are then free).
    uint32_t take_status(uint32_t id, std::string* text);

private:
    enum class Phase { kIdle, kListening, kRecognizing, kDone };

    void post(std::function<void()> task);
    void run();
    // Drops the recording of the owner (mutex_ held); returns the session to free.
    uint32_t drop_locked();
    void release_capture(uint32_t id);
    void begin_recording(uint64_t generation);
    void finish_recording(uint64_t generation);
    void recording_failed(uint64_t generation, VoiceRecorder::Error error);
    bool ensure_detector(std::string* error);
    bool ensure_recognizer(std::string* error);
    std::string tidy(std::string text, uint32_t owner);

    Sessions sessions_;
    VoiceOverlay overlay_;

    std::mutex mutex_;
    Phase phase_ = Phase::kIdle;
    uint32_t owner_ = 0;
    uint64_t generation_ = 0;
    std::string text_;
    std::chrono::steady_clock::time_point done_at_{};
    std::shared_ptr<const cxxime::Config> config_;

    // The worker's own.
    std::thread worker_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> tasks_;
    bool stopping_ = false;
    VoiceRecorder recorder_;
    cxxime::voice::SpeechDetector detector_;
    std::wstring detector_model_;
    std::unique_ptr<cxxime::voice::SpeechRecognizer> recognizer_;
    std::wstring recognizer_model_;
    std::string recognizer_device_;
    std::chrono::steady_clock::time_point last_used_{};
};

#endif  // CXXIME_SERVER_VOICE_SERVICE_H_
