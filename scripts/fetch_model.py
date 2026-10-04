"""Downloads the Laya model used by 知意输入法 into models/laya/.

  python scripts/fetch_model.py            # download the release asset and verify it
  python scripts/fetch_model.py --pack     # maintainers: zip models/laya/ for a release upload

The model (laya.int8g.onnx, ~335 MB) is too large for git; it is published as a GitHub release
asset. While the repository is private, set GITHUB_TOKEN (a token with read access to the
repository's contents) so the asset is fetched through the GitHub API.
It can also be rebuilt from bench/ (see bench/README.md: train, then export_int8g.py).
"""
import argparse
import hashlib
import io
import json
import os
import sys
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEST = os.path.join(ROOT, "models", "laya")
FILES = ["laya.int8g.onnx", "tokenizer.json", "rl_agent_config.json"]
REPO = "dd1000001000/zhiyi_ime"
TAG = "model-zhen-r64"
ASSET = "laya-zhen-r64-int8g.zip"
URL = f"https://github.com/{REPO}/releases/download/{TAG}/{ASSET}"
# SHA-256 of laya.int8g.onnx (joint Chinese + English LoRA r64, int8 with quantized embeddings)
ONNX_SHA256 = "def6890326d8382f8b7db3143715378784e21593267aa55a185c5fa0b9021bbb"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def pack():
    out = os.path.join(ROOT, "build", ASSET)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for f in FILES:
            z.write(os.path.join(DEST, f), f)
    print(f"{out}\nlaya.int8g.onnx sha256 {sha256(os.path.join(DEST, 'laya.int8g.onnx'))}")


def download():
    token = os.environ.get("GITHUB_TOKEN")
    if not token:
        print(f"downloading {URL}")
        with urllib.request.urlopen(URL, timeout=600) as r:
            return r.read()
    # Private repository: find the asset through the API and download it with the token.
    headers = {"Authorization": f"Bearer {token}", "X-GitHub-Api-Version": "2022-11-28"}
    req = urllib.request.Request(f"https://api.github.com/repos/{REPO}/releases/tags/{TAG}",
                                 headers={**headers, "Accept": "application/vnd.github+json"})
    with urllib.request.urlopen(req, timeout=60) as r:
        assets = json.load(r)["assets"]
    asset = next(a for a in assets if a["name"] == ASSET)
    print(f"downloading {ASSET} ({asset['size'] / 1e6:.0f} MB) via the GitHub API")
    req = urllib.request.Request(asset["url"], headers={**headers, "Accept": "application/octet-stream"})
    with urllib.request.urlopen(req, timeout=600) as r:
        return r.read()


def fetch(dest):
    onnx = os.path.join(dest, "laya.int8g.onnx")
    if all(os.path.exists(os.path.join(dest, f)) for f in FILES) and sha256(onnx) == ONNX_SHA256:
        print(f"model already in {dest}")
        return 0
    try:
        data = download()
    except urllib.error.HTTPError as e:
        hint = " (private repository: set GITHUB_TOKEN)" if e.code == 404 and "GITHUB_TOKEN" not in os.environ else ""
        print(f"download failed: HTTP {e.code}{hint}", file=sys.stderr)
        return 1
    os.makedirs(dest, exist_ok=True)
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        for f in FILES:
            with open(os.path.join(dest, f), "wb") as out:
                out.write(z.read(f))
    if sha256(onnx) != ONNX_SHA256:
        print("checksum mismatch for laya.int8g.onnx", file=sys.stderr)
        return 1
    print(f"model -> {dest} (SHA-256 verified)")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pack", action="store_true")
    ap.add_argument("--dest", default=DEST, help="target directory (default models/laya)")
    a = ap.parse_args()
    if a.pack:
        pack()
        return 0
    return fetch(a.dest)


if __name__ == "__main__":
    sys.exit(main())
