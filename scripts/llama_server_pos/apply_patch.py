"""Zhiyi IME patch for llama.cpp b11480: llama-server returns the parts of speech of the text being
translated ("pos": true in a /completion request, the head in the model's GGUF; see server-pos.h).
Copies server-pos.h/.cpp into tools/server and edits five server files; run once on a clean
checkout, every anchor must match exactly once.

    python apply_patch.py <llama.cpp folder>
"""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def edit(path, pairs):
    s = open(path, encoding='utf-8').read()
    for a, b in pairs:
        assert s.count(a) == 1, (path, a[:70], s.count(a))
        s = s.replace(a, b)
    open(path, 'w', encoding='utf-8', newline='').write(s)


os.chdir(sys.argv[1])
for name in ('server-pos.h', 'server-pos.cpp'):
    shutil.copyfile(os.path.join(HERE, name), os.path.join('tools', 'server', name))


edit('tools/server/CMakeLists.txt', [
    ("    server-context.cpp\n    server-context.h\n",
     "    server-context.cpp\n    server-context.h\n    server-pos.cpp\n    server-pos.h\n"),
])

edit('tools/server/server-task.h', [
    ("    bool return_progress = false;\n",
     "    bool return_progress = false;\n"
     "    bool pos             = false; // Zhiyi: the part-of-speech tag of the prompt's text (server-pos.h)\n"),
    ("    std::vector<std::string>  response_fields;\n",
     "    std::vector<std::string>  response_fields;\n\n"
     "    // Zhiyi: the likeliest part-of-speech tags with their probabilities, most likely first;\n"
     "    // empty when none was asked or the model has no head\n"
     "    std::vector<std::pair<std::string, float>> pos;\n"),
])

edit('tools/server/server-schema.cpp', [
    ("""    add((new field_bool("return_progress", params.return_progress))""",
     """    add((new field_bool("pos", params.pos))
        ->set_desc("Zhiyi: return the part-of-speech tag of the prompt's text (needs a model with a part-of-speech head)"));

    add((new field_bool("return_progress", params.return_progress))"""),
])

edit('tools/server/server-task.cpp', [
    ("""    if (!stream && !probs_output.empty()) {
        res["completion_probabilities"] = completion_token_output::probs_vector_to_json(probs_output, post_sampling_probs);
    }
    return response_fields.empty() ? res : json_get_nested_values(response_fields, res);
}

json server_task_result_cmpl_final::usage_json_oaicompat() {""",
     """    if (!stream && !probs_output.empty()) {
        res["completion_probabilities"] = completion_token_output::probs_vector_to_json(probs_output, post_sampling_probs);
    }
    if (!pos.empty()) {
        res["pos"]      = pos.front().first;
        res["pos_prob"] = pos.front().second;
        json tags = json::array();
        for (const auto & [tag, prob] : pos) {
            tags.push_back({ { "tag", tag }, { "prob", prob } });
        }
        res["pos_top"] = tags;
    }
    return response_fields.empty() ? res : json_get_nested_values(response_fields, res);
}

json server_task_result_cmpl_final::usage_json_oaicompat() {"""),
])

edit('tools/server/server-context.cpp', [
    ('#include "server-decision.h"\n',
     '#include "server-decision.h"\n#include "server-pos.h"\n'),
    # slot fields and their reset
    ("    std::string stopping_word;\n",
     "    std::string stopping_word;\n\n"
     "    // Zhiyi: the part-of-speech tags of the prompt's text, most likely first (server-pos.h)\n"
     "    std::vector<std::pair<std::string, float>> pos;\n"),
    ('        stopping_word  = "";\n',
     '        stopping_word  = "";\n        pos.clear();\n'),
    # server members
    ("    common_params params_base;\n",
     "    common_params params_base;\n\n"
     "    server_pos_head    pos_head;     // Zhiyi: the model's part-of-speech head, if it has one\n"
     "    server_pos_capture pos_capture;\n"),
    # load the head before the context is made
    ("""        llama_init = common_init_from_params(params_base);

        model_tgt = llama_init->model();""",
     """        // Zhiyi: with a part-of-speech head in the model file, the hidden states of a sequence's
        // last prompt token are read while the prompt is decoded (eval callback). The callback sees
        // ubatch rows: one ubatch in the batch's own order (a unified KV cache splits simply) keeps
        // them the batch positions.
        if (pos_head.load(params_base.model.path)) {
            params_base.kv_unified        = true;
            params_base.n_ubatch          = params_base.n_batch;
            pos_capture.head              = &pos_head;
            params_base.cb_eval           = server_pos_capture::eval_cb;
            params_base.cb_eval_user_data = &pos_capture;
            SRV_INF("part-of-speech head: %zu layers, %zu tags\\n", pos_head.layers.size(), pos_head.tags.size());
        }

        llama_init = common_init_from_params(params_base);

        model_tgt = llama_init->model();"""),
    # capture around each decode
    ("""                batch.render(off, n_tokens);
                bool ok = decode(n_batch, off);""",
     """                batch.render(off, n_tokens);

                // Zhiyi: the slots asking for a part-of-speech tag whose prompt ends in this view
                std::vector<server_slot *> pos_slots;
                if (pos_head.loaded()) {
                    std::vector<int32_t> rows;
                    for (auto & slot : slots) {
                        if (slot.state == SLOT_STATE_DONE_PROMPT && slot.task && slot.task->params.pos &&
                            slot.i_batch >= off && slot.i_batch < off + n_tokens) {
                            rows.push_back(slot.i_batch - off);
                            pos_slots.push_back(&slot);
                        }
                    }
                    pos_capture.begin(std::move(rows));
                }

                bool ok = decode(n_batch, off);

                if (ok && pos_capture.complete()) {
                    const size_t n_states = pos_head.layers.size() * pos_head.n_embd;
                    for (size_t i = 0; i < pos_slots.size(); ++i) {
                        pos_slots[i]->pos = pos_head.apply(pos_capture.states.data() + i * n_states);
                    }
                }
                if (pos_head.loaded()) {
                    pos_capture.begin({});
                }"""),
    # final response
    ("""        res->stop                  = slot.stop;
        res->post_sampling_probs   = slot.task->params.post_sampling_probs;

        res->verbose           = slot.task->params.verbose;
        res->stream            = slot.task->params.stream;
        res->include_usage     = slot.task->params.include_usage;""",
     """        res->stop                  = slot.stop;
        res->post_sampling_probs   = slot.task->params.post_sampling_probs;
        res->pos                   = slot.pos;

        res->verbose           = slot.task->params.verbose;
        res->stream            = slot.task->params.stream;
        res->include_usage     = slot.task->params.include_usage;"""),
])
print('patched', sys.argv[1])
