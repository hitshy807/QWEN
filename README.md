# Qwen3.5 OCR GUI

Win32 C++ GUI application for semiconductor label OCR using Qwen3.5-2B with native llama.cpp integration (no HTTP server, direct GPU inference).

## Requirements

- **CUDA 12.x or 13.x** (must match your GPU)
- **NVIDIA GPU** with 4GB+ VRAM
- **Visual Studio 2022+** (C++ Desktop workload, x64)
- **CMake 3.20+** + **Ninja** (for llama.cpp build)

## Setup

### 1. Build llama.cpp

```bash
git clone https://github.com/ggml-org/llama.cpp
cd llama.cpp
cmake -B build -G Ninja -DGGML_CUDA=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

### 2. Download Qwen3.5-2B Model

Place both files in `exe/models/` folder:

| File | Size | Link |
|------|------|------|
| Qwen3.5-2B-Q4_K_M.gguf | 1.27 GB | [Download](https://huggingface.co/unsloth/Qwen3.5-2B-GGUF/resolve/main/Qwen3.5-2B-Q4_K_M.gguf) |
| mmproj-F16.gguf | 668 MB | [Download](https://huggingface.co/unsloth/Qwen3.5-2B-GGUF/resolve/main/mmproj-F16.gguf) |

### 3. Build & Run

From **x64 Developer Command Prompt**:

```bash
cd gui_cpp/dev
build.bat
```

Then copy DLLs from llama.cpp build to `exe/`:
```
llama.cpp/build/bin/llama.dll
llama.cpp/build/bin/mtmd.dll
llama.cpp/build/bin/ggml.dll
llama.cpp/build/bin/ggml-base.dll
llama.cpp/build/bin/ggml-cpu.dll
llama.cpp/build/bin/ggml-cuda.dll
```

Run `exe/qwen_ocr.exe`.

## Folder Structure

```
gui_cpp/
  dev/                    # Source code
    main.cpp              # Main application
    build.bat             # Build script (edit llama.cpp path)
    CMakeLists.txt        # CMake alternative
  exe/                    # Runtime folder (created by build)
    qwen_ocr.exe
    *.dll                 # Copy from llama.cpp build
    models/
      Qwen3.5-2B-Q4_K_M.gguf
      mmproj-F16.gguf
  README.md
```

## Usage

1. Click **Load Model** (wait for "Ready")
2. Click **Browse** to select image (auto crop + bilinear 1/2 resize)
3. Click **OCR**

## Features

- Auto blob crop + bilinear 1/2 resize (1/4 area)
- Native llama.cpp GPU inference (no HTTP overhead)
- Flash Attention + Q4_0 KV cache
- ~1.5 sec/image inference

## Tested Environment

| Component | Spec |
|-----------|------|
| GPU | NVIDIA RTX 5060 Laptop (8GB GDDR7) |
| OS | Windows 11 Pro |
| CUDA | 13.0 |
| Performance | ~1.5 sec/image |
