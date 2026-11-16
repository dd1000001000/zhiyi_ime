"""Downloads the Laya model used by 知意输入法 into models/laya/, and the word n-gram next to it into
models/ngram/.

  python scripts/fetch_model.py            # download the release assets and verify them
  python scripts/fetch_model.py --pack     # maintainers: zip models/laya/ and models/ngram/ for release uploads

The model (laya.one.next.slim.onnx, ~64 MB) and the n-gram (zh.ngram ~79 MB) are too large for git; each is
published as a release asset of github.com/dd1000001000/zhiyi_models. The training code and data
are not published.
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
# laya.one.next.slim.onnx: the one graph with the context's KV cache and the next-word decoder, run on the CPU
# (engine/src/laya/reranker.h, next_words.h); next_rows.json: the decoder's rows and each language's masks. Since 1.6.5
# the word choice runs only there, so the whole graph (laya.int8g.onnx) is no longer needed.
GRAPH = "laya.one.next.slim.onnx"
FILES = [GRAPH, "next_rows.json", "tokenizer.json", "rl_agent_config.json", "unified_vocab.json"]
# The decoder graphs of the 1.5.4-1.5.8 model: removed from models/laya (the packager ships them when present).
OLD_FILES = ["laya_decoder.int8.onnx", "laya_decoder_memory.int8.onnx", "laya_decoder_step.int8.onnx"]
ASSET = "laya-ml-flash-next-preview-1114-slim.zip"
URL = f"https://github.com/dd1000001000/zhiyi_models/releases/download/model-ml-flash-next-preview-1114/{ASSET}"
# SHA-256 of the graph: the slim one graph of 知意 ml-flash-next-preview-1114 (from 1.7.0): ML-Flash-1103 trained further with a
# 4-layer next-word decoder (all six languages) next to its word choice and composition, the decoder sharing the token
# table (4-bit) as one graph (is_step / need_cross); word choice end to end as ML-Flash-1103's. Before: the slim one
# graph of 知意 ML-Flash-1103 (1.6.6-1.6.7; 53 MB instead of 78 MB, the same model
# without retraining: the token table 4-bit in blocks of 32, the head's attention in-projections 8-bit, float16 block
# scales, an int32 vocabulary remap; 1.6.4-1.6.5 shipped the int8g zip's 78 MB graph, after 1.6.3; 1.6.0-1.6.3 shipped ML-pumpkin-1026 from
# model-ml-pumpkin-1026, 1.5.12-1.5.18 1.4V2-Long-L12, 1.5.9-1.5.11 1.4V2-Long-L18). ML-pumpkin-1026 (12 encoder
# layers of mmBERT-small, width 384; Simplified / Traditional Chinese, English, Japanese, Korean, Vietnamese) cut to 8
# layers with a prefix mask (the leading block, the context, attends only to itself, so its keys and values are
# cached per context) and one head shared by word choice and composition, distilled from Qwen3.5-9B LoRA teachers;
# trained further on rows typed without context. The engine reads its zh / en side only: the other tables of
# unified_vocab.json are under "ml". int8, 126963 embedding rows, the tokenizer of 1.4V2-Long-L18.
ONE_SHA256 = "8c84eb0169980529fdba84d83234007e345ea651a0ae0638cf15930c807ce02d"

# The word n-gram scored next to the word choice (laya.ngram, engine/src/laya/ngram.h), ng1017: Youmo
# IME's pinyin language model (LGPL-2.1-or-later, on libime's) with science text mixed in (CSL
# abstracts and science pages of SkyPile, C4, FineWeb-2), pruned by relative entropy to 4M bigrams
# and 4M trigrams. LGPL-2.1-or-later as its base.
NGRAM_DEST = os.path.join(ROOT, "models", "ngram")
NGRAM_FILE = "zh.ngram"
NGRAM_ASSET = "zhiyi-ngram-zh-1017.zip"
NGRAM_URL = f"https://github.com/dd1000001000/zhiyi_models/releases/download/ngram-zh-1017/{NGRAM_ASSET}"
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
    print(f"{out}\n{GRAPH} sha256 {sha256(os.path.join(DEST, GRAPH))}")
    out = os.path.join(ROOT, "build", NGRAM_ASSET)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(os.path.join(NGRAM_DEST, NGRAM_FILE), NGRAM_FILE)
    print(f"{out}\n{NGRAM_FILE} sha256 {sha256(os.path.join(NGRAM_DEST, NGRAM_FILE))}")


def matches():
    if not all(os.path.exists(os.path.join(DEST, f)) for f in FILES):
        return False
    return sha256(os.path.join(DEST, GRAPH)) == ONE_SHA256


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
