// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
#include <cxxime/qwen_asr.h>

#include <windows.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <json.hpp>

#include "voice_ort.h"

namespace cxxime {
namespace voice {

namespace {

constexpr std::size_t kFft = 400;
constexpr std::size_t kHop = 160;
constexpr std::size_t kBins = kFft / 2 + 1;  // 201
constexpr std::size_t kMel = 128;
constexpr std::size_t kMinSamples = 8000;    // shorter clips are padded to 0.5 s
constexpr std::size_t kPadSamples = 480000;  // and every clip to 30 s
constexpr std::size_t kChunk = 100;          // mel frames per encoder chunk
constexpr std::size_t kHidden = 1024;
constexpr int kLayers = 28;
constexpr int64_t kKvHeads = 8;
constexpr int64_t kHeadDim = 128;
// The official streaming defaults: no previous text for the first 2 calls, then the previous
// text without its last 5 tokens.
constexpr int kUnfixedChunks = 2;
constexpr std::size_t kUnfixedTokens = 5;
constexpr double kPi = 3.14159265358979323846;

struct Dft {
    std::vector<float> cos, sin, window;  // kBins x kFft, kFft
};

const Dft& dft() {
    static const Dft table = [] {
        Dft d;
        d.cos.resize(kBins * kFft);
        d.sin.resize(kBins * kFft);
        d.window.resize(kFft);
        for (std::size_t k = 0; k < kFft; ++k) d.window[k] = static_cast<float>(0.5 - 0.5 * std::cos(2 * kPi * k / kFft));
        for (std::size_t b = 0; b < kBins; ++b) {
            for (std::size_t k = 0; k < kFft; ++k) {
                const double angle = 2 * kPi * static_cast<double>((b * k) % kFft) / kFft;
                d.cos[b * kFft + k] = static_cast<float>(std::cos(angle));
                d.sin[b * kFft + k] = static_cast<float>(-std::sin(angle));
            }
        }
        return d;
    }();
    return table;
}

// The frames' log10 mel energies before flooring, kMel each; rows [0, cached) are kept between
// calls on a growing recording (only frames whose 400 samples were all recorded).
struct MelCache {
    std::vector<float> logs;
    std::size_t frames = 0;
};

// The processor's features (make_runtime.py mel() is the same in numpy).
std::vector<float> log_mel(const float* samples, std::size_t count, const std::vector<float>& filters,
                           MelCache* cache, std::size_t* mel_len_out, std::size_t* frames_out) {
    std::vector<float> x(samples, samples + count);
    if (x.size() < kMinSamples) x.resize(kMinSamples, 0.0f);
    const std::size_t n = x.size();
    const std::size_t length = (std::max)(n, kPadSamples);  // zeros after the recording up to 30 s
    const std::size_t total = length / kHop;                 // STFT frames 1 + length / 160, the last dropped
    const std::size_t mel_len = (std::min)(total, (n + kHop - 1) / kHop);
    const std::size_t chunks = (mel_len + kChunk - 1) / kChunk;
    const std::size_t frames = chunks * kChunk;
    const std::size_t need = (std::min)(total, frames + 3);  // past `frames` only padding
    auto sample = [&](std::ptrdiff_t i) -> float {
        if (i < 0) i = -i;  // reflected at the start
        if (static_cast<std::size_t>(i) < n) return x[static_cast<std::size_t>(i)];
        if (length == n) {  // no zero padding: reflected at the end too
            const std::ptrdiff_t j = 2 * static_cast<std::ptrdiff_t>(n - 1) - i;
            return j >= 0 ? x[static_cast<std::size_t>(j)] : 0.0f;
        }
        return 0.0f;
    };
    const Dft& d = dft();
    std::vector<float> logs(need * kMel);
    std::size_t from = 0;
    // Frames t with t * 160 + 200 < count lie wholly in what was recorded (and the start's
    // reflection): they stay as they are while the recording grows.
    const std::size_t stable = count >= kMinSamples && count > kFft / 2 ? (count - kFft / 2 - 1) / kHop + 1 : 0;
    if (cache) {
        from = (std::min)({cache->frames, need, stable});
        std::copy_n(cache->logs.begin(), from * kMel, logs.begin());
    }
    std::vector<float> frame(kFft), power(kBins);
    for (std::size_t t = from; t < need; ++t) {
        const std::ptrdiff_t start = static_cast<std::ptrdiff_t>(t * kHop) - static_cast<std::ptrdiff_t>(kFft / 2);
        for (std::size_t k = 0; k < kFft; ++k) frame[k] = sample(start + static_cast<std::ptrdiff_t>(k)) * d.window[k];
        for (std::size_t b = 0; b < kBins; ++b) {
            const float* c = d.cos.data() + b * kFft;
            const float* s = d.sin.data() + b * kFft;
            float re = 0.0f, im = 0.0f;
            for (std::size_t k = 0; k < kFft; ++k) {
                re += frame[k] * c[k];
                im += frame[k] * s[k];
            }
            power[b] = re * re + im * im;
        }
        float* row = logs.data() + t * kMel;
        for (std::size_t m = 0; m < kMel; ++m) {
            const float* f = filters.data() + m * kBins;
            double energy = 0.0;
            for (std::size_t b = 0; b < kBins; ++b) energy += static_cast<double>(f[b]) * power[b];
            row[m] = static_cast<float>(std::log10((std::max)(energy, 1e-10)));
        }
    }
    if (cache) {
        const std::size_t keep = (std::min)(need, stable);
        if (keep > cache->frames) {
            cache->logs.assign(logs.begin(), logs.begin() + keep * kMel);
            cache->frames = keep;
        }
    }
    float top = -FLT_MAX;
    for (float v : logs) top = (std::max)(top, v);
    std::vector<float> out(kMel * frames, 0.0f);  // frames past the spectrogram stay 0
    const std::size_t shown = (std::min)(need, frames);
    for (std::size_t t = 0; t < shown; ++t) {
        for (std::size_t m = 0; m < kMel; ++m) {
            out[m * frames + t] = ((std::max)(logs[t * kMel + m], top - 8.0f) + 4.0f) / 4.0f;
        }
    }
    *mel_len_out = mel_len;
    if (frames_out) *frames_out = frames;
    return out;
}

// Whether `bytes` ends at a character boundary (no UTF-8 sequence cut short).
bool whole_utf8(const std::string& bytes) {
    std::size_t i = bytes.size();
    std::size_t continuation = 0;
    while (i > 0 && (static_cast<unsigned char>(bytes[i - 1]) & 0xC0) == 0x80 && continuation < 4) {
        --i;
        ++continuation;
    }
    if (i == 0) return continuation == 0;
    const unsigned char lead = static_cast<unsigned char>(bytes[i - 1]);
    const std::size_t want = lead < 0x80 ? 0 : (lead >> 5) == 0x6 ? 1 : (lead >> 4) == 0xE ? 2 : (lead >> 3) == 0x1E ? 3 : 0;
    return continuation == want;
}

// The official repetition fix (qwen_asr inference utils fix_char_repeats): a character repeated
// more than 20 times in a row is kept once.
std::string fix_repeats(const std::string& text) {
    std::vector<std::string> chars;
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const std::size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
        chars.push_back(text.substr(i, len));
        i += len;
    }
    std::string out;
    for (std::size_t i = 0; i < chars.size();) {
        std::size_t run = 1;
        while (i + run < chars.size() && chars[i + run] == chars[i]) ++run;
        if (run > 20) {
            out += chars[i];
        } else {
            for (std::size_t k = 0; k < run; ++k) out += chars[i];
        }
        i += run;
    }
    return out;
}

std::string trim(const std::string& text) {
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

}  // namespace

std::vector<float> qwen_log_mel(const float* samples, std::size_t count, const std::vector<float>& filters,
                                std::size_t* mel_len, std::size_t* frames) {
    return log_mel(samples, count, filters, nullptr, mel_len, frames);
}

struct QwenRecognizer::Impl {
    std::unique_ptr<Ort::Session> encoder;
    std::unique_ptr<Ort::Session> decoder;
    std::vector<std::string> decoder_inputs, decoder_outputs;
    std::vector<float> filters;               // kMel x kBins
    std::vector<std::string> token_bytes;     // by id
    std::vector<uint8_t> token_kind;          // 0 text, 1 special, 2 <asr_text>
    std::vector<int64_t> prefix, suffix;      // the prompt around the audio placeholders
    int64_t audio_pad = 151676;
    std::vector<int64_t> eos{151643, 151645};
    std::vector<int64_t> lang_zh, lang_en, lang_yue;

    // Text while speaking.
    Language language = Language::kAuto;
    int calls = 0;
    std::vector<int64_t> raw;  // what the model wrote last (language header included when auto)
    MelCache cache;

    const std::vector<int64_t>* forced(Language l) const {
        switch (l) {
        case Language::kMandarin:
            return &lang_zh;
        case Language::kEnglish:
            return &lang_en;
        case Language::kCantonese:
            return &lang_yue;
        default:
            return nullptr;
        }
    }

    std::string bytes_of(const std::vector<int64_t>& ids, std::size_t end) const {
        std::string out;
        for (std::size_t i = 0; i < end && i < ids.size(); ++i) {
            const int64_t id = ids[i];
            if (id >= 0 && static_cast<std::size_t>(id) < token_bytes.size() && token_kind[id] != 1) {
                out += token_bytes[id];
            }
        }
        return out;
    }

    // "language Chinese<asr_text>text" -> "text"; "language None<asr_text>" -> "".
    std::string text_of(const std::vector<int64_t>& ids, bool language_given) const {
        const std::string all = bytes_of(ids, ids.size());
        std::string text;
        const std::size_t marker = all.find("<asr_text>");
        if (marker != std::string::npos) {
            std::string header = trim(all.substr(0, marker));
            std::transform(header.begin(), header.end(), header.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (header == "language none") return {};
            text = all.substr(marker + std::string("<asr_text>").size());
        } else if (language_given) {
            text = all;
        } else {
            // Only the header so far (or nothing recognizable).
            if (trim(all).rfind("language", 0) == 0) return {};
            text = all;
        }
        return trim(fix_repeats(text));
    }

    bool encode(const float* samples, std::size_t count, MelCache* mel_cache, std::vector<float>* audio,
                int64_t* rows, std::string* error);
    bool decode(const std::vector<float>& audio, int64_t rows, const std::vector<int64_t>& answer_start,
                const std::vector<int64_t>* language_suffix, std::size_t max_new, std::vector<int64_t>* out,
                std::string* error);
};

QwenRecognizer::QwenRecognizer() = default;
QwenRecognizer::~QwenRecognizer() = default;

bool QwenRecognizer::loaded() const { return impl_ && impl_->encoder && impl_->decoder; }

bool QwenRecognizer::load(const QwenFiles& files, int threads, std::string* error) {
    impl_.reset();
    if (!runtime_available()) {
        if (error) *error = "onnxruntime.dll not found";
        return false;
    }
    auto impl = std::make_unique<Impl>();
    // Tokens.
    {
        std::ifstream in{std::filesystem::path(files.tokens)};
        if (!in) {
            if (error) *error = "cannot read the tokens";
            return false;
        }
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const std::size_t tab = line.find('\t');
            if (tab == std::string::npos || tab % 2 != 0) {
                if (error) *error = "bad tokens line";
                return false;
            }
            auto hex = [](char c) {
                return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c - 'A' + 10;
            };
            std::string bytes;
            for (std::size_t i = 0; i < tab; i += 2) {
                bytes.push_back(static_cast<char>(hex(line[i]) * 16 + hex(line[i + 1])));
            }
            impl->token_bytes.push_back(std::move(bytes));
            impl->token_kind.push_back(static_cast<uint8_t>(line.size() > tab + 1 ? line[tab + 1] - '0' : 1));
        }
        if (impl->token_bytes.size() < 151936) {
            if (error) *error = "too few tokens";
            return false;
        }
    }
    // Mel filters.
    {
        std::ifstream in{std::filesystem::path(files.mel), std::ios::binary};
        impl->filters.resize(kMel * kBins);
        if (!in.read(reinterpret_cast<char*>(impl->filters.data()),
                     static_cast<std::streamsize>(impl->filters.size() * sizeof(float)))) {
            if (error) *error = "cannot read the mel filters";
            return false;
        }
    }
    // The prompt.
    {
        std::ifstream in{std::filesystem::path(files.config)};
        const nlohmann::json config = nlohmann::json::parse(in, nullptr, false);
        try {
            impl->prefix = config.at("prefix").get<std::vector<int64_t>>();
            impl->suffix = config.at("suffix").get<std::vector<int64_t>>();
            impl->audio_pad = config.at("audio_pad").get<int64_t>();
            impl->eos = config.at("eos").get<std::vector<int64_t>>();
            const nlohmann::json& languages = config.at("languages");
            impl->lang_zh = languages.at("zh").get<std::vector<int64_t>>();
            impl->lang_en = languages.at("en").get<std::vector<int64_t>>();
            impl->lang_yue = languages.at("yue").get<std::vector<int64_t>>();
        } catch (...) {
            if (error) *error = "bad qwen3-asr.json";
            return false;
        }
    }
    try {
        Ort::SessionOptions options;
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        options.SetIntraOpNumThreads((std::max)(1, threads));
        options.SetInterOpNumThreads(1);
        impl->encoder = std::make_unique<Ort::Session>(detail::voice_ort_env(), files.encoder.c_str(), options);
        impl->decoder = std::make_unique<Ort::Session>(detail::voice_ort_env(), files.decoder.c_str(), options);
        Ort::AllocatorWithDefaultOptions allocator;
        for (std::size_t i = 0; i < impl->decoder->GetInputCount(); ++i) {
            impl->decoder_inputs.push_back(impl->decoder->GetInputNameAllocated(i, allocator).get());
        }
        for (std::size_t i = 0; i < impl->decoder->GetOutputCount(); ++i) {
            impl->decoder_outputs.push_back(impl->decoder->GetOutputNameAllocated(i, allocator).get());
        }
        if (impl->decoder_inputs.size() != 2 + 2 * kLayers || impl->decoder_outputs.size() != 1 + 2 * kLayers ||
            impl->decoder_outputs[0] != "logits") {
            if (error) *error = "not a Qwen3-ASR decoder";
            return false;
        }
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("onnxruntime: ") + e.what();
        return false;
    }
    impl_ = std::move(impl);
    return true;
}

bool QwenRecognizer::Impl::encode(const float* samples, std::size_t count, MelCache* mel_cache,
                                  std::vector<float>* audio, int64_t* rows, std::string* error) {
    std::size_t mel_len = 0, frames = 0;
    std::vector<float> mel = log_mel(samples, count, filters, mel_cache, &mel_len, &frames);
    const Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    const int64_t mel_shape[] = {1, static_cast<int64_t>(kMel), static_cast<int64_t>(frames)};
    int64_t length = static_cast<int64_t>(mel_len);
    const int64_t one[] = {1};
    Ort::Value inputs[] = {
        Ort::Value::CreateTensor<float>(memory, mel.data(), mel.size(), mel_shape, 3),
        Ort::Value::CreateTensor<int64_t>(memory, &length, 1, one, 1),
    };
    const char* input_names[] = {"mel", "mel_len"};
    const char* output_names[] = {"audio"};
    try {
        std::vector<Ort::Value> out = encoder->Run(Ort::RunOptions{nullptr}, input_names, inputs, 2, output_names, 1);
        const std::vector<int64_t> shape = out[0].GetTensorTypeAndShapeInfo().GetShape();
        if (shape.size() != 2 || shape[1] != static_cast<int64_t>(kHidden) || shape[0] <= 0) {
            if (error) *error = "unexpected encoder output";
            return false;
        }
        const float* data = out[0].GetTensorData<float>();
        audio->assign(data, data + shape[0] * shape[1]);
        *rows = shape[0];
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("onnxruntime: ") + e.what();
        return false;
    }
    return true;
}

bool QwenRecognizer::Impl::decode(const std::vector<float>& audio, int64_t rows,
                                  const std::vector<int64_t>& answer_start,
                                  const std::vector<int64_t>* language_suffix, std::size_t max_new,
                                  std::vector<int64_t>* out, std::string* error) {
    out->clear();
    std::vector<int64_t> ids = prefix;
    ids.insert(ids.end(), static_cast<std::size_t>(rows), audio_pad);
    ids.insert(ids.end(), suffix.begin(), suffix.end());
    if (language_suffix) ids.insert(ids.end(), language_suffix->begin(), language_suffix->end());
    ids.insert(ids.end(), answer_start.begin(), answer_start.end());

    const Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::vector<const char*> input_names, output_names;
    for (const std::string& n : decoder_inputs) input_names.push_back(n.c_str());
    for (const std::string& n : decoder_outputs) output_names.push_back(n.c_str());
    // Where each past_* input takes its value from in the previous outputs (present_*).
    std::vector<int> from_output(decoder_inputs.size(), -1);
    for (std::size_t i = 0; i < decoder_inputs.size(); ++i) {
        const std::string& name = decoder_inputs[i];
        if (name.rfind("past_", 0) != 0) continue;
        const std::string present = "present_" + name.substr(5);
        for (std::size_t o = 0; o < decoder_outputs.size(); ++o) {
            if (decoder_outputs[o] == present) from_output[i] = static_cast<int>(o);
        }
        if (from_output[i] < 0) {
            if (error) *error = "decoder input without output: " + name;
            return false;
        }
    }
    float empty = 0.0f;
    std::vector<float> no_audio(kHidden, 0.0f);
    std::vector<Ort::Value> previous;
    int64_t token = 0;
    try {
        for (std::size_t step = 0;; ++step) {
            std::vector<Ort::Value> inputs;
            const int64_t ids_shape[] = {1, step == 0 ? static_cast<int64_t>(ids.size()) : 1};
            const int64_t audio_shape[] = {step == 0 ? rows : 1, static_cast<int64_t>(kHidden)};
            const int64_t past_shape[] = {1, kKvHeads, 0, kHeadDim};
            for (std::size_t i = 0; i < decoder_inputs.size(); ++i) {
                const std::string& name = decoder_inputs[i];
                if (name == "input_ids") {
                    inputs.push_back(Ort::Value::CreateTensor<int64_t>(memory, step == 0 ? ids.data() : &token,
                                                                       step == 0 ? ids.size() : 1, ids_shape, 2));
                } else if (name == "audio") {
                    float* data = step == 0 ? const_cast<float*>(audio.data()) : no_audio.data();
                    const std::size_t size = step == 0 ? audio.size() : no_audio.size();
                    inputs.push_back(Ort::Value::CreateTensor<float>(memory, data, size, audio_shape, 2));
                } else if (step == 0) {
                    inputs.push_back(Ort::Value::CreateTensor<float>(memory, &empty, 0, past_shape, 4));
                } else {
                    inputs.push_back(std::move(previous[static_cast<std::size_t>(from_output[i])]));
                }
            }
            previous = decoder->Run(Ort::RunOptions{nullptr}, input_names.data(), inputs.data(), inputs.size(),
                                    output_names.data(), output_names.size());
            const float* logits = previous[0].GetTensorData<float>();
            const std::size_t vocab = static_cast<std::size_t>(previous[0].GetTensorTypeAndShapeInfo().GetElementCount());
            token = static_cast<int64_t>(std::max_element(logits, logits + vocab) - logits);
            if (std::find(eos.begin(), eos.end(), token) != eos.end()) break;
            out->push_back(token);
            if (out->size() >= max_new) break;
        }
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("onnxruntime: ") + e.what();
        return false;
    }
    return true;
}

bool QwenRecognizer::transcribe(const float* samples, std::size_t count, Language language, std::string* text,
                                std::string* error) {
    text->clear();
    if (!loaded()) {
        if (error) *error = "model not loaded";
        return false;
    }
    if (count == 0) return true;
    std::vector<float> audio;
    int64_t rows = 0;
    if (!impl_->encode(samples, count, nullptr, &audio, &rows, error)) return false;
    const std::size_t max_new = 16 + 8 * (count / kSampleRate + 1);
    std::vector<int64_t> out;
    const std::vector<int64_t>* forced = impl_->forced(language);
    if (!impl_->decode(audio, rows, {}, forced, (std::min<std::size_t>)(max_new, 512), &out, error)) return false;
    *text = impl_->text_of(out, forced != nullptr);
    return true;
}

void QwenRecognizer::start(Language language) {
    if (!impl_) return;
    impl_->language = language;
    impl_->calls = 0;
    impl_->raw.clear();
    impl_->cache = MelCache();
}

bool QwenRecognizer::partial(const float* samples, std::size_t count, std::string* text, std::string* error) {
    text->clear();
    if (!loaded()) {
        if (error) *error = "model not loaded";
        return false;
    }
    if (count == 0) return true;
    Impl& m = *impl_;
    // The previous text without its last tokens (more if they would cut a character).
    std::vector<int64_t> start;
    if (m.calls >= kUnfixedChunks && !m.raw.empty()) {
        std::size_t back = kUnfixedTokens;
        for (;;) {
            const std::size_t end = m.raw.size() > back ? m.raw.size() - back : 0;
            if (end == 0) break;
            if (whole_utf8(m.bytes_of(m.raw, end))) {
                start.assign(m.raw.begin(), m.raw.begin() + static_cast<std::ptrdiff_t>(end));
                break;
            }
            ++back;
        }
    }
    std::vector<float> audio;
    int64_t rows = 0;
    if (!m.encode(samples, count, &m.cache, &audio, &rows, error)) return false;
    const std::size_t max_new = 16 + 8 * (count / kSampleRate + 1);
    std::vector<int64_t> out;
    const std::vector<int64_t>* forced = m.forced(m.language);
    if (!m.decode(audio, rows, start, forced, (std::min<std::size_t>)(max_new, 512), &out, error)) return false;
    m.raw = start;
    m.raw.insert(m.raw.end(), out.begin(), out.end());
    ++m.calls;
    *text = m.text_of(m.raw, forced != nullptr);
    return true;
}

}  // namespace voice
}  // namespace cxxime
