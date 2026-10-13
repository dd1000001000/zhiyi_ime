"""Builds latest.json for a release and signs it (see update/include/cxxime/update.h).

  python scripts/make_release.py --version 1.6.0 \
      --installer dist/zhiyu-v1.6.0-setup.exe \
      --notes-zh dist/notes.zh-CN.md --notes-en dist/notes.en-US.md

Writes latest.json + latest.json.sig next to the installer (or --out). Then upload the
installer (renamed to zhiyu-v<version>-setup.exe), latest.json and latest.json.sig as assets
of a release tagged `v<version>` in https://github.com/w3c0929/zhiyi_ime.

The manifest must satisfy update_manifest.cc: the file name is exactly
zhiyu-v<version>-setup.exe and the url is <base>v<version>/<file>. The signature uses the same
key as scripts/update_signing.py (update-signing-key.pem by default).
"""
import argparse
import datetime
import hashlib
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import update_signing

DEFAULT_BASE = "https://github.com/w3c0929/zhiyi_ime/releases/download/"


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as file:
        for chunk in iter(lambda: file.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description="Build and sign latest.json.")
    parser.add_argument("--version", required=True, help="e.g. 1.6.0")
    parser.add_argument("--installer", required=True, help="the setup .exe")
    parser.add_argument("--base", default=DEFAULT_BASE, help="release download prefix")
    parser.add_argument("--key", default=update_signing.DEFAULT_KEY)
    parser.add_argument("--out", default=None, help="where latest.json goes (default: the installer's folder)")
    parser.add_argument("--notes-zh", default=None)
    parser.add_argument("--notes-en", default=None)
    args = parser.parse_args()

    file_name = f"zhiyu-v{args.version}-setup.exe"
    if os.path.basename(args.installer) != file_name:
        print(f"warning: the installer should be named {file_name} (rename it before uploading)",
              file=sys.stderr)

    manifest = {
        "version": args.version,
        "file": file_name,
        "url": args.base + f"v{args.version}/" + file_name,
        "size": os.path.getsize(args.installer),
        "sha256": sha256(args.installer),
        "published": datetime.date.today().isoformat(),
        "notes": {},
    }
    for language, path in (("zh-CN", args.notes_zh), ("en-US", args.notes_en)):
        if path:
            with open(path, encoding="utf-8") as file:
                manifest["notes"][language] = file.read()

    out_dir = args.out or os.path.dirname(os.path.abspath(args.installer))
    manifest_path = os.path.join(out_dir, "latest.json")
    with open(manifest_path, "w", encoding="utf-8") as file:
        json.dump(manifest, file, ensure_ascii=False, indent=4)
    print(f"wrote {manifest_path}")
    print(f"  version {manifest['version']}  {manifest['file']}  {manifest['size']} bytes")
    return update_signing.sign(args.key, [manifest_path])


if __name__ == "__main__":
    sys.exit(main())
