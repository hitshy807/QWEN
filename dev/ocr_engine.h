/*
 * ocr_engine.h - Qwen3.5 OCR Engine (native llama.cpp)
 *
 * Model loading, inference, prompt configuration.
 * Edit this file to change OCR behavior without touching GUI code.
 */
#pragma once

#define NOMINMAX
#define LLAMA_SHARED
#include <windows.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <chrono>
#include <algorithm>

#include "llama.h"
#include "mtmd.h"
#include "mtmd-helper.h"

// ===== OCR Configuration =====
static const int OCR_MAX_TOKENS  = 256;
static const int OCR_N_CTX       = 1536;
static const int OCR_N_GPU_LAYERS = 99;
static const int BLOB_THRESHOLD  = 180;
static const int CROP_PADDING    = 10;

// ===== OCR Engine State =====
static llama_model*       g_model    = nullptr;
static llama_context*     g_lctx     = nullptr;
static mtmd_context*      g_mtmd_ctx = nullptr;
static const llama_vocab* g_vocab    = nullptr;
static bool               g_modelLoaded = false;

// ===== String Conversion =====
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

// ===== File Utilities =====
static std::wstring GetExeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path(buf);
    return path.substr(0, path.rfind(L'\\'));
}

static bool FileExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
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

    outW = cropW / 2;
    outH = cropH / 2;
    if (outW < 100) { outW = cropW; outH = cropH; }

    // Crop
    Gdiplus::Bitmap* cropped = src.Clone(minX, minY, cropW, cropH, PixelFormat32bppARGB);
    if (!cropped || cropped->GetLastStatus() != Gdiplus::Ok) {
        delete cropped;
        return nullptr;
    }

    // Resize with bilinear interpolation
    auto* result = new Gdiplus::Bitmap(outW, outH, PixelFormat32bppARGB);
    Gdiplus::Graphics gr(result);
    gr.SetInterpolationMode(Gdiplus::InterpolationModeBilinear);
    gr.DrawImage(cropped, 0, 0, outW, outH);
    delete cropped;

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
            rgb[(y * w + x) * 3 + 0] = row[x * 3 + 2]; // R
            rgb[(y * w + x) * 3 + 1] = row[x * 3 + 1]; // G
            rgb[(y * w + x) * 3 + 2] = row[x * 3 + 0]; // B
        }
    }
    bmp->UnlockBits(&data);
    return rgb;
}

// ===== Free Model =====
static void OcrFreeModel() {
    g_modelLoaded = false;
    if (g_mtmd_ctx) { mtmd_free(g_mtmd_ctx); g_mtmd_ctx = nullptr; }
    if (g_lctx) { llama_free(g_lctx); g_lctx = nullptr; }
    if (g_model) { llama_model_free(g_model); g_model = nullptr; }
    g_vocab = nullptr;
}

// ===== Load Model =====
// Returns empty string on success, error message on failure.
static std::string OcrLoadModel() {
    std::wstring exeDir = GetExeDir();

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
    if (modelPath.empty())
        return "Model not found. Place Qwen3.5-2B-Q4_K_M.gguf + mmproj-F16.gguf in exe/models/";

    OcrFreeModel();

    std::string modelUtf8 = WideToUtf8(modelPath);
    std::string mmprojUtf8 = WideToUtf8(mmprojPath);

    // Load model
    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = OCR_N_GPU_LAYERS;
    g_model = llama_model_load_from_file(modelUtf8.c_str(), mparams);
    if (!g_model) return "Failed to load model";
    g_vocab = llama_model_get_vocab(g_model);

    // Create context
    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = OCR_N_CTX;
    cparams.n_batch = OCR_N_CTX;
    cparams.n_threads = (int)std::thread::hardware_concurrency();
    cparams.n_threads_batch = (int)std::thread::hardware_concurrency();
    cparams.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED;
    cparams.type_k = GGML_TYPE_Q4_0;
    cparams.type_v = GGML_TYPE_Q4_0;
    g_lctx = llama_init_from_model(g_model, cparams);
    if (!g_lctx) { OcrFreeModel(); return "Failed to create context"; }

    // Create multimodal context
    mtmd_context_params mtparams = mtmd_context_params_default();
    mtparams.use_gpu = true;
    mtparams.print_timings = false;
    mtparams.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED;
    mtparams.n_threads = (int)std::thread::hardware_concurrency();
    mtparams.warmup = true;
    g_mtmd_ctx = mtmd_init_from_file(mmprojUtf8.c_str(), g_model, mtparams);
    if (!g_mtmd_ctx) { OcrFreeModel(); return "Failed to load vision encoder"; }

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
    return "";
}

// ===== Run OCR =====
// Returns OCR text result. Sets elapsed time in seconds.
static std::string OcrRunInference(Gdiplus::Bitmap* processedImage, double& elapsed) {
    if (!processedImage || !g_modelLoaded)
        return "";

    // Extract RGB from processed image
    uint32_t imgW, imgH;
    auto rgb = BitmapToRGB(processedImage, imgW, imgH);

    // Create mtmd bitmap
    mtmd_bitmap* bmp = mtmd_bitmap_init(imgW, imgH, rgb.data());
    if (!bmp) return "";

    // Build prompt
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
    if (bufLen < 0) { mtmd_bitmap_free(bmp); return ""; }

    std::vector<char> buf(bufLen + 1);
    llama_chat_apply_template(tmpl, msgs, 1, true, buf.data(), (int32_t)buf.size());
    std::string prompt(buf.data(), bufLen);
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
    if (res != 0) { mtmd_input_chunks_free(chunks); return ""; }

    // Clear KV cache
    llama_memory_clear(llama_get_memory(g_lctx), true);

    auto t0 = std::chrono::high_resolution_clock::now();

    // Eval all chunks (text + image)
    llama_pos n_past = 0;
    llama_pos new_n_past;
    res = mtmd_helper_eval_chunks(g_mtmd_ctx, g_lctx, chunks,
        n_past, 0, OCR_N_CTX, true, &new_n_past);
    mtmd_input_chunks_free(chunks);
    if (res != 0) return "";
    n_past = new_n_past;

    // Greedy sampler (temperature = 0)
    auto sparams = llama_sampler_chain_default_params();
    llama_sampler* smpl = llama_sampler_chain_init(sparams);
    llama_sampler_chain_add(smpl, llama_sampler_init_greedy());

    // Generate tokens
    std::string result;
    llama_batch batch = llama_batch_init(1, 0, 1);

    for (int i = 0; i < OCR_MAX_TOKENS; i++) {
        llama_token token_id = llama_sampler_sample(smpl, g_lctx, -1);
        if (llama_vocab_is_eog(g_vocab, token_id)) break;

        char piece[256];
        int n = llama_token_to_piece(g_vocab, token_id, piece, sizeof(piece), 0, false);
        if (n > 0) result.append(piece, n);

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
    elapsed = std::chrono::duration<double>(t1 - t0).count();

    // Trim leading whitespace/newlines
    while (!result.empty() && (result[0] == '\n' || result[0] == ' ')) result.erase(0, 1);

    return result;
}
