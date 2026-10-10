// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_PROCESSOR_H_
#define CXXIME_PROCESSOR_H_

#include <cxxime/context.h>
#include <cxxime/key_event.h>

namespace cxxime {

enum class ProcessResult {
    ACCEPTED,
    REJECTED,
    COMMITTED,
    CANDIDATE_SELECTED,
    TOGGLE_SHAPE,   // full/half shape toggle (shortcuts.shape_toggle, Shift+Space)
    TOGGLE_PUNCT,   // Chinese/English punctuation toggle (shortcuts.punct_toggle, Ctrl+.)
    // The first KeyDown switches; repeats and the matching KeyUp are only consumed.
    SWITCH_INPUT_MODE,
    INPUT_MODE_SHORTCUT_HANDLED,
    // English mode style shortcut (word completion <-> letter by letter); same KeyDown/KeyUp
    // handling as SWITCH_INPUT_MODE.
    TOGGLE_ENGLISH_STYLE,
    // The same shortcut in Chinese pinyin mode: full pinyin <-> initials (首字母).
    TOGGLE_PINYIN_STYLE,
    // Learning mode: Ctrl+1..9 asks for that candidate's translation (docs/learning-mode.md).
    // The server looks it up and calls Engine::commit_translation; never sent to the client.
    COMMIT_TRANSLATION,
};

// Abstract processor interface
class IProcessor {
public:
    virtual ~IProcessor() = default;
    virtual bool accepts_code_key(const KeyEvent&, const Context&) const { return false; }
    virtual ProcessResult process_key(const KeyEvent& event, Context& context) = 0;
};

// Pinyin processor implementation
class PinyinProcessor : public IProcessor {
public:
    void set_shuangpin_enabled(bool enabled) { shuangpin_enabled_ = enabled; }
    bool accepts_code_key(const KeyEvent& event, const Context& context) const override;
    ProcessResult process_key(const KeyEvent& event, Context& context) override;

private:
    bool shuangpin_enabled_ = false;
};

} // namespace cxxime

#endif // CXXIME_PROCESSOR_H_
