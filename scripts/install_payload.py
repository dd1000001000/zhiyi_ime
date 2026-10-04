# Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

from __future__ import annotations

import json
import os


def write_install_payload(
    dist_dir: str,
    include_x86_modules: bool,
    host_diagnostics: bool,
) -> None:
    """Generate one payload list for NSIS extraction and lifecycle cleanup."""
    root_files = [
        "zhiyi_tsf_x64.dll",
        "zhiyi_ime_x64.ime",
        "zhiyi-resources.dll",
        "zhiyi-server.exe",
        "zhiyi-settings.exe",
        "collect_diagnostics.ps1",
        "license.txt",
        "THIRD_PARTY_NOTICES.txt",
    ]
    if include_x86_modules:
        root_files.insert(1, "zhiyi_tsf_x86.dll")
        root_files.insert(3, "zhiyi_ime_x86.ime")
    if host_diagnostics:
        root_files.extend(["zhiyi-ime-host-probe-x64.exe", "export_host_trace.ps1"])
        if include_x86_modules:
            root_files.append("zhiyi-ime-host-probe-x86.exe")

    # Laya reranking: ONNX Runtime, the Visual C++ runtime it needs, and the model are optional
    # (the IME works without them).
    for name in ("onnxruntime.dll", "onnxruntime_providers_shared.dll", "msvcp140.dll",
                 "msvcp140_1.dll", "vcruntime140.dll", "vcruntime140_1.dll"):
        if os.path.isfile(os.path.join(dist_dir, name)):
            root_files.append(name)

    payload_files = list(root_files)
    for directory in ("data", "licenses", "laya"):
        source_dir = os.path.join(dist_dir, directory)
        if directory == "laya" and not os.path.isdir(source_dir):
            continue
        for entry in sorted(os.scandir(source_dir), key=lambda item: item.name):
            if entry.is_file():
                payload_files.append(f"{directory}/{entry.name}")
    missing = [
        name for name in payload_files
        if not os.path.isfile(os.path.join(dist_dir, *name.split("/")))
    ]
    if missing:
        raise RuntimeError(f"install payload files are missing: {', '.join(missing)}")

    manifest = {
        "format": "zhiyi-install-manifest",
        "version": 1,
        "files": payload_files + ["uninstall.exe"],
    }
    with open(os.path.join(dist_dir, "install-manifest.json"), "w",
              encoding="utf-8", newline="\n") as output:
        json.dump(manifest, output, ensure_ascii=False, indent=2)
        output.write("\n")

    grouped: dict[str, list[str]] = {}
    for relative in payload_files:
        directory, _, name = relative.rpartition("/")
        grouped.setdefault(directory, []).append(name)
    macro_lines = ["!macro InstallVersionPayload"]
    for directory, names in grouped.items():
        destination = "$StageDir" if not directory else f"$StageDir\\{directory}"
        if directory in ("data", "laya"):
            macro_lines.extend(["    !ifdef FAST", "        SetCompress off", "    !endif"])
        macro_lines.append(f'    SetOutPath "{destination}"')
        for name in names:
            source = name if not directory else f"{directory}\\{name}"
            macro_lines.append(f'    File "{source}"')
        if directory in ("data", "laya"):
            macro_lines.extend(["    !ifdef FAST", "        SetCompress auto", "    !endif"])
    macro_lines.extend([
        '    SetOutPath "$StageDir"',
        '    File "install-manifest.json"',
        "!macroend",
        "",
    ])
    with open(os.path.join(dist_dir, "install_payload.nsh"), "w",
              encoding="utf-8", newline="\n") as output:
        output.write("\n".join(macro_lines))
