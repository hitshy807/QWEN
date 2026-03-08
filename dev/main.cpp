/*
 * Qwen3.5 OCR GUI - Native llama.cpp Integration
 *
 * Flow:
 *   1. Browse Image -> auto blob crop + bilinear 1/2 resize -> show preview
 *   2. Load Model -> loads model + vision encoder directly (no server)
 *   3. OCR -> native GPU inference, no HTTP overhead
 *
 * Dependencies: Windows SDK, llama.cpp (llama.dll, mtmd.dll, ggml*.dll)
 */
#define NOMINMAX
#define LLAMA_SHARED
#include <windows.h>
#include <objidl.h>
#include <commdlg.h>
#include <gdiplus.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <algorithm>

#include "llama.h"
#include "mtmd.h"
#include "mtmd-helper.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' \
version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ===== Constants =====
static const wchar_t* APP_TITLE = L"Qwen3.5 OCR";
static const int WIN_W = 1100, WIN_H = 720;
static const int IMG_X = 15, IMG_Y = 55, IMG_W = 520, IMG_H = 540;
static const int CTL_X = 555;
static const int BLOB_THRESHOLD = 180;
static const int CROP_PADDING = 10;
static const int MAX_TOKENS = 256;

enum {
    IDC_MODEL_LOAD = 101, IDC_MODEL_STATUS,
    IDC_IMAGE_PATH = 111, IDC_IMAGE_BROWSE, IDC_IMAGE_INFO,
    IDC_OCR_BTN = 121, IDC_OCR_TIME,
    IDC_RESULT = 131,
};
enum { WM_MODEL_READY = WM_APP + 1, WM_MODEL_FAIL, WM_OCR_DONE, WM_OCR_FAIL };

// ===== Global State =====
static HWND g_hWnd;
static HWND g_hModelLoad, g_hModelStatus;
static HWND g_hImagePath, g_hImageBrowse, g_hImageInfo;
static HWND g_hOcrBtn, g_hOcrTime;
static HWND g_hResult;
static HFONT g_hFont, g_hFontBold;

static Gdiplus::Bitmap* g_processedImage = nullptr;
static std::wstring g_imagePath;
static std::string g_ocrResult;
static double g_ocrElapsed = 0;

// llama.cpp state
static llama_model*    g_model    = nullptr;
static llama_context*  g_lctx     = nullptr;
static mtmd_context*   g_mtmd_ctx = nullptr;
static const llama_vocab* g_vocab = nullptr;
static bool g_modelLoaded = false;

// ===== Utility: String Conversion =====
static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring ws(len - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], len);
    return ws;
}

static std::string WideToUtf8(const std::wstring& ws) {
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &s[0], len, nullptr, nullptr);
    return s;
}

// ===== Image Preprocessing: Blob Crop + Bilinear Resize =====
static Gdiplus::Bitmap* CropAndResize(const std::wstring& path,
                                       int& origW, int& origH, int& cropW, int& cropH,
                                       int& outW, int& outH) {
    Gdiplus::Bitmap src(path.c_str());
    if (src.GetLastStatus() != Gdiplus::Ok) return nullptr;

    int w = src.GetWidth(), h = src.GetHeight();
    origW = w; origH = h;

    Gdiplus::BitmapData bmpData;
    Gdiplus::Rect rect(0, 0, w, h);
    if (src.LockBits(&rect, Gdiplus::ImageLockModeRead,
                      PixelFormat32bppARGB, &bmpData) != Gdiplus::Ok) return nullptr;

    int minX = w, minY = h, maxX = 0, maxY = 0;
    for (int y = 0; y < h; y++) {
        uint8_t* row = (uint8_t*)bmpData.Scan0 + y * bmpData.Stride;
        for (int x = 0; x < w; x++) {
            uint8_t b = row[x * 4 + 0];
            uint8_t g = row[x * 4 + 1];
            uint8_t r = row[x * 4 + 2];
            uint8_t gray = (uint8_t)(0.299f * r + 0.587f * g + 0.114f * b);
            if (gray > BLOB_THRESHOLD) {
                if (x < minX) minX = x;
                if (y < minY) minY = y;
                if (x > maxX) maxX = x;
                if (y > maxY) maxY = y;
            }
        }
    }
    src.UnlockBits(&bmpData);

    if (maxX <= minX || maxY <= minY) {
        minX = 0; minY = 0; maxX = w - 1; maxY = h - 1;
    }

    minX = (std::max)(0, minX - CROP_PADDING);
    minY = (std::max)(0, minY - CROP_PADDING);
    maxX = (std::min)(w - 1, maxX + CROP_PADDING);
    maxY = (std::min)(h - 1, maxY + CROP_PADDING);

    cropW = maxX - minX + 1;
    cropH = maxY - minY + 1;

    // Bilinear resize to 1/4 area (1/2 each dimension)
    outW = cropW / 2;
    outH = cropH / 2;
    if (outW < 100) { outW = cropW; outH = cropH; }

    auto* result = new Gdiplus::Bitmap(outW, outH, PixelFormat24bppRGB);
    Gdiplus::Graphics g(result);
    g.SetInterpolationMode(Gdiplus::InterpolationModeBilinear);
    Gdiplus::Rect destRect(0, 0, outW, outH);
    g.DrawImage(&src, destRect, minX, minY, cropW, cropH, Gdiplus::UnitPixel);

    return result;
}

// ===== Extract RGB bytes from GDI+ Bitmap for mtmd =====
static std::vector<uint8_t> BitmapToRGB(Gdiplus::Bitmap* bmp, uint32_t& w, uint32_t& h) {
    w = bmp->GetWidth();
    h = bmp->GetHeight();
    Gdiplus::BitmapData data;
    Gdiplus::Rect rect(0, 0, (int)w, (int)h);
    bmp->LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat24bppRGB, &data);
    std::vector<uint8_t> rgb(w * h * 3);
    for (uint32_t y = 0; y < h; y++) {
        uint8_t* row = (uint8_t*)data.Scan0 + y * data.Stride;
        for (uint32_t x = 0; x < w; x++) {
            // GDI+ PixelFormat24bppRGB is actually BGR
            rgb[(y * w + x) * 3 + 0] = row[x * 3 + 2]; // R
            rgb[(y * w + x) * 3 + 1] = row[x * 3 + 1]; // G
            rgb[(y * w + x) * 3 + 2] = row[x * 3 + 0]; // B
        }
    }
    bmp->UnlockBits(&data);
    return rgb;
}

// ===== File & Path Utilities =====
static std::wstring GetExeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path(buf);
    return path.substr(0, path.rfind(L'\\'));
}

static bool FileExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// ===== Free llama resources =====
static void FreeModel() {
    g_modelLoaded = false;
    if (g_mtmd_ctx) { mtmd_free(g_mtmd_ctx); g_mtmd_ctx = nullptr; }
    if (g_lctx) { llama_free(g_lctx); g_lctx = nullptr; }
    if (g_model) { llama_model_free(g_model); g_model = nullptr; }
    g_vocab = nullptr;
}

// ===== Model Load Thread =====
static void ModelLoadThread() {
    std::wstring exeDir = GetExeDir();

    // Search model dirs
    static const wchar_t* MODEL_FILE = L"Qwen3.5-2B-Q4_K_M.gguf";
    static const wchar_t* MMPROJ_FILE = L"mmproj-F16.gguf";
    std::wstring modelDirs[] = {
        exeDir + L"\\models",
        exeDir + L"\\..\\models",
        exeDir + L"\\..\\..\\models",
    };
    std::wstring modelPath, mmprojPath;
    for (auto& dir : modelDirs) {
        std::wstring m = dir + L"\\" + MODEL_FILE;
        std::wstring p = dir + L"\\" + MMPROJ_FILE;
        if (FileExists(m) && FileExists(p)) {
            modelPath = m; mmprojPath = p; break;
        }
    }
    if (modelPath.empty()) {
        g_ocrResult = "Model not found. Place Qwen3.5-2B-Q4_K_M.gguf + mmproj-F16.gguf in exe/models/";
        PostMessage(g_hWnd, WM_MODEL_FAIL, 0, 0);
        return;
    }

    FreeModel();

    std::string modelUtf8 = WideToUtf8(modelPath);
    std::string mmprojUtf8 = WideToUtf8(mmprojPath);

    // Load model
    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = 99;
    g_model = llama_model_load_from_file(modelUtf8.c_str(), mparams);
    if (!g_model) {
        g_ocrResult = "Failed to load model";
        PostMessage(g_hWnd, WM_MODEL_FAIL, 0, 0);
        return;
    }
    g_vocab = llama_model_get_vocab(g_model);

    // Create context
    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = 1536;
    cparams.n_batch = 1536;
    cparams.n_threads = (int)std::thread::hardware_concurrency();
    cparams.n_threads_batch = (int)std::thread::hardware_concurrency();
    cparams.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED;
    cparams.type_k = GGML_TYPE_Q4_0;
    cparams.type_v = GGML_TYPE_Q4_0;
    g_lctx = llama_init_from_model(g_model, cparams);
    if (!g_lctx) {
        g_ocrResult = "Failed to create context";
        FreeModel();
        PostMessage(g_hWnd, WM_MODEL_FAIL, 0, 0);
        return;
    }

    // Create multimodal context
    mtmd_context_params mtparams = mtmd_context_params_default();
    mtparams.use_gpu = true;
    mtparams.print_timings = false;
    mtparams.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED;
    mtparams.n_threads = (int)std::thread::hardware_concurrency();
    mtparams.warmup = true;
    g_mtmd_ctx = mtmd_init_from_file(mmprojUtf8.c_str(), g_model, mtparams);
    if (!g_mtmd_ctx) {
        g_ocrResult = "Failed to load vision encoder";
        FreeModel();
        PostMessage(g_hWnd, WM_MODEL_FAIL, 0, 0);
        return;
    }

    // Warmup: dummy decode to pre-compile CUDA kernels
    {
        llama_batch wb = llama_batch_init(1, 0, 1);
        wb.n_tokens = 1;
        wb.token[0] = 0;
        wb.pos[0] = 0;
        wb.n_seq_id[0] = 1;
        wb.seq_id[0][0] = 0;
        wb.logits[0] = 0;
        llama_decode(g_lctx, wb);
        llama_batch_free(wb);
        llama_memory_clear(llama_get_memory(g_lctx), true);
    }

    g_modelLoaded = true;
    PostMessage(g_hWnd, WM_MODEL_READY, 0, 0);
}

// ===== OCR Thread =====
static void OcrThread() {
    if (!g_processedImage || !g_modelLoaded) {
        g_ocrResult = "Model or image not ready.";
        PostMessage(g_hWnd, WM_OCR_FAIL, 0, 0);
        return;
    }

    // Extract RGB from processed image
    uint32_t imgW, imgH;
    auto rgb = BitmapToRGB(g_processedImage, imgW, imgH);

    // Create mtmd bitmap
    mtmd_bitmap* bmp = mtmd_bitmap_init(imgW, imgH, rgb.data());
    if (!bmp) {
        g_ocrResult = "Failed to create bitmap";
        PostMessage(g_hWnd, WM_OCR_FAIL, 0, 0);
        return;
    }

    // Build prompt: <media_marker>\nOCR instruction
    const char* marker = mtmd_default_marker();
    std::string userContent = std::string(marker) +
        "\nRead every line strictly top to bottom in exact order. "
        "Include *...* number lines in their exact position between surrounding lines. "
        "Output text only. /no_think";

    // Apply chat template
    llama_chat_message msgs[] = {
        {"user", userContent.c_str()},
    };
    const char* tmpl = llama_model_chat_template(g_model, nullptr);
    int32_t bufLen = llama_chat_apply_template(tmpl, msgs, 1, true, nullptr, 0);
    if (bufLen < 0) {
        g_ocrResult = "Failed to apply chat template";
        mtmd_bitmap_free(bmp);
        PostMessage(g_hWnd, WM_OCR_FAIL, 0, 0);
        return;
    }
    std::vector<char> buf(bufLen + 1);
    llama_chat_apply_template(tmpl, msgs, 1, true, buf.data(), (int32_t)buf.size());
    std::string prompt(buf.data(), bufLen);
    // Force no-think: pre-fill completed think block
    prompt += "<think>\n</think>\n";

    // Tokenize with multimodal support
    mtmd_input_text text;
    text.text = prompt.c_str();
    text.add_special = true;
    text.parse_special = true;

    mtmd_input_chunks* chunks = mtmd_input_chunks_init();
    const mtmd_bitmap* bitmaps[] = { bmp };
    int32_t res = mtmd_tokenize(g_mtmd_ctx, chunks, &text, bitmaps, 1);
    mtmd_bitmap_free(bmp);

    if (res != 0) {
        g_ocrResult = "Failed to tokenize prompt";
        mtmd_input_chunks_free(chunks);
        PostMessage(g_hWnd, WM_OCR_FAIL, 0, 0);
        return;
    }

    // Clear KV cache
    llama_memory_clear(llama_get_memory(g_lctx), true);

    auto t0 = std::chrono::high_resolution_clock::now();

    // Eval all chunks (text + image)
    llama_pos n_past = 0;
    llama_pos new_n_past;
    res = mtmd_helper_eval_chunks(g_mtmd_ctx, g_lctx, chunks,
        n_past, 0, 1536, true, &new_n_past);
    mtmd_input_chunks_free(chunks);

    if (res != 0) {
        g_ocrResult = "Failed to eval prompt";
        PostMessage(g_hWnd, WM_OCR_FAIL, 0, 0);
        return;
    }
    n_past = new_n_past;

    // Greedy sampler (temperature = 0)
    auto sparams = llama_sampler_chain_default_params();
    llama_sampler* smpl = llama_sampler_chain_init(sparams);
    llama_sampler_chain_add(smpl, llama_sampler_init_greedy());

    // Find </think> token ID to filter thinking output
    llama_token think_end_id = -1;
    {
        llama_token tmp[4];
        int32_t n = llama_tokenize(g_vocab, "</think>", 8, tmp, 4, false, true);
        if (n == 1) think_end_id = tmp[0];
    }

    // Generate tokens
    std::string result;
    llama_batch batch = llama_batch_init(1, 0, 1);
    bool past_think = true; // pre-fill handles think suppression, collect all tokens

    for (int i = 0; i < MAX_TOKENS; i++) {
        llama_token token_id = llama_sampler_sample(smpl, g_lctx, -1);

        if (llama_vocab_is_eog(g_vocab, token_id)) break;

        // Skip all tokens until after </think>
        if (!past_think) {
            if (token_id == think_end_id) past_think = true;
        } else {
            char piece[256];
            int n = llama_token_to_piece(g_vocab, token_id, piece, sizeof(piece), 0, false);
            if (n > 0) result.append(piece, n);
        }

        // Decode next token
        batch.n_tokens = 1;
        batch.token[0] = token_id;
        batch.pos[0] = n_past++;
        batch.n_seq_id[0] = 1;
        batch.seq_id[0][0] = 0;
        batch.logits[0] = 1;

        if (llama_decode(g_lctx, batch)) break;
    }

    llama_batch_free(batch);
    llama_sampler_free(smpl);

    auto t1 = std::chrono::high_resolution_clock::now();
    g_ocrElapsed = std::chrono::duration<double>(t1 - t0).count();

    // Trim leading whitespace/newlines
    while (!result.empty() && (result[0] == '\n' || result[0] == ' ')) result.erase(0, 1);

    g_ocrResult = result.empty() ? "No output generated" : result;
    PostMessage(g_hWnd, result.empty() ? WM_OCR_FAIL : WM_OCR_DONE, 0, 0);
}

// ===== File Dialog =====
static std::wstring BrowseImage(HWND hWnd) {
    wchar_t buf[MAX_PATH] = {};
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.hwndOwner = hWnd;
    ofn.lpstrFilter = L"Images (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0All Files\0*.*\0";
    ofn.lpstrFile = buf; ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST;
    ofn.lpstrTitle = L"Select Image for OCR";
    return GetOpenFileNameW(&ofn) ? buf : L"";
}

// ===== GDI+ Image Preview =====
static void DrawPreview(HDC hdc) {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen pen(Gdiplus::Color(80, 80, 80), 1.5f);
    g.DrawRectangle(&pen, IMG_X, IMG_Y, IMG_W, IMG_H);

    if (g_processedImage && g_processedImage->GetLastStatus() == Gdiplus::Ok) {
        int iw = g_processedImage->GetWidth(), ih = g_processedImage->GetHeight();
        float scale = (std::min)((float)(IMG_W - 10) / iw, (float)(IMG_H - 10) / ih);
        int dw = (int)(iw * scale), dh = (int)(ih * scale);
        int dx = IMG_X + (IMG_W - dw) / 2, dy = IMG_Y + (IMG_H - dh) / 2;
        g.SetInterpolationMode(Gdiplus::InterpolationModeBilinear);
        g.DrawImage(g_processedImage, dx, dy, dw, dh);
    } else {
        Gdiplus::Font font(L"Segoe UI", 18);
        Gdiplus::SolidBrush brush(Gdiplus::Color(130, 130, 130));
        Gdiplus::StringFormat sf;
        sf.SetAlignment(Gdiplus::StringAlignmentCenter);
        sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        Gdiplus::RectF rc((float)IMG_X, (float)IMG_Y, (float)IMG_W, (float)IMG_H);
        g.DrawString(L"No Image", -1, &font, rc, &sf, &brush);
    }
}

// ===== Process Image (crop + resize) =====
static void ProcessImage(const std::wstring& path) {
    delete g_processedImage;
    g_processedImage = nullptr;

    int origW, origH, cropW, cropH, outW, outH;
    g_processedImage = CropAndResize(path, origW, origH, cropW, cropH, outW, outH);

    if (g_processedImage) {
        wchar_t info[256];
        swprintf(info, 256, L"  %dx%d -> crop %dx%d -> resize %dx%d",
                 origW, origH, cropW, cropH, outW, outH);
        SetWindowTextW(g_hImageInfo, info);
    } else {
        SetWindowTextW(g_hImageInfo, L"  Failed to process image");
    }
}

// ===== Create Controls =====
static void CreateControls(HWND hWnd) {
    g_hFont = CreateFontW(15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
        0, 0, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    g_hFontBold = CreateFontW(15, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
        0, 0, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    auto MK = [&](const wchar_t* cls, const wchar_t* txt, DWORD style,
                   int x, int y, int w, int h, int id, bool bold = false) -> HWND {
        HWND hw = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style,
            x, y, w, h, hWnd, (HMENU)(INT_PTR)id, nullptr, nullptr);
        SendMessage(hw, WM_SETFONT, (WPARAM)(bold ? g_hFontBold : g_hFont), TRUE);
        return hw;
    };

    int x = CTL_X, y = 15;

    MK(L"STATIC", APP_TITLE, SS_LEFT, 15, 15, 300, 25, 0, true);

    // Model
    g_hModelLoad = MK(L"BUTTON", L"Load Model", BS_PUSHBUTTON, x, y, 130, 33, IDC_MODEL_LOAD, true);
    g_hModelStatus = MK(L"STATIC", L"  Not Loaded", SS_LEFT | SS_CENTERIMAGE,
        x + 140, y, 340, 33, IDC_MODEL_STATUS);
    y += 50;

    // Image
    MK(L"STATIC", L"Image (auto crop + bilinear resize)", SS_LEFT, x, y, 400, 20, 0);
    y += 22;
    g_hImagePath = MK(L"EDIT", L"", ES_AUTOHSCROLL | ES_READONLY | WS_BORDER,
        x, y, 400, 25, IDC_IMAGE_PATH);
    g_hImageBrowse = MK(L"BUTTON", L"Browse", BS_PUSHBUTTON, x + 408, y, 70, 25, IDC_IMAGE_BROWSE);
    y += 28;
    g_hImageInfo = MK(L"STATIC", L"", SS_LEFT, x, y, 480, 20, IDC_IMAGE_INFO);
    y += 30;

    // OCR
    g_hOcrBtn = MK(L"BUTTON", L"OCR", BS_PUSHBUTTON, x, y, 130, 40, IDC_OCR_BTN, true);
    EnableWindow(g_hOcrBtn, FALSE);
    g_hOcrTime = MK(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE, x + 140, y, 340, 40, IDC_OCR_TIME);
    y += 55;

    // Result
    MK(L"STATIC", L"OCR Result", SS_LEFT, x, y, 200, 20, 0);
    y += 22;
    g_hResult = MK(L"EDIT", L"",
        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER,
        x, y, 510, WIN_H - y - 20, IDC_RESULT);
}

static void UpdateOcrButton() {
    EnableWindow(g_hOcrBtn, g_modelLoaded && g_processedImage != nullptr);
}

// ===== Window Procedure =====
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        CreateControls(hWnd);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        DrawPreview(hdc);
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_IMAGE_BROWSE: {
            auto path = BrowseImage(hWnd);
            if (!path.empty()) {
                g_imagePath = path;
                SetWindowTextW(g_hImagePath, path.c_str());
                SetWindowTextW(g_hImageInfo, L"  Processing...");
                ProcessImage(path);
                InvalidateRect(hWnd, nullptr, TRUE);
                UpdateOcrButton();
            }
            break;
        }
        case IDC_MODEL_LOAD: {
            EnableWindow(g_hModelLoad, FALSE);
            SetWindowTextW(g_hModelStatus, L"  Loading...");
            std::thread(ModelLoadThread).detach();
            break;
        }
        case IDC_OCR_BTN: {
            if (!g_modelLoaded || !g_processedImage) break;
            EnableWindow(g_hOcrBtn, FALSE);
            SetWindowTextW(g_hOcrBtn, L"Processing...");
            SetWindowTextW(g_hOcrTime, L"");
            SetWindowTextW(g_hResult, L"");
            std::thread(OcrThread).detach();
            break;
        }
        }
        return 0;

    case WM_MODEL_READY:
        g_modelLoaded = true;
        SetWindowTextW(g_hModelStatus, L"  Ready");
        EnableWindow(g_hModelLoad, TRUE);
        UpdateOcrButton();
        return 0;

    case WM_MODEL_FAIL:
        SetWindowTextW(g_hModelStatus, Utf8ToWide("  FAIL: " + g_ocrResult).c_str());
        EnableWindow(g_hModelLoad, TRUE);
        return 0;

    case WM_OCR_DONE: {
        std::wstring result = Utf8ToWide(g_ocrResult);
        std::wstring display;
        for (wchar_t c : result) {
            if (c == L'\n') display += L"\r\n";
            else display += c;
        }
        SetWindowTextW(g_hResult, display.c_str());
        wchar_t timeBuf[64];
        swprintf(timeBuf, 64, L"  %.2f sec", g_ocrElapsed);
        SetWindowTextW(g_hOcrTime, timeBuf);
        SetWindowTextW(g_hOcrBtn, L"OCR");
        EnableWindow(g_hOcrBtn, TRUE);
        return 0;
    }

    case WM_OCR_FAIL:
        SetWindowTextW(g_hResult, Utf8ToWide(g_ocrResult).c_str());
        SetWindowTextW(g_hOcrBtn, L"OCR");
        EnableWindow(g_hOcrBtn, TRUE);
        return 0;

    case WM_CLOSE:
        FreeModel();
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        delete g_processedImage;
        g_processedImage = nullptr;
        DeleteObject(g_hFont);
        DeleteObject(g_hFontBold);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ===== WinMain =====
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    Gdiplus::GdiplusStartupInput gdipInput;
    ULONG_PTR gdipToken;
    Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, nullptr);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"QwenOCRClass";
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    RECT rc = { 0, 0, WIN_W, WIN_H };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    g_hWnd = CreateWindowExW(0, wc.lpszClassName, APP_TITLE,
        WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInst, nullptr);

    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    Gdiplus::GdiplusShutdown(gdipToken);
    CoUninitialize();
    return (int)msg.wParam;
}
