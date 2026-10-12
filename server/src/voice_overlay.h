// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Voice input's window (docs/voice-input.md): a small bar at the bottom of the screen the
// focused program is on. While recording it shows a microphone, a waveform that moves while
// someone speaks (a flat line otherwise) and a stop button; then that the speech is being turned
// into text; or a short message ("No speech detected") that goes away by itself. It never takes
// the focus. It runs on a thread of its own and follows the candidate window's theme.
#ifndef CXXIME_SERVER_VOICE_OVERLAY_H_
#define CXXIME_SERVER_VOICE_OVERLAY_H_

#include <windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <cxxime/config.h>

class VoiceOverlay {
public:
    enum class Message {
        kNoSpeech,        // nothing was said
        kNoText,          // speech, but no text came out
        kNoMicrophone,
        kMicrophoneDenied,
        kMicrophoneFailed,
        kNoModel,         // the model is not downloaded
        kDisabled,        // voice input is off in Settings
        kRestartProgram,  // the program still has the text service from before voice input
        kFailed,          // the model failed
    };

    VoiceOverlay();
    ~VoiceOverlay();
    VoiceOverlay(const VoiceOverlay&) = delete;
    VoiceOverlay& operator=(const VoiceOverlay&) = delete;

    // `stop_clicked` is called (on the window's thread) when the stop button is clicked.
    bool start(std::function<void()> stop_clicked);
    void stop();
    void update_config(const std::shared_ptr<const cxxime::Config>& config);

    void show_listening();
    // The level of the last 32 ms (0..1) and whether someone is speaking.
    void add_level(float level, bool speaking);
    void show_recognizing();
    void show_message(Message message);
    void hide();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif  // CXXIME_SERVER_VOICE_OVERLAY_H_
