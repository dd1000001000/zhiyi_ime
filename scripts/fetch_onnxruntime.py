"""Downloads ONNX Runtime (CPU, Windows x64) into third_party/onnxruntime/.

  python scripts/fetch_onnxruntime.py

The engine links onnxruntime.lib from there and the build copies onnxruntime.dll next to
zhiyi-server.exe. ONNX Runtime is MIT licensed (see THIRD_PARTY_NOTICES.txt).
"""
import io
import os
import shutil
import sys
import urllib.request
import zipfile

VERSION = "1.30.0"
URL = (f"https://github.com/microsoft/onnxruntime/releases/download/v{VERSION}/"
       f"onnxruntime-win-x64-{VERSION}.zip")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEST = os.path.join(ROOT, "third_party", "onnxruntime")


def main():
    marker = os.path.join(DEST, "VERSION_NUMBER")
    if os.path.exists(marker) and open(marker).read().strip() == VERSION:
        print(f"ONNX Runtime {VERSION} already in {DEST}")
        return 0
    print(f"downloading {URL}")
    with urllib.request.urlopen(URL, timeout=300) as r:
        data = r.read()
    shutil.rmtree(DEST, ignore_errors=True)
    prefix = f"onnxruntime-win-x64-{VERSION}/"
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        for info in z.infolist():
            if not info.filename.startswith(prefix) or info.is_dir():
                continue
            target = os.path.join(DEST, info.filename[len(prefix):])
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with z.open(info) as src, open(target, "wb") as dst:
                shutil.copyfileobj(src, dst)
    print(f"ONNX Runtime {VERSION} -> {DEST}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
