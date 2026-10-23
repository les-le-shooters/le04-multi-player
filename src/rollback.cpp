// rollback.cpp —— 回滚（联机回滚模式用；做法见 README 的「回滚」一节）
// 1. 确定性内存池：游戏自己的代码（0x401000..0x666000）在主线程里 operator new 的内存都从一块自己的内存里分，
//    分配器的状态（空闲链表等）也放在这块内存里，所以"还原这块内存"就连分配器一起回到当时，之后再分配到的地址也一样。
//    DxLib 和它带的库（Ogg 解码等）的分配照旧走 CRT 的堆：它们的内存可能在别的线程里用（声音线程边解码边写），
//    不能被回滚。free / realloc / _msize 按地址分辨是不是池里的。
// 2. 逐帧快照（RbSave / RbRestore）：内存池按写过的页记撤销日志；游戏的静态数据按页（写保护找出会变的页）；
//    随机数状态和 coop.cpp 每局的状态整个存。模拟里建 / 删的 DxLib 句柄跟着回滚。
// 3. 重跑一帧（RbSimulate）：Game::Run 和主循环里其余改模拟状态的部分，不画、不放音。
// 4. 绘制前后存 / 还原随机数状态（原版绘制特效时会调用 GetRand），这样不画也能得到同样的模拟结果。
// 5. 自检（调试命令）：rbtest N 用完整快照逐字节比较；synctest K 用逐帧快照每 30 帧回滚 K 帧再比较。
#include "mod.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <cstring>
#include <intrin.h>
#include <xmmintrin.h>
#include <vector>

// 随机数状态（DxLib GetRand 的 0x107E4D0..0x107F860）整个存 / 放回，按用途分格子。
// 不内联、缓冲区对外可见：MSVC 曾把 ResimCutin 里"存进函数内的静态数组 → 调用游戏 → 放回"优化错了
// （删掉了存，放回变成把随机数复制给它自己），这样写它就不能那样做
uint8_t g_rngKeep[RNG_KEEP_SLOTS][0x107F860 - 0x107E4D0];
__declspec(noinline) void RngKeep(int slot) { memcpy(g_rngKeep[slot], (void*)Va(0x107E4D0), sizeof g_rngKeep[slot]); }
__declspec(noinline) void RngPut(int slot) { memcpy((void*)Va(0x107E4D0), g_rngKeep[slot], sizeof g_rngKeep[slot]); }

namespace {

template <class T> T& At(uintptr_t a) { return *(T*)a; }

// ---------------------------------------------------------------- 内存池
constexpr uint32_t VA_MALLOC = 0x86784D, VA_FREE = 0x867815, VA_REALLOC = 0x8678DF, VA_MSIZE = 0x86BA42, VA_CALLOC = 0x87279B;
constexpr size_t COMMIT_STEP = 1u << 20;
constexpr uint32_t SMALL_MAX = 2048;
constexpr int NCLASS = 24;
const uint32_t CLASS_SIZE[NCLASS] = { 16, 32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 640, 768, 896, 1024,
                                      1280, 1536, 1792, 2048 };
enum : uint32_t { F_USED = 1, F_SMALL = 2 };

struct Hdr {            // 每个块前 16 字节；块的总长 = 16 + cap
    uint32_t cap;       // 可用字节数（16 的倍数）
    uint32_t req;       // 请求的字节数（_msize 返回它）
    uint32_t prev;      // 前一个块的总长（第一个块为 0）
    uint32_t flags;     // F_USED / F_SMALL，位 8.. 是小块的大小档
};
struct FreeLink { Hdr* next; Hdr* prev; };   // 空闲大块的载荷里放链表指针

struct Meta {           // 放在内存池开头（也会被快照 / 还原）
    uint8_t* top;       // 最后一个块之后
    Hdr* last;          // 最后一个块
    Hdr* small[NCLASS]; // 各档小块的空闲链表（载荷第一个字放 next）
    Hdr* large;         // 空闲大块链表
    uint32_t used, peak, allocs, frees;
};

uint8_t* g_pool = nullptr;
size_t g_reserve = 0;
uint8_t* g_committed = nullptr;   // 已提交的末尾（只增不减，不进快照）
Meta* g_meta = nullptr;
DWORD g_mainThread = 0;
CRITICAL_SECTION g_lock;
bool g_poolFull = false;

using MallocFn = void*(__cdecl*)(size_t);
using FreeFn = void(__cdecl*)(void*);
using ReallocFn = void*(__cdecl*)(void*, size_t);
using MsizeFn = size_t(__cdecl*)(void*);
using CallocFn = void*(__cdecl*)(size_t, size_t);
MallocFn g_origMalloc;
FreeFn g_origFree;
ReallocFn g_origRealloc;
MsizeFn g_origMsize;
CallocFn g_origCalloc;

bool InPool(const void* p) { return g_pool && (const uint8_t*)p >= g_pool && (const uint8_t*)p < g_pool + g_reserve; }
Hdr* HdrOf(void* p) { return (Hdr*)((uint8_t*)p - 16); }
uint8_t* Payload(Hdr* h) { return (uint8_t*)h + 16; }
Hdr* NextOf(Hdr* h) { return (Hdr*)((uint8_t*)h + 16 + h->cap); }
int ClassOf(size_t n)
{
    for (int c = 0; c < NCLASS; ++c)
        if (n <= CLASS_SIZE[c]) return c;
    return -1;
}

bool EnsureCommitted(uint8_t* end)
{
    while (g_committed < end) {
        if (g_committed + COMMIT_STEP > g_pool + g_reserve) return false;
        if (!VirtualAlloc(g_committed, COMMIT_STEP, MEM_COMMIT, PAGE_READWRITE)) return false;
        g_committed += COMMIT_STEP;
    }
    return true;
}

Hdr* Carve(uint32_t cap, uint32_t flags)
{
    uint8_t* h = g_meta->top;
    if (!EnsureCommitted(h + 16 + cap)) return nullptr;
    Hdr* b = (Hdr*)h;
    b->cap = cap;
    b->req = 0;
    b->prev = g_meta->last ? 16 + g_meta->last->cap : 0;
    b->flags = flags;
    g_meta->last = b;
    g_meta->top = h + 16 + cap;
    return b;
}

void LargeUnlink(Hdr* b)
{
    FreeLink* l = (FreeLink*)Payload(b);
    if (l->prev) ((FreeLink*)Payload(l->prev))->next = l->next;
    else g_meta->large = l->next;
    if (l->next) ((FreeLink*)Payload(l->next))->prev = l->prev;
}

void LargeLink(Hdr* b)
{
    FreeLink* l = (FreeLink*)Payload(b);
    l->prev = nullptr;
    l->next = g_meta->large;
    if (l->next) ((FreeLink*)Payload(l->next))->prev = b;
    g_meta->large = b;
}

void* PoolAlloc(size_t n)
{
    if (!n) n = 1;
    if (n > 0x7FF00000) return nullptr;
    Hdr* b = nullptr;
    int c = n <= SMALL_MAX ? ClassOf(n) : -1;
    if (c >= 0) {
        if ((b = g_meta->small[c])) {
            g_meta->small[c] = *(Hdr**)Payload(b);
            b->flags |= F_USED;
        } else {
            b = Carve(CLASS_SIZE[c], F_USED | F_SMALL | (uint32_t)c << 8);
        }
    } else {
        uint32_t cap = (uint32_t)((n + 15) & ~15u);
        for (Hdr* f = g_meta->large; f; f = ((FreeLink*)Payload(f))->next)
            if (f->cap >= cap) { b = f; break; }
        if (b) {
            LargeUnlink(b);
            if (b->cap - cap >= 16 + SMALL_MAX + 16) {   // 拆成两块，剩下的放回空闲链表
                Hdr* r = (Hdr*)(Payload(b) + cap);
                r->cap = b->cap - cap - 16;
                r->req = 0;
                r->prev = 16 + cap;
                r->flags = 0;
                if ((uint8_t*)NextOf(r) < g_meta->top) NextOf(r)->prev = 16 + r->cap;
                else g_meta->last = r;
                b->cap = cap;
                LargeLink(r);
            }
            b->flags = F_USED;
        } else {
            b = Carve(cap, F_USED);
        }
    }
    if (!b) return nullptr;
    b->req = (uint32_t)n;
    g_meta->used += b->cap;
    if (g_meta->used > g_meta->peak) g_meta->peak = g_meta->used;
    ++g_meta->allocs;
    return Payload(b);
}

void PoolFree(void* p)
{
    Hdr* b = HdrOf(p);
    if (!(b->flags & F_USED)) return;   // 重复释放：忽略
    g_meta->used -= b->cap;
    ++g_meta->frees;
    if (b->flags & F_SMALL) {
        b->flags &= ~F_USED;
        *(Hdr**)Payload(b) = g_meta->small[b->flags >> 8 & 0xFF];
        g_meta->small[b->flags >> 8 & 0xFF] = b;
        return;
    }
    b->flags = 0;
    // 和后一个空闲大块合并
    Hdr* n = NextOf(b);
    if ((uint8_t*)n < g_meta->top && !(n->flags & (F_USED | F_SMALL))) {
        LargeUnlink(n);
        if (g_meta->last == n) g_meta->last = b;
        b->cap += 16 + n->cap;
        if ((uint8_t*)NextOf(b) < g_meta->top) NextOf(b)->prev = 16 + b->cap;
    }
    // 和前一个空闲大块合并
    if (b->prev) {
        Hdr* pv = (Hdr*)((uint8_t*)b - b->prev);
        if (!(pv->flags & (F_USED | F_SMALL))) {
            LargeUnlink(pv);
            if (g_meta->last == b) g_meta->last = pv;
            pv->cap += 16 + b->cap;
            b = pv;
            if ((uint8_t*)NextOf(b) < g_meta->top) NextOf(b)->prev = 16 + b->cap;
        }
    }
    // 最后一块：还给顶端
    if (g_meta->last == b) {
        g_meta->top = (uint8_t*)b;
        g_meta->last = b->prev ? (Hdr*)((uint8_t*)b - b->prev) : nullptr;
        return;
    }
    LargeLink(b);
}

bool g_poolOff = false;   // 单人 / 本地多人：新的分配照旧走 CRT（和原版一样）
bool UsePool() { return g_meta && !g_poolFull && !g_poolOff && GetCurrentThreadId() == g_mainThread; }

// 游戏代码的范围：operator new 的调用方在这里面的才从池里分。DxLib 从 0x66B000 左右开始；
// 游戏代码里最后一个 operator new 调用在 0x6637A0，库里的从 0x700607 起
constexpr uint32_t GAME_CODE_BEGIN = 0x401000, GAME_CODE_END = 0x666000, VA_NEW = 0x864674;
using NewFn = void*(__cdecl*)(size_t);
NewFn g_origNew;

void* PoolAllocLocked(size_t n)
{
    EnterCriticalSection(&g_lock);
    void* p = PoolAlloc(n);
    LeaveCriticalSection(&g_lock);
    if (!p && !g_poolFull) {
        g_poolFull = true;
        Log("pool: full, falling back to the CRT heap");
    }
    return p;
}

// 本 DLL 也用游戏的 operator new 建游戏对象（2P / 3P 的 PM），也算游戏的
uintptr_t g_selfBegin = 0, g_selfEnd = 0;

void* __cdecl NewHook(size_t n)
{
    uintptr_t ra = (uintptr_t)_ReturnAddress();
    if (UsePool() && ((ra >= Va(GAME_CODE_BEGIN) && ra < Va(GAME_CODE_END)) || (ra >= g_selfBegin && ra < g_selfEnd)))
        if (void* p = PoolAllocLocked(n)) return p;
    return g_origNew(n);
}

void __cdecl FreeHook(void* p)
{
    if (!p) return;
    if (InPool(p)) {
        EnterCriticalSection(&g_lock);
        PoolFree(p);
        LeaveCriticalSection(&g_lock);
        return;
    }
    g_origFree(p);
}

size_t __cdecl MsizeHook(void* p)
{
    if (p && InPool(p)) return HdrOf(p)->req;
    return g_origMsize(p);
}

void* __cdecl ReallocHook(void* p, size_t n)
{
    if (!p) return g_origMalloc(n);
    if (!n) { FreeHook(p); return nullptr; }
    if (!InPool(p)) return g_origRealloc(p, n);
    Hdr* b = HdrOf(p);
    if (n <= b->cap && (n > SMALL_MAX || !(b->flags & F_SMALL) || ClassOf(n) == (int)(b->flags >> 8 & 0xFF))) {
        b->req = (uint32_t)n;
        return p;
    }
    void* q = g_poolFull ? nullptr : PoolAllocLocked(n);   // 池里的块变大：还在池里分
    if (!q) q = g_origMalloc(n);
    if (!q) return nullptr;
    memcpy(q, p, b->req < n ? b->req : n);
    FreeHook(p);
    return q;
}

// ---------------------------------------------------------------- 快照
// .data 里游戏用的静态数据（存档 / 各种单例 / 函数内静态对象）和 DxLib 随机数的状态；DxLib 自己的其它全局（输入、绘制状态等）不回滚
struct Range { uint32_t va, end; };
const Range STATICS[] = {
    { 0x1089000, 0x1CB0000 },   // 游戏的静态对象、存档（SAVE_dat）
    // 游戏的各个单例（STG_set、Player_Upgrade、Effect_main、Item_main、Controller……），跳过两个不属于模拟的：
    { 0x2FAF000, 0x2FB3010 },
    { 0x2FB3034, 0x2FB4E80 },   // 0x2FB3010..0x2FB3034：帧率计数（主循环按真实时间算）
    { 0x2FB5E88, 0x2FB8000 },   // 0x2FB4E80..0x2FB5E88：键盘 / 手柄的设备状态（只有 Controller::Update 读，重跑的帧直接给 Controller 的结果）
    { 0x107E4D0, 0x107F860 },   // DxLib GetRand 的状态
};
constexpr uint32_t DATA_BEGIN = 0x9FB000, DATA_END = 0x2FB9000;
constexpr uint32_t VA_RAND_BEGIN = 0x107E4D0, VA_RAND_END = 0x107F860;

struct Snapshot {
    std::vector<uint8_t> pool;                  // [g_pool, top)
    std::vector<std::vector<uint8_t>> statics;
    std::vector<std::vector<uint8_t>> coop;
    std::vector<uint8_t> data;                  // 整个 .data（只在自检时用来找漏掉的状态）
};

void Capture(Snapshot& s, bool withData)
{
    size_t len = g_meta->top - g_pool;
    s.pool.assign(g_pool, g_pool + len);
    s.statics.clear();
    for (auto& r : STATICS) s.statics.emplace_back((uint8_t*)Va(r.va), (uint8_t*)Va(r.end));
    std::vector<std::pair<void*, size_t>> parts;
    CoopSimState(parts);
    s.coop.clear();
    for (auto& p : parts) s.coop.emplace_back((uint8_t*)p.first, (uint8_t*)p.first + p.second);
    if (withData) s.data.assign((uint8_t*)Va(DATA_BEGIN), (uint8_t*)Va(DATA_END));
}

void Restore(const Snapshot& s)
{
    memcpy(g_pool, s.pool.data(), s.pool.size());   // g_meta->top 也一起回到当时
    for (size_t i = 0; i < s.statics.size(); ++i) memcpy((void*)Va(STATICS[i].va), s.statics[i].data(), s.statics[i].size());
    std::vector<std::pair<void*, size_t>> parts;
    CoopSimState(parts);
    for (size_t i = 0; i < parts.size() && i < s.coop.size(); ++i) memcpy(parts[i].first, s.coop[i].data(), s.coop[i].size());
}

// 内存池里地址 a 落在哪个块（自检报告用）
void DescribePool(uintptr_t a, char* out, size_t n)
{
    for (Hdr* h = (Hdr*)(g_pool + 4096 - 16); (uint8_t*)h < g_meta->top; h = NextOf(h)) {   // 开头 4KB 是 Meta
        uintptr_t p = (uintptr_t)Payload(h);
        if (a >= (uintptr_t)h && a < p + h->cap) {
            snprintf(out, n, "block %p cap %u req %u %s +0x%X", (void*)p, h->cap, h->req, h->flags & F_USED ? "used" : "free",
                (unsigned)(a - p));
            return;
        }
    }
    snprintf(out, n, "?");
}

// 不一样的字节按段报告（间隔 16 字节以内的算一段）
int DiffBytes(const uint8_t* a, const uint8_t* b, size_t n, uintptr_t base, const char* what, int show, bool pool)
{
    int count = 0, shown = 0;
    for (size_t i = 0; i < n; ++i) {
        if (a[i] == b[i]) continue;
        size_t end = i, gap = 0;
        int d = 0;
        for (size_t j = i; j < n && gap < 16; ++j) {
            if (a[j] != b[j]) { ++d; end = j; gap = 0; } else ++gap;
        }
        count += d;
        if (shown < show) {
            char desc[128] = "", hexa[40] = "", hexb[40] = "";
            if (pool) DescribePool(base + i, desc, sizeof desc);
            for (size_t k = 0; k < 8 && i + k < n; ++k) {
                snprintf(hexa + k * 3, 4, "%02X ", a[i + k]);
                snprintf(hexb + k * 3, 4, "%02X ", b[i + k]);
            }
            Log("  diff %s %08X..%08X (%d bytes): %s-> %s %s", what, (unsigned)(base + i), (unsigned)(base + end), d, hexa, hexb, desc);
            ++shown;
        }
        i = end;
    }
    return count;
}

int g_cmpShow = 4;   // 报告多少段（4 = 全部；同步测试时 1，少报）

void Compare(const Snapshot& a, const Snapshot& b)
{
    if (a.pool.size() != b.pool.size()) Log("rbtest: pool size %u vs %u", (unsigned)a.pool.size(), (unsigned)b.pool.size());
    size_t n = a.pool.size() < b.pool.size() ? a.pool.size() : b.pool.size();
    int dp = DiffBytes(a.pool.data(), b.pool.data(), n, (uintptr_t)g_pool, "pool", 12 * g_cmpShow / 4, true);
    int ds = 0;
    for (size_t i = 0; i < a.statics.size(); ++i)
        ds += DiffBytes(a.statics[i].data(), b.statics[i].data(), a.statics[i].size(), Va(STATICS[i].va), "static", 24 * g_cmpShow / 4, false);
    int dc = 0;
    for (size_t i = 0; i < a.coop.size() && i < b.coop.size(); ++i) {
        char what[16];
        snprintf(what, sizeof what, "coop%u", (unsigned)i);
        dc += DiffBytes(a.coop[i].data(), b.coop[i].data(), a.coop[i].size(), 0, what, 4 * g_cmpShow / 4, false);
    }
    // .data 里没被快照覆盖、却不一样的页：可能是漏掉的状态（也可能是 DxLib 自己的计时 / 输入，属正常）
    int pages = 0;
    for (uint32_t va = DATA_BEGIN; va < DATA_END && !a.data.empty() && !b.data.empty(); va += 4096) {
        bool covered = false;
        for (auto& r : STATICS) covered |= va >= (r.va & ~0xFFFu) && va < r.end;
        if (covered) continue;
        size_t off = va - DATA_BEGIN;
        if (memcmp(&a.data[off], &b.data[off], 4096)) {
            int d = 0;
            for (int i = 0; i < 4096; ++i) d += a.data[off + i] != b.data[off + i];
            Log("  .data page %08X differs (%d bytes, not in snapshot)", va, d);
            ++pages;
        }
    }
    Log("rbtest: pool %d bytes differ, statics %d, coop %d, uncovered .data pages %d (pool %u KB, peak %u KB, %u allocs)", dp, ds, dc,
        pages, (unsigned)(a.pool.size() / 1024), g_meta->peak / 1024, g_meta->allocs);
}


// ---------------------------------------------------------------- 逐帧快照（回滚用）
// 每个帧边界存一份，最多存 RB_RING 帧：
//   内存池：GetWriteWatch 列出这一帧写过的页。影子副本 g_shadow 是上一个帧边界时的内容：写过的页把影子里的旧内容
//     记进上一帧的撤销日志，再更新影子。回到第 g 帧 = 从现在往回逐帧套用撤销日志。每帧只写几十页，代价很小。
//   静态数据：常写的页（热页）每帧整页存一份；其余页写保护，第一次被写时（异常处理）存下原内容、放开保护、加入热页。
//   小块：DxLib 随机数的状态、coop.cpp 每局的状态，每帧整个存。
constexpr int RB_RING = 64;
constexpr uint32_t ST_BEGIN = 0x1089000, ST_END = 0x1CB0000;   // 写保护跟踪的范围（游戏的静态对象、存档）
const Range HOT_INIT[] = {
    { 0x1089000, 0x108E000 },   // SE 管理器（音效请求、切曲）：每帧都写
    { 0x1254000, 0x1256000 },   // 成就（每帧计数）
    { 0x2FAF000, 0x2FB8000 },   // 各单例（STG_set、Player_Upgrade、Controller、Item_main……）
};
// 热页里不回滚的部分：画面截图特效的图像句柄（绘制里重建）、帧率计数、输入设备
const Range PRESERVE[] = { { 0x2FB0230, 0x2FB0234 }, { 0x2FB3010, 0x2FB3034 }, { 0x2FB4E80, 0x2FB5E88 } };
constexpr size_t MAX_LEARNED = 1024;   // 运行中变成热页的最多页数（存原内容的空间预先分配好，异常处理里不分配内存）

struct RbSlot {
    int frame = -1;
    std::vector<uint32_t> undoPages;   // 这一帧里写过的内存池页（页号）
    std::vector<uint8_t> undoData;     // 它们在这一帧开始时的内容
    size_t hotCount = 0;
    std::vector<uint8_t> hot;          // 热页在这一帧开始时的内容（g_hot 的前 hotCount 页）
    std::vector<uint8_t> small;        // 随机数状态 + coop 的状态
    std::vector<int> created;          // 这一帧（模拟里）新建的 DxLib 句柄
    std::vector<int> deleted;          // 这一帧（模拟里）要删的 DxLib 句柄：推迟到这一帧不会再被回滚时才真删
};
RbSlot g_slots[RB_RING];
bool g_rbOn = false;
int g_rbCur = -1;                      // 正在模拟的帧（最近一次 RbSave 的）

// DxLib 的句柄（图像、声音、字体……）：模拟里（不是绘制里）新建 / 删除的要跟着回滚。
//   新建：记在当时那一帧；回滚丢掉那一帧时删掉（还原后的状态不再引用它）
//   删除：推迟。那一帧被回滚丢掉时取消（还原后的状态还在用它）；那一帧移出回滚范围时才真删
constexpr uint32_t VA_ADD_HANDLE = 0x6C9B00, VA_SUB_HANDLE = 0x6CA530;
using AddHandleFn = int(__cdecl*)(int, int, int);
using SubHandleFn = int(__cdecl*)(int);
AddHandleFn g_origAddHandle;
SubHandleFn g_origSubHandle;
int g_simDepth = 0;                    // > 0：在模拟里（Game::Run、特效、切曲、成就）
extern bool g_resim;

// 浮点控制状态：模拟里统一成 CRT 的默认值（x87 控制字 0x027F、MXCSR 0x1F80），出了模拟再换回去。
// 游戏用的 sin / cos（0x8680F0、0x8682B0 等）在状态不是默认值时会改走 x87 的实现，结果的最后一位不一样；
// MXCSR 的 FTZ / DAZ 也会直接改变 SSE 运算的结果。
// 别的软件（游戏叠加层、录屏等，在 Present 里跑）改了这些状态又没改回来时，那台电脑就会和别人不同步
constexpr uint16_t FP_CW = 0x027F;
constexpr uint32_t FP_CSR = 0x1F80;
int g_fpLogged = 0;

uint16_t GetCw()
{
    uint16_t cw;
    __asm { fnstcw cw }
    return cw;
}

void SetCw(uint16_t cw)
{
    __asm { fldcw cw }
}

struct SimScope {
    uint16_t cw = 0;
    uint32_t csr = 0;
    bool fixed = false;
    SimScope()
    {
        static bool s_off = g_debug && getenv("LE04MULTI_NOFPUFIX");   // 调试：对比用，不纠正
        if (g_simDepth++ || GetCurrentThreadId() != g_mainThread || s_off) return;
        cw = GetCw();
        csr = _mm_getcsr();
        static bool s_first;
        if (!s_first) {
            s_first = true;
            Log("fpu: first simulation step with x87 %04X, mxcsr %08X", cw, csr);
        }
        if ((cw & 0x1F3F) == (FP_CW & 0x1F3F) && (csr & 0xFFC0) == FP_CSR) return;   // 只比控制位（MXCSR 的低 6 位是异常标志）
        if (g_fpLogged < 5) {
            ++g_fpLogged;
            Log("fpu: state was changed outside the game (x87 %04X, mxcsr %08X); using the defaults for the simulation", cw, csr);
        }
        SetCw(FP_CW);
        _mm_setcsr(FP_CSR);
        fixed = true;
    }
    ~SimScope()
    {
        if (--g_simDepth || !fixed) return;
        SetCw(cw);
        _mm_setcsr(csr);
    }
};
bool TrackHandles() { return g_rbOn && g_simDepth > 0 && g_rbCur >= 0 && GetCurrentThreadId() == g_mainThread; }

// 一局里漏删的句柄：原版 STG_main 的构造函数（HUD、自机、敌人、关卡……）每局都重新读一套图像、字体等，析构时大多不删，
// 回菜单再开一局就多一套（每局约 450 张图、十几个字体；字体有 40 个的上限）。构造期间新建的都记下，析构后还在的删掉。
// 声音不删（BGM 可能还在放，SE 管理器下次换曲时自己删）
std::unordered_set<int>* g_gameHandles;
bool g_collectGameHandles = false;
bool KeepAcrossGames(int h) { int t = (int)((unsigned)h >> 26 & 0x1F); return t >= 3 && t <= 6; }   // 声音、软件声音、音乐、视频

// 调试：每个还在的 DxLib 句柄是从哪里建的（找练习重开时漏删的）
struct HandleOrigin { int type; uint32_t from[4]; };
std::unordered_map<int, HandleOrigin>* g_handleOrigins;

void NoteHandle(int type, int h)
{
    if (!g_handleOrigins) g_handleOrigins = new std::unordered_map<int, HandleOrigin>;
    HandleOrigin o { type, {} };
    void* bt[16];
    USHORT n = RtlCaptureStackBackTrace(2, 16, bt, nullptr);
    int k = 0;
    for (USHORT i = 0; i < n && k < 4; ++i) {
        uintptr_t a = (uintptr_t)bt[i];
        if (a >= Va(0x401000) && a < Va(0x666000)) o.from[k++] = (uint32_t)(a - g_base + 0x400000);   // 游戏自己的代码（DxLib 之外）
    }
    (*g_handleOrigins)[h] = o;
}

// 调试命令 handles：按来源统计还在的句柄（和上一次比较）
void DebugHandlesImpl()
{
    if (!g_handleOrigins) return;
    std::map<std::string, int> count;
    for (auto& [h, o] : *g_handleOrigins) {
        char key[96];
        snprintf(key, sizeof key, "t%d %X %X %X %X", o.type, o.from[0], o.from[1], o.from[2], o.from[3]);
        ++count[key];
    }
    static std::map<std::string, int> s_last;
    int total = 0;
    for (auto& [k, c] : count) {
        total += c;
        auto it = s_last.find(k);
        int before = it == s_last.end() ? 0 : it->second;
        if (c != before) Log("handles %s: %d (%+d)", k.c_str(), c, c - before);
    }
    Log("handles: %d live", total);
    s_last = count;
}

int __cdecl AddHandleHook(int type, int async, int handle)
{
    // 按指定的编号重建（DxLib 自己删掉一个句柄后马上用同一个编号再建，例如给流式播放的 BGM 设循环位置 0x6A8F80，
    // 游戏放 BGM 时 0x41FDA4 会调用）：这个编号的删除要是还推迟着，编号还占着，就会建成另一个编号，
    // 游戏手里的还是原来的那个，推迟的删除执行后 BGM 就没了。先把它真删掉，重建出来还是原来的编号（游戏一直在用它，回滚也不删）
    if (handle != -1 && g_rbOn)
        for (RbSlot& s : g_slots) {
            auto it = std::find(s.deleted.begin(), s.deleted.end(), handle);
            if (it == s.deleted.end()) continue;
            s.deleted.erase(it);
            g_origSubHandle(handle);
            Log("rollback: handle %08X re-created in place (frame %d)", (unsigned)handle, g_rbCur);
            break;
        }
    int h = g_origAddHandle(type, async, handle);
    if (g_debug && h != -1 && GetCurrentThreadId() == g_mainThread) NoteHandle(type, h);
    if (g_collectGameHandles && h != -1 && handle == -1 && GetCurrentThreadId() == g_mainThread) g_gameHandles->insert(h);
    if (h != -1 && handle == -1 && TrackHandles()) {
        g_slots[g_rbCur % RB_RING].created.push_back(h);
        if (g_debug && g_resim) {   // 调试：重跑时新建句柄的调用链（找每帧都建图的地方）
            static int s_logged;
            if (s_logged < 60) {
                void* bt[12];
                USHORT n = RtlCaptureStackBackTrace(1, 12, bt, nullptr);
                char line[256];
                int len = snprintf(line, sizeof line, "handle: frame %d type %d:", g_rbCur, type);
                for (USHORT i = 0; i < n && len < (int)sizeof line - 12; ++i)
                    if ((uintptr_t)bt[i] >= 0x400000 && (uintptr_t)bt[i] < 0x9E0000) len += snprintf(line + len, sizeof line - len, " %X", (unsigned)(uintptr_t)bt[i]);
                Log("%s", line);
                ++s_logged;
            }
        }
    }
    return h;
}

int __cdecl SubHandleHook(int h)
{
    if (g_gameHandles && GetCurrentThreadId() == g_mainThread) g_gameHandles->erase(h);
    if (g_debug && g_handleOrigins && !TrackHandles() && GetCurrentThreadId() == g_mainThread) g_handleOrigins->erase(h);
    if (h != -1 && TrackHandles()) {
        g_slots[g_rbCur % RB_RING].deleted.push_back(h);
        return 0;
    }
    return g_origSubHandle(h);
}

// 一帧移出回滚范围 / 回滚结束：推迟的删除真删
void FlushDeletes(RbSlot& s)
{
    for (int h : s.deleted) { g_origSubHandle(h); if (g_handleOrigins) g_handleOrigins->erase(h); }
    s.deleted.clear();
    s.created.clear();
}

// 一帧被回滚丢掉：它新建的删掉，它要删的不删了
// 句柄的类型（DxLib：位 26..30）。3 = 声音
int HandleType(int h) { return (int)((unsigned)h >> 26 & 0x1F); }
constexpr int HT_SOUND = 3;
constexpr uint32_t VA_STOP_SOUND = 0x6AB2C0;   // StopSoundMem

void DiscardFrame(RbSlot& s)
{
    for (int h : s.created) {
        // 正在放的声音（切曲时新读的 BGM）先停：直接删一个还在流式播放的声音，DxLib 的声音线程会读写已经释放的缓冲而崩溃。
        // 游戏自己删 BGM 时也是先停再删（0x4200B0）
        if (HandleType(h) == HT_SOUND) {
            Log("rollback: discarding sound %08X created in frame %d", (unsigned)h, s.frame);
            ((int(__cdecl*)(int))Va(VA_STOP_SOUND))(h);
        }
        g_origSubHandle(h);
    }
    s.created.clear();
    s.deleted.clear();
}
uint8_t* g_shadow = nullptr;
size_t g_shadowCommitted = 0;
std::vector<void*> g_wwPages;

uintptr_t g_hot[64 + MAX_LEARNED];     // 热页地址
uint8_t* g_hotBase = nullptr;          // 各热页变成热页时的内容
volatile LONG g_hotN = 0;
uint8_t* g_stProt = nullptr;           // 跟踪范围里每页一个字节：1 = 写保护中
uint8_t* g_stCopyOnWrite = nullptr;    // 原来的保护是 PAGE_WRITECOPY
CRITICAL_SECTION g_hotLock;
PVOID g_veh = nullptr;
size_t StPages() { return (ST_END - ST_BEGIN) >> 12; }

void AddHot(uintptr_t page)
{
    LONG i = g_hotN;
    g_hot[i] = page;
    memcpy(g_hotBase + (size_t)i * 4096, (void*)page, 4096);
    g_hotN = i + 1;
}

LONG CALLBACK RbVeh(EXCEPTION_POINTERS* e)
{
    auto r = e->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 || r->ExceptionInformation[0] != 1)
        return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t a = r->ExceptionInformation[1];
    if (!g_stProt || a < Va(ST_BEGIN) || a >= Va(ST_END)) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t page = a & ~(uintptr_t)0xFFF;
    size_t idx = (page - Va(ST_BEGIN)) >> 12;
    EnterCriticalSection(&g_hotLock);
    bool ok = true;
    if (g_stProt[idx]) {
        if ((size_t)g_hotN < sizeof g_hot / sizeof g_hot[0]) AddHot(page);
        DWORD old;
        ok = VirtualProtect((void*)page, 4096, g_stCopyOnWrite[idx] ? PAGE_WRITECOPY : PAGE_READWRITE, &old) != 0;
        g_stProt[idx] = 0;
    }
    LeaveCriticalSection(&g_hotLock);
    if (!ok) return EXCEPTION_CONTINUE_SEARCH;
    // 别的线程刚放开的页也走这里：重试那条写指令
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void*)page, &mbi, sizeof mbi)) return EXCEPTION_CONTINUE_SEARCH;
    return (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

void SmallSave(std::vector<uint8_t>& out)
{
    std::vector<std::pair<void*, size_t>> parts;
    CoopSimState(parts);
    size_t n = VA_RAND_END - VA_RAND_BEGIN;
    for (auto& p : parts) n += p.second;
    out.resize(n);
    uint8_t* w = out.data();
    memcpy(w, (void*)Va(VA_RAND_BEGIN), VA_RAND_END - VA_RAND_BEGIN);
    w += VA_RAND_END - VA_RAND_BEGIN;
    for (auto& p : parts) { memcpy(w, p.first, p.second); w += p.second; }
}

void SmallLoad(const std::vector<uint8_t>& in)
{
    std::vector<std::pair<void*, size_t>> parts;
    CoopSimState(parts);
    const uint8_t* r = in.data();
    memcpy((void*)Va(VA_RAND_BEGIN), r, VA_RAND_END - VA_RAND_BEGIN);
    r += VA_RAND_END - VA_RAND_BEGIN;
    for (auto& p : parts) { memcpy(p.first, r, p.second); r += p.second; }
}

bool ShadowSync()
{
    size_t need = g_committed - g_pool;
    if (need > g_shadowCommitted) {
        if (!VirtualAlloc(g_shadow + g_shadowCommitted, need - g_shadowCommitted, MEM_COMMIT, PAGE_READWRITE)) return false;
        g_shadowCommitted = need;   // 新提交的页在池里也是从全 0 开始的
    }
    return true;
}

RbSlot* SlotOf(int frame)
{
    if (frame < 0) return nullptr;
    RbSlot& s = g_slots[frame % RB_RING];
    return s.frame == frame ? &s : nullptr;
}

// 写保护 / 放开跟踪范围里还没变成热页的页（连续的一段一起，按原来的保护分段）
void StProtect(bool on)
{
    size_t n = StPages();
    for (size_t i = 0; i < n;) {
        if (!g_stProt[i]) { ++i; continue; }
        size_t j = i;
        while (j < n && g_stProt[j] && g_stCopyOnWrite[j] == g_stCopyOnWrite[i]) ++j;
        DWORD old;
        VirtualProtect((void*)(Va(ST_BEGIN) + i * 4096), (j - i) * 4096,
            on ? PAGE_READONLY : (g_stCopyOnWrite[i] ? PAGE_WRITECOPY : PAGE_READWRITE), &old);
        i = j;
    }
}

// ---------------------------------------------------------------- 一帧的模拟（不画）
constexpr uint32_t VA_GAME_RUN = 0x6356E0, CALL_GAME_RUN = 0x6160BE;   // Game::Run：当前场景的更新（ecx = 主循环里的 Game 对象）
constexpr uint32_t PATCH_DRAW = 0x6162A8;                              // mov eax,[ecx] / call [eax+10h]：画当前场景
constexpr uint32_t VA_EFFECT_MAIN_GET = 0x41D990, VA_EFFECT_UPDATE = 0x40D770;   // 主循环在 Game::Run 之后更新特效
constexpr uint32_t VA_LOADING = 0x2FAFEC8;
using ThisFn = void(__thiscall*)(uintptr_t);

uintptr_t g_game = 0;

void RtMark(int k);

void __fastcall GameRunHook(uintptr_t game, void*)
{
    g_game = game;
    SimScope sim;
    ((ThisFn)Va(VA_GAME_RUN))(game);
    RtMark(1);
}

// 主循环里模拟的其余部分：特效更新、切曲、成就（回滚时跟踪句柄要算在模拟里）
constexpr uint32_t CALL_EFFECT_UPDATE = 0x61618F, CALL_BGM_UPDATE = 0x616356, CALL_ACHIEVE_UPDATE = 0x616392;
void __fastcall EffectUpdateHook(uintptr_t self, void*) { SimScope sim; ((ThisFn)Va(0x40D770))(self); RtMark(2); }
void __fastcall BgmUpdateHook(uintptr_t self, void*) { SimScope sim; ((ThisFn)Va(0x41FF30))(self); }
void __fastcall AchieveUpdateHook(uintptr_t self, void*) { SimScope sim; ((ThisFn)Va(0x406640))(self); }

// "跳帧"设置（0x2FB0234 对象）：这一帧更新特效、画场地吗（主循环 0x616110 起、STG 绘制 0x6537F0 起，条件一样）
bool SkipAllowsFrame()
{
    if (!At<uint8_t>(Va(0x2FB0238))) return true;
    int mode = At<int>(Va(0x2FB0244)), cnt = At<int>(Va(0x2FB0240)), div = At<int>(Va(0x2FB023C));
    if (div <= 0) return true;
    if (mode == 0) return cnt % div == 0;
    if (mode == 1) return cnt % div != div - 1;
    return false;
}

// 同主循环：加载中不更新特效
bool EffectsThisFrame() { return !At<uint8_t>(Va(VA_LOADING)) && SkipAllowsFrame(); }

// 同 STG 绘制 0x6537F0：跳过的帧不画；STG+0x7AFC 为 2、3、5 时画场地 0x64F630（演出在里面）
bool FieldDrawThisFrame()
{
    uintptr_t stg = CoopStg();
    if (!stg) return false;
    int mode = At<int>(stg + 0x7AFC);
    return (mode == 2 || mode == 3 || mode == 5) && SkipAllowsFrame();
}

// 一帧的输入：Controller 单例整个（按键持续帧数表之外还有别的数组）+ 其他座位的按键持续帧数
constexpr uint32_t VA_CONTROLLER = 0x2FB2A78, CONTROLLER_SIZE = 0x160;
// 音效请求：88 个 {byte 播放, …, int 音量} 每个 16 字节。原版在画完之后播放并清掉（0x420210）；重跑的帧不放音，直接清掉
constexpr uint32_t VA_SE_FLAGS = 0x10896BC, SE_COUNT = 88;
// 主循环画完之后还有三件改状态的事，重跑的帧照做：
//   音乐：SE 管理器 0x41FF30 处理切曲请求（倒数、开始播放，顺带点亮音乐室）
//   成就：0x406640 每帧计数、每 2 秒检查一次、弹出提示
//   跳帧：0x2FB0234 对象的倒数（决定特效这一帧更新不更新）
constexpr uint32_t VA_SE_MANAGER = 0x10896B8, VA_BGM_UPDATE = 0x41FF30;
constexpr uint32_t VA_ACHIEVE = 0x1254F90, VA_ACHIEVE_UPDATE = 0x406640;

struct FrameInput {
    uint8_t ctrl[CONTROLLER_SIZE];
    int hold[MAX_SEATS][16];
};

void SimulateCore();

void SimulateFrame(const FrameInput& in)
{
    memcpy((void*)Va(VA_CONTROLLER), in.ctrl, CONTROLLER_SIZE);
    CoopSetInputs(in.hold);
    SimulateCore();
}

// 不同步诊断：模拟里 GetRand 的调用位置（见下面的 GetRandHook）
constexpr uint32_t VA_GETRAND = 0x6A1E50, VA_GENRAND = 0x78E5C0;
constexpr int RT_RING = 128, RT_MAX = 160;
struct RandTrace { int frame; int n; uint32_t start; uint32_t site[RT_MAX]; };   // start：开始时梅森旋转的位置（调试）
RandTrace g_rt[RT_RING];
int g_rtFrame = -1;   // 正在模拟的帧（联机局；-1 = 不记）

bool g_resim = false;   // 正在重跑（不画）

// 调试：随机数追踪里插一个标记（编号 k、当时梅森旋转的位置）
void RtMark(int k)
{
    if (!g_debug || g_rtFrame < 0) return;
    RandTrace& t = g_rt[g_rtFrame % RT_RING];
    if (t.frame != g_rtFrame) return;
    if (t.n < RT_MAX) t.site[t.n] = 0xF0000000u | (uint32_t)k << 24 | (At<uint32_t>(Va(VA_RAND_BEGIN)) & 0xFFFF);
    ++t.n;
}
bool g_inDraw = false;  // 在主循环的绘制里（RbDrawStub）

// ボム演出：场地绘制 0x64F630 里按演出编号调用 0x41B310（this = Effect_main 0x2FB0590，[+0x170] 是演出对象，
// 编号不对的直接返回）。它除了画以外还推进演出：计时 +0xC 每次加一，到某些帧触发震屏（0x2FB0254）、闪屏（0x2FB2A40）、
// 放音效、每帧生成几十个粒子特效（位置用随机数）、建图、结束演出。这些都进模拟（震屏的偏移加在场地矩形 [STG+0x1C] 上、
// 震屏每帧抽随机数；粒子由模拟里的特效更新推进），只在画出来的帧里做就会不同步：回滚重跑的帧不画。
// 所以开着逐帧快照时：
//   正常帧（绘制里）：随机数先换成绘制开始时的（= 这一帧模拟完的；不然要看绘制在它之前用掉了多少），
//                    在 SimScope 里调用（浮点状态统一、新建的句柄记在这一帧），完了换回绘制自己的随机数
//   重跑的帧：同样的条件下在模拟之后调用一次（画到一张看不见的小图上），随机数用完还原
constexpr uint32_t VA_EFFECT_OBJ = 0x2FB0590, VA_CUTIN = 0x41B310, VA_SET_DRAW_SCREEN = 0x67F0F0, VA_DRAW_SCREEN = 0xB95038;
const uint32_t CUTIN_CALLS[] = { 0x64FE54, 0x64FF38, 0x64FFA8, 0x650100, 0x650170, 0x6501E0, 0x650250, 0x6502C0, 0x650330,
                                 0x6503A0, 0x650410, 0x650480, 0x6504F0, 0x650560, 0x6505D0 };   // 场地绘制里的 call 0x41B310
const int CUTIN_IDS[] = { 0x21, 0x22, 0x23, 0x26, 0x27, 0x28, 0x29, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x2A, 0x32 };   // 它们的编号
int g_cutinScreen = -1;   // 看不见的小图（RbBegin 里建）
bool g_cutinOff = false;  // 调试：LE04MULTI_NOCUTINFIX（对比用，两处都不改）

// 有的演出（如 0x21 号）在计时 0xA0 时截当前画面（场地矩形）切成 20×20 的碎片（GetDrawScreenGraph 0x6706D0）。
// 重跑的帧画到看不见的小图上，从那里截就是空的：改从重跑前的画面（上一帧画好的）截
constexpr uint32_t CALL_CUTIN_CAPTURE = 0x41B902, VA_GET_DRAW_SCREEN_GRAPH = 0x6706D0;
int g_cutinRealScreen = -1;   // ResimCutin 里：原来的绘制目标

int __cdecl RbCutinCapture(int x1, int y1, int x2, int y2, int graph, int useClient)
{
    using CaptureFn = int(__cdecl*)(int, int, int, int, int, int);
    using SetScreenFn = int(__cdecl*)(int);
    if (!g_resim || g_cutinRealScreen == -1) return ((CaptureFn)Va(VA_GET_DRAW_SCREEN_GRAPH))(x1, y1, x2, y2, graph, useClient);
    ((SetScreenFn)Va(VA_SET_DRAW_SCREEN))(g_cutinRealScreen);
    int r = ((CaptureFn)Va(VA_GET_DRAW_SCREEN_GRAPH))(x1, y1, x2, y2, graph, useClient);
    ((SetScreenFn)Va(VA_SET_DRAW_SCREEN))(g_cutinScreen);
    return r;
}

// 演出正在进行、编号是 id（0x41B310 会做事）
bool CutinActive(int id)
{
    uintptr_t eff = Va(VA_EFFECT_OBJ);
    if (At<uint8_t>(eff + 0x1148)) return false;
    uintptr_t obj = At<uintptr_t>(eff + 0x170);
    return obj && At<uint8_t>(obj) == 1 && At<int>(obj + 0x10) == id;
}

void CallCutin(uintptr_t eff, int id) { ((void(__thiscall*)(uintptr_t, int))Va(VA_CUTIN))(eff, id); }

void ResimCutin()
{
    if (g_cutinOff || g_cutinScreen == -1 || !FieldDrawThisFrame()) return;
    uintptr_t eff = Va(VA_EFFECT_OBJ);
    uintptr_t obj = At<uintptr_t>(eff + 0x170);
    if (!obj) return;
    int id = At<int>(obj + 0x10);
    bool known = false;
    for (int c : CUTIN_IDS) known |= c == id;
    if (!known || !CutinActive(id)) return;
    using SetScreenFn = int(__cdecl*)(int);
    int prev = At<int>(Va(VA_DRAW_SCREEN));
    RngKeep(RNG_RESIM_CUTIN);
    RtMark(3);
    ((SetScreenFn)Va(VA_SET_DRAW_SCREEN))(g_cutinScreen);
    g_cutinRealScreen = prev;
    CallCutin(eff, id);
    g_cutinRealScreen = -1;
    ((SetScreenFn)Va(VA_SET_DRAW_SCREEN))(prev);
    RngPut(RNG_RESIM_CUTIN);
    RtMark(4);
}

void SimulateCore()
{
    SimScope sim;
    g_resim = true;
    ((ThisFn)Va(VA_GAME_RUN))(g_game);
    RtMark(1);
    if (EffectsThisFrame()) { ((ThisFn)Va(VA_EFFECT_UPDATE))(((uintptr_t(__cdecl*)())Va(VA_EFFECT_MAIN_GET))()); RtMark(2); }
    ResimCutin();   // 正常帧里它在绘制里（特效更新之后、放音效之前）
    RtMark(5);
    // STG 绘制 0x6537F0 的最后把 STG 状态 +0x7AF8 抄到 +0x7AFC（跳帧的帧不抄）。世界更新 0x651EA0 会读 +0x7AFC
    // （是 3、5 时跳过一些东西），所以重跑的帧也要抄，不然状态在重跑的几帧里变了（对话开始 / 结束）就不同步
    if (uintptr_t stg = CoopStg())
        if (SkipAllowsFrame()) At<int>(stg + 0x7AFC) = At<int>(stg + 0x7AF8);
    for (uint32_t i = 0; i < SE_COUNT; ++i) At<uint8_t>(Va(VA_SE_FLAGS) + i * 16) = 0;
    ((ThisFn)Va(VA_BGM_UPDATE))(Va(VA_SE_MANAGER));
    ((ThisFn)Va(VA_ACHIEVE_UPDATE))(Va(VA_ACHIEVE));
    if (At<uint8_t>(Va(0x2FB0238)) == 1) {   // 同 0x616420
        int& cnt = At<int>(Va(0x2FB0240));
        if (cnt > 0) --cnt;
        else {
            At<uint8_t>(Va(0x2FB0238)) = 0;
            cnt = 0;
            At<int>(Va(0x2FB023C)) = 3;
        }
    }
    g_resim = false;
}

// ---------------------------------------------------------------- 自检
enum { RB_IDLE, RB_ARMED, RB_RECORD };
int g_rbState = RB_IDLE, g_rbTarget = 0;
Snapshot g_rbStart;
std::vector<FrameInput> g_rbInputs;

// 同步测试（调试命令 synctest K，同 GGPO 的 SyncTest）：逐帧快照开着，每 30 帧回滚 K 帧、按记下的输入重跑，
// 和回滚前的状态逐字节比较
int g_stK = 0;
bool g_stWrong = false;   // synctest -K：先按错的按键重跑一遍，再回去按对的（模仿联机时预测错了）
int g_stFrame = 0;
FrameInput g_stInputs[RB_RING];
int g_stTests = 0, g_stBad = 0;
double g_stWorstMs = 0;

} // namespace

// ---------------------------------------------------------------- 逐帧快照：对外
bool RbBegin()
{
    if (!g_meta || g_poolFull || g_poolOff) { Log("rollback: no pool"); return false; }
    if (g_rbOn) RbEnd();
    if (!g_shadow) {
        g_shadow = (uint8_t*)VirtualAlloc(nullptr, g_reserve, MEM_RESERVE, PAGE_READWRITE);
        g_hotBase = (uint8_t*)VirtualAlloc(nullptr, sizeof g_hot / sizeof g_hot[0] * 4096, MEM_COMMIT, PAGE_READWRITE);
        g_stProt = (uint8_t*)calloc(StPages(), 1);
        g_stCopyOnWrite = (uint8_t*)calloc(StPages(), 1);
        InitializeCriticalSection(&g_hotLock);
        g_wwPages.resize(g_reserve / 4096);
        if (!g_shadow || !g_hotBase || !g_stProt || !g_stCopyOnWrite) { Log("rollback: out of memory"); return false; }
    }
    if (!ShadowSync()) { Log("rollback: cannot commit shadow"); return false; }
    memcpy(g_shadow, g_pool, g_committed - g_pool);
    ResetWriteWatch(g_pool, g_committed - g_pool);
    for (auto& s : g_slots) { s.frame = -1; s.undoPages.clear(); s.undoData.clear(); s.created.clear(); s.deleted.clear(); }
    g_rbCur = -1;
    // 静态数据：初始热页 + 其余页写保护
    g_hotN = 0;
    for (auto& r : HOT_INIT)
        for (uint32_t va = r.va; va < r.end; va += 4096) AddHot(Va(va));
    size_t n = StPages();
    for (size_t i = 0; i < n; ++i) {
        uintptr_t page = Va(ST_BEGIN) + i * 4096;
        MEMORY_BASIC_INFORMATION mbi {};
        VirtualQuery((void*)page, &mbi, sizeof mbi);
        g_stCopyOnWrite[i] = mbi.Protect == PAGE_WRITECOPY;
        bool hot = false;
        for (LONG h = 0; h < g_hotN; ++h) hot |= g_hot[h] == page;
        g_stProt[i] = !hot;
    }
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, RbVeh);
    if (g_cutinScreen == -1) g_cutinScreen = ((int(__cdecl*)(int, int, int))Va(0x67E0D0))(16, 16, 0);   // MakeScreen（不在模拟里，不跟着回滚）
    StProtect(true);
    g_rbOn = true;
    Log("rollback: on (pool %u KB, %d hot pages)", (unsigned)((g_committed - g_pool) / 1024), (int)g_hotN);
    if (g_debug) DebugMemory("at game start");
    return true;
}

void RbEnd()
{
    if (!g_rbOn) return;
    EnterCriticalSection(&g_hotLock);
    StProtect(false);
    memset(g_stProt, 0, StPages());
    LeaveCriticalSection(&g_hotLock);
    for (auto& s : g_slots) {
        FlushDeletes(s);
        s.frame = -1;
        std::vector<uint32_t>().swap(s.undoPages);
        std::vector<uint8_t>().swap(s.undoData);
        std::vector<uint8_t>().swap(s.hot);
    }
    g_rbOn = false;
    g_rbCur = -1;
    Log("rollback: off (%d hot pages)", (int)g_hotN);
}

bool RbActive() { return g_rbOn; }

// 不同步诊断：第 frame 帧开始模拟（之后的 GetRand 调用记在这一帧；回滚重跑时重记）
void RbTraceFrame(int frame)
{
    g_rtFrame = frame;
    if (frame < 0) return;
    RandTrace& t = g_rt[frame % RT_RING];
    t.frame = frame;
    t.n = 0;
    t.start = At<uint32_t>(Va(VA_RAND_BEGIN)) ^ At<uint32_t>(Va(VA_RAND_BEGIN) + 0x9D0) * 31;
}

// 不同步诊断：第 frame 帧模拟里 GetRand 的调用位置写进日志
void RbTraceDump(int frame)
{
    const RandTrace& t = g_rt[frame % RT_RING];
    if (frame < 0 || t.frame != frame) return;
    char line[1000];
    int len = snprintf(line, sizeof line, "diag rand %d: %d calls:", frame, t.n);
    for (int i = 0; i < t.n && i < RT_MAX && len < (int)sizeof line - 20;) {   // 连续同一处写成 地址*次数
        int j = i + 1;
        while (j < t.n && j < RT_MAX && t.site[j] == t.site[i]) ++j;
        len += j - i > 1 ? snprintf(line + len, sizeof line - len, " %X*%d", t.site[i], j - i) : snprintf(line + len, sizeof line - len, " %X", t.site[i]);
        i = j;
    }
    Log("%s", line);
}

// ---------------------------------------------------------------- 不同步诊断：模拟里谁调用了 GetRand
// GetRand（0x6A1E50）整个换成自己的（同样的算法：梅森旋转 0x78E5C0 取 32 位 × (max+1) 取高 32 位），
// 联机局里记下每帧模拟中每次调用的位置（返回地址）。不同步时两边各写出来对比

int __cdecl GetRandHook(int max)
{
    uint32_t site = (uint32_t)(uintptr_t)_ReturnAddress();
    uint32_t r = ((uint32_t(__cdecl*)())Va(VA_GENRAND))();
    bool mainThread = GetCurrentThreadId() == g_mainThread;
    bool other = g_debug && (!mainThread || g_simDepth == 0 && !g_inDraw);   // 调试：模拟、绘制之外的调用也记（地址最高位为 1；别的线程再加 0x40000000）
    if (g_rtFrame >= 0 && (g_simDepth > 0 && mainThread || other)) {
        RandTrace& t = g_rt[g_rtFrame % RT_RING];
        if (t.frame == g_rtFrame) {
            if (t.n < RT_MAX) t.site[t.n] = site | (other ? 0x80000000u : 0) | (mainThread ? 0 : 0x40000000u);
            ++t.n;
        }
    }
    int64_t p = (int64_t)(uint64_t)r * ((int64_t)max + 1);
    return (int)(uint32_t)((uint64_t)p >> 32);
}

// 调试：LE04MULTI_FPUMESS=1 时在模拟之外把浮点状态改掉（模仿叠加层软件），测上面的纠正
void RbDebugMessFpu()
{
    static bool s_on = g_debug && getenv("LE04MULTI_FPUMESS");
    if (!s_on) return;
    _mm_setcsr(0x9FC0);   // FTZ + DAZ
    SetCw(0x007F);        // x87 单精度
}
bool RbInPool(const void* p) { return InPool(p); }

// 调试：第 frame 帧（模拟里）新建 / 要删的句柄数
void RbFrameHandles(int frame, int* created, int* deleted)
{
    RbSlot* s = SlotOf(frame);
    *created = s ? (int)s->created.size() : -1;
    *deleted = s ? (int)s->deleted.size() : -1;
}

// 第 frame 帧开始时调用（这一帧的 Controller::Update 之前）
void RbSave(int frame)
{
    if (!g_rbOn) return;
    // 上一帧写过的内存池页：影子里的旧内容（上一帧开始时）记进上一帧的撤销日志，影子更新成现在的
    ShadowSync();
    RbSlot* prev = SlotOf(frame - 1);
    ULONG_PTR count = g_wwPages.size();
    ULONG gran = 0;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, g_pool, g_committed - g_pool, g_wwPages.data(), &count, &gran) == 0) {
        if (prev) {
            size_t base = prev->undoPages.size();
            prev->undoPages.resize(base + count);
            prev->undoData.resize((base + count) * 4096);
            for (ULONG_PTR i = 0; i < count; ++i) {
                size_t off = (uint8_t*)g_wwPages[i] - g_pool;
                prev->undoPages[base + i] = (uint32_t)(off >> 12);
                memcpy(&prev->undoData[(base + i) * 4096], g_shadow + off, 4096);
            }
        }
        for (ULONG_PTR i = 0; i < count; ++i) {
            size_t off = (uint8_t*)g_wwPages[i] - g_pool;
            memcpy(g_shadow + off, g_pool + off, 4096);
        }
    }
    RbSlot& s = g_slots[frame % RB_RING];
    if (s.frame != frame) FlushDeletes(s);   // 环绕：那一帧已经不会再回滚了（同一帧重存时，列表已在回滚时处理过）
    s.frame = frame;
    g_rbCur = frame;
    s.undoPages.clear();
    s.undoData.clear();
    if (s.undoData.capacity() > (256u << 12)) {   // 加载那几帧会写几千页：用完还掉，不然 64 个槽各留一份峰值
        std::vector<uint32_t>().swap(s.undoPages);
        std::vector<uint8_t>().swap(s.undoData);
    }
    s.hotCount = (size_t)g_hotN;
    s.hot.resize(s.hotCount * 4096);
    for (size_t i = 0; i < s.hotCount; ++i) memcpy(&s.hot[i * 4096], (void*)g_hot[i], 4096);
    SmallSave(s.small);
}

// 第 frame 帧及之前的输入都确定了，不会再回滚到它们：推迟的删除现在就删（不然每帧重建的字体、截屏图会攒 64 帧）
void RbConfirm(int frame)
{
    if (!g_rbOn) return;
    for (auto& s : g_slots)
        if (s.frame >= 0 && s.frame <= frame && !s.deleted.empty()) {
            for (int h : s.deleted) g_origSubHandle(h);
            s.deleted.clear();
        }
}

bool RbCanRestore(int g, int cur)
{
    if (!g_rbOn || g > cur || cur - g >= RB_RING) return false;
    for (int b = g; b <= cur; ++b)
        if (!SlotOf(b)) return false;
    return true;
}

// 回到第 g 帧开始时（现在在第 cur 帧开始，RbSave(cur) 已调用）
bool RbRestore(int g, int cur)
{
    if (!RbCanRestore(g, cur)) return false;
    for (int b = cur; b >= g; --b) DiscardFrame(*SlotOf(b));
    for (int b = cur - 1; b >= g; --b) {
        RbSlot& s = *SlotOf(b);
        for (size_t i = s.undoPages.size(); i-- > 0;) {   // 同一页在一帧里只会出现一次，倒序只是稳妥
            size_t off = (size_t)s.undoPages[i] << 12;
            memcpy(g_pool + off, &s.undoData[i * 4096], 4096);
            memcpy(g_shadow + off, &s.undoData[i * 4096], 4096);
        }
    }
    ResetWriteWatch(g_pool, g_committed - g_pool);
    RbSlot& s = *SlotOf(g);
    static uint8_t keep[4 + 0x24 + 0x1008];   // PRESERVE 的总长
    size_t k = 0;
    for (auto& r : PRESERVE) { memcpy(keep + k, (void*)Va(r.va), r.end - r.va); k += r.end - r.va; }
    for (LONG i = 0; i < g_hotN; ++i)
        memcpy((void*)g_hot[i], (size_t)i < s.hotCount ? &s.hot[i * 4096] : g_hotBase + (size_t)i * 4096, 4096);
    k = 0;
    for (auto& r : PRESERVE) { memcpy((void*)Va(r.va), keep + k, r.end - r.va); k += r.end - r.va; }
    SmallLoad(s.small);
    for (int b = g + 1; b <= cur; ++b) g_slots[b % RB_RING].frame = -1;
    s.undoPages.clear();
    s.undoData.clear();
    g_rbCur = g;   // 接下来重跑第 g 帧
    return true;
}

// 只在绘制里推进的状态：回滚时保留回滚前的值（不然背景卷动等会往回跳几帧）。
//   关卡背景 [[STG+0xC]]：+0xC04、+0xC18；它的镜头 [背景+0x74] 的动画
//   画面截图特效（STG::Draw 里每秒 10% 触发）：STG+0x7AF4、[STG+0x7AEC] 的状态
//   HUD [STG+0x14]：+0x08..+0x18、+0x7AE0..+0x7AF0
namespace {
struct DrawField { uintptr_t addr; uint32_t size; uint8_t data[32]; };
std::vector<DrawField> g_drawKeep;

bool PoolPtr(uintptr_t p) { return p && InPool((void*)p); }

void DrawFields(std::vector<DrawField>& out)
{
    out.clear();
    uintptr_t stg = CoopStg();
    if (!PoolPtr(stg)) return;
    auto add = [&](uintptr_t a, uint32_t n) { out.push_back(DrawField { a, n, {} }); };
    uintptr_t holder = At<uintptr_t>(stg + 0xC);
    if (PoolPtr(holder)) add(holder + 0x68, 4);   // 场地绘制 0x439920 里全屏闪光的计数
    uintptr_t stage = PoolPtr(holder) ? At<uintptr_t>(holder) : 0;
    if (PoolPtr(stage)) {
        add(stage + 0xC04, 4);
        add(stage + 0xC18, 4);
        uintptr_t cam = At<uintptr_t>(stage + 0x74);   // 镜头动画：0x42C850（绘制里）推进 +0（结束时清零）、+4、+0x60..+0x74
        if (PoolPtr(cam)) { add(cam, 8); add(cam + 0x60, 0x14); }
    }
    add(stg + 0x7AF4, 4);
    uintptr_t ui = At<uintptr_t>(stg + 0x14);   // HUD：+0x08..+0x18 是画数字时用的草稿（UI::Update 每帧重抄、UI::Draw 画完是 0）
    if (PoolPtr(ui)) {
        add(ui + 0x08, 16);
        add(ui + 0x7AE0, 16);   // 4 条 HUD 消息的计时（UI::Draw 里加 1）
    }
    uintptr_t fx = At<uintptr_t>(stg + 0x7AEC);
    if (PoolPtr(fx)) add(fx, 32);
    // 截屏对象（{byte 要截, int 图, …, 宽, 高}）：模拟里只置"要截"，STG 绘制 0x6537F0 里删旧图、建图、截、清标志
    // （[+0x7AE4] 是跳帧时画的缓存）。图归绘制管，回滚时保留"截没截、现在是哪张图"
    for (uint32_t off : { 0x7ADCu, 0x7AE0u, 0x7AE4u, 0x7AE8u }) {
        uintptr_t cap = At<uintptr_t>(stg + off);
        if (PoolPtr(cap)) add(cap, 8);
    }
}
}

void RbKeepDraw()
{
    DrawFields(g_drawKeep);
    for (auto& f : g_drawKeep) memcpy(f.data, (void*)f.addr, f.size);
}

void RbPutDraw()
{
    std::vector<DrawField> now;
    DrawFields(now);
    // 对象还在原处（重跑期间没换关卡）才写回
    for (auto& f : g_drawKeep)
        for (auto& n : now)
            if (n.addr == f.addr && n.size == f.size) memcpy((void*)f.addr, f.data, f.size);
}

// 场景切换（STG 析构、重开、回标题……）已经要求了、下一帧 Game::Run 一开头就做：Game+8 是下一个场景（8 = 不切）。
// 切场景（还会在新 STG 的构造里和别人会合）不能回滚，只能在确定的帧里做
bool RbSceneSwitchPending() { return g_game && At<int>(g_game + 8) != 8; }

// 重跑一帧（不画）：输入已经放好（Controller + 各座位的按键持续帧数）
void RbSimulate() { SimulateCore(); }

// 绘制前后存 / 还原随机数状态：原版画特效时会调用 GetRand，不画（回滚重跑）时模拟结果就会不一样。
// 只在开着逐帧快照（回滚的联机局、同步测试）或 rbtest 时这样做，其他时候和原版一样（单人的录像不受影响）
static uint8_t g_drawRng[VA_RAND_END - VA_RAND_BEGIN];
static bool g_drawRngSaved;
extern "C" void __cdecl RbSaveRng()
{
    g_inDraw = true;
    g_drawRngSaved = g_rbOn || g_rbState != RB_IDLE;
    if (g_drawRngSaved) memcpy(g_drawRng, (void*)Va(VA_RAND_BEGIN), sizeof g_drawRng);
}
extern "C" void __cdecl RbRestoreRng()
{
    if (g_drawRngSaved) { memcpy((void*)Va(VA_RAND_BEGIN), g_drawRng, sizeof g_drawRng); RtMark(5); }
    g_inDraw = false;
}

// 场地绘制里调用 ボム演出 0x41B310 的地方（见 ResimCutin 上面）
void __fastcall CutinDrawHook(uintptr_t eff, void*, int id)
{
    if (!g_drawRngSaved || g_cutinOff || !CutinActive(id)) { CallCutin(eff, id); return; }
    RngKeep(RNG_DRAW_CUTIN);
    memcpy((void*)Va(VA_RAND_BEGIN), g_drawRng, sizeof g_drawRng);
    RtMark(3);
    {
        SimScope sim;
        CallCutin(eff, id);
    }
    memcpy((void*)Va(VA_RAND_BEGIN), g_drawRng, sizeof g_drawRng);
    RtMark(4);
    RngPut(RNG_DRAW_CUTIN);
}

// 启动设置定好之后：只有联机（可能用回滚）和调试模式（同步测试）才让游戏的分配走内存池
void ArenaUse(bool on)
{
    g_poolOff = !on;
    Log("pool: %s", on ? "used" : "not used (new allocations go to the CRT heap)");
}

// 0x63FAED：局内加载前截一张屏（删掉旧图、MakeScreen 640x480、GetDrawScreenGraph）。某些场面里每帧都做，
// 重跑时每帧都重建一张屏幕大小的图很慢（8 帧十几 ms），而且截的是旧画面：重跑时有图就不重截
constexpr uint32_t PATCH_CAPTURE = 0x63FAED, CAPTURE_RESUME = 0x63FB29, VA_CAPTURE_HANDLE = 0x2FB4A1C;
extern "C" void __cdecl RbCaptureScreen()
{
    int& h = *(int*)Va(VA_CAPTURE_HANDLE);
    if (g_resim && h != -1) return;
    ((int(__cdecl*)(int, int))Va(0x6A4B40))(h, 0);                 // DeleteGraph
    h = ((int(__cdecl*)(int, int, int))Va(0x67E0D0))(640, 480, 0);   // MakeScreen
    ((int(__cdecl*)(int, int, int, int, int, int))Va(0x6706D0))(0, 0, 640, 480, h, 1);   // GetDrawScreenGraph
}

// UI 的缩放文字（0x448DC0，esi = 那条文字）：每帧删掉旧字体、按新字号 CreateFontToHandle、量文字宽度。建字体很慢
// （一帧十几 ms），字体和宽度只给绘制用：重跑时沿用原来的字体和宽度
uint32_t g_vaDeleteFont = 0x692400, g_vaCreateFont = 0x691F00, g_vaTextWidth = 0x69E220;
extern "C" __declspec(naked) void RbUiDeleteFont()
{
    __asm {
        cmp g_resim, 0
        jne skip
        jmp dword ptr [g_vaDeleteFont]
    skip:
        xor eax, eax
        ret
    }
}
extern "C" __declspec(naked) void RbUiCreateFont()
{
    __asm {
        cmp g_resim, 0
        jne skip
        jmp dword ptr [g_vaCreateFont]
    skip:
        mov eax, [esi + 0x114]
        ret
    }
}
extern "C" __declspec(naked) void RbUiTextWidth()
{
    __asm {
        cmp g_resim, 0
        jne skip
        jmp dword ptr [g_vaTextWidth]
    skip:
        mov eax, [esi + 0x118]
        ret
    }
}

// BGM 是不是真的在放：自机 / 敌人的更新里会对"标着在放"的 BGM 问 DxLib（0x6A39E0，按真实的播放状态回答），
// 没在放就让关卡曲重新开始等。原版里 BGM 都是循环播放，标记和实际一直一致，所以这个问题的答案总是"在放"。
// 但它取决于真实的声音：回滚恢复了标记，却撤销不了已经做过的 StopSoundMem，重跑时就会答"没在放"，和别人走不同的分支而不同步
// （切曲前后回滚时）。四处调用都换成按标记回答（调用方只对标着在放的问）
constexpr uint32_t BGM_QUERY_SITES[] = { 0x41FEB8, 0x5D268D, 0x61BDBC, 0x61C5C8 };
constexpr uint32_t VA_SOUND_PLAYING = 0x6A39E0;
extern "C" int __cdecl RbBgmPlaying(int) { return 1; }

// 回滚重跑完：让真实的 BGM 播放状态跟上恢复后的标记（重跑里游戏自己停 / 放过的已经是对的；
// 被回滚撤销了的"停"要接着放，被撤销了的"放"要停）。SE 管理器 0x10896B8：+0x584 + i*0x11C 是标记，+4 是句柄，54 首
void RbSyncBgm()
{
    using CheckFn = int(__cdecl*)(int);
    using PlayFn = int(__cdecl*)(int, int, int);
    using StopFn = int(__cdecl*)(int);
    for (int i = 0; i < 0x36; ++i) {
        uintptr_t e = Va(0x10896B8 + 0x584) + i * 0x11C;
        bool flag = *(uint8_t*)e != 0;
        int h = *(int*)(e + 4);
        if (h <= 0) continue;
        int playing = ((CheckFn)Va(0x6A3A70))(h);   // CheckSoundMem：1 在放，0 没放，-1 句柄无效
        if (playing < 0) continue;
        if (flag && !playing) {
            ((PlayFn)Va(0x6A7970))(h, 3, 0);   // 循环、从停下的地方接着放（同游戏自己的恢复 0x420182）
            Log("rollback: resumed BGM %d", i);
        } else if (!flag && playing) {
            ((StopFn)Va(VA_STOP_SOUND))(h);
            Log("rollback: stopped BGM %d", i);
        }
    }
}

// 0x6162A8：mov eax,[ecx] / call [eax+10h]（ecx = 场景）
extern "C" __declspec(naked) void RbDrawStub()
{
    __asm {
        push ecx
        call RbSaveRng
        pop ecx
        mov eax, [ecx]
        call dword ptr [eax + 0x10]
        call RbRestoreRng
        ret
    }
}

bool InstallArena()
{
    g_mainThread = GetCurrentThreadId();
    InitializeCriticalSection(&g_lock);
    // 32 位进程（exe 开了大地址，64 位系统上 4GB）。一局用的内存约 10～20MB，回滚还要一份同样大小的影子副本，各保留 128MB
    for (size_t mb : { 128u, 96u, 64u }) {
        g_pool = (uint8_t*)VirtualAlloc(nullptr, (size_t)mb << 20, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE);
        if (g_pool) { g_reserve = (size_t)mb << 20; break; }
    }
    if (!g_pool) { Log("pool: cannot reserve"); return false; }
    g_committed = g_pool;
    if (!EnsureCommitted(g_pool + 4096)) return false;
    g_meta = (Meta*)g_pool;
    memset(g_meta, 0, sizeof *g_meta);
    g_meta->top = g_pool + 4096 - 16;   // 第一个块的头放在 4KB 边界前 16 字节，载荷从 4KB 对齐开始
    struct Hook { uint32_t va; void* fn; void** orig; const char* expect; };
    g_origMalloc = (MallocFn)Va(VA_MALLOC);
    {
        auto nt = (IMAGE_NT_HEADERS*)((uint8_t*)g_self + ((IMAGE_DOS_HEADER*)g_self)->e_lfanew);
        g_selfBegin = (uintptr_t)g_self;
        g_selfEnd = g_selfBegin + nt->OptionalHeader.SizeOfImage;
    }
    const Hook hooks[] = {
        { VA_NEW, (void*)NewHook, (void**)&g_origNew, "558BEC83EC10" },
        { VA_FREE, (void*)FreeHook, (void**)&g_origFree, "558BEC837D0800" },
        { VA_REALLOC, (void*)ReallocHook, (void**)&g_origRealloc, "558BEC837D0800" },
        { VA_MSIZE, (void*)MsizeHook, (void**)&g_origMsize, "558BEC837D0800" },
    };
    for (auto& h : hooks)
        if (!BytesAt(Va(h.va), Hex(h.expect)) || !InlineHookN((void*)Va(h.va), h.fn, h.orig, strlen(h.expect) / 2)) {
            Log("pool: hook %X failed", h.va);
            g_meta = nullptr;   // 已经装上的钩子会照旧走 CRT 的堆
            return false;
        }
    Log("pool: %u MB reserved at %p", (unsigned)(g_reserve >> 20), g_pool);
    return true;
}

// 游戏用 printfDx（0x6A13F0）打到画面左上角的报错（"Error:Load_Enemy_dat" 等）也记进日志（崩溃 / 不同步日志里能看到）
void* g_origPrintfDx = nullptr;
extern "C" void __cdecl LogPrintfDx(const wchar_t* fmt)
{
    static int s_n;
    if (!fmt || s_n >= 50) return;
    ++s_n;
    char buf[160];
    if (!WideCharToMultiByte(CP_UTF8, 0, fmt, -1, buf, sizeof buf, nullptr, nullptr)) buf[0] = 0;
    buf[sizeof buf - 1] = 0;
    for (char* c = buf; *c; ++c)
        if (*c == '\n' || *c == '\r') *c = ' ';
    Log("game printfDx: %s", buf);
}
extern "C" __declspec(naked) void PrintfDxHook()
{
    __asm {
        pushad
        push dword ptr [esp + 36]
        call LogPrintfDx
        add esp, 4
        popad
        jmp dword ptr [g_origPrintfDx]
    }
}

// 读敌人移动数据 csv（0x5D3220 里的 FileRead_open）失败时记下路径和当时的情况
int __cdecl EnemyCsvOpen(const wchar_t* path, int async)
{
    int h = ((int(__cdecl*)(const wchar_t*, int))Va(0x681E00))(path, async);
    if (h == -1) {
        DWORD err = GetLastError();
        char p[200];
        if (!path || !WideCharToMultiByte(CP_UTF8, 0, path, -1, p, sizeof p, nullptr, nullptr)) strcpy_s(p, "?");
        Log("enemy csv open failed: %s (async %d, win32 error %u, open files %d, resim %d, sim %d)", p, async, (unsigned)err,
            At<int>(Va(0xBF59F0) + 17 * 0x2F0 + 0x34), (int)g_resim, g_simDepth);
    }
    return h;
}

// 调试：现在在模拟里（>0）、绘制里（+100）
int RbDebugPhase() { return g_simDepth + (g_inDraw ? 100 : 0); }

bool InstallRollback()
{
    if (g_debug) {   // 调试：游戏打到画面上的报错、读敌人 csv 失败记进 multi.log
        if (BytesAt(Va(0x6A13F0), Hex("558BECB804100000"))) InlineHookN((void*)Va(0x6A13F0), (void*)PrintfDxHook, &g_origPrintfDx, 8);
        PatchCall(0x5D3493 - 0x400000, (void*)EnemyCsvOpen, 0x681E00 - 0x400000);
    }
    uintptr_t at = Va(PATCH_DRAW);
    if (!BytesAt(at, Hex("8B01FF5010"))) { Log("rollback: draw call bytes differ"); return false; }
    uint8_t code[5] = { 0xE8 };
    *(int32_t*)&code[1] = (int32_t)((uintptr_t)RbDrawStub - (at + 5));
    if (!WriteCode(at, code, 5) || !PatchCall(CALL_GAME_RUN - 0x400000, (void*)GameRunHook, VA_GAME_RUN - 0x400000)
        || !PatchCall(CALL_EFFECT_UPDATE - 0x400000, (void*)EffectUpdateHook, 0x40D770 - 0x400000)
        || !PatchCall(CALL_BGM_UPDATE - 0x400000, (void*)BgmUpdateHook, 0x41FF30 - 0x400000)
        || !PatchCall(CALL_ACHIEVE_UPDATE - 0x400000, (void*)AchieveUpdateHook, 0x406640 - 0x400000))
        return false;
    {   // 截屏：call RbCaptureScreen / sub esp,18h（原来的 add esp,18h 还在后面）/ jmp 回去
        uintptr_t c = Va(PATCH_CAPTURE);
        if (!BytesAt(c, Hex("6A00FF351C4AFB02E8"))) { Log("rollback: capture bytes differ"); return false; }
        uint8_t code[CAPTURE_RESUME - PATCH_CAPTURE];
        memset(code, 0x90, sizeof code);
        code[0] = 0xE8;
        *(int32_t*)&code[1] = (int32_t)((uintptr_t)RbCaptureScreen - (c + 5));
        code[5] = 0x83; code[6] = 0xEC; code[7] = 0x18;
        code[8] = 0xE9;
        *(int32_t*)&code[9] = (int32_t)(Va(CAPTURE_RESUME) - (c + 13));
        if (!WriteCode(c, code, sizeof code)) return false;
    }
    if (!(g_debug && getenv("LE04MULTI_NOBGMFIX")))   // 调试：对比用，不换
        for (uint32_t site : BGM_QUERY_SITES)
            if (!PatchCall(site - 0x400000, (void*)RbBgmPlaying, VA_SOUND_PLAYING - 0x400000)) return false;
    {   // GetRand 换成自己的（push ebp / mov ebp,esp / call 梅森旋转）
        uintptr_t gr = Va(VA_GETRAND);
        if (!BytesAt(gr, Hex("558BECE8"))) { Log("rollback: GetRand bytes differ"); return false; }
        uint8_t jmp[5] = { 0xE9 };
        *(int32_t*)&jmp[1] = (int32_t)((uintptr_t)GetRandHook - (gr + 5));
        if (!WriteCode(gr, jmp, 5)) return false;
    }
    g_cutinOff = g_debug && getenv("LE04MULTI_NOCUTINFIX");
    for (uint32_t site : CUTIN_CALLS)
        if (!PatchCall(site - 0x400000, (void*)CutinDrawHook, VA_CUTIN - 0x400000)) return false;
    if (!PatchCall(CALL_CUTIN_CAPTURE - 0x400000, (void*)RbCutinCapture, VA_GET_DRAW_SCREEN_GRAPH - 0x400000)) return false;
    g_vaDeleteFont = Va(0x692400);
    g_vaCreateFont = Va(0x691F00);
    g_vaTextWidth = Va(0x69E220);
    if (!PatchCall(0x448EA9 - 0x400000, (void*)RbUiDeleteFont, 0x692400 - 0x400000)
        || !PatchCall(0x448F06 - 0x400000, (void*)RbUiCreateFont, 0x691F00 - 0x400000)
        || !PatchCall(0x448F16 - 0x400000, (void*)RbUiTextWidth, 0x69E220 - 0x400000))
        return false;
    if (!BytesAt(Va(VA_ADD_HANDLE), Hex("558BEC578B7D08")) || !BytesAt(Va(VA_SUB_HANDLE), Hex("558BEC83EC08"))) {
        Log("rollback: handle functions differ");
        return false;
    }
    return InlineHookN((void*)Va(VA_ADD_HANDLE), (void*)AddHandleHook, (void**)&g_origAddHandle, 7)
        && InlineHookN((void*)Va(VA_SUB_HANDLE), (void*)SubHandleHook, (void**)&g_origSubHandle, 6);
}

void RollbackSyncTest(int k)
{
    if (k == 0) {
        if (g_stK) Log("synctest: off after %d tests, %d with differences, worst %.2f ms", g_stTests, g_stBad, g_stWorstMs);
        g_stK = 0;
        RbEnd();
        return;
    }
    g_stWrong = k < 0;
    if (k < 0) k = -k;
    if (k >= RB_RING - 1) k = RB_RING - 2;
    g_stK = k;
    g_stFrame = -1;   // 下一个帧边界开始
    g_stTests = g_stBad = 0;
    g_stWorstMs = 0;
    Log("synctest: rolling back %d frames every 30 frames%s", k, g_stWrong ? ", first with wrong inputs" : "");
}

void RollbackTest(int frames)
{
    if (!g_meta) { Log("rbtest: no pool"); return; }
    if (g_rbOn) { Log("rbtest: not while per-frame snapshots are on"); return; }
    g_rbTarget = frames > 0 ? frames : 1;
    g_rbState = RB_ARMED;
    Log("rbtest: armed for %d frames", g_rbTarget);
}

// 同步测试：每个帧边界
static void SyncTestBoundary()
{
    if (!CoopStg() || !g_game) {
        if (g_rbOn) { Log("synctest: game ended"); RbEnd(); }
        g_stFrame = -1;
        return;
    }
    if (g_stFrame < 0) {
        if (!RbBegin()) { g_stK = 0; return; }
        g_stFrame = 0;
    }
    int c = g_stFrame;
    RbSave(c);
    if (c < g_stK || c % 30) { RbTraceFrame(c); return; }
    static RandTrace orig[RT_RING];   // 第一次跑时各帧 GetRand 的调用位置（和重跑的比）
    for (int x = c - g_stK; x < c; ++x) orig[x % RT_RING] = g_rt[x % RT_RING];
    static Snapshot a, b;
    Capture(a, false);
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    RbKeepDraw();
    if (g_stWrong) {   // 先按别的帧的按键跑一遍（同联机时按预测跑过），存下各帧，再回去按对的重跑
        if (!RbRestore(c - g_stK, c)) { Log("synctest: cannot restore frame %d", c - g_stK); return; }
        for (int x = c - g_stK; x < c; ++x) {
            if (x > c - g_stK) RbSave(x);
            int w = x >= 13 ? x - 13 : x + 1 < c ? x + 1 : x;
            SimulateFrame(g_stInputs[w % RB_RING]);
        }
        RbSave(c);
    }
    if (!RbRestore(c - g_stK, c)) { Log("synctest: cannot restore frame %d", c - g_stK); return; }
    for (int x = c - g_stK; x < c; ++x) {
        if (x > c - g_stK) RbSave(x);
        RbTraceFrame(x);
        SimulateFrame(g_stInputs[x % RB_RING]);
    }
    RbPutDraw();
    RbSave(c);
    RbTraceFrame(c);
    for (int x = c - g_stK; x < c; ++x) {   // 第一个 GetRand 调用不一样的帧
        const RandTrace& a = orig[x % RT_RING];
        const RandTrace& b = g_rt[x % RT_RING];
        if (a.frame != x) continue;
        int n = a.n < b.n ? a.n : b.n, i = 0;
        while (i < n && i < RT_MAX && a.site[i] == b.site[i]) ++i;
        if (a.n == b.n && (i == n || i == RT_MAX) && a.start == b.start) continue;
        Log("synctest: GetRand differs at frame %d call %d: %d calls first, %d calls rerun, start %08X / %08X", x, i, a.n, b.n, a.start, b.start);
        RandTrace rerun = b;
        g_rt[x % RT_RING] = a;
        RbTraceDump(x);   // 第一次
        g_rt[x % RT_RING] = rerun;
        RbTraceDump(x);   // 重跑
        break;
    }
    QueryPerformanceCounter(&t1);
    double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
    if (ms > g_stWorstMs) g_stWorstMs = ms;
    Capture(b, false);
    ++g_stTests;
    // 重跑时故意不重建的绘制用资源（截屏图、UI 缩放文字的字体和宽度），不算差别
    if (uintptr_t ui = CoopStg() ? At<uintptr_t>(CoopStg() + 0x14) : 0)
        if (InPool((void*)ui) && ui + 0x11C - (uintptr_t)g_pool <= a.pool.size() && ui + 0x11C - (uintptr_t)g_pool <= b.pool.size())
            memcpy(&b.pool[ui + 0x114 - (uintptr_t)g_pool], &a.pool[ui + 0x114 - (uintptr_t)g_pool], 8);
    for (size_t i = 0; i < a.statics.size(); ++i)
        if (VA_CAPTURE_HANDLE >= STATICS[i].va && VA_CAPTURE_HANDLE + 4 <= STATICS[i].end)
            memcpy(&b.statics[i][VA_CAPTURE_HANDLE - STATICS[i].va], &a.statics[i][VA_CAPTURE_HANDLE - STATICS[i].va], 4);
    // 只报告有差别的（绘制推进的几个计数除外，见 Compare 的输出）
    size_t n = a.pool.size() < b.pool.size() ? a.pool.size() : b.pool.size();
    bool same = a.pool.size() == b.pool.size() && !memcmp(a.pool.data(), b.pool.data(), n);
    for (size_t i = 0; i < a.statics.size() && same; ++i) same = a.statics[i] == b.statics[i];
    for (size_t i = 0; i < a.coop.size() && same; ++i) same = a.coop[i] == b.coop[i];
    if (same) {
        if (g_stTests % 20 == 1) Log("synctest: frame %d ok (rollback %d frames in %.2f ms)", c, g_stK, ms);
        return;
    }
    ++g_stBad;
    Log("synctest: frame %d differs after rolling back %d frames (%.2f ms):", c, g_stK, ms);
    g_cmpShow = 1;
    Compare(a, b);
    g_cmpShow = 4;
}

// 一帧开始时（Controller::Update 之前）
void RollbackBoundary()
{
    if (g_debug) {
        static int s_frame;
        DebugFrameIo(s_frame++);
    }
    if (g_stK) SyncTestBoundary();
    if (g_rbState == RB_IDLE) return;
    if (!CoopStg() || !g_game) { g_rbState = RB_IDLE; Log("rbtest: not in a game"); return; }
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    auto ms = [&] { return (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart; };
    if (g_rbState == RB_ARMED) {
        QueryPerformanceCounter(&t0);
        Capture(g_rbStart, false);
        QueryPerformanceCounter(&t1);
        g_rbInputs.clear();
        g_rbState = RB_RECORD;
        ResetWriteWatch(g_pool, g_committed - g_pool);
        Log("rbtest: snapshot taken (pool %u KB) in %.2f ms", (unsigned)(g_rbStart.pool.size() / 1024), ms());
        return;
    }
    if ((int)g_rbInputs.size() < g_rbTarget) return;
    {
        static std::vector<void*> pages(1 << 18);
        ULONG_PTR count = pages.size();
        ULONG gran = 0;
        if (GetWriteWatch(0, g_pool, g_committed - g_pool, pages.data(), &count, &gran) == 0)
            Log("rbtest: %u of %u pool pages written in %d frames", (unsigned)count, (unsigned)((g_committed - g_pool) / 4096), g_rbTarget);
    }
    static Snapshot a, b;
    Capture(a, true);
    QueryPerformanceCounter(&t0);
    Restore(g_rbStart);
    QueryPerformanceCounter(&t1);
    Log("rbtest: restored in %.2f ms", ms());
    QueryPerformanceCounter(&t0);
    for (auto& in : g_rbInputs) SimulateFrame(in);
    QueryPerformanceCounter(&t1);
    Capture(b, true);
    Log("rbtest: resimulated %u frames in %.2f ms", (unsigned)g_rbInputs.size(), ms());
    Compare(a, b);
    g_rbState = RB_IDLE;
}

// 这一帧的输入都定好之后
void RollbackAfterInput()
{
    if (g_stK && g_stFrame >= 0) {
        FrameInput& in = g_stInputs[g_stFrame % RB_RING];
        memcpy(in.ctrl, (void*)Va(VA_CONTROLLER), CONTROLLER_SIZE);
        CoopGetInputs(in.hold);
        ++g_stFrame;
    }
    if (g_rbState != RB_RECORD) return;
    FrameInput in;
    memcpy(in.ctrl, (void*)Va(VA_CONTROLLER), CONTROLLER_SIZE);
    CoopGetInputs(in.hold);
    g_rbInputs.push_back(in);
}

void RbDebugHandles() { DebugHandlesImpl(); }
int RbDebugHandleCount() { return g_handleOrigins ? (int)g_handleOrigins->size() : -1; }

// STG_main 构造期间新建的句柄开始 / 停止记下
void RbCollectGameHandles(bool on)
{
    if (!g_gameHandles) g_gameHandles = new std::unordered_set<int>;
    if (on) g_gameHandles->clear();
    g_collectGameHandles = on;
}

// STG_main 析构之后：构造期间建的、还在的删掉
int RbFreeGameHandles()
{
    if (!g_gameHandles) return 0;
    std::vector<int> hs(g_gameHandles->begin(), g_gameHandles->end());
    g_gameHandles->clear();
    int n = 0;
    for (int h : hs) {
        if (KeepAcrossGames(h)) continue;
        if (g_handleOrigins) g_handleOrigins->erase(h);
        g_origSubHandle(h);
        ++n;
    }
    return n;
}
