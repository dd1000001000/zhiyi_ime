#!/usr/bin/env python3
# Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest


ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))

from install_payload import write_install_payload


class InstallPayloadTest(unittest.TestCase):
    def create_payload(self, directory: str, include_x86: bool, host_diagnostics: bool) -> None:
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
        if include_x86:
            root_files.extend(["zhiyi_tsf_x86.dll", "zhiyi_ime_x86.ime"])
        if host_diagnostics:
            root_files.extend(["zhiyi-ime-host-probe-x64.exe", "export_host_trace.ps1"])
            if include_x86:
                root_files.append("zhiyi-ime-host-probe-x86.exe")
        for name in root_files:
            with open(os.path.join(directory, name), "w", encoding="ascii") as output:
                output.write("x")
        for child, name in (("data", "default.json"), ("licenses", "miniz-MIT.txt")):
            os.makedirs(os.path.join(directory, child), exist_ok=True)
            with open(os.path.join(directory, child, name), "w", encoding="ascii") as output:
                output.write("x")

    def test_manifest_and_nsis_macro_share_the_same_payload(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            self.create_payload(directory, include_x86=True, host_diagnostics=True)
            write_install_payload(directory, True, True)

            with open(os.path.join(directory, "install-manifest.json"), encoding="utf-8") as source:
                manifest = json.load(source)
            with open(os.path.join(directory, "install_payload.nsh"), encoding="utf-8") as source:
                macro = source.read()

            self.assertEqual(manifest["format"], "zhiyi-install-manifest")
            self.assertEqual(len(manifest["files"]), len(set(manifest["files"])))
            self.assertIn("uninstall.exe", manifest["files"])
            for relative in manifest["files"]:
                if relative != "uninstall.exe":
                    nsis_path = relative.replace("/", "\\")
                    self.assertIn(f'File "{nsis_path}"', macro)

    def test_x86_diagnostics_follow_x86_payload_selection(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            self.create_payload(directory, include_x86=False, host_diagnostics=True)
            write_install_payload(directory, False, True)

            with open(os.path.join(directory, "install-manifest.json"), encoding="utf-8") as source:
                files = json.load(source)["files"]
            self.assertNotIn("zhiyi_tsf_x86.dll", files)
            self.assertNotIn("zhiyi-ime-host-probe-x86.exe", files)


if __name__ == "__main__":
    unittest.main()
