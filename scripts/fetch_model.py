"""Downloads the Laya model used by 知意输入法 into models/laya/.

  python scripts/fetch_model.py            # download the release asset and verify it
  python scripts/fetch_model.py --pack     # maintainers: zip models/laya/ for a release upload

The model (laya.int8g.onnx ~84 MB, laya_decoder.int8.onnx ~17 MB and its KV cache graphs
laya_decoder_memory.int8.onnx / laya_decoder_step.int8.onnx ~17 MB) is too large for git; it is
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
FILES = ["laya.int8g.onnx", "laya_decoder.int8.onnx", "laya_decoder_memory.int8.onnx", "laya_decoder_step.int8.onnx",
         "tokenizer.json", "rl_agent_config.json", "unified_vocab.json"]
ASSET = "laya-zhen-unified-dec-int8g.zip"
URL = f"https://github.com/dd1000001000/zhiyi_ime/releases/download/model-zhen-unified-dec/{ASSET}"
# SHA-256 of the two graphs: the decoder model (1.5.4). The unified model of 1.4 (mmBERT-small,
# 22 layers, width 384: word choice among the candidates, reading the typed pinyin as slots) with
# its one-shot continuation head replaced by a two-layer decoder (laya_decoder.int8.onnx) that
# writes long pinyin a character at a time, each character given the context, all the typed
# syllables and the characters chosen before it; the engine runs it in a beam over the
# dictionary's words. Trained from the 1.4 model on the same choice samples plus 3.15M
# long-pinyin rows (long-form web text with real preceding context, and dialogues), both heads
# distilled from a larger unified teacher. int8, the 1.2 model's pruned vocabulary;
# unified_vocab.json lists the units and characters and names the decoder graph. The same decoder
# also comes as two graphs that keep a KV cache (the memory's cross-attention keys and values, and
# one character from what the earlier ones kept), so each character costs only its own work.
ONNX_SHA256 = "31f1124f50fd9d1ea9cd5ea5ce71ed5008ed593fd440707e41d565cd9dbd3884"
GRAPH_SHA256 = {
    "laya_decoder.int8.onnx": "2ac3f7f9ddcb9d7a02ab520b7ebbc5dfe48f8f677cf44f80ddf8d269fb9eecec",
    "laya_decoder_memory.int8.onnx": "da6e94df7b44fe0f77153a506e6e57dd06508859c6889f0118ffdb1e132bfa57",
    "laya_decoder_step.int8.onnx": "9a650da4b653456d64a75a31da1ac5b8847f52c784baede9f84b555dbdc71bbc",
}


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
    return sha256(os.path.join(DEST, "laya.int8g.onnx")) == ONNX_SHA256 and all(
        sha256(os.path.join(DEST, f)) == h for f, h in GRAPH_SHA256.items())


def fetch():
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
