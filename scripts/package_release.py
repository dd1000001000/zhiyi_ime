"""Assembles a release folder (and a zip) from a build plus a reference installation.

  python scripts/package_release.py --reference "D:\\scoop\\...\\1.5.4.xxxx" \
      --out dist --version 1.6.0

The build tree holds only the binaries; the data (dictionaries, themes, ui strings), the model
folders, the runtime DLLs (onnxruntime, DirectML, the MSVC runtimes) and the licenses live in an
installed copy. So this copies the reference installation, overwrites it with the freshly built
programs, and zips the result into dist/zhiyu-v<version>.zip.

It does not build the NSIS setup.exe: upstream's zhiyu-setup.nsi is not published here. Either
point makensis at your own installer script over the produced folder, or ship the zip. After the
setup.exe exists, run scripts/make_release.py to build and sign latest.json.
"""
import argparse
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Built programs, relative to the build tree; both architectures where they exist.
BUILT = {
    "x64": [
        (r"server\Release\zhiyu-server.exe", "zhiyu-server.exe"),
        (r"settings\Release\zhiyu-settings.exe", "zhiyu-settings.exe"),
        (r"tsf\Release\zhiyu_tsf_x64.dll", "zhiyu_tsf_x64.dll"),
        (r"legacy_ime\Release\zhiyu_ime_x64.ime", "zhiyu_ime_x64.ime"),
        (r"resource\Release\zhiyu-resources.dll", "zhiyu-resources.dll"),
        (r"installer\Release\zhiyu-installer-helper.exe", "zhiyu-installer-helper.exe"),
    ],
    "x86": [
        (r"tsf\Release\zhiyu_tsf_x86.dll", "zhiyu_tsf_x86.dll"),
        (r"legacy_ime\Release\zhiyu_ime_x86.ime", "zhiyu_ime_x86.ime"),
    ],
}


def copy_reference(reference, out_dir):
    shutil.copytree(reference, out_dir, dirs_exist_ok=True, ignore=shutil.ignore_patterns(
        "*.replaced", "uninstall.exe", "install-manifest.json", ".zhiyu-install-complete"))


def overlay_builds(reference, out_dir, build_x64, build_x86):
    written = []
    for arch, build in (("x64", build_x64), ("x86", build_x86)):
        if not build:
            continue
        for relative, name in BUILT[arch]:
            source = os.path.join(build, relative)
            if os.path.exists(source):
                shutil.copy2(source, os.path.join(out_dir, name))
                written.append(name)
            else:
                print(f"missing (skipped): {name} in {build}", file=sys.stderr)
    return written


# Text data the repository owns (ui strings, default config, themes, ...): the reference's copy
# is the upstream brand. Overwrite these with the repo's. Only the text config is listed — the
# repository also holds the dictionary *sources* (*.dict.db.zip) and build helpers, which must
# not go into the installer (they are already-compressed build inputs and just bloat it); the
# built .bin/.idx and the model folders stay from the reference.
DATA_OVERLAY = ("default.json", "ui.zh-CN.json", "ui.en-US.json", "themes.json",
                "punctuation.json", "symbols.json", "settings_presets.json",
                "english.words.tsv")


def overlay_repo_data(out_dir):
    source_dir = os.path.join(ROOT, "data")
    target_dir = os.path.join(out_dir, "data")
    written = []
    for name in DATA_OVERLAY:
        source = os.path.join(source_dir, name)
        if os.path.isfile(source):
            shutil.copy2(source, os.path.join(target_dir, name))
            written.append(name)
    return written


def main():
    parser = argparse.ArgumentParser(description="Assemble a release folder and zip.")
    parser.add_argument("--reference", required=True, help="an installed copy to take data from")
    parser.add_argument("--build", default=os.path.join(ROOT, "build-x64"))
    parser.add_argument("--x86-build", default=os.path.join(ROOT, "build-x86"))
    parser.add_argument("--out", default=os.path.join(ROOT, "dist"))
    parser.add_argument("--version", required=True)
    args = parser.parse_args()

    out_dir = os.path.join(args.out, f"zhiyu-v{args.version}")
    if os.path.exists(out_dir):
        shutil.rmtree(out_dir)
    os.makedirs(args.out, exist_ok=True)

    print(f"copying {args.reference} -> {out_dir}")
    copy_reference(args.reference, out_dir)
    print("overwriting built programs:")
    for name in overlay_builds(args.reference, out_dir, args.build, args.x86_build):
        print(f"  {name}")
    print("overwriting repository data:")
    for name in overlay_repo_data(out_dir):
        print(f"  data\\{name}")

    archive = os.path.join(args.out, f"zhiyu-v{args.version}")
    shutil.make_archive(archive, "zip", args.out, f"zhiyu-v{args.version}")
    print(f"\nfolder: {out_dir}\nzip:    {archive}.zip")
    print("\nNext: build the setup.exe (your NSIS script), then")
    print(f"  python scripts/make_release.py --version {args.version} --installer "
          f"<zhiyu-v{args.version}-setup.exe>")
    return 0


if __name__ == "__main__":
    sys.exit(main())
