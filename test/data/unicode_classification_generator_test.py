#!/usr/bin/env python3
"""Repository-owned Unicode inputs reproduce the frozen classification data."""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "data/tools"))
import generate_emoji_classification as generator


class UnicodeGeneratorTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="cxxime-unicode-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.output = self.root / "classification.json"
        self.inputs = [
            generator.DEFAULT_SOURCE_DIR / name for name, _, _ in generator.SOURCES
        ]

    def test_repository_inputs_reproduce_classification(self):
        self.assertEqual(generator.DEFAULT_SOURCE_DIR,
                         ROOT / "data/unicode" / generator.VERSION)
        generator.generate(*self.inputs, self.output)
        self.assertEqual(
            self.output.read_text(encoding="utf-8"),
            generator.DEFAULT_OUTPUT.read_text(encoding="utf-8"),
        )

    def copy_inputs(self):
        inputs = []
        for source in self.inputs:
            target = self.root / source.name
            shutil.copy2(source, target)
            inputs.append(target)
        return inputs

    def test_each_input_is_pinned_before_output_is_opened(self):
        inputs = self.copy_inputs()
        self.output.write_bytes(b"previous")
        for path, source in zip(inputs, self.inputs):
            with self.subTest(path=path.name):
                with path.open("ab") as stream:
                    stream.write(b"altered")
                with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                    generator.generate(*inputs, self.output)
                self.assertEqual(self.output.read_bytes(), b"previous")
                shutil.copy2(source, path)

    def test_output_cannot_overwrite_source(self):
        inputs = self.copy_inputs()
        for path in inputs:
            with self.subTest(path=path.name):
                before = path.read_bytes()
                with self.assertRaisesRegex(ValueError, "overwrite Unicode"):
                    generator.generate(*inputs, path)
                self.assertEqual(path.read_bytes(), before)

    def test_cli_defaults_do_not_depend_on_working_directory(self):
        result = subprocess.run(
            [sys.executable, str(ROOT / "data/tools/generate_emoji_classification.py"),
             "--output", str(self.output)],
            cwd=self.root, capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            self.output.read_text(encoding="utf-8"),
            generator.DEFAULT_OUTPUT.read_text(encoding="utf-8"),
        )

    def test_missing_input_does_not_overwrite_output(self):
        self.output.write_bytes(b"previous")
        with mock.patch.object(sys, "argv", [
            "generate_emoji_classification.py", "--source-dir", str(self.root),
            "--output", str(self.output),
        ]):
            with self.assertRaises(SystemExit) as error:
                generator.main()
        self.assertEqual(error.exception.code, 1)
        self.assertEqual(self.output.read_bytes(), b"previous")


if __name__ == "__main__":
    unittest.main()
