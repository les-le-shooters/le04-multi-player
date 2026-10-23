// 调试模式的模拟按键（取自 lenprac）：挂在 DxLib 的键盘状态刷新函数后面，把模拟的键写进它的 DIK 缓冲。
// multi_input.txt："z*6 w10 down x shift+x*20"：默认按 3 帧再松 3 帧；键名*N = 按 N 帧；wN = 等 N 帧；
// 键名后加 ! 表示一直按住到脚本结束（如 "z! w300"）。
#include "mod.h"
#include <cstdio>
#include <cstring>

static const uint32_t KEY_UPDATE_RVA = 0x2FA650;   // DxLib 键盘状态刷新（cdecl，入口 push ebp/mov ebp,esp/sub esp,104h）
static const uint32_t KEY_BUF_RVA = 0x7B6D2C;      // 256 字节 DIK 缓冲

using KeyUpdateFn = int(__cdecl*)(int);
static KeyUpdateFn g_origKeyUpdate;
static uint8_t* g_keyBuf;
static uint8_t g_kbFake[256];
static uint8_t g_kbHeld[256];       // 带 ! 的常按键
static uint8_t g_kbPrevFake[256];

static int __cdecl HookKeyUpdate(int arg)
{
    int r = g_origKeyUpdate(arg);
    for (int k = 0; k < 256; ++k) g_keyBuf[k] |= g_kbFake[k] | g_kbHeld[k];
    return r;
}

const uint8_t* DikBuffer() { return (const uint8_t*)Rva(KEY_BUF_RVA); }

bool InstallKeyboardHook()
{
    uintptr_t fn = Rva(KEY_UPDATE_RVA);
    if (!BytesAt(fn, Hex("558BEC81EC04010000"))) {
        Log("key update hook: prologue mismatch");
        return false;
    }
    g_keyBuf = (uint8_t*)Rva(KEY_BUF_RVA);
    return InlineHookN((void*)fn, HookKeyUpdate, (void**)&g_origKeyUpdate, 9);
}

struct Step { std::vector<uint8_t> keys, hold; int frames; int gap; };
static std::vector<Step> g_steps;
static size_t g_pos = 0;
static int g_stepFrame = 0;

static uint8_t DikByName(const std::string& n)
{
    static const std::pair<const char*, uint8_t> tbl[] = {
        { "z", 0x2C }, { "x", 0x2D }, { "c", 0x2E }, { "shift", 0x2A }, { "esc", 0x01 }, { "enter", 0x1C },
        { "up", 0xC8 }, { "down", 0xD0 }, { "left", 0xCB }, { "right", 0xCD }, { "space", 0x39 }, { "ctrl", 0x1D },
        { "a", 0x1E }, { "s", 0x1F }, { "d", 0x20 }, { "q", 0x10 }, { "w", 0x11 }, { "e", 0x12 }, { "v", 0x2F },
        { "i", 0x17 }, { "j", 0x24 }, { "k", 0x25 }, { "l", 0x26 }, { "f", 0x21 }, { "g", 0x22 }, { "f5", 0x3F }, { "f6", 0x40 },
    };
    for (auto& [k, v] : tbl)
        if (n == k) return v;
    return 0;
}

static FILETIME g_scriptTime;   // 正在跑的脚本文件的修改时间：文件被改写就中止它、读新的

void DebugInputTick()
{
    if (!g_keyBuf) return;
    memset(g_kbFake, 0, sizeof g_kbFake);
    std::wstring path = DebugFile(L"_input.txt");
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (g_pos < g_steps.size() && GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)
        && CompareFileTime(&fa.ftLastWriteTime, &g_scriptTime) != 0) {
        Log("input script replaced (frame %d)", g_frame);
        g_steps.clear();
        g_pos = 0;
        memset(g_kbHeld, 0, sizeof g_kbHeld);
    }
    if (g_pos >= g_steps.size()) {
        if (!g_steps.empty()) {
            g_steps.clear();
            memset(g_kbHeld, 0, sizeof g_kbHeld);
            DeleteFileW(path.c_str());
            Log("input script done (frame %d)", g_frame);
        }
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) g_scriptTime = fa.ftLastWriteTime;
        FILE* f = _wfopen(path.c_str(), L"r");
        if (!f) goto sync;
        {
            static char buf[1 << 20];
            memset(buf, 0, sizeof buf);
            fread(buf, 1, sizeof buf - 1, f);
            fclose(f);
            char* ctx = nullptr;
            for (char* tok = strtok_s(buf, " \r\n\t", &ctx); tok; tok = strtok_s(nullptr, " \r\n\t", &ctx)) {
                Step st { {}, {}, 3, 3 };
                std::string t = tok;
                if (t[0] == 'w' && t.size() > 1 && isdigit((unsigned char)t[1])) { st.frames = 0; st.gap = atoi(t.c_str() + 1); g_steps.push_back(st); continue; }
                size_t star = t.find('*');
                if (star != std::string::npos) { st.frames = atoi(t.c_str() + star + 1); t.resize(star); }
                size_t p = 0;
                while (p <= t.size()) {
                    size_t plus = t.find('+', p);
                    std::string k = t.substr(p, plus == std::string::npos ? std::string::npos : plus - p);
                    bool hold = !k.empty() && k.back() == '!';
                    if (hold) k.pop_back();
                    if (uint8_t d = DikByName(k)) (hold ? st.hold : st.keys).push_back(d);
                    if (plus == std::string::npos) break;
                    p = plus + 1;
                }
                if (st.keys.empty() && !st.hold.empty()) { st.frames = 0; st.gap = 0; }
                g_steps.push_back(st);
            }
            g_pos = 0;
            g_stepFrame = 0;
            Log("input script: %zu steps (frame %d)", g_steps.size(), g_frame);
            if (g_steps.empty()) DeleteFileW(path.c_str());
        }
        goto sync;
    }
    {
        auto& st = g_steps[g_pos];
        if (g_stepFrame == 0)
            for (uint8_t d : st.hold) g_kbHeld[d] = 0x80;
        if (g_stepFrame < st.frames)
            for (uint8_t d : st.keys) g_kbFake[d] = 0x80;
        if (++g_stepFrame >= st.frames + st.gap) { ++g_pos; g_stepFrame = 0; }
    }
sync:
    // 后台时 DxLib 很少刷新缓冲：每帧直接同步（按下置位、松开清位），否则短按会丢
    for (int k = 0; k < 256; ++k) {
        uint8_t on = g_kbFake[k] | g_kbHeld[k];
        if (on) g_keyBuf[k] |= 0x80;
        else if (g_kbPrevFake[k]) g_keyBuf[k] = 0;
        g_kbPrevFake[k] = on;
    }
}
