// 渲染：挂 IDXGISwapChain::Present（DxLib 走 D3D11），每帧跑调试逻辑并用 ImGui 画 P2 的标记和状态（取自 lenprac）
#include "mod.h"
#include <d3d11.h>
#include <dxgi1_2.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

using DxgiPresentFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
using DxgiPresent1Fn = HRESULT(__stdcall*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using DxgiResizeFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

static DxgiPresentFn g_origPresent;
static DxgiPresent1Fn g_origPresent1;
static DxgiResizeFn g_origResize;

static bool g_ready = false, g_failed = false;
static IDXGISwapChain* g_swap;
static ID3D11Device* g_dev;
static ID3D11DeviceContext* g_ctx;
static ID3D11RenderTargetView* g_rtv;
static WNDPROC g_origWndProc;
static UINT g_bbWidth = 640, g_bbHeight = 480;
int g_fpsCap = 0;

// 游戏在后台也当作激活状态（吞掉失焦类消息）
static LRESULT CALLBACK HookWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    bool lose = (msg == WM_ACTIVATEAPP && !wp) || (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE)
        || msg == WM_KILLFOCUS || (msg == WM_NCACTIVATE && !wp);
    if (lose) return msg == WM_NCACTIVATE ? TRUE : 0;
    return CallWindowProcW(g_origWndProc, hwnd, msg, wp, lp);
}

static void CreateRtv()
{
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)))
        return;
    D3D11_TEXTURE2D_DESC d;
    bb->GetDesc(&d);
    g_bbWidth = d.Width;
    g_bbHeight = d.Height;
    g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
    bb->Release();
}

// 叠加层的字体：按界面语言选系统字体，只放当前语言用得到的字
static void BuildFonts(float size)
{
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    ImFontGlyphRangesBuilder rb;
    rb.AddRanges(io.Fonts->GetGlyphRangesDefault());
    std::string text = AllText();
    rb.AddText(text.c_str());
    static ImVector<ImWchar> ranges;
    ranges.clear();
    rb.BuildRanges(&ranges);
    static const wchar_t* const FONTS[LG_COUNT][5] = {
        { L"msyh.ttc", L"msyh.ttf", L"simhei.ttf", L"YuGothM.ttc", L"meiryo.ttc" },     // 中文
        { L"segoeui.ttf", L"msyh.ttc", L"meiryo.ttc", L"arial.ttf", nullptr },        // English
        { L"YuGothM.ttc", L"meiryo.ttc", L"msgothic.ttc", L"msyh.ttc", nullptr },     // 日本語
        { L"malgun.ttf", L"gulim.ttc", L"msyh.ttc", nullptr, nullptr },               // 한국어
    };
    wchar_t win[MAX_PATH];
    GetWindowsDirectoryW(win, MAX_PATH);
    for (auto c : FONTS[g_lang >= 0 && g_lang < LG_COUNT ? g_lang : LG_EN]) {
        if (!c) break;
        std::wstring p = std::wstring(win) + L"\\Fonts\\" + c;
        if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        char utf8[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, p.c_str(), -1, utf8, sizeof utf8, nullptr, nullptr);
        ImFontConfig cfg;
        cfg.OversampleH = 1;
        cfg.PixelSnapH = true;
        if (io.Fonts->AddFontFromFileTTF(utf8, size, &cfg, ranges.Data)) return;
    }
    io.Fonts->AddFontDefault();
}

static bool Init(IDXGISwapChain* sc)
{
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&g_dev))) {
        Log("swapchain is not D3D11");
        return false;
    }
    g_dev->GetImmediateContext(&g_ctx);
    DXGI_SWAP_CHAIN_DESC sd;
    sc->GetDesc(&sd);
    g_hwnd = sd.OutputWindow;
    g_swap = sc;
    CreateRtv();
    Log("present init: hwnd=%p backbuffer=%ux%u buffers=%u swapeffect=%d windowed=%d", g_hwnd, g_bbWidth, g_bbHeight, sd.BufferCount,
        (int)sd.SwapEffect, sd.Windowed);
    // 显示延迟：DXGI 默认允许 CPU 比显卡多排 3 帧，垂直同步时排着的帧会让画面晚 1～2 帧才出来。只允许排 1 帧
    IDXGIDevice1* dxgi = nullptr;
    if (SUCCEEDED(g_dev->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxgi))) {
        int lat = 1;
        if (g_debug)
            if (const char* e = getenv("LE04MULTI_FRAMELAT")) lat = atoi(e);   // 调试：0 = 不改（对比用）
        if (lat > 0) dxgi->SetMaximumFrameLatency(lat);
        Log("max frame latency %d", lat);
        dxgi->Release();
    }

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NoMouse;
    ImGui::StyleColorsDark();
    float scale = g_bbHeight / 480.0f;
    if (scale < 1) scale = 1;
    ImGui::GetStyle().ScaleAllSizes(scale);
    BuildFonts(14.0f * scale);
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
    // 调试 / 联机：窗口失焦也照常运行（联机时一边切出去另一边不能停）
    if (g_debug || NetRole() == NET_HOST || NetRole() == NET_GUEST)
        g_origWndProc = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)HookWndProc);
    return true;
}

static thread_local int t_depth = 0;   // Present1 内部可能再调 Present，避免一帧跑两次

static void Frame(IDXGISwapChain* sc)
{
    if (t_depth > 1) return;
    if (!g_ready) {
        if (g_failed || !Init(sc)) { g_failed = true; return; }
        g_ready = true;
    }
    if (sc != g_swap) return;
    if (!g_rtv) CreateRtv();
    ++g_frame;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::GetIO().DisplaySize = ImVec2((float)g_bbWidth, (float)g_bbHeight);
    ImGui::NewFrame();
    DrawCoopUi();
    ImGui::Render();
    if (g_rtv) {
        ID3D11RenderTargetView* oldRtv = nullptr;
        ID3D11DepthStencilView* oldDsv = nullptr;
        g_ctx->OMGetRenderTargets(1, &oldRtv, &oldDsv);
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_ctx->OMSetRenderTargets(1, &oldRtv, oldDsv);
        if (oldRtv) oldRtv->Release();
        if (oldDsv) oldDsv->Release();
    }
    if (g_debug) DebugTick();
}

bool CaptureBackbuffer(std::vector<uint8_t>& bgra, UINT& w, UINT& h)
{
    ID3D11Texture2D* bb = nullptr;
    if (!g_swap || FAILED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) return false;
    D3D11_TEXTURE2D_DESC d;
    bb->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    bool ok = false;
    if (SUCCEEDED(g_dev->CreateTexture2D(&d, nullptr, &st))) {
        g_ctx->CopyResource(st, bb);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            w = d.Width;
            h = d.Height;
            bgra.resize((size_t)w * h * 4);
            bool bgrSrc = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8X8_UNORM;
            for (UINT y = 0; y < h; ++y) {
                auto src = (const uint8_t*)m.pData + y * m.RowPitch;
                auto dst = &bgra[(size_t)y * w * 4];
                for (UINT x = 0; x < w; ++x) {
                    dst[x * 4 + 0] = src[x * 4 + (bgrSrc ? 0 : 2)];
                    dst[x * 4 + 1] = src[x * 4 + 1];
                    dst[x * 4 + 2] = src[x * 4 + (bgrSrc ? 2 : 0)];
                    dst[x * 4 + 3] = 255;
                }
            }
            g_ctx->Unmap(st, 0);
            ok = true;
        }
        st->Release();
    }
    bb->Release();
    return ok;
}

// 调试：Present 里阻塞了多久（排队满了时会等到有空位），每 300 帧记一次
static void PresentStats(UINT sync, LONGLONG ticks)
{
    if (!g_debug) return;
    static LARGE_INTEGER freq;
    static LONGLONG sum, worst;
    static int n;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    sum += ticks;
    if (ticks > worst) worst = ticks;
    if (++n == 300) {
        Log("present: sync %u, blocked avg %.2f ms, max %.2f ms", sync, sum * 1000.0 / freq.QuadPart / n, worst * 1000.0 / freq.QuadPart);
        sum = worst = 0;
        n = 0;
    }
}

static HRESULT __stdcall HookPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    t_depth++;
    if (!(flags & DXGI_PRESENT_TEST)) Frame(sc);
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    HRESULT hr = g_origPresent(sc, sync, flags);
    QueryPerformanceCounter(&b);
    if (!(flags & DXGI_PRESENT_TEST) && t_depth == 1) PresentStats(sync, b.QuadPart - a.QuadPart);
    t_depth--;
    return hr;
}

static HRESULT __stdcall HookPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p)
{
    t_depth++;
    if (!(flags & DXGI_PRESENT_TEST)) Frame(sc);
    HRESULT hr = g_origPresent1(sc, sync, flags, p);
    t_depth--;
    return hr;
}

static HRESULT __stdcall HookResize(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl)
{
    if (sc == g_swap && g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    HRESULT hr = g_origResize(sc, n, w, h, f, fl);
    if (sc == g_swap && g_ready) CreateRtv();
    return hr;
}

bool InstallRenderHooks()
{
    WNDCLASSEXW wc { sizeof wc, CS_CLASSDC, DefWindowProcW, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr, L"le04multi_dummy" };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC sd {};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = E_FAIL;
    for (D3D_DRIVER_TYPE dt : { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP }) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, dt, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
        if (SUCCEEDED(hr)) break;
    }
    bool ok = false;
    if (SUCCEEDED(hr)) {
        void** vt = *(void***)sc;
        ok = InlineHookHotpatch(vt[8], HookPresent, (void**)&g_origPresent);
        InlineHookHotpatch(vt[13], HookResize, (void**)&g_origResize);
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1))) {
            InlineHookHotpatch((*(void***)sc1)[22], HookPresent1, (void**)&g_origPresent1);
            sc1->Release();
        }
        sc->Release();
        dev->Release();
        ctx->Release();
    } else {
        Log("D3D11CreateDeviceAndSwapChain failed %08X", hr);
    }
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}
