# LABEL OCR 1.0

Semiconductor label OCR using Qwen3.5-2B + llama.cpp native GPU inference.

## Quick Start

Open **x64 Native Tools Command Prompt for VS** and run:

```
git clone -b OCR https://github.com/hitshy807/QWEN.git
cd QWEN
setup.bat
```

This will automatically:
- Check CUDA, MSVC, CMake, Ninja
- Clone and build llama.cpp with CUDA
- Download Qwen3.5-2B model files (~2 GB)
- Copy DLLs and build the application

Then run `exe\qwen_ocr.exe`.

## Requirements

- NVIDIA GPU (4GB+ VRAM)
- CUDA Toolkit (12.x / 13.x)
- Visual Studio 2022+ (C++ Desktop workload)
- CMake + Ninja
