// 调试模式（游戏目录有 multi_debug 文件）专用：截图、读写内存、硬件断点。供 tools/game.py 自动测试和动态逆向。
// multi_cmd.txt 每行一条（数值均为十六进制，地址可写 VA，或 "r+RVA"）：
//   dump <addr> <len>              以 dword / float 打印
//   write <addr> <val>             写 dword；writef <addr> <float(十进制)>
//   watch|watchrw <addr>           DR0 数据断点：记录写入（或读写）该地址的指令（每个 EIP 一次，最多 64）
//   bp <addr> [次数]               DR1-3 执行断点：命中时记录寄存器和栈顶 12 个 dword（默认 1 次）
//   bpclear                        清掉所有断点
// multi_shot 文件出现时，把后台缓冲存成 multi_shot.bmp
#include "mod.h"
#include <psapi.h>
#include <cstdio>
#include <cstring>

static void DebugScreenshot()
{
    std::wstring trig = DebugFile(L"_shot");
    if (GetFileAttributesW(trig.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    std::vector<uint8_t> px;
    UINT w = 0, h = 0;
    if (CaptureBackbuffer(px, w, h)) {
        if (FILE* f = _wfopen(DebugFile(L"_shot.bmp").c_str(), L"wb")) {
            BITMAPFILEHEADER fh {};
            BITMAPINFOHEADER ih {};
            ih.biSize = sizeof ih;
            ih.biWidth = w;
            ih.biHeight = -(LONG)h;
            ih.biPlanes = 1;
            ih.biBitCount = 32;
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof fh + sizeof ih;
            fh.bfSize = fh.bfOffBits + (DWORD)px.size();
            fwrite(&fh, sizeof fh, 1, f);
            fwrite(&ih, sizeof ih, 1, f);
            fwrite(px.data(), px.size(), 1, f);
            fclose(f);
        }
    }
    DeleteFileW(trig.c_str());
}

// ------------------------------------------------------------------ 硬件断点
struct Bp { uintptr_t addr = 0; int left = 0; };
static uintptr_t s_watch = 0;
static bool s_watchRw = false;
static std::vector<uint32_t> s_watchHits;
static Bp s_bp[3];   // DR1..DR3

static void LogCtx(const char* tag, CONTEXT* c)
{
    uint32_t st[12] = {};
    for (int i = 0; i < 12; ++i) ReadU32(c->Esp + i * 4, &st[i]);
    Log("%s eip=%08X (rva %X) eax=%08X ecx=%08X edx=%08X ebx=%08X esi=%08X edi=%08X ebp=%08X esp=%08X",
        tag, c->Eip, c->Eip - (uint32_t)g_base, c->Eax, c->Ecx, c->Edx, c->Ebx, c->Esi, c->Edi, c->Ebp, c->Esp);
    Log("   stack: %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X",
        st[0], st[1], st[2], st[3], st[4], st[5], st[6], st[7], st[8], st[9], st[10], st[11]);
}

static uint32_t Dr7Value()
{
    uint32_t v = 0;
    if (s_watch) v |= 1 | ((s_watchRw ? 3u : 1u) << 16) | (3u << 18);       // DR0：写 / 读写，4 字节
    for (int i = 0; i < 3; ++i)
        if (s_bp[i].addr && s_bp[i].left > 0) v |= 1u << ((i + 1) * 2);       // DR1..3：执行（RW=00 LEN=00）
    return v;
}

static void ApplyDr(CONTEXT* c)
{
    c->Dr0 = s_watch;
    c->Dr1 = s_bp[0].addr;
    c->Dr2 = s_bp[1].addr;
    c->Dr3 = s_bp[2].addr;
    c->Dr7 = Dr7Value();
}

static LONG NTAPI DebugVeh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    uint32_t dr6 = c->Dr6;
    if (!(dr6 & 0xF)) return EXCEPTION_CONTINUE_SEARCH;
    if (dr6 & 1) {
        uint32_t eip = c->Eip;
        bool seen = false;
        for (auto e : s_watchHits) seen |= e == eip;
        if (!seen && s_watchHits.size() < 64) {
            s_watchHits.push_back(eip);
            char tag[32];
            sprintf_s(tag, "watch(phase %d)", RbDebugPhase());
            LogCtx(tag, c);
        }
    }
    for (int i = 0; i < 3; ++i) {
        if (!(dr6 & (2u << i))) continue;
        char tag[32];
        sprintf_s(tag, "bp%d", i + 1);
        LogCtx(tag, c);
        if (--s_bp[i].left <= 0) s_bp[i].addr = 0;
        c->EFlags |= 0x10000;   // RF：从断点处继续执行
    }
    c->Dr6 = 0;
    ApplyDr(c);
    return EXCEPTION_CONTINUE_EXECUTION;
}

// 调试：统计游戏主线程每帧读了多少文件（找局内的资源加载，回滚不能跨过它们）
using ReadFileFn = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using WriteFileFn = BOOL(WINAPI*)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
static ReadFileFn s_readFile;
static WriteFileFn s_writeFile;
static int s_ioWrites, s_ioWriteBytes;
static CreateFileWFn s_createFileW;
static DWORD s_ioThread;
static int s_ioReads, s_ioBytes;
static wchar_t s_ioLastFile[128];

static BOOL WINAPI ReadFileHook(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov)
{
    if (GetCurrentThreadId() == s_ioThread) { ++s_ioReads; s_ioBytes += n; }
    return s_readFile(h, buf, n, got, ov);
}

static BOOL WINAPI WriteFileHook(HANDLE h, LPCVOID buf, DWORD n, LPDWORD put, LPOVERLAPPED ov)
{
    if (GetCurrentThreadId() == s_ioThread) { ++s_ioWrites; s_ioWriteBytes += n; }
    return s_writeFile(h, buf, n, put, ov);
}

static HANDLE WINAPI CreateFileWHook(LPCWSTR name, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t)
{
    if (GetCurrentThreadId() == s_ioThread && name) wcsncpy_s(s_ioLastFile, name, _TRUNCATE);
    return s_createFileW(name, a, s, sa, c, f, t);
}

// 主模块导入表里的函数指针换成 hook
static void* HookImport(const char* dll, const char* fn, void* hook)
{
    auto base = (uint8_t*)GetModuleHandleW(nullptr);
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp((char*)(base + d->Name), dll)) continue;
        auto names = (IMAGE_THUNK_DATA*)(base + d->OriginalFirstThunk);
        auto slots = (IMAGE_THUNK_DATA*)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            if (strcmp((char*)((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name, fn)) continue;
            void* orig = (void*)slots->u1.Function;
            DWORD old;
            VirtualProtect(&slots->u1.Function, 4, PAGE_READWRITE, &old);
            slots->u1.Function = (uintptr_t)hook;
            VirtualProtect(&slots->u1.Function, 4, old, &old);
            return orig;
        }
    }
    return nullptr;
}

// 每帧开始时调用：上一帧读过文件就记下来
void DebugFrameIo(int frame)
{
    if (!s_readFile) return;
    if ((s_ioReads || s_ioWrites) && CoopStg())
        Log("io: frame %d read %d times, %d KB, wrote %d times, %d KB (last opened %ls)", frame, s_ioReads, s_ioBytes / 1024, s_ioWrites,
            s_ioWriteBytes / 1024, s_ioLastFile);
    s_ioReads = s_ioBytes = s_ioWrites = s_ioWriteBytes = 0;
    s_ioLastFile[0] = 0;
}

// 调试：没人处理的异常（崩溃）记下位置和栈
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
{
    Log("CRASH: code %08X at %p (access %d, address %p)", ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress,
        ep->ExceptionRecord->NumberParameters > 0 ? (int)ep->ExceptionRecord->ExceptionInformation[0] : -1,
        ep->ExceptionRecord->NumberParameters > 1 ? (void*)ep->ExceptionRecord->ExceptionInformation[1] : nullptr);
    LogCtx("crash", ep->ContextRecord);
    uint32_t ebp = ep->ContextRecord->Ebp;
    char line[512];
    int len = snprintf(line, sizeof line, "   frames:");
    for (int i = 0; i < 16 && ebp && len < (int)sizeof line - 12; ++i) {
        uint32_t ret = 0, next = 0;
        if (!ReadU32(ebp + 4, &ret) || !ReadU32(ebp, &next)) break;
        len += snprintf(line + len, sizeof line - len, " %08X", ret);
        if (next <= ebp) break;
        ebp = next;
    }
    Log("%s", line);
    return EXCEPTION_CONTINUE_SEARCH;
}

// 调试：严重的异常第一时间记下（游戏 / DxLib 可能换掉了未处理异常的过滤器）。回滚写保护页的那种不算
static LONG NTAPI SevereVeh(EXCEPTION_POINTERS* ep)
{
    static volatile LONG s_count;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != STATUS_NO_MEMORY && code != EXCEPTION_STACK_OVERFLOW
        && code != STATUS_HEAP_CORRUPTION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO)
        return EXCEPTION_CONTINUE_SEARCH;
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters > 1 && ep->ExceptionRecord->ExceptionInformation[0] == 1) {
        uintptr_t a = ep->ExceptionRecord->ExceptionInformation[1];
        if (a >= Va(0x1089000) && a < Va(0x1CB0000)) return EXCEPTION_CONTINUE_SEARCH;
    }
    if (InterlockedIncrement(&s_count) > 8) return EXCEPTION_CONTINUE_SEARCH;
    Log("exception %08X at %p (thread %u, address %p)", code, ep->ExceptionRecord->ExceptionAddress, GetCurrentThreadId(),
        ep->ExceptionRecord->NumberParameters > 1 ? (void*)ep->ExceptionRecord->ExceptionInformation[1] : nullptr);
    LogCtx("exception", ep->ContextRecord);
    DebugMemory("at exception");
    return EXCEPTION_CONTINUE_SEARCH;
}

// 调试：进程内存（看一局局下来有没有漏）
void DebugMemory(const char* when)
{
    PROCESS_MEMORY_COUNTERS_EX pmc { sizeof pmc };
    char counts[256] = "";
    int len = 0;
    for (int type = 0; type < 32 && len < 200; ++type) {   // DxLib 各类句柄的个数（句柄管理 0xBF59F0 + 类型*0x2F0，+0x34 = 个数）
        uintptr_t m = Va(0xBF59F0) + type * 0x2F0;
        if (*(int*)m && *(int*)(m + 0x34)) len += snprintf(counts + len, sizeof counts - len, " t%d=%d", type, *(int*)(m + 0x34));
    }
    // 地址空间：最大的空闲块（32 位、2GB）
    size_t largest = 0, freeTotal = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t a = 0x10000; a < 0x7FFF0000 && VirtualQuery((void*)a, &mbi, sizeof mbi); a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize)
        if (mbi.State == MEM_FREE) { freeTotal += mbi.RegionSize; if (mbi.RegionSize > largest) largest = mbi.RegionSize; }
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof pmc))
        Log("memory %s: private %u MB, working set %u MB, free address space %u MB (largest %u MB), handles%s", when,
            (unsigned)(pmc.PrivateUsage >> 20), (unsigned)(pmc.WorkingSetSize >> 20), (unsigned)(freeTotal >> 20), (unsigned)(largest >> 20), counts);
}

void InstallDebugVeh()
{
    AddVectoredExceptionHandler(1, DebugVeh);
    AddVectoredExceptionHandler(0, SevereVeh);   // 排在最后：回滚的写保护先处理
    s_ioThread = GetCurrentThreadId();
    s_readFile = (ReadFileFn)HookImport("kernel32.dll", "ReadFile", (void*)ReadFileHook);
    s_createFileW = (CreateFileWFn)HookImport("kernel32.dll", "CreateFileW", (void*)CreateFileWHook);
    s_writeFile = (WriteFileFn)HookImport("kernel32.dll", "WriteFile", (void*)WriteFileHook);
    if (!s_writeFile) s_writeFile = WriteFile;
    if (!s_createFileW) s_createFileW = CreateFileW;
    Log("debug io hooks %s", s_readFile ? "ok" : "missing");
}

// 不能在被设置的线程里改它自己的调试寄存器：在临时线程里挂起游戏主线程再设
static DWORD WINAPI SetDrThread(LPVOID)
{
    DWORD tid = GetWindowThreadProcessId(g_hwnd, nullptr);
    HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid);
    if (!th) return 0;
    SuspendThread(th);
    CONTEXT c {};
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    GetThreadContext(th, &c);
    ApplyDr(&c);
    SetThreadContext(th, &c);
    ResumeThread(th);
    CloseHandle(th);
    Log("debug regs: dr0=%p dr1=%p dr2=%p dr3=%p dr7=%08X", (void*)s_watch, (void*)s_bp[0].addr, (void*)s_bp[1].addr, (void*)s_bp[2].addr, Dr7Value());
    return 0;
}

static void UpdateDr()
{
    if (HANDLE t = CreateThread(nullptr, 0, SetDrThread, nullptr, 0, nullptr)) {
        WaitForSingleObject(t, 1000);
        CloseHandle(t);
    }
}

// 地址：十六进制 VA；"r+RVA"；"STG"；"PM"/"ST"/"PB" 1P 的玩家管理器 / 状态 [PM] / P_Bullet [PM+8]；"P2"/"S2"、"P3"/"S3" 2P / 3P 的；可再加 "+偏移"
static uintptr_t ParseAddr(const char* s)
{
    uintptr_t base = 0;
    const char* rest = s;
    uintptr_t pm1 = CoopPm(0), pm2 = CoopPm(1);
    uint32_t v = 0;
    if (!strncmp(s, "r+", 2)) return g_base + strtoul(s + 2, nullptr, 16);
    if (!strncmp(s, "STG", 3)) { base = CoopStg(); rest = s + 3; }
    else if (!strncmp(s, "PM", 2)) { base = pm1; rest = s + 2; }
    else if (!strncmp(s, "ST", 2)) { if (pm1) ReadU32(pm1, &v); base = v; rest = s + 2; }
    else if (!strncmp(s, "PB", 2)) { if (pm1) ReadU32(pm1 + 8, &v); base = v; rest = s + 2; }
    else if (!strncmp(s, "P2", 2)) { base = pm2; rest = s + 2; }
    else if (!strncmp(s, "S2", 2)) { if (pm2) ReadU32(pm2, &v); base = v; rest = s + 2; }
    else if (!strncmp(s, "P3", 2)) { base = CoopPm(2); rest = s + 2; }
    else if (!strncmp(s, "S3", 2)) { if (CoopPm(2)) ReadU32(CoopPm(2), &v); base = v; rest = s + 2; }
    else return (uintptr_t)strtoul(s, nullptr, 16);
    if (*rest == '+') base += strtoul(rest + 1, nullptr, 16);
    return base;
}

// 无敌：按座位（受伤函数 0x6201A0 由 coop.cpp 挂钩）
void SetGod(bool on)
{
    for (int seat = 0; seat < MAX_SEATS; ++seat) CoopSetGod(seat, on);
}

// pages <帧数>：.data 段每 4KB 页算哈希，过 N 帧再算一次，列出变过的页（回滚要保存哪些静态数据）
static constexpr uint32_t DATA_BEGIN = 0x9FB000, DATA_END = 0x2FB9000;
static std::vector<uint64_t> s_pageHash;
static int s_pageScanAt = -1;
static std::vector<int> s_pageChanged;   // 各页在扫描期间变过几次

static uint64_t PageHash(uintptr_t a)
{
    const uint64_t* p = (const uint64_t*)a;
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 512; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static void PageScanTick()
{
    if (s_pageScanAt < 0) return;
    size_t n = (DATA_END - DATA_BEGIN) / 4096;
    for (size_t i = 0; i < n; ++i) {
        uint64_t h = PageHash(Va(DATA_BEGIN) + i * 4096);
        if (h != s_pageHash[i]) { s_pageHash[i] = h; ++s_pageChanged[i]; }
    }
    if (g_frame < s_pageScanAt) return;
    s_pageScanAt = -1;
    size_t total = 0;
    for (size_t i = 0; i < n;) {
        if (!s_pageChanged[i]) { ++i; continue; }
        size_t j = i;
        int maxc = 0;
        while (j < n && s_pageChanged[j]) { maxc = s_pageChanged[j] > maxc ? s_pageChanged[j] : maxc; ++j; }
        Log("pages %08X-%08X (%u pages, changed up to %d times)", (unsigned)(DATA_BEGIN + i * 4096), (unsigned)(DATA_BEGIN + j * 4096),
            (unsigned)(j - i), maxc);
        total += j - i;
        i = j;
    }
    Log("pages: %u changed pages (%u KB)", (unsigned)total, (unsigned)(total * 4));
}

static void DebugCommands()
{
    std::wstring path = DebugFile(L"_cmd.txt");
    FILE* f = _wfopen(path.c_str(), L"r");
    if (!f) return;
    char line[256];
    bool drDirty = false;
    while (fgets(line, sizeof line, f)) {
        char cmd[32] = {}, a1[32] = {}, a2[32] = {};
        int n = sscanf_s(line, "%31s %31s %31s", cmd, (unsigned)sizeof cmd, a1, (unsigned)sizeof a1, a2, (unsigned)sizeof a2);
        if (n < 1) continue;
        if (!strcmp(cmd, "dump") && n >= 2) {
            uintptr_t p = ParseAddr(a1);
            unsigned len = n >= 3 ? strtoul(a2, nullptr, 16) : 0x40;
            Log("dump %p len %X", (void*)p, len);
            for (unsigned o = 0; p && o < len; o += 16) {
                uint32_t v[4] = {};
                for (int i = 0; i < 4; ++i) ReadU32(p + o + i * 4, &v[i]);
                float fl[4];
                memcpy(fl, v, sizeof fl);
                Log("  +%03X: %08X %08X %08X %08X | %d %d %d %d | %.3g %.3g %.3g %.3g", o, v[0], v[1], v[2], v[3],
                    (int)v[0], (int)v[1], (int)v[2], (int)v[3], fl[0], fl[1], fl[2], fl[3]);
            }
        } else if (!strcmp(cmd, "write") && n >= 3) {
            uintptr_t p = ParseAddr(a1);
            WriteU32(p, strtoul(a2, nullptr, 16));
            Log("write %p = %s", (void*)p, a2);
        } else if (!strcmp(cmd, "writef") && n >= 3) {
            uintptr_t p = ParseAddr(a1);
            float v = (float)atof(a2);
            uint32_t u;
            memcpy(&u, &v, 4);
            WriteU32(p, u);
            Log("writef %p = %g", (void*)p, v);
        } else if ((!strcmp(cmd, "watch") || !strcmp(cmd, "watchrw")) && n >= 2) {
            s_watch = ParseAddr(a1);
            s_watchRw = !strcmp(cmd, "watchrw");
            s_watchHits.clear();
            drDirty = true;
        } else if (!strcmp(cmd, "bp") && n >= 2) {
            int slot = -1;
            for (int i = 0; i < 3 && slot < 0; ++i)
                if (!s_bp[i].addr || s_bp[i].left <= 0) slot = i;
            if (slot < 0) slot = 2;
            s_bp[slot].addr = ParseAddr(a1);
            s_bp[slot].left = n >= 3 ? (int)strtoul(a2, nullptr, 16) : 1;
            drDirty = true;
        } else if (!strcmp(cmd, "bpclear")) {
            for (auto& b : s_bp) b = Bp {};
            s_watch = 0;
            drDirty = true;
        } else if (!strcmp(cmd, "frame")) {
            Log("frame %d stg %p pm %p pm2 %p", g_frame, (void*)CoopStg(), (void*)CoopPm(0), (void*)CoopPm(1));
        } else if (!strcmp(cmd, "god") && n >= 3) {     // god <座位 1|2> <0|1>
            CoopSetGod(atoi(a1) - 1, atoi(a2) != 0);
        } else if (!strcmp(cmd, "god") && n >= 2) {
            SetGod(atoi(a1) != 0);
        } else if (!strcmp(cmd, "rbtest") && n >= 2) {
            RollbackTest(atoi(a1));
        } else if (!strcmp(cmd, "synctest") && n >= 2) {
            RollbackSyncTest(atoi(a1));
        } else if (!strcmp(cmd, "pages") && n >= 2) {
            size_t pages = (DATA_END - DATA_BEGIN) / 4096;
            s_pageHash.assign(pages, 0);
            s_pageChanged.assign(pages, 0);
            for (size_t i = 0; i < pages; ++i) s_pageHash[i] = PageHash(Va(DATA_BEGIN) + i * 4096);
            s_pageScanAt = g_frame + atoi(a1);
            Log("pages: scanning %u pages for %d frames", (unsigned)pages, atoi(a1));
        } else if (!strcmp(cmd, "fps") && n >= 2) {
            g_fpsCap = atoi(a1);
            Log("fps cap %d", g_fpsCap);
        } else if (!strcmp(cmd, "handles")) {
            RbDebugHandles();
        } else if (!strcmp(cmd, "hudbomb")) {
            CoopDebugHudBombs();
        } else if (!strcmp(cmd, "dark")) {
            CoopDebugDarkness();
        } else if (!strcmp(cmd, "bgm")) {   // 各首 BGM：SE 管理器里的标记、句柄、DxLib 的真实状态
            using QueryFn = int(__cdecl*)(int);
            for (int i = 0; i < 0x36; ++i) {
                uintptr_t e = Va(0x10896B8 + 0x584) + i * 0x11C;
                int h = *(int*)(e + 4);
                if (h <= 0 && !*(uint8_t*)e) continue;
                Log("bgm %d: flag %d handle %X check %d query %d", i, *(uint8_t*)e, h, h > 0 ? ((QueryFn)Va(0x6A3A70))(h) : -9,
                    h > 0 ? ((QueryFn)Va(0x6A39E0))(h) : -9);
            }
        }
    }
    fclose(f);
    DeleteFileW(path.c_str());
    if (drDirty) UpdateDr();
}

void DebugTick()
{
    PageScanTick();
    DebugInputTick();
    DebugScreenshot();
    DebugCommands();
}
