# Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

from __future__ import annotations

import os

from package_checks.common import (
    MACHINE_X64,
    MACHINE_X86,
    add_error,
    read_text,
    require_file,
    require_machine,
    require_text,
)
from package_checks.diagnostics import check_diagnostics_script
from package_checks.dictionary import check_dictionary_manifest
from package_checks.installer import check_installer_script


def check_required_files(
    errors: list[str],
    dist_dir: str,
    require_x86: bool,
    host_diagnostics: bool,
) -> None:
    required_files = [
        "zhiyi_tsf_x64.dll",
        "zhiyi_ime_x64.ime",
        "zhiyi-resources.dll",
        "zhiyi-server.exe",
        "zhiyi-settings.exe",
        "zhiyi-installer-helper.exe",
        "collect_diagnostics.ps1",
        "zhiyi-setup.nsi",
        "install-manifest.json",
        "install_payload.nsh",
        "zhiyi.ico",
        "license.txt",
        "THIRD_PARTY_NOTICES.txt",
        os.path.join("licenses", "Apache-2.0.txt"),
        os.path.join("licenses", "rime-ice-GPL-3.0.txt"),
        os.path.join("data", "default.json"),
        os.path.join("data", "settings_presets.json"),
        os.path.join("data", "themes.json"),
        os.path.join("data", "punctuation.json"),
        os.path.join("data", "symbols.json"),
    ]
    diagnostic_files = [
        "export_host_trace.ps1",
        "zhiyi-ime-host-probe-x64.exe",
    ]
    if host_diagnostics:
        required_files.extend(diagnostic_files)
    else:
        for name in diagnostic_files + ["zhiyi-ime-host-probe-x86.exe"]:
            if os.path.exists(os.path.join(dist_dir, name)):
                add_error(errors, f"host diagnostic file must not be packaged: {name}")
    if require_x86:
        required_files.extend(["zhiyi_tsf_x86.dll", "zhiyi_ime_x86.ime"])
        if host_diagnostics:
            required_files.append("zhiyi-ime-host-probe-x86.exe")

    for name in required_files:
        require_file(errors, os.path.join(dist_dir, name), dist_dir)


def check_licenses(errors: list[str], dist_dir: str) -> None:
    notices_path = os.path.join(dist_dir, "THIRD_PARTY_NOTICES.txt")
    if os.path.isfile(notices_path):
        notices = read_text(notices_path)
        require_text(errors, notices, "rime-ice", "THIRD_PARTY_NOTICES.txt")
        require_text(errors, notices, "GPL-3.0-only", "THIRD_PARTY_NOTICES.txt")
        require_text(
            errors,
            notices,
            "licenses/rime-ice-GPL-3.0.txt",
            "THIRD_PARTY_NOTICES.txt",
        )

    rime_ice_license_path = os.path.join(
        dist_dir,
        "licenses",
        "rime-ice-GPL-3.0.txt",
    )
    if os.path.isfile(rime_ice_license_path):
        rime_ice_license = read_text(rime_ice_license_path)
        require_text(
            errors,
            rime_ice_license,
            "GNU GENERAL PUBLIC LICENSE",
            "rime-ice license",
        )
        require_text(
            errors,
            rime_ice_license,
            "Version 3, 29 June 2007",
            "rime-ice license",
        )


def check_binary_architectures(
    errors: list[str],
    dist_dir: str,
    require_x86: bool,
    host_diagnostics: bool,
) -> None:
    x64_files = [
        "zhiyi_tsf_x64.dll",
        "zhiyi_ime_x64.ime",
        "zhiyi-resources.dll",
        "zhiyi-server.exe",
        "zhiyi-settings.exe",
        "zhiyi-installer-helper.exe",
    ]
    if host_diagnostics:
        x64_files.append("zhiyi-ime-host-probe-x64.exe")
    for name in x64_files:
        require_machine(errors, os.path.join(dist_dir, name), dist_dir, MACHINE_X64)

    if require_x86:
        x86_files = ["zhiyi_tsf_x86.dll", "zhiyi_ime_x86.ime"]
        if host_diagnostics:
            x86_files.append("zhiyi-ime-host-probe-x86.exe")
        for name in x86_files:
            require_machine(errors, os.path.join(dist_dir, name), dist_dir, MACHINE_X86)


def run_checks(dist_dir: str, require_x86: bool, host_diagnostics: bool) -> list[str]:
    errors: list[str] = []
    check_required_files(errors, dist_dir, require_x86, host_diagnostics)
    check_licenses(errors, dist_dir)

    for name in ["zhiyi_tsf_x64.dll.old", "zhiyi_tsf_x86.dll.old"]:
        if os.path.exists(os.path.join(dist_dir, name)):
            add_error(errors, f"obsolete file must not be packaged: {name}")

    check_binary_architectures(errors, dist_dir, require_x86, host_diagnostics)
    manifest_files = check_dictionary_manifest(errors, dist_dir)
    check_installer_script(
        errors,
        dist_dir,
        require_x86,
        manifest_files,
        host_diagnostics,
    )
    check_diagnostics_script(errors, dist_dir, require_x86, manifest_files)
    return errors
