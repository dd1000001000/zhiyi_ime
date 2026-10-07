"""Downloads ONNX Runtime (DirectML build, Windows x64) into third_party/onnxruntime/.

  python scripts/fetch_onnxruntime.py

The DirectML build also contains the CPU execution provider; the GPU is only used when
laya.device names a graphics card. The engine links onnxruntime.lib from there and the build
copies onnxruntime.dll and DirectML.dll next to zhiyi-server.exe.
  - ONNX Runtime: NuGet package Microsoft.ML.OnnxRuntime.DirectML (MIT). Its DirectML builds
    stop at 1.24.4.
  - DirectML.dll 1.15.4 (the version that ONNX Runtime 1.24.4 requires; Microsoft DirectML
    license, redistributable): taken from the onnxruntime-directml wheel on PyPI, which ships the
    same file and is much smaller than the Microsoft.AI.DirectML NuGet package.
See THIRD_PARTY_NOTICES.txt.
"""
import hashlib
import io
import json
import os
import shutil
import sys
import urllib.request
import zipfile

VERSION = "1.24.4"
NUGET = (f"https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.directml/{VERSION}/"
         f"microsoft.ml.onnxruntime.directml.{VERSION}.nupkg")
WHEEL_INDEX = f"https://pypi.org/pypi/onnxruntime-directml/{VERSION}/json"
WHEEL_TAG = "cp313-cp313-win_amd64"
DIRECTML_DLL_SHA256 = "9f7d19ab3bec5488bbcd572942c202c8ad2cd23d4f6ecf00000a2200e574b6c9"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEST = os.path.join(ROOT, "third_party", "onnxruntime")


def download(url):
    print(f"downloading {url}")
    with urllib.request.urlopen(url, timeout=300) as r:
        return r.read()


def main():
    marker = os.path.join(DEST, "VERSION_NUMBER")
    if (os.path.exists(marker) and open(marker).read().strip() == VERSION and
            os.path.exists(os.path.join(DEST, "lib", "DirectML.dll"))):
        print(f"ONNX Runtime {VERSION} (DirectML) already in {DEST}")
        return 0
    package = download(NUGET)
    index = json.loads(download(WHEEL_INDEX))
    wheel_url = next(f["url"] for f in index["urls"] if WHEEL_TAG in f["filename"])
    wheel = download(wheel_url)
    with zipfile.ZipFile(io.BytesIO(wheel)) as z:
        directml = z.read("onnxruntime/capi/DirectML.dll")
    digest = hashlib.sha256(directml).hexdigest()
    if digest != DIRECTML_DLL_SHA256:
        print(f"DirectML.dll SHA-256 {digest} does not match {DIRECTML_DLL_SHA256}", file=sys.stderr)
        return 1

    shutil.rmtree(DEST, ignore_errors=True)
    files = {
        "build/native/include/": "include/",
        "runtimes/win-x64/native/onnxruntime.dll": "lib/onnxruntime.dll",
        "runtimes/win-x64/native/onnxruntime.lib": "lib/onnxruntime.lib",
        "runtimes/win-x64/native/onnxruntime_providers_shared.dll": "lib/onnxruntime_providers_shared.dll",
        "LICENSE": "LICENSE",
        "ThirdPartyNotices.txt": "ThirdPartyNotices.txt",
    }
    with zipfile.ZipFile(io.BytesIO(package)) as z:
        for info in z.infolist():
            if info.is_dir():
                continue
            for source, target in files.items():
                if info.filename == source or (source.endswith("/") and info.filename.startswith(source)):
                    path = os.path.join(DEST, target + info.filename[len(source):] if source.endswith("/") else target)
                    os.makedirs(os.path.dirname(path), exist_ok=True)
                    with z.open(info) as src, open(path, "wb") as dst:
                        shutil.copyfileobj(src, dst)
    with open(os.path.join(DEST, "lib", "DirectML.dll"), "wb") as f:
        f.write(directml)
    with open(marker, "w") as f:
        f.write(VERSION)
    print(f"ONNX Runtime {VERSION} (DirectML) -> {DEST}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
