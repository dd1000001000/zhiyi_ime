"""Downloads the Laya model used by 知意输入法 into models/laya/, and the word n-gram next to it into
models/ngram/.

  python scripts/fetch_model.py            # download the release assets and verify them
  python scripts/fetch_model.py --pack     # maintainers: zip models/laya/ and models/ngram/ for release uploads

The model (laya.int8g.onnx ~84 MB) and the n-gram (zh.ngram ~79 MB) are too large for git; each is
published as a GitHub release asset. The training code and data are not published.
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
ASSET = "laya-zhen-long-l12-int8g.zip"
URL = f"https://github.com/dd1000001000/zhiyi_ime/releases/download/model-zhen-long-l12/{ASSET}"
# SHA-256 of the graph: 知意 1.4V2-Long-L12 (1.5.12; 1.5.9-1.5.11 shipped 1.4V2-Long-L18 from
# model-zhen-long-l18). The unified model 1.4V2 (mmBERT-small, width 384) with 12 of its 22 encoder
# layers (0, 1, 2, 6, 7, 8, 12, 13, 14, 19, 20, 21); word choice distilled from a fine-tuned
# Qwen3.5-9B teacher on 3.77M rows of the IME's real candidates, in the ctx-first sequence
# (unified_vocab.json "choice_format": the context, then the typed input as slots, then the
# candidates, no instructions); 1.4V2's one-shot continuation head, distilled from a second
# Qwen3.5-9B teacher, scores every character at every typed syllable for the sentence composition.
# int8, the vocabulary and tokenizer of 1.4V2-Long-L18.
ONNX_SHA256 = "82d0fd05a5c2bff7898be518101d23dd03d0a5c4ecb8abaebcb397d42bebb529"

# The word n-gram scored next to the word choice (laya.ngram, engine/src/laya/ngram.h), ng1017: Youmo
# IME's pinyin language model (LGPL-2.1-or-later, on libime's) with science text mixed in (CSL
# abstracts and science pages of SkyPile, C4, FineWeb-2), pruned by relative entropy to 4M bigrams
# and 4M trigrams. LGPL-2.1-or-later as its base.
NGRAM_DEST = os.path.join(ROOT, "models", "ngram")
NGRAM_FILE = "zh.ngram"
NGRAM_ASSET = "zhiyi-ngram-zh-1017.zip"
NGRAM_URL = f"https://github.com/dd1000001000/zhiyi_ime/releases/download/ngram-zh-1017/{NGRAM_ASSET}"
NGRAM_SHA256 = "0a9e5c151d7dc90665358b3f6ab10436520457a26e9cfe1fa2054d6c046f72cd"


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
    out = os.path.join(ROOT, "build", NGRAM_ASSET)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(os.path.join(NGRAM_DEST, NGRAM_FILE), NGRAM_FILE)
    print(f"{out}\n{NGRAM_FILE} sha256 {sha256(os.path.join(NGRAM_DEST, NGRAM_FILE))}")


def matches():
    if not all(os.path.exists(os.path.join(DEST, f)) for f in FILES):
        return False
    return sha256(os.path.join(DEST, "laya.int8g.onnx")) == ONNX_SHA256


def fetch_ngram():
    path = os.path.join(NGRAM_DEST, NGRAM_FILE)
    if os.path.exists(path) and sha256(path) == NGRAM_SHA256:
        print(f"n-gram already in {NGRAM_DEST}")
        return 0
    print(f"downloading {NGRAM_URL}")
    with urllib.request.urlopen(NGRAM_URL, timeout=600) as r:
        data = r.read()
    os.makedirs(NGRAM_DEST, exist_ok=True)
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        with open(path, "wb") as dst:
            dst.write(z.read(NGRAM_FILE))
    if sha256(path) != NGRAM_SHA256:
        print("checksum mismatch for the n-gram", file=sys.stderr)
        return 1
    print(f"n-gram -> {NGRAM_DEST}")
    return 0


def fetch():
    for f in OLD_FILES:
        if os.path.exists(os.path.join(DEST, f)):
            os.remove(os.path.join(DEST, f))
    if matches():
        print(f"model already in {DEST}")
        return fetch_ngram()
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
    return fetch_ngram()


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
