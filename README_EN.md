# CxxIME

**English** | [中文](README.md)

![C++](https://img.shields.io/badge/C%2B%2B-17-00599C?style=flat-square&logo=cplusplus&logoColor=white)
![Windows](https://img.shields.io/badge/Windows-10%2F11-0078D6?style=flat-square&logo=windows&logoColor=white)
![CMake](https://img.shields.io/badge/CMake-3.15%2B-064F8C?style=flat-square&logo=cmake&logoColor=white)
![Windows CI](https://img.shields.io/github/actions/workflow/status/deanxyuan/cxx-ime/windows-ci.yml?branch=master&label=Windows%20CI&style=flat-square)
![License](https://img.shields.io/github/license/deanxyuan/cxx-ime?style=flat-square)

> A lightweight Windows TSF (Text Services Framework) input method (Pinyin + Wubi + Mixed).

CxxIME is a lightweight Windows TSF-based input method with three modes: Pinyin, Wubi 86, and mixed Pinyin + Wubi. The client (TSF DLL) only captures keystrokes and presents candidates; Pinyin parsing, dictionary lookup, and candidate generation all happen in a separate server process, so every input session shares one copy of the dictionary and a crashing engine cannot take down the host application you are typing in.

## Features

- Pinyin, Wubi 86, and mixed modes, with full Pinyin, four Shuangpin schemes (Microsoft / Xiaohe / Ziranma / Sogou), shorthand, fuzzy syllables, dynamic sentence building, and segment-by-segment selection
- Candidates ranked in tiers by match quality, so exact syllables and near-complete words are never buried by frequent long words; long Pinyin can be selected by segment
- A dedicated Wubi prefix index covering shortcut codes, completion hints, automatic commit on a unique four-code match, and fifth-code handling
- System symbols are kept out of dictionary candidates: type `\` or a category mnemonic (such as `\bd`) to browse and commit them by category, and browse or copy them in Settings
- Candidate window supports horizontal and vertical layouts, D2D and GDI rendering, and 12 built-in themes (6 palettes × light/dark)
- App hosts can take over inline preedit and candidate rendering via TSF UIElement (verified in DOTA2)
- Candidate learning is off by default; when enabled, preferences persist independently, and the user dictionary, candidate order, and learning data are managed separately in Settings
- User data lives in `%USERPROFILE%\cxxime\`, is kept on uninstall by default, and supports backup and merge import

## Screenshots

Candidate window theme previews (6 palettes × light/dark):

| Palette | Light | Dark |
|---------|-------|------|
| Moon | ![Moon Light](docs/images/themes/moon_light.png) | ![Moon Dark](docs/images/themes/moon_dark.png) |
| Sky | ![Sky Light](docs/images/themes/sky_light.png) | ![Sky Dark](docs/images/themes/sky_dark.png) |
| Jade | ![Jade Light](docs/images/themes/jade_light.png) | ![Jade Dark](docs/images/themes/jade_dark.png) |
| Amber | ![Amber Light](docs/images/themes/amber_light.png) | ![Amber Dark](docs/images/themes/amber_dark.png) |
| Coral | ![Coral Light](docs/images/themes/coral_light.png) | ![Coral Dark](docs/images/themes/coral_dark.png) |
| Iris | ![Iris Light](docs/images/themes/iris_light.png) | ![Iris Dark](docs/images/themes/iris_dark.png) |

## Installation

1. Download `cxxime-v<version>-setup.exe` from [Releases](https://github.com/deanxyuan/cxx-ime/releases) and follow the wizard
2. **Log off and log back on** after installation (the TSF text service is only loaded at logon)
3. Switch to CxxIME with `Ctrl+Space` or `Win+Space`

Each version gets its own version directory, so upgrades and downgrades never overwrite older files; files still in use by a host process are cleaned up by a later install or a full restart, and installation never forces an immediate reboot. Uninstalling keeps your configuration, user dictionary, and learning data under `%USERPROFILE%\cxxime\`.

See [docs/installation.md](docs/installation.md) for detailed installation, uninstall, and upgrade instructions.

## Performance

The dictionary and indexes are loaded into server memory in one pass. Frequent inputs are served from pre-built indexes, while other inputs use a scan budget, bounded candidate collection, and a query deadline to keep latency predictable.

In the release benchmark history, the Preedit IPC round trip averages about `50 µs`, and the query P50 for `nihao` and `nihaoshijie` is no higher than `60 µs` and about `170 µs` respectively.

Results vary with hardware and dictionary data. See [docs/benchmark-data.md](docs/benchmark-data.md) and [docs/ipc-architecture.md](docs/ipc-architecture.md) for the full data and reproduction steps.

## Configuration

- Use **CxxIME Settings** from the Start Menu
- Or edit the user configuration file `%USERPROFILE%\cxxime\default.json` directly

All options (input modes, Pinyin scheme, candidate window, themes, dictionary management, symbols, shortcuts, etc.) are documented in [docs/settings-guide.md](docs/settings-guide.md).

## Dictionaries

CxxIME ships with Pinyin and Wubi 86 dictionaries. The Pinyin data comes from [rime-ice](https://github.com/iDvel/rime-ice) (~1.9M entries, GPL-3.0-only), and the Wubi data from [KyleBing/rime-wubi86-jidian](https://github.com/KyleBing/rime-wubi86-jidian) (Apache-2.0). Dictionary sources and licenses are documented in [data/README.md](data/README.md), and the data formats and build/maintenance pipeline in [docs/dictionary.md](docs/dictionary.md).

System symbols (emoji and standalone punctuation or symbol characters) are stripped from the dictionaries at build time and never appear as Pinyin or Wubi candidates; they are provided by the bundled symbol table and entered with `\` by category.

## Compatibility

- **Windows 10 / 11**: verified through daily use and regression testing
- **Windows 7 and earlier**: not verified and not supported

## Building from Source

```cmd
build.bat                                             # development build (output in build\, tools and tests enabled)
ctest --test-dir build -C Release --output-on-failure # run unit tests
python scripts\package.py                             # build a releasable installer
```

Requirements: Windows 10/11, Visual Studio 2017 or newer (C++ workload), CMake 3.15+; packaging additionally needs Python 3.10+ and [NSIS 3.x](https://nsis.sourceforge.io/). The installer is written to `..\output\cxxime-v<version>-setup.exe`.

## License

The project code is released under the Apache License 2.0. Third-party components and dictionary data retain their respective licenses; see [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).
