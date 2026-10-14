// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
#include <cxxime/voice_recognizer.h>

#include <windows.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <complex>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include <dml_provider_factory.h>
#include <onnxruntime_cxx_api.h>

namespace cxxime {
namespace voice {

namespace {

constexpr std::size_t kFrameLength = 400;  // 25 ms
constexpr std::size_t kFrameShift = 160;   // 10 ms
constexpr std::size_t kFftSize = 512;
constexpr double kPreemphasis = 0.97;
constexpr double kLowFrequency = 20.0;
constexpr double kPi = 3.14159265358979323846;

double mel_scale(double hz) { return 1127.0 * std::log(1.0 + hz / 700.0); }

// One mel bin's weights over the FFT bins [first, first + weights.size()).
struct MelBin {
    std::size_t first = 0;
    std::vector<double> weights;
};

// Kaldi's triangular mel bins over the FFT bins below Nyquist.
const std::vector<MelBin>& mel_bins() {
    static const std::vector<MelBin> bins = [] {
        const double low = mel_scale(kLowFrequency);
        const double high = mel_scale(kSampleRate / 2.0);
        const double delta = (high - low) / (kFbankBins + 1);
        std::vector<MelBin> out(kFbankBins);
        for (std::size_t b = 0; b < kFbankBins; ++b) {
            const double left = low + b * delta;
            const double center = left + delta;
            const double right = center + delta;
            MelBin& bin = out[b];
            for (std::size_t k = 0; k < kFftSize / 2; ++k) {
                const double mel = mel_scale(static_cast<double>(k) * kSampleRate / kFftSize);
                if (mel <= left || mel >= right) {
                    if (!bin.weights.empty()) break;
                    continue;
                }
                if (bin.weights.empty()) bin.first = k;
                bin.weights.push_back(mel <= center ? (mel - left) / (center - left)
                                                    : (right - mel) / (right - center));
            }
        }
        return out;
    }();
    return bins;
}

const std::vector<double>& hamming_window() {
    static const std::vector<double> window = [] {
        std::vector<double> w(kFrameLength);
        for (std::size_t i = 0; i < kFrameLength; ++i) {
            w[i] = 0.54 - 0.46 * std::cos(2.0 * kPi * i / (kFrameLength - 1));
        }
        return w;
    }();
    return window;
}

// In-place radix-2 FFT of kFftSize points.
void fft(std::vector<std::complex<double>>& a) {
    static const std::vector<std::complex<double>> twiddles = [] {
        std::vector<std::complex<double>> t(kFftSize / 2);
        for (std::size_t i = 0; i < t.size(); ++i) {
            t[i] = std::polar(1.0, -2.0 * kPi * i / kFftSize);
        }
        return t;
    }();
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t length = 2; length <= n; length <<= 1) {
        const std::size_t step = n / length;
        for (std::size_t start = 0; start < n; start += length) {
            for (std::size_t k = 0; k < length / 2; ++k) {
                const std::complex<double> u = a[start + k];
                const std::complex<double> v = a[start + k + length / 2] * twiddles[k * step];
                a[start + k] = u + v;
                a[start + k + length / 2] = u - v;
            }
        }
    }
}

std::string ort_error(const Ort::Exception& e) { return std::string("onnxruntime: ") + e.what(); }

Ort::Env& ort_env() {
    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "zhiyu-voice");
    return env;
}

std::string metadata(Ort::Session& session, const char* key) {
    Ort::AllocatorWithDefaultOptions allocator;
    const Ort::ModelMetadata meta = session.GetModelMetadata();
    const Ort::AllocatedStringPtr value = meta.LookupCustomMetadataMapAllocated(key, allocator);
    return value ? std::string(value.get()) : std::string();
}

int metadata_int(Ort::Session& session, const char* key, int fallback) {
    const std::string value = metadata(session, key);
    if (value.empty()) return fallback;
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

std::vector<float> metadata_floats(Ort::Session& session, const char* key) {
    std::vector<float> out;
    std::stringstream in(metadata(session, key));
    std::string item;
    while (std::getline(in, item, ',')) {
        try {
            out.push_back(std::stof(item));
        } catch (...) {
            return {};
        }
    }
    return out;
}

}  // namespace

std::vector<float> fbank(const float* samples, std::size_t count, std::size_t* frames) {
    *frames = count < kFrameLength ? 0 : 1 + (count - kFrameLength) / kFrameShift;
    std::vector<float> out(*frames * kFbankBins);
    const std::vector<double>& window = hamming_window();
    const std::vector<MelBin>& bins = mel_bins();
    std::vector<double> frame(kFrameLength);
    std::vector<std::complex<double>> spectrum(kFftSize);
    std::vector<double> power(kFftSize / 2);
    for (std::size_t t = 0; t < *frames; ++t) {
        const float* in = samples + t * kFrameShift;
        double mean = 0.0;
        for (std::size_t i = 0; i < kFrameLength; ++i) {
            frame[i] = in[i] * 32768.0;  // the model was trained on 16-bit sample values
            mean += frame[i];
        }
        mean /= kFrameLength;
        for (double& value : frame) value -= mean;
        for (std::size_t i = kFrameLength - 1; i > 0; --i) frame[i] -= kPreemphasis * frame[i - 1];
        frame[0] -= kPreemphasis * frame[0];
        std::fill(spectrum.begin(), spectrum.end(), std::complex<double>());
        for (std::size_t i = 0; i < kFrameLength; ++i) spectrum[i] = frame[i] * window[i];
        fft(spectrum);
        for (std::size_t k = 0; k < power.size(); ++k) power[k] = std::norm(spectrum[k]);
        float* row = out.data() + t * kFbankBins;
        for (std::size_t b = 0; b < kFbankBins; ++b) {
            double energy = 0.0;
            for (std::size_t i = 0; i < bins[b].weights.size(); ++i) {
                energy += bins[b].weights[i] * power[bins[b].first + i];
            }
            row[b] = static_cast<float>(std::log((std::max)(energy, static_cast<double>(FLT_EPSILON))));
        }
    }
    return out;
}

std::vector<float> stack_frames(const std::vector<float>& features, std::size_t frames,
                                std::size_t* stacked) {
    *stacked = (frames + kStackedShift - 1) / kStackedShift;
    std::vector<float> out(*stacked * kStackedDim);
    const std::size_t padding = (kStackedFrames - 1) / 2;
    for (std::size_t i = 0; i < *stacked; ++i) {
        for (std::size_t j = 0; j < kStackedFrames; ++j) {
            // Position in the padded sequence: the first frame `padding` times, then all frames,
            // then the last frame for as long as needed.
            const std::size_t padded = i * kStackedShift + j;
            const std::size_t source =
                padded < padding ? 0 : (std::min)(padded - padding, frames - 1);
            std::copy_n(features.data() + source * kFbankBins, kFbankBins,
                        out.data() + i * kStackedDim + j * kFbankBins);
        }
    }
    return out;
}

Language parse_language(const std::string& name) {
    if (name == "zh") return Language::kMandarin;
    if (name == "en") return Language::kEnglish;
    if (name == "yue") return Language::kCantonese;
    return Language::kAuto;
}

bool runtime_available() {
    return LoadLibraryExW(L"onnxruntime.dll", nullptr,
                          LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS) !=
           nullptr;
}

int cpu_threads() {
    const unsigned cores = std::thread::hardware_concurrency();
    return static_cast<int>(std::clamp(cores / 2, 1u, 4u));
}

double time_recognition(const std::wstring& model, const std::wstring& tokens, int gpu_device,
                        std::string* error) {
    SpeechRecognizer recognizer;
    if (!recognizer.load(model, tokens, gpu_device, cpu_threads(), error)) return 0.0;
    // A voice-like sound: a buzz around 150 Hz whose loudness and pitch wander, with a little
    // noise (the time depends on the length, not on what is said).
    std::vector<float> sound(static_cast<std::size_t>(kTimedSeconds) * kSampleRate);
    uint32_t seed = 12345;
    double phase = 0.0;
    for (std::size_t i = 0; i < sound.size(); ++i) {
        const double t = static_cast<double>(i) / kSampleRate;
        phase += 2.0 * kPi * (150.0 + 40.0 * std::sin(2.0 * kPi * 0.7 * t)) / kSampleRate;
        seed = seed * 1664525u + 1013904223u;
        const double noise = (static_cast<double>(seed >> 8) / (1u << 24) - 0.5) * 0.02;
        const double loudness = 0.5 + 0.5 * std::sin(2.0 * kPi * 3.0 * t);
        sound[i] = static_cast<float>(0.2 * loudness * (std::sin(phase) + 0.5 * std::sin(2.0 * phase)) + noise);
    }
    // Every recording has a length of its own, and DirectML prepares each new length the first
    // time it sees it: the timed runs have lengths not seen before (0.4 s apart, the last the full
    // length), as speech would.
    std::vector<double> times;
    std::string text;
    for (int run = 0; run < 4; ++run) {
        const std::size_t length =
            run == 0 ? sound.size() * 3 / 5 : sound.size() - static_cast<std::size_t>(3 - run) * kSampleRate * 2 / 5;
        const auto start = std::chrono::steady_clock::now();
        if (!recognizer.transcribe(sound.data(), length, Language::kAuto, true, &text, error)) {
            return 0.0;
        }
        if (run > 0) {
            times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                                .count());
        }
    }
    std::sort(times.begin(), times.end());
    return (std::max)(0.1, times[times.size() / 2]);
}

// ---- SenseVoice ----

struct SpeechRecognizer::Impl {
    std::unique_ptr<Ort::Session> session;
    std::vector<float> neg_mean;
    std::vector<float> inv_stddev;
    std::vector<std::string> tokens;  // by id
    int lang_auto = 0, lang_zh = 3, lang_en = 4, lang_yue = 7;
    int with_itn = 14, without_itn = 15;
};

SpeechRecognizer::SpeechRecognizer() = default;
SpeechRecognizer::~SpeechRecognizer() = default;

bool SpeechRecognizer::loaded() const { return impl_ && impl_->session; }

bool SpeechRecognizer::load(const std::wstring& model, const std::wstring& tokens, int gpu_device,
                            int threads, std::string* error) {
    impl_.reset();
    if (!runtime_available()) {
        if (error) *error = "onnxruntime.dll not found";
        return false;
    }
    auto impl = std::make_unique<Impl>();
    std::ifstream in{std::filesystem::path(tokens)};
    if (!in) {
        if (error) *error = "cannot read tokens.txt";
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t space = line.rfind(' ');
        if (space == std::string::npos || space == 0) continue;
        int id = -1;
        try {
            id = std::stoi(line.substr(space + 1));
        } catch (...) {
            continue;
        }
        if (id < 0 || id > 1000000) continue;
        if (impl->tokens.size() <= static_cast<std::size_t>(id)) impl->tokens.resize(id + 1);
        impl->tokens[id] = line.substr(0, space);
    }
    try {
        Ort::SessionOptions options;
        // On DirectML, ONNX Runtime 1.24's extended graph optimizations crash (access violation)
        // on this model while the session is made: basic ones only there.
        options.SetGraphOptimizationLevel(gpu_device >= 0 ? GraphOptimizationLevel::ORT_ENABLE_BASIC
                                                          : GraphOptimizationLevel::ORT_ENABLE_ALL);
        if (gpu_device >= 0) {
            options.DisableMemPattern();
            options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
            const OrtDmlApi* dml = nullptr;
            Ort::ThrowOnError(Ort::GetApi().GetExecutionProviderApi(
                "DML", ORT_API_VERSION, reinterpret_cast<const void**>(&dml)));
            if (!dml) {
                if (error) *error = "onnxruntime has no DirectML provider";
                return false;
            }
            Ort::ThrowOnError(dml->SessionOptionsAppendExecutionProvider_DML(options, gpu_device));
        } else {
            options.SetIntraOpNumThreads((std::max)(1, threads));
            options.SetInterOpNumThreads(1);
        }
        impl->session = std::make_unique<Ort::Session>(ort_env(), model.c_str(), options);
        Ort::Session& session = *impl->session;
        impl->neg_mean = metadata_floats(session, "neg_mean");
        impl->inv_stddev = metadata_floats(session, "inv_stddev");
        impl->lang_auto = metadata_int(session, "lang_auto", impl->lang_auto);
        impl->lang_zh = metadata_int(session, "lang_zh", impl->lang_zh);
        impl->lang_en = metadata_int(session, "lang_en", impl->lang_en);
        impl->lang_yue = metadata_int(session, "lang_yue", impl->lang_yue);
        impl->with_itn = metadata_int(session, "with_itn", impl->with_itn);
        impl->without_itn = metadata_int(session, "without_itn", impl->without_itn);
        if (metadata(session, "model_type") != "sense_voice_ctc" ||
            metadata_int(session, "lfr_window_size", 0) != static_cast<int>(kStackedFrames) ||
            metadata_int(session, "lfr_window_shift", 0) != static_cast<int>(kStackedShift) ||
            impl->neg_mean.size() != kStackedDim || impl->inv_stddev.size() != kStackedDim ||
            impl->tokens.size() < static_cast<std::size_t>(metadata_int(session, "vocab_size", 1))) {
            if (error) *error = "not a SenseVoice model";
            return false;
        }
    } catch (const Ort::Exception& e) {
        if (error) *error = ort_error(e);
        return false;
    }
    impl_ = std::move(impl);
    return true;
}

bool SpeechRecognizer::transcribe(const float* samples, std::size_t count, Language language,
                                  bool itn, std::string* text, std::string* error) {
    text->clear();
    if (!loaded()) {
        if (error) *error = "model not loaded";
        return false;
    }
    std::size_t frames = 0;
    const std::vector<float> features = fbank(samples, count, &frames);
    if (frames == 0) return true;
    std::size_t stacked = 0;
    std::vector<float> x = stack_frames(features, frames, &stacked);
    for (std::size_t i = 0; i < stacked; ++i) {
        float* row = x.data() + i * kStackedDim;
        for (std::size_t d = 0; d < kStackedDim; ++d) {
            row[d] = (row[d] + impl_->neg_mean[d]) * impl_->inv_stddev[d];
        }
    }
    int32_t length = static_cast<int32_t>(stacked);
    int32_t lang = language == Language::kMandarin    ? impl_->lang_zh
                   : language == Language::kEnglish   ? impl_->lang_en
                   : language == Language::kCantonese ? impl_->lang_yue
                                                      : impl_->lang_auto;
    int32_t norm = itn ? impl_->with_itn : impl_->without_itn;
    try {
        const Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const int64_t x_shape[] = {1, static_cast<int64_t>(stacked), static_cast<int64_t>(kStackedDim)};
        const int64_t one[] = {1};
        Ort::Value inputs[] = {
            Ort::Value::CreateTensor<float>(memory, x.data(), x.size(), x_shape, 3),
            Ort::Value::CreateTensor<int32_t>(memory, &length, 1, one, 1),
            Ort::Value::CreateTensor<int32_t>(memory, &lang, 1, one, 1),
            Ort::Value::CreateTensor<int32_t>(memory, &norm, 1, one, 1),
        };
        const char* input_names[] = {"x", "x_length", "language", "text_norm"};
        const char* output_names[] = {"logits"};
        std::vector<Ort::Value> outputs =
            impl_->session->Run(Ort::RunOptions{nullptr}, input_names, inputs, 4, output_names, 1);
        const std::vector<int64_t> shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
        if (shape.size() != 3 || shape[0] != 1) {
            if (error) *error = "unexpected model output";
            return false;
        }
        const int64_t steps = shape[1];
        const int64_t vocab = shape[2];
        const float* logits = outputs[0].GetTensorData<float>();
        // CTC: the best token of each step, repeats merged, blanks (0) dropped; the tags
        // (<|zh|><|NEUTRAL|><|Speech|><|withitn|>) are not text.
        int64_t previous = -1;
        for (int64_t t = 0; t < steps; ++t) {
            const float* row = logits + t * vocab;
            const int64_t best = std::max_element(row, row + vocab) - row;
            if (best != previous && best != 0 && best < static_cast<int64_t>(impl_->tokens.size())) {
                const std::string& token = impl_->tokens[static_cast<std::size_t>(best)];
                if (token.rfind("<|", 0) != 0) text->append(token);
            }
            previous = best;
        }
    } catch (const Ort::Exception& e) {
        if (error) *error = ort_error(e);
        return false;
    }
    // SentencePiece marks a word start with U+2581.
    static const std::string kWordStart = "\xE2\x96\x81";
    for (std::size_t at = text->find(kWordStart); at != std::string::npos;
         at = text->find(kWordStart, at + 1)) {
        text->replace(at, kWordStart.size(), " ");
    }
    const std::size_t first = text->find_first_not_of(' ');
    const std::size_t last = text->find_last_not_of(' ');
    *text = first == std::string::npos ? std::string() : text->substr(first, last - first + 1);
    return true;
}

// ---- Silero VAD (v4: input, sr, h, c -> output, hn, cn) ----

namespace {
constexpr float kSpeechThreshold = 0.5f;
constexpr int kSpeakingHangover = 10;  // windows (320 ms) still counted as speaking
constexpr int kHeardWindows = 8;       // windows in a row (256 ms) that make speech
}  // namespace

struct SpeechDetector::Impl {
    std::unique_ptr<Ort::Session> session;
    std::vector<float> h = std::vector<float>(2 * 64);
    std::vector<float> c = std::vector<float>(2 * 64);
    int run = 0;          // windows in a row scoring kSpeechThreshold or more
    int since_speech = kSpeakingHangover + 1;
    bool heard = false;
};

SpeechDetector::SpeechDetector() = default;
SpeechDetector::~SpeechDetector() = default;

bool SpeechDetector::loaded() const { return impl_ && impl_->session; }

bool SpeechDetector::load(const std::wstring& model, std::string* error) {
    impl_.reset();
    if (!runtime_available()) {
        if (error) *error = "onnxruntime.dll not found";
        return false;
    }
    auto impl = std::make_unique<Impl>();
    try {
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(1);
        options.SetInterOpNumThreads(1);
        impl->session = std::make_unique<Ort::Session>(ort_env(), model.c_str(), options);
        if (impl->session->GetInputCount() != 4) {
            if (error) *error = "not a Silero VAD v4 model";
            return false;
        }
    } catch (const Ort::Exception& e) {
        if (error) *error = ort_error(e);
        return false;
    }
    impl_ = std::move(impl);
    return true;
}

void SpeechDetector::reset() {
    if (!impl_) return;
    std::fill(impl_->h.begin(), impl_->h.end(), 0.0f);
    std::fill(impl_->c.begin(), impl_->c.end(), 0.0f);
    impl_->run = 0;
    impl_->since_speech = kSpeakingHangover + 1;
    impl_->heard = false;
}

bool SpeechDetector::accept(const float* window) {
    if (!loaded()) return false;
    float probability = 0.0f;
    try {
        const Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const int64_t input_shape[] = {1, static_cast<int64_t>(kVadWindow)};
        const int64_t state_shape[] = {2, 1, 64};
        int64_t rate = kSampleRate;
        Ort::Value inputs[] = {
            Ort::Value::CreateTensor<float>(memory, const_cast<float*>(window), kVadWindow,
                                            input_shape, 2),
            Ort::Value::CreateTensor<int64_t>(memory, &rate, 1, nullptr, 0),
            Ort::Value::CreateTensor<float>(memory, impl_->h.data(), impl_->h.size(), state_shape, 3),
            Ort::Value::CreateTensor<float>(memory, impl_->c.data(), impl_->c.size(), state_shape, 3),
        };
        const char* input_names[] = {"input", "sr", "h", "c"};
        const char* output_names[] = {"output", "hn", "cn"};
        std::vector<Ort::Value> outputs =
            impl_->session->Run(Ort::RunOptions{nullptr}, input_names, inputs, 4, output_names, 3);
        probability = outputs[0].GetTensorData<float>()[0];
        std::copy_n(outputs[1].GetTensorData<float>(), impl_->h.size(), impl_->h.begin());
        std::copy_n(outputs[2].GetTensorData<float>(), impl_->c.size(), impl_->c.begin());
    } catch (const Ort::Exception&) {
        return false;
    }
    if (probability >= kSpeechThreshold) {
        impl_->since_speech = 0;
        if (++impl_->run >= kHeardWindows) impl_->heard = true;
    } else {
        impl_->run = 0;
        ++impl_->since_speech;
    }
    return impl_->since_speech <= kSpeakingHangover;
}

bool SpeechDetector::heard() const { return impl_ && impl_->heard; }

}  // namespace voice
}  // namespace cxxime
