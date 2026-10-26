"""Zhiyi IME: the translation runtime archive with the patched llama-server (build.bat): the
official llama.cpp b11480 Vulkan runtime (llama-b11480-vulkan-x64.zip from the `translator`
release) with llama-server.exe, llama-server-impl.dll, llama-common.dll and mtmd.dll replaced, the
Visual C++ runtime the build links app-local beside them, and its README.txt rewritten.

    python make_runtime.py <llama-b11480-vulkan-x64.zip> <build-zhiyi\\bin> <VC CRT folder> <out.zip>

The VC CRT folder is Visual Studio's VC\\Redist\\MSVC\\<version>\\x64\\Microsoft.VC*.CRT of the
toolset that built it.
"""
import os
import sys
import zipfile

REPLACED = ("llama-server.exe", "llama-server-impl.dll", "llama-common.dll", "mtmd.dll")
VC_RUNTIME = ("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll")

README = """Zhiyi IME offline translation runtime

llama.cpp b11480 (https://github.com/ggml-org/llama.cpp), Windows x64 Vulkan build: llama-server and the
libraries it loads. MIT License (LICENSE-llama.cpp.txt); libomp.dll is LLVM OpenMP (LICENSE-LLVM-OpenMP.txt).
llama-server.exe, llama-server-impl.dll, llama-common.dll and mtmd.dll are built by Zhiyi IME from llama.cpp
b11480 with a small patch: given "pos": true, /completion also returns the parts of speech of the text being
translated, from the part-of-speech head stored in the model's GGUF (zhiyi.pos_head.*). The patch and how to
build it are in https://github.com/dd1000001000/zhiyi_ime (scripts/llama_server_pos). The other libraries are
those of llama.cpp's release llama-b11480-bin-win-vulkan-x64.zip. msvcp140.dll, vcruntime140.dll and
vcruntime140_1.dll are the Microsoft Visual C++ runtime, redistributed with the program.

It runs the Zhiyi translation model v4-1026 (zhiyi-mt-v4-1026-Q4_K_M.gguf, downloaded beside this archive):
NiuTrans LMT-60-0.6B (https://huggingface.co/NiuTrans/LMT-60-0.6B, Apache License 2.0) with 20 of its 28
layers, distilled from translations made by Tencent Hy-MT2-7B (https://huggingface.co/tencent/Hy-MT2-7B)
and Hy-MT2-30B-A3B (https://huggingface.co/tencent/Hy-MT2-30B-A3B), both Apache License 2.0, quantized
to Q4_K_M, with its part-of-speech head. Licensed under the Apache License 2.0 (LICENSE-zhiyi-mt.txt; the
LMT-60 and Hy-MT2 licenses are in LICENSE-LMT-60.txt, LICENSE-Hy-MT2.txt and LICENSE-Hy-MT2-30B-A3B.txt).
"""


def main(base, bin_dir, crt_dir, out):
    with zipfile.ZipFile(base) as source, zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as target:
        for info in source.infolist():
            if info.filename in REPLACED or info.filename == "README.txt":
                continue
            target.writestr(info, source.read(info.filename))
        for name in REPLACED:
            target.write(os.path.join(bin_dir, name), name)
        for name in VC_RUNTIME:
            target.write(os.path.join(crt_dir, name), name)
        target.writestr("README.txt", README)
    print("wrote", out, os.path.getsize(out))


if __name__ == "__main__":
    main(*sys.argv[1:5])
