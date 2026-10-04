# Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

from __future__ import annotations

import json
import os

from package_checks.common import add_error, forbid_text, read_text, require_file, require_text


def check_installer_payload(
    errors: list[str],
    text: str,
    dist_dir: str,
    require_x86: bool,
    manifest_files: list[str],
    host_diagnostics: bool,
) -> None:
    label = "zhiyi-setup.nsi"
    for item in (
        '!define MUI_ICON "zhiyi.ico"',
        '!define MUI_UNICON "zhiyi.ico"',
        'File /oname=zhiyi-installer-helper.exe',
        '!include "install_payload.nsh"',
        "!insertmacro InstallVersionPayload",
        '"$WINDIR\\Sysnative\\regsvr32.exe" /s',
        '"$WINDIR\\Sysnative\\regsvr32.exe" /u /s',
        '"$SYSDIR\\regsvr32.exe" /s',
        '"$SYSDIR\\regsvr32.exe" /u /s',
        "kernel32::CopyFileW",
        'Delete /REBOOTOK "$WINDIR\\Sysnative\\zhiyi.ime"',
        'Delete /REBOOTOK "$SYSDIR\\zhiyi.ime"',
        "知意输入法需要 64 位 Windows。",
        "UninstPage custom un.ConfirmPage un.ConfirmPageLeave",
        "删除用户配置和词库数据",
        'StrCpy $UninstallUserDataDir "$PROFILE\\zhiyi"',
    ):
        require_text(errors, text, item, label)

    manifest_path = os.path.join(dist_dir, "install-manifest.json")
    macro_path = os.path.join(dist_dir, "install_payload.nsh")
    if not require_file(errors, manifest_path, dist_dir) or not require_file(
        errors, macro_path, dist_dir
    ):
        return
    try:
        manifest = json.loads(read_text(manifest_path))
    except (OSError, json.JSONDecodeError) as exc:
        add_error(errors, f"install-manifest.json: invalid JSON: {exc}")
        return
    if not isinstance(manifest, dict):
        add_error(errors, "install-manifest.json: root must be an object")
        return
    files = manifest.get("files")
    if (
        manifest.get("format") != "zhiyi-install-manifest"
        or manifest.get("version") != 1
        or not isinstance(files, list)
        or not all(isinstance(item, str) and item for item in files)
        or len(files) != len(set(files))
    ):
        add_error(errors, "install-manifest.json: invalid lifecycle manifest")
        return

    required = {
        "zhiyi_tsf_x64.dll",
        "zhiyi_ime_x64.ime",
        "zhiyi-resources.dll",
        "zhiyi-server.exe",
        "zhiyi-settings.exe",
        "uninstall.exe",
        "licenses/miniz-MIT.txt",
        "licenses/rime-ice-GPL-3.0.txt",
        *(f"data/{name}" for name in manifest_files),
    }
    if require_x86:
        required.update({"zhiyi_tsf_x86.dll", "zhiyi_ime_x86.ime"})
    if host_diagnostics:
        required.update({"zhiyi-ime-host-probe-x64.exe", "export_host_trace.ps1"})
        if require_x86:
            required.add("zhiyi-ime-host-probe-x86.exe")
    for name in sorted(required - set(files)):
        add_error(errors, f"install-manifest.json: missing `{name}`")

    macro = read_text(macro_path)
    require_text(errors, macro, "!macro InstallVersionPayload", "install_payload.nsh")
    require_text(errors, macro, 'File "install-manifest.json"', "install_payload.nsh")
    for relative in files:
        if relative == "uninstall.exe":
            continue
        source = relative.replace("/", "\\")
        require_text(errors, macro, f'File "{source}"', "install_payload.nsh")
        require_file(errors, os.path.join(dist_dir, *relative.split("/")), dist_dir)

    forbid_text(errors, text, "StageInstalledEntry", label)
    forbid_text(errors, text, "zhiyi_tsf.dll", label)
