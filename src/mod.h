// le04_multi —— 連縁天影戦記 (Le04 1.20f) 双人模式
// 模块间共享声明
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

// ------------------------------------------------------------------ core.cpp
extern HMODULE g_self;
extern uintptr_t g_base;          // 游戏模块基址（Le04_Multi.exe 去掉了 ASLR，通常是 0x400000）
extern wchar_t g_dir[MAX_PATH];   // 游戏目录（带结尾反斜杠）
extern bool g_debug;              // 游戏目录有 multi_debug 文件时为调试模式
extern HWND g_hwnd;
extern int g_frame;               // 每次 Present +1

void Log(const char* fmt, ...);
extern std::wstring g_tag;
std::wstring DebugFile(const wchar_t* suffix);   // multi<tag><suffix>
std::wstring GamePath(const wchar_t* name);
bool ReadU32(uintptr_t a, uint32_t* out);
bool WriteU32(uintptr_t a, uint32_t v);
std::vector<uint8_t> Hex(const char* s);
bool WriteCode(uintptr_t addr, const uint8_t* data, size_t n);
bool BytesAt(uintptr_t addr, const std::vector<uint8_t>& b);
inline uintptr_t Rva(uint32_t rva) { return g_base + rva; }
inline uintptr_t Va(uint32_t va) { return g_base + (va - 0x400000); }   // 反汇编里的 VA（基址 0x400000）
// 内联钩子：复制入口 n 字节（必须是完整、不含相对跳转的指令）到跳板，入口改成 jmp
bool InlineHookN(void* target, void* detour, void** orig, size_t n);
bool InlineHookHotpatch(void* target, void* detour, void** orig);
bool PatchCall(uint32_t rva, void* target, uint32_t expectRva);

// ------------------------------------------------------------------ render.cpp
bool InstallRenderHooks();
bool CaptureBackbuffer(std::vector<uint8_t>& bgra, UINT& w, UINT& h);
extern int g_fpsCap;              // 帧率：0 = 60fps，>0 = 指定帧率，<0 = 跟显示器刷新率（调试加速）

// ------------------------------------------------------------------ input.cpp
bool InstallKeyboardHook();
void DebugInputTick();
const uint8_t* DikBuffer();       // DxLib 的 256 字节 DIK 缓冲（0x80 = 按下）

// ------------------------------------------------------------------ debug.cpp
void DebugTick();
void InstallDebugVeh();
void SetGod(bool on);

// ------------------------------------------------------------------ launcher.cpp
bool InstallLauncher();           // WinMain 之前弹出设置窗口

// ------------------------------------------------------------------ net.cpp
enum { NET_SOLO, NET_LOCAL, NET_HOST, NET_GUEST };
constexpr int MAX_SEATS = 3;      // 最多几个人（座位 0 = 1P）
#pragma pack(push, 1)
struct NetStartInfo {             // 开局时两边交换的状态
    uint8_t stgSet[0x7C];         // STG_set +4..+0x80（模式、难度、关卡、种子、练习选择……以主机的为准）
    uint8_t pu[0x314];            // Player_Upgrade +4..+0x318（各自的升级）
    uint8_t optAssist, optXBomb;  // 存档选项 0x1AD4B31 即死（以主机的为准）/ 0x1AD4B33 フラッシュボム＝ボム（各自的）
    uint8_t unlocks[38];          // 0x1AD4B34（以主机的为准）
    uint8_t flag7c, flag7d;       // 0x1AD4B7C / 7D（以主机的为准）
    int32_t ships[3];             // 各自选的主 / 副 / 支援
    uint8_t rules;                // 规则（以主机的为准）：RULE_* 的位
};
#pragma pack(pop)
struct NetStatusInfo {
    int role, seat, seats;            // 本机的座位、这一局（或主机设的）人数
    bool connected;                   // 客机：连着主机；主机：所有客机都连上了
    bool buildMismatch, full, inGame, broken, autoDelay;
    int guests;                       // 连着的客机数（客机：0 / 1 = 主机）
    int rtt[MAX_SEATS];               // 主机：到各客机（下标 = 座位）；客机：[0] = 到主机
    int delay, desyncFrame, desyncSeat, frame, port;
    uint8_t left;                     // 这一局已退出的座位
    uint8_t direct;                   // 客机：和哪些客机直连（rtt[座位] 是直连的 RTT）
    bool rollback;                    // 这一局（或主机设的）用回滚模式
};
void NetConfigure(int role, const std::string& host, int port, int delay, int seats, bool rollback);
bool NetStart();
int NetRole();
int NetSeat();                    // 本机的座位：主机 0，客机 1 / 2
bool NetConnected();
bool NetInGame();
bool NetBarrier(const NetStartInfo& mine, NetStartInfo infos[MAX_SEATS], int* seats);
void NetEndGame();
bool NetFrame(uint8_t localMask, uint32_t checksum, uint8_t masks[MAX_SEATS], uint8_t* left);
void NetStatus(NetStatusInfo* s);
std::string NetLocalIPv6();       // 本机的公网 IPv6 地址（没有时为空）
double NetPaceMs();               // 时间同步：本机领先时这一帧该多等的 ms
// 回滚模式
bool NetRollbackGame();           // 这一局用回滚
int NetFrameNo();                 // 下一个要处理的帧
bool NetWaitFor(int frame);       // 等齐第 frame 帧所有人的按键（不能回滚的场面）
bool NetFrameRB(uint8_t localMask, uint8_t masks[MAX_SEATS], uint8_t* left);
int NetMispredicted();            // 预测错的最早一帧（-1 = 没有）
void NetInputsRB(int frame, uint8_t masks[MAX_SEATS], uint8_t* left);
void NetChecksumRB(int frame, uint32_t cs);
void NetReportDesync(int frame);
void NetRewind(int frame);        // 回滚重跑停在第 frame 帧（之后的帧重新正常跑）
int NetConfirmedFrame();          // 所有人的输入都确定、比过了的最后一帧

// ------------------------------------------------------------------ lang.cpp
enum { LG_ZH, LG_EN, LG_JA, LG_KO, LG_COUNT };
enum TextId {
    TX_TITLE, TX_LANGUAGE, TX_MODE, TX_SOLO, TX_LOCAL2, TX_LOCAL3, TX_HOST, TX_GUEST, TX_SHIPS_P2, TX_SHIPS_P3, TX_SAME_AS_P1,
    TX_RULES, TX_RULE_AIM, TX_RULE_ROUTE, TX_RULE_SPELLS, TX_ONLINE, TX_ADDR, TX_PORT, TX_DELAY, TX_SEATS, TX_SEATS2, TX_SEATS3, TX_DELAY_NOTE,
    TX_START, TX_QUIT, TX_NEED_ADDR, TX_NET_FAIL,
    TX_W_WAIT_GUESTS, TX_W_WAIT_READY, TX_W_FULL, TX_W_CONNECTING, TX_W_STARTING, TX_W_WAIT_HOST,
    TX_O_HOST, TX_O_GUEST, TX_O_DELAY, TX_O_AUTO, TX_O_MISMATCH, TX_O_FULL, TX_O_WAIT_GUESTS, TX_O_CONNECTING, TX_O_LOST,
    TX_O_DESYNC, TX_O_LEFT,
    TX_MY_IPV6, TX_COPY, TX_NO_IPV6, TX_O_DIRECT,
    TX_ROLLBACK, TX_O_ROLLBACK,
    TX_COUNT
};
extern int g_lang;                // 界面语言（LG_*）
int DetectLang();                 // 按 Windows 的界面语言
const char* Tr(int id);           // 当前语言的文字（UTF-8）
std::wstring TrW(int id);
std::wstring Widen(const char* utf8);
const char* LangName(int lang);
std::string AllText();            // 当前语言用到的全部文字（建字体用）

// ------------------------------------------------------------------ rollback.cpp
bool InstallArena();              // DllMain 里最先调用：接管游戏的 malloc / free（确定性内存池）
bool InstallRollback();
void ArenaUse(bool on);           // 启动设置定好后：联机 / 调试才用内存池
void RollbackTest(int frames);    // 调试：快照 → 玩 N 帧 → 还原重跑 → 比较
void RollbackSyncTest(int k);     // 调试：逐帧快照，每 30 帧回滚 k 帧重跑并比较（0 = 关）
void DebugFrameIo(int frame);     // 调试：上一帧主线程读过的文件
bool RbInPool(const void* p);
void RbDebugMessFpu();
int RbDebugPhase();               // 调试：模拟层数 + 绘制里时 100
void RbTraceFrame(int frame);     // 调试：这一帧开始模拟（记 GetRand 的调用位置）
void RbTraceDump(int frame);
void RbDebugHandles();            // 调试：按来源统计还在的 DxLib 句柄
int RbDebugHandleCount();         // 调试：还在的 DxLib 句柄数
void RbCollectGameHandles(bool on);   // STG_main 构造期间新建的 DxLib 句柄开始 / 停止记下
int RbFreeGameHandles();          // STG_main 析构后：删掉记下的、还在的（原版漏删的），返回删了几个
enum { RNG_RESIM_CUTIN, RNG_DRAW_CUTIN, RNG_HUD, RNG_KEEP_SLOTS };
void RngKeep(int slot);           // 随机数状态整个存进格子 slot / 放回（rollback.cpp）
void RngPut(int slot);
void DebugMemory(const char* when);   // 调试：进程内存
// 逐帧快照（联机回滚）
bool RbBegin();                   // 一局开始时（第 0 帧开始）
void RbEnd();
bool RbActive();
void RbSave(int frame);           // 第 frame 帧开始时
bool RbCanRestore(int g, int cur);
void RbConfirm(int frame);        // 第 frame 帧及之前不会再回滚了
bool RbRestore(int g, int cur);   // 回到第 g 帧开始时（现在在第 cur 帧开始，已 RbSave(cur)）
void RbSimulate();                // 重跑一帧（不画），输入已放好
bool RbSceneSwitchPending();      // 下一帧要切场景（不能回滚）
void RbSyncBgm();                 // 回滚重跑完：真实的 BGM 播放状态跟上恢复后的标记
void RbFrameHandles(int frame, int* created, int* deleted);   // 调试
void RbKeepDraw();                // 回滚前：记下只在绘制里推进的状态
void RbPutDraw();                 // 重跑完：写回
void RollbackBoundary();          // 每帧开始（Controller::Update 之前）
void RollbackAfterInput();        // 每帧的输入定好之后

// ------------------------------------------------------------------ coop.cpp
bool InstallCoop();
// 启动时：模式、本地几个人、本地 2P / 3P 的自机（ships[座位-1]，-1 = 同 1P）、自机狙规则
// rules：RULE_* 的位（联机时以主机的为准）
enum { RULE_AIM_NEAREST = 1, RULE_ROUTE_ALL = 2, RULE_MULTI_SPELLS = 4 };
void CoopConfigure(int role, int localSeats, const int ships[MAX_SEATS - 1][3], int rules);
uintptr_t CoopStg();              // 当前的 STG_main（没有时 0）
uintptr_t CoopPm(int seat);       // 0 = 1P，1 = 2P，2 = 3P
void DrawCoopUi();                // 每帧在 ImGui 帧里调用
void CoopDebugHudBombs();         // 调试：HUD 的ボム图标位置
void CoopDebugDarkness();         // 调试：列出视界不良的每一层
void CoopSetGod(int seat, bool on);   // 调试：按座位无敌（0 = P1，1 = P2）
void CoopSimState(std::vector<std::pair<void*, size_t>>& out);   // 每局的模拟状态（回滚要一起保存）
void CoopGetInputs(int hold[MAX_SEATS][16]);   // 各座位这一帧的按键持续帧数
void CoopSetInputs(const int hold[MAX_SEATS][16]);
