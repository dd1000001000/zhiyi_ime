// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_TEST_WUBI_ENGINE_TEST_SUPPORT_H_
#define CXXIME_TEST_WUBI_ENGINE_TEST_SUPPORT_H_

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include <windows.h>

#include <cxxime/engine.h>
#include <cxxime/input_limits.h>
#include <cxxime/syllabifier.h>
#include <cxxime/wubi_processor.h>
#include <cxxime/wubi_translator.h>

#include "support/testutil.h"
#include "support/test_runtime.h"
#include "support/wubi_index_test_data.h"

inline char wubi_engine_test_temp_path[MAX_PATH] = {};

inline std::string make_temp_path(const char* name) {
    return std::string(wubi_engine_test_temp_path) + "\\" + name;
}

inline cxxime::KeyEvent make_key(uint32_t vk, bool shift = false) {
    cxxime::KeyEvent event;
    event.keycode = vk;
    event.is_key_up = false;
    if (shift) {
        event.set_shift();
    }
    return event;
}

#endif // CXXIME_TEST_WUBI_ENGINE_TEST_SUPPORT_H_
