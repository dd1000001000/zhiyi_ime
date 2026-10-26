// Zhiyi IME patch for llama.cpp (scripts/llama_server_pos). MIT License, as llama.cpp.

#include "server-pos.h"

#include "ggml-backend.h"
#include "gguf.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

bool read_f32(const gguf_context * ctx, const char * key, std::vector<float> & out) {
    const int64_t id = gguf_find_key(ctx, key);
    if (id < 0 || gguf_get_kv_type(ctx, id) != GGUF_TYPE_ARRAY || gguf_get_arr_type(ctx, id) != GGUF_TYPE_FLOAT32) {
        return false;
    }
    const size_t n = gguf_get_arr_n(ctx, id);
    const float * data = static_cast<const float *>(gguf_get_arr_data(ctx, id));
    out.assign(data, data + n);
    return true;
}

} // namespace

bool server_pos_head::load(const std::string & gguf_path) {
    *this = {};
    gguf_init_params params = { /* no_alloc = */ true, /* ctx = */ nullptr };
    gguf_context * ctx = gguf_init_from_file(gguf_path.c_str(), params);
    if (!ctx) {
        return false;
    }
    bool ok = true;
    const int64_t layers_id = gguf_find_key(ctx, "zhiyi.pos_head.layers");
    const int64_t tags_id   = gguf_find_key(ctx, "zhiyi.pos_head.tags");
    if (layers_id < 0 || tags_id < 0 ||
        gguf_get_kv_type(ctx, layers_id) != GGUF_TYPE_ARRAY || gguf_get_arr_type(ctx, layers_id) != GGUF_TYPE_INT32 ||
        gguf_get_kv_type(ctx, tags_id) != GGUF_TYPE_ARRAY || gguf_get_arr_type(ctx, tags_id) != GGUF_TYPE_STRING) {
        ok = false;
    } else {
        const int32_t * l = static_cast<const int32_t *>(gguf_get_arr_data(ctx, layers_id));
        layers.assign(l, l + gguf_get_arr_n(ctx, layers_id));
        for (size_t i = 0; i < gguf_get_arr_n(ctx, tags_id); ++i) {
            tags.emplace_back(gguf_get_arr_str(ctx, tags_id, i));
        }
        ok = read_f32(ctx, "zhiyi.pos_head.mean", mean) && read_f32(ctx, "zhiyi.pos_head.std", stdev) &&
             read_f32(ctx, "zhiyi.pos_head.fc1.weight", w1) && read_f32(ctx, "zhiyi.pos_head.fc1.bias", b1) &&
             read_f32(ctx, "zhiyi.pos_head.fc2.weight", w2) && read_f32(ctx, "zhiyi.pos_head.fc2.bias", b2);
    }
    gguf_free(ctx);

    const size_t n_layers = layers.size();
    if (ok && n_layers > 0 && !tags.empty() && mean.size() % n_layers == 0) {
        n_embd   = static_cast<int32_t>(mean.size() / n_layers);
        n_hidden = static_cast<int32_t>(b1.size());
        ok = stdev.size() == mean.size() && w1.size() == b1.size() * mean.size() &&
             b2.size() == tags.size() && w2.size() == b2.size() * b1.size();
    } else {
        ok = false;
    }
    if (!ok) {
        *this = {};
    }
    return ok;
}

std::vector<std::pair<std::string, float>> server_pos_head::apply(const float * states) const {
    const size_t n_in = mean.size();
    std::vector<float> z(n_in);
    for (size_t i = 0; i < n_in; ++i) {
        z[i] = (states[i] - mean[i]) / stdev[i];
    }
    std::vector<float> h(n_hidden);
    for (int32_t j = 0; j < n_hidden; ++j) {
        const float * w = w1.data() + static_cast<size_t>(j) * n_in;
        double sum = b1[j];
        for (size_t i = 0; i < n_in; ++i) {
            sum += static_cast<double>(w[i]) * z[i];
        }
        h[j] = static_cast<float>(0.5 * sum * (1.0 + std::erf(sum / std::sqrt(2.0))));  // exact GELU
    }
    std::vector<double> logits(tags.size());
    for (size_t k = 0; k < tags.size(); ++k) {
        const float * w = w2.data() + k * n_hidden;
        double sum = b2[k];
        for (int32_t j = 0; j < n_hidden; ++j) {
            sum += static_cast<double>(w[j]) * h[j];
        }
        logits[k] = sum;
    }
    const double top = *std::max_element(logits.begin(), logits.end());
    double total = 0.0;
    for (double & l : logits) {
        l = std::exp(l - top);
        total += l;
    }
    std::vector<std::pair<std::string, float>> out;
    for (size_t k = 0; k < tags.size(); ++k) {
        const float prob = static_cast<float>(logits[k] / total);
        if (prob >= 0.01f) {
            out.emplace_back(tags[k], prob);
        }
    }
    std::sort(out.begin(), out.end(), [](const auto & a, const auto & b) { return a.second > b.second; });
    if (out.size() > 5) {
        out.resize(5);
    }
    return out;
}

void server_pos_capture::begin(std::vector<int32_t> wanted) {
    rows = std::move(wanted);
    const size_t n_layers = head ? head->layers.size() : 0;
    states.assign(rows.size() * n_layers * (head ? head->n_embd : 0), 0.0f);
    taken.assign(n_layers, false);
}

bool server_pos_capture::complete() const {
    return !rows.empty() && std::all_of(taken.begin(), taken.end(), [](bool b) { return b; });
}

bool server_pos_capture::eval_cb(struct ggml_tensor * t, bool ask, void * user_data) {
    auto * capture = static_cast<server_pos_capture *>(user_data);
    if (!capture || !capture->head || capture->rows.empty()) {
        return !ask;  // nothing wanted: no tensor is read back, the computation goes on
    }
    const server_pos_head & head = *capture->head;
    int32_t k = -1;
    for (size_t i = 0; i < head.layers.size(); ++i) {
        char name[GGML_MAX_NAME];
        snprintf(name, sizeof(name), "l_out-%d", head.layers[i]);
        if (strcmp(t->name, name) == 0) {
            k = static_cast<int32_t>(i);
            break;
        }
    }
    if (k < 0) {
        return !ask;
    }
    if (ask) {
        return true;
    }
    if (t->type != GGML_TYPE_F32 || t->ne[0] != head.n_embd) {
        return true;  // not the expected layout: left incomplete, no tag
    }
    const size_t row_bytes = static_cast<size_t>(head.n_embd) * sizeof(float);
    for (size_t r = 0; r < capture->rows.size(); ++r) {
        const int32_t row = capture->rows[r];
        if (row < 0 || row >= t->ne[1]) {
            return true;
        }
        float * dst = capture->states.data() + (r * head.layers.size() + k) * head.n_embd;
        ggml_backend_tensor_get(t, dst, static_cast<size_t>(row) * t->nb[1], row_bytes);
    }
    capture->taken[k] = true;
    return true;
}
