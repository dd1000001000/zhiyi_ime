// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Installed learning-mode language packs (docs/learning-mode.md). The IME loads packs only from
// %USERPROFILE%\zhiyi\glossary; the installer's built-in packs (data\glossary) are copied there
// once, and again when the installer brings a newer version, unless the user removed them
// (glossary\state.json).
#ifndef CXXIME_SETTINGS_GLOSSARY_PACKS_H_
#define CXXIME_SETTINGS_GLOSSARY_PACKS_H_

#include <cstdint>
#include <string>

#include <cxxime/glossary.h>

namespace cxxime {
namespace settings {

struct InstalledPack {
    bool installed = false;
    GlossaryHeader header;
    std::uint64_t size = 0;
};

InstalledPack installed_pack(const std::string& source, const std::string& target);
bool is_builtin_pack(const std::string& source, const std::string& target);
std::uint32_t builtin_pack_version(const std::string& source, const std::string& target);  // 0: none

// At settings start: copies built-in packs as described above; removes leftovers of replaced
// packs that were still mapped.
void seed_builtin_packs();

// Puts a downloaded (and checked) pack in place; the old one may still be mapped by the server.
bool install_pack(const std::wstring& downloaded, const std::string& source,
                  const std::string& target);
// Copies the built-in pack back (after a removal).
bool restore_builtin_pack(const std::string& source, const std::string& target);
bool remove_pack(const std::string& source, const std::string& target);

}  // namespace settings
}  // namespace cxxime

#endif  // CXXIME_SETTINGS_GLOSSARY_PACKS_H_
