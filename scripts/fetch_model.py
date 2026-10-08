"""Downloads the Laya model used by 知意输入法 into models/laya/.

  python scripts/fetch_model.py            # download the release asset and verify it
  python scripts/fetch_model.py --pack     # maintainers: zip models/laya/ for a release upload

The model (laya.int8g.onnx, ~85 MB) is too large for git; it is published as a GitHub release
asset. The training code and data are not published.
"""
import argparse
import hashlib
import io
import os
import sys
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEST = os.path.join(ROOT, "models", "laya")
FILES = ["laya.int8g.onnx", "tokenizer.json", "rl_agent_config.json"]
ASSET = "laya-zhen-small-distill-int8g.zip"
URL = f"https://github.com/dd1000001000/zhiyi_ime/releases/download/model-zhen-small-distill/{ASSET}"
# SHA-256 of laya.int8g.onnx: mmBERT-small (22 layers, width 384) with the Laya decision head,
# distilled from the previous model (the multilingual Laya checkpoint fine-tuned with a joint
# Chinese + English LoRA r64) on 1.1M samples: the earlier engine-candidate sets plus samples drawn
# from LCCC and C4 Chinese, each with the IME engine's real candidates. Guess prompt (context and
# candidates only; rl_agent_config.json says "zhiyi_prompt": "guess"), candidates shuffled during
# training (no position preference), int8 with quantized embeddings, the embedding table pruned to
# the 82,006 tokens Chinese and English use (85 MB; a remap Gather keeps the tokenizer unchanged)
ONNX_SHA256 = "9c7487ee983db329d866470ecd035c406d67c2548d61ec9fc7fcbe51882be167"


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


def fetch():
    onnx = os.path.join(DEST, "laya.int8g.onnx")
    if all(os.path.exists(os.path.join(DEST, f)) for f in FILES) and sha256(onnx) == ONNX_SHA256:
        print(f"model already in {DEST}")
        return 0
    print(f"downloading {URL}")
    with urllib.request.urlopen(URL, timeout=600) as r:
        data = r.read()
    os.makedirs(DEST, exist_ok=True)
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        for f in FILES:
            with open(os.path.join(DEST, f), "wb") as dst:
                dst.write(z.read(f))
    if sha256(onnx) != ONNX_SHA256:
        print("checksum mismatch for laya.int8g.onnx", file=sys.stderr)
        return 1
    print(f"model -> {DEST}")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pack", action="store_true")
    a = ap.parse_args()
    if a.pack:
        pack()
        return 0
    return fetch()


if __name__ == "__main__":
    sys.exit(main())
