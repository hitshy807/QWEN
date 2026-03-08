@echo off
REM ============================================
REM Qwen OCR GUI - Build Script
REM Run from x64 VS Developer Command Prompt
REM ============================================
REM Edit this path to your llama.cpp location:
set LLAMA_DIR=..\..\llama.cpp

echo [Build] Qwen3.5 OCR GUI (native llama.cpp)
echo.

if not exist "..\exe" mkdir "..\exe"

cl /EHsc /O2 /DUNICODE /D_UNICODE /std:c++17 ^
   /I"%LLAMA_DIR%\include" /I"%LLAMA_DIR%\ggml\include" /I"%LLAMA_DIR%\tools\mtmd" ^
   main.cpp ^
   /Fe:"..\exe\qwen_ocr.exe" ^
   /link "%LLAMA_DIR%\build\src\llama.lib" "%LLAMA_DIR%\build\tools\mtmd\mtmd.lib" ^
         user32.lib gdi32.lib gdiplus.lib comctl32.lib comdlg32.lib shell32.lib ole32.lib

if %ERRORLEVEL%==0 (
    echo.
    echo [OK] Build successful: ..\exe\qwen_ocr.exe
    echo [NOTE] Copy DLLs from %LLAMA_DIR%\build\bin\ to ..\exe\
    del *.obj 2>nul
) else (
    echo.
    echo [FAIL] Build failed
)

pause
