"""Downloads the Laya model used by 知意输入法 into models/laya/.

  python scripts/fetch_model.py            # download the release asset and verify it
  python scripts/fetch_model.py --pack     # maintainers: zip models/laya/ for a release upload

The model (laya.int8g.onnx ~96 MB) is too large for git; it is published as a GitHub release
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
FILES = ["laya.int8g.onnx", "tokenizer.json", "rl_agent_config.json", "unified_vocab.json"]
# The decoder graphs of the 1.5.4-1.5.8 model: removed from models/laya (the packager ships them when present).
OLD_FILES = ["laya_decoder.int8.onnx", "laya_decoder_memory.int8.onnx", "laya_decoder_step.int8.onnx"]
ASSET = "laya-zhen-long-l18-int8g.zip"
URL = f"https://github.com/dd1000001000/zhiyi_ime/releases/download/model-zhen-long-l18/{ASSET}"
# SHA-256 of the graph: 知意 1.4V2-Long-L18 (1.5.9). The unified model 1.4V2 (mmBERT-small,
# width 384) with 18 of its 22 encoder layers (sliding-window layers 7, 10, 13, 16 removed); word
# choice distilled from a fine-tuned Qwen3.5-9B teacher on 1.7M rows of the IME's real
# candidates, in the ctx-first sequence (unified_vocab.json "choice_format": the context, then the
# typed input as slots, then the candidates, no instructions); 1.4V2's one-shot continuation head
# scores every character at every typed syllable for the sentence composition. int8, the earlier
# models' pruned vocabulary plus 30387 tokens of the new training text.
ONNX_SHA256 = "4aa1e0a3a8496d0970b3cf5c5c3bf8b06338a4f0d12f8831721082f48b32b714"


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


def matches():
    if not all(os.path.exists(os.path.join(DEST, f)) for f in FILES):
        return False
    return sha256(os.path.join(DEST, "laya.int8g.onnx")) == ONNX_SHA256


def fetch():
    for f in OLD_FILES:
        if os.path.exists(os.path.join(DEST, f)):
            os.remove(os.path.join(DEST, f))
    if matches():
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
    if not matches():
        print("checksum mismatch for the model files", file=sys.stderr)
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
