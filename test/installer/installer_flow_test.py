#!/usr/bin/env python3
# Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

from __future__ import annotations

import os
import sys
import unittest


ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))

from package_checks.installer import find_section
from package_checks.installer_flow import check_installer_flow


class InstallerFlowTest(unittest.TestCase):
    def test_installer_sources_follow_current_lifecycle(self) -> None:
        script_path = os.path.join(ROOT, "scripts", "zhiyi-setup.nsi")
        with open(script_path, encoding="utf-8-sig") as source:
            text = source.read()
        include_dir = os.path.join(ROOT, "scripts", "nsis")
        for entry in sorted(os.scandir(include_dir), key=lambda item: item.name):
            if entry.is_file() and entry.name.endswith(".nsh"):
                with open(entry.path, encoding="utf-8-sig") as source:
                    text += "\n" + source.read()

        errors: list[str] = []
        install_text = find_section(errors, text, "Install")
        uninstall_text = find_section(errors, text, "Uninstall")
        check_installer_flow(errors, text, install_text, uninstall_text)
        self.assertEqual(errors, [])


if __name__ == "__main__":
    unittest.main()
