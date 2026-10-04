# Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

from __future__ import annotations

import os

from package_checks.common import read_text, require_file, require_text


def check_diagnostics_script(
    errors: list[str],
    dist_dir: str,
    require_x86: bool,
    manifest_files: list[str],
) -> None:
    script_path = os.path.join(dist_dir, "collect_diagnostics.ps1")
    if not require_file(errors, script_path, dist_dir):
        return

    text = read_text(script_path)
    label = "collect_diagnostics.ps1"
    require_text(errors, text, '"zhiyi_tsf_x64.dll"', label)
    require_text(errors, text, '"zhiyi_ime_x64.ime"', label)
    require_text(errors, text, '"zhiyi-resources.dll"', label)
    require_text(errors, text, "$report.system_ime_files", label)
    require_text(errors, text, "Get-FileInfoSafe -Path $systemImeX64", label)
    require_text(errors, text, "Get-FileInfoSafe -Path $systemImeX86", label)
    require_text(errors, text, "registry-clsid-64.txt", label)
    require_text(errors, text, "registry-tip-64.txt", label)
    require_text(errors, text, "registry-keyboard-layouts-zhiyi.txt", label)
    require_text(errors, text, "learning_pinyin.tsv", label)
    require_text(errors, text, "learning_wubi.tsv", label)
    require_text(errors, text, "candidate_order_pinyin.tsv", label)
    require_text(errors, text, "candidate_order_wubi.tsv", label)
    require_text(errors, text, "IncludeCandidatePreferences", label)
    for name in ["dictionary_manifest.json"] + manifest_files:
        require_text(errors, text, f'"{name}"', label)
    if require_x86:
        require_text(errors, text, '"zhiyi_tsf_x86.dll"', label)
        require_text(errors, text, '"zhiyi_ime_x86.ime"', label)
        require_text(errors, text, "registry-clsid-32.txt", label)
        require_text(errors, text, "registry-tip-32.txt", label)
