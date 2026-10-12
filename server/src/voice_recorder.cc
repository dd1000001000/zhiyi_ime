// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
#include "voice_recorder.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <cxxime/logging.h>

namespace {

template <typename T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr() {
        if (p) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
};

VoiceRecorder::Error error_for(HRESULT hr) {
    if (hr == E_ACCESSDENIED) return VoiceRecorder::Error::kDenied;
    if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) || hr == AUDCLNT_E_DEVICE_INVALIDATED) {
        return VoiceRecorder::Error::kNoDevice;
    }
    return VoiceRecorder::Error::kFailed;
}

}  // namespace

VoiceRecorder::~VoiceRecorder() {
    bool heard = false;
    stop(&heard);
}

void VoiceRecorder::start(cxxime::voice::SpeechDetector* detector, int max_seconds, Listener listener) {
    bool heard = false;
    stop(&heard);
    detector_ = detector;
    if (detector_) detector_->reset();
    listener_ = std::move(listener);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        samples_.clear();
        window_.clear();
        limit_reached_ = false;
    }
    stop_ = false;
    thread_ = std::thread(&VoiceRecorder::run, this, max_seconds);
}

std::vector<float> VoiceRecorder::stop(bool* heard) {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    *heard = detector_ && detector_->heard();
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<float> out = std::move(samples_);
    samples_.clear();
    window_.clear();
    return out;
}

void VoiceRecorder::deliver(const float* samples, std::size_t count, std::size_t max_samples) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t room = max_samples > samples_.size() ? max_samples - samples_.size() : 0;
    count = (std::min)(count, room);
    samples_.insert(samples_.end(), samples, samples + count);
    window_.insert(window_.end(), samples, samples + count);
    while (window_.size() >= cxxime::voice::kVadWindow) {
        double energy = 0.0;
        for (std::size_t i = 0; i < cxxime::voice::kVadWindow; ++i) energy += window_[i] * window_[i];
        const double rms = std::sqrt(energy / cxxime::voice::kVadWindow);
        // About -50 dB to -10 dB onto 0..1, so a normal voice fills most of the bar.
        const float level = static_cast<float>(
            std::clamp((20.0 * std::log10(rms + 1e-9) + 50.0) / 40.0, 0.0, 1.0));
        const bool speaking = detector_ && detector_->accept(window_.data());
        if (listener_.level) listener_.level(level, speaking);
        window_.erase(window_.begin(), window_.begin() + cxxime::voice::kVadWindow);
    }
    if (samples_.size() >= max_samples) limit_reached_ = true;
}

void VoiceRecorder::run(int max_seconds) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const std::size_t max_samples =
        static_cast<std::size_t>((std::max)(1, max_seconds)) * cxxime::voice::kSampleRate;
    Error error = Error::kNone;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioCaptureClient> capture;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(&enumerator));
        if (SUCCEEDED(hr)) hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
        if (SUCCEEDED(hr)) {
            hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(&client));
        }
        WAVEFORMATEX format = {};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = 1;
        format.nSamplesPerSec = cxxime::voice::kSampleRate;
        format.wBitsPerSample = 32;
        format.nBlockAlign = 4;
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        if (SUCCEEDED(hr)) {
            // Windows converts the device's format (AUTOCONVERTPCM); 200 ms of buffer.
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                    2000000, 0, &format, nullptr);
        }
        if (SUCCEEDED(hr)) hr = client->SetEventHandle(event);
        if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&capture));
        if (SUCCEEDED(hr)) hr = client->Start();
        if (FAILED(hr)) {
            error = error_for(hr);
            CXXIME_LOG(L"voice: microphone failed hr=0x%08x", static_cast<unsigned>(hr));
        }
        while (error == Error::kNone && !stop_) {
            WaitForSingleObject(event, 100);
            UINT32 packet = 0;
            while (SUCCEEDED(hr = capture->GetNextPacketSize(&packet)) && packet > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (FAILED(hr)) break;
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !data) {
                    const std::vector<float> silence(frames, 0.0f);
                    deliver(silence.data(), silence.size(), max_samples);
                } else {
                    deliver(reinterpret_cast<const float*>(data), frames, max_samples);
                }
                capture->ReleaseBuffer(frames);
            }
            if (FAILED(hr)) {
                error = error_for(hr);
                CXXIME_LOG(L"voice: microphone stopped hr=0x%08x", static_cast<unsigned>(hr));
                break;
            }
            bool limit = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                limit = limit_reached_;
            }
            if (limit) {
                client->Stop();
                if (listener_.limit) listener_.limit();
                break;
            }
        }
        if (client.p) client->Stop();
    }
    CloseHandle(event);
    if (SUCCEEDED(com)) CoUninitialize();
    if (error != Error::kNone && listener_.error) listener_.error(error);
}
