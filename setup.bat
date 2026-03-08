@echo off
setlocal enabledelayedexpansion
echo ============================================
echo  LABEL OCR 1.0 - Auto Setup
echo ============================================
echo.

REM === Check prerequisites ===
echo [1/6] Checking prerequisites...

where nvcc >nul 2>&1
if errorlevel 1 (
    echo   [FAIL] CUDA not found.
    echo          Install CUDA Toolkit: https://developer.nvidia.com/cuda-downloads
    goto :fail
)
for /f "tokens=5 delims=, " %%a in ('nvcc --version ^| findstr release') do echo   CUDA: %%a

where cl >nul 2>&1
if errorlevel 1 (
    echo   [FAIL] cl.exe not found.
    echo          Run this from "x64 Native Tools Command Prompt for VS"
    goto :fail
)
echo   MSVC: OK

where cmake >nul 2>&1
if errorlevel 1 (
    echo   [FAIL] CMake not found. Install: https://cmake.org/download/
    goto :fail
)
echo   CMake: OK

where ninja >nul 2>&1
if errorlevel 1 (
    echo   [FAIL] Ninja not found. Install: pip install ninja
    goto :fail
)
echo   Ninja: OK
echo.

REM === Clone and build llama.cpp ===
echo [2/6] Setting up llama.cpp...
if not exist "..\llama.cpp\CMakeLists.txt" (
    echo   Cloning llama.cpp...
    git clone https://github.com/ggml-org/llama.cpp ..\llama.cpp
    if errorlevel 1 goto :fail
)
if not exist "..\llama.cpp\build\bin\llama.dll" (
    echo   Building llama.cpp with CUDA (this takes a few minutes)...
    pushd ..\llama.cpp
    cmake -B build -G Ninja -DGGML_CUDA=ON -DCMAKE_BUILD_TYPE=Release
    if errorlevel 1 ( popd & goto :fail )
    cmake --build build --config Release
    if errorlevel 1 ( popd & goto :fail )
    popd
) else (
    echo   llama.cpp already built, skipping.
)
echo.

REM === Download models ===
echo [3/6] Downloading Qwen3.5-2B model files...
if not exist "exe\models" mkdir "exe\models"
if not exist "exe\models\Qwen3.5-2B-Q4_K_M.gguf" (
    echo   Downloading Qwen3.5-2B-Q4_K_M.gguf [1.27 GB]...
    curl -L --progress-bar -o "exe\models\Qwen3.5-2B-Q4_K_M.gguf" ^
        "https://huggingface.co/unsloth/Qwen3.5-2B-GGUF/resolve/main/Qwen3.5-2B-Q4_K_M.gguf"
    if errorlevel 1 goto :fail
) else (
    echo   Model file exists, skipping.
)
if not exist "exe\models\mmproj-F16.gguf" (
    echo   Downloading mmproj-F16.gguf [668 MB]...
    curl -L --progress-bar -o "exe\models\mmproj-F16.gguf" ^
        "https://huggingface.co/unsloth/Qwen3.5-2B-GGUF/resolve/main/mmproj-F16.gguf"
    if errorlevel 1 goto :fail
) else (
    echo   Vision encoder exists, skipping.
)
echo.

REM === Copy DLLs ===
echo [4/6] Copying runtime DLLs...
if not exist "exe" mkdir "exe"
for %%f in (llama.dll mtmd.dll ggml.dll ggml-base.dll ggml-cpu.dll ggml-cuda.dll) do (
    copy /Y "..\llama.cpp\build\bin\%%f" "exe\" >nul 2>&1
)
echo   DLLs copied.
echo.

REM === Build application ===
echo [5/6] Building LABEL OCR...
pushd dev
call build.bat
popd
if not exist "exe\qwen_ocr.exe" goto :fail
echo.

echo [6/6] Setup complete!
echo.
echo   Run:  exe\qwen_ocr.exe
echo.
pause
exit /b 0

:fail
echo.
echo [FAIL] Setup failed. Check errors above.
pause
exit /b 1
