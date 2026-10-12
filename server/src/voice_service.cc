// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
#include "voice_service.h"

#include <windows.h>

#include <algorithm>
#include <optional>
#include <utility>

#include <cxxime/gpu_adapters.h>
#include <cxxime/ipc_protocol.h>
#include <cxxime/logging.h>
#include <cxxime/voice_files.h>

namespace {

// The text waits this long for its client; then the session's keys are freed anyway.
constexpr auto kDoneTimeout = std::chrono::seconds(5);
// The model leaves memory after this long unused.
constexpr auto kModelIdle = std::chrono::minutes(10);

VoiceOverlay::Message message_for(VoiceRecorder::Error error) {
    switch (error) {
    case VoiceRecorder::Error::kNoDevice:
        return VoiceOverlay::Message::kNoMicrophone;
    case VoiceRecorder::Error::kDenied:
        return VoiceOverlay::Message::kMicrophoneDenied;
    default:
        return VoiceOverlay::Message::kMicrophoneFailed;
    }
}

// Full-width punctuation the model writes, as typed in English punctuation mode.
const std::pair<const char*, const char*> kAsciiPunctuation[] = {
    {"\xEF\xBC\x8C", ", "},  // ，
    {"\xE3\x80\x82", ". "},  // 。
    {"\xEF\xBC\x9F", "? "},  // ？
    {"\xEF\xBC\x81", "! "},  // ！
    {"\xEF\xBC\x9A", ": "},  // ：
    {"\xEF\xBC\x9B", "; "},  // ；
    {"\xE3\x80\x81", ", "},  // 、
};

}  // namespace

VoiceService::VoiceService() = default;

VoiceService::~VoiceService() { stop(); }

bool VoiceService::start(Sessions sessions, const std::shared_ptr<const cxxime::Config>& config) {
    sessions_ = std::move(sessions);
    update_config(config);
    // The stop button: as the owner's voice key.
    const bool overlay = overlay_.start([this] {
        uint32_t owner = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            owner = owner_;
        }
        if (owner) toggle(owner);
    });
    stopping_ = false;
    worker_ = std::thread(&VoiceService::run, this);
    return overlay;
}

void VoiceService::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!worker_.joinable()) return;
        stopping_ = true;
        ++generation_;
    }
    wake_.notify_all();
    worker_.join();
    overlay_.stop();
}

void VoiceService::update_config(const std::shared_ptr<const cxxime::Config>& config) {
    if (!config) return;
    overlay_.update_config(config);
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
}

void VoiceService::post(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.push_back(std::move(task));
    }
    wake_.notify_all();
}

void VoiceService::run() {
    for (;;) {
        std::function<void()> task;
        uint32_t expired = 0;
        bool unload = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait_for(lock, std::chrono::seconds(1), [&] { return stopping_ || !tasks_.empty(); });
            if (stopping_) break;
            if (!tasks_.empty()) {
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            const auto now = std::chrono::steady_clock::now();
            if (phase_ == Phase::kDone && now - done_at_ > kDoneTimeout) {
                CXXIME_LOG(L"%s", L"voice: the client did not take the text");
                expired = drop_locked();
            }
            unload = phase_ == Phase::kIdle && recognizer_ && now - last_used_ > kModelIdle;
        }
        if (expired) release_capture(expired);
        if (unload) {
            recognizer_.reset();
            recognizer_model_.clear();
            CXXIME_LOG(L"%s", L"voice: model unloaded (unused)");
        }
        if (task) task();
    }
    bool heard = false;
    recorder_.stop(&heard);
}

uint32_t VoiceService::drop_locked() {
    const uint32_t owner = owner_;
    phase_ = Phase::kIdle;
    owner_ = 0;
    text_.clear();
    ++generation_;
    return owner;
}

void VoiceService::release_capture(uint32_t id) {
    if (id && sessions_.set_capture) sessions_.set_capture(id, false);
    // The recording (if any) ends on the worker.
    post([this] {
        bool heard = false;
        if (recorder_.recording()) recorder_.stop(&heard);
    });
}

void VoiceService::toggle(uint32_t id) {
    std::shared_ptr<const cxxime::Config> config;
    uint32_t dropped = 0;
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (owner_ == id && phase_ == Phase::kListening) {
            phase_ = Phase::kRecognizing;
            generation = generation_;
            overlay_.show_recognizing();
            tasks_.push_back([this, generation] { finish_recording(generation); });
            wake_.notify_all();
            return;
        }
        if (owner_ == id && phase_ == Phase::kRecognizing) return;
        if (phase_ != Phase::kIdle) dropped = drop_locked();
        config = config_;
    }
    if (dropped) release_capture(dropped);
    // Can it start?
    std::optional<VoiceOverlay::Message> refused;
    if (!config || !config->voice_enable) {
        refused = VoiceOverlay::Message::kDisabled;
    } else if (!sessions_.capabilities ||
               (sessions_.capabilities(id) & cxxime::kClientCapabilityVoiceInput) == 0) {
        refused = VoiceOverlay::Message::kRestartProgram;
    } else if (!cxxime::voice::find_installed(nullptr)) {
        refused = VoiceOverlay::Message::kNoModel;
    }
    if (refused) {
        overlay_.show_message(*refused);
        return;
    }
    if (!sessions_.set_capture || !sessions_.set_capture(id, true)) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        phase_ = Phase::kListening;
        owner_ = id;
        text_.clear();
        generation = ++generation_;
        overlay_.show_listening();
        tasks_.push_back([this, generation] { begin_recording(generation); });
    }
    wake_.notify_all();
}

void VoiceService::cancel(uint32_t id) {
    uint32_t dropped = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!id || owner_ != id || phase_ == Phase::kIdle) return;
        dropped = drop_locked();
    }
    overlay_.hide();
    release_capture(dropped);
}

void VoiceService::focus_in(uint32_t id) {
    uint32_t owner = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        owner = owner_;
    }
    if (owner && owner != id) cancel(owner);
}

void VoiceService::focus_out(uint32_t id) { cancel(id); }

void VoiceService::session_ended(uint32_t id) { cancel(id); }

uint32_t VoiceService::state(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (owner_ != id) return cxxime::kVoiceIdle;
    switch (phase_) {
    case Phase::kListening:
        return cxxime::kVoiceListening;
    case Phase::kRecognizing:
        return cxxime::kVoiceRecognizing;
    case Phase::kDone:
        return cxxime::kVoiceDone;
    default:
        return cxxime::kVoiceIdle;
    }
}

uint32_t VoiceService::take_status(uint32_t id, std::string* text) {
    text->clear();
    uint32_t done = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (owner_ != id || phase_ != Phase::kDone) {
            return owner_ != id                      ? cxxime::kVoiceIdle
                   : phase_ == Phase::kListening     ? cxxime::kVoiceListening
                   : phase_ == Phase::kRecognizing   ? cxxime::kVoiceRecognizing
                                                     : cxxime::kVoiceIdle;
        }
        *text = std::move(text_);
        done = drop_locked();
    }
    if (done && sessions_.set_capture) sessions_.set_capture(done, false);
    return cxxime::kVoiceDone;
}

bool VoiceService::ensure_detector(std::string* error) {
    cxxime::voice::InstalledModel installed;
    if (!cxxime::voice::find_installed(&installed)) {
        if (error) *error = "model not installed";
        return false;
    }
    if (detector_.loaded() && detector_model_ == installed.vad) return true;
    if (!detector_.load(installed.vad, error)) return false;
    detector_model_ = installed.vad;
    return true;
}

bool VoiceService::ensure_recognizer(std::string* error) {
    cxxime::voice::InstalledModel installed;
    if (!cxxime::voice::find_installed(&installed)) {
        if (error) *error = "model not installed";
        return false;
    }
    std::string device;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (config_) device = config_->voice_device;
    }
    last_used_ = std::chrono::steady_clock::now();
    if (recognizer_ && recognizer_model_ == installed.model && recognizer_device_ == device) return true;
    recognizer_.reset();
    auto recognizer = std::make_unique<cxxime::voice::SpeechRecognizer>();
    const auto start = std::chrono::steady_clock::now();
    bool loaded = false;
    if (!device.empty()) {
        const std::vector<cxxime::GpuAdapter> adapters = cxxime::list_gpu_adapters();
        const cxxime::GpuAdapter* adapter = cxxime::find_gpu_adapter(adapters, device);
        std::string gpu_error = "graphics card not found";
        loaded = adapter && recognizer->load(installed.model, installed.tokens,
                                             static_cast<int>(adapter->index), 0, &gpu_error);
        if (!loaded) {
            CXXIME_LOG(L"voice: graphics card %S failed (%S), using the CPU", device.c_str(),
                       gpu_error.c_str());
        }
    }
    if (!loaded) {
        loaded = recognizer->load(installed.model, installed.tokens, -1, cxxime::voice::cpu_threads(),
                                  error);
    }
    if (!loaded) return false;
    CXXIME_LOG(L"voice: model loaded in %lld ms",
               static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - start)
                                          .count()));
    recognizer_ = std::move(recognizer);
    recognizer_model_ = installed.model;
    recognizer_device_ = device;
    return true;
}

void VoiceService::begin_recording(uint64_t generation) {
    int max_seconds = 60;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != generation_ || phase_ != Phase::kListening) return;
        if (config_) max_seconds = config_->voice_max_seconds;
    }
    std::string error;
    if (!ensure_detector(&error)) {
        CXXIME_LOG(L"voice: speech detector failed: %S", error.c_str());
        recording_failed(generation, VoiceRecorder::Error::kFailed);
        return;
    }
    VoiceRecorder::Listener listener;
    listener.level = [this](float level, bool speaking) { overlay_.add_level(level, speaking); };
    listener.error = [this, generation](VoiceRecorder::Error e) {
        post([this, generation, e] { recording_failed(generation, e); });
    };
    listener.limit = [this, generation] {
        // As the voice key: what was recorded is recognized.
        uint32_t owner = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation == generation_ && phase_ == Phase::kListening) owner = owner_;
        }
        if (owner) toggle(owner);
    };
    recorder_.start(&detector_, max_seconds, std::move(listener));
    // The model, while the user speaks (the first time takes about a second).
    if (!ensure_recognizer(&error)) CXXIME_LOG(L"voice: model failed to load: %S", error.c_str());
}

void VoiceService::recording_failed(uint64_t generation, VoiceRecorder::Error error) {
    uint32_t dropped = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != generation_) return;
        dropped = drop_locked();
    }
    bool heard = false;
    recorder_.stop(&heard);
    overlay_.show_message(message_for(error));
    if (dropped && sessions_.set_capture) sessions_.set_capture(dropped, false);
}

void VoiceService::finish_recording(uint64_t generation) {
    bool heard = false;
    std::vector<float> samples = recorder_.stop(&heard);
    uint32_t owner = 0;
    std::string language = "auto";
    bool itn = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != generation_ || phase_ != Phase::kRecognizing) return;
        owner = owner_;
        if (config_) {
            language = config_->voice_language;
            itn = config_->voice_itn;
        }
    }
    auto end = [&](VoiceOverlay::Message message) {
        uint32_t dropped = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation != generation_) return;
            dropped = drop_locked();
        }
        overlay_.show_message(message);
        if (dropped && sessions_.set_capture) sessions_.set_capture(dropped, false);
    };
    if (!heard) {
        end(VoiceOverlay::Message::kNoSpeech);
        return;
    }
    std::string error;
    std::string text;
    const auto start = std::chrono::steady_clock::now();
    if (!ensure_recognizer(&error) ||
        !recognizer_->transcribe(samples.data(), samples.size(), cxxime::voice::parse_language(language),
                                 itn, &text, &error)) {
        CXXIME_LOG(L"voice: recognition failed: %S", error.c_str());
        end(VoiceOverlay::Message::kFailed);
        return;
    }
    last_used_ = std::chrono::steady_clock::now();
    CXXIME_LOG(L"voice: %.1f s recognized in %lld ms", samples.size() / 16000.0,
               static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          last_used_ - start)
                                          .count()));
    text = tidy(std::move(text), owner);
    if (text.empty()) {
        end(VoiceOverlay::Message::kNoText);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != generation_) return;
        phase_ = Phase::kDone;
        text_ = std::move(text);
        done_at_ = std::chrono::steady_clock::now();
    }
    overlay_.hide();
}

std::string VoiceService::tidy(std::string text, uint32_t owner) {
    if (sessions_.chinese_punct && !sessions_.chinese_punct(owner)) {
        for (const auto& [full, ascii] : kAsciiPunctuation) {
            for (std::size_t at = text.find(full); at != std::string::npos; at = text.find(full, at)) {
                text.replace(at, std::char_traits<char>::length(full), ascii);
            }
        }
        // No double spaces, none at the end.
        for (std::size_t at = text.find("  "); at != std::string::npos; at = text.find("  ", at)) {
            text.erase(at, 1);
        }
        while (!text.empty() && text.back() == ' ') text.pop_back();
    }
    // Room in the answer (cut at a character boundary).
    if (text.size() >= cxxime::kVoiceTextCapacity) {
        std::size_t cut = cxxime::kVoiceTextCapacity - 1;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
        text.resize(cut);
    }
    return text;
}
