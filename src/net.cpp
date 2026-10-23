// net.cpp —— 联机：UDP 连接、开局同步关卡、逐帧延迟锁步、不同步检测
// 主机 = 1P，客机按连上的先后是 2P、3P（主机在启动窗口里定这一局几个人）。星形：客机只和主机通信，
// 主机把每个客机的按键包原样转发给其它客机。
// 菜单各走各的；STG（一局）开始前在同步关卡会合、交换开局状态，之后每帧交换 1 字节的按键状态（8 个槽位），
// 所有人用同样的输入推进，模拟结果一致。
// 输入延迟：本机第 f 帧的按键用在第 f + 延迟 帧。各管各的延迟（按键包里标明是哪一帧的），
// 自动模式下按 RTT 和别人报告的等待时间调整，保证不用等，始终 60fps。
// 掉线：主机等某个客机的按键超时，就在当前帧把那个座位定为退出，写进之后的按键包，所有人在同一帧把它当出局。
// 地址：一个 IPv6 双栈套接字同时收发 IPv4（映射成 ::ffff:a.b.c.d）和 IPv6；系统不支持 IPv6 时退回纯 IPv4。
// 时间同步（同 GGPO）：按键包里带发送方当时在第几帧、帧开始后多少 ms 发出，收到的一方按单程延迟推算对方"现在"
// 在第几帧，和自己比。领先的一方每帧多睡一点（最多 4 ms），各台的帧对齐了，各人的延迟只需盖住自己的单程。
// 3 人时客机之间尽量直连：主机把各客机的地址（主机看到的）告诉另外的客机，客机互相 PING（顺带在 NAT 上打洞），
// 通了就把按键包也直接发给对方；不通时照样经主机转发。
// 回滚模式（主机在启动窗口里选）：别人的按键没到时不等，先按"和他最后一次的按键一样"预测着往下跑；真的按键到了、
// 和预测的不一样，就回到那一帧按真的重跑（rollback.cpp 存每帧的状态）。输入延迟可以很小（1～4 帧）。
// 预测超过 RB_MAX 帧时像锁步一样等。暂停 / 续关菜单、加载中等不能回滚的场面也按锁步来（等齐真的按键再跑）。
// 校验值只发"之前的按键都已确定"的帧的。
#include "mod.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <timeapi.h>
#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>

namespace {

constexpr uint32_t MAGIC = 0x4D34454C;   // "LE4M"
constexpr uint16_t PROTO = 7;
enum : uint8_t { P_HELLO = 1, P_WELCOME, P_PING, P_PONG, P_READY, P_GO, P_INPUT, P_BYE, P_FULL, P_PEERS };
constexpr int WINDOW = 64;               // 每个输入包带最近 64 帧的按键（丢包时后面的包补上；回滚模式下发送方可能领先二三十帧）
constexpr int CS_WINDOW = 32;            // 校验值带最近 32 帧
constexpr int RING = 1024;
constexpr DWORD DROP_MS = 8000;          // 主机：等某个客机的按键这么久就让它退出
constexpr DWORD GUEST_WAIT_MS = 15000;   // 客机：等按键这么久就算断线（主机的退出通知应先到）
constexpr DWORD PEER_TIMEOUT_MS = 10000;
constexpr int MAX_DELAY = 30;            // 3 人时客机之间的按键要经主机转发，RTT 大时 15 帧不够
constexpr double FRAME_MS = 1000.0 / 60;
constexpr uint8_t NO_SEAT = 0xFF;
constexpr int RB_MAX = 15;               // 回滚模式：最多预测这么多帧（再多就等）；须小于回滚的环（64 帧）减去延迟
constexpr uint32_t IN_LEFT = 0x100;      // 回滚模式记下的"这一帧用的输入"里：那个座位已退出

using Addr = sockaddr_in6;            // 所有地址都按 IPv6 存（IPv4 是映射地址 ::ffff:a.b.c.d）

#pragma pack(push, 1)
struct Hdr {
    uint32_t magic;
    uint16_t proto;
    uint8_t type;
    uint8_t seat;                // 发送方的座位（0 = 主机；客机还没分到座位时 NO_SEAT）
    uint32_t build;
};
struct PktWelcome { Hdr h; uint8_t seat, seats; };
struct PktPing { Hdr h; uint32_t t; };
struct PktReadyHost { Hdr h; uint32_t gen; uint8_t seats; };        // 主机 → 客机：第 gen 次开局的会合
struct PktReadyGuest { Hdr h; uint32_t gen; NetStartInfo info; };   // 客机 → 主机：自己的开局状态
struct PktGo { Hdr h; uint32_t gen; uint8_t seats; NetStartInfo info[MAX_SEATS]; uint8_t rollback; };
struct PktGen { Hdr h; uint32_t gen; };
struct PktInput {
    Hdr h;                       // 主机转发客机的包时原样转发，h.seat 还是原发送方
    uint32_t gen;
    uint8_t seat;                // 这些按键是哪个座位的
    int32_t last;                // masks[WINDOW-1] 是第 last 帧
    uint8_t masks[WINDOW];
    int32_t csLast;              // 发送方正在处理的帧（时间同步用）
    int32_t csTop;               // cs[CS_WINDOW-1] 是第 csTop 帧开始时的校验值（锁步 = csLast；回滚模式是已确定的最后一帧）
    uint32_t cs[CS_WINDOW];
    uint8_t late[MAX_SEATS];     // 发送方最近 1 秒里为等各座位的按键最多等了多久（ms）：那个座位据此加大延迟
    uint8_t delay;               // 发送方当前的延迟（显示用）
    uint8_t dropMask;            // 主机：已退出的座位
    int32_t dropFrame[MAX_SEATS];   // 主机：从这一帧起那个座位算退出
    uint16_t hostRtt;            // 主机：到各客机 RTT 的最大值（客机估算到另一个客机的单程用）
    uint16_t sentAge;            // 发送时离发送方第 csLast 帧开始过了多少 ms（时间同步用）
    uint8_t advMask;             // 发送方对哪些座位量到了领先量
    int8_t adv[MAX_SEATS];       // 发送方比各座位领先几帧（1/8 帧为单位）。收到的一方和自己量的取差的一半，两边共同的偏差抵消
};
struct PktPeers { Hdr h; uint8_t mask; Addr addr[MAX_SEATS]; };   // 主机 → 客机：各客机的地址（主机看到的）
#pragma pack(pop)

struct FrameRing {
    int32_t frame[RING];
    uint32_t val[RING];
    void Clear() { for (auto& f : frame) f = -1; }
    void Put(int f, uint32_t v) { if (f >= 0) { frame[f % RING] = f; val[f % RING] = v; } }
    bool Get(int f, uint32_t* v) const { if (f < 0 || frame[f % RING] != f) return false; *v = val[f % RING]; return true; }
};

// 一个对端（受 g_mx 保护）。主机这边：下标 = 客机的座位（1..）。
// 客机这边：[0] = 主机；[另一个客机的座位] = 直连那个客机（used = 知道地址，connected = 收到过它的包）
struct Peer {
    bool used = false;           // 主机：这个座位分给了这个地址；客机：知道这个对端的地址
    Addr addr {};
    bool connected = false;
    DWORD lastRecv = 0;
    int rtt = -1;
    int rttHist[6] = {};
    int rttN = 0;
    bool readyValid = false;     // 主机：客机对 readyGen 回了 READY
    uint32_t readyGen = 0;
    NetStartInfo ready {};
    int lateForMe = 0;           // 它最近报告的、为等本机座位的按键等了多久
};

int g_role = NET_SOLO;
std::string g_hostAddr;
int g_port = 17723;
int g_cfgDelay = 0;                     // 启动器设的延迟；0 = 自动
int g_cfgSeats = 2;                     // 主机：这一局几个人
bool g_cfgRollback = false;             // 主机：用回滚模式
uint32_t g_build = 0;
int g_fakeLag = 0;                      // 调试：发出去的包人为延迟（ms），测自动延迟用
int g_fakeLoss = 0;                     // 调试：发出去的包人为丢掉百分之几
int g_fakeOutage = 0;                   // 调试：每 10 秒里有这么多 ms 的包全丢（模拟网络断一下）
bool g_noDirect = false;                // 调试：客机之间不直连（只经主机转发），对比用

SOCKET g_sock = INVALID_SOCKET;
std::mutex g_mx;
Peer g_peers[MAX_SEATS];
std::atomic<int> g_mySeat { 0 };        // 客机：主机分给的座位
std::atomic<int> g_hostSeats { 2 };     // 客机：主机设的人数
std::atomic<bool> g_buildMismatch { false };
std::atomic<bool> g_full { false };     // 客机：主机已满
struct Delayed { DWORD due; Addr to; std::vector<char> data; };
bool g_v6 = true;                       // 套接字是 IPv6 双栈（否则是纯 IPv4）
std::vector<Addr> g_hostCands;          // 客机：主机地址解析出的候选（都发 HELLO，哪个先回就用哪个）
std::deque<Delayed> g_lagQueue;         // 调试用的人为延迟队列（受 g_mx 保护）

// 同步关卡
uint32_t g_hostGen = 0;                 // 主机：第几次开局
bool g_goValid = false;                 // 主机：这次开局的 GO 已经凑齐（受 g_mx 保护）
PktGo g_go {};                          // 主机：发出的 GO / 客机：收到的 GO
uint32_t g_lastGen = 0;                 // 客机：上次会合的编号
uint32_t g_hostReadyGen = 0;            // 客机：主机最近一次 READY 的编号（受 g_mx 保护）
uint32_t g_goGen = 0;                   // 客机：收到的 GO 的编号（受 g_mx 保护）

// 锁步（受 g_mx 保护的部分：环形缓冲、退出信息、别人的报告）
bool g_session = false;
bool g_broken = false;
uint32_t g_gen = 0;
int g_seats = 2;                        // 这一局的人数
int g_lsFrame = 0;
int g_curFrame = 0;                     // 正在处理的帧（受 g_mx 保护）
LONGLONG g_frameStartQpc = 0;           // 它开始的时刻（QPC）
LONGLONG g_qpcFreq = 1;
double g_adv[MAX_SEATS];                // 本机量的：本机比各座位领先几帧（平滑后）；> 0 = 本机在前
bool g_advValid[MAX_SEATS];
double g_advTheirs[MAX_SEATS];          // 各座位量的：它比本机领先几帧
bool g_advTheirsValid[MAX_SEATS];
int g_curDelay = 2;                     // 本机当前的输入延迟（帧）
int g_nextLocal = 0;                    // 下一个还没定按键的帧
uint8_t g_lastLocalMask = 0, g_pendingMask = 0;
DWORD g_lastDelayDown = 0, g_lastDelayUp = 0;
int g_waitMs[MAX_SEATS][60];            // 最近 60 帧每帧等各座位按键的时间
int g_lateSent[MAX_SEATS];
FrameRing g_in[MAX_SEATS], g_cs[MAX_SEATS];
int g_csChecked[MAX_SEATS];
int g_csTopSeen[MAX_SEATS];             // 各座位发来的校验值最新到第几帧（中间漏掉的、窗口已经移过去的就不等了）
int g_desyncFrame = -1;
int g_desyncSeat = -1;
uint8_t g_dropMask = 0;
int g_dropFrame[MAX_SEATS];
int g_hostRtt = 0;                      // 客机：主机报告的到各客机 RTT 的最大值
// 回滚模式（这一局用主机的设置）
bool g_rb = false;
FrameRing g_used[MAX_SEATS];            // 各帧模拟时各座位实际用的输入（预测的或真的，IN_LEFT = 已退出）
int g_rbChecked[MAX_SEATS];             // 各座位：这一帧及之前都已和真的输入比过
int g_front[MAX_SEATS];                 // 各座位：连续收到的按键到第几帧
FrameRing g_csMine;                     // 各帧开始时本机算的校验值（回滚重跑时会改写）
int g_csSent = -1;                      // g_csMine 里已确定、放进 g_cs[本机] 发出去的最后一帧
DWORD g_lastSend = 0;

bool g_run = false;

uint32_t Now() { return timeGetTime(); }   // 1 ms 精度（NetStart 里 timeBeginPeriod(1)）；GetTickCount 只有 16 ms
int MySeat() { return g_role == NET_HOST ? 0 : (int)g_mySeat; }
bool SameAddr(const Addr& a, const Addr& b)
{
    return a.sin6_port == b.sin6_port && !memcmp(&a.sin6_addr, &b.sin6_addr, sizeof a.sin6_addr);
}

bool IsMapped(const Addr& a)
{
    static const uint8_t prefix[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF };
    return !memcmp(&a.sin6_addr, prefix, 12);
}

// sockaddr（IPv4 / IPv6）→ Addr
Addr ToAddr(const sockaddr* sa)
{
    Addr a {};
    a.sin6_family = AF_INET6;
    if (sa->sa_family == AF_INET6) {
        a = *(const sockaddr_in6*)sa;
    } else if (sa->sa_family == AF_INET) {
        auto v4 = (const sockaddr_in*)sa;
        uint8_t* b = (uint8_t*)&a.sin6_addr;
        b[10] = b[11] = 0xFF;
        memcpy(b + 12, &v4->sin_addr, 4);
        a.sin6_port = v4->sin_port;
    }
    return a;
}

std::string AddrStr(const Addr& a)
{
    char ip[64] = "?";
    if (IsMapped(a)) inet_ntop(AF_INET, (const uint8_t*)&a.sin6_addr + 12, ip, sizeof ip);
    else inet_ntop(AF_INET6, &a.sin6_addr, ip, sizeof ip);
    char out[96];
    snprintf(out, sizeof out, IsMapped(a) ? "%s:%d" : "[%s]:%d", ip, ntohs(a.sin6_port));
    return out;
}

void SendRaw(const Addr& to, const void* p, int n)
{
    if (g_v6) {
        sendto(g_sock, (const char*)p, n, 0, (const sockaddr*)&to, sizeof to);
    } else if (IsMapped(to)) {
        sockaddr_in v4 {};
        v4.sin_family = AF_INET;
        v4.sin_port = to.sin6_port;
        memcpy(&v4.sin_addr, (const uint8_t*)&to.sin6_addr + 12, 4);
        sendto(g_sock, (const char*)p, n, 0, (const sockaddr*)&v4, sizeof v4);
    }
}

Hdr MakeHdr(uint8_t type)
{
    int seat = MySeat();
    return Hdr { MAGIC, PROTO, type, (uint8_t)(g_role == NET_GUEST && !g_peers[0].connected ? NO_SEAT : seat), g_build };
}

// 调用方持有 g_mx
void SendToLocked(const Addr& to, const void* p, int n)
{
    if (g_fakeLoss > 0 && (int)(rand() % 100) < g_fakeLoss) return;
    if (g_fakeOutage > 0 && (int)(Now() % 10000) < g_fakeOutage) return;
    if (g_fakeLag > 0) {
        g_lagQueue.push_back(Delayed { Now() + g_fakeLag, to, std::vector<char>((const char*)p, (const char*)p + n) });
        return;
    }
    SendRaw(to, p, n);
}

// 主机：发给所有连着的客机（except 座位除外）；客机：发给主机
void SendAllLocked(const void* p, int n, int except = -1)
{
    if (g_role == NET_GUEST) {
        if (g_peers[0].used) SendToLocked(g_peers[0].addr, p, n);
        return;
    }
    for (int s = 1; s < MAX_SEATS; ++s)
        if (s != except && g_peers[s].used && g_peers[s].connected) SendToLocked(g_peers[s].addr, p, n);
}

void SendAll(const void* p, int n)
{
    std::lock_guard<std::mutex> lk(g_mx);
    SendAllLocked(p, n);
}

template <class T> void SendPkt(const T& p) { SendAll(&p, sizeof p); }

void FlushLagQueue()
{
    std::lock_guard<std::mutex> lk(g_mx);
    DWORD now = Now();
    while (!g_lagQueue.empty() && (int)(now - g_lagQueue.front().due) >= 0) {
        auto& d = g_lagQueue.front();
        SendRaw(d.to, d.data.data(), (int)d.data.size());
        g_lagQueue.pop_front();
    }
}

void AddRtt(Peer& p, int rtt)
{
    p.rtt = rtt;
    p.rttHist[p.rttN++ % 6] = rtt;
}

bool SeatDropped(int seat, int frame) { return (g_dropMask >> seat & 1) && frame >= g_dropFrame[seat]; }

double QpcMs(LONGLONG ticks) { return ticks * 1000.0 / g_qpcFreq; }

LONGLONG QpcNow()
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return q.QuadPart;
}

int BestRtt(const Peer& p);

// 输入包：存进对应座位的缓冲；主机转发给其它客机（调用方持有 g_mx）。
// path：包是那个座位的电脑直接发来的（主机收客机的、客机收主机自己的、直连）时是那条路径，经主机转发来的为空
void TakeInput(const PktInput* p, int n, const Peer* path)
{
    if (!g_session || p->gen != g_gen || p->seat >= MAX_SEATS || p->seat == MySeat()) return;
    // 时间同步：对方发这个包时在第 csLast 帧 + sentAge ms，到这里过了单程（最好的那次 RTT 的一半）
    if (path && !(g_dropMask >> p->seat & 1)) {
        int best = BestRtt(*path);
        if (best >= 0) {
            double mine = g_curFrame + QpcMs(QpcNow() - g_frameStartQpc) / FRAME_MS;
            double theirs = p->csLast + (p->sentAge + best / 2.0) / FRAME_MS;
            double adv = mine - theirs;
            double& a = g_adv[p->seat];
            a = g_advValid[p->seat] ? a * 0.95 + adv * 0.05 : adv;
            g_advValid[p->seat] = true;
        }
    }
    if (p->advMask >> MySeat() & 1) {
        g_advTheirs[p->seat] = p->adv[MySeat()] / 8.0;
        g_advTheirsValid[p->seat] = true;
    }
    if (g_role == NET_HOST) {
        if (g_dropMask >> p->seat & 1) return;   // 已经定为退出的座位：之后的包不再收、不再转发
        SendAllLocked(p, n, p->seat);
    } else if (p->seat == 0) {
        // 主机的包带着退出信息
        for (int s = 0; s < MAX_SEATS; ++s)
            if ((p->dropMask >> s & 1) && !(g_dropMask >> s & 1)) {
                g_dropMask |= (uint8_t)(1 << s);
                g_dropFrame[s] = p->dropFrame[s];
                Log("net: host says %dP left at frame %d", s + 1, p->dropFrame[s]);
            }
        g_hostRtt = p->hostRtt;
    }
    for (int i = 0; i < WINDOW; ++i) {
        g_in[p->seat].Put(p->last - (WINDOW - 1) + i, p->masks[i]);
        if (i < CS_WINDOW) g_cs[p->seat].Put(p->csTop - (CS_WINDOW - 1) + i, p->cs[i]);
    if (p->csTop > g_csTopSeen[p->seat]) g_csTopSeen[p->seat] = p->csTop;
    }
    // 发送方报告的"为等本机座位的按键等了多久"
    int me = MySeat();
    int from = p->h.seat;
    if (g_role == NET_HOST) {
        if (from > 0 && from < MAX_SEATS) g_peers[from].lateForMe = p->late[me];
    } else if (from < MAX_SEATS) {
        // 客机：主机自己的报告记在 [0]，另一个客机（经主机转发）的记在它的座位
        g_peers[from == 0 ? 0 : from].lateForMe = p->late[me];
    }
}

void HandlePacket(const char* buf, int n, const Addr& from)
{
    if (n < (int)sizeof(Hdr)) return;
    auto h = (const Hdr*)buf;
    if (h->magic != MAGIC || h->proto != PROTO) return;
    if (h->build != g_build) { g_buildMismatch = true; return; }
    std::lock_guard<std::mutex> lk(g_mx);
    DWORD now = Now();
    if (g_role == NET_HOST) {
        if (h->seat == 0) return;   // 别的主机
        int seat = -1;
        for (int s = 1; s < MAX_SEATS; ++s)
            if (g_peers[s].used && SameAddr(g_peers[s].addr, from)) seat = s;
        if (h->type == P_HELLO) {
            if (seat < 0) {   // 新来的客机：分一个空座位（没人用，或原来的人已经断开）
                for (int s = 1; s < g_cfgSeats && seat < 0; ++s)
                    if (!g_peers[s].used || !g_peers[s].connected) seat = s;
                if (seat < 0 || (g_session && seat < g_seats)) {
                    Hdr full = MakeHdr(P_FULL);
                    SendToLocked(from, &full, sizeof full);
                    return;
                }
                g_peers[seat] = Peer {};
                g_peers[seat].used = true;
                g_peers[seat].addr = from;
                Log("net: guest %s is %dP", AddrStr(from).c_str(), seat + 1);
            }
            g_peers[seat].connected = true;
            g_peers[seat].lastRecv = now;
            PktWelcome w { MakeHdr(P_WELCOME), (uint8_t)seat, (uint8_t)g_cfgSeats };
            SendToLocked(from, &w, sizeof w);
            return;
        }
        if (seat < 0) return;
        Peer& p = g_peers[seat];
        p.lastRecv = now;
        p.connected = true;
        switch (h->type) {
        case P_PING:
            if (n >= (int)sizeof(PktPing)) {
                PktPing r { MakeHdr(P_PONG), ((const PktPing*)buf)->t };
                SendToLocked(from, &r, sizeof r);
            }
            break;
        case P_PONG:
            if (n >= (int)sizeof(PktPing)) AddRtt(p, (int)(now - ((const PktPing*)buf)->t));
            break;
        case P_READY:
            if (n >= (int)sizeof(PktReadyGuest)) {
                auto r = (const PktReadyGuest*)buf;
                p.ready = r->info;
                p.readyGen = r->gen;
                p.readyValid = true;
                // 这次开局已经凑齐：客机没收到 GO 会一直重发 READY，每次都回
                if (g_goValid && r->gen == g_go.gen) SendToLocked(from, &g_go, sizeof g_go);
            }
            break;
        case P_BYE:
            if (n >= (int)sizeof(PktGen) && p.readyGen == ((const PktGen*)buf)->gen) p.readyValid = false;
            break;
        case P_INPUT:
            if (n >= (int)sizeof(PktInput) && ((const PktInput*)buf)->seat == seat) TakeInput((const PktInput*)buf, n, &p);
            break;
        }
        return;
    }
    // 客机：主机，或者主机告诉过地址的另一个客机（直连）
    Peer& host = g_peers[0];
    bool fromHost = host.used && SameAddr(host.addr, from);
    if (!fromHost && !host.connected && (h->type == P_WELCOME || h->type == P_FULL))
        for (auto& c : g_hostCands)   // 还没连上：主机地址解析出的哪个候选先回了，就用哪个
            if (SameAddr(c, from)) {
                host.addr = from;
                host.used = fromHost = true;
            }
    if (!fromHost) {
        int peer = -1;
        for (int s = 1; s < MAX_SEATS; ++s)
            if (s != MySeat() && g_peers[s].used && SameAddr(g_peers[s].addr, from)) peer = s;
        if (peer < 0 || h->seat != peer) return;
        Peer& d = g_peers[peer];
        d.lastRecv = now;
        if (!d.connected) Log("net: direct link to %dP (%s)", peer + 1, AddrStr(from).c_str());
        d.connected = true;
        if (h->type == P_PING && n >= (int)sizeof(PktPing)) {
            PktPing r { MakeHdr(P_PONG), ((const PktPing*)buf)->t };
            SendToLocked(from, &r, sizeof r);
        } else if (h->type == P_PONG && n >= (int)sizeof(PktPing)) {
            AddRtt(d, (int)(now - ((const PktPing*)buf)->t));
        } else if (h->type == P_INPUT && n >= (int)sizeof(PktInput)) {
            auto in = (const PktInput*)buf;
            TakeInput(in, n, in->seat == peer ? &d : nullptr);
        }
        return;
    }
    if (h->type != P_INPUT && h->seat != 0) return;   // 按键包可能是主机转发的另一个客机的
    host.lastRecv = now;
    switch (h->type) {
    case P_PEERS:
        if (n >= (int)sizeof(PktPeers)) {
            auto pp = (const PktPeers*)buf;
            for (int s = 1; s < MAX_SEATS; ++s) {
                if (s == MySeat()) continue;
                Peer& d = g_peers[s];
                if (!(pp->mask >> s & 1)) { d.used = d.connected = false; continue; }
                if (!d.used || !SameAddr(d.addr, pp->addr[s])) {
                    d = Peer {};
                    d.used = true;
                    d.addr = pp->addr[s];
                    Log("net: %dP is at %s, trying a direct link", s + 1, AddrStr(d.addr).c_str());
                }
            }
        }
        break;
    case P_WELCOME:
        if (n >= (int)sizeof(PktWelcome)) {
            auto w = (const PktWelcome*)buf;
            if (!host.connected || g_mySeat != w->seat) Log("net: connected to %s as %dP (%d players)", AddrStr(from).c_str(), w->seat + 1, w->seats);
            g_mySeat = w->seat;
            g_hostSeats = w->seats;
            host.connected = true;
            g_full = false;
        }
        break;
    case P_FULL:
        g_full = true;
        break;
    case P_PING:
        if (n >= (int)sizeof(PktPing)) {
            PktPing r { MakeHdr(P_PONG), ((const PktPing*)buf)->t };
            SendToLocked(from, &r, sizeof r);
        }
        break;
    case P_PONG:
        if (n >= (int)sizeof(PktPing)) AddRtt(host, (int)(now - ((const PktPing*)buf)->t));
        break;
    case P_READY:
        if (n >= (int)sizeof(PktReadyHost)) {
            auto r = (const PktReadyHost*)buf;
            g_hostReadyGen = r->gen;
            g_hostSeats = r->seats;
        }
        break;
    case P_GO:
        if (n >= (int)sizeof(PktGo)) {
            memcpy(&g_go, buf, sizeof g_go);
            g_goGen = g_go.gen;
        }
        break;
    case P_BYE:
        if (n >= (int)sizeof(PktGen) && g_hostReadyGen == ((const PktGen*)buf)->gen) g_hostReadyGen = g_lastGen;
        break;
    case P_INPUT:
        if (n >= (int)sizeof(PktInput)) {
            auto in = (const PktInput*)buf;
            TakeInput(in, n, in->seat == 0 ? &host : nullptr);
        }
        break;
    }
}

DWORD WINAPI NetThread(LPVOID)
{
    static char buf[8192];
    DWORD lastHello = 0, lastPing = 0, lastPeers = 0;
    while (g_run) {
        sockaddr_storage from {};
        int fl = sizeof from;
        int n = recvfrom(g_sock, buf, sizeof buf, 0, (sockaddr*)&from, &fl);
        if (n > 0) HandlePacket(buf, n, ToAddr((const sockaddr*)&from));
        if (g_fakeLag > 0) FlushLagQueue();
        DWORD now = Now();
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_role == NET_GUEST && !g_peers[0].connected && now - lastHello > 300) {
            Hdr h = MakeHdr(P_HELLO);
            for (auto& c : g_hostCands) SendToLocked(c, &h, sizeof h);
            lastHello = now;
        }
        if (now - lastPing > 500) {
            PktPing p { MakeHdr(P_PING), now };
            SendAllLocked(&p, sizeof p);
            if (g_role == NET_GUEST && g_peers[0].connected && !g_noDirect)   // 直连另一个客机：一直 PING（也是在 NAT 上打洞）
                for (int s = 1; s < MAX_SEATS; ++s)
                    if (s != MySeat() && g_peers[s].used) SendToLocked(g_peers[s].addr, &p, sizeof p);
            lastPing = now;
        }
        if (g_role == NET_HOST && now - lastPeers > 1000) {   // 告诉各客机其他客机的地址
            PktPeers pp {};
            pp.h = MakeHdr(P_PEERS);
            int n2 = 0;
            for (int s = 1; s < MAX_SEATS; ++s)
                if (g_peers[s].used && g_peers[s].connected) {
                    pp.mask |= (uint8_t)(1 << s);
                    pp.addr[s] = g_peers[s].addr;
                    ++n2;
                }
            if (n2 >= 2) SendAllLocked(&pp, sizeof pp);
            lastPeers = now;
        }
        for (int s = 0; s < MAX_SEATS; ++s) {
            Peer& p = g_peers[s];
            if (p.connected && now - p.lastRecv > PEER_TIMEOUT_MS) {
                p.connected = false;
                p.rtt = -1;
                if (g_role == NET_HOST) Log("net: %dP timed out", s + 1);
                else Log(s == 0 ? "net: host timed out" : "net: direct link to %dP timed out", s + 1);
            }
        }
    }
    return 0;
}

// 阻塞等待时保持窗口有响应
void PumpMessages()
{
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

bool EscPressed()
{
    return g_hwnd && GetForegroundWindow() == g_hwnd && (GetAsyncKeyState(VK_ESCAPE) & 0x8000);
}

void SetTitle(const wchar_t* text)
{
    static wchar_t orig[256];
    if (!g_hwnd) return;
    if (!orig[0]) GetWindowTextW(g_hwnd, orig, 256);
    SetWindowTextW(g_hwnd, text ? text : orig);
}

int WorstRtt(const Peer& p)
{
    int w = -1;
    for (int i = 0; i < std::min(p.rttN, 6); ++i) w = std::max(w, p.rttHist[i]);
    return w;
}

int BestRtt(const Peer& p)
{
    int b = -1;
    for (int i = 0; i < std::min(p.rttN, 6); ++i) b = b < 0 ? p.rttHist[i] : std::min(b, p.rttHist[i]);
    return b;
}

// 按 RTT 算的延迟：本机的按键要在最远的那个人模拟到那一帧之前到达。
// 每条路径：单程（最近几次 RTT 最大值的一半）+ 抖动余量（最近几次 RTT 的最大减最小，至少 3 ms）。
// 主机：到各客机；客机：到主机，3 人时还有到另一个客机（直连通了用直连，否则是经主机转发：到主机 + 主机到它）。
// 算出来的是下限，实际不够时别人会报告等待，本机再加
int RttDelay()
{
    int need = -1;   // ms
    auto path = [&](const Peer& p, int extraOneWay) {
        int w = WorstRtt(p), b = BestRtt(p);
        if (w < 0) return;
        need = std::max(need, w / 2 + extraOneWay + std::max(3, w - b));
    };
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_role == NET_HOST) {
            for (int s = 1; s < MAX_SEATS; ++s)
                if (g_peers[s].connected) path(g_peers[s], 0);
        } else {
            path(g_peers[0], 0);
            for (int s = 1; s < g_seats && s < MAX_SEATS; ++s) {
                if (s == MySeat()) continue;
                if (g_peers[s].connected && g_peers[s].rttN > 0) path(g_peers[s], 0);
                else path(g_peers[0], g_hostRtt / 2);
            }
        }
    }
    if (need < 0) return 2;
    return std::clamp((int)std::ceil(need / FRAME_MS), 1, MAX_DELAY);
}

// 回滚模式的自动延迟：单程的一半左右（剩下的靠回滚），1～6 帧
int RbAutoDelay() { return std::clamp((RttDelay() + 1) / 2, 1, 6); }

void UpdateDelay()
{
    if (g_cfgDelay > 0) { g_curDelay = g_cfgDelay; return; }
    DWORD now = Now();
    int target = g_rb ? RbAutoDelay() : RttDelay();
    int late = 0;
    if (!g_rb) {   // 回滚模式下别人的等待不是延迟不够造成的（是预测超过了 RB_MAX）
        std::lock_guard<std::mutex> lk(g_mx);
        for (auto& p : g_peers) late = std::max(late, p.lateForMe);
    }
    // 别人报告最近 1 秒为等本机的按键停过：本机的延迟不够，加一帧（等报告刷新，每 1.1 秒最多加一次）
    if (late > 4 && now - g_lastDelayUp > 1100) target = std::max(target, g_curDelay + 1);
    target = std::min(target, MAX_DELAY);
    if (target > g_curDelay) {
        g_curDelay = target;
        g_lastDelayUp = g_lastDelayDown = now;
        Log("net: delay up to %d (others waited %d ms)", g_curDelay, late);
    } else if (target < g_curDelay && late == 0 && now - g_lastDelayDown > 3000 && now - g_lastDelayUp > 10000) {
        --g_curDelay;   // 网络变好：慢慢减（刚加过的 10 秒内不减，免得来回跳）
        g_lastDelayDown = now;
        Log("net: delay down to %d", g_curDelay);
    }
}

void SendInputs();

// 连续收到的按键到第几帧（调用方持有 g_mx）
int Frontier(int s)
{
    uint32_t v;
    while (g_in[s].Get(g_front[s] + 1, &v)) ++g_front[s];
    return g_front[s];
}

// 等到所有人第 need 帧的按键都到了（f = 本机正在处理的帧，用于报告等待和掉线）。客机和主机断开时返回 false
bool WaitInputs(int need, int f)
{
    int me = MySeat();
    LARGE_INTEGER q0, qf;
    QueryPerformanceCounter(&q0);
    QueryPerformanceFrequency(&qf);
    int waited[MAX_SEATS] = {};
    DWORD start = Now();
    bool stallLogged = false;
    while (need >= 0) {
        bool all = true;
        {
            std::lock_guard<std::mutex> lk(g_mx);
            LARGE_INTEGER q;
            QueryPerformanceCounter(&q);
            int ms = (int)((q.QuadPart - q0.QuadPart) * 1000 / qf.QuadPart);
            for (int s = 0; s < g_seats; ++s) {
                uint32_t v;
                if (s == me || SeatDropped(s, need) || waited[s] < 0) continue;
                if (g_in[s].Get(need, &v)) { waited[s] = -ms - 1; continue; }   // 到了：记下等了多久（负数表示已到）
                all = false;
            }
            DWORD now = Now();
            if (!all && g_role == NET_HOST && now - start > DROP_MS) {
                for (int s = 1; s < g_seats; ++s) {
                    uint32_t v;
                    if (SeatDropped(s, need) || waited[s] < 0 || g_in[s].Get(need, &v)) continue;
                    g_dropMask |= (uint8_t)(1 << s);
                    // 锁步：从当前帧起；回滚：从它最后一个按键之后（之间的帧是预测的，所有人都按"已退出"重跑）
                    g_dropFrame[s] = g_rb ? Frontier(s) + 1 : need;
                    Log("net: %dP sent nothing for frame %d, dropped from frame %d", s + 1, need, g_dropFrame[s]);
                }
                all = true;
            }
            if (!all && g_role == NET_GUEST && (now - start > GUEST_WAIT_MS || !g_peers[0].connected)) {
                g_broken = true;
                Log("net: no input for frame %d, connection lost", need);
                return false;
            }
        }
        if (all) break;
        DWORD now = Now();
        if (!stallLogged && now - start > 2000) {   // 卡住了：记下在等谁（崩溃日志里能看到）
            stallLogged = true;
            std::lock_guard<std::mutex> lk(g_mx);
            for (int s = 0; s < g_seats; ++s)
                if (s != me && !SeatDropped(s, need) && waited[s] >= 0)
                    Log("net: frame %d waiting for %dP's input for frame %d (received up to %d, rtt %d)", f, s + 1, need, Frontier(s),
                        g_role == NET_HOST ? g_peers[s].rtt : g_peers[0].rtt);
        }
        if (now - g_lastSend > 30) SendInputs();
        if (now - start > 100) PumpMessages();
        if (g_fakeLag > 0) FlushLagQueue();
        Sleep(now - start > 2 ? 1 : 0);
    }
    // 记下这一帧为等各座位的按键等了多久，报告出去（那个座位据此加大它的延迟）。
    // 只算最后到的那个座位：先到的那个即使也晚了一点，这一帧反正要等最后那个，不该让它也加延迟
    // （不然 3 人时会有人的延迟被多加）。开局最初 2 秒不算：主机比客机早开始一点，开头那一下等待是正常的
    int last = 0;
    for (int s = 0; s < MAX_SEATS; ++s)
        if (waited[s] < 0) last = std::max(last, -waited[s] - 1);
    for (int s = 0; s < MAX_SEATS; ++s) {
        int w = waited[s] < 0 ? -waited[s] - 1 : 0;
        g_waitMs[s][f % 60] = f < 120 || w < last ? 0 : w;
        g_lateSent[s] = *std::max_element(g_waitMs[s], g_waitMs[s] + 60);
    }
    return true;
}

// 回滚模式：座位 s 第 x 帧的输入。到了（或已退出）时 *actual = true；没到时预测 = 它最后收到的那帧的（调用方持有 g_mx）
uint32_t InputFor(int s, int x, bool* actual)
{
    uint32_t v = 0;
    if (s >= g_seats || SeatDropped(s, x)) { *actual = true; return IN_LEFT; }
    if (g_in[s].Get(x, &v)) { *actual = true; return v; }
    *actual = false;
    int fr = Frontier(s);
    if (fr >= 0 && fr < x && g_in[s].Get(fr, &v)) return v;
    for (int y = x - 1; y >= 0 && y > x - RING / 2; --y)
        if (g_in[s].Get(y, &v)) return v;
    return 0;
}

// 本机这一帧的按键定到第 f + 延迟 帧（调用方不持有 g_mx）
void PutLocal(int f, uint8_t localMask, uint32_t checksum, bool putChecksum)
{
    int me = MySeat();
    std::lock_guard<std::mutex> lk(g_mx);
    int t = f + g_curDelay;
    if (t >= g_nextLocal) {
        for (int x = g_nextLocal; x < t; ++x) g_in[me].Put(x, g_lastLocalMask);
        uint8_t m = localMask | g_pendingMask;
        g_in[me].Put(t, m);
        g_lastLocalMask = m;
        g_pendingMask = 0;
        g_nextLocal = t + 1;
    } else {
        g_pendingMask |= localMask;
    }
    if (putChecksum) g_cs[me].Put(f, checksum);
}

// 校验值：别人算出的第 x 帧开始时的状态，和本机的比（调用方持有 g_mx）
void CompareChecksums(int f)
{
    int me = MySeat();
    for (int s = 0; s < g_seats; ++s) {
        if (s == me) continue;
        for (int x = g_csChecked[s] + 1; x <= f; ++x) {
            uint32_t a, b;
            if (SeatDropped(s, x)) { g_csChecked[s] = x; continue; }
            if (!g_cs[me].Get(x, &a)) break;
            if (!g_cs[s].Get(x, &b)) {
                // 回滚模式下网络断了一下之后，对方"已确定"的帧会一下跳过好几十帧，中间的校验值不会再发：跳过
                if (x <= g_csTopSeen[s] - CS_WINDOW) { g_csChecked[s] = x; continue; }
                break;
            }
            if (a != b && g_desyncFrame < 0) {
                g_desyncFrame = x;
                g_desyncSeat = s;
                Log("net: DESYNC with %dP at frame %d (%08X vs %08X), now at frame %d, delay %d%s", s + 1, x, a, b, g_lsFrame, g_curDelay,
                    g_rb ? ", rollback" : "");
            }
            g_csChecked[s] = x;
        }
    }
}

void SendInputs()
{
    PktInput p {};
    p.h = MakeHdr(P_INPUT);
    p.gen = g_gen;
    int me = MySeat();
    p.seat = (uint8_t)me;
    std::lock_guard<std::mutex> lk(g_mx);
    p.last = g_nextLocal - 1;
    for (int i = 0; i < WINDOW; ++i) {
        uint32_t v = 0;
        g_in[me].Get(p.last - (WINDOW - 1) + i, &v);
        p.masks[i] = (uint8_t)v;
    }
    p.csLast = g_lsFrame;
    p.csTop = g_rb ? g_csSent : g_lsFrame;
    p.sentAge = (uint16_t)std::clamp((int)QpcMs(QpcNow() - g_frameStartQpc), 0, 65535);
    for (int s = 0; s < MAX_SEATS; ++s)
        if (g_advValid[s]) {
            p.advMask |= (uint8_t)(1 << s);
            p.adv[s] = (int8_t)std::clamp((int)std::lround(g_adv[s] * 8), -127, 127);
        }
    for (int i = 0; i < CS_WINDOW; ++i) {
        uint32_t v = 0;
        g_cs[me].Get(p.csTop - (CS_WINDOW - 1) + i, &v);
        p.cs[i] = v;
    }
    for (int s = 0; s < MAX_SEATS; ++s) p.late[s] = (uint8_t)std::min(g_lateSent[s], 255);
    p.delay = (uint8_t)g_curDelay;
    if (g_role == NET_HOST) {
        p.dropMask = g_dropMask;
        for (int s = 0; s < MAX_SEATS; ++s) p.dropFrame[s] = g_dropFrame[s];
        int worst = 0;
        for (int s = 1; s < MAX_SEATS; ++s)
            if (g_peers[s].connected) worst = std::max(worst, WorstRtt(g_peers[s]));
        p.hostRtt = (uint16_t)worst;
    }
    SendAllLocked(&p, sizeof p);
    if (g_role == NET_GUEST)   // 直连通了的另一个客机也直接发一份（主机那边照样会转发，重复的收到无妨）
        for (int s = 1; s < MAX_SEATS; ++s)
            if (s != me && g_peers[s].connected) SendToLocked(g_peers[s].addr, &p, sizeof p);
    g_lastSend = Now();
}

} // namespace

// ------------------------------------------------------------------ 对外
int NetRole() { return g_role; }
int NetSeat() { return MySeat(); }

bool NetConnected()
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_role == NET_GUEST) return g_peers[0].connected;
    for (int s = 1; s < g_cfgSeats; ++s)
        if (!g_peers[s].connected) return false;
    return true;
}

bool NetInGame() { return g_session && !g_broken; }

void NetConfigure(int role, const std::string& host, int port, int delay, int seats, bool rollback)
{
    g_cfgRollback = rollback;
    g_role = role;
    g_hostAddr = host;
    g_port = port > 0 && port < 65536 ? port : 17723;
    g_cfgDelay = std::clamp(delay, 0, MAX_DELAY);
    g_cfgSeats = std::clamp(seats, 2, MAX_SEATS);
}

bool NetStart()
{
    if (g_role != NET_HOST && g_role != NET_GUEST) return true;
    // 所有人必须是同一个 le04multi.dll：用它的链接时间戳当版本号
    auto dos = (IMAGE_DOS_HEADER*)g_self;
    g_build = ((IMAGE_NT_HEADERS*)((uint8_t*)g_self + dos->e_lfanew))->FileHeader.TimeDateStamp;
    timeBeginPeriod(1);   // 等别人按键时 Sleep(1) 要真的只睡 1ms
    {
        LARGE_INTEGER fq;
        QueryPerformanceFrequency(&fq);
        g_qpcFreq = fq.QuadPart;
    }
    if (g_debug)
        if (const char* lag = getenv("LE04MULTI_FAKELAG")) g_fakeLag = atoi(lag);
    if (g_debug)
        if (const char* loss = getenv("LE04MULTI_FAKELOSS")) g_fakeLoss = atoi(loss);
    if (g_debug)
        if (const char* o = getenv("LE04MULTI_FAKEOUTAGE")) g_fakeOutage = atoi(o);
    if (g_debug && getenv("LE04MULTI_NODIRECT")) g_noDirect = true;
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd)) return false;
    // IPv6 双栈（同时收发 IPv4）；系统没有 IPv6 时退回纯 IPv4
    g_sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock != INVALID_SOCKET) {
        DWORD off = 0;
        if (setsockopt(g_sock, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&off, sizeof off)) {
            closesocket(g_sock);
            g_sock = INVALID_SOCKET;
        }
    }
    if (g_sock == INVALID_SOCKET) {
        g_v6 = false;
        g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    }
    if (g_sock == INVALID_SOCKET) return false;
    DWORD tmo = g_fakeLag > 0 ? 1 : 20;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof tmo);
    int bufSize = 1 << 20;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, (const char*)&bufSize, sizeof bufSize);
    u_short port = htons(g_role == NET_HOST ? (u_short)g_port : 0);
    int bound;
    if (g_v6) {
        sockaddr_in6 local {};
        local.sin6_family = AF_INET6;
        local.sin6_addr = in6addr_any;
        local.sin6_port = port;
        bound = bind(g_sock, (sockaddr*)&local, sizeof local);
    } else {
        sockaddr_in local {};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = INADDR_ANY;
        local.sin_port = port;
        bound = bind(g_sock, (sockaddr*)&local, sizeof local);
    }
    if (bound) {
        Log("net: bind failed %d", WSAGetLastError());
        return false;
    }
    if (g_role == NET_GUEST) {
        // 主机地址：IPv4、IPv6（可以带方括号）或域名。解析出的每个地址都发 HELLO，先回的那个就是主机
        std::string host = g_hostAddr;
        if (!host.empty() && host.front() == '[') {
            size_t close = host.find(']');
            host = host.substr(1, close == std::string::npos ? std::string::npos : close - 1);
        }
        addrinfo hints {}, *res = nullptr;
        hints.ai_family = g_v6 ? AF_UNSPEC : AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        char portStr[16];
        sprintf_s(portStr, "%d", g_port);
        if (getaddrinfo(host.c_str(), portStr, &hints, &res) || !res) {
            Log("net: cannot resolve %s", g_hostAddr.c_str());
            return false;
        }
        for (addrinfo* r = res; r; r = r->ai_next) {
            Addr a = ToAddr(r->ai_addr);
            if (!g_v6 && !IsMapped(a)) continue;
            g_hostCands.push_back(a);
            Log("net: host candidate %s", AddrStr(a).c_str());
        }
        freeaddrinfo(res);
        if (g_hostCands.empty()) return false;
        g_peers[0].addr = g_hostCands[0];
    }
    g_run = true;
    if (HANDLE t = CreateThread(nullptr, 0, NetThread, nullptr, 0, nullptr)) CloseHandle(t);
    Log("net: %s port %d delay %d seats %d build %08X fakelag %d %s", g_role == NET_HOST ? "host" : "guest", g_port, g_cfgDelay,
        g_cfgSeats, g_build, g_fakeLag, g_v6 ? "dual-stack" : "IPv4 only");
    return true;
}

// 同步关卡：STG 构造之前调用。成功时 infos[座位] 是各人的开局状态、*seats 是人数；取消（Esc）返回 false
bool NetBarrier(const NetStartInfo& mine, NetStartInfo infos[MAX_SEATS], int* seats)
{
    if (g_role != NET_HOST && g_role != NET_GUEST) return false;
    g_session = false;
    bool ok = false;
    uint32_t gen = 0;
    bool have = false;
    DWORD lastReady = 0;
    if (g_role == NET_HOST) {
        gen = ++g_hostGen;
        std::lock_guard<std::mutex> lk(g_mx);
        g_goValid = false;
        for (auto& p : g_peers) p.readyValid = false;
    }
    char title[256];
    auto fmt = [&](int id, int a = 0, int b = 0) { snprintf(title, sizeof title, Tr(id), a, b); };
    while (true) {
        PumpMessages();
        if (EscPressed()) {
            PktGen bye { MakeHdr(P_BYE), gen };
            SendPkt(bye);
            break;
        }
        DWORD now = Now();
        if (g_role == NET_HOST) {
            std::lock_guard<std::mutex> lk(g_mx);
            int need = g_cfgSeats - 1, conn = 0, ready = 0;
            for (int s = 1; s < g_cfgSeats; ++s) {
                conn += g_peers[s].connected;
                ready += g_peers[s].connected && g_peers[s].readyValid && g_peers[s].readyGen == gen;
            }
            if (conn < need) {
                fmt(TX_W_WAIT_GUESTS, conn, need);
            } else {
                fmt(TX_W_WAIT_READY, ready, need);
                if (now - lastReady > 100) {
                    PktReadyHost r { MakeHdr(P_READY), gen, (uint8_t)g_cfgSeats };
                    SendAllLocked(&r, sizeof r);
                    lastReady = now;
                }
                if (ready == need) {
                    g_go = PktGo {};
                    g_go.h = MakeHdr(P_GO);
                    g_go.gen = gen;
                    g_go.seats = (uint8_t)g_cfgSeats;
                    g_go.rollback = g_cfgRollback;
                    g_go.info[0] = mine;
                    for (int s = 1; s < g_cfgSeats; ++s) g_go.info[s] = g_peers[s].ready;
                    g_goValid = true;
                    SendAllLocked(&g_go, sizeof g_go);
                    ok = true;
                }
            }
        } else {
            std::lock_guard<std::mutex> lk(g_mx);
            if (g_full) {
                fmt(TX_W_FULL);
            } else if (!g_peers[0].connected) {
                fmt(TX_W_CONNECTING);
            } else {
                if (g_hostReadyGen > g_lastGen) { gen = g_hostReadyGen; have = true; }
                if (have && g_goGen == gen) {
                    ok = true;
                } else {
                    fmt(have ? TX_W_STARTING : TX_W_WAIT_HOST);
                    if (have && now - lastReady > 100) {
                        PktReadyGuest r { MakeHdr(P_READY), gen, mine };
                        SendAllLocked(&r, sizeof r);
                        lastReady = now;
                    }
                }
            }
        }
        if (ok) break;
        SetTitle(Widen(title).c_str());
        Sleep(2);
    }
    SetTitle(nullptr);
    if (!ok) { Log("net: barrier canceled"); return false; }
    int startDelay = g_cfgDelay > 0 ? g_cfgDelay : g_go.rollback ? RbAutoDelay() : RttDelay();
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_role == NET_GUEST) g_lastGen = gen;
    g_seats = std::clamp((int)g_go.seats, 2, MAX_SEATS);
    *seats = g_seats;
    for (int s = 0; s < MAX_SEATS; ++s) infos[s] = g_go.info[s];
    g_gen = gen;
    g_rb = g_go.rollback != 0;
    g_csMine.Clear();
    g_csSent = -1;
    g_lsFrame = g_curFrame = 0;
    g_frameStartQpc = QpcNow();
    for (int s = 0; s < MAX_SEATS; ++s) {
        g_adv[s] = g_advTheirs[s] = 0;
        g_advValid[s] = g_advTheirsValid[s] = false;
        g_in[s].Clear();
        g_cs[s].Clear();
        g_used[s].Clear();
        g_rbChecked[s] = g_front[s] = -1;
        g_csChecked[s] = g_csTopSeen[s] = -1;
        g_dropFrame[s] = INT_MAX;
        g_lateSent[s] = 0;
        g_peers[s].lateForMe = 0;
    }
    memset(g_waitMs, 0, sizeof g_waitMs);
    g_dropMask = 0;
    g_desyncFrame = g_desyncSeat = -1;
    g_broken = false;
    g_session = true;
    // 开头几帧（还在延迟里的）按键为空；之后的延迟每帧按网络情况调整
    g_curDelay = startDelay;
    for (int f = 0; f < g_curDelay; ++f) g_in[MySeat()].Put(f, 0);
    g_nextLocal = g_curDelay;
    g_lastLocalMask = g_pendingMask = 0;
    g_lastDelayDown = g_lastDelayUp = Now();
    Log("net: game %u started as %dP of %d (delay %d%s%s)", gen, MySeat() + 1, g_seats, g_curDelay, g_cfgDelay > 0 ? "" : " auto",
        g_rb ? ", rollback" : "");
    return true;
}

void NetEndGame()
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_session) Log("net: game %u ended at frame %d%s", g_gen, g_lsFrame, g_desyncFrame >= 0 ? " (desynced)" : "");
    g_session = false;
}

// 一帧：localMask 是本机这一帧的按键；masks[座位] 返回第 g_lsFrame 帧各人实际使用的按键，*left 返回已退出的座位。
// 客机和主机断开时返回 false
bool NetFrame(uint8_t localMask, uint32_t checksum, uint8_t masks[MAX_SEATS], uint8_t* left)
{
    if (!g_session || g_broken) return false;
    int f = g_lsFrame;
    int me = MySeat();
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_curFrame = f;
        g_frameStartQpc = QpcNow();
        if (g_debug && f % 300 == 0 && f > 0)
            Log("net: frame %d delay %d advantage mine %.2f / %.2f / %.2f theirs %.2f / %.2f / %.2f", f, g_curDelay, g_adv[0], g_adv[1],
                g_adv[2], g_advTheirs[0], g_advTheirs[1], g_advTheirs[2]);
    }
    UpdateDelay();
    // 本机这一帧的按键定到第 f + 延迟 帧。延迟变大时中间空出的帧沿用上一帧的按键；
    // 变小时这一帧的按键并进下一个能定的帧（已经发出去的帧不能改）
    PutLocal(f, localMask, checksum, true);
    SendInputs();
    if (!WaitInputs(f, f)) return false;
    std::lock_guard<std::mutex> lk(g_mx);
    CompareChecksums(f);
    *left = 0;
    for (int s = 0; s < MAX_SEATS; ++s) {
        uint32_t v = 0;
        if (s >= g_seats || SeatDropped(s, f)) {
            masks[s] = 0;
            if (s < g_seats) *left |= (uint8_t)(1 << s);
            continue;
        }
        g_in[s].Get(f, &v);
        masks[s] = (uint8_t)v;
    }
    ++g_lsFrame;
    return true;
}

void NetStatus(NetStatusInfo* s)
{
    *s = NetStatusInfo {};
    s->role = g_role;
    s->seat = MySeat();
    s->seats = g_session ? g_seats : (g_role == NET_HOST ? g_cfgSeats : (int)g_hostSeats);
    s->buildMismatch = g_buildMismatch;
    s->full = g_full;
    s->delay = g_session ? g_curDelay : g_cfgDelay;
    s->autoDelay = g_cfgDelay == 0;
    s->inGame = g_session;
    s->rollback = g_session ? g_rb : g_cfgRollback;
    s->broken = g_broken;
    s->desyncFrame = g_desyncFrame;
    s->desyncSeat = g_desyncSeat;
    s->frame = g_lsFrame;
    s->port = g_port;
    std::lock_guard<std::mutex> lk(g_mx);
    s->left = g_session ? g_dropMask : 0;
    if (g_role == NET_GUEST) {
        s->connected = g_peers[0].connected;
        s->guests = s->connected;
        s->rtt[0] = g_peers[0].rtt;
        for (int seat = 1; seat < MAX_SEATS; ++seat)   // 直连的另一个客机
            if (seat != MySeat() && g_peers[seat].connected) {
                s->direct |= (uint8_t)(1 << seat);
                s->rtt[seat] = g_peers[seat].rtt;
            }
    } else {
        s->connected = true;
        for (int seat = 1; seat < MAX_SEATS; ++seat) {
            s->rtt[seat] = g_peers[seat].connected ? g_peers[seat].rtt : -1;
            if (seat < g_cfgSeats) {
                s->guests += g_peers[seat].connected;
                if (!g_peers[seat].connected) s->connected = false;
            }
        }
    }
}

// 本机的公网 IPv6 地址（全局单播 2000::/3；优先不是临时地址的那个）。没有时返回空
std::string NetLocalIPv6()
{
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf(size);
    auto aa = (IP_ADAPTER_ADDRESSES*)buf.data();
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET6, flags, nullptr, aa, &size) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        aa = (IP_ADAPTER_ADDRESSES*)buf.data();
        if (GetAdaptersAddresses(AF_INET6, flags, nullptr, aa, &size) != NO_ERROR) return "";
    }
    std::string stable, temp;
    for (auto a = aa; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        for (auto u = a->FirstUnicastAddress; u; u = u->Next) {
            auto sa = (const sockaddr_in6*)u->Address.lpSockaddr;
            const uint8_t* b = (const uint8_t*)&sa->sin6_addr;
            if ((b[0] & 0xE0) != 0x20) continue;                 // 不是全局单播
            if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0 && b[3] == 0) continue;   // Teredo
            if (u->DadState != IpDadStatePreferred) continue;
            char ip[64];
            inet_ntop(AF_INET6, &sa->sin6_addr, ip, sizeof ip);
            if (u->SuffixOrigin == IpSuffixOriginRandom) { if (temp.empty()) temp = ip; }
            else if (stable.empty()) stable = ip;
        }
    }
    return stable.empty() ? temp : stable;
}

// 时间同步：本机比别人领先时，这一帧多睡几 ms（帧率限速那里调用）。领先超过 0.3 帧才动，每帧最多 4 ms
double NetPaceMs()
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_session || g_broken) return 0;
    double lead = 0;
    for (int s = 0; s < g_seats; ++s)
        if (s != MySeat() && g_advValid[s] && g_advTheirsValid[s] && !(g_dropMask >> s & 1))
            lead = std::max(lead, (g_adv[s] - g_advTheirs[s]) / 2);
    if (lead <= 0.3) return 0;
    return std::min(4.0, (lead - 0.3) * FRAME_MS * 0.5);
}

// ------------------------------------------------------------------ 回滚模式
bool NetRollbackGame() { return g_session && g_rb; }
int NetFrameNo() { return g_lsFrame; }

// 等到所有人第 frame 帧的按键都到（不能回滚的场面：按锁步来）。客机和主机断开时返回 false
bool NetWaitFor(int frame)
{
    if (!g_session || g_broken) return false;
    return WaitInputs(frame, frame);
}

// 回滚模式的一帧：本机按键定到 f + 延迟；返回第 f 帧各座位要用的输入（到了的用真的，没到的预测）。
// 有人落后超过 RB_MAX 帧时等。客机和主机断开时返回 false
bool NetFrameRB(uint8_t localMask, uint8_t masks[MAX_SEATS], uint8_t* left)
{
    if (!g_session || g_broken) return false;
    int f = g_lsFrame;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_curFrame = f;
        g_frameStartQpc = QpcNow();
        if (g_debug && f % 300 == 0 && f > 0)
            Log("net: frame %d delay %d advantage mine %.2f / %.2f / %.2f theirs %.2f / %.2f / %.2f", f, g_curDelay, g_adv[0], g_adv[1],
                g_adv[2], g_advTheirs[0], g_advTheirs[1], g_advTheirs[2]);
    }
    if (g_debug && f % 600 == 0 && f > 0) {
        std::lock_guard<std::mutex> lk(g_mx);
        Log("net: checksums compared up to %d / %d / %d, confirmed %d / %d / %d", g_csChecked[0], g_csChecked[1], g_csChecked[2],
            g_rbChecked[0], g_rbChecked[1], g_rbChecked[2]);
    }
    UpdateDelay();
    PutLocal(f, localMask, 0, false);
    {
        // 校验值：之前的按键都已确定的帧（第 x 帧开始时的状态只取决于 x 之前的输入）
        std::lock_guard<std::mutex> lk(g_mx);
        int confirmed = INT_MAX;
        for (int s = 0; s < g_seats; ++s)
            if (s != MySeat()) confirmed = std::min(confirmed, g_rbChecked[s]);
        if (confirmed == INT_MAX) confirmed = f - 1;
        uint32_t cs;
        for (int x = g_csSent + 1; x <= confirmed + 1 && x <= f; ++x) {
            if (g_csMine.Get(x, &cs)) g_cs[MySeat()].Put(x, cs);
            g_csSent = x;
        }
    }
    SendInputs();
    if (!WaitInputs(f - RB_MAX, f)) return false;
    std::lock_guard<std::mutex> lk(g_mx);
    CompareChecksums(f);
    *left = 0;
    for (int s = 0; s < MAX_SEATS; ++s) {
        bool actual;
        uint32_t v = s == MySeat() ? 0 : InputFor(s, f, &actual);
        if (s == MySeat()) g_in[s].Get(f, &v);
        if (s < g_seats) g_used[s].Put(f, v);
        masks[s] = (uint8_t)v;
        if ((v & IN_LEFT) && s < g_seats) *left |= (uint8_t)(1 << s);
    }
    ++g_lsFrame;
    return true;
}

// 帧边界：真的按键到了、和当时预测的不一样的最早一帧（-1 = 没有）
int NetMispredicted()
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_session || !g_rb) return -1;
    int first = -1;
    for (int s = 0; s < g_seats; ++s) {
        if (s == MySeat()) continue;
        for (int x = g_rbChecked[s] + 1; x < g_lsFrame; ++x) {
            bool actual;
            uint32_t v = InputFor(s, x, &actual);
            if (!actual) break;
            uint32_t used = 0;
            if (!g_used[s].Get(x, &used) || used != v)
                if (first < 0 || x < first) first = x;
            g_rbChecked[s] = x;
        }
    }
    return first;
}

// 回滚重跑第 x 帧时的输入（同样记下用了什么）
void NetInputsRB(int x, uint8_t masks[MAX_SEATS], uint8_t* left)
{
    std::lock_guard<std::mutex> lk(g_mx);
    *left = 0;
    for (int s = 0; s < MAX_SEATS; ++s) {
        bool actual;
        uint32_t v = 0;
        if (s == MySeat()) g_in[s].Get(x, &v);
        else v = InputFor(s, x, &actual);
        if (s < g_seats) g_used[s].Put(x, v);
        masks[s] = (uint8_t)v;
        if ((v & IN_LEFT) && s < g_seats) *left |= (uint8_t)(1 << s);
    }
}

// 第 x 帧开始时本机算的校验值（回滚重跑时会再报一次，以最后的为准）
void NetChecksumRB(int x, uint32_t cs)
{
    std::lock_guard<std::mutex> lk(g_mx);
    g_csMine.Put(x, cs);
}

// 回滚重跑停在了第 frame 帧（那里要切场景，只能按确定的输入正常跑）：之后的帧重新跑
void NetRewind(int frame)
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (frame < g_lsFrame) {
        Log("net: rewound from frame %d to %d", g_lsFrame, frame);
        g_lsFrame = frame;
    }
}

int NetConfirmedFrame()
{
    std::lock_guard<std::mutex> lk(g_mx);
    int confirmed = g_lsFrame - 1;
    for (int s = 0; s < g_seats; ++s)
        if (s != MySeat()) confirmed = std::min(confirmed, g_rbChecked[s]);
    return confirmed;
}

// 回滚超出范围：报告为不同步
void NetReportDesync(int frame)
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_desyncFrame < 0) {
        g_desyncFrame = frame;
        g_desyncSeat = MySeat();
        Log("net: DESYNC: cannot roll back to frame %d", frame);
    }
}

