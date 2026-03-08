// Qwen3.5 OCR GUI - ImGui + DirectX11 Modern Dashboard
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <tchar.h>
#include <string>
#include <thread>
#include <mutex>
#include <gdiplus.h>
#include <commdlg.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "ocr_engine.h"

// Data
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

static Gdiplus::Bitmap* g_processedImage = nullptr;
static ID3D11ShaderResourceView* g_pTextureView = nullptr;
static int g_imgWidth = 0;
static int g_imgHeight = 0;

static std::string g_imagePathStr;
static std::wstring g_imagePath;
static std::string g_ocrResult = "Ready.";
static double g_ocrElapsed = 0;

static bool g_isModelLoading = false;
static bool g_isOcrRunning = false;
static std::mutex g_stateMutex;

// Forward declarations
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Utility to create DX11 texture from GDI+ Bitmap
void LoadTextureFromGdiplus(Gdiplus::Bitmap* bmp, ID3D11Device* d3dDevice, ID3D11ShaderResourceView** out_srv, int* out_width, int* out_height) {
    if (*out_srv) { (*out_srv)->Release(); *out_srv = nullptr; }
    if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) return;

    UINT width = bmp->GetWidth();
    UINT height = bmp->GetHeight();

    Gdiplus::Rect rect(0, 0, width, height);
    Gdiplus::BitmapData bmpData;
    bmp->LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bmpData);

    // Create texture
    D3D11_TEXTURE2D_DESC desc;
    ZeroMemory(&desc, sizeof(desc));
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;

    D3D11_SUBRESOURCE_DATA subResource;
    subResource.pSysMem = bmpData.Scan0;
    subResource.SysMemPitch = bmpData.Stride;
    subResource.SysMemSlicePitch = 0;

    ID3D11Texture2D* pTexture = nullptr;
    d3dDevice->CreateTexture2D(&desc, &subResource, &pTexture);
    bmp->UnlockBits(&bmpData);

    if (pTexture) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
        ZeroMemory(&srvDesc, sizeof(srvDesc));
        srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = desc.MipLevels;
        srvDesc.Texture2D.MostDetailedMip = 0;
        d3dDevice->CreateShaderResourceView(pTexture, &srvDesc, out_srv);
        pTexture->Release();
    }
    *out_width = width;
    *out_height = height;
}

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

void ProcessImage(const std::wstring& path) {
    delete g_processedImage;
    g_processedImage = nullptr;

    // Display original in INPUT SOURCE
    Gdiplus::Bitmap original(path.c_str());
    if (original.GetLastStatus() == Gdiplus::Ok)
        LoadTextureFromGdiplus(&original, g_pd3dDevice, &g_pTextureView, &g_imgWidth, &g_imgHeight);

    // Crop+resize for OCR engine
    int origW, origH, cropW, cropH, outW, outH;
    g_processedImage = CropAndResize(path, origW, origH, cropW, cropH, outW, outH);
}

void SetupImGuiStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    // Cyberpunk neon theme - deep black + cyan/magenta neon
    colors[ImGuiCol_Text]                   = ImVec4(0.00f, 0.90f, 0.88f, 1.00f); // Cyan text
    colors[ImGuiCol_TextDisabled]           = ImVec4(0.35f, 0.35f, 0.40f, 1.00f);
    colors[ImGuiCol_WindowBg]               = ImVec4(0.04f, 0.04f, 0.08f, 1.00f); // Near black
    colors[ImGuiCol_ChildBg]                = ImVec4(0.06f, 0.06f, 0.10f, 1.00f);
    colors[ImGuiCol_PopupBg]                = ImVec4(0.05f, 0.05f, 0.08f, 0.96f);
    colors[ImGuiCol_Border]                 = ImVec4(0.00f, 0.70f, 0.68f, 0.40f); // Cyan border
    colors[ImGuiCol_BorderShadow]           = ImVec4(0.00f, 0.30f, 0.30f, 0.15f);
    colors[ImGuiCol_FrameBg]                = ImVec4(0.08f, 0.08f, 0.14f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.12f, 0.12f, 0.20f, 1.00f);
    colors[ImGuiCol_FrameBgActive]          = ImVec4(0.16f, 0.16f, 0.25f, 1.00f);
    colors[ImGuiCol_TitleBg]                = ImVec4(0.04f, 0.04f, 0.08f, 1.00f);
    colors[ImGuiCol_TitleBgActive]          = ImVec4(0.06f, 0.06f, 0.12f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]       = ImVec4(0.03f, 0.03f, 0.06f, 1.00f);
    colors[ImGuiCol_MenuBarBg]              = ImVec4(0.06f, 0.06f, 0.10f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]            = ImVec4(0.03f, 0.03f, 0.06f, 0.60f);
    colors[ImGuiCol_ScrollbarGrab]          = ImVec4(0.00f, 0.50f, 0.48f, 0.60f);
    colors[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(0.00f, 0.70f, 0.68f, 0.80f);
    colors[ImGuiCol_ScrollbarGrabActive]    = ImVec4(0.00f, 0.90f, 0.88f, 1.00f);
    colors[ImGuiCol_CheckMark]              = ImVec4(0.00f, 1.00f, 0.98f, 1.00f);
    colors[ImGuiCol_SliderGrab]             = ImVec4(0.00f, 0.60f, 0.58f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]       = ImVec4(0.00f, 1.00f, 0.98f, 1.00f);
    colors[ImGuiCol_Button]                 = ImVec4(0.10f, 0.10f, 0.18f, 1.00f);
    colors[ImGuiCol_ButtonHovered]          = ImVec4(0.00f, 0.80f, 0.78f, 0.50f); // Cyan glow hover
    colors[ImGuiCol_ButtonActive]           = ImVec4(0.90f, 0.10f, 0.50f, 0.90f); // Magenta active
    colors[ImGuiCol_Header]                 = ImVec4(0.10f, 0.10f, 0.18f, 1.00f);
    colors[ImGuiCol_HeaderHovered]          = ImVec4(0.00f, 0.70f, 0.68f, 0.50f);
    colors[ImGuiCol_HeaderActive]           = ImVec4(0.00f, 0.90f, 0.88f, 0.70f);
    colors[ImGuiCol_Separator]              = ImVec4(0.00f, 0.50f, 0.48f, 0.40f); // Cyan separator
    colors[ImGuiCol_SeparatorHovered]       = ImVec4(0.90f, 0.10f, 0.50f, 0.70f);
    colors[ImGuiCol_SeparatorActive]        = ImVec4(0.90f, 0.10f, 0.50f, 1.00f);
    colors[ImGuiCol_ResizeGrip]             = ImVec4(0.00f, 0.50f, 0.48f, 0.40f);
    colors[ImGuiCol_ResizeGripHovered]      = ImVec4(0.00f, 0.90f, 0.88f, 0.70f);
    colors[ImGuiCol_ResizeGripActive]       = ImVec4(0.90f, 0.10f, 0.50f, 1.00f);
    colors[ImGuiCol_Tab]                    = ImVec4(0.08f, 0.08f, 0.14f, 1.00f);
    colors[ImGuiCol_TabHovered]             = ImVec4(0.90f, 0.10f, 0.50f, 0.60f);
    colors[ImGuiCol_TabActive]              = ImVec4(0.12f, 0.12f, 0.20f, 1.00f);
    colors[ImGuiCol_TabUnfocused]           = ImVec4(0.06f, 0.06f, 0.10f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive]     = ImVec4(0.08f, 0.08f, 0.14f, 1.00f);

    style.WindowRounding    = 0.0f;  // Sharp edges = cyberpunk
    style.ChildRounding     = 2.0f;
    style.FrameRounding     = 2.0f;
    style.PopupRounding     = 2.0f;
    style.ScrollbarRounding = 0.0f;
    style.GrabRounding      = 0.0f;
    style.TabRounding       = 0.0f;
    style.WindowBorderSize  = 1.0f;
    style.ChildBorderSize   = 1.0f;
    style.FrameBorderSize   = 1.0f;
    style.ItemSpacing       = ImVec2(12, 8);
    style.FramePadding      = ImVec2(10, 6);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    // GDI+ Init for image processing
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance, nullptr, nullptr, nullptr, nullptr, L"ImGui OCR", nullptr };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"LABEL OCR 1.0", WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX, 100, 100, 1200, 800, nullptr, nullptr, wc.hInstance, nullptr);

    // Dark title bar / window frame
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &darkMode, sizeof(darkMode));

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ::ShowWindow(hwnd, nCmdShow);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    
    // Load font - Consolas Bold for cyberpunk terminal feel
    ImFontConfig font_cfg;
    font_cfg.OversampleH = 3;
    font_cfg.OversampleV = 2;
    font_cfg.RasterizerDensity = 1.5f;
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consolab.ttf", 18.0f, &font_cfg, io.Fonts->GetGlyphRangesDefault());

    SetupImGuiStyle();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    ImVec4 clear_color = ImVec4(0.05f, 0.05f, 0.07f, 1.00f);

    bool done = false;
    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        // Yield CPU/GPU resources so the OCR engine (CUDA/CPU) isn't starved.
        if (g_isOcrRunning || g_isModelLoading) {
            // Throttle GUI to ~15 FPS during heavy processing
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
        } else {
            // Tiny sleep to prevent 100% single-core usage if VSync is disabled or missed
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Main Dashboard Window taking full screen
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("Dashboard", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);

        // Custom Title Bar
        ImGui::SetCursorPos(ImVec2(10, 10));
        ImGui::TextColored(ImVec4(0.00f, 1.0f, 0.98f, 1.0f), "[ LABEL OCR 1.0 ]");
        ImGui::SameLine(ImGui::GetWindowWidth() - 40);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.2f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        if (ImGui::Button("X", ImVec2(30, 24))) {
            ::PostMessage(hwnd, WM_CLOSE, 0, 0);
        }
        ImGui::PopStyleColor(3);
        ImGui::Separator();
        ImGui::Spacing();

        float topH = io.DisplaySize.y * 0.45f;

        // ===== Top Row: INPUT SOURCE | SYSTEM CONTROLS =====
        // Left: Image Preview
        ImGui::BeginChild("ImagePanel", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, topH), true);
        ImGui::TextColored(ImVec4(0.90f, 0.10f, 0.50f, 1.0f), ">> INPUT SOURCE");
        ImGui::Separator();
        ImGui::Spacing();

        if (g_pTextureView) {
            float availW = ImGui::GetContentRegionAvail().x;
            float availH = ImGui::GetContentRegionAvail().y;
            float aspect = (float)g_imgWidth / (float)g_imgHeight;
            float drawW = availW;
            float drawH = availW / aspect;
            if (drawH > availH) { drawH = availH; drawW = availH * aspect; }
            ImGui::Image((void*)g_pTextureView, ImVec2(drawW, drawH));
        } else {
            ImGui::TextColored(ImVec4(0.35f, 0.35f, 0.40f, 1.0f), "[ NO SIGNAL ]");
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // Right: System Controls
        ImGui::BeginChild("ControlPanel", ImVec2(0, topH), true);
        ImGui::TextColored(ImVec4(0.90f, 0.10f, 0.50f, 1.0f), ">> SYSTEM CONTROLS");
        ImGui::Separator();
        ImGui::Spacing();

        // Model Load
        bool canLoad = !g_isModelLoading && !g_isOcrRunning;
        if (!canLoad) {
            ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
        }
        if (ImGui::Button("INIT MODEL", ImVec2(180, 40))) {
            g_isModelLoading = true;
            std::thread([]() {
                std::string res = OcrLoadModel();
                std::lock_guard<std::mutex> lock(g_stateMutex);
                if (res.empty()) g_ocrResult = "Model Loaded successfully.";
                else g_ocrResult = "Error: " + res;
                g_isModelLoading = false;
            }).detach();
        }
        if (!canLoad) { ImGui::PopItemFlag(); ImGui::PopStyleVar(); }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        if (g_modelLoaded) ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "ONLINE");
        else ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "OFFLINE");

        ImGui::Spacing();

        // Image Selection
        if (ImGui::Button("BROWSE IMAGE", ImVec2(180, 40))) {
            std::wstring path = BrowseImage(hwnd);
            if (!path.empty()) {
                g_imagePath = path;
                g_imagePathStr = WideToUtf8(path);
                ProcessImage(path);
            }
        }
        ImGui::SameLine();
        ImGui::TextWrapped("%s", g_imagePathStr.empty() ? "No file" : g_imagePathStr.c_str());

        ImGui::Spacing();

        // OCR Action
        bool canOcr = g_modelLoaded && g_processedImage && !g_isOcrRunning && !g_isModelLoading;
        if (!canOcr) {
            ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
        }
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.9f, 0.16f, 0.55f, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.2f, 0.6f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.8f, 0.1f, 0.5f, 1.0f));
        if (ImGui::Button("RUN EXTRACTION", ImVec2(ImGui::GetContentRegionAvail().x, 50))) {
            g_isOcrRunning = true;
            g_ocrResult = "Processing...";
            std::thread([]() {
                double elapsed = 0;
                std::string res = OcrRunInference(g_processedImage, elapsed);
                std::lock_guard<std::mutex> lock(g_stateMutex);
                g_ocrElapsed = elapsed;
                g_ocrResult = res.empty() ? "No output generated" : res;
                g_isOcrRunning = false;
            }).detach();
        }
        ImGui::PopStyleColor(3);
        if (!canOcr) { ImGui::PopItemFlag(); ImGui::PopStyleVar(); }

        if (g_ocrElapsed > 0) {
            ImGui::TextColored(ImVec4(0.9f, 0.16f, 0.55f, 1.0f), "Time: %.2f sec", g_ocrElapsed);
        }
        ImGui::EndChild();

        ImGui::Spacing();

        // ===== Bottom: OUTPUT DATA (full width) =====
        ImGui::BeginChild("OutputPanel", ImVec2(0, 0), true);
        ImGui::TextColored(ImVec4(0.90f, 0.10f, 0.50f, 1.0f), ">> OUTPUT DATA");
        ImGui::Separator();

        std::string resultCopy;
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            resultCopy = g_ocrResult;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.94f, 0.96f, 1.0f));
        ImGui::InputTextMultiline("##Result", &resultCopy[0], resultCopy.size() + 1,
            ImVec2(-FLT_MIN, ImGui::GetContentRegionAvail().y), ImGuiInputTextFlags_ReadOnly);
        ImGui::PopStyleColor();

        ImGui::EndChild();

        ImGui::End();

        // Render
        ImGui::Render();
        const float clear_color_with_alpha[4] = { clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_pSwapChain->Present(1, 0); // Present with vsync
    }

    // Cleanup
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    OcrFreeModel();
    if (g_pTextureView) { g_pTextureView->Release(); g_pTextureView = nullptr; }
    delete g_processedImage; g_processedImage = nullptr;

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

    Gdiplus::GdiplusShutdown(gdiplusToken);

    return 0;
}

// DX11 Boilerplate
bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext) != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_NCHITTEST: {
        LRESULT hit = ::DefWindowProcW(hWnd, msg, wParam, lParam);
        if (hit == HTCLIENT) {
            POINT pt;
            pt.x = (short)LOWORD(lParam);
            pt.y = (short)HIWORD(lParam);
            ::ScreenToClient(hWnd, &pt);
            RECT rc;
            ::GetClientRect(hWnd, &rc);
            // Allow dragging from the top 40 pixels (excluding the close button area on the right)
            if (pt.y >= 0 && pt.y < 40 && pt.x >= 0 && pt.x < rc.right - 50) {
                return HTCAPTION;
            }
        }
        return hit;
    }
    case WM_SIZE:
        if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}