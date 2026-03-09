@echo off
REM ============================================
REM Qwen OCR GUI - Build Script (ImGui + DX11)
REM ============================================
set LLAMA_DIR=..\..\llama.cpp

echo [Build] Qwen3.5 OCR GUI (ImGui + DX11)
echo.

if not exist "..\exe" mkdir "..\exe"

set IMGUI_DIR=imgui
set IMGUI_SOURCES=%IMGUI_DIR%\imgui.cpp %IMGUI_DIR%\imgui_draw.cpp %IMGUI_DIR%\imgui_tables.cpp %IMGUI_DIR%\imgui_widgets.cpp %IMGUI_DIR%\backends\imgui_impl_win32.cpp %IMGUI_DIR%\backends\imgui_impl_dx11.cpp

cl /EHsc /O2 /DUNICODE /D_UNICODE /std:c++17 ^
   /I"%LLAMA_DIR%\include" /I"%LLAMA_DIR%\ggml\include" /I"%LLAMA_DIR%\tools\mtmd" ^
   /I"%IMGUI_DIR%" /I"%IMGUI_DIR%\backends" ^
   main.cpp %IMGUI_SOURCES% ^
   /Fe:"..\exe\qwen_ocr.exe" ^
   /link "%LLAMA_DIR%\build\src\llama.lib" "%LLAMA_DIR%\build\tools\mtmd\mtmd.lib" ^
         user32.lib gdi32.lib gdiplus.lib comctl32.lib comdlg32.lib shell32.lib ole32.lib d3d11.lib d3dcompiler.lib dwmapi.lib

if %ERRORLEVEL%==0 (
    echo.
    copy /Y qwen_ocr.exe.manifest "..\exe\" >nul 2>&1
    echo [OK] Build successful: ..\exe\qwen_ocr.exe
    del *.obj 2>nul
) else (
    echo.
    echo [FAIL] Build failed
)
