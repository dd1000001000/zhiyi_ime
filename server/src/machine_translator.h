// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Offline translation for the learning mode (docs/learning-mode.md): candidates on the page
// the language pack has no translation for are translated by the downloaded model in the
// background (machine_translation.h). The page shows at once; the translations are added when
// they arrive (the callback updates the candidate window) and remembered for the next pages.
// Only the latest page is translated; the model is loaded on the first request and unloaded
// after learning.translator_idle_seconds without one, so nothing is loaded while the
// translations or this option are off.
#ifndef CXXIME_SERVER_MACHINE_TRANSLATOR_H_
#define CXXIME_SERVER_MACHINE_TRANSLATOR_H_

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <cxxime/machine_translation.h>

class MachineTranslator {
public:
    // text -> translation, for the candidates just translated.
    using Callback = std::function<void(const std::vector<std::pair<std::string, std::string>>&)>;

    static MachineTranslator& instance();

    void set_callback(Callback callback);

    // The translation remembered for (target, text); false when it was never asked.
    bool cached(const std::string& target, const std::string& text, std::string* translation);

    // Translates these candidates (the current page) on the graphics card `device`; replaces a
    // page still waiting.
    void request(std::vector<cxxime::mt::Item> items, std::string context, std::string device,
                 int idle_seconds);

    void stop();  // at server exit

private:
    MachineTranslator() = default;
    ~MachineTranslator();
    void run();

    struct Page {
        std::vector<cxxime::mt::Item> items;
        std::string context;
        std::string device;
        int idle_seconds = 60;
    };

    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    bool stopping_ = false;
    bool have_page_ = false;
    Page page_;
    Callback callback_;
    std::unordered_map<std::string, std::string> cache_;  // target \x1F text -> translation
    std::string failed_device_;  // the model could not start there; not tried again for a while
    long long failed_at_ms_ = 0;
};

#endif  // CXXIME_SERVER_MACHINE_TRANSLATOR_H_
