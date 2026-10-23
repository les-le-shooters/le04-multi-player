// 基础工具：日志、内存读写、代码补丁、内联钩子（取自 le04_mitori / lenprac）
#include "mod.h"
#include <cstdarg>
#include <cstdio>

HMODULE g_self;
uintptr_t g_base;
wchar_t g_dir[MAX_PATH];
bool g_debug = false;
HWND g_hwnd;
int g_frame = 0;

std::wstring g_tag;   // 调试文件名后缀（本机双开测试时两边各用各的，环境变量 LE04MULTI_TAG）

std::wstring DebugFile(const wchar_t* suffix)
{
    return GamePath((L"multi" + g_tag + suffix).c_str());
}

// 日志：只在调试模式（游戏目录里有 multi_debug）写 multi.log，平时不写任何文件
void Log(const char* fmt, ...)
{
    if (!g_debug) return;
    char line[1024];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = snprintf(line, sizeof line, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof line - n, fmt, ap);
    va_end(ap);
    if (FILE* f = _wfopen(DebugFile(L".log").c_str(), L"a")) {
        fputs(line + n, f);
        fputc('\n', f);
        fclose(f);
    }
}

std::wstring GamePath(const wchar_t* name)
{
    return std::wstring(g_dir) + name;
}

bool ReadU32(uintptr_t a, uint32_t* out)
{
    __try {
        *out = *(uint32_t*)a;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteU32(uintptr_t a, uint32_t v)
{
    if (!a) return false;
    __try {
        *(uint32_t*)a = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::vector<uint8_t> Hex(const char* s)
{
    std::vector<uint8_t> v;
    for (; s[0] && s[1]; s += 2)
        v.push_back((uint8_t)strtoul(std::string(s, 2).c_str(), nullptr, 16));
    return v;
}

bool WriteCode(uintptr_t addr, const uint8_t* data, size_t n)
{
    DWORD old;
    if (!VirtualProtect((LPVOID)addr, n, PAGE_EXECUTE_READWRITE, &old))
        return false;
    memcpy((void*)addr, data, n);
    VirtualProtect((LPVOID)addr, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (LPVOID)addr, n);
    return true;
}

bool BytesAt(uintptr_t addr, const std::vector<uint8_t>& b)
{
    return memcmp((void*)addr, b.data(), b.size()) == 0;
}

bool InlineHookN(void* target, void* detour, void** orig, size_t n)
{
    auto t = (uint8_t*)target;
    auto tramp = (uint8_t*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp || n < 5 || n > 16) return false;
    memcpy(tramp, t, n);
    tramp[n] = 0xE9;
    int32_t back = (int32_t)((t + n) - (tramp + n + 5));
    memcpy(tramp + n + 1, &back, 4);
    if (orig) *orig = tramp;
    uint8_t jmp[16];
    memset(jmp, 0x90, sizeof jmp);
    jmp[0] = 0xE9;
    int32_t rel = (int32_t)((uint8_t*)detour - (t + 5));
    memcpy(jmp + 1, &rel, 4);
    return WriteCode((uintptr_t)t, jmp, n);
}

bool InlineHookHotpatch(void* target, void* detour, void** orig)
{
    static std::vector<void*> hooked;
    for (void* h : hooked)
        if (h == target) return false;
    if (!BytesAt((uintptr_t)target, Hex("8BFF558BEC"))) {
        Log("inline hook: unexpected prologue at %p", target);
        return false;
    }
    if (!InlineHookN(target, detour, orig, 5)) return false;
    hooked.push_back(target);
    return true;
}

// 把 rva 处的 call rel32（原目标 expectRva）改成 call target
bool PatchCall(uint32_t rva, void* target, uint32_t expectRva)
{
    uintptr_t at = Rva(rva);
    if (*(uint8_t*)at != 0xE8) { Log("patch call %X: not a call", rva); return false; }
    uintptr_t cur = at + 5 + *(int32_t*)(at + 1);
    if (cur != Rva(expectRva)) { Log("patch call %X: unexpected target %p", rva, (void*)cur); return false; }
    uint8_t buf[5] = { 0xE8 };
    *(int32_t*)(buf + 1) = (int32_t)((uintptr_t)target - (at + 5));
    return WriteCode(at, buf, 5);
}
