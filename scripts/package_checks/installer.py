# Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

from __future__ import annotations

import os
import re

from package_checks.common import add_error, read_text, require_file
from package_checks.installer_flow import check_installer_flow
from package_checks.installer_payload import check_installer_payload


def read_installer_sources(errors: list[str], dist_dir: str, nsi_path: str) -> str:
    text = read_text(nsi_path)
    include_names = re.findall(r'!include\s+"nsis\\([^"\\]+\.nsh)"', text)
    if not include_names:
        add_error(errors, "zhiyi-setup.nsi: no project NSIS includes found")
        return text

    if len(include_names) != len(set(include_names)):
        add_error(errors, "zhiyi-setup.nsi: duplicate project NSIS include")

    include_dir = os.path.join(dist_dir, "nsis")
    actual_names: set[str] = set()
    if os.path.isdir(include_dir):
        actual_names = {
            entry.name
            for entry in os.scandir(include_dir)
            if entry.is_file() and entry.name.endswith(".nsh")
        }

    for name in include_names:
        include_path = os.path.join(include_dir, name)
        if require_file(errors, include_path, dist_dir):
            text += "\n" + read_text(include_path)

    for name in sorted(actual_names - set(include_names)):
        add_error(errors, f"unused NSIS include: nsis/{name}")

    return text


def find_section(errors: list[str], text: str, section_name: str) -> str:
    label = f"{section_name} section"
    start = text.find(f'Section "{section_name}"')
    end = text.find("SectionEnd", start)
    if start < 0 or end < 0:
        add_error(errors, f"zhiyi-setup.nsi: missing {label}")
        return ""
    return text[start:end]


def check_installer_script(
    errors: list[str],
    dist_dir: str,
    require_x86: bool,
    manifest_files: list[str],
    host_diagnostics: bool,
) -> None:
    nsi_path = os.path.join(dist_dir, "zhiyi-setup.nsi")
    if not require_file(errors, nsi_path, dist_dir):
        return

    text = read_installer_sources(errors, dist_dir, nsi_path)
    install_text = find_section(errors, text, "Install")
    uninstall_text = find_section(errors, text, "Uninstall")
    check_installer_flow(errors, text, install_text, uninstall_text)
    check_installer_payload(
        errors,
        text,
        dist_dir,
        require_x86,
        manifest_files,
        host_diagnostics,
    )
