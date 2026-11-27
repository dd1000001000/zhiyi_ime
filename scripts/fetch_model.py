"""Downloads the Laya model used by 知意输入法 into models/laya/, and the word n-gram next to it into
models/ngram/.

  python scripts/fetch_model.py            # download the release assets and verify them
  python scripts/fetch_model.py --pack     # maintainers: zip models/laya/ and models/ngram/ for release uploads

The model (laya.feather.onnx, ~40 MB) and the n-gram (zh.ngram ~79 MB) are too large for git. The model comes from
its Hugging Face repository (huggingface.co/zhiyi-ime/zhiyi-zhen-feather, at a fixed commit), the n-gram from a
release asset of github.com/dd1000001000/zhiyi_models. The training code and data are not published.
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
# laya.feather.onnx: word choice, composition and next words in one file (the context mode with its KV cache, the key
# mode, and the next-word step behind is_step), run on the CPU (engine/src/laya/reranker.h, next_words.h);
# next_rows.json: the decoder's rows (zh / en modes); tokenizer.json: its 32k Unigram tokenizer. The repository's
# tokenizer.candidates.json is tokenizer.json with the Metaspace prepend "never" (the engine's
# BpeTokenizer::encode_no_prepend): not downloaded. No rl_agent_config.json (only the old Laya prompt format read it).
GRAPH = "laya.feather.onnx"
HF_COMMIT = "bdc47101727eb486fec256b5d9b95a1be4134c32"
HF_URL = f"https://huggingface.co/zhiyi-ime/zhiyi-zhen-feather/resolve/{HF_COMMIT}/onnx/"
# Each file and its SHA-256.
FILES = {
    GRAPH: "f11b51692ecf9133920f79ec6a76cc4ec78c6ee406425ac5d95456c038c40232",
    "next_rows.json": "40eba16abd5ac187db8b744ec8c35ef7ba64760c950444de49f5cfd18e95cfac",
    "tokenizer.json": "c15b29233af2d53707b61d999037bef16433b48a652651521f038220eb4cf814",
    "unified_vocab.json": "815f236a184359a1170cc962d186c4775d803f1390124916db8811de3e949af2",
}
# Removed from models/laya: the decoder graphs of the 1.5.4-1.5.8 model, ml-flash-next's one graph (1.7.0-1.7.1) and
# zhen-feather-preview-1121's three graphs (1.7.2), which the engine and the packager would otherwise take first, and
# the old rl_agent_config.json.
OLD_FILES = ["laya_decoder.int8.onnx", "laya_decoder_memory.int8.onnx", "laya_decoder_step.int8.onnx",
             "laya.one.next.slim.onnx", "laya.one.w4.onnx", "laya.next.head.w4.onnx", "laya.next.step.w4.onnx",
             "rl_agent_config.json"]
# The model: 知意 zhen-feather-1127 (from 1.7.3), Feather retrained in full (no pre-training) on new and old rows with
# Mini's 32k vocabulary and one n-gram table shared by its word choice, composition and next words (PQ16); the word
# choice / composition / context branch 8-bit, the next-word step and the token table 4-bit.
# Before: the slim one graph of 知意 zhen-feather-preview-1121 (1.7.2), a Lite model (8 encoder layers of width
# 256, mmBERT-small's tokenizer): zh-lite-next-preview-1120 (word choice, a 4-layer next-word decoder with a memory
# table, single characters among its next words) trained further with whole-sentence composition; token table pruned
# to the 116,118 rows its data uses, 4-bit. Before: the slim one graph of 知意 ml-flash-next-preview-1114 (1.7.0-1.7.1): ML-Flash-1103 trained further with a
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
    for f in FILES:
        print(f"{f} sha256 {sha256(os.path.join(DEST, f))}")
    out = os.path.join(ROOT, "build", NGRAM_ASSET)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(os.path.join(NGRAM_DEST, NGRAM_FILE), NGRAM_FILE)
    print(f"{out}\n{NGRAM_FILE} sha256 {sha256(os.path.join(NGRAM_DEST, NGRAM_FILE))}")


def matches(f):
    path = os.path.join(DEST, f)
    return os.path.exists(path) and sha256(path) == FILES[f]


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
    os.makedirs(DEST, exist_ok=True)
    for f in FILES:
        if matches(f):
            continue
        print(f"downloading {HF_URL}{f}")
        with urllib.request.urlopen(HF_URL + f, timeout=600) as r:
            data = r.read()
        with open(os.path.join(DEST, f), "wb") as dst:
            dst.write(data)
        if not matches(f):
            print(f"checksum mismatch for {f}", file=sys.stderr)
            return 1
    print(f"model in {DEST}")
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
