// Zhiyi IME patch for llama.cpp (scripts/llama_server_pos). MIT License, as llama.cpp.

#pragma once

// Zhiyi IME: the part-of-speech head of its translation model (zhiyi-mt-v4-1026 and later).
// The head is stored in the model's GGUF as metadata (zhiyi.pos_head.*). It reads the hidden
// states after a few layers at a sequence's last prompt token, taken with an eval callback while
// the prompt is decoded, and tags the text being translated (noun, verb, ...).

#include "ggml.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct server_pos_head {
    std::vector<int32_t>     layers;  // 0-based layer numbers, read from the graph tensors l_out-N
    std::vector<std::string> tags;    // one per output, in order (Universal Dependencies names)
    int32_t n_embd   = 0;
    int32_t n_hidden = 0;
    std::vector<float> mean, stdev;   // [layers][n_embd]
    std::vector<float> w1, b1;        // [n_hidden][layers * n_embd], [n_hidden]
    std::vector<float> w2, b2;        // [tags][n_hidden], [tags]

    // false when the file has no head (the server then runs as it always did)
    bool load(const std::string & gguf_path);
    bool loaded() const { return !tags.empty(); }

    // states: layers.size() * n_embd floats, the layers in order -> the tags at least 1% likely
    // with their probabilities, most likely first, at most 5
    std::vector<std::pair<std::string, float>> apply(const float * states) const;
};

// The rows of the decoded batch whose hidden states are wanted: the eval callback's user data.
// Rows are batch positions, so the batch must be decoded as one ubatch in its own order.
struct server_pos_capture {
    const server_pos_head * head = nullptr;
    std::vector<int32_t> rows;
    std::vector<float>   states;  // [rows][layers][n_embd]
    std::vector<bool>    taken;   // per layer

    void begin(std::vector<int32_t> wanted);
    bool complete() const;

    static bool eval_cb(struct ggml_tensor * t, bool ask, void * user_data);
};
