// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// End-to-end check of Laya reranking inside the real Engine (project dictionary + model):
// types a sentence word by word in two engines (reranking off / on), selects the target word
// in both so they share the same context, and compares first-candidate hits and key latency.
//
//   laya_smoke <model_dir>      (defaults to <project>/models/laya)
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <cxxime/engine.h>
#include <cxxime/english_learning.h>
#include <cxxime/key_event.h>
#include <cxxime/laya_rerank.h>

namespace {

std::string project_path(const char* rel) { return std::string(CXXIME_PROJECT_DIR) + rel; }

std::string json_escape_path(std::string p) {
    std::replace(p.begin(), p.end(), '\\', '/');
    return p;
}

std::string write_config(const std::string& name, bool enable, const std::string& model_dir) {
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    std::string path = std::string(tmp) + name;
    std::ofstream f(path, std::ios::binary);
    f << "{\"engine\": {\"page_size\": 7, \"candidate_learning\": false},\n"
      << " \"laya\": {\"enable\": " << (enable ? "true" : "false")
      << ", \"model_dir\": \"" << json_escape_path(model_dir) << "\"}}\n";
    return path;
}

bool type(cxxime::Engine& engine, const std::string& code, double* last_key_ms) {
    for (size_t i = 0; i < code.size(); ++i) {
        cxxime::KeyEvent ev;
        ev.keycode = static_cast<uint32_t>(toupper(static_cast<unsigned char>(code[i])));
        ev.is_key_up = false;
        auto t0 = std::chrono::steady_clock::now();
        auto r = engine.process_key(ev);
        if (i + 1 == code.size() && last_key_ms)
            *last_key_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (r != cxxime::ProcessResult::ACCEPTED) return false;
    }
    return true;
}

std::vector<std::string> page(cxxime::Engine& engine) {
    std::vector<std::string> out;
    for (const auto& c : engine.context().candidate_page().candidates) out.push_back(c.text);
    return out;
}

// Candidate texts with the Laya recommendation marked as "*".
std::vector<std::string> marked_page(cxxime::Engine& engine) {
    std::vector<std::string> out;
    for (const auto& c : engine.context().candidate_page().candidates)
        out.push_back(c.recommended ? c.text + "*" : c.text);
    return out;
}

std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (size_t i = 0; i < v.size() && i < 7; ++i) s += (i ? " " : "") + std::to_string(i + 1) + "." + v[i];
    return s;
}

// Selects `gold` (or the first candidate) and commits it, so it becomes context.
void choose(cxxime::Engine& engine, const std::string& gold) {
    auto p = page(engine);
    int pick = 0;
    for (size_t i = 0; i < p.size(); ++i)
        if (p[i] == gold) pick = static_cast<int>(i);
    engine.select_candidate(pick);
    engine.take_commit_text_with_source();
    engine.clear();
}

}  // namespace

// laya_smoke --keys <config.json> <code>...: types each code key by key with that config and
// prints every key's result, latency and page (latency investigations with a user's config).
int keys_mode(int argc, char** argv) {
    const std::string dict = project_path("data/pinyin.dict.bin");
    cxxime::Engine engine;
    if (!engine.initialize(dict, argv[2])) {
        std::printf("engine initialization failed\n");
        return 1;
    }
    for (int i = 0; i < 120 && !cxxime::LayaRerank::instance().stats().model_ready; ++i) {
        if (cxxime::LayaRerank::instance().stats().model_failed) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    std::printf("model ready: %d\n", cxxime::LayaRerank::instance().stats().model_ready ? 1 : 0);
    engine.set_trace_enabled(true);
    engine.set_partial_selection_enabled(true);  // as the TSF client requests
    for (int a = 3; a < argc; ++a) {
        const std::string code = argv[a];
        engine.clear();
        std::printf("%s\n", code.c_str());
        for (size_t i = 0; i < code.size(); ++i) {
            cxxime::KeyEvent ev;
            ev.keycode = static_cast<uint32_t>(toupper(static_cast<unsigned char>(code[i])));
            auto t0 = std::chrono::steady_clock::now();
            const auto r = engine.process_key(ev);
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            ev.is_key_up = true;
            engine.process_key(ev);
            std::printf("  %c result=%d %7.1f ms  input=%s  %s\n", code[i], static_cast<int>(r), ms,
                        engine.context().composition().active().input.c_str(),
                        join(marked_page(engine)).c_str());
            const cxxime::QueryTrace& tr = engine.last_trace();
            std::printf("      trace: cache=%d paths=%d live=%d cand=%d known=%d state=%u"
                        " complete=%u deadline=%d trunc=%d mixed=%d\n",
                        tr.cache_hit, tr.syllable_path_count, tr.live_path_count,
                        tr.candidate_count, tr.candidate_known_count, tr.candidate_extent_state,
                        tr.candidate_extent_complete, tr.deadline_exceeded, tr.truncated,
                        tr.mixed_cache_hit);
        }
    }
    engine.finalize();
    return 0;
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc >= 3 && std::string(argv[1]) == "--keys") return keys_mode(argc, argv);
    const std::string model_dir = argc > 1 ? argv[1] : project_path("models/laya");
    const std::string dict = project_path("data/pinyin.dict.bin");

    cxxime::Engine off, on;
    if (!off.initialize(dict, write_config("laya_smoke_off.json", false, model_dir)) ||
        !on.initialize(dict, write_config("laya_smoke_on.json", true, model_dir))) {
        std::printf("engine initialization failed (dictionary built? %s)\n", dict.c_str());
        return 1;
    }
    std::printf("waiting for the Laya model (%s) ...\n", model_dir.c_str());
    for (int i = 0; i < 120 && !cxxime::LayaRerank::instance().stats().model_ready; ++i) {
        if (cxxime::LayaRerank::instance().stats().model_failed) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (!cxxime::LayaRerank::instance().stats().model_ready) {
        std::printf("model not loaded\n");
        return 1;
    }

    // Most words are already first by frequency; quanli / xie / ci / shixiao need context.
    const std::vector<std::pair<const char*, const char*>> sentence = {
        {"wo", "我"}, {"jintian", "今天"}, {"xiawu", "下午"}, {"qu", "去"}, {"gongsi", "公司"},
        {"kaihui", "开会"}, {"women", "我们"}, {"hui", "会"}, {"jin", "尽"}, {"quanli", "全力"},
        {"wancheng", "完成"}, {"zhe", "这"}, {"ge", "个"}, {"xiangmu", "项目"},
        {"laoshi", "老师"}, {"zai", "在"}, {"heiban", "黑板"}, {"shang", "上"}, {"xie", "写"},
        {"le", "了"}, {"yi", "一"}, {"ge", "个"}, {"shuxue", "数学"}, {"gongshi", "公式"},
        {"zhe", "这"}, {"ci", "次"}, {"huaxue", "化学"}, {"shiyan", "实验"}, {"hen", "很"},
        {"chenggong", "成功"}, {"zhe", "这"}, {"zhang", "张"}, {"youhuiquan", "优惠券"},
        {"yijing", "已经"}, {"shixiao", "失效"}, {"le", "了"}};

    int off_hits = 0, on_hits = 0, marked = 0;
    auto first_marked = [](cxxime::Engine& e) {
        const auto& c = e.context().candidate_page().candidates;
        return !c.empty() && c.front().recommended;
    };
    std::vector<double> off_ms, on_ms;
    for (const auto& [py, gold] : sentence) {
        double a = 0, b = 0;
        type(off, py, &a);
        type(on, py, &b);
        off_ms.push_back(a);
        on_ms.push_back(b);
        auto po = page(off), pn = page(on);
        marked += first_marked(on);
        off_hits += !po.empty() && po[0] == gold;
        on_hits += !pn.empty() && pn[0] == gold;
        if (po != pn || po.empty() || po[0] != gold || pn[0] != gold)
            std::printf("[%s -> %s]%s\n  原序: %s\n  重排: %s\n", py, gold,
                        (!po.empty() && !pn.empty() && po[0] != pn[0]) ? "  << 首选改变" : "",
                        join(po).c_str(), join(marked_page(on)).c_str());
        choose(off, gold);
        choose(on, gold);
    }
    std::sort(off_ms.begin(), off_ms.end());
    std::sort(on_ms.begin(), on_ms.end());
    auto stats = cxxime::LayaRerank::instance().stats();
    std::printf("\n首选命中: 原序 %d/%zu, 重排 %d/%zu\n", off_hits, sentence.size(), on_hits, sentence.size());
    std::printf("首位带推荐标记 (*): %d/%zu\n", marked, sentence.size());
    std::printf("末键延迟中位数: 原序 %.2f ms, 重排 %.2f ms (模型推理最近一次 %.1f ms; 调用 %lld 次, 改变顺序 %lld 次)\n",
                off_ms[off_ms.size() / 2], on_ms[on_ms.size() / 2], stats.last_ms, stats.calls, stats.reordered);
    // English words in Chinese mode (config.english.mixed_in_chinese).
    std::printf("\n中文模式下的英文单词:\n");
    for (const char* code : {"hello", "computer", "progr", "china", "make", "can", "men", "hen", "wechat", "iphone", "xyzzy"}) {
        on.clear();
        type(on, code, nullptr);
        std::printf("  %-9s %s\n", code, join(page(on)).c_str());
    }
    on.clear();

    // English (ASCII) mode word completion (config.english.completion_in_ascii).
    std::printf("\n英文模式补全:\n");
    on.ascii_composer().set_ascii_mode(true);
    struct Key { uint32_t vk; bool shift; };
    auto keys_for = [](const std::string& s) {
        std::vector<Key> k;
        for (char c : s) {
            if (c >= 'a' && c <= 'z') k.push_back({static_cast<uint32_t>(c - 'a' + 'A'), false});
            else if (c >= 'A' && c <= 'Z') k.push_back({static_cast<uint32_t>(c), true});
            else if (c >= '0' && c <= '9') k.push_back({static_cast<uint32_t>(c), false});
            else if (c == ' ') k.push_back({VK_SPACE, false});
            else if (c == '\n') k.push_back({VK_RETURN, false});
            else if (c == '.') k.push_back({VK_OEM_PERIOD, false});
            else if (c == '\t') k.push_back({VK_TAB, false});
            else if (c == '\b') k.push_back({VK_BACK, false});
        }
        return k;
    };
    auto run = [&](const char* label, const std::string& keys) {
        std::string out, shown;
        for (const Key& k : keys_for(keys)) {
            cxxime::KeyEvent ev;
            ev.keycode = k.vk;
            if (k.shift) ev.set_shift();
            auto r = on.process_key(ev);
            if (r == cxxime::ProcessResult::COMMITTED) out += on.take_commit_text_with_source().first;
            else if (r == cxxime::ProcessResult::REJECTED && k.vk >= 'A' && k.vk <= 'Z') out += "<rejected>";
            if (on.context().is_composing()) shown = join(marked_page(on));
        }
        std::printf("  %-22s 最后候选: %-60s 上屏: \"%s\"\n", label, shown.c_str(), out.c_str());
    };
    run("comp + 空格", "comp ");
    run("comp + 选 3", "comp3");
    run("Comp + Tab + 空格", "Comp\t ");
    run("HEL + 2", "HEL2");
    run("xyzzy + 空格", "xyzzy ");
    run("wechat + 回车", "wechat\n");
    run("Hello. (句号)", "Hello.");
    run("helo + 退格", "helo\b\b ");
    // Letter-by-letter style (shortcuts.english_style, default Ctrl+Space): no word composition.
    on.set_english_word_mode(false);
    run("字母模式 comp + 空格", "comp ");
    on.set_english_word_mode(true);
    {
        cxxime::KeyEvent ev;
        ev.keycode = VK_SPACE;
        ev.modifiers = cxxime::kKeyModifierControl;
        auto r = on.process_key(ev);
        std::printf("  Ctrl+Space -> %s\n",
                    r == cxxime::ProcessResult::TOGGLE_ENGLISH_STYLE ? "TOGGLE_ENGLISH_STYLE" : "other");
        ev.is_key_up = true;
        on.process_key(ev);
    }
    // English sentences typed word by word (first 3 letters, then the word is picked with
    // Tab + Space, so both engines share the same context): is the intended word the first
    // completion (candidate 2, right after the typed text)?
    std::printf("\n英文句子 (每个词打前 3 个字母):\n");
    off.ascii_composer().set_ascii_mode(true);
    const std::vector<std::string> english = {
        "I would like to order a large pizza with extra cheese and a cold drink",
        "Could you please send me the report before the meeting tomorrow morning",
        "We should probably check the weather forecast before the trip",
        "She bought a new computer because the old one was broken",
    };
    auto press = [](cxxime::Engine& e, uint32_t vk, bool shift = false) {
        cxxime::KeyEvent ev;
        ev.keycode = vk;
        if (shift) ev.set_shift();
        if (e.process_key(ev) == cxxime::ProcessResult::COMMITTED) e.take_commit_text_with_source();
    };
    auto type_word = [&](cxxime::Engine& e, const std::string& word, size_t n) {
        for (size_t i = 0; i < n && i < word.size(); ++i) {
            const char c = word[i];
            press(e, static_cast<uint32_t>(toupper(static_cast<unsigned char>(c))), c >= 'A' && c <= 'Z');
        }
    };
    int en_off = 0, en_on = 0, en_total = 0;
    std::vector<double> en_ms;
    for (const std::string& line : english) {
        size_t pos = 0;
        while (pos < line.size()) {
            size_t end = line.find(' ', pos);
            if (end == std::string::npos) end = line.size();
            const std::string word = line.substr(pos, end - pos);
            pos = end + 1;
            const size_t n = word.size() > 3 ? 3 : word.size();
            std::vector<std::string> pages[2];
            int engine_index = 0;
            for (cxxime::Engine* e : {&off, &on}) {
                auto t0 = std::chrono::steady_clock::now();
                type_word(*e, word, n);
                if (e == &on)
                    en_ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
                auto p = page(*e);
                pages[engine_index++] = p;
                int idx = -1;
                for (size_t i = 0; i < p.size(); ++i)
                    if (p[i] == word) idx = static_cast<int>(i);
                if (idx >= 0) {
                    for (int t = 0; t < idx; ++t) press(*e, VK_TAB);
                } else {
                    type_word(*e, word.substr(n), word.size() - n);
                }
                press(*e, VK_SPACE);
            }
            if (word.size() <= 3) continue;  // fully typed: nothing to predict
            ++en_total;
            const bool hit_off = pages[0].size() > 1 && pages[0][1] == word;
            const bool hit_on = pages[1].size() > 1 && pages[1][1] == word;
            en_off += hit_off;
            en_on += hit_on;
            if (hit_off != hit_on)
                std::printf("  [%s] 原序: %s\n  %*s 重排: %s\n", word.c_str(), join(pages[0]).c_str(),
                            static_cast<int>(word.size()), "", join(pages[1]).c_str());
        }
    }
    std::sort(en_ms.begin(), en_ms.end());
    std::printf("  第一个补全命中: 原序 %d/%d, 重排 %d/%d; 打 3 个字母耗时中位数 %.1f ms\n", en_off, en_total,
                en_on, en_total, en_ms.empty() ? 0.0 : en_ms[en_ms.size() / 2]);
    off.ascii_composer().set_ascii_mode(false);
    on.ascii_composer().set_ascii_mode(false);
    // Pinyin style: full pinyin (initials may be mixed in) vs initials only (every letter is one
    // syllable's initial; zh/ch/sh count as one).
    std::printf("\npinyin style: full / initials\n");
    for (const char* code : {"zgr", "jt", "xian", "nh", "zhgr", "bjdx", "wmyq"}) {
        std::string line = std::string(code) + ":";
        for (bool initials : {false, true}) {
            on.set_pinyin_initials(initials);
            on.clear();
            type(on, code, nullptr);
            line += std::string(initials ? "  | initials " : "  full ") + join(page(on));
            on.clear();
        }
        std::printf("  %s\n", line.c_str());
    }
    on.set_pinyin_initials(false);

    // Fuzzy pinyin: every pair on vs only z=zh. Candidates reached only through a fuzzy
    // spelling carry their correct pinyin as the comment, shown as "text(pinyin)".
    auto fuzzy_config = [&](const char* name, const char* groups) {
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        const std::string path = std::string(tmp) + name;
        std::ofstream f(path, std::ios::binary);
        f << "{\"engine\": {\"page_size\": 7, \"candidate_learning\": false, "
          << "\"fuzzy_pinyin\": true, \"fuzzy_groups\": [" << groups << "]},\n"
          << " \"laya\": {\"enable\": false}}\n";
        return path;
    };
    cxxime::Engine fuzzy_all, fuzzy_zh;
    if (fuzzy_all.initialize(dict, fuzzy_config("laya_smoke_fuzzy_all.json",
            "\"z_zh\", \"c_ch\", \"s_sh\", \"n_l\", \"an_ang\", \"en_eng\", \"in_ing\"")) &&
        fuzzy_zh.initialize(dict, fuzzy_config("laya_smoke_fuzzy_zh.json", "\"z_zh\""))) {
        auto commented = [](cxxime::Engine& e) {
            std::vector<std::string> out;
            for (const auto& c : e.context().candidate_page().candidates)
                out.push_back(c.comment.empty() ? c.text : c.text + "(" + c.comment + ")");
            return out;
        };
        std::printf("\nfuzzy pinyin: off | all pairs | z=zh only\n");
        for (const char* code : {"zongguo", "lihao", "xinfu", "censhi", "xiangang", "zhongguo"}) {
            std::string line = std::string(code) + ":";
            for (cxxime::Engine* e : {&off, &fuzzy_all, &fuzzy_zh}) {
                e->clear();
                type(*e, code, nullptr);
                line += "  | " + join(e == &off ? page(*e) : commented(*e));
                e->clear();
            }
            std::printf("  %s\n", line.c_str());
        }
        fuzzy_all.finalize();
        fuzzy_zh.finalize();
    }

    // English spelling correction (english.correction) and what self-learning makes of the
    // commits (EnglishLearning, here in a temporary file).
    {
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        const std::string learning_path = std::string(tmp) + "laya_smoke_learning_english.json";
        DeleteFileA(learning_path.c_str());
        cxxime::EnglishLearning::open_shared(learning_path);
        const std::string config_path = std::string(tmp) + "laya_smoke_learn.json";
        {
            std::ofstream f(config_path, std::ios::binary);
            f << "{\"engine\": {\"page_size\": 7, \"candidate_learning\": true},\n"
              << " \"laya\": {\"enable\": true, \"model_dir\": \"" << json_escape_path(model_dir)
              << "\"}}\n";
        }
        cxxime::Engine learn;
        if (learn.initialize(dict, config_path)) {
            learn.ascii_composer().set_ascii_mode(true);
            auto show = [&](const std::string& word, double* ms) {
                learn.clear();
                for (const Key& k : keys_for(word)) {
                    cxxime::KeyEvent ev;
                    ev.keycode = k.vk;
                    if (k.shift) ev.set_shift();
                    auto t0 = std::chrono::steady_clock::now();
                    learn.process_key(ev);
                    if (ms) *ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                }
                return join(marked_page(learn));
            };
            auto commit = [&](const std::string& keys) {
                learn.clear();
                for (const Key& k : keys_for(keys)) {
                    cxxime::KeyEvent ev;
                    ev.keycode = k.vk;
                    if (k.shift) ev.set_shift();
                    if (learn.process_key(ev) == cxxime::ProcessResult::COMMITTED)
                        learn.take_commit_text_with_source();
                }
            };
            auto pick = [&](const std::string& typed, const std::string& word) {
                show(typed, nullptr);
                auto p = page(learn);
                for (size_t i = 0; i < p.size() && i < 9; ++i) {
                    if (p[i] == word) {
                        commit(typed + std::to_string(i + 1));
                        return true;
                    }
                }
                learn.clear();
                return false;
            };

            std::printf("\nEnglish spelling correction (last key ms):\n");
            std::vector<double> ms_list;
            for (const char* code : {"teh", "Teh", "recieve", "adress", "wierd", "definately",
                                     "beautf", "acomodate", "untill", "thier", "becuase",
                                     "goverment", "seperate", "tommorow", "occured", "form",
                                     "kubectl", "xyzzy"}) {
                double ms = 0;
                const std::string shown = show(code, &ms);
                ms_list.push_back(ms);
                std::printf("  %-11s %5.1f  %s\n", code, ms, shown.c_str());
            }
            std::sort(ms_list.begin(), ms_list.end());
            std::printf("  last key median %.1f ms\n", ms_list[ms_list.size() / 2]);

            std::printf("\nEnglish learning:\n");
            std::printf("  teh before           %s\n", show("teh", nullptr).c_str());
            pick("teh", "ten");
            std::printf("  teh, ten picked once %s\n", show("teh", nullptr).c_str());
            pick("teh", "ten");
            std::printf("  ten picked twice     %s\n", show("teh", nullptr).c_str());
            commit("teh\n");
            commit("teh\n");
            commit("teh\n");
            std::printf("  teh kept 3 times     %s\n", show("teh", nullptr).c_str());
            std::printf("  kubectk before       %s\n", show("kubectk", nullptr).c_str());
            commit("kubectk\n");
            std::printf("  kubectk kept once    %s\n", show("kubectk", nullptr).c_str());
            commit("kubectk ");
            std::printf("  kubectk kept twice   %s\n", show("kubectk", nullptr).c_str());
            std::printf("  kub                  %s\n", show("kub", nullptr).c_str());
            learn.clear();
            learn.finalize();
        }
        cxxime::EnglishLearning::close_shared();
        DeleteFileA(learning_path.c_str());
    }

    off.finalize();
    on.finalize();
    return 0;
}
