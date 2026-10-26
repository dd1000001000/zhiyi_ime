@echo off
rem Zhiyi IME: builds llama-server with the part-of-speech patch (apply_patch.py) in
rem <llama.cpp folder>\build-zhiyi\bin. Only llama-server.exe, llama-server-impl.dll,
rem llama-common.dll and mtmd.dll are used; the backends (Vulkan, CPU variants) stay those of the
rem official b11480 release (make_runtime.py).
rem
rem     build.bat <llama.cpp folder>
rem
rem Needs Visual Studio with C++ (found with vswhere, or VS_PATH).
setlocal
set "ROOT=%~f1"
if "%ROOT%"=="" (
    echo usage: build.bat ^<llama.cpp folder^>
    exit /b 1
)
if not defined VS_PATH (
    for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_PATH=%%i"
)
call "%VS_PATH%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul || exit /b 1
set "PATH=%VS_PATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VS_PATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
if not exist "%ROOT%\build-zhiyi\build.ninja" (
    cmake -S "%ROOT%" -B "%ROOT%\build-zhiyi" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
        -DGGML_NATIVE=OFF -DGGML_CPU=OFF -DGGML_BACKEND_DL=ON ^
        -DLLAMA_OPENSSL=OFF -DLLAMA_BUILD_UI=OFF -DLLAMA_USE_PREBUILT_UI=OFF ^
        -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF || exit /b 1
)
cmake --build "%ROOT%\build-zhiyi" --target llama-server
