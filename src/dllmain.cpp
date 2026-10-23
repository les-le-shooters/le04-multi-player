// le04_multi —— 由 Le04_Multi.exe 的导入表加载（le04multi.dll），在游戏主线程启动前初始化
#include "mod.h"

static const uint32_t LE04_120F_TIMESTAMP = 1506432950;

extern "C" __declspec(dllexport) int MultiVersion() { return 1; }   // 供导入表引用

static DWORD WINAPI InitThread(LPVOID)
{
    Log("render hooks %s", InstallRenderHooks() ? "ok" : "failed");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    g_self = mod;
    DisableThreadLibraryCalls(mod);
    g_base = (uintptr_t)GetModuleHandleW(nullptr);
    GetModuleFileNameW(nullptr, g_dir, MAX_PATH);
    if (wchar_t* s = wcsrchr(g_dir, L'\\')) s[1] = 0;
    g_debug = GetFileAttributesW(GamePath(L"multi_debug").c_str()) != INVALID_FILE_ATTRIBUTES;
    if (const char* t = getenv("LE04MULTI_TAG"))
        for (const char* c = t; *c; ++c) g_tag += (wchar_t)*c;
    Log("---- le04_multi loaded, base=%p%s", (void*)g_base, g_debug ? " (debug)" : "");

    auto dos = (IMAGE_DOS_HEADER*)g_base;
    auto nt = (IMAGE_NT_HEADERS*)(g_base + dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != LE04_120F_TIMESTAMP) {
        MessageBoxW(nullptr, L"le04multi.dll：只支持 連縁天影戦記 1.20f", L"le04_multi", MB_ICONWARNING);
        return TRUE;
    }
    // 游戏代码补丁在主线程运行前装好（导入表加载时游戏还没开始跑）。内存池最先装：游戏的 CRT 初始化还没分配过内存
    Log("pool %s", InstallArena() ? "ok" : "failed");
    Log("coop hooks %s", InstallCoop() ? "ok" : "failed");
    Log("rollback hooks %s", InstallRollback() ? "ok" : "failed");
    Log("launcher hook %s", InstallLauncher() ? "ok" : "failed");
    *(int*)Va(0xC1F054) = 1;   // DxLib 的 DoubleStartValidFlag：允许同时开两个（本机测试联机用）
    if (g_debug) {
        InstallDebugVeh();
        Log("keyboard hook %s", InstallKeyboardHook() ? "ok" : "failed");
    }
    // d3d11 / dxgi 的钩子要建临时设备，不能在加载器锁里做
    if (HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr)) CloseHandle(t);
    return TRUE;
}
