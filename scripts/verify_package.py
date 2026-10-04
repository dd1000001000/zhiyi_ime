#!/usr/bin/env python3
# Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.
#
# Static package preflight checks. This script validates dist/ contents before
# NSIS builds the installer. It does not install, register, or execute ZhiyiIME.

from __future__ import annotations

import argparse
import os
import sys

from package_checks.preflight import run_checks

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_DIST_DIR = os.path.join(ROOT, "dist")


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify ZhiyiIME dist package layout")
    parser.add_argument("--dist-dir", default=DEFAULT_DIST_DIR)
    parser.add_argument(
        "--allow-missing-x86",
        action="store_true",
        help="Allow dist-only checks without 32-bit TSF/legacy IME modules.",
    )
    parser.add_argument(
        "--host-diag",
        action="store_true",
        help="Require the host diagnostics package layout.",
    )
    args = parser.parse_args()

    errors = run_checks(
        os.path.abspath(args.dist_dir),
        require_x86=not args.allow_missing_x86,
        host_diagnostics=args.host_diag,
    )
    if errors:
        print("Package preflight FAILED:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1

    print("Package preflight PASSED.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
