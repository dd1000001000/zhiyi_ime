#!/usr/bin/env python3
# Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.
#
# ZhiyiIME packaging script — builds, prepares dictionaries, and creates installer.
#
# Usage:
#   python scripts/package.py                # Release build + package
#   python scripts/package.py --debug        # Debug build + package
#   python scripts/package.py --skip-build   # Skip cmake build (already built)
#   python scripts/package.py --skip-dict    # Skip dictionary generation
#   python scripts/package.py --host-diag    # Include host diagnostics and Probe
#   python scripts/package.py --output-dir output
#   python scripts/package.py --generator "Visual Studio 17 2022" --platform x64

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys
import time

from dictionary_bundle_layout import PINYIN_SPELLING_FILES
from install_payload import write_install_payload

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPTS = os.path.join(ROOT, "scripts")
HOST_TAKEOVER_DIAGNOSTICS = os.path.join(ROOT, "diagnostics", "host_takeover")
DATA = os.path.join(ROOT, "data")
DEFAULT_BUILD_DIR = os.path.join(ROOT, "build-package")
DEFAULT_X86_BUILD_DIR = os.path.join(ROOT, "build-package-x86")
DIST_DIR = os.path.join(ROOT, "dist")
DEFAULT_OUTPUT_DIR = os.path.join(ROOT, "..", "output")
with open(os.path.join(ROOT, "VERSION"), encoding="ascii") as version_file:
    VERSION = version_file.read().strip()
_VERSION_MATCH = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)(?:-[0-9A-Za-z.-]+)?", VERSION)
if _VERSION_MATCH is None:
    raise RuntimeError("VERSION must contain a valid semantic version")
VERSION_NUMERIC = ".".join(_VERSION_MATCH.groups()) + ".0"
SINGLE_CONFIG_GENERATORS = (
    "NMake Makefiles",
    "NMake Makefiles JOM",
    "Ninja",
    "Unix Makefiles",
    "MinGW Makefiles",
)


def step(msg: str) -> None:
    """Print a step header."""
    print(f"\n{msg}")


def run(cmd: list[str], **kwargs) -> None:
    """Run a command, raising on failure."""
    subprocess.run(cmd, check=True, **kwargs)


def default_job_count() -> int:
    """Return the default parallel job budget for package builds."""
    return max(1, os.cpu_count() or 1)


def format_duration(seconds: float) -> str:
    """Format elapsed seconds for package timing output."""
    seconds = max(0.0, seconds)
    minutes = int(seconds // 60)
    secs = seconds - minutes * 60
    if minutes:
        return f"{minutes}m {secs:04.1f}s"
    return f"{secs:.1f}s"


def timed_call(fn, *args, **kwargs):
    """Run a callable and return (elapsed_seconds, result)."""
    start = time.perf_counter()
    result = fn(*args, **kwargs)
    return time.perf_counter() - start, result


def wait_task(name: str, future: concurrent.futures.Future) -> float:
    """Wait for a package task and print a clear failure label."""
    try:
        elapsed, _ = future.result()
        print(f"  {name}: {format_duration(elapsed)}")
        return elapsed
    except SystemExit as e:
        code = e.code if isinstance(e.code, int) else 1
        sys.exit(code)
    except Exception as e:
        print(f"  ERROR: {name} failed: {e}", file=sys.stderr)
        sys.exit(1)


def print_timing_summary(timings: list[tuple[str, float]], total_elapsed: float) -> None:
    """Print final timing summary."""
    print()
    print("Timing:")
    print("  Note: build and dictionary task timings may overlap.")
    for name, elapsed in timings:
        print(f"  {name:<30} {format_duration(elapsed)}")
    print(f"  {'Total':<30} {format_duration(total_elapsed)}")


def choose_cmake_generator(
    generator: str | None,
    platform: str | None,
) -> tuple[str | None, str | None]:
    """Choose CMake generator/platform from explicit args or environment."""
    if generator:
        return generator, platform

    env_generator = os.environ.get("CXXIME_CMAKE_GENERATOR") or os.environ.get("CMAKE_GENERATOR")
    env_platform = os.environ.get("CXXIME_CMAKE_PLATFORM") or os.environ.get("CMAKE_GENERATOR_PLATFORM")
    return env_generator, platform or env_platform


def is_single_config_generator(generator: str | None) -> bool:
    """Return true for generators that use CMAKE_BUILD_TYPE."""
    if not generator:
        return False
    return any(generator == name or generator.startswith(name + " ") for name in SINGLE_CONFIG_GENERATORS)


def supports_cmake_platform(generator: str | None) -> bool:
    """Return true for generators that support CMake -A platform selection."""
    return generator is None or generator.startswith("Visual Studio")


def is_child_path(parent: str, child: str) -> bool:
    """Return true if child is strictly inside parent."""
    parent_abs = os.path.normcase(os.path.abspath(parent))
    child_abs = os.path.normcase(os.path.abspath(child))
    try:
        return os.path.commonpath([parent_abs, child_abs]) == parent_abs and child_abs != parent_abs
    except ValueError:
        return False


def recreate_build_dir(build_dir: str) -> None:
    """Recreate a package build directory from scratch."""
    if not is_child_path(ROOT, build_dir):
        print(f"  ERROR: refusing to delete build directory outside repository: {build_dir}", file=sys.stderr)
        sys.exit(1)

    if os.path.exists(build_dir):
        print(f"Recreating build directory: {build_dir}")
        shutil.rmtree(build_dir)
    else:
        print(f"Creating build directory: {build_dir}")


def read_cmake_cache_bool(build_dir: str, name: str) -> bool | None:
    """Read a BOOL entry from an existing CMake cache."""
    cache_path = os.path.join(build_dir, "CMakeCache.txt")
    try:
        with open(cache_path, encoding="utf-8", errors="replace") as cache:
            prefix = f"{name}:BOOL="
            for line in cache:
                if line.startswith(prefix):
                    value = line[len(prefix):].strip().upper()
                    if value in {"1", "ON", "TRUE", "YES", "Y"}:
                        return True
                    if value in {"0", "OFF", "FALSE", "NO", "N", "IGNORE", "NOTFOUND"}:
                        return False
                    return None
    except OSError:
        return None
    return None


def verify_prebuilt_host_mode(build_dir: str, host_diagnostics: bool) -> None:
    """Reject --skip-build directories configured for the other package mode."""
    expected = {
        "CXXIME_ENABLE_HOST_DIAGNOSTICS": host_diagnostics,
        "CXXIME_BUILD_HOST_PROBE": host_diagnostics,
    }
    for name, expected_value in expected.items():
        actual_value = read_cmake_cache_bool(build_dir, name)
        if actual_value is None:
            print(
                f"  ERROR: cannot verify {name} in {build_dir}; rebuild without --skip-build.",
                file=sys.stderr,
            )
            sys.exit(1)
        if actual_value != expected_value:
            requested = "ON" if expected_value else "OFF"
            actual = "ON" if actual_value else "OFF"
            print(
                f"  ERROR: {build_dir} has {name}={actual}, "
                f"requested package requires {requested}.",
                file=sys.stderr,
            )
            sys.exit(1)


def build(
    build_dir: str,
    config: str,
    skip_tests: bool = False,
    skip_tools: bool = False,
    generator: str | None = None,
    platform: str | None = None,
    target: str | None = None,
    jobs: int = 1,
    host_diagnostics: bool = False,
) -> None:
    """Configure and build the project."""
    print(f"Building {config} with PRODUCTION=ON...")
    jobs = max(1, jobs)

    generator, platform = choose_cmake_generator(generator, platform)
    if generator:
        print(f"  CMake generator: {generator}")
    single_config = is_single_config_generator(generator)
    if single_config:
        platform = None
    elif supports_cmake_platform(generator) and not platform:
        platform = "x64"
    if platform:
        print(f"  CMake platform:  {platform}")

    recreate_build_dir(build_dir)
    os.makedirs(build_dir, exist_ok=True)

    cmake_args = [
        "cmake", "-S", ROOT, "-B", build_dir,
        "-DCXXIME_PRODUCTION_BUILD=ON",
    ]
    if generator:
        cmake_args.extend(["-G", generator])
    if platform:
        cmake_args.extend(["-A", platform])
    cmake_args.append(f"-DCXXIME_BUILD_TESTS={'OFF' if skip_tests else 'ON'}")
    cmake_args.append(f"-DCXXIME_BUILD_TOOLS={'OFF' if skip_tools else 'ON'}")
    cmake_args.append(
        f"-DCXXIME_ENABLE_HOST_DIAGNOSTICS={'ON' if host_diagnostics else 'OFF'}"
    )
    cmake_args.append(
        f"-DCXXIME_BUILD_HOST_PROBE={'ON' if host_diagnostics else 'OFF'}"
    )
    if single_config:
        cmake_args.append(f"-DCMAKE_BUILD_TYPE={config}")
        build_cmd = ["cmake", "--build", build_dir]
    else:
        build_cmd = ["cmake", "--build", build_dir, "--config", config]
    if target:
        build_cmd.extend(["--target", target])
    build_cmd.extend(["--parallel", str(jobs)])
    print(f"  Build parallelism: {jobs}")

    run(cmake_args)
    run(build_cmd)


def build_x86_platform_modules(
    build_dir: str,
    config: str,
    generator: str | None = None,
    jobs: int = 1,
    host_diagnostics: bool = False,
) -> None:
    """Build the 32-bit in-process IME modules with a platform-aware generator."""
    generator, _ = choose_cmake_generator(generator, None)
    if generator and is_single_config_generator(generator):
        print(
            "  ERROR: x86 IME packaging requires a platform-aware CMake generator "
            "(for example Visual Studio) or an explicit 32-bit native build.",
            file=sys.stderr,
        )
        print(
            "  Use --generator \"Visual Studio 17 2022\" --platform x64 for release "
            "packaging, or --skip-x86-tsf --skip-nsis for local dist-only checks.",
            file=sys.stderr,
        )
        sys.exit(1)
    if generator and not supports_cmake_platform(generator):
        print(
            f"  ERROR: x86 IME packaging requires a generator with -A support, got: {generator}",
            file=sys.stderr,
        )
        sys.exit(1)

    build(
        build_dir,
        config,
        skip_tests=True,
        skip_tools=True,
        generator=generator,
        platform="Win32",
        target="zhiyi-platform-modules",
        jobs=jobs,
        host_diagnostics=host_diagnostics,
    )


def clean_dist(keep_data: bool = False) -> None:
    """Remove and recreate dist directory.

    If keep_data is True, preserves dist/data/ contents (for --skip-dict).
    """
    if keep_data and os.path.exists(DIST_DIR):
        # Remove everything except data/
        for entry in os.listdir(DIST_DIR):
            if entry == "data":
                continue
            path = os.path.join(DIST_DIR, entry)
            if os.path.isdir(path):
                shutil.rmtree(path)
            else:
                os.remove(path)
    else:
        if os.path.exists(DIST_DIR):
            shutil.rmtree(DIST_DIR)
    os.makedirs(os.path.join(DIST_DIR, "data"), exist_ok=True)


def copy_binary(build_dir: str, config: str, subdir: str, name: str) -> None:
    """Copy one built binary to dist."""
    src = built_binary_path(build_dir, config, subdir, name)
    shutil.copy2(src, os.path.join(DIST_DIR, name))
    print(f"  {name}")


def built_binary_path(build_dir: str, config: str, subdir: str, name: str) -> str:
    """Resolve a binary from a single- or multi-config CMake build."""
    target_dir = os.path.join(build_dir, subdir)
    src_dir = os.path.join(target_dir, config)
    if not os.path.isdir(src_dir):
        src_dir = target_dir
    src = os.path.join(src_dir, name)
    if not os.path.isfile(src):
        print(f"  ERROR: binary not found: {src}", file=sys.stderr)
        sys.exit(1)
    return src


def copy_binaries(
    build_dir: str,
    x86_build_dir: str,
    config: str,
    include_x86_modules: bool,
    host_diagnostics: bool,
) -> None:
    """Copy built binaries to dist."""
    copy_binary(build_dir, config, "tsf", "zhiyi_tsf_x64.dll")
    copy_binary(build_dir, config, "legacy_ime", "zhiyi_ime_x64.ime")
    if host_diagnostics:
        copy_binary(
            build_dir,
            config,
            "diagnostics/host_takeover/probe",
            "zhiyi-ime-host-probe-x64.exe",
        )
    if include_x86_modules:
        copy_binary(x86_build_dir, config, "tsf", "zhiyi_tsf_x86.dll")
        copy_binary(x86_build_dir, config, "legacy_ime", "zhiyi_ime_x86.ime")
        if host_diagnostics:
            copy_binary(
                x86_build_dir,
                config,
                "diagnostics/host_takeover/probe",
                "zhiyi-ime-host-probe-x86.exe",
            )
    copy_binary(build_dir, config, "resource", "zhiyi-resources.dll")
    copy_binary(build_dir, config, "server", "zhiyi-server.exe")
    copy_binary(build_dir, config, "settings", "zhiyi-settings.exe")
    copy_binary(
        build_dir,
        config,
        "installer",
        "zhiyi-installer-helper.exe",
    )


def copy_config(host_diagnostics: bool) -> None:
    """Copy config and themes to dist/data."""
    data_dir = os.path.join(DIST_DIR, "data")
    default_json = os.path.join(data_dir, "default.json")
    shutil.copy2(os.path.join(DATA, "default.json"), default_json)
    if host_diagnostics:
        with open(default_json, encoding="utf-8") as f:
            config = json.load(f)
        config.setdefault("diagnostics", {})["trace_mode"] = "normal"
        with open(default_json, "w", encoding="utf-8", newline="\n") as f:
            json.dump(config, f, ensure_ascii=False, indent=4)
            f.write("\n")
    print("  default.json")

    presets = os.path.join(DATA, "settings_presets.json")
    if os.path.isfile(presets):
        shutil.copy2(presets, data_dir)
        print("  settings_presets.json")
    else:
        print("  WARNING: settings_presets.json not found")

    themes = os.path.join(DATA, "themes.json")
    if os.path.isfile(themes):
        shutil.copy2(themes, data_dir)
        print("  themes.json")
    else:
        print("  WARNING: themes.json not found")

    punct = os.path.join(DATA, "punctuation.json")
    if os.path.isfile(punct):
        shutil.copy2(punct, data_dir)
        print("  punctuation.json")
    else:
        print("  WARNING: punctuation.json not found")


def prepare_dictionaries(workers: int) -> None:
    """Prepare the Pinyin and Wubi runtime dictionary bundle."""
    from prepare_dictionary_bundle import prepare_dictionary_bundle

    data_dir = DATA
    output_dir = os.path.join(DIST_DIR, "data")

    generated = prepare_dictionary_bundle(
        data_dir,
        output_dir,
        workers=workers,
        defer_topn_conversion=True,
    )
    if not generated:
        print("  ERROR: No dictionary files generated.", file=sys.stderr)
        print("  Need .dict.db or .dict.db.zip in data/.", file=sys.stderr)
        sys.exit(1)


def write_dictionary_manifest_for_existing_data() -> None:
    """Rebuild manifest for existing dist/data dictionary files."""
    from prepare_dictionary_bundle import write_dictionary_manifest

    data_dir = os.path.join(DIST_DIR, "data")
    manifest = write_dictionary_manifest(data_dir)
    print(f"  {os.path.basename(manifest)}")


def finalize_topn_data(build_dir: str, config: str) -> None:
    """Produce the only runtime Top-N format and refresh its manifest entry."""
    from prepare_dictionary_bundle import finalize_topn_index

    builder = built_binary_path(
        build_dir,
        config,
        "tools/topn_index",
        "topn_builder.exe",
    )
    finalize_topn_index(os.path.join(DIST_DIR, "data"), builder)
    write_dictionary_manifest_for_existing_data()


def verify_dictionary_bundle() -> None:
    """Verify the generated runtime dictionary bundle."""
    script = os.path.join(SCRIPTS, "verify_dictionary_bundle.py")
    data_dir = os.path.join(DIST_DIR, "data")
    run([sys.executable, script, "--data-dir", data_dir])


def check_debug_crt(config: str) -> None:
    """Ensure Release builds don't link Debug CRT."""
    if config == "Debug":
        print("  Debug CRT expected for Debug build, skipping check.")
        return

    print("  Checking for Debug CRT dependencies...")
    dumpbin = shutil.which("dumpbin")
    if not dumpbin:
        print("  dumpbin not available; skipping optional Debug CRT dependency check.")
        return

    debug_crts = {"ucrtbased.dll", "vcruntimed.dll"}
    has_debug = False

    for name in [
        "zhiyi_tsf_x64.dll",
        "zhiyi_tsf_x86.dll",
        "zhiyi_ime_x64.ime",
        "zhiyi_ime_x86.ime",
        "zhiyi-ime-host-probe-x64.exe",
        "zhiyi-ime-host-probe-x86.exe",
        "zhiyi-resources.dll",
        "zhiyi-server.exe",
        "zhiyi-settings.exe",
        "zhiyi-installer-helper.exe",
    ]:
        path = os.path.join(DIST_DIR, name)
        if not os.path.isfile(path):
            continue
        try:
            result = subprocess.run(
                [dumpbin, "/dependents", path],
                capture_output=True,
                text=True,
                errors="replace",
            )
        except FileNotFoundError:
            print("  dumpbin not available; skipping optional Debug CRT dependency check.")
            return
        if result.returncode != 0:
            print(f"  WARNING: dumpbin failed for {name}, skipping CRT check for this file.")
            continue
        output = (result.stdout or "") + "\n" + (result.stderr or "")
        for crt in debug_crts:
            if crt.lower() in output.lower():
                print(f"  ERROR: {name} links to Debug CRT ({crt})", file=sys.stderr)
                has_debug = True

    if has_debug:
        print("  ERROR: Release build must not depend on Debug CRT.", file=sys.stderr)
        sys.exit(1)
    print("  No Debug CRT dependencies.")


def count_logs_in_functions(path: str, function_names: list[str]) -> int:
    with open(path, encoding="utf-8") as f:
        lines = f.readlines()

    count = 0
    for function_name in function_names:
        in_signature = False
        in_body = False
        brace_depth = 0
        for line in lines:
            if not in_signature and function_name not in line:
                continue
            if not in_signature:
                in_signature = True

            if in_signature and not in_body and "{" in line:
                in_body = True

            if in_body:
                stripped = line.strip()
                if stripped.startswith("CXXIME_LOG") and not stripped.startswith("//"):
                    count += 1
                brace_depth += line.count("{") - line.count("}")
                if brace_depth <= 0:
                    in_signature = False
                    in_body = False
                    brace_depth = 0

    return count


def check_hot_path_logs() -> None:
    """Warn about CXXIME_LOG calls in hot paths."""
    print("  Checking for high-frequency CXXIME_LOG in hot paths...")
    hot_file_checks = [
        (os.path.join(ROOT, "engine", "src", "pinyin_translator.cc"), None),
        (
            os.path.join(ROOT, "engine", "src", "dict.cc"),
            [
                "Dict::lookup(",
                "Dict::lookup_by_syllables(",
                "Dict::lookup_by_ids(",
                "Dict::lookup_user_exact(",
                "Dict::lookup_user_prefix(",
                "Dict::lookup_user_indexed(",
                "Dict::has_prefix(",
            ],
        ),
        (os.path.join(ROOT, "ipc", "src", "ipc_server.cc"), None),
    ]

    for path, functions in hot_file_checks:
        if not os.path.isfile(path):
            continue
        if functions:
            count = count_logs_in_functions(path, functions)
        else:
            with open(path, encoding="utf-8") as f:
                content = f.read()
            count = len(re.findall(r"^\s*CXXIME_LOG", content, re.MULTILINE))
        if count > 0:
            basename = os.path.basename(path)
            print(f"  WARNING: {basename}: {count} CXXIME_LOG call(s) in hot path")


def check_diagnostics_config(host_diagnostics: bool) -> None:
    """Check the packaged diagnostics mode and log rotation settings."""
    print("  Checking diagnostics config...")
    default_json = os.path.join(DIST_DIR, "data", "default.json")
    if os.path.isfile(default_json):
        with open(default_json, encoding="utf-8") as f:
            cfg = json.load(f)
        diag = cfg.get("diagnostics", {})
        required = ("trace_mode", "log_max_size", "log_max_files")
        if isinstance(diag, dict) and all(k in diag for k in required):
            expected_mode = "normal" if host_diagnostics else "off"
            if diag["trace_mode"] != expected_mode:
                raise RuntimeError(
                    f"Packaged diagnostics trace_mode must be {expected_mode!r}, "
                    f"got {diag['trace_mode']!r}"
                )
            print(f"  Diagnostics trace mode: {expected_mode}.")
            return
    raise RuntimeError("default.json does not contain diagnostics log rotation config")


def verify_package_layout(include_x86_modules: bool, host_diagnostics: bool) -> None:
    """Run static dist package preflight checks."""
    script = os.path.join(SCRIPTS, "verify_package.py")
    cmd = [sys.executable, script, "--dist-dir", DIST_DIR]
    if not include_x86_modules:
        cmd.append("--allow-missing-x86")
    if host_diagnostics:
        cmd.append("--host-diag")
    run(cmd)


def copy_installer_scripts(config: str, host_diagnostics: bool) -> None:
    """Copy install/uninstall scripts and NSIS template."""
    files = [
        "install.bat", "uninstall.bat",
        "install.ps1", "uninstall.ps1",
        "collect_diagnostics.ps1",
    ]
    for fn in files:
        src = os.path.join(SCRIPTS, fn)
        if os.path.isfile(src):
            dest = os.path.join(DIST_DIR, fn)
            if fn == "collect_diagnostics.ps1":
                with open(src, encoding="utf-8") as f:
                    content = f.read()
                marker = 'package_version = "development"'
                replacement = f'package_version = "{VERSION}"'
                if content.count(marker) != 1:
                    raise RuntimeError("collect_diagnostics.ps1 package version marker is missing")
                with open(dest, "w", encoding="utf-8", newline="") as f:
                    f.write(content.replace(marker, replacement))
            else:
                shutil.copy2(src, dest)
            print(f"  {fn}")

    if host_diagnostics:
        host_trace_exporter = os.path.join(
            HOST_TAKEOVER_DIAGNOSTICS, "scripts", "export_host_trace.ps1"
        )
        shutil.copy2(host_trace_exporter, DIST_DIR)
        print("  export_host_trace.ps1")

    # NSIS template and function includes.
    shutil.copy2(os.path.join(SCRIPTS, "zhiyi-setup.nsi"), DIST_DIR)
    shutil.copy2(
        os.path.join(ROOT, "resource", "zhiyi.ico"),
        os.path.join(DIST_DIR, "zhiyi.ico"),
    )
    print("  zhiyi.ico")
    nsis_source_dir = os.path.join(SCRIPTS, "nsis")
    nsis_dist_dir = os.path.join(DIST_DIR, "nsis")
    os.makedirs(nsis_dist_dir, exist_ok=True)
    for entry in sorted(os.scandir(nsis_source_dir), key=lambda item: item.name):
        if entry.is_file() and entry.name.endswith(".nsh"):
            shutil.copy2(entry.path, nsis_dist_dir)

    # Project license for the NSIS license page.
    license_src = os.path.join(ROOT, "LICENSE")
    if os.path.isfile(license_src):
        shutil.copy2(license_src, os.path.join(DIST_DIR, "license.txt"))
        print("  license.txt (from LICENSE)")
    else:
        print("  WARNING: LICENSE not found, NSIS may fail")

    notices_src = os.path.join(ROOT, "THIRD_PARTY_NOTICES.txt")
    shutil.copy2(notices_src, os.path.join(DIST_DIR, "THIRD_PARTY_NOTICES.txt"))
    print("  THIRD_PARTY_NOTICES.txt")

    data_license_dir = os.path.join(DIST_DIR, "licenses")
    os.makedirs(data_license_dir, exist_ok=True)
    for data_license_name in ("rime-ice-GPL-3.0.txt", "miniz-MIT.txt"):
        data_license_src = os.path.join(DATA, "licenses", data_license_name)
        shutil.copy2(data_license_src, os.path.join(data_license_dir, data_license_name))
        print(f"  licenses/{data_license_name}")


def build_nsis(
    config: str,
    output_dir: str,
    fast: bool = False,
    host_diagnostics: bool = False,
) -> None:
    """Run makensis to create the installer."""
    # Find makensis
    makensis = shutil.which("makensis")
    if makensis is None:
        # Try registry paths
        import winreg
        for key_path in [
            r"SOFTWARE\WOW6432Node\NSIS",
            r"SOFTWARE\NSIS",
        ]:
            try:
                key = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, key_path)
                path, _ = winreg.QueryValueEx(key, "")
                candidate = os.path.join(path, "makensis.exe")
                if os.path.isfile(candidate):
                    makensis = candidate
                    break
            except OSError:
                continue

    if makensis is None:
        raise RuntimeError("makensis was not found; install NSIS 3.x or add it to PATH")

    print(f"  Using NSIS: {makensis}")
    nsi_file = os.path.join(DIST_DIR, "zhiyi-setup.nsi")
    cmd = [
        makensis,
        "/INPUTCHARSET",
        "UTF8",
        f"/DVERSION={VERSION}",
        f"/DVERSION_NUMERIC={VERSION_NUMERIC}",
    ]
    if fast:
        cmd.append("/DFAST")
    if host_diagnostics:
        cmd.append("/DHOST_DIAGNOSTICS")
    cmd.append(nsi_file)
    run(cmd, cwd=DIST_DIR)

    # Move installer to output
    os.makedirs(output_dir, exist_ok=True)
    suffix = "-host-diag" if host_diagnostics else ""
    installer = os.path.join(DIST_DIR, f"zhiyi-v{VERSION}{suffix}-setup.exe")
    dest = os.path.join(output_dir, f"zhiyi-v{VERSION}{suffix}-setup.exe")
    if os.path.isfile(installer):
        shutil.move(installer, dest)
        print(f"  Installer created: {dest}")
    else:
        raise RuntimeError("NSIS completed without producing the expected installer")


def print_summary(config: str, include_x86_modules: bool, host_diagnostics: bool) -> None:
    """Print final distribution summary."""
    print(f"\n{'=' * 60}")
    print("=== Packaging complete ===")
    print(f"{'=' * 60}")
    print(f"\nConfiguration: {config}")
    print(f"Distribution:  {DIST_DIR}")
    print()
    print("Contents:")
    print("  zhiyi_tsf_x64.dll       64-bit TSF text service DLL")
    print("  zhiyi_ime_x64.ime       64-bit legacy IMM IME module")
    if host_diagnostics:
        print("  zhiyi-ime-host-probe-x64.exe 64-bit host takeover Probe")
    if include_x86_modules:
        print("  zhiyi_tsf_x86.dll       32-bit TSF text service DLL")
        print("  zhiyi_ime_x86.ime       32-bit legacy IMM IME module")
        if host_diagnostics:
            print("  zhiyi-ime-host-probe-x86.exe 32-bit host takeover Probe")
    print("  zhiyi-resources.dll     Stable input profile resources")
    print("  zhiyi-server.exe        Background server process")
    print("  zhiyi-settings.exe      Configuration editor")
    print("  zhiyi-installer-helper.exe  Installer lock preflight")
    print("  data/")
    print("    default.json           Default configuration")
    print("    settings_presets.json  Settings UI presets")
    print("    themes.json            Color themes")
    print("    punctuation.json       Punctuation mapping")
    print("    symbols.json           Symbol categories")
    print("    dictionary_manifest.json Dictionary bundle manifest")
    print("    pinyin.dict.bin        Pinyin binary dictionary (runtime)")
    print("    pinyin.dict.idx        Pinyin syllable index (runtime)")
    for filename in PINYIN_SPELLING_FILES:
        print(f"    {filename:<26} Pinyin spelling trie (runtime)")
    print("    pinyin.topn.bin        Short code cache (runtime)")
    print("    pinyin.reverse.idx     Pinyin Settings reverse index")
    print("    wubi86.dict.bin        Wubi binary dictionary")
    print("    wubi86.dict.idx        Wubi complete-prefix candidate index")
    print("    wubi86.reverse.idx     Wubi Settings reverse index")
    optional_scripts = [
        ("install.bat", "Installer helper"),
        ("uninstall.bat", "Uninstaller helper"),
        ("install.ps1", "PowerShell installer helper"),
        ("uninstall.ps1", "PowerShell uninstaller helper"),
    ]
    for filename, description in optional_scripts:
        if os.path.isfile(os.path.join(DIST_DIR, filename)):
            print(f"  {filename:<24} {description}")
    print("  collect_diagnostics.ps1  Diagnostics collector")
    if host_diagnostics:
        print("  export_host_trace.ps1    Host trace exporter")
    print("  zhiyi-setup.nsi         NSIS script")
    print("  license.txt              License")
    print("  THIRD_PARTY_NOTICES.txt  Third-party notices")
    print("  licenses/                Third-party data licenses")


def main():
    parser = argparse.ArgumentParser(
        description="Zhiyi IME packaging - build, prepare dicts, create installer"
    )
    parser.add_argument(
        "--debug", action="store_true",
        help="Build Debug configuration (default: Release)",
    )
    parser.add_argument(
        "--build-dir", default=DEFAULT_BUILD_DIR,
        help="CMake build directory for package builds (default: build-package)",
    )
    parser.add_argument(
        "--x86-build-dir", default=DEFAULT_X86_BUILD_DIR,
        help="CMake build directory for 32-bit IME module builds (default: build-package-x86)",
    )
    parser.add_argument(
        "--output-dir", default=DEFAULT_OUTPUT_DIR,
        help="Installer output directory (default: ../output)",
    )
    parser.add_argument(
        "--skip-build", action="store_true",
        help="Skip cmake build step (binaries already built)",
    )
    parser.add_argument(
        "--skip-dict", action="store_true",
        help="Skip dictionary generation (use existing dist/data/ files)",
    )
    parser.add_argument(
        "--fast", action="store_true",
        help="Fast NSIS build: skip compression for large data files",
    )
    parser.add_argument(
        "--skip-nsis", action="store_true",
        help="Skip NSIS installer build (only update dist/ contents)",
    )
    parser.add_argument(
        "--skip-x86-tsf", action="store_true",
        help="Skip 32-bit TSF/legacy IME builds for local dist-only checks (requires --skip-nsis)",
    )
    parser.add_argument(
        "--skip-tests", action="store_true", default=True,
        help="Skip building unit tests (default: on)",
    )
    parser.add_argument(
        "--with-tests", action="store_false", dest="skip_tests",
        help="Build unit tests",
    )
    parser.add_argument(
        "--skip-tools", action="store_true", default=True,
        help="Skip building development tools (default: on)",
    )
    parser.add_argument(
        "--with-tools", action="store_false", dest="skip_tools",
        help="Build development tools",
    )
    parser.add_argument(
        "--host-diag", action="store_true",
        help="Build and package host diagnostics and the IME host Probe",
    )
    parser.add_argument(
        "--generator",
        help="CMake generator (default: let CMake choose, or use CMAKE_GENERATOR)",
    )
    parser.add_argument(
        "--platform",
        help="CMake platform for generators that support -A, such as Visual Studio",
    )
    parser.add_argument(
        "-j", "--jobs", type=int, default=default_job_count(),
        help="Total CMake build job budget (default: CPU count)",
    )
    parser.add_argument(
        "--dict-workers", type=int, default=min(2, default_job_count()),
        help="Dictionary preparation workers (default: 2, capped by CPU count)",
    )
    args = parser.parse_args()

    if args.skip_x86_tsf and not args.skip_nsis:
        parser.error("--skip-x86-tsf requires --skip-nsis because the installer requires both bitnesses")
    if args.jobs < 1:
        parser.error("--jobs must be >= 1")
    if args.dict_workers < 1:
        parser.error("--dict-workers must be >= 1")

    config = "Debug" if args.debug else "Release"
    build_dir = os.path.abspath(args.build_dir)
    x86_build_dir = os.path.abspath(args.x86_build_dir)
    output_dir = os.path.abspath(args.output_dir)
    if not args.skip_x86_tsf and os.path.normcase(build_dir) == os.path.normcase(x86_build_dir):
        parser.error("--x86-build-dir must be different from --build-dir")
    if args.skip_build:
        verify_prebuilt_host_mode(build_dir, args.host_diag)
        if not args.skip_x86_tsf:
            verify_prebuilt_host_mode(x86_build_dir, args.host_diag)

    print(f"=== Zhiyi IME Packager v{VERSION} ({config}) ===")
    total_start = time.perf_counter()
    timings: list[tuple[str, float]] = []

    # 1. Clean dist before parallel tasks so dictionaries can be produced while builds run.
    step("[1/9] Preparing distribution directory...")
    stage_start = time.perf_counter()
    clean_dist(keep_data=args.skip_dict)
    timings.append(("prepare distribution", time.perf_counter() - stage_start))

    build_futures: list[tuple[str, concurrent.futures.Future]] = []
    dict_future: concurrent.futures.Future | None = None
    build_count = 0 if args.skip_build else (1 if args.skip_x86_tsf else 2)
    package_workers = build_count + (0 if args.skip_dict else 1)
    package_workers = max(1, package_workers)

    with concurrent.futures.ThreadPoolExecutor(max_workers=package_workers) as executor:
        if args.skip_build:
            step("[2/9] Skipping build (--skip-build).")
            print(f"  Using existing binaries in {build_dir}.")
        else:
            step("[2/9] Starting clean builds...")
            x64_jobs = args.jobs
            x86_jobs = args.jobs
            if not args.skip_x86_tsf:
                x64_jobs = max(1, (args.jobs + 1) // 2)
                x86_jobs = max(1, args.jobs // 2)
            print(f"  Build job budget: {args.jobs} (x64={x64_jobs}"
                  f"{', x86=' + str(x86_jobs) if not args.skip_x86_tsf else ''})")
            build_futures.append((
                "x64 product build",
                executor.submit(
                    timed_call,
                    build,
                    build_dir,
                    config,
                    skip_tests=args.skip_tests,
                    skip_tools=args.skip_tools,
                    generator=args.generator,
                    platform=args.platform,
                    jobs=x64_jobs,
                    host_diagnostics=args.host_diag,
                ),
            ))
            if not args.skip_x86_tsf:
                build_futures.append((
                    "x86 platform module build",
                    executor.submit(
                        timed_call,
                        build_x86_platform_modules,
                        x86_build_dir,
                        config,
                        generator=args.generator,
                        jobs=x86_jobs,
                        host_diagnostics=args.host_diag,
                    ),
                ))

        if args.skip_dict:
            step("[3/9] Skipping dictionary generation (--skip-dict).")
            print("  Using existing files in dist/data/.")
        else:
            step("[3/9] Starting dictionary preparation...")
            print(f"  Dictionary workers: {args.dict_workers}")
            dict_future = executor.submit(timed_call, prepare_dictionaries, args.dict_workers)

        # 2. Wait for build output before copying binaries.
        step("[4/9] Waiting for build outputs...")
        for name, future in build_futures:
            timings.append((name, wait_task(name, future)))

        # 3. Copy files while dictionary preparation continues.
        step("[5/9] Preparing package files...")
        stage_start = time.perf_counter()
        print("  Copying binaries...")
        copy_binaries(
            build_dir,
            x86_build_dir,
            config,
            include_x86_modules=not args.skip_x86_tsf,
            host_diagnostics=args.host_diag,
        )

        print("  Copying config and themes...")
        copy_config(host_diagnostics=args.host_diag)
        timings.append(("copy package files", time.perf_counter() - stage_start))

        if dict_future is not None:
            print("  Waiting for dictionaries...")
            timings.append((
                "dictionary preparation",
                wait_task("dictionary preparation", dict_future),
            ))

        stage_start = time.perf_counter()
        finalize_topn_data(build_dir, config)
        timings.append(("Top-N finalization", time.perf_counter() - stage_start))

    # 4. Verification
    step("[6/9] Verifying data files...")
    stage_start = time.perf_counter()
    verify_dictionary_bundle()
    check_debug_crt(config)
    check_hot_path_logs()
    check_diagnostics_config(host_diagnostics=args.host_diag)
    timings.append(("verify data and checks", time.perf_counter() - stage_start))

    # 5. Installer scripts
    step("[7/9] Copying installer scripts...")
    stage_start = time.perf_counter()
    copy_installer_scripts(config, host_diagnostics=args.host_diag)
    write_install_payload(
        DIST_DIR,
        include_x86_modules=not args.skip_x86_tsf,
        host_diagnostics=args.host_diag,
    )
    print("  install-manifest.json")
    print("  install_payload.nsh")
    timings.append(("copy installer scripts", time.perf_counter() - stage_start))

    # 6. Package layout
    step("[8/9] Verifying package layout...")
    stage_start = time.perf_counter()
    verify_package_layout(
        include_x86_modules=not args.skip_x86_tsf,
        host_diagnostics=args.host_diag,
    )
    timings.append(("verify package layout", time.perf_counter() - stage_start))

    # 7. NSIS
    if args.skip_nsis:
        step("[9/9] Skipping NSIS installer (--skip-nsis).")
    else:
        step("[9/9] Building NSIS installer...")
        stage_start = time.perf_counter()
        build_nsis(
            config,
            output_dir,
            fast=args.fast,
            host_diagnostics=args.host_diag,
        )
        timings.append(("build nsis installer", time.perf_counter() - stage_start))

    print_summary(
        config,
        include_x86_modules=not args.skip_x86_tsf,
        host_diagnostics=args.host_diag,
    )
    print_timing_summary(timings, time.perf_counter() - total_start)


if __name__ == "__main__":
    main()
