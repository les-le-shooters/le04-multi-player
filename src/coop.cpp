// coop.cpp —— 2P / 3P
// 做法：用游戏自己的构造函数为每个多出来的座位再建一个玩家管理器 PM。游戏内存里同一时刻只放一个座位的"上下文"，
// 其它座位的存在 g_ctx 里；每帧在游戏处理完内存里那个座位之后，依次换成其它座位的上下文，再调用一遍同样的
// 游戏函数（更新 / 碰撞 / 绘制），然后换回。上下文包括：
//   [STG+0] 玩家管理器、STG+0x38/3C/40 主/副/支援自机、Controller 的按键持续帧数表、
//   Player_Upgrade 的升级数据（每人一份）、存档选项 0x1AD4B31（即死）/ 0x1AD4B33（フラッシュボム＝ボム）。
// 平时内存里是 1P；1P 出局而别人还在时，世界更新期间换成第一个还在场的人（敌人瞄准、道具、死亡判定都对着他）。
// 自机狙默认瞄准离那个敌人 / Boss 最近的自机（th07_multi_player 的做法）；残机 / 火力转移参照 th06_multi_net、eagler-touhou。
// 残机用完、还有别人在场时成为幽灵（th07_multi_player 的做法）：停在场地下方、半透明、不能操作也不会被弹，
// 队友靠近它按住低速可以花一个残机复活它；过关时、エクステンド时自动复活。
// 地址均为 Le04 1.20f 的 VA（基址 0x400000）。
#include "mod.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>
#include "imgui.h"

namespace {

// ---- STG_main（游戏核心对象，[Game+0xC]）
constexpr uint32_t VA_STG_CTOR = 0x612040;      // thiscall，无参数；构造时 PM = new(0x520) + 0x61D710
constexpr uint32_t VA_STG_DTOR = 0x613DF0;      // 析构里 PM 由 0x611760 + operator delete 释放
constexpr uint32_t STG_FIELD_RECT = 0x1C, STG_FIELD_X = 0x20, STG_FIELD_Y = 0x24, STG_FIELD_W = 0x28, STG_FIELD_H = 0x2C;
constexpr uint32_t STG_MODE = 0x30, STG_DIFF = 0x34;
constexpr uint32_t STG_SHIPS = 0x38;            // 主 / 副 / 支援
constexpr uint32_t STG_BATTLE = 0x4C;           // 0 道中、1 Boss、2/3 特殊
constexpr uint32_t STG_PAUSED = 0x7A34, STG_STATE = 0x7AF8, STG_PRACTICE = 0x7AB0;
constexpr uint32_t VA_WORLD_UPDATE = 0x651EA0;  // thiscall STG：GAME_state 2/3/5 时每帧
constexpr uint32_t VA_STAGE_NEXT = 0x64A590, CALL_STAGE_NEXT = 0x6544C5;   // thiscall STG：过关后进下一关
constexpr uint32_t VA_CONTINUE_MENU = 0x64EF00, CALL_CONTINUE_MENU = 0x6544F0;   // GAME_state 7
constexpr uint32_t PATCH_GAME_OVER = 0x652A6D;  // mov al,[edx+49Ah]：出局 → GAME_state 7
// ---- 玩家管理器 PM，状态 ST = [PM]
constexpr uint32_t VA_PM_CTOR = 0x61D710;       // thiscall(rect, main, sub, support, diff, main)，ret 18h
constexpr uint32_t VA_PM_DTOR = 0x611760;
constexpr uint32_t VA_OP_NEW = 0x864674, VA_OP_DELETE = 0x863AB4;
constexpr uint32_t PM_OUT = 0x49A;              // 残机用完（游戏结束）
constexpr uint32_t PM_LOCK = 0x49B;             // 操作锁定（对话、Boss 击破等，世界更新每帧写）
constexpr uint32_t VA_PM_UPDATE = 0x62A2D0, CALL_PM_UPDATE = 0x651FFB;   // 世界更新里
constexpr uint32_t VA_MODIFIERS = 0x6422F0;     // thiscall STG：关卡修饰对 [STG] 的效果（按 STG+0x7B20 的 4 格分派）
constexpr uint32_t STG_MODIFIER_SLOTS = 0x7B20;  // 关卡修饰（副事件）4 格，ID 0..0x29，-1 = 空
constexpr uint32_t PM_MODIFIER_COPY = 0x4B8;     // 部队开始时（0x6403E5）只抄进 [STG] 的 PM：高重力、ボム封印、反转、速度限制等读的是这份
constexpr uint32_t CALL_EXTEND = 0x640234, VA_EXTEND = 0x61D340;   // エクステンド（0x1B）：thiscall PM(?)，残机 +1
constexpr uint32_t PATCH_TALK_SHIP = 0x6530BD;   // 读对话脚本时 push [STG+0x38]（对话用哪个自机的文本）
// ---- 视界不良（关卡修饰 0x13..0x15）：0x6422F0 每帧在自机处放一个 0x5D 号特效（中间透明的黑图，约 10 层叠着），
// 场地绘制在 0x651556 用 Effect_main::DrawType(0x5D, 1, 0) 画。各人各放一套时黑层相乘，只剩几个洞的交集看得见
constexpr uint32_t VA_DRAW_TYPE = 0x40B1E0, CALL_DRAW_DARKNESS = 0x651556;
constexpr int EFF_DARKNESS = 0x5D;
constexpr uint32_t PM_SLOW = 0x498;               // PM：低速中（byte）
constexpr uint32_t VA_DELETE_GRAPH = 0x6A4B40;    // DeleteGraph(handle, logOut)
constexpr uint32_t EM_DARK_GRAPH = 0x1118;       // Effect_main+0x1118：low_visibility.png
constexpr uint32_t EFF_AGE = 4, EFF_TYPE = 8, EFF_X = 0x18, EFF_Y = 0x1C, EFF_SCALE = 0x48, EFF_ANGLE = 0x50, EFF_TAG = 0x9C;
constexpr float DARK_SHRINK = 0.85f;             // 多人在场时各自的视野缩小一点
// DxLib（cdecl）
constexpr uint32_t VA_MAKE_SCREEN = 0x67E120, VA_SET_DRAW_SCREEN = 0x67F0F0, VA_SET_BLEND = 0x67EF60, VA_DRAW_BOX = 0x66B790,
    VA_GET_COLOR = 0x68AD10, VA_DRAW_ROTA_F = 0x66FE10, VA_DRAW_GRAPH = 0x66D990;
constexpr int DX_SCREEN_BACK = -2, BLEND_NONE = 0, BLEND_ALPHA = 1, BLEND_ADD = 2, BLEND_MUL = 4, BLEND_INVDEST = 9;
constexpr uint32_t VA_NEAREST = 0x64A130;       // thiscall STG：给 [STG] 算自机弹的追踪目标
constexpr uint32_t VA_COLLISION = 0x643A00, CALL_COLLISION = 0x6526AB;    // thiscall STG：所有与自机有关的判定
constexpr uint32_t VA_PM_SPRITE = 0x6252C0;     // 场地绘制 0x64F630 里 1P 四个绘制调用的最后一个：自机图
constexpr uint32_t VA_PM_DRAW0 = 0x629080;      // thiscall PM
constexpr uint32_t VA_BULLETS_DRAW = 0x62B310;  // thiscall 弹容器(int, int)
constexpr uint32_t VA_LASERS_DRAW = 0x5A33A0;   // thiscall 激光容器
constexpr uint32_t VA_SUPPORT_DRAW = 0x625FB0, CALL_SUPPORT_DRAW = 0x650911;
constexpr uint32_t VA_DAMAGE = 0x6201A0;        // thiscall PM(dmg, bool, bool)，ret 0Ch，命中返回 al=1
constexpr uint32_t VA_BOSS_DAMAGE = 0x636C50;   // thiscall Boss(dmg, bool, bool)，ret 0Ch
// ---- Boss
constexpr uint32_t B_SPELL_FAILED = 0x108;      // 符卡奖励失败（世界更新里锁存 ST+0xB0）
// ---- 道具
constexpr uint32_t VA_ITEM_MAIN = 0x2FB2F88;    // Item_main 单例；+4/+8 vector<Item*>，+0x18 吸引目标（Vec2*），+0x20 最大下标
constexpr uint32_t VA_ITEM_UPDATE = 0x5A0530, CALL_ITEM_UPDATE = 0x6524E0;
constexpr uint32_t VA_ITEM_ATTRACT = 0x635A20, CALL_ITEM_ATTRACT = 0x6492B9;  // 低速吸引：设 / 清道具状态 1
constexpr uint32_t VA_COLLECT_SCALE = 0x9ED310; // 自动回收线 = 升级值 × 它
// ---- 输入
constexpr uint32_t VA_CTRL_UPDATE = 0x407220, CALL_CTRL_UPDATE = 0x616049;
constexpr uint32_t VA_HOLD = 0x2FB2A8C;         // int[16] 按键持续帧数（按手柄位编号）
constexpr uint32_t VA_SLOT_MAP = 0x2FB2B8C;     // int[8] 槽位 -> 手柄位：下 左 右 上 射击 ボム 暂停 低速
constexpr uint32_t VA_LOADING = 0x2FAFEC8;      // 异步加载中（游戏这时把按键全清零）
constexpr uint32_t VA_GET_JOYPAD = 0x6A16E0;    // DxLib GetJoypadInputState（cdecl）：1..4 = 1..4 号手柄
constexpr int SLOT_SHOT = 4, SLOT_BOMB = 5, SLOT_PAUSE = 6, SLOT_FOCUS = 7;
// ---- 升级
constexpr uint32_t VA_PU = 0x2FB0270;           // Player_Upgrade 单例
constexpr uint32_t PU_DATA = 4, PU_DATA_END = 0x318;   // 每人一份的范围（+0x318 是 Boss / 绘制会改写的位掩码）
constexpr uint32_t PU_OFF = 0x169;              // 升级关闭
constexpr uint32_t PU_LINE_ON = 0x290, PU_LINE_OFF = 0x294;   // 自动回收线（float）
constexpr uint32_t VA_PU_RESET = 0x407C90;      // 开局时按顺序调用：RESET(0) LEVELS() SHIPS(主,副,支援) COST(0)
constexpr uint32_t VA_PU_LEVELS = 0x5BD650;
constexpr uint32_t VA_PU_SHIPS = 0x4083E0;
constexpr uint32_t VA_PU_COST = 0x408300;
constexpr uint32_t PU_LIVES_BASE = 0x16C, PU_LIVES = 0x170;   // 开局残机：升级关闭时 / 平时（PM 构造时抄进 ST+0x48）
// 存档选项：即死モード（被弹直接掉残机；HUD 构造时选 game_board_death.png，酒旁边画"x３"，开局残机基础 4）、
// フラッシュボム＝ボム
constexpr uint32_t VA_OPT_ASSIST = 0x1AD4B31, VA_OPT_XBOMB = 0x1AD4B33;
// ---- 联机开局同步的状态（与录像文件头记录的范围一致）
constexpr uint32_t VA_STG_SET = 0x2FB2F08;      // STG_set 单例（+0 是虚表）
constexpr uint32_t VA_STG_SET_SHIPS = 0x2FB2F10;
constexpr uint32_t VA_UNLOCKS = 0x1AD4B34, VA_FLAG_7C = 0x1AD4B7C, VA_FLAG_7D = 0x1AD4B7D;
constexpr uint32_t VA_EFFECT_MAIN_GET = 0x41D990, VA_EFFECT_CLEAR = 0x40AB30, VA_ITEM_CLEAR = 0x59FF50;
constexpr uint32_t VA_MT_INDEX = 0x107E4D0, VA_MT_STATE = 0x107E4D8;   // DxLib GetRand 的梅森旋转
constexpr uint32_t VA_SET_ASYNC_LOAD = 0x6A1510;   // DxLib SetUseASyncLoadFlag：mov eax,[ebp+8] / mov [0xBB6D1C],eax
// ボム名 / フラッシュボム名的图：STG 构造时 Effect_main(0x2FB0590)::LoadBombNames(0x409060, thiscall(?, 副自机, ?), ret 0Ch)
// 按 1P 的副自机读进 Effect_main 的这几格，横幅特效绘制时用（0x40F299 / 0x40F4B4）
constexpr uint32_t VA_EFFECT_MAIN = 0x2FB0590, VA_LOAD_BOMB_NAMES = 0x409060;
constexpr uint32_t BOMB_NAME_SLOTS[5] = { 0x380, 0x384, 0x394, 0x398, 0x39C };   // ボム名、枠、フラッシュ名、枠、（藪雨）
constexpr uint32_t VA_FOPEN_S = 0x8637C0, VA_FCLOSE = 0x863DA1;   // CRT；存档由 SAVE_dat 的各个保存函数用 fopen_s("save_dat/…", "wb") 整块写出
// ---- 其它
constexpr uint32_t VA_PRAC = 0x2FB2F30, VA_PRAC_ST7 = 0x2FB2F28;   // STG 构造函数末尾对 1P 的练习模式调整（0x613935 起）
constexpr uint32_t VA_REPLAY_RECORDING = 0x2FAFED4;                // Replay_main+4
constexpr uint32_t CALL_WAIT_VSYNC = 0x615F94, VA_WAIT_VSYNC = 0x680930;  // 主循环里的 WaitVSync(1)
// ---- 自机狙：敌人管理器 [STG+4] 的 +0x2CC 和 Boss [STG+8] 的 +0x1C 是"瞄准的自机位置"（Vec2*），世界更新每帧写成 [STG]
// 那个自机的（有诱饵 PM+0x4E8 时是 PM+0x4F0）。各敌人的行动函数（0x5DE000 起）开枪时直接读 M+0x2CC
constexpr uint32_t PATCH_ENEMY_PATTERN = 0x5D401C;   // mov eax,[edx+eax*4+2F0h] / call eax：每个敌人的行动函数（push 下标，edx = M）
constexpr uint32_t VA_ENEMY_UPDATE = 0x5D3F90, CALL_ENEMY_UPDATE = 0x652034;
constexpr uint32_t VA_BOSS_UPDATE = 0x44BD70, CALL_BOSS_UPDATE = 0x6520E5;
constexpr uint32_t M_AIM = 0x2CC, B_AIM = 0x1C, PM_DECOY = 0x4E8, PM_DECOY_POS = 0x4F0;
// ---- 残机 / 火力转移
constexpr uint32_t ST_LIVES = 0x48, ST_STATE = 0x54, ST_SCORE = 0x58, ST_POWER = 0xA4;
constexpr uint32_t ST_FLASH = 0x90;              // 瞬雷槽（满 4000：低速 + X 放瞬雷；不满时放ボム）
constexpr int FLASH_MAX = 4000;   // ST+0x54：0 通常，1..4 被弹 / 无敌，7 ボム，8 出局
constexpr int POWER_MAX = 50000, POWER_GIVE = 10000;   // 火力 0..50000（显示 0.00..5.00），一次转移 1.00
constexpr float GIVE_RANGE = 24;                       // 两机距离（像素）
constexpr int LIFE_HOLD = 90, TAP_GAP = 24, TAP_COUNT = 5;
constexpr uint32_t VA_SE_GUARD = 0x10896B0, VA_SE_EXTEND = 0x10899AC;   // エクステンド的音效（静态对象）：+0 播放、+0xC 音量
// ---- 队友靠近时半透明（只是绘制，不影响模拟）
constexpr uint32_t CALL_PM_DRAW0 = 0x650E12, PATCH_SKIP_P1_DRAW = 0x650E19;   // 场地绘制里 1P 的四个绘制调用（0x650E10..0x650E34）
constexpr uint32_t VA_BLEND_MODE = 0xB94FE4, VA_BLEND_PARAM = 0xB94FE8;      // DxLib 当前的混合模式 / 参数
constexpr float FADE_NEAR = 24, FADE_FAR = 96, FADE_MIN = 0.35f;
// ---- 幽灵：场地下方中间，按座位左右排开，上下轻微浮动
constexpr float GHOST_SPREAD = 48, GHOST_FROM_BOTTOM = 48, GHOST_RANGE = 32, GHOST_ALPHA = 0.5f;
constexpr uint32_t STG_STAGE_CLEAR = 0x7A2C;     // 最后一格的事件结束、要进下一关时置 1（0x652C9C），Boss 曲淡出后进 GAME_state 5
// ---- 进路ゲージ（场地下方决定下一格往哪走的条）：STG+0xB8 每帧加上 (自机 x - 场地中线) × 升级系数（0x63ED40 只看 [STG] 的自机）
constexpr uint32_t VA_ROUTE = 0x63ED40, CALL_ROUTE = 0x65282F;

// ---- HUD（[STG+0x14]）：STG 每帧（0x6545BC 起）把 [STG] 自机的分数、残机、HP、屏障、ボム、火力、瞬雷槽、LV 等抄进它，
// 0x6538B3 调用 HUD 的绘制 0x444820。联机时每台电脑的 HUD 画本机自己的数据：只在绘制期间换成本机座位的值
constexpr uint32_t VA_UI_DRAW = 0x444820, CALL_UI_DRAW = 0x6538B3;
constexpr uint32_t VA_BOMB_COUNT = 0x639A40;     // thiscall PM：现在有几个ボム（按升级的上限数）
constexpr uint32_t UI_FED_BEGIN = 0x04, UI_FED_END = 0xA4, UI_POS = 0x7C3C;
constexpr uint32_t ST_HITS = 0x354;              // 被弹次数（HUD 右下显示 0..99；受伤函数 0x6201A0 里 +1）
constexpr uint32_t PU_LEVEL_ON = 0x25E, PU_LEVEL_OFF = 0x25C;   // u16：左下的 LV（已用的升级 / 技能的总量；升级关闭时用后者，0x4408BF）
constexpr uint32_t VA_RAND_BEGIN = 0x107E4D0, VA_RAND_END = 0x107F860;   // DxLib GetRand 的全部状态（下标、初始化标志、MT 状态、输出缓冲）

constexpr float BOSS_DAMAGE_2P = 0.75f;         // 对 Boss 的伤害：两人在场 ×0.75，三人在场 ×2/3（th07_multi_player 的值）

using ThisFn = void(__thiscall*)(uintptr_t);
using Arg1Fn = void(__thiscall*)(uintptr_t, int);
using Arg2Fn = void(__thiscall*)(uintptr_t, int, int);
using Arg3Fn = void(__thiscall*)(uintptr_t, int, int, int);
using BossDamageFn = int(__thiscall*)(uintptr_t, int, int, int);
using PmCtorFn = uintptr_t(__thiscall*)(uintptr_t, uint32_t, int, int, int, int, int);
using DamageFn = bool(__thiscall*)(uintptr_t, int, int, int);
using NewFn = void*(__cdecl*)(size_t);
using DeleteFn = void(__cdecl*)(void*);
using JoypadFn = int(__cdecl*)(int);
using DrawTypeFn = void(__thiscall*)(uintptr_t, int, int, int);
using MakeScreenFn = int(__cdecl*)(int, int, int);
using Int1Fn = int(__cdecl*)(int);
using Int2Fn = int(__cdecl*)(int, int);
using DrawBoxFn = int(__cdecl*)(int, int, int, int, unsigned, int);
using GetColorFn = unsigned(__cdecl*)(int, int, int);
using DrawRotaFFn = int(__cdecl*)(float, float, double, double, int, int, int);
using DrawGraphFn = int(__cdecl*)(int, int, int, int);
using ExtendFn = void(__thiscall*)(uintptr_t, int);
using CountFn = int(__thiscall*)(uintptr_t);

template <class T> T& At(uintptr_t a) { return *(T*)a; }
template <class F> F Fn(uint32_t va) { return (F)Va(va); }

// 一个座位的上下文（不在游戏内存里的座位存在 g_ctx[座位]；g_ctx[g_mem] 不用）
struct Ctx {
    uintptr_t pm = 0;
    int ships[3] = {};
    int hold[16] = {};
    uint8_t pu[PU_DATA_END - PU_DATA] = {};
    uint8_t optAssist = 0, optXBomb = 0;
};

Ctx g_ctx[MAX_SEATS];
uintptr_t g_stg = 0;              // 当前 STG（多人局时）
uintptr_t g_pm[MAX_SEATS] = {};   // 各座位的 PM
int g_seats = 0;                  // 这一局几个人（0 = 不是多人局）
int g_mem = 0;                    // 游戏内存里是哪个座位的上下文
bool g_window = false;            // 正在别的座位的窗口里（防止钩子重入）
int g_role = NET_SOLO;            // 启动器选的模式
int g_cfgSeats = 2;               // 本地多人：几个人
int g_cfgShips[MAX_SEATS][3];     // 本地多人：2P / 3P 的自机（-1 = 跟 1P 一样）
int g_mySeat = 0;                 // 本机操作的座位（联机客机是 1 / 2）
bool g_netGame = false;           // 这一局是联机局（锁步中）
bool g_netSolo = false;           // 联机局里和主机断开了：本机继续一个人玩
bool g_gone[MAX_SEATS] = {};      // 联机局里退出（掉线）的座位：算出局，不再复活、不画
NetStartInfo g_guestOwn;          // 客机：开局时被主机状态覆盖前的自己的状态（局终还原）
bool g_haveGuestOwn = false;
int g_cnt[MAX_SEATS][8];          // 联机：各人各槽位的按键持续帧数
uint32_t g_bombNames[MAX_SEATS][5];   // 各人的ボム名图
int g_viewScore = 0;              // HUD：本机座位的滚动显示分数（规则同 HUD 自己的更新 0x444A7D）
// HUD 的ボム图标动画：看画面的不是内存里那个座位时用的那份（见 DrawHudWithBombIcons）
constexpr uint32_t UI_BOMB_ANIM = 0x7AF0, UI_BOMB_ANIM_SIZE = 10 * 0x20, UI_BOMB_TOP = 0x7C48;
constexpr uint32_t VA_BOMB_DROP_FROM = 0x9ED23C;   // 0x63BDB0：放回的 y = HUD+0x7C48 − 它
struct BombIcons { uint8_t anim[UI_BOMB_ANIM_SIZE]; int slots[10]; bool ready; };
BombIcons g_bombIcons[MAX_SEATS];
bool g_rbFallback = false;        // 联机回滚模式但存不了快照：每帧都等齐（等于锁步）

int g_cfgRules = RULE_AIM_NEAREST | RULE_ROUTE_ALL | RULE_MULTI_SPELLS;   // 启动器设的规则
int g_rules = g_cfgRules;         // 这一局的（联机以主机的为准）
bool g_aimNearest = true;         // 自机狙瞄准最近的自机（否则只瞄准内存里那个座位，平时是 1P）
bool g_god[MAX_SEATS] = {};       // 调试：按座位无敌
uint8_t g_gameOver = 0;           // GameOverStub 的结果

void SwapBytes(void* a, void* b, size_t n)
{
    auto x = (uint8_t*)a, y = (uint8_t*)b;
    for (size_t i = 0; i < n; ++i) std::swap(x[i], y[i]);
}

// 游戏内存 ↔ c
void SwapMem(Ctx& c)
{
    SwapBytes((void*)g_stg, &c.pm, 4);
    SwapBytes((void*)(g_stg + STG_SHIPS), c.ships, sizeof c.ships);
    SwapBytes((void*)Va(VA_HOLD), c.hold, sizeof c.hold);
    SwapBytes((void*)(Va(VA_PU) + PU_DATA), c.pu, sizeof c.pu);
    SwapBytes((void*)Va(VA_OPT_ASSIST), &c.optAssist, 1);
    SwapBytes((void*)Va(VA_OPT_XBOMB), &c.optXBomb, 1);
}

// 把 seat 的上下文换进游戏内存（原来内存里那个座位的存回 g_ctx）
void SwitchTo(int seat)
{
    if (seat == g_mem) return;
    SwapMem(g_ctx[seat]);                     // 现在 g_ctx[seat] 里是原来内存里那个座位的
    std::swap(g_ctx[seat], g_ctx[g_mem]);
    g_mem = seat;
}

bool Coop() { return g_stg && g_seats >= 2; }
bool Out(int seat) { return At<uint8_t>(g_pm[seat] + PM_OUT) != 0; }
bool Alive(int seat) { return Coop() && seat >= 0 && seat < g_seats && g_pm[seat] && !Out(seat); }
bool Ghost(int seat) { return Coop() && seat >= 0 && seat < g_seats && g_pm[seat] && Out(seat) && !g_gone[seat]; }
uintptr_t StOf(int seat) { return At<uintptr_t>(g_pm[seat]); }
float* PosOf(int seat) { return (float*)At<uintptr_t>(StOf(seat)); }

int AliveCount()
{
    int n = 0;
    for (int s = 0; s < g_seats; ++s) n += Alive(s);
    return n;
}

bool AnyAliveExcept(int seat)
{
    for (int s = 0; s < g_seats; ++s)
        if (s != seat && Alive(s)) return true;
    return false;
}

// 关卡修饰函数 0x6422F0 里作用在 [STG] 那个自机身上的：次元決壊 0..3、結界干渉 4、瘴気 0xF、视界不良 0x13..0x15、
// エクステンド失败判定 0x1B。其余（援護射撃、酒の亡者、重力 / 封印的画面效果……）是全场的，给别人再跑会翻倍
bool PerPlayerModifier(int id)
{
    return (id >= 0 && id <= 4) || id == 0xF || (id >= 0x13 && id <= 0x15) || id == 0x1B;
}

// 视界不良特效的归属：+0x9C（0x5D 号不用它；生成时清零）。座位 + 1
// 特效是否存活只看 +0 的那一个字节（游戏自己也只看它）：后面 3 个字节没人初始化，联机时特效对象来自内存池，常有残留的数据
template <class F> void ForEachDarkness(F f)
{
    uintptr_t em = Va(VA_EFFECT_MAIN);
    auto begin = At<uintptr_t*>(em + 4), end = At<uintptr_t*>(em + 8);
    for (auto it = begin; it && it < end; ++it) {
        uintptr_t e = *it;
        if (e && At<uint8_t>(e) == 1 && At<int>(e + EFF_TYPE) == EFF_DARKNESS) f(e);
    }
}

void TagDarkness(int seat)
{
    ForEachDarkness([&](uintptr_t e) {
        if (At<int>(e + EFF_AGE) == 0 && At<int>(e + EFF_TAG) == 0) At<int>(e + EFF_TAG) = seat + 1;
    });
}

// 在 seat 的上下文里执行 f（不管出局与否）
template <class F> void AsSeat(int seat, F f)
{
    if (seat == g_mem) { f(g_pm[seat]); return; }
    if (g_window) return;
    int base = g_mem;
    g_window = true;
    SwitchTo(seat);
    f(g_pm[seat]);
    SwitchTo(base);
    g_window = false;
}

// 依次在其它还在场的座位的上下文里执行 f
template <class F> void ForOthers(F f)
{
    if (!Coop() || g_window) return;
    int base = g_mem;
    for (int s = 0; s < g_seats; ++s)
        if (s != base && Alive(s)) AsSeat(s, f);
}

// 读某个座位的升级值
template <class T> T SeatPu(int seat, uint32_t off)
{
    T v;
    const uint8_t* src = seat == g_mem ? (const uint8_t*)Va(VA_PU) + off : g_ctx[seat].pu + off - PU_DATA;
    memcpy(&v, src, sizeof v);
    return v;
}

// 只用加减乘和 sqrt（IEEE 精确舍入），各台电脑算出的一样（hypotf / sinf 这类库函数可能按 CPU 走不同实现）
float Dist2(const float* a, const float* b)
{
    if (!a || !b) return 1e18f;
    float dx = a[0] - b[0], dy = a[1] - b[1];
    return dx * dx + dy * dy;
}
float Dist(const float* a, const float* b) { return sqrtf(Dist2(a, b)); }

// 一个座位"被瞄准的点"：有诱饵时是诱饵
uintptr_t AimPoint(int seat)
{
    uintptr_t pm = g_pm[seat];
    return At<uint8_t>(pm + PM_DECOY) ? At<uintptr_t>(pm + PM_DECOY_POS) : At<uintptr_t>(At<uintptr_t>(pm));
}

// 离 from 最近的那个自机的瞄准点；在场的不到两人（或关掉了这条规则）时用 fallback（世界更新写的那个）
uintptr_t NearestAim(const float* from, uintptr_t fallback)
{
    if (!Coop() || !g_aimNearest || !from || AliveCount() < 2) return fallback;
    uintptr_t best = 0;
    float bestD = 0;
    for (int s = 0; s < g_seats; ++s) {
        if (!Alive(s)) continue;
        uintptr_t a = AimPoint(s);
        if (!a) continue;
        float d = Dist2(from, (const float*)a);
        if (!best || d < bestD) { best = a; bestD = d; }
    }
    return best ? best : fallback;
}

void ResetGestures();

// ------------------------------------------------------------------ 创建 / 销毁
// 为座位 1..seats-1 建 PM。infos：联机时各人的开局状态（nullptr = 本地多人，按启动窗口选的自机）
void CreatePms(uintptr_t stg, const NetStartInfo* infos, int seats)
{
    uintptr_t pm1 = At<uintptr_t>(stg);
    if (!pm1) return;
    // 0x613935：STG+0x7AB0 == 1（练习）或 [0x2FB2F30] 时，1P 的火力 / 残机在构造函数末尾被改写
    bool prac = At<uint8_t>(stg + STG_PRACTICE) == 1 || At<uint8_t>(Va(VA_PRAC)) != 0;
    bool st7 = At<uint8_t>(Va(VA_PRAC_ST7)) != 0;
    g_stg = stg;
    g_mem = 0;
    g_seats = 0;
    for (int s = 0; s < MAX_SEATS; ++s) {
        g_pm[s] = 0;
        g_ctx[s] = Ctx {};
        g_gone[s] = false;
    }
    g_pm[0] = pm1;
    uintptr_t eff = Va(VA_EFFECT_MAIN);
    for (int i = 0; i < 5; ++i) g_bombNames[0][i] = At<uint32_t>(eff + BOMB_NAME_SLOTS[i]);
    for (int seat = 1; seat < seats; ++seat) {
        Ctx& c = g_ctx[seat];
        if (infos) {
            // 联机：自机、升级、"フラッシュボム＝ボム"是那个客机自己的（客机开局时按自己的存档算好的）。
            // 即死只看主机的：客机的升级是按自己的设定算的，开局残机（PU+0x16C 基础 2 / 即死 4，+0x170 = 基础 + 升级加的）跟着改
            memcpy(c.ships, infos[seat].ships, sizeof c.ships);
            memcpy(c.pu, infos[seat].pu, sizeof c.pu);
            c.optAssist = infos[0].optAssist;
            c.optXBomb = infos[seat].optXBomb;
            int lives = (infos[0].optAssist ? 4 : 2) - (infos[seat].optAssist ? 4 : 2);
            for (uint32_t off : { PU_LIVES_BASE, PU_LIVES })
                if (lives) { int v; memcpy(&v, c.pu + off - PU_DATA, 4); v += lives; memcpy(c.pu + off - PU_DATA, &v, 4); }
        } else {
            memcpy(c.ships, (void*)(stg + STG_SHIPS), sizeof c.ships);
            for (int i = 0; i < 3; ++i)
                if (g_cfgShips[seat][i] >= 0) c.ships[i] = g_cfgShips[seat][i];
            memcpy(c.pu, (void*)(Va(VA_PU) + PU_DATA), sizeof c.pu);
            c.optAssist = At<uint8_t>(Va(VA_OPT_ASSIST));
            c.optXBomb = At<uint8_t>(Va(VA_OPT_XBOMB));
        }
        int m = c.ships[0], s = c.ships[1], u = c.ships[2];
        // 在这个座位的上下文里构造 PM。本地多人时先按这个人的自机重算升级（与开局时 0x614A01 的顺序相同；
        // 升级等级按自机分别存在存档里，所以用的是这几个自机自己的等级）
        SwitchTo(seat);
        uintptr_t pu = Va(VA_PU);
        if (!infos) {
            Fn<Arg1Fn>(VA_PU_RESET)(pu, 0);
            Fn<ThisFn>(VA_PU_LEVELS)(pu);
            Fn<Arg3Fn>(VA_PU_SHIPS)(pu, m, s, u);
            Fn<Arg2Fn>(VA_PU_COST)(pu, 0, 0);
        }
        uintptr_t mem = (uintptr_t)Fn<NewFn>(VA_OP_NEW)(0x520);
        uintptr_t pm = Fn<PmCtorFn>(VA_PM_CTOR)(mem, At<uint32_t>(stg + STG_FIELD_RECT), m, s, u, At<int>(stg + STG_DIFF), m);
        // 这个人的ボム名图：按他的副自机（和升级）再读一套，然后把 1P 的放回去
        Fn<Arg3Fn>(VA_LOAD_BOMB_NAMES)(eff, 0, s, 0);
        for (int i = 0; i < 5; ++i) {
            g_bombNames[seat][i] = At<uint32_t>(eff + BOMB_NAME_SLOTS[i]);
            At<uint32_t>(eff + BOMB_NAME_SLOTS[i]) = g_bombNames[0][i];
        }
        SwitchTo(0);
        g_ctx[seat].pm = g_pm[seat] = pm;
        Log("PM %dP %p (ships %d %d %d)", seat + 1, (void*)pm, m, s, u);
    }
    g_seats = seats;
    g_viewScore = 0;
    for (BombIcons& b : g_bombIcons) b.ready = false;
    ResetGestures();

    uintptr_t st1 = At<uintptr_t>(pm1);
    for (int seat = 0; seat < seats; ++seat) {
        uintptr_t st = StOf(seat);
        if (seat > 0 && prac) {
            At<int>(st + ST_POWER) = At<int>(st1 + ST_POWER);
            At<int>(st + ST_LIVES) = At<int>(st1 + ST_LIVES);
        }
        if (seat > 0 && st7 && At<int>(stg + STG_MODE) == 7) At<int>(st + ST_POWER) = At<int>(st1 + ST_POWER);
        // 左右错开：两人 ±32，三人 -48 / 0 / +48
        float step = seats == 2 ? 64.f : 48.f;
        PosOf(seat)[0] += ((float)seat - (float)(seats - 1) / 2) * step;
    }
    // 录像只记一个人的输入：多人局不录
    At<uint8_t>(Va(VA_REPLAY_RECORDING)) = 0;
    Log("%d players in STG %p (1P PM %p)", seats, (void*)stg, (void*)pm1);
}

void DestroyPms(uintptr_t stg)
{
    if (stg != g_stg || !g_seats) return;
    SwitchTo(0);   // 保险：内存里换回 1P
    for (int seat = 1; seat < g_seats; ++seat) {
        uintptr_t pm = g_pm[seat];
        g_pm[seat] = 0;
        g_ctx[seat].pm = 0;
        if (!pm) continue;
        Fn<ThisFn>(VA_PM_DTOR)(pm);
        Fn<DeleteFn>(VA_OP_DELETE)((void*)pm);
        Log("PM %dP %p destroyed", seat + 1, (void*)pm);
    }
    g_pm[0] = 0;
    g_seats = 0;
    g_stg = 0;
}

ThisFn g_origStgCtor, g_origStgDtor, g_origWorldUpdate;
DamageFn g_origDamage;
BossDamageFn g_origBossDamage;

// ------------------------------------------------------------------ 全场升级
// 有些升级 / 技能作用在整个关卡上：部队开始时抽副事件、生成杂兵（诅咒）、部队升级、中 Boss 的数量和符卡、Boss 的血量、续关时扣酒。
// 原版读的是内存里那个座位（平时 1P）的 Player_Upgrade。多人局里这几处读的地址改成 g_mg：在场的人（幽灵不算；都出局了就算所有人）
// 里效果最强的那一个，不叠加（概率类也一样，只判一次）；技能任何一人有就生效。各人按自己的"升级关闭"开关取值，技能的自机条件也按各人的
struct MergedPu {
    float subevt, bossHp;               // 副事件发生率（雀巳）、Boss 血量倍率（牌示）：越小越好
    float keep, curse, noLevelUp;       // 续关时保留的酒（黒巫鳥）、杂兵被诅咒的概率（蝶鬼）、部队不升级的概率（ゼノア）：越大越好
    uint8_t linking, cancel, tsuru, overwrite;   // リバースリンキング、キャンセルアロー、鶴の一声、オーバーライト
    uint8_t zero;                       // 原来读"升级关闭"的地方改读它（各人的开关已经算进上面的值里了）
};
MergedPu g_mg;
constexpr int SKILL_LINKING = 27, SKILL_CANCEL = 52, SKILL_TSURU = 59, SKILL_OVERWRITE = 61;   // 技能开关在 PU+4+编号
constexpr int SHIP_KUROJI = 9;          // リバースリンキング：主机或副机是黒巫鳥时才有效
constexpr uint32_t PU_SUBEVT = 0x2A0, PU_KEEP = 0x2C8, PU_CURSE = 0x2E0, PU_NO_LEVEL_UP = 0x2F0, PU_BOSS_HP = 0x300;   // 升级关闭时的值，+4 是平时的

struct SeatPuView { const uint8_t* pu; const int* ships; };   // 一个人的 Player_Upgrade（+4 起）和主 / 副 / 支援

void MergePu(const SeatPuView* v, int n)
{
    if (n <= 0) return;
    MergedPu m {};
    for (int i = 0; i < n; ++i) {
        const uint8_t* pu = v[i].pu;
        bool on = pu[PU_OFF - PU_DATA] == 0;
        auto f = [&](uint32_t off) { float x; memcpy(&x, pu + off + (on ? 4 : 0) - PU_DATA, 4); return x; };
        auto skill = [&](int k) { return on && pu[4 + k - PU_DATA] != 0; };
        float subevt = f(PU_SUBEVT), bossHp = f(PU_BOSS_HP), keep = f(PU_KEEP), curse = f(PU_CURSE), noLevelUp = f(PU_NO_LEVEL_UP);
        if (!i || subevt < m.subevt) m.subevt = subevt;
        if (!i || bossHp < m.bossHp) m.bossHp = bossHp;
        if (!i || keep > m.keep) m.keep = keep;
        if (!i || curse > m.curse) m.curse = curse;
        if (!i || noLevelUp > m.noLevelUp) m.noLevelUp = noLevelUp;
        m.linking |= skill(SKILL_LINKING) && (v[i].ships[0] == SHIP_KUROJI || v[i].ships[1] == SHIP_KUROJI);
        m.cancel |= skill(SKILL_CANCEL);
        m.tsuru |= skill(SKILL_TSURU);
        m.overwrite |= skill(SKILL_OVERWRITE);
    }
    if (g_debug && memcmp(&m, &g_mg, sizeof m))
        Log("stage upgrades (%d): sub-event %.3f boss HP %.3f keep %.3f curse %.3f no-level-up %.3f, skills %d%d%d%d", n, m.subevt, m.bossHp,
            m.keep, m.curse, m.noLevelUp, m.linking, m.cancel, m.tsuru, m.overwrite);
    g_mg = m;
}

// 局中：在场的人（都出局了就算所有还在局里的人）
void UpdateMergedPu()
{
    if (!Coop()) return;
    SeatPuView v[MAX_SEATS];
    int n = 0;
    for (int pass = 0; pass < 2 && !n; ++pass)
        for (int s = 0; s < g_seats; ++s) {
            if (pass == 0 ? !Alive(s) : g_gone[s]) continue;
            bool mem = s == g_mem;
            v[n++] = { mem ? (const uint8_t*)Va(VA_PU) + PU_DATA : g_ctx[s].pu, mem ? (const int*)(g_stg + STG_SHIPS) : g_ctx[s].ships };
        }
    MergePu(v, n);
}

// 开局（STG 构造之前：第一格的部队就在构造函数里生成）。联机：各人的开局状态；本地：存档是同一份，只有自机不同
void MergeAtStart(const NetStartInfo* infos, int seats)
{
    SeatPuView v[MAX_SEATS];
    int ships[MAX_SEATS][3];
    const int* ships0 = (const int*)Va(VA_STG_SET_SHIPS);
    for (int s = 0; s < seats; ++s) {
        if (infos) { v[s] = { infos[s].pu, infos[s].ships }; continue; }
        for (int i = 0; i < 3; ++i) ships[s][i] = s && g_cfgShips[s][i] >= 0 ? g_cfgShips[s][i] : ships0[i];
        v[s] = { (const uint8_t*)Va(VA_PU) + PU_DATA, ships[s] };
    }
    MergePu(v, seats);
}

// 读这些值的指令里的地址：多人局时换成 g_mg 的
struct AddrPatch { uint32_t va; uint8_t at; uint32_t orig; const void* repl; };
const AddrPatch MERGE_PATCHES[] = {
    { 0x641AB3, 4, 0x2FB0510, &g_mg.subevt }, { 0x641ABD, 4, 0x2FB0514, &g_mg.subevt },        // 部队开始：抽副事件
    { 0x447DBD, 4, 0x2FB0570, &g_mg.bossHp }, { 0x447DC7, 4, 0x2FB0574, &g_mg.bossHp },        // Boss 的血量（两处）
    { 0x447FBC, 4, 0x2FB0570, &g_mg.bossHp }, { 0x447FC6, 4, 0x2FB0574, &g_mg.bossHp },
    { 0x63C73F, 4, 0x2FB0538, &g_mg.keep }, { 0x63C749, 4, 0x2FB053C, &g_mg.keep },            // 续关：本局的酒 × 它
    { 0x5D3F4D, 4, 0x2FB0550, &g_mg.curse }, { 0x5D3F57, 4, 0x2FB0554, &g_mg.curse },          // 读敌人表：诅咒
    { 0x6409EA, 4, 0x2FB0560, &g_mg.noLevelUp }, { 0x6409F4, 4, 0x2FB0564, &g_mg.noLevelUp },  // 部队开始：部队升级
    { 0x641EEB, 2, 0x2FB03D9, &g_mg.zero }, { 0x641EF8, 2, 0x2FB02A8, &g_mg.cancel },          // 部队开始：抵消副事件
    { 0x642119, 2, 0x2FB03D9, &g_mg.zero }, { 0x642126, 2, 0x2FB028F, &g_mg.linking },         //   副事件换成酒の亡者
    { 0x63DF46, 2, 0x2FB03D9, &g_mg.zero }, { 0x63DF4F, 2, 0x2FB02AF, &g_mg.tsuru },           // 生成地图：中 Boss 的数量
    { 0x637033, 2, 0x2FB03D9, &g_mg.zero }, { 0x637040, 2, 0x2FB02B1, &g_mg.overwrite },       // 中 Boss 用不用符卡
};
// リバースリンキング的自机条件（主机或副机是黒巫鳥）已经按各人算进 g_mg.linking：cmp [esi+38h],9 换成 jmp 0x642143
constexpr uint32_t PATCH_LINKING_SHIP = 0x642133;
bool g_mergePatched = false;

void SetMergePatches(bool on)
{
    if (on == g_mergePatched) return;
    for (const AddrPatch& p : MERGE_PATCHES) {
        uintptr_t at = Va(p.va) + p.at;
        uint32_t orig = (uint32_t)Va(p.orig), repl = (uint32_t)(uintptr_t)p.repl, cur = At<uint32_t>(at);
        uint32_t want = on ? repl : orig;
        if (cur != (on ? orig : repl)) { Log("merge patch %X: unexpected %08X", p.va, cur); continue; }
        WriteCode(at, (const uint8_t*)&want, 4);
    }
    uintptr_t ship = Va(PATCH_LINKING_SHIP);
    if (BytesAt(ship, Hex(on ? "837E" : "EB0E"))) WriteCode(ship, Hex(on ? "EB0E" : "837E").data(), 2);
    else Log("merge patch %X: unexpected bytes", PATCH_LINKING_SHIP);
    g_mergePatched = on;
}

// ------------------------------------------------------------------ 联机开局
NetStartInfo CaptureStart()
{
    NetStartInfo s {};
    if (const char* e = g_debug ? getenv("LE04MULTI_SHIPS") : nullptr) {   // 调试：本机的自机（联机测试用）
        int* ships = (int*)Va(VA_STG_SET_SHIPS);
        sscanf_s(e, "%d,%d,%d", &ships[0], &ships[1], &ships[2]);
    }
    memcpy(s.stgSet, (void*)(Va(VA_STG_SET) + 4), sizeof s.stgSet);
    memcpy(s.pu, (void*)(Va(VA_PU) + PU_DATA), sizeof s.pu);
    s.optAssist = At<uint8_t>(Va(VA_OPT_ASSIST));
    s.optXBomb = At<uint8_t>(Va(VA_OPT_XBOMB));
    memcpy(s.unlocks, (void*)Va(VA_UNLOCKS), sizeof s.unlocks);
    s.flag7c = At<uint8_t>(Va(VA_FLAG_7C));
    s.flag7d = At<uint8_t>(Va(VA_FLAG_7D));
    memcpy(s.ships, (void*)Va(VA_STG_SET_SHIPS), sizeof s.ships);
    s.rules = (uint8_t)g_cfgRules;
    return s;
}

void ApplyStart(const NetStartInfo& s)
{
    memcpy((void*)(Va(VA_STG_SET) + 4), s.stgSet, sizeof s.stgSet);
    memcpy((void*)(Va(VA_PU) + PU_DATA), s.pu, sizeof s.pu);
    At<uint8_t>(Va(VA_OPT_ASSIST)) = s.optAssist;
    At<uint8_t>(Va(VA_OPT_XBOMB)) = s.optXBomb;
    memcpy((void*)Va(VA_UNLOCKS), s.unlocks, sizeof s.unlocks);
    At<uint8_t>(Va(VA_FLAG_7C)) = s.flag7c;
    At<uint8_t>(Va(VA_FLAG_7D)) = s.flag7d;
}

// 所有人从同样的状态开始：清掉菜单遗留的特效 / 道具，按键计数归零
void PrepareNetStart()
{
    uintptr_t eff = ((uintptr_t(__cdecl*)())Va(VA_EFFECT_MAIN_GET))();
    if (eff) Fn<ThisFn>(VA_EFFECT_CLEAR)(eff);
    Fn<ThisFn>(VA_ITEM_CLEAR)(Va(VA_ITEM_MAIN));
    memset(g_cnt, 0, sizeof g_cnt);
    memset((void*)Va(VA_HOLD), 0, 16 * sizeof(int));
}

// 客机联机局里游戏写存档时（练习结束、游戏结束等），内存里的选项 / 解锁是主机的：
// 写之前换回客机自己的，关文件后再换回主机的，客机的存档不受影响
using FopenFn = int(__cdecl*)(void**, const char*, const char*);
using FcloseFn = int(__cdecl*)(void*);
FopenFn g_origFopen;
FcloseFn g_origFclose;
void* g_saveFile = nullptr;
NetStartInfo g_hostSaveBytes;

void PutSaveBytes(const NetStartInfo& s)
{
    At<uint8_t>(Va(VA_OPT_ASSIST)) = s.optAssist;
    At<uint8_t>(Va(VA_OPT_XBOMB)) = s.optXBomb;
    memcpy((void*)Va(VA_UNLOCKS), s.unlocks, sizeof s.unlocks);
    At<uint8_t>(Va(VA_FLAG_7C)) = s.flag7c;
    At<uint8_t>(Va(VA_FLAG_7D)) = s.flag7d;
}

int __cdecl FopenHook(void** pf, const char* name, const char* mode)
{
    int r = g_origFopen(pf, name, mode);
    if (!r && pf && *pf && g_haveGuestOwn && !g_saveFile && mode && mode[0] == 'w' && name && !strncmp(name, "save_dat", 8)) {
        g_hostSaveBytes = CaptureStart();
        PutSaveBytes(g_guestOwn);
        g_saveFile = *pf;
        Log("guest save: wrote own options (%s)", name);
    }
    return r;
}

int __cdecl FcloseHook(void* f)
{
    int r = g_origFclose(f);
    if (f && f == g_saveFile) {
        PutSaveBytes(g_hostSaveBytes);
        g_saveFile = nullptr;
    }
    return r;
}

uintptr_t __fastcall StgCtorHook(uintptr_t stg, void*)
{
    if (g_debug) { Log("handles before STG ctor: %d", RbDebugHandleCount()); RbDebugHandles(); }
    static NetStartInfo infos[MAX_SEATS];
    int seats = 0;
    bool net = false;
    if (g_role == NET_HOST || g_role == NET_GUEST) {
        NetStartInfo mine = CaptureStart();
        net = NetBarrier(mine, infos, &seats);
        if (net) {
            if (g_role == NET_GUEST) {   // 客机：内存换成主机的开局状态（局终还原）
                g_guestOwn = mine;
                g_haveGuestOwn = true;
                ApplyStart(infos[0]);
            }
            PrepareNetStart();
        }
    }
    // 全场升级：第一格的部队、地图在构造函数里就生成了，构造之前先合并好
    bool multi = net ? seats >= 2 : g_role == NET_LOCAL;
    if (multi) MergeAtStart(net ? infos : nullptr, net ? seats : g_cfgSeats);
    SetMergePatches(multi);
    RbCollectGameHandles(true);   // 这一局读的图像、字体等，局终删掉（原版漏删）
    g_origStgCtor(stg);
    g_rules = net ? infos[0].rules : g_cfgRules;
    g_aimNearest = (g_rules & RULE_AIM_NEAREST) != 0;
    g_netGame = net;
    g_netSolo = false;
    g_mySeat = net ? NetSeat() : 0;
    if (g_role == NET_LOCAL) CreatePms(stg, nullptr, g_cfgSeats);
    else if (net) CreatePms(stg, infos, seats);
    RbCollectGameHandles(false);
    return stg;
}

void __fastcall StgDtorHook(uintptr_t stg, void*)
{
    if (RbActive()) RbEnd();   // 回滚到此为止（析构里删的图像等立即删）
    if (g_debug) DebugMemory("at game end");
    g_rbFallback = false;
    DestroyPms(stg);
    if (g_netGame) NetEndGame();
    g_netGame = false;
    if (g_haveGuestOwn) {   // 在原析构之前还原：它末尾会按 STG_set 的自机重算升级
        ApplyStart(g_guestOwn);
        g_haveGuestOwn = false;
    }
    g_origStgDtor(stg);
    int freed = RbFreeGameHandles();
    if (g_debug) Log("game end: freed %d handles the game left behind, %d live", freed, RbDebugHandleCount());
    SetMergePatches(false);
}

// ------------------------------------------------------------------ 世界更新
// 1P 出局而别人还在：整个世界更新期间让第一个还在场的人当"内存里的座位"
void __fastcall WorldUpdateHook(uintptr_t stg, void*)
{
    int seat = 0;
    if (Coop() && stg == g_stg && g_mem == 0 && Out(0))
        for (int s = 1; s < g_seats && !seat; ++s)
            if (Alive(s)) seat = s;
    if (seat) SwitchTo(seat);
    if (stg == g_stg) UpdateMergedPu();
    static int s_forceMod = g_debug && getenv("LE04MULTI_FORCEMOD") ? (int)strtol(getenv("LE04MULTI_FORCEMOD"), nullptr, 16) : -1;
    if (s_forceMod >= 0) At<int>(stg + STG_MODIFIER_SLOTS) = s_forceMod;   // 调试：关卡修饰第一格固定（各台在同一帧写，联机也一致）
    g_origWorldUpdate(stg);
    if (seat && Coop()) SwitchTo(0);
}

// 进下一关（地图、第一格的部队在这里生成）：先按现在在场的人重新合并全场升级（过关时复活的幽灵也算上）
void __fastcall StageNextHook(uintptr_t stg, void*)
{
    if (stg == g_stg) UpdateMergedPu();
    Fn<ThisFn>(VA_STAGE_NEXT)(stg);
}

// 进路ゲージ（规则可选）：各人的偏移相加（向同一边时走得快，向两边时互相抵消）；关掉时只看内存里那个座位（平时是 1P）
void __fastcall RouteHook(uintptr_t stg, void*)
{
    Fn<ThisFn>(VA_ROUTE)(stg);
    if (stg == g_stg && (g_rules & RULE_ROUTE_ALL)) ForOthers([&](uintptr_t) { Fn<ThisFn>(VA_ROUTE)(stg); });
}

// 敌人更新：每个敌人的行动函数之前把 M+0x2CC 换成离它最近的自机（EnemyPatternStub），全部更新完放回原值
uintptr_t g_enemyAim = 0;

void __fastcall EnemyUpdateHook(uintptr_t m, void*)
{
    g_enemyAim = At<uintptr_t>(m + M_AIM);
    Fn<ThisFn>(VA_ENEMY_UPDATE)(m);
    At<uintptr_t>(m + M_AIM) = g_enemyAim;
}

// 部分符牌多人化：这一帧复制出来的轮盘光标和它们各自的瞄准点（SpellBegin）
struct RouletteClone { uintptr_t obj, aim; };
constexpr int ROULETTE_CLONES = 8;
RouletteClone g_rc[ROULETTE_CLONES];
int g_rcN = 0;
void SpellReset();

// Boss：瞄准离 Boss 最近的自机（Boss 的位置是 [[B]]，和敌人一样）
void __fastcall BossUpdateHook(uintptr_t boss, void*)
{
    g_rcN = 0;
    SpellReset();
    if (Coop() && boss) {
        uintptr_t rec = At<uintptr_t>(boss);
        const float* pos = rec ? At<const float*>(rec) : nullptr;
        At<uintptr_t>(boss + B_AIM) = NearestAim(pos, At<uintptr_t>(boss + B_AIM));
    }
    Fn<ThisFn>(VA_BOSS_UPDATE)(boss);
}

// ------------------------------------------------------------------ 残机 / 火力转移
// 残机：靠近队友（24 像素内）或队友的幽灵（32 像素内），不按射击、按住低速 1.5 秒 → 给对方 1 个残机 / 复活幽灵。
//   范围内有几个人时：幽灵优先，其次残机少的，再其次座位小的。
// 火力：靠近队友，5 次快速点按射击（间隔不超过 24 帧）→ 给对方 1.00 火力（火力少的优先）。
// 所有人按同样的按键和状态判定，联机时结果一致
struct Give {
    int hold = 0;          // 满足条件地按住低速的帧数
    bool used = false;     // 这次按住已经给过（松开低速才能再给）
    int taps = 0, lastTap = -1000;
};
Give g_give[MAX_SEATS];
int g_coopFrame = 0;
int g_calm = 0;                    // 连续正常游戏（非对话 / 暂停 / 加载）的帧数
struct ReviveFix { bool pending; int score, x5c, xa8, xac; };
ReviveFix g_reviveFix[MAX_SEATS];
uint8_t g_prevStageClear = 0;

int HoldOf(int seat, int slot)
{
    int idx = ((const int*)Va(VA_SLOT_MAP))[slot];
    if (idx < 0 || idx >= 16) return 0;
    return seat == g_mem ? ((const int*)Va(VA_HOLD))[idx] : g_ctx[seat].hold[idx];
}

int& StInt(int seat, uint32_t off) { return At<int>(StOf(seat) + off); }

bool CanGive(int seat)
{
    if (!Alive(seat) || At<uint8_t>(g_pm[seat] + PM_LOCK)) return false;
    int st = StInt(seat, ST_STATE);
    return st == 0 || st == 4;
}

void PlayExtendSe()
{
    if (!(At<uint32_t>(Va(VA_SE_GUARD)) & 1)) return;
    At<uint8_t>(Va(VA_SE_EXTEND)) = 1;
    At<int>(Va(VA_SE_EXTEND) + 0xC) = 0xFF;
}

// 幽灵复活：清掉出局标志，PM 的状态 8 接着做续关时的复活处理（残机、HP、屏障、ボム、火力重置，分数清零，自动放ボム）；
// 处理完之后（ApplyReviveFix）改成：残机 0（就是给的那一个）、分数等保留、火力为满的一半
void Revive(int seat)
{
    g_reviveFix[seat] = { true, StInt(seat, ST_SCORE), StInt(seat, 0x5C), StInt(seat, 0xA8), StInt(seat, 0xAC) };
    At<uint8_t>(g_pm[seat] + PM_OUT) = 0;
    PlayExtendSe();
    Log("revive %dP", seat + 1);
}

void ApplyReviveFix()
{
    for (int seat = 0; seat < g_seats; ++seat) {
        ReviveFix& f = g_reviveFix[seat];
        if (!f.pending || Out(seat) || StInt(seat, ST_STATE) == 8) continue;
        f.pending = false;
        StInt(seat, ST_LIVES) = 0;
        StInt(seat, ST_SCORE) = f.score;
        StInt(seat, 0x5C) = f.x5c;
        StInt(seat, 0xA8) = f.xa8;
        StInt(seat, 0xAC) = f.xac;
        StInt(seat, ST_POWER) = POWER_MAX / 2;
        Log("revive %dP: done (state %d)", seat + 1, StInt(seat, ST_STATE));
    }
}

void ResetGestures()
{
    for (Give& g : g_give) g = Give {};
    g_coopFrame = g_calm = 0;
    g_prevStageClear = 0;
    for (ReviveFix& f : g_reviveFix) f.pending = false;
}

// 幽灵的位置：场地中线两边按座位排开（间隔 48），离下边 48，上下 ±3 像素浮动（三角波，只用整数和精确的浮点运算）
void PlaceGhosts()
{
    if (!AliveCount()) return;
    float w = At<float>(g_stg + STG_FIELD_W), h = At<float>(g_stg + STG_FIELD_H);
    int t = g_coopFrame % 120;
    float bob = (float)((t < 60 ? t : 120 - t) - 30) / 10.0f;
    for (int seat = 0; seat < g_seats; ++seat) {
        float* p = Ghost(seat) ? PosOf(seat) : nullptr;
        if (!p) continue;
        p[0] = w / 2 + ((float)seat - (float)(g_seats - 1) / 2) * GHOST_SPREAD;
        p[1] = h - GHOST_FROM_BOTTOM + bob;
    }
}

// g 给残机时的接收方：幽灵 > 残机少的 > 座位小的；没有时 -1
int LifeReceiver(int g)
{
    int best = -1;
    for (int r = 0; r < g_seats; ++r) {
        if (r == g) continue;
        bool ghost = Ghost(r);
        float range = ghost ? GHOST_RANGE : GIVE_RANGE;
        if (!(ghost || Alive(r)) || Dist2(PosOf(g), PosOf(r)) > range * range) continue;
        if (best < 0) { best = r; continue; }
        bool bestGhost = Ghost(best);
        if (ghost != bestGhost) { if (ghost) best = r; continue; }
        if (!ghost && StInt(r, ST_LIVES) < StInt(best, ST_LIVES)) best = r;
    }
    return best;
}

// g 给火力时的接收方：火力不满、火力少的 > 座位小的
int PowerReceiver(int g)
{
    int best = -1;
    for (int r = 0; r < g_seats; ++r) {
        if (r == g || !Alive(r) || StInt(r, ST_POWER) >= POWER_MAX) continue;
        if (Dist2(PosOf(g), PosOf(r)) > GIVE_RANGE * GIVE_RANGE) continue;
        if (best < 0 || StInt(r, ST_POWER) < StInt(best, ST_POWER)) best = r;
    }
    return best;
}

void CoopGestures()
{
    ++g_coopFrame;
    uintptr_t stg = g_stg;
    PlaceGhosts();
    // 过关（进下一关）时幽灵复活
    uint8_t clear = At<uint8_t>(stg + STG_STAGE_CLEAR);
    if (clear && !g_prevStageClear && AliveCount())
        for (int seat = 0; seat < g_seats; ++seat)
            if (Ghost(seat)) { Revive(seat); Log("stage clear: %dP revived", seat + 1); }
    g_prevStageClear = clear;
    // 对话 / 暂停 / 加载之后 1 秒内不算（连按射击翻完对话时不会误给火力）
    if (g_netSolo || At<int>(stg + STG_STATE) != 2 || At<uint8_t>(stg + STG_PAUSED) || At<uint8_t>(Va(VA_LOADING)))
        g_calm = 0;
    else
        ++g_calm;
    if (g_calm < 60) {
        for (Give& g : g_give) g.hold = g.taps = 0;
        return;
    }
    for (int g = 0; g < g_seats; ++g) {
        Give& gv = g_give[g];
        if (!CanGive(g)) { gv.hold = gv.taps = 0; continue; }
        bool focus = HoldOf(g, SLOT_FOCUS) > 0;
        int shot = HoldOf(g, SLOT_SHOT);
        // 残机
        if (!focus) gv.used = false;
        int lifeTo = focus && !shot && !gv.used && StInt(g, ST_LIVES) >= 1 ? LifeReceiver(g) : -1;
        if (lifeTo >= 0) {
            if (++gv.hold >= LIFE_HOLD) {
                gv.hold = 0;
                gv.used = true;
                StInt(g, ST_LIVES) -= 1;
                if (Ghost(lifeTo))
                    Revive(lifeTo);
                else
                    Fn<ExtendFn>(VA_EXTEND)(g_pm[lifeTo], 0);   // 残机 +1（带エクステンド的横幅和音效）
                Log("life: %dP -> %dP", g + 1, lifeTo + 1);
            }
        } else {
            gv.hold = 0;
        }
        // 火力
        if (shot == 1) {
            int powerTo = StInt(g, ST_POWER) >= POWER_GIVE ? PowerReceiver(g) : -1;
            if (powerTo >= 0) {
                gv.taps = g_coopFrame - gv.lastTap <= TAP_GAP ? gv.taps + 1 : 1;
                gv.lastTap = g_coopFrame;
                if (gv.taps >= TAP_COUNT) {
                    gv.taps = 0;
                    int amount = POWER_MAX - StInt(powerTo, ST_POWER);
                    if (amount > POWER_GIVE) amount = POWER_GIVE;
                    StInt(g, ST_POWER) -= amount;
                    StInt(powerTo, ST_POWER) += amount;
                    Log("power: %dP -> %dP %d", g + 1, powerTo + 1, amount);
                }
            } else {
                gv.taps = 0;
            }
        } else if (g_coopFrame - gv.lastTap > TAP_GAP) {
            gv.taps = 0;
        }
    }
}

// ボム名横幅的图在放ボム（Effect_main::Spawn 0x40E0C0）那一刻从 Effect_main 那几格复制进特效：
// 所以每人的 PM 更新（放ボム都在里面）之前，先把那几格换成这个人的
void UseBombNames(int seat)
{
    uintptr_t eff = Va(VA_EFFECT_MAIN);
    for (int i = 0; i < 5; ++i) At<uint32_t>(eff + BOMB_NAME_SLOTS[i]) = g_bombNames[seat][i];
}

void __fastcall PmUpdateHook(uintptr_t pm, void*)
{
    bool coop = Coop() && !g_window && pm == g_pm[g_mem];
    if (coop) UseBombNames(g_mem);
    Fn<ThisFn>(VA_PM_UPDATE)(pm);
    if (!coop) return;
    uintptr_t stg = g_stg;
    int base = g_mem;
    uintptr_t st = At<uintptr_t>(pm);
    // 世界更新在调用 PM 的更新之前往 PM 里写的字段，照抄给其他人
    for (int s = 0; s < g_seats; ++s) {
        if (s == base) continue;
        uintptr_t other = g_pm[s], sto = At<uintptr_t>(other);
        At<uint8_t>(other + PM_LOCK) = At<uint8_t>(pm + PM_LOCK);   // 操作锁定
        At<int>(other + 0x488) = At<int>(pm + 0x488);               // GAME_state
        At<uint8_t>(other + 0x4A8) = At<uint8_t>(pm + 0x4A8);
        At<uint8_t>(other + 0x4A2) = At<uint8_t>(pm + 0x4A2);       // Boss 的移动限制
        At<uint8_t>(other + 0x4A1) = At<uint8_t>(pm + 0x4A1);       // 镜像
        At<uint8_t>(sto + 0x34D) = At<uint8_t>(st + 0x34D);         // 关卡修饰：移动速度 UP / DOWN
        At<uint8_t>(sto + 0x34E) = At<uint8_t>(st + 0x34E);
        memcpy((void*)(other + PM_MODIFIER_COPY), (void*)(pm + PM_MODIFIER_COPY), 16);   // 关卡修饰在 PM 里的那份
    }
    int state = At<int>(stg + STG_STATE);
    bool modifiers = state != 3 && state != 5 && !At<uint8_t>(stg + STG_PAUSED) && !At<uint8_t>(Va(VA_LOADING));
    ForOthers([&](uintptr_t pmo) {
        if (modifiers) {   // 只让作用在自机身上的那几种再跑一遍
            int saved[4];
            int* slots = (int*)(stg + STG_MODIFIER_SLOTS);
            memcpy(saved, slots, sizeof saved);
            for (int i = 0; i < 4; ++i)
                if (!PerPlayerModifier(slots[i])) slots[i] = -1;
            TagDarkness(base);    // 这一帧前面放的视界不良是内存里那个座位的
            Fn<ThisFn>(VA_MODIFIERS)(stg);
            TagDarkness(g_mem);   // 刚放的是这个座位的
            memcpy(slots, saved, sizeof saved);
        }
        Fn<ThisFn>(VA_NEAREST)(stg);
        UseBombNames(g_mem);
        Fn<ThisFn>(VA_PM_UPDATE)(pmo);
    });
    UseBombNames(base);
    // 符卡奖励：任何一人被弹都算失败（世界更新稍后只锁存内存里那个座位的）
    if (At<int>(stg + STG_BATTLE) == 1 && !At<uint8_t>(stg + STG_PAUSED)) {
        uintptr_t boss = At<uintptr_t>(stg + 8);
        for (int s = 0; s < g_seats; ++s)
            if (s != base && Alive(s) && boss && !At<uint8_t>(boss + B_SPELL_FAILED) && At<uint8_t>(StOf(s) + 0xB0))
                At<uint8_t>(boss + B_SPELL_FAILED) = 1;
    }
    ApplyReviveFix();
    CoopGestures();
}

void __fastcall CollisionHook(uintptr_t stg, void*)
{
    Fn<ThisFn>(VA_COLLISION)(stg);
    if (stg == g_stg) ForOthers([&](uintptr_t) { Fn<ThisFn>(VA_COLLISION)(stg); });
}

// 第二遍以后的碰撞里，低速吸引只能"加上"吸引状态，不能把前面别人吸住的道具放掉
void __fastcall ItemAttractHook(uintptr_t items, void*, int index, int attract)
{
    if (g_window && !(attract & 0xFF)) return;
    Fn<Arg2Fn>(VA_ITEM_ATTRACT)(items, index, attract);
}

// 自动回收线：其他人按自己升级决定的线高也能触发
void __fastcall ItemUpdateHook(uintptr_t items, void*)
{
    for (int s = 0; Coop() && s < g_seats; ++s) {
        if (s == g_mem || !Alive(s)) continue;
        float line = SeatPu<float>(s, SeatPu<uint8_t>(s, PU_OFF) ? PU_LINE_ON : PU_LINE_OFF) * At<float>(Va(VA_COLLECT_SCALE));
        float* p = PosOf(s);
        if (p && line > p[1]) {
            auto begin = At<uintptr_t*>(items + 4), end = At<uintptr_t*>(items + 8);
            int last = At<int>(items + 0x20);
            for (int i = 0; i <= last && begin + i < end; ++i) At<int>(begin[i] + 0x38) = 2;
        }
    }
    Fn<ThisFn>(VA_ITEM_UPDATE)(items);
}

// 0x652A6D：内存里的座位出局时，还有别人在场就不算游戏结束
uint8_t __cdecl OtherAlive() { return Coop() && AnyAliveExcept(g_mem) ? 1 : 0; }

__declspec(naked) void GameOverStub()
{
    __asm {
        mov al, byte ptr [edx + 0x49A]
        test al, al
        jz done
        pushad
        call OtherAlive
        xor al, 1
        mov g_gameOver, al
        popad
        mov al, g_gameOver
    done:
        ret
    }
}

// 续关菜单：续关清掉 1P 的出局标志时，其他出局的人也一起复活（PM 的状态 8 会接着做复活处理）
void __fastcall ContinueMenuHook(uintptr_t stg, void*)
{
    if (stg == g_stg) UpdateMergedPu();   // 续关时扣酒（全场升级）
    bool wasOut = Coop() && stg == g_stg && Out(0);
    Fn<ThisFn>(VA_CONTINUE_MENU)(stg);
    if (!wasOut || !Coop() || Out(0)) return;
    for (int s = 1; s < g_seats; ++s)
        if (Ghost(s)) {
            At<uint8_t>(g_pm[s] + PM_OUT) = 0;
            Log("continue: %dP revived", s + 1);
        }
}

// エクステンド：游戏只给 [STG] 的自机加残机，其他在场的人也加；幽灵改成复活
void __fastcall ExtendHook(uintptr_t pm, void*, int arg)
{
    Fn<ExtendFn>(VA_EXTEND)(pm, arg);
    if (!Coop() || g_window || pm != g_pm[g_mem]) return;
    for (int s = 0; s < g_seats; ++s) {
        if (s == g_mem) continue;
        if (Alive(s)) Fn<ExtendFn>(VA_EXTEND)(g_pm[s], arg);
        else if (Ghost(s)) { Revive(s); Log("extend: %dP revived", s + 1); }
    }
}

bool __fastcall DamageHook(uintptr_t pm, void*, int dmg, int a, int b)
{
    int seat = 0;
    for (int s = 1; s < g_seats; ++s)
        if (Coop() && pm == g_pm[s]) seat = s;
    if (g_god[seat]) return false;
    return g_origDamage(pm, dmg, a, b);
}

int __fastcall BossDamageHook(uintptr_t boss, void*, int dmg, int a, int b)
{
    int n = Coop() ? AliveCount() : 1;
    if (n == 2) dmg = (int)(dmg * BOSS_DAMAGE_2P);
    else if (n >= 3) dmg = dmg * 2 / 3;
    return g_origBossDamage(boss, dmg, a, b);
}

// ------------------------------------------------------------------ 视界不良：各人的视野取并集
// 每人的黑层先画进各自的离屏画面（白底上叠黑图 = 亮度 B），反相成"暗度" D，几张相乘得 ΠD，再反相得
// B = 1 - ΠD（任何一人周围亮的地方都亮），最后用乘法混合盖到场地上。只是绘制，不影响模拟
int g_darkRt[MAX_SEATS] = { -1, -1, -1 };
uintptr_t g_darkStg = 0;          // 离屏画面是哪一局建的（每局重建：上一局的句柄可能已经失效）
int g_darkW = 0, g_darkH = 0;
constexpr uint32_t VA_CUR_DRAW_SCREEN = 0xB95038;   // DxLib 当前的绘制目标

// 视界不良特效归谁：生成时打的标记（座位 + 1，见 TagDarkness）；没有标记或那人已出局时，归离它最近的在场的人
// （特效生成在自机的位置上，之后不动）
int DarknessOwner(uintptr_t e)
{
    int tag = At<int>(e + EFF_TAG);
    if (tag >= 1 && tag <= g_seats && Alive(tag - 1)) return tag - 1;
    int best = -1;
    float bd = 0, ex = At<float>(e + EFF_X), ey = At<float>(e + EFF_Y);
    for (int s = 0; s < g_seats; ++s) {
        float* p = Alive(s) ? PosOf(s) : nullptr;
        if (!p) continue;
        float dx = p[0] - ex, dy = p[1] - ey, d = dx * dx + dy * dy;
        if (best < 0 || d < bd) { best = s; bd = d; }
    }
    return best;
}

bool DrawDarknessUnion(uintptr_t em)
{
    std::vector<uintptr_t> mine[MAX_SEATS];
    ForEachDarkness([&](uintptr_t e) {
        int o = DarknessOwner(e);
        if (o >= 0) mine[o].push_back(e);
    });
    int groups[MAX_SEATS], n = 0;
    for (int s = 0; s < MAX_SEATS; ++s)
        if (!mine[s].empty()) groups[n++] = s;
    if (n < 2) {
        static uintptr_t s_loggedStg;
        if (s_loggedStg != g_stg && n == 1) {   // 每局记一次
            s_loggedStg = g_stg;
            Log("darkness: only %dP's layers (%d), drawing own view", groups[0] + 1, (int)mine[groups[0]].size());
        }
        return false;
    }
    int w = (int)At<float>(g_stg + STG_FIELD_W), h = (int)At<float>(g_stg + STG_FIELD_H);
    int fx = (int)At<float>(g_stg + STG_FIELD_X), fy = (int)At<float>(g_stg + STG_FIELD_Y);
    if (g_darkStg != g_stg || g_darkW != w || g_darkH != h) {   // 每局（或场地大小变了）重建
        for (int& rt : g_darkRt) {
            if (rt >= 0) Fn<Int2Fn>(VA_DELETE_GRAPH)(rt, 0);
            rt = -1;
        }
        g_darkStg = g_stg;
        g_darkW = w;
        g_darkH = h;
    }
    for (int i = 0; i < n; ++i) {
        int& rt = g_darkRt[i];
        if (rt < 0) rt = Fn<MakeScreenFn>(VA_MAKE_SCREEN)(w, h, 0);
        if (rt < 0) {
            static bool s_logged;
            if (!s_logged) { s_logged = true; Log("darkness: MakeScreen(%d, %d) failed", w, h); }
            return false;
        }
    }
    auto setScreen = Fn<Int1Fn>(VA_SET_DRAW_SCREEN);
    auto blend = Fn<Int2Fn>(VA_SET_BLEND);
    auto box = Fn<DrawBoxFn>(VA_DRAW_BOX);
    unsigned white = Fn<GetColorFn>(VA_GET_COLOR)(255, 255, 255);
    int graph = At<int>(em + EM_DARK_GRAPH);
    int prevScreen = At<int>(Va(VA_CUR_DRAW_SCREEN));   // 画完换回原来的绘制目标
    for (int i = 0; i < n; ++i) {
        setScreen(g_darkRt[i]);
        blend(BLEND_NONE, 0);
        box(0, 0, w, h, white, 1);
        blend(BLEND_ALPHA, 255);
        // 每层画两遍：原来的大小（黑色盖满整个场地，同原版），再缩小一点（洞小一点）。只画缩小的那遍的话，
        // 黑图本身也跟着变小，人在场地边上时盖不到另一头，那边会露出一大片亮的
        for (uintptr_t e : mine[groups[i]]) {
            float ex = At<float>(e + EFF_X), ey = At<float>(e + EFF_Y), scale = At<float>(e + EFF_SCALE);
            double angle = At<double>(e + EFF_ANGLE);
            Fn<DrawRotaFFn>(VA_DRAW_ROTA_F)(ex, ey, scale, angle, graph, 1, 0);
            Fn<DrawRotaFFn>(VA_DRAW_ROTA_F)(ex, ey, scale * DARK_SHRINK, angle, graph, 1, 0);
        }
        blend(BLEND_INVDEST, 255);
        box(0, 0, w, h, white, 1);
    }
    setScreen(g_darkRt[0]);
    blend(BLEND_MUL, 255);
    for (int i = 1; i < n; ++i) Fn<DrawGraphFn>(VA_DRAW_GRAPH)(0, 0, g_darkRt[i], 0);
    blend(BLEND_INVDEST, 255);
    box(0, 0, w, h, white, 1);
    setScreen(prevScreen);
    blend(BLEND_MUL, 255);
    Fn<DrawGraphFn>(VA_DRAW_GRAPH)(fx, fy, g_darkRt[0], 0);
    blend(BLEND_NONE, 0);
    return true;
}

// 画不了并集时：只画本机座位那一组（联机时至少自己周围看得见）；本地多人照原版
void DrawDarknessOwn(uintptr_t em, int type, int clip, int shake)
{
    int viewer = g_role == NET_LOCAL ? -1 : g_mySeat;
    std::vector<uintptr_t> hidden;
    if (viewer >= 0 && Alive(viewer))
        ForEachDarkness([&](uintptr_t e) {
            if (DarknessOwner(e) != viewer) { At<uint8_t>(e) = 0; hidden.push_back(e); }   // 只在这次绘制里当成不在
        });
    Fn<DrawTypeFn>(VA_DRAW_TYPE)(em, type, clip, shake);
    for (uintptr_t e : hidden) At<uint8_t>(e) = 1;
}

void __fastcall DrawDarknessHook(uintptr_t em, void*, int type, int clip, int shake)
{
    if (type == EFF_DARKNESS && Coop() && AliveCount() >= 2) {
        if (!DrawDarknessUnion(em)) DrawDarknessOwn(em, type, clip, shake);
        return;
    }
    Fn<DrawTypeFn>(VA_DRAW_TYPE)(em, type, clip, shake);
}

// ------------------------------------------------------------------ 绘制
// 队友靠近时半透明：联机时淡化别人（离本机的自机越近越淡），本地多人时淡化 2P / 3P（离 1P 越近越淡）。
// 自机、判定点、自机弹、支援都算。做法：画那个座位期间，DxLib 的 SetDrawBlendMode 把不透明 / 半透明 / 加算的参数乘上系数
using SetBlendFn = int(__cdecl*)(int, int);
SetBlendFn g_origSetBlend;
float g_fade = 1;
int g_reqMode = 0, g_reqParam = 0;   // 游戏最后要求的混合模式（未乘系数）

int __cdecl SetBlendHook(int mode, int param)
{
    g_reqMode = mode;
    g_reqParam = param;
    if (g_fade < 1) {
        if (mode == BLEND_NONE) { mode = BLEND_ALPHA; param = 255; }
        if (mode == BLEND_ALPHA || mode == BLEND_ADD) param = (int)(param * g_fade + 0.5f);
    }
    return g_origSetBlend(mode, param);
}

float FadeOf(int seat)
{
    int viewer = g_role == NET_LOCAL ? 0 : g_mySeat;
    if (seat == viewer || !Alive(seat) || !Alive(viewer)) return 1;
    float t = (Dist(PosOf(seat), PosOf(viewer)) - FADE_NEAR) / (FADE_FAR - FADE_NEAR);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return FADE_MIN + (1 - FADE_MIN) * t;
}

void BeginFadeAlpha(float f)
{
    if (f >= 1) return;
    g_fade = f;
    SetBlendHook(At<int>(Va(VA_BLEND_MODE)), At<int>(Va(VA_BLEND_PARAM)));
}

void BeginFade(int seat) { BeginFadeAlpha(FadeOf(seat)); }

void EndFade()
{
    if (g_fade >= 1) return;
    g_fade = 1;
    g_origSetBlend(g_reqMode, g_reqParam);
}

void DrawSeat(uintptr_t pm)
{
    Fn<ThisFn>(VA_PM_DRAW0)(pm);
    Fn<Arg2Fn>(VA_BULLETS_DRAW)(At<uintptr_t>(pm + 8), 0, 0);
    Fn<ThisFn>(VA_LASERS_DRAW)(At<uintptr_t>(pm + 4));
    Fn<ThisFn>(VA_PM_SPRITE)(pm);
}

// 幽灵：只画自机图，半透明。出局的状态 8 不画自机图，画的时候临时当成状态 0
// 低速时死的也画成普通状态：PM+0x498 是低速标志（0x6222F0 设；自机图 0x6252C0 按它画判定点、0x626DE0 画低速特效）
void DrawGhost(uintptr_t pm)
{
    uintptr_t st = At<uintptr_t>(pm);
    int state = At<int>(st + ST_STATE);
    uint8_t slow = At<uint8_t>(pm + PM_SLOW);
    At<int>(st + ST_STATE) = 0;
    At<uint8_t>(pm + PM_SLOW) = 0;
    BeginFadeAlpha(GHOST_ALPHA);
    Fn<ThisFn>(VA_PM_SPRITE)(pm);
    EndFade();
    At<uint8_t>(pm + PM_SLOW) = slow;
    At<int>(st + ST_STATE) = state;
}

// 画的顺序：内存里的座位，然后其他座位按编号
template <class F> void ForSeatsInDrawOrder(F f)
{
    int base = g_mem;
    f(base);
    for (int s = 0; s < g_seats; ++s)
        if (s != base) f(s);
}

// 替换场地绘制里 1P 的四个绘制调用（0x650E12 的 call 换成这里，0x650E19 跳到 0x650E34）
void __fastcall DrawPlayersHook(uintptr_t pm, void*)
{
    if (!Coop() || pm != g_pm[g_mem] || !AliveCount()) { DrawSeat(pm); return; }   // 全员出局（续关菜单）：照原版
    ForSeatsInDrawOrder([&](int seat) {
        if (!Alive(seat) && !Ghost(seat)) return;
        bool ghost = Ghost(seat);
        AsSeat(seat, [&](uintptr_t p) {
            if (ghost) { DrawGhost(p); return; }
            BeginFade(seat);
            DrawSeat(p);
            EndFade();
        });
    });
}

void __fastcall SupportDrawHook(uintptr_t pm, void*)
{
    if (!Coop() || pm != g_pm[g_mem] || !AliveCount()) { Fn<ThisFn>(VA_SUPPORT_DRAW)(pm); return; }
    ForSeatsInDrawOrder([&](int seat) {
        if (!Alive(seat)) return;   // 幽灵不画支援
        AsSeat(seat, [&](uintptr_t p) {
            BeginFade(seat);
            Fn<ThisFn>(VA_SUPPORT_DRAW)(p);
            EndFade();
        });
    });
}

// ------------------------------------------------------------------ HUD：每台电脑画本机自己的数据
// 本机的座位：联机是自己的座位，本地多人是 1P
int ViewerSeat()
{
    static int s_debugViewer = g_debug && getenv("LE04MULTI_VIEWER") ? atoi(getenv("LE04MULTI_VIEWER")) : -1;   // 调试：本地多人时看别人的 HUD
    if (s_debugViewer >= 0 && g_role == NET_LOCAL && s_debugViewer < g_seats) return s_debugViewer;
    return g_role == NET_HOST || g_role == NET_GUEST ? g_mySeat : 0;
}


void RollScore(int& disp, int score)
{
    if (score < disp) disp = score;
    static const int STEP[][2] = { { 100000000, 11111111 }, { 10000000, 1111111 }, { 1000000, 111111 }, { 100000, 11111 },
                                   { 10000, 1111 }, { 1000, 111 }, { 100, 11 } };
    for (auto& st : STEP)
        if (score - disp >= st[0]) disp += st[1];
    if (score > disp) ++disp;
}

// HUD 抄进来的那些字段，按 seat 的数据重新填（同 0x6545BC 起的那段）
void FeedHud(uintptr_t ui, int seat)
{
    uintptr_t pm = g_pm[seat], st = StOf(seat);
    auto nonneg = [](int v) { return v < 0 ? 0 : v; };
    RollScore(g_viewScore, At<int>(st + ST_SCORE));
    At<int>(ui + 0x04) = g_viewScore;
    At<int>(ui + 0x90) = At<int>(st + ST_SCORE);
    At<int>(ui + 0x18) = At<int>(st + ST_LIVES);
    At<int>(ui + 0x20) = nonneg(At<int>(st + 0xB4));
    At<int>(ui + 0x24) = At<int>(st + 0xB8);
    At<int>(ui + 0x1C) = At<int>(st + 0xBC);
    At<int>(ui + 0x28) = nonneg(At<int>(st + 0xC0));
    At<int>(ui + 0x2C) = At<int>(st + 0xC4);
    At<int>(ui + 0x30) = Fn<CountFn>(VA_BOMB_COUNT)(pm);
    for (int i = 0; i < 10; ++i) At<int>(ui + 0x34 + i * 4) = nonneg(At<int>(st + 0x60 + i * 4));
    At<int>(ui + 0x5C) = At<int>(st + 0x90);
    At<int>(ui + 0x94) = At<int>(ui + 0x08) = At<int>(st + ST_POWER);
    At<int>(ui + 0x98) = At<int>(ui + 0x0C) = At<int>(st + 0xA8);
    At<int>(ui + 0x9C) = At<int>(ui + 0x10) = At<int>(st + 0xAC);
    At<int>(ui + 0xA0) = At<int>(ui + 0x14) = At<int>(st + 0x5C);
    At<int>(ui + 0x80) = At<int>(st + 0xD8);
    int hits = At<int>(st + ST_HITS);
    At<int>(ui + 0x88) = hits < 0 ? 0 : hits > 99 ? 99 : hits;
    At<uintptr_t>(ui + UI_POS) = At<uintptr_t>(st);
}

// ボム图标的动画：HUD+0x7AF0 起每格 0x20（y、速度、角度、下落中、转向），HUD 只画有ボム的格。原版全在模拟里、只跟着
// [STG] 那个人（平时 1P）：每帧 HUD 更新 0x444A50 → 0x440760 让图标往下落；那个人放了ボム（PM+0x4D4）时 0x63BE40 把图标
// 往上挪一格再落下；加ボム、掉残补ボム时 0x63BDB0 把空格的图标放回顶上。联机时看画面的不是 1P（客机）的话，1P 放ボム、掉残
// 都会让客机画面上的图标重新落一遍，看起来像自己的ボム被重置了 / 也拿到了。
// 看画面的不是内存里那个座位时，用他自己的一份：画的时候换进来，他放了ボム就照 0x63BE40 挪，哪一格从空变成有ボム就从顶上落，
// 每画一次用 0x440760 推进一步
constexpr uint32_t VA_BOMB_ICONS_SHIFT = 0x63BE40, VA_BOMB_ICONS_FALL = 0x440760;

void DrawHudWithBombIcons(uintptr_t ui, int seat, uintptr_t st)
{
    BombIcons& b = g_bombIcons[seat];
    int cur[10], count = 0, before = 0;
    for (int i = 0; i < 10; ++i) {
        cur[i] = At<int>(st + 0x60 + i * 4) > 0;
        count += cur[i];
    }
    if (!b.ready) {
        memcpy(b.anim, (void*)(ui + UI_BOMB_ANIM), sizeof b.anim);
        memcpy(b.slots, cur, sizeof cur);
        b.ready = true;
    }
    for (int i = 0; i < 10; ++i) before += b.slots[i];
    SwapBytes((void*)(ui + UI_BOMB_ANIM), b.anim, sizeof b.anim);
    if (count < before) Fn<ThisFn>(VA_BOMB_ICONS_SHIFT)(ui);
    for (int i = 0; i < 10; ++i)
        if (!b.slots[i] && cur[i]) At<float>(ui + UI_BOMB_ANIM + i * 0x20) = At<float>(ui + UI_BOMB_TOP) - At<float>(Va(VA_BOMB_DROP_FROM));
    memcpy(b.slots, cur, sizeof cur);
    Fn<ThisFn>(VA_BOMB_ICONS_FALL)(ui);
    Fn<ThisFn>(VA_UI_DRAW)(ui);
    SwapBytes((void*)(ui + UI_BOMB_ANIM), b.anim, sizeof b.anim);
}

void __fastcall UiDrawHook(uintptr_t ui, void*)
{
    if (!Coop() || ui != At<uintptr_t>(g_stg + 0x14)) { Fn<ThisFn>(VA_UI_DRAW)(ui); return; }
    // 各台电脑的 HUD 内容不同：随机数状态画之前存、画完放回（所有人都这样做），模拟用的随机数序列不受 HUD 影响
    RngKeep(RNG_HUD);
    int seat = ViewerSeat();
    if (seat == g_mem || seat >= g_seats || !g_pm[seat]) {
        Fn<ThisFn>(VA_UI_DRAW)(ui);
    } else {
        static uint8_t saved[UI_FED_END - UI_FED_BEGIN];
        uintptr_t savedPos = At<uintptr_t>(ui + UI_POS);
        memcpy(saved, (void*)(ui + UI_FED_BEGIN), sizeof saved);
        AsSeat(seat, [&](uintptr_t) {   // 升级（ボム上限等）也按本机座位的
            FeedHud(ui, seat);
            DrawHudWithBombIcons(ui, seat, StOf(seat));
        });
        memcpy((void*)(ui + UI_FED_BEGIN), saved, sizeof saved);
        At<uintptr_t>(ui + UI_POS) = savedPos;
    }
    RngPut(RNG_HUD);
}

// ------------------------------------------------------------------ 本地多人的输入
// 2P：键盘 WASD 移动、J 射击、K ボム、L 低速（同 東方紅魔郷 NewClassic 的键位），或 2 号手柄；3P：3 号手柄。
// 手柄按游戏里设的手柄键位
constexpr uint8_t P2_KEYS[8] = { 0x1F /*S 下*/, 0x1E /*A 左*/, 0x20 /*D 右*/, 0x11 /*W 上*/,
                                 0x24 /*J 射击*/, 0x25 /*K ボム*/, 0 /*暂停*/, 0x26 /*L 低速*/ };
int g_localHold[MAX_SEATS][16];

void UpdateLocalInputs()
{
    const uint8_t* dik = DikBuffer();
    bool loading = At<uint8_t>(Va(VA_LOADING)) != 0;
    const int* map = (const int*)Va(VA_SLOT_MAP);
    for (int seat = 1; seat < g_cfgSeats; ++seat) {
        int pad = Fn<JoypadFn>(VA_GET_JOYPAD)(seat + 1);
        const uint8_t* keys = seat == 1 ? P2_KEYS : nullptr;
        int next[16] = {};
        for (int slot = 0; slot < 8; ++slot) {
            int bit = map[slot];
            if (bit < 0 || bit >= 16) continue;
            bool down = (pad >> bit & 1) || (keys && keys[slot] && dik && (dik[keys[slot]] & 0x80));
            if (down && !loading) next[bit] = g_localHold[seat][bit] + 1;
        }
        memcpy(g_localHold[seat], next, sizeof next);
        if (Coop() && g_mem == 0 && seat < g_seats) memcpy(g_ctx[seat].hold, next, sizeof next);
    }
}

// ------------------------------------------------------------------ 联机：每帧的锁步
// 本机这一帧的按键（8 个槽位一位一个）：取原版刚算好的持续帧数；窗口不在前台时只认手柄
uint8_t LocalMask()
{
    const int* map = (const int*)Va(VA_SLOT_MAP);
    const int* hold = (const int*)Va(VA_HOLD);
    bool focused = g_debug || GetForegroundWindow() == g_hwnd;
    int pad = focused ? 0 : Fn<JoypadFn>(VA_GET_JOYPAD)(1);
    uint8_t m = 0;
    for (int s = 0; s < 8; ++s) {
        int idx = map[s];
        if (idx < 0 || idx >= 16) continue;
        if (focused ? hold[idx] > 0 : (pad >> idx & 1)) m |= 1 << s;
    }
    return m;
}

// 按所有人一致的规则把按键位换成持续帧数（各槽位按本机的槽位表放进数组，游戏读出来的槽位值各台相同）
void CountMask(int seat, uint8_t mask)
{
    for (int s = 0; s < 8; ++s) g_cnt[seat][s] = (mask >> s & 1) ? g_cnt[seat][s] + 1 : 0;
}

// slotMerge 的位：这个槽位所有人合并（任何一人刚按下就算刚按下，否则取最长的持续帧数）
void WriteHold(int seat, int* hold, uint8_t slotMerge)
{
    const int* map = (const int*)Va(VA_SLOT_MAP);
    int out[16] = {};
    for (int s = 0; s < 8; ++s) {
        int v = g_cnt[seat][s];
        if (slotMerge >> s & 1) {
            bool fresh = false;
            int longest = 0;
            for (int x = 0; x < g_seats; ++x) {
                fresh |= g_cnt[x][s] == 1;
                if (g_cnt[x][s] > longest) longest = g_cnt[x][s];
            }
            v = fresh ? 1 : longest;
        }
        int idx = map[s];
        if (idx >= 0 && idx < 16 && v > out[idx]) out[idx] = v;
    }
    memcpy(hold, out, sizeof out);
}

// 不同步检测用：随机数状态 + 各人的位置 / HP / 状态
// 敌人：[STG+4] 的 +0..+4 是敌人指针的数组；敌人 +8 状态（0 未出场、1 活动、-1 / -2 已死）、+0xC 出场后的帧数
template <class F> void ForEachEnemy(F f)
{
    uintptr_t m = At<uintptr_t>(g_stg + 4);
    if (!RbInPool((void*)m)) return;
    auto begin = At<uintptr_t*>(m), end = At<uintptr_t*>(m + 4);
    if (!begin || end < begin || end - begin > 4096) return;
    for (auto it = begin; it < end; ++it)
        if (RbInPool((void*)*it)) f(*it);
}

uint32_t Checksum()
{
    uint32_t h = 2166136261u;
    auto mix = [&](const void* p, size_t n) {
        for (size_t i = 0; i < n; ++i) h = (h ^ ((const uint8_t*)p)[i]) * 16777619u;
    };
    mix((void*)Va(VA_MT_INDEX), 4);
    mix((void*)Va(VA_MT_STATE), 64);
    if (g_stg) {   // 部队编号、类型、计时，各敌人的状态和计时（都是整数）
        mix((void*)(g_stg + 0xA8), 4);
        mix((void*)(g_stg + 0x4C), 4);
        mix((void*)(g_stg + 0x7A08), 4);
        ForEachEnemy([&](uintptr_t e) { mix((void*)(e + 8), 8); });
    }
    for (int seat = 0; seat < g_seats; ++seat) {
        if (!g_pm[seat]) continue;
        uintptr_t st = StOf(seat);
        if (float* p = PosOf(seat)) mix(p, 8);
        mix((void*)(st + 0xB4), 4);
        mix((void*)(st + ST_STATE), 4);
    }
    return h;
}

// 联机掉线退出的座位：算出局，之后不复活、不画
void MarkGone(int seat)
{
    if (seat < 0 || seat >= g_seats || g_gone[seat]) return;
    g_gone[seat] = true;
    At<uint8_t>(g_pm[seat] + PM_OUT) = 1;
    Log("net: %dP left the game", seat + 1);
}

// 收到（或预测）的各座位按键 → 持续帧数（所有人按同样的规则算）
void ApplyMasks(const uint8_t masks[MAX_SEATS], uint8_t left)
{
    for (int s = 0; s < g_seats; ++s) {
        if (left >> s & 1) MarkGone(s);
        CountMask(s, masks[s]);
    }
    int state = At<int>(g_stg + STG_STATE);
    // 暂停菜单 / 续关菜单：所有人都能操作；对话：所有人都能用射击键翻页、ボム键快进（移动不合并）；任何人都能暂停
    uint8_t merge = (state == 1 || state == 7) ? 0xFF
        : state == 3 ? (uint8_t)(1 << SLOT_SHOT | 1 << SLOT_BOMB | 1 << SLOT_PAUSE)
        : (uint8_t)(1 << SLOT_PAUSE);
    WriteHold(0, (int*)Va(VA_HOLD), merge);
    for (int s = 1; s < g_seats; ++s) WriteHold(s, g_ctx[s].hold, 0);
}

// 客机和主机断开：其他人都算退出，本机继续一个人玩
void ContinueAlone()
{
    for (int s = 0; s < g_seats; ++s)
        if (s != g_mySeat) MarkGone(s);
    g_netGame = false;
    g_netSolo = true;
    RbEnd();
    Log("net: continuing alone");
}

// ------------------------------------------------------------------ 联机：回滚模式
// 每帧开始时存一份状态（rollback.cpp）；别人的按键没到就预测（net.cpp）。帧边界上发现预测错了，
// 回到那一帧、按真的输入重跑到现在（不画），再接着跑这一帧。
// 不能回滚的场面（暂停中、续关菜单、过关 / 结局、加载中）：先等齐这一帧所有人的按键再跑（同锁步）。
// 进入这些场面的那一帧本身是在普通场面里跑的，可以回滚；离开它们（退出、重开、下一关）的选择只在确定的帧里做。
constexpr uint32_t VA_CONTROLLER = 0x2FB2A78, CONTROLLER_SIZE = 0x160;
constexpr int RB_CTRL_RING = 64;
uint8_t g_rbCtrl[RB_CTRL_RING][CONTROLLER_SIZE];   // 各帧 Controller::Update 之后的 Controller（本机设备的部分；按键持续帧数随后按输入重算）
int g_rbDepthShown = 0;                            // 状态栏：最近 1 秒最深的回滚
int g_rbDepthWindow = 0;
DWORD g_rbWindowStart = 0;
struct { int rollbacks, frames, maxDepth; double maxMs, totalMs; } g_rbStats;

// 这一帧一开头就要做不能回滚的事：切场景（Game+8），或 STG 场景按状态重开（6：析构重建 STG）/ 存档后离开（8～13）
bool IrreversiblePending()
{
    if (RbSceneSwitchPending() || !g_stg) return true;
    int state = At<int>(g_stg + STG_STATE);
    return state == 6 || (state >= 8 && state <= 13);
}

bool HardFrame()
{
    if (g_rbFallback || !g_stg || RbSceneSwitchPending()) return true;
    int state = At<int>(g_stg + STG_STATE);
    return At<uint8_t>(g_stg + STG_PAUSED) || (state != 2 && state != 3) || At<uint8_t>(Va(VA_LOADING));
}

void RbNetBoundary()
{
    int c = NetFrameNo();
    if (g_debug) DebugFrameIo(c - 1);   // 上一帧（正常跑的）读写过的文件
    if (!RbActive() && !g_rbFallback && !RbBegin()) {
        g_rbFallback = true;
        Log("rollback: cannot keep snapshots, waiting for every input instead");
    }
    if (RbActive()) RbSave(c);
    // 调试：LE04MULTI_STRESSRB=K 时，没有预测错的帧也每帧回滚 K 帧重跑（重跑的结果和原来不一样的话，会和别人对不上）
    static int s_stress = g_debug && getenv("LE04MULTI_STRESSRB") ? atoi(getenv("LE04MULTI_STRESSRB")) : 0;
    for (int pass = 0; pass < 3; ++pass) {
        if (HardFrame() && !NetWaitFor(c)) return;   // 断线：之后 NetFrameRB 返回 false
        int g = NetMispredicted();
        if (g < 0 && pass == 0 && s_stress > 0 && !HardFrame() && RbCanRestore(c - s_stress, c)) g = c - s_stress;
        if (g < 0) break;
        if (!RbCanRestore(g, c)) { NetReportDesync(g); break; }
        LARGE_INTEGER fq, t0, t1;
        QueryPerformanceFrequency(&fq);
        QueryPerformanceCounter(&t0);
        RbKeepDraw();
        RbRestore(g, c);
        int stop = c;
        for (int x = g; x < c; ++x) {
            if (x > g) RbSave(x);
            if (x > g && IrreversiblePending()) {   // 按真的输入，第 x 帧要切场景 / 重开：重跑到此为止，从这一帧起重新正常跑（先等齐输入）
                stop = x;
                break;
            }
            NetChecksumRB(x, Checksum());
            uint8_t masks[MAX_SEATS] = {}, left = 0;
            NetInputsRB(x, masks, &left);
            memcpy((void*)Va(VA_CONTROLLER), g_rbCtrl[x % RB_CTRL_RING], CONTROLLER_SIZE);
            ApplyMasks(masks, left);
            LARGE_INTEGER f0, f1;
            QueryPerformanceCounter(&f0);
            RbSimulate();
            QueryPerformanceCounter(&f1);
            double fms = (f1.QuadPart - f0.QuadPart) * 1000.0 / fq.QuadPart;
            if (g_debug && fms > 4) {
                int cr, del;
                RbFrameHandles(x, &cr, &del);
                Log("rollback: resimulating frame %d took %.2f ms (state %d, %d handles created, %d deleted)", x, fms,
                    At<int>(g_stg + STG_STATE), cr, del);
            }
        }
        RbPutDraw();
        RbSyncBgm();
        if (stop < c) {
            NetRewind(stop);
            c = stop;
        }
        RbSave(c);
        QueryPerformanceCounter(&t1);
        double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart;
        int depth = stop - g;
        ++g_rbStats.rollbacks;
        g_rbStats.frames += depth;
        g_rbStats.totalMs += ms;
        if (depth > g_rbStats.maxDepth) g_rbStats.maxDepth = depth;
        if (ms > g_rbStats.maxMs) g_rbStats.maxMs = ms;
        if (depth > g_rbDepthWindow) g_rbDepthWindow = depth;
        if (g_debug && ms > 8) {
            Log("rollback: frame %d back to %d took %.2f ms", c, g, ms);
            DebugFrameIo(-g);   // 重跑时读写过的文件
        }
    }
    NetChecksumRB(c, Checksum());
    RbConfirm(NetConfirmedFrame());
    DWORD now = GetTickCount();
    if (now - g_rbWindowStart > 1000) {
        g_rbDepthShown = g_rbDepthWindow;
        g_rbDepthWindow = 0;
        g_rbWindowStart = now;
    }
    if (g_debug && c % 600 == 0 && c > 0) {
        Log("rollback: frame %d: %d rollbacks, %d frames resimulated (deepest %d), %.2f ms avg, %.2f ms max", c, g_rbStats.rollbacks,
            g_rbStats.frames, g_rbStats.maxDepth, g_rbStats.rollbacks ? g_rbStats.totalMs / g_rbStats.rollbacks : 0.0, g_rbStats.maxMs);
        g_rbStats = {};
        DebugMemory("in game");
    }
}

void RbNetStep()
{
    int c = NetFrameNo();
    memcpy(g_rbCtrl[c % RB_CTRL_RING], (void*)Va(VA_CONTROLLER), CONTROLLER_SIZE);
    uint8_t masks[MAX_SEATS] = {}, left = 0;
    if (NetFrameRB(LocalMask(), masks, &left)) ApplyMasks(masks, left);
    else ContinueAlone();
}

void NetStep()
{
    uint8_t masks[MAX_SEATS] = {}, left = 0;
    if (NetFrame(LocalMask(), Checksum(), masks, &left)) ApplyMasks(masks, left);
    else ContinueAlone();
}

void __fastcall CtrlUpdateHook(uintptr_t ctrl, void*)
{
    RbDebugMessFpu();
    bool rb = g_netGame && Coop() && g_mem == 0 && NetRollbackGame();
    if (rb) RbNetBoundary();
    else RollbackBoundary();
    Fn<ThisFn>(VA_CTRL_UPDATE)(ctrl);
    if (rb) RbNetStep();
    else if (g_netGame && Coop() && g_mem == 0) NetStep();
    else if (g_netSolo && Coop() && g_mem == 0 && g_mySeat > 0)
        memcpy(g_ctx[g_mySeat].hold, (void*)Va(VA_HOLD), sizeof g_ctx[g_mySeat].hold);   // 客机断线后：本机设备操作自己的座位
    else if (g_role == NET_LOCAL) UpdateLocalInputs();
    if (!rb) RollbackAfterInput();
}

// ------------------------------------------------------------------ 帧率：主循环的 WaitVSync(1) 换成固定 60fps
// （原版只靠垂直同步定速，高刷新率显示器上会跑快）
using WaitVSyncFn = int(__cdecl*)(int);

int __cdecl WaitFrameHook(int n)
{
    if (g_fpsCap < 0) return Fn<WaitVSyncFn>(VA_WAIT_VSYNC)(n);
    static LARGE_INTEGER freq, next;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    LONGLONG step = freq.QuadPart / (g_fpsCap > 0 ? g_fpsCap : 60);
    if (!next.QuadPart || now.QuadPart - next.QuadPart > step * 4) next = now;
    if (g_netGame) next.QuadPart += (LONGLONG)(NetPaceMs() * freq.QuadPart / 1000);   // 联机：领先别人时多等一点，各台的帧对齐
    while (now.QuadPart < next.QuadPart) {
        LONGLONG left = (next.QuadPart - now.QuadPart) * 1000 / freq.QuadPart;
        if (left > 2) Sleep((DWORD)(left - 1));
        QueryPerformanceCounter(&now);
    }
    next.QuadPart += step;
    return 0;
}

// ------------------------------------------------------------------ 安装
bool HookEntry(uint32_t va, void* detour, void** orig)
{
    if (!BytesAt(Va(va), Hex("558BEC6AFF68"))) { Log("entry %X: unexpected prologue", va); return false; }
    return InlineHookN((void*)Va(va), detour, orig, 10);   // push ebp / mov ebp,esp / push -1 / push imm32
}

bool Call(uint32_t site, void* hook, uint32_t target)
{
    return PatchCall(site - 0x400000, hook, target - 0x400000);
}

// va 处的字节（须为 expect）换成 call stub，余下补 nop
bool CallStub(uint32_t va, const char* expect, void* stub)
{
    uintptr_t at = Va(va);
    auto want = Hex(expect);
    if (!BytesAt(at, want)) { Log("stub %X: unexpected bytes", va); return false; }
    std::vector<uint8_t> code(want.size(), 0x90);
    code[0] = 0xE8;
    *(int32_t*)&code[1] = (int32_t)((uintptr_t)stub - (at + 5));
    return WriteCode(at, code.data(), code.size());
}

} // namespace

// 道具飞向离它最近的、还在场的自机（原版是固定飞向 Item_main+0x18）
extern "C" uintptr_t __cdecl CoopNearestTarget(uintptr_t item)
{
    uintptr_t fallback = At<uintptr_t>(Va(VA_ITEM_MAIN) + 0x18);
    if (!Coop()) return fallback;
    float* best = nullptr;
    float bestD = 0;
    for (int seat = 0; seat < g_seats; ++seat) {
        if (!Alive(seat)) continue;
        float* p = PosOf(seat);
        if (!p) continue;
        float d = Dist2(p, (const float*)item);
        if (!best || d < bestD) { best = p; bestD = d; }
    }
    return best ? (uintptr_t)best : fallback;
}

// 三处 "mov ecx,[ebx+18h]"（取吸引目标）换成 call：保存全部寄存器和 xmm0-7，ecx = CoopNearestTarget(道具)
static uintptr_t g_targetTmp;
#define SAVE_ALL __asm pushad __asm sub esp, 128 __asm movdqu [esp], xmm0 __asm movdqu [esp + 16], xmm1 \
    __asm movdqu [esp + 32], xmm2 __asm movdqu [esp + 48], xmm3 __asm movdqu [esp + 64], xmm4 \
    __asm movdqu [esp + 80], xmm5 __asm movdqu [esp + 96], xmm6 __asm movdqu [esp + 112], xmm7
#define RESTORE_ALL __asm movdqu xmm0, [esp] __asm movdqu xmm1, [esp + 16] __asm movdqu xmm2, [esp + 32] \
    __asm movdqu xmm3, [esp + 48] __asm movdqu xmm4, [esp + 64] __asm movdqu xmm5, [esp + 80] \
    __asm movdqu xmm6, [esp + 96] __asm movdqu xmm7, [esp + 112] __asm add esp, 128 __asm popad

// 0x5A0786：mov ecx,[ebx+18h] / movss xmm0,[eax+4]（eax = 道具）
extern "C" __declspec(naked) void CoopTargetStubA()
{
    __asm {
        SAVE_ALL
        push eax
        call CoopNearestTarget
        add esp, 4
        mov g_targetTmp, eax
        RESTORE_ALL
        mov ecx, g_targetTmp
        movss xmm0, dword ptr [eax + 4]
        ret
    }
}

// 0x5A0ABF：mov ecx,[ebx+18h] / mov eax,esi（esi = 道具）
extern "C" __declspec(naked) void CoopTargetStubB()
{
    __asm {
        SAVE_ALL
        push esi
        call CoopNearestTarget
        add esp, 4
        mov g_targetTmp, eax
        RESTORE_ALL
        mov ecx, g_targetTmp
        mov eax, esi
        ret
    }
}

// 0x5A0BA5：mov ecx,[ebx+18h] / movss [ebp-4],xmm0（esi = 道具）
extern "C" __declspec(naked) void CoopTargetStubC()
{
    __asm {
        movss dword ptr [ebp - 4], xmm0
        SAVE_ALL
        push esi
        call CoopNearestTarget
        add esp, 4
        mov g_targetTmp, eax
        RESTORE_ALL
        mov ecx, g_targetTmp
        ret
    }
}

// 每个敌人的行动函数之前：M+0x2CC = 离这个敌人最近的自机（敌人的位置是 [敌人+0]）
extern "C" void __cdecl AimEnemy(uintptr_t m, int index)
{
    if (!Coop() || !g_aimNearest) return;
    auto begin = At<uintptr_t*>(m), end = At<uintptr_t*>(m + 4);
    if (index < 0 || !begin || begin + index >= end) return;
    uintptr_t e = begin[index];
    const float* pos = e ? At<const float*>(e) : nullptr;
    At<uintptr_t>(m + M_AIM) = NearestAim(pos, g_enemyAim);
}

// 0x5D401C：mov eax,[edx+eax*4+2F0h]（随后 call eax；栈上是 push 的敌人下标 ebx，edx = M）
extern "C" __declspec(naked) void EnemyPatternStub()
{
    __asm {
        SAVE_ALL
        push ebx
        push edx
        call AimEnemy
        add esp, 8
        RESTORE_ALL
        mov eax, dword ptr [edx + eax * 4 + 0x2F0]
        ret
    }
}

// ------------------------------------------------------------------ 部分符牌多人化（规则 RULE_MULTI_SPELLS）
// 八尾呂智「二つの煌き」(0x505320)、雀巳「背中に突き立てる思い」(0x58CA10，前后两段)：到点时在瞄准点 Boss+0x1C（离 Boss
// 最近的自机）放一个交叉斩击和 4 个小斩击。这一段代码对每个在场的自机各跑一遍：段开头的条件跳转改成跳到 SpellEnter*（记下
// 寄存器），段结束处跳到 SpellEnd*：还有下一个自机就把瞄准点换成他的、寄存器换回段开头的再跑一遍，最后放回原来的瞄准点。
// ゼノア「運命のルーレット」(0x552BE0)：轮盘光标（Boss 对象池 [B+0x18] 里类型 2 的对象）计时到 150 时落到瞄准点所在的格子
// （落格段），之后每 80 帧以光标为中心放一圈弹。多人时给每个其他自机复制一个光标、按他的瞄准点跑一遍落格段；落到已有光标
// 那一格的（或者那个自机不在任何格里的）复制品删掉，一个格子只激活一次。复制品排在后面、同一帧又被池循环处理到时，
// 落格段按它自己的自机再跑一遍（结果相同）
// 同样整段对每个自机跑一遍的（Boss 寄存器见 BossReg）：照雲「抉るは自らの傷」(0x458EF0) 以瞄准点为中心的两圈弹、
// 蝶鬼「魂の煉獄車」(0x5265A0) 左右两边冲向瞄准点的火车、クラウゼ「ジーフリーゼロ」(0x4EF7D0) / 「スーパーノヴァ」(0x4F6600)
// 直接改瞄准点坐标的吸引、天堺「眠り猫」(0x512200) 瞄准点周围的圈（特效）。
// 按每颗弹各算的（BulletAim：换成离这颗弹最近的自机）：海仙「シロクのガマ」(0x489790) 弹进到瞄准点 50 以内就停下、
// 天堺「弾幕結界【古拙】」(0x514320) 瞄准点周围一圈里的弹删掉。
// 「眠り猫」出了 Boss 圈的弹挪到瞄准点周围的圈上（0x5136EB 起）：给每个其他自机复制一颗，挪到他周围的同一个位置
// 雀巳「リプレイ」(0x536A60) 不给每人各放一份：1000 帧以内每 150 帧以瞄准点为中心放一圈向内收的弹，两圈中心离得近时
// 缝隙减半、接近无缝。改成轮流：第 k 圈（计时 / 150）对准在场的第 k % 人数 个自机（ReplayAim）
enum { SPELL_YAOROCHI, SPELL_SUZUMI_A, SPELL_SUZUMI_B, SPELL_ROULETTE,
    SPELL_MITSUMO, SPELL_CHOUKI_A, SPELL_CHOUKI_B, SPELL_CLAUSE_1, SPELL_CLAUSE_7, SPELL_TENKAI_3 };
constexpr uint32_t B_POOL = 0x18;                 // Boss 的对象池（0xC0 字节的对象：斩击、光标……）
constexpr uint32_t VA_POOL_ALLOC = 0x5A1690;      // thiscall 池：取一个空位并初始化，返回下标
constexpr uint32_t VA_POOL_KILL = 0x44FC10;       // thiscall 池(下标)
constexpr int SAVED_EDI = 128, SAVED_EBX = 144;   // SAVE_ALL 的布局：xmm0..7（128 字节），然后 pushad
using PoolAllocFn = int(__thiscall*)(uintptr_t);
using PoolKillFn = void(__thiscall*)(uintptr_t, int);

static int BossReg(int site) { return site == SPELL_MITSUMO || site == SPELL_CHOUKI_A || site == SPELL_CHOUKI_B ? SAVED_EDI : SAVED_EBX; }

struct SpellLoop {
    bool active;                       // 段开头到段结束之间（有的段结束处也是跳过整段时的去处）
    int site;
    uintptr_t boss, aim0;              // Boss、原来的瞄准点
    uintptr_t aims[MAX_SEATS];         // 依次用的瞄准点（[0] = 原来的）
    int n, i;
    int obj0, cur, rc0;                // 轮盘：原来的光标、刚落格的复制品（池下标）、这一轮的复制品从 g_rc 的第几个开始
    uint8_t regs[160];                 // 段开头的寄存器
};
static SpellLoop g_sl;
static int g_slRet;
namespace { void SpellReset() { g_sl.active = false; } }   // BossUpdateHook 里调用（声明在前面的匿名命名空间里）

static uint32_t& SavedReg(int off) { return *(uint32_t*)(g_sl.regs + off); }

static uintptr_t PoolObj(uintptr_t pool, int idx)
{
    auto begin = At<uintptr_t*>(pool + 0xC), end = At<uintptr_t*>(pool + 0x10);
    return idx >= 0 && begin && begin + idx < end ? begin[idx] : 0;
}

// 池对象整个复制一份：+0 / +4（坐标等，各 8 字节）、+0x58 / +0x5C（各 0x20 字节）是对象自己 new 的，复制内容不复制指针
static void ClonePoolObj(uintptr_t dst, uintptr_t src)
{
    static const struct { uint32_t off, size; } OWNED[] = { { 0, 8 }, { 4, 8 }, { 0x58, 0x20 }, { 0x5C, 0x20 } };
    uintptr_t keep[4];
    for (int k = 0; k < 4; ++k) keep[k] = At<uintptr_t>(dst + OWNED[k].off);
    memcpy((void*)dst, (const void*)src, 0xC0);
    for (int k = 0; k < 4; ++k) {
        At<uintptr_t>(dst + OWNED[k].off) = keep[k];
        uintptr_t from = At<uintptr_t>(src + OWNED[k].off);
        if (keep[k] && from) memcpy((void*)keep[k], (const void*)from, OWNED[k].size);
    }
}

static bool SamePos(uintptr_t a, uintptr_t b)
{
    const float* p = a ? At<const float*>(a) : nullptr;
    const float* q = b ? At<const float*>(b) : nullptr;
    return p && q && p[0] == q[0] && p[1] == q[1];
}

// 段开头（regs：SAVE_ALL 存下的寄存器）
extern "C" void __cdecl SpellBegin(int site, const uint8_t* regs)
{
    memcpy(g_sl.regs, regs, sizeof g_sl.regs);
    uintptr_t boss = SavedReg(BossReg(site));
    g_sl.active = true;
    g_sl.site = site;
    g_sl.boss = boss;
    g_sl.aim0 = g_sl.aims[0] = At<uintptr_t>(boss + B_AIM);
    g_sl.n = 1;
    g_sl.i = 0;
    g_sl.obj0 = g_sl.cur = -1;
    g_sl.rc0 = g_rcN;
    if (!Coop() || !(g_rules & RULE_MULTI_SPELLS) || AliveCount() < 2) return;
    if (site == SPELL_ROULETTE) {
        int idx = (int)SavedReg(SAVED_EDI);
        uintptr_t obj = PoolObj(At<uintptr_t>(boss + B_POOL), idx);
        for (int k = 0; k < g_rcN; ++k)
            if (g_rc[k].obj == obj) {   // 复制出来的光标：落到它那个自机的格子
                At<uintptr_t>(boss + B_AIM) = g_rc[k].aim;
                return;
            }
        g_sl.obj0 = idx;
    }
    for (int s = 0; s < g_seats && g_sl.n < MAX_SEATS; ++s) {
        uintptr_t a = Alive(s) ? AimPoint(s) : 0;
        if (a && a != g_sl.aim0) g_sl.aims[g_sl.n++] = a;
    }
    static uint32_t s_logged;   // 调试：每段第一次多人化时记一笔
    if (g_debug && g_sl.n > 1 && !(s_logged & 1u << site)) {
        s_logged |= 1u << site;
        Log("spell site %d: %d aims", site, g_sl.n);
    }
}

// 段跑完一遍：1 = 换下一个自机再跑（寄存器换回段开头的）；0 = 结束，寄存器保持现状；2 = 结束，寄存器换回段开头的（轮盘）
extern "C" int __cdecl SpellNext(int site)
{
    if (!g_sl.active || g_sl.site != site) return 0;   // 跳过了整段
    uintptr_t boss = g_sl.boss, pool = At<uintptr_t>(boss + B_POOL);
    if (g_sl.cur >= 0) {   // 轮盘：刚落格的复制品和已有的光标在同一格（包括那个自机不在任何格里、没动的）就删掉
        uintptr_t c = PoolObj(pool, g_sl.cur);
        bool dup = SamePos(c, PoolObj(pool, g_sl.obj0));
        for (int k = g_sl.rc0; k < g_rcN - 1 && !dup; ++k) dup = SamePos(c, g_rc[k].obj);
        if (dup) {
            Fn<PoolKillFn>(VA_POOL_KILL)(pool, g_sl.cur);
            --g_rcN;
        }
        Log("roulette: cursor %d -> copy %d%s", g_sl.obj0, g_sl.cur, dup ? " (same cell, removed)" : "");
        g_sl.cur = -1;
    }
    while (++g_sl.i < g_sl.n) {
        if (g_sl.site == SPELL_ROULETTE) {
            if (g_rcN >= ROULETTE_CLONES) continue;
            int idx = Fn<PoolAllocFn>(VA_POOL_ALLOC)(pool);
            uintptr_t dst = PoolObj(pool, idx), src = PoolObj(pool, g_sl.obj0);
            if (!dst || !src) continue;
            ClonePoolObj(dst, src);
            g_rc[g_rcN++] = { dst, g_sl.aims[g_sl.i] };
            SavedReg(SAVED_EDI) = (uint32_t)idx;
            g_sl.cur = idx;
        }
        At<uintptr_t>(boss + B_AIM) = g_sl.aims[g_sl.i];
        return 1;
    }
    At<uintptr_t>(boss + B_AIM) = g_sl.aim0;
    g_sl.active = false;
    if (g_sl.site != SPELL_ROULETTE) return 0;
    if (g_sl.obj0 >= 0) SavedReg(SAVED_EDI) = (uint32_t)g_sl.obj0;
    return 2;
}

extern "C" void __cdecl SpellRegs(uint8_t* out) { memcpy(out, g_sl.regs, sizeof g_sl.regs); }

// 「眠り猫」：刚挪到瞄准点周围的弹 idx 给每个其他自机复制一颗，挪到他周围的同一个位置
constexpr uint32_t B_BULLETS = 0x14;              // Boss 的弹幕管理器：+0x1B0 起是 0x28 字节外壳的指针数组，[外壳] = 弹（0xB0 字节）
constexpr uint32_t BM_VEC = 0x1B0;
constexpr uint32_t VA_BULLET_SLOT = 0x62AC90;     // thiscall 管理器：找一个空位（没有就新建一个），返回下标
constexpr size_t BULLET_SIZE = 0xB0, BULLET_WRAP = 0x28;
using SlotFn = int(__thiscall*)(uintptr_t);

static uintptr_t BulletWrap(uintptr_t mgr, int idx)
{
    auto begin = mgr ? At<uintptr_t*>(mgr + BM_VEC) : nullptr, end = mgr ? At<uintptr_t*>(mgr + BM_VEC + 4) : nullptr;
    return idx >= 0 && begin && begin + idx < end ? begin[idx] : 0;
}

// 弹整个复制一份：+0（坐标）、+4 是弹自己 new 的 8 字节，复制内容不复制指针；外壳里只有 +0 是指针
static void CloneBullet(uintptr_t dstW, uintptr_t srcW)
{
    uintptr_t dst = At<uintptr_t>(dstW), src = At<uintptr_t>(srcW);
    if (!dst || !src) return;
    memcpy((void*)(dstW + 4), (const void*)(srcW + 4), BULLET_WRAP - 4);
    uintptr_t keep[2] = { At<uintptr_t>(dst), At<uintptr_t>(dst + 4) };
    memcpy((void*)dst, (const void*)src, BULLET_SIZE);
    for (int k = 0; k < 2; ++k) {
        At<uintptr_t>(dst + 4 * k) = keep[k];
        uintptr_t from = At<uintptr_t>(src + 4 * k);
        if (keep[k] && from) memcpy((void*)keep[k], (const void*)from, 8);
    }
}

extern "C" void __cdecl BarrierClones(uintptr_t boss, int idx)
{
    if (!Coop() || !(g_rules & RULE_MULTI_SPELLS) || AliveCount() < 2) return;
    uintptr_t mgr = At<uintptr_t>(boss + B_BULLETS), aim0 = At<uintptr_t>(boss + B_AIM);
    if (!mgr || !aim0) return;
    for (int s = 0; s < g_seats; ++s) {
        uintptr_t a = Alive(s) ? AimPoint(s) : 0;
        if (!a || a == aim0) continue;
        int j = Fn<SlotFn>(VA_BULLET_SLOT)(mgr);   // 可能新建外壳、数组搬家：之后再按下标取
        uintptr_t srcW = BulletWrap(mgr, idx), dstW = BulletWrap(mgr, j);
        if (!srcW || !dstW || j == idx) continue;
        CloneBullet(dstW, srcW);
        float* p = At<float*>(At<uintptr_t>(dstW));
        if (!p) continue;
        const float *from = (const float*)aim0, *to = (const float*)a;
        p[0] = p[0] - from[0] + to[0];
        p[1] = p[1] - from[1] + to[1];
        static int s_logged;   // 调试
        if (g_debug && s_logged++ < 3) Log("barrier: bullet %d -> copy %d for seat %d", idx, j, s);
    }
}

// 弹 idx 用的瞄准点：离它最近的在场自机的
extern "C" uintptr_t __cdecl BulletAim(uintptr_t boss, int idx)
{
    uintptr_t aim = At<uintptr_t>(boss + B_AIM);
    if (!Coop() || !(g_rules & RULE_MULTI_SPELLS) || AliveCount() < 2) return aim;
    uintptr_t w = BulletWrap(At<uintptr_t>(boss + B_BULLETS), idx);
    uintptr_t obj = w ? At<uintptr_t>(w) : 0;
    const float* pos = obj ? At<const float*>(obj) : nullptr;
    if (!pos) return aim;
    uintptr_t best = 0;
    float bestD = 0;
    for (int s = 0; s < g_seats; ++s) {
        uintptr_t a = Alive(s) ? AimPoint(s) : 0;
        if (!a) continue;
        float d = Dist2(pos, (const float*)a);
        if (!best || d < bestD) { best = a; bestD = d; }
    }
    static int s_logged;   // 调试
    if (g_debug && best && best != aim && s_logged++ < 3) Log("bullet aim: bullet %d -> other ship (%.0f,%.0f)", idx, pos[0], pos[1]);
    return best ? best : aim;
}
static uintptr_t g_bulletAim;

// 「リプレイ」这一圈的圆心：轮流对准在场的自机（按座位顺序）。只看 Boss 的计时和谁在场，各台一致、回滚重跑也一样
extern "C" uintptr_t __cdecl ReplayAim(uintptr_t boss)
{
    uintptr_t aim = At<uintptr_t>(boss + B_AIM);
    if (!Coop() || !(g_rules & RULE_MULTI_SPELLS)) return aim;
    int alive[MAX_SEATS], n = 0;
    for (int s = 0; s < g_seats; ++s)
        if (Alive(s) && AimPoint(s)) alive[n++] = s;
    if (n < 2) return aim;
    uintptr_t rec = At<uintptr_t>(boss);
    int ring = rec ? At<int>(rec + 0x6C) / 150 : 0;   // 计时 < 0 时原版不放圈
    int seat = alive[(ring < 0 ? 0 : ring) % n];
    static int s_last = -1;   // 调试
    if (g_debug && ring != s_last) {
        s_last = ring;
        const float* c = (const float*)AimPoint(seat);
        Log("replay: ring %d -> seat %d (%.0f,%.0f)", ring, seat, c[0], c[1]);
    }
    return AimPoint(seat);
}

// 跳板的跳转目标（InstallSpells 里填）
static uint32_t s_yBlock, s_ySkip, s_yLoop, s_yCont;           // 二つの煌き
static uint32_t s_saBlock, s_sbBlock, s_sbLoop, s_sSkip, s_sCont;   // 背中に突き立てる思い
static uint32_t s_rBlock, s_rSkip, s_rCont;                   // 運命のルーレット
static uint32_t s_mBlock, s_mNext, s_mCont;                   // 抉るは自らの傷
static uint32_t s_caBlock, s_caSkip, s_caCont, s_caNext, s_cbBlock, s_cbCont, s_cbNext;   // 魂の煉獄車
static uint32_t s_c1Block, s_c1Next, s_c1Cont, s_c1After;     // ジーフリーゼロ
static uint32_t s_c7Block, s_c7Next, s_c7Cont, s_c7After;     // スーパーノヴァ
static uint32_t s_t3Block, s_t3Next, s_t3Cont, s_t3After, s_t3Skip, s_t3Flag, s_t3CloneNext;   // 眠り猫
static uint32_t s_kNext, s_t5Next;                            // シロクのガマ、弾幕結界【古拙】

// 段开头：记下寄存器，再去 block
#define SPELL_BEGIN(site, block) SAVE_ALL __asm mov eax, esp __asm push eax __asm push site __asm call SpellBegin \
    __asm add esp, 8 RESTORE_ALL __asm jmp dword ptr [block]
// 段开头：原来是 jne 跳过（标志位是前面 test / cmp 的结果）
#define SPELL_ENTER(site, block, skip) __asm jne skip_ SPELL_BEGIN(site, block) \
    __asm skip_: __asm jmp dword ptr [skip]
// 段结束：还要再跑就换回段开头的寄存器跳回段开头，否则去 cont
#define SPELL_END(site, block, cont) SAVE_ALL __asm push site __asm call SpellNext __asm add esp, 4 \
    __asm mov g_slRet, eax RESTORE_ALL \
    __asm cmp g_slRet, 0 __asm je done_ \
    __asm sub esp, 160 __asm mov eax, esp __asm push eax __asm call SpellRegs __asm add esp, 4 RESTORE_ALL \
    __asm cmp g_slRet, 1 __asm je again_ \
    __asm done_: __asm jmp dword ptr [cont] \
    __asm again_: __asm jmp dword ptr [block]

extern "C" __declspec(naked) void SpellEnterY() { __asm { SPELL_ENTER(0, s_yBlock, s_ySkip) } }
extern "C" __declspec(naked) void SpellEnterSA() { __asm { SPELL_ENTER(1, s_saBlock, s_sSkip) } }
extern "C" __declspec(naked) void SpellEnterSB() { __asm { SPELL_ENTER(2, s_sbBlock, s_sSkip) } }
extern "C" __declspec(naked) void SpellEnterR() { __asm { SPELL_ENTER(3, s_rBlock, s_rSkip) } }

// 0x505DF9：cmp esi,4 / jl 0x505C20（4 个小斩击的循环），之后是 0x505E02
extern "C" __declspec(naked) void SpellEndY()
{
    __asm {
        cmp esi, 4
        jl loop_
        SPELL_END(0, s_yBlock, s_yCont)
    loop_:
        jmp dword ptr [s_yLoop]
    }
}

// 0x58D78A：jmp 0x58DF11（前面的小斩击循环已经结束）
extern "C" __declspec(naked) void SpellEndSA() { __asm { SPELL_END(1, s_saBlock, s_sCont) } }

// 0x58DF08：cmp esi,4 / jl 0x58DCF0，之后是 0x58DF11
extern "C" __declspec(naked) void SpellEndSB()
{
    __asm {
        cmp esi, 4
        jl loop_
        SPELL_END(2, s_sbBlock, s_sCont)
    loop_:
        jmp dword ptr [s_sbLoop]
    }
}

// 0x553C49：jmp 0x554068（落格段结束，去池循环的下一个对象）
extern "C" __declspec(naked) void SpellEndR() { __asm { SPELL_END(3, s_rBlock, s_rCont) } }

// 抉るは自らの傷 0x459272：xor eax,eax / mov [esp+24h],eax / jmp 0x459280；段结束 0x4596AD：jmp 0x459D3B
extern "C" __declspec(naked) void SpellBlockM()
{
    __asm {
        xor eax, eax
        mov [esp + 0x24], eax
        jmp dword ptr [s_mNext]
    }
}
extern "C" __declspec(naked) void SpellEnterM() { __asm { SPELL_BEGIN(4, s_mBlock) } }
extern "C" __declspec(naked) void SpellEndM() { __asm { SPELL_END(4, s_mBlock, s_mCont) } }

// 魂の煉獄車 左边 0x5269B8：jne 0x526C71，段结束 0x526C6C：mov ebx,6；右边 0x526C8C：jne 0x526F4B，
// 段结束 0x526F4B（也是跳过时的去处）：mov ecx,[edi+14h] / xor ebx,ebx
extern "C" __declspec(naked) void SpellContCA()
{
    __asm {
        mov ebx, 6
        jmp dword ptr [s_caNext]
    }
}
extern "C" __declspec(naked) void SpellContCB()
{
    __asm {
        mov ecx, [edi + 0x14]
        xor ebx, ebx
        jmp dword ptr [s_cbNext]
    }
}
extern "C" __declspec(naked) void SpellEnterCA() { __asm { SPELL_ENTER(5, s_caBlock, s_caSkip) } }
extern "C" __declspec(naked) void SpellEndCA() { __asm { SPELL_END(5, s_caBlock, s_caCont) } }
extern "C" __declspec(naked) void SpellEnterCB() { __asm { SPELL_ENTER(6, s_cbBlock, s_cbCont) } }
extern "C" __declspec(naked) void SpellEndCB() { __asm { SPELL_END(6, s_cbBlock, s_cbCont) } }

// ジーフリーゼロ 0x4EFDF2：mov eax,[ebx+1Ch] / movss xmm1,[eax+4]；段结束 0x4EFE24：mov ecx,[ebx+14h] / xor esi,esi
extern "C" __declspec(naked) void SpellBlockC1()
{
    __asm {
        mov eax, [ebx + 0x1C]
        movss xmm1, dword ptr [eax + 4]
        jmp dword ptr [s_c1Next]
    }
}
extern "C" __declspec(naked) void SpellContC1()
{
    __asm {
        mov ecx, [ebx + 0x14]
        xor esi, esi
        jmp dword ptr [s_c1After]
    }
}
extern "C" __declspec(naked) void SpellEnterC1() { __asm { SPELL_BEGIN(7, s_c1Block) } }
extern "C" __declspec(naked) void SpellEndC1() { __asm { SPELL_END(7, s_c1Block, s_c1Cont) } }

// スーパーノヴァ 0x4F7513：mov eax,[ebx+40h] / mov ecx,[ebx+1Ch]；段结束 0x4F75B1（也是跳过时的去处）：cmp edi,0C8h
extern "C" __declspec(naked) void SpellBlockC7()
{
    __asm {
        mov eax, [ebx + 0x40]
        mov ecx, [ebx + 0x1C]
        jmp dword ptr [s_c7Next]
    }
}
extern "C" __declspec(naked) void SpellContC7()
{
    __asm {
        cmp edi, 0xC8
        jmp dword ptr [s_c7After]
    }
}
extern "C" __declspec(naked) void SpellEnterC7() { __asm { SPELL_BEGIN(8, s_c7Block) } }
extern "C" __declspec(naked) void SpellEndC7() { __asm { SPELL_END(8, s_c7Block, s_c7Cont) } }

// 眠り猫 0x51231C：mov esi,[ebx+1Ch] / sub esp,28h；段结束 0x51236C：test edi,edi / jne 0x5123B6 / mov eax,[10896B0h]
extern "C" __declspec(naked) void SpellBlockT3()
{
    __asm {
        mov esi, [ebx + 0x1C]
        sub esp, 0x28
        jmp dword ptr [s_t3Next]
    }
}
extern "C" __declspec(naked) void SpellContT3()
{
    __asm {
        test edi, edi
        jne skip_
        mov eax, s_t3Flag
        mov eax, [eax]
        jmp dword ptr [s_t3After]
    skip_:
        jmp dword ptr [s_t3Skip]
    }
}
extern "C" __declspec(naked) void SpellEnterT3() { __asm { SPELL_BEGIN(9, s_t3Block) } }
extern "C" __declspec(naked) void SpellEndT3() { __asm { SPELL_END(9, s_t3Block, s_t3Cont) } }

// 0x513896：mov dword ptr [eax+40h],0C8h（弹 edi 挪完了），之后是 0x51389D
extern "C" __declspec(naked) void BarrierCloneStub()
{
    __asm {
        mov dword ptr [eax + 0x40], 0xC8
        SAVE_ALL
        push edi
        push ebx
        call BarrierClones
        add esp, 8
        RESTORE_ALL
        jmp dword ptr [s_t3CloneNext]
    }
}

// シロクのガマ 0x489DDF：mov eax,[ebx+1Ch] / push 8（弹 edi）
extern "C" __declspec(naked) void BulletAimStubK()
{
    __asm {
        SAVE_ALL
        push edi
        push ebx
        call BulletAim
        add esp, 8
        mov g_bulletAim, eax
        RESTORE_ALL
        mov eax, g_bulletAim
        push 8
        jmp dword ptr [s_kNext]
    }
}

// 弾幕結界【古拙】 0x514AB2：mov ecx,[eax+1Ch] / mov eax,[eax+40h]（eax = Boss，弹 edi）
extern "C" __declspec(naked) void BulletAimStubT5()
{
    __asm {
        SAVE_ALL
        push edi
        push eax
        call BulletAim
        add esp, 8
        mov g_bulletAim, eax
        RESTORE_ALL
        mov ecx, g_bulletAim
        mov eax, [eax + 0x40]
        jmp dword ptr [s_t5Next]
    }
}

// リプレイ 0x536BCA / 0x536C3E（call 进来）：mov eax,[ebx+1Ch] / cvtsd2ss xmm0,xmm0（ebx = Boss）
extern "C" __declspec(naked) void ReplayAimStub()
{
    __asm {
        SAVE_ALL
        push ebx
        call ReplayAim
        add esp, 4
        mov g_bulletAim, eax
        RESTORE_ALL
        mov eax, g_bulletAim
        cvtsd2ss xmm0, xmm0
        ret
    }
}

namespace {

// va 处的字节（须为 expect）换成 jmp stub，余下补 nop
bool JmpStub(uint32_t va, const char* expect, void* stub)
{
    uintptr_t at = Va(va);
    auto want = Hex(expect);
    if (!BytesAt(at, want)) { Log("stub %X: unexpected bytes", va); return false; }
    std::vector<uint8_t> code(want.size(), 0x90);
    code[0] = 0xE9;
    *(int32_t*)&code[1] = (int32_t)((uintptr_t)stub - (at + 5));
    return WriteCode(at, code.data(), code.size());
}

bool InstallSpells()
{
    s_yBlock = Va(0x5059F1); s_ySkip = s_yCont = Va(0x505E02); s_yLoop = Va(0x505C20);
    s_saBlock = Va(0x58D30F); s_sbBlock = Va(0x58DA7E); s_sbLoop = Va(0x58DCF0); s_sSkip = Va(0x58DF15); s_sCont = Va(0x58DF11);
    s_rBlock = Va(0x553A43); s_rSkip = Va(0x553C4E); s_rCont = Va(0x554068);
    s_mBlock = (uint32_t)SpellBlockM; s_mNext = Va(0x459280); s_mCont = Va(0x459D3B);
    s_caBlock = Va(0x5269BE); s_caSkip = s_caNext = Va(0x526C71); s_caCont = (uint32_t)SpellContCA;
    s_cbBlock = Va(0x526C92); s_cbCont = (uint32_t)SpellContCB; s_cbNext = Va(0x526F50);
    s_c1Block = (uint32_t)SpellBlockC1; s_c1Next = Va(0x4EFDFA); s_c1Cont = (uint32_t)SpellContC1; s_c1After = Va(0x4EFE29);
    s_c7Block = (uint32_t)SpellBlockC7; s_c7Next = Va(0x4F7519); s_c7Cont = (uint32_t)SpellContC7; s_c7After = Va(0x4F75B7);
    s_t3Block = (uint32_t)SpellBlockT3; s_t3Next = Va(0x512322); s_t3Cont = (uint32_t)SpellContT3;
    s_t3After = Va(0x512375); s_t3Skip = Va(0x5123B6); s_t3Flag = Va(0x10896B0); s_t3CloneNext = Va(0x51389D);
    s_kNext = Va(0x489DE4); s_t5Next = Va(0x514AB8);
    return JmpStub(0x5059EB, "0F8511040000", (void*)SpellEnterY)
        && JmpStub(0x505DF9, "83FE040F8C1EFEFFFF", (void*)SpellEndY)
        && JmpStub(0x58D309, "0F85060C0000", (void*)SpellEnterSA)
        && JmpStub(0x58D78A, "E982070000", (void*)SpellEndSA)
        && JmpStub(0x58DA78, "0F8597040000", (void*)SpellEnterSB)
        && JmpStub(0x58DF08, "83FE040F8CDFFDFFFF", (void*)SpellEndSB)
        && JmpStub(0x553A3D, "0F850B020000", (void*)SpellEnterR)
        && JmpStub(0x553C49, "E91A040000", (void*)SpellEndR)
        && JmpStub(0x459272, "33C089442424EB06", (void*)SpellEnterM)
        && JmpStub(0x4596AD, "E989060000", (void*)SpellEndM)
        && JmpStub(0x5269B8, "0F85B3020000", (void*)SpellEnterCA)
        && JmpStub(0x526C6C, "BB06000000", (void*)SpellEndCA)
        && JmpStub(0x526C8C, "0F85B9020000", (void*)SpellEnterCB)
        && JmpStub(0x526F4B, "8B4F1433DB", (void*)SpellEndCB)
        && JmpStub(0x4EFDF2, "8B431CF30F104804", (void*)SpellEnterC1)
        && JmpStub(0x4EFE24, "8B4B1433F6", (void*)SpellEndC1)
        && JmpStub(0x4F7513, "8B43408B4B1C", (void*)SpellEnterC7)
        && JmpStub(0x4F75B1, "81FFC8000000", (void*)SpellEndC7)
        && JmpStub(0x51231C, "8B731C83EC28", (void*)SpellEnterT3)
        && JmpStub(0x51236C, "85FF7546A1B0960801", (void*)SpellEndT3)
        && JmpStub(0x513896, "C74040C8000000", (void*)BarrierCloneStub)
        && JmpStub(0x489DDF, "8B431C6A08", (void*)BulletAimStubK)
        && JmpStub(0x514AB2, "8B481C8B4040", (void*)BulletAimStubT5)
        && CallStub(0x536BCA, "8B431CF20F5AC0", (void*)ReplayAimStub)
        && CallStub(0x536C3E, "8B431CF20F5AC0", (void*)ReplayAimStub);
}

} // namespace

// 一格结束时（0x63EE20(跳过, x)）：副机是雀巳（0xC..0xE）的人按几率得一个ボム（0x63EEF6 起：道中 10%、Boss 战 50%，
// 带ビッグミステイク时减半；用ビッグミステイク跳过这一格时不给）。原版只看、只给 [STG] 那个人。
// 其他在场的人（幽灵不算）各自按自己的副机和升级再判一次。用游戏的随机数，各台一致
constexpr uint32_t PATCH_UNIT_BOMB = 0x63EEF6, VA_GIVE_BOMB = 0x639AE0, VA_GET_RAND = 0x6A1E50;
constexpr int SKILL_BIG_MISTAKE = 62;
using RandFn = int(__cdecl*)(int);

extern "C" void __cdecl UnitBombOthers(uintptr_t stg, int skip, int arg2)
{
    if (!Coop() || stg != g_stg || ((skip & 0xFF) && !(arg2 & 0xFF))) return;
    int battle = At<int>(stg + STG_BATTLE);
    if (battle != 0 && battle != 1) return;
    for (int s = 0; s < g_seats; ++s) {
        if (s == g_mem || !Alive(s)) continue;
        int sub = g_ctx[s].ships[1];
        if (sub < 0xC || sub > 0xE) continue;
        bool half = !SeatPu<uint8_t>(s, PU_OFF) && SeatPu<uint8_t>(s, 4 + SKILL_BIG_MISTAKE);
        int chance = battle == 0 ? (half ? 5 : 10) : (half ? 25 : 50);
        static int s_force = g_debug && getenv("LE04MULTI_UNITBOMB") ? atoi(getenv("LE04MULTI_UNITBOMB")) : -1;   // 调试：几率固定
        if (s_force >= 0) chance = s_force;
        if (Fn<RandFn>(VA_GET_RAND)(100) < chance) AsSeat(s, [&](uintptr_t pm) { Fn<ThisFn>(VA_GIVE_BOMB)(pm); });
    }
}

// 0x63EEF6：mov eax,[esi+3Ch] / cmp eax,0Ch（esi = STG，bl = 跳过，[ebp+0Ch] = 第二个参数）
extern "C" __declspec(naked) void UnitBombStub()
{
    __asm {
        SAVE_ALL
        push dword ptr [ebp + 0xC]
        push ebx
        push esi
        call UnitBombOthers
        add esp, 12
        RESTORE_ALL
        mov eax, dword ptr [esi + 0x3C]
        cmp eax, 0xC
        ret
    }
}

uintptr_t CoopStg() { return g_stg; }

// 每局的模拟状态（回滚时和游戏的内存一起保存 / 还原）。绘制、输入设备、联机连接用的状态不在里面
void CoopSimState(std::vector<std::pair<void*, size_t>>& out)
{
    auto add = [&](void* p, size_t n) { out.emplace_back(p, n); };
    add(g_ctx, sizeof g_ctx);
    add(&g_stg, sizeof g_stg);
    add(g_pm, sizeof g_pm);
    add(&g_seats, sizeof g_seats);
    add(&g_mem, sizeof g_mem);
    add(g_gone, sizeof g_gone);
    add(g_cnt, sizeof g_cnt);
    add(g_bombNames, sizeof g_bombNames);
    add(g_give, sizeof g_give);
    add(&g_coopFrame, sizeof g_coopFrame);
    add(&g_calm, sizeof g_calm);
    add(g_reviveFix, sizeof g_reviveFix);
    add(&g_prevStageClear, sizeof g_prevStageClear);
    add(&g_enemyAim, sizeof g_enemyAim);
    add(&g_gameOver, sizeof g_gameOver);
    add(&g_mg, sizeof g_mg);
}

// 各座位这一帧的按键持续帧数：内存里的座位在 0x2FB2A8C，其他座位在 g_ctx
void CoopGetInputs(int hold[MAX_SEATS][16])
{
    memset(hold, 0, sizeof(int) * MAX_SEATS * 16);
    for (int s = 0; s < MAX_SEATS; ++s)
        memcpy(hold[s], s == g_mem ? (const int*)Va(VA_HOLD) : g_ctx[s].hold, sizeof(int) * 16);
}

void CoopSetInputs(const int hold[MAX_SEATS][16])
{
    for (int s = 0; s < MAX_SEATS; ++s)
        memcpy(s == g_mem ? (int*)Va(VA_HOLD) : g_ctx[s].hold, hold[s], sizeof(int) * 16);
}
uintptr_t CoopPm(int seat) { return (g_stg && seat >= 0 && seat < MAX_SEATS) ? g_pm[seat] : 0; }

// 调试：HUD 的ボム图标（HUD 里那份、各座位那份的 y）
void CoopDebugHudBombs()
{
    if (!Coop()) return;
    uintptr_t ui = At<uintptr_t>(g_stg + 0x14);
    char line[200];
    int n = snprintf(line, sizeof line, "hud bombs y:");
    for (int i = 0; i < 5; ++i) n += snprintf(line + n, sizeof line - n, " %.0f", At<float>(ui + UI_BOMB_ANIM + i * 0x20));
    Log("%s", line);
    for (int s = 0; s < g_seats; ++s) {
        n = snprintf(line, sizeof line, "  %dP%s:", s + 1, g_bombIcons[s].ready ? "" : " (unused)");
        for (int i = 0; i < 5; ++i) n += snprintf(line + n, sizeof line - n, " %.0f", At<float>((uintptr_t)g_bombIcons[s].anim + i * 0x20));
        Log("%s", line);
    }
}

// 调试：列出视界不良的每一层（状态、年龄、标记、位置、大小）
void CoopDebugDarkness()
{
    int i = 0;
    ForEachDarkness([&](uintptr_t e) {
        Log("dark %d: state %08X age %d tag %d owner %d pos %.1f %.1f scale %.2f", i++, At<uint32_t>(e), At<int>(e + EFF_AGE), At<int>(e + EFF_TAG),
            Coop() ? DarknessOwner(e) + 1 : 0, At<float>(e + EFF_X), At<float>(e + EFF_Y), At<float>(e + EFF_SCALE));
    });
    for (int s = 0; s < g_seats; ++s)
        if (float* p = PosOf(s)) Log("dark: %dP at %.1f %.1f", s + 1, p[0], p[1]);
}

void CoopSetGod(int seat, bool on)
{
    if (seat < 0 || seat >= MAX_SEATS) return;
    g_god[seat] = on;
    Log("god %dP %d", seat + 1, on);
}

void CoopConfigure(int role, int localSeats, const int ships[MAX_SEATS - 1][3], int rules)
{
    g_role = role;
    g_cfgSeats = localSeats < 2 ? 2 : localSeats > MAX_SEATS ? MAX_SEATS : localSeats;
    g_cfgRules = rules;
    for (int s = 1; s < MAX_SEATS; ++s)
        for (int i = 0; i < 3; ++i) g_cfgShips[s][i] = ships[s - 1][i];
    if (g_debug && getenv("LE04MULTI_GOD"))   // 调试：开局前就让所有座位无敌（联机时各台都设，才不会不同步）
        for (int s = 0; s < MAX_SEATS; ++s) g_god[s] = true;
    if (role == NET_HOST || role == NET_GUEST) {
        // 联机：强制同步加载。异步加载的完成时机取决于硬盘速度，加载中游戏会清空按键、随机数返回 0，各台会不同步
        uintptr_t at = Va(VA_SET_ASYNC_LOAD) + 3;
        if (BytesAt(at, Hex("8B4508"))) WriteCode(at, Hex("33C090").data(), 3);
        else Log("async load patch: unexpected bytes");
    }
}

bool InstallCoop()
{
    if (!BytesAt(Va(VA_FOPEN_S), Hex("558BEC568B7508")) || !BytesAt(Va(VA_FCLOSE), Hex("6A0C68"))) {
        Log("fopen_s / fclose: unexpected bytes");
        return false;
    }
    bool ok = InlineHookN((void*)Va(VA_FOPEN_S), (void*)FopenHook, (void**)&g_origFopen, 7)
        && InlineHookN((void*)Va(VA_FCLOSE), (void*)FcloseHook, (void**)&g_origFclose, 7)
        && HookEntry(VA_STG_CTOR, (void*)StgCtorHook, (void**)&g_origStgCtor)
        && HookEntry(VA_STG_DTOR, (void*)StgDtorHook, (void**)&g_origStgDtor)
        && HookEntry(VA_WORLD_UPDATE, (void*)WorldUpdateHook, (void**)&g_origWorldUpdate)
        && HookEntry(VA_DAMAGE, (void*)DamageHook, (void**)&g_origDamage)
        && HookEntry(VA_BOSS_DAMAGE, (void*)BossDamageHook, (void**)&g_origBossDamage)
        && Call(CALL_PM_UPDATE, (void*)PmUpdateHook, VA_PM_UPDATE)
        && Call(CALL_COLLISION, (void*)CollisionHook, VA_COLLISION)
        && Call(CALL_ITEM_ATTRACT, (void*)ItemAttractHook, VA_ITEM_ATTRACT)
        && Call(CALL_ITEM_UPDATE, (void*)ItemUpdateHook, VA_ITEM_UPDATE)
        && Call(CALL_CONTINUE_MENU, (void*)ContinueMenuHook, VA_CONTINUE_MENU)
        && Call(CALL_EXTEND, (void*)ExtendHook, VA_EXTEND)
        && Call(CALL_DRAW_DARKNESS, (void*)DrawDarknessHook, VA_DRAW_TYPE)
        && Call(CALL_PM_DRAW0, (void*)DrawPlayersHook, VA_PM_DRAW0)
        && Call(CALL_ENEMY_UPDATE, (void*)EnemyUpdateHook, VA_ENEMY_UPDATE)
        && Call(CALL_ROUTE, (void*)RouteHook, VA_ROUTE)
        && Call(CALL_STAGE_NEXT, (void*)StageNextHook, VA_STAGE_NEXT)
        && Call(CALL_BOSS_UPDATE, (void*)BossUpdateHook, VA_BOSS_UPDATE)
        && CallStub(PATCH_ENEMY_PATTERN, "8B8482F0020000", (void*)EnemyPatternStub)
        && Call(CALL_SUPPORT_DRAW, (void*)SupportDrawHook, VA_SUPPORT_DRAW)
        && Call(CALL_UI_DRAW, (void*)UiDrawHook, VA_UI_DRAW)
        && Call(CALL_CTRL_UPDATE, (void*)CtrlUpdateHook, VA_CTRL_UPDATE)
        && Call(CALL_WAIT_VSYNC, (void*)WaitFrameHook, VA_WAIT_VSYNC)
        && CallStub(0x5A0786, "8B4B18F30F104004", (void*)CoopTargetStubA)
        && CallStub(0x5A0ABF, "8B4B188BC6", (void*)CoopTargetStubB)
        && CallStub(0x5A0BA5, "8B4B18F30F1145FC", (void*)CoopTargetStubC)
        && CallStub(PATCH_GAME_OVER, "8A829A040000", (void*)GameOverStub)
        && CallStub(PATCH_UNIT_BOMB, "8B463C83F80C", (void*)UnitBombStub)
        && InstallSpells();
    // 1P 的四个绘制调用都在 DrawPlayersHook 里了：0x650E19 起跳到 0x650E34（保留 0x650E17 的 mov esi,[edi]）
    uintptr_t skip = Va(PATCH_SKIP_P1_DRAW);
    if (ok && BytesAt(skip, Hex("6A008B4E08")))
        ok = WriteCode(skip, Hex("EB19").data(), 2);
    else if (ok) { Log("draw patch: unexpected bytes"); ok = false; }
    // DxLib SetDrawBlendMode：push ebp / mov ebp,esp / mov eax,[0B94FE4h]
    if (ok && BytesAt(Va(VA_SET_BLEND), Hex("558BECA1E44FB900")))
        ok = InlineHookN((void*)Va(VA_SET_BLEND), (void*)SetBlendHook, (void**)&g_origSetBlend, 8);
    else if (ok) { Log("SetDrawBlendMode: unexpected bytes"); ok = false; }
    // 对话脚本按对话对象记下的自机（[talk+0x10]，即 1P 的）来选，和立绘、Boss 曲开始的行一致。
    // 原版读 [STG+0x38]：1P 出局时世界更新里换成了别人的，文本就成了别人的，立绘和放 BGM 的行还是 1P 的
    // push [ebx+38h] / mov ecx,[ebx+10h]  →  mov ecx,[ebx+10h] / push [ecx+10h]
    uintptr_t talk = Va(PATCH_TALK_SHIP);
    if (ok && BytesAt(talk, Hex("FF73388B4B10FF7344")))
        ok = WriteCode(talk, Hex("8B4B10FF7110FF7344").data(), 9);
    else if (ok) { Log("talk patch: unexpected bytes"); ok = false; }
    return ok;
}

// ------------------------------------------------------------------ 界面：自机标记和 2P / 3P 的状态
namespace {

const ImU32 SEAT_COL[MAX_SEATS] = { IM_COL32(255, 120, 150, 255), IM_COL32(120, 220, 255, 255), IM_COL32(150, 240, 120, 255) };
const char* const SEAT_NAME[MAX_SEATS] = { "1P", "2P", "3P" };

void Label(ImDrawList* dl, ImVec2 c, const char* text, ImU32 col)
{
    ImVec2 ts = ImGui::CalcTextSize(text);
    ImVec2 p(c.x - ts.x / 2, c.y - ts.y / 2);
    dl->AddText(ImVec2(p.x + 1, p.y + 1), IM_COL32(0, 0, 0, 220), text);
    dl->AddText(p, col, text);
}

void Bar(ImDrawList* dl, ImVec2 at, float w, float h, float frac, ImU32 col)
{
    frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
    dl->AddRectFilled(at, ImVec2(at.x + w, at.y + h), IM_COL32(40, 40, 40, 220));
    if (frac > 0) dl->AddRectFilled(at, ImVec2(at.x + w * frac, at.y + h), col);
    dl->AddRect(at, ImVec2(at.x + w, at.y + h), IM_COL32(0, 0, 0, 255));
}

// 一个座位的状态面板。第一行：座位（出局 / 退出时标出）、LV（居中）、残机；下面 HP、屏障、火力、瞬雷槽（FL）、ボム个数（Bomb）。
// k：整体缩放
constexpr float PANEL_ROW_GAP = 1;   // 行间距（640×480 时的像素）
constexpr int PANEL_ROWS = 6;
float PanelHeight(float sc, float k = 1) { return (ImGui::GetTextLineHeight() * k + PANEL_ROW_GAP * sc) * PANEL_ROWS; }

// ボム的个数：ST+0x60 起一格一个（放ボム时整排左移），格数 = 升级的上限（升级关闭时 PU+0x19C，否则 +0x1A0；0x623CE6）
int BombCount(int seat, uintptr_t st)
{
    int max = SeatPu<uint8_t>(seat, PU_OFF) ? SeatPu<int>(seat, 0x19C) : SeatPu<int>(seat, 0x1A0);
    max = max < 0 ? 0 : max > 16 ? 16 : max;
    int n = 0;
    for (int i = 0; i < max; ++i) n += At<int>(st + 0x60 + i * 4) != 0;
    return n;
}

void Panel(ImDrawList* dl, int seat, float bottom, float sc, float k = 1)
{
    uintptr_t st = StOf(seat);
    if (!st) return;
    ImFont* font = ImGui::GetFont();
    float fs = ImGui::GetFontSize() * k;
    auto text = [&](float tx, float ty, ImU32 col, const char* str) { dl->AddText(font, fs, ImVec2(tx, ty), col, str); };
    auto width = [&](const char* str) { return ImGui::CalcTextSize(str).x * k; };
    float x = 6 * sc, w = 104 * sc, lh = ImGui::GetTextLineHeight() * k;
    float y = bottom - PanelHeight(sc, k);
    dl->AddRectFilled(ImVec2(x - 4 * sc, y - 4 * sc), ImVec2(x + w + 4 * sc, bottom + 4 * sc), IM_COL32(10, 20, 40, 190), 4 * sc);
    char buf[64];
    snprintf(buf, sizeof buf, "%s%s", SEAT_NAME[seat], g_gone[seat] ? " LEFT" : Out(seat) ? " OUT" : "");
    text(x, y, SEAT_COL[seat], buf);
    float nameEnd = x + width(buf) + 4 * sc;
    int lv = SeatPu<uint16_t>(seat, SeatPu<uint8_t>(seat, PU_OFF) ? PU_LEVEL_OFF : PU_LEVEL_ON);
    snprintf(buf, sizeof buf, "LV %d", lv);
    float lvx = x + (w - width(buf)) / 2;
    text(lvx > nameEnd ? lvx : nameEnd, y, IM_COL32(255, 255, 255, 255), buf);   // 座位名带 OUT / LEFT 时往右让
    snprintf(buf, sizeof buf, "x%d", At<int>(st + ST_LIVES));
    text(x + w - width(buf), y, IM_COL32(255, 255, 255, 255), buf);
    y += lh + PANEL_ROW_GAP * sc;
    auto row = [&](const char* name, float frac, ImU32 col, const char* extra) {
        text(x, y, IM_COL32(220, 220, 220, 255), name);
        if (frac >= 0) {
            float bx = x + 26 * sc, bw = w - 26 * sc - (extra ? width(extra) + 3 * sc : 0);
            Bar(dl, ImVec2(bx, y + lh * 0.25f), bw, lh * 0.5f, frac, col);
        }
        if (extra) text(x + w - width(extra), y, IM_COL32(255, 255, 255, 255), extra);
        y += lh + PANEL_ROW_GAP * sc;
    };
    auto frac = [](int a, int b) { return b > 0 ? (float)a / b : 0.f; };
    row("HP", frac(At<int>(st + 0xB4), At<int>(st + 0xB8)), IM_COL32(230, 60, 60, 255), nullptr);
    snprintf(buf, sizeof buf, "%d", At<int>(st + 0xBC));
    row("BR", frac(At<int>(st + 0xC0), At<int>(st + 0xC4)), IM_COL32(80, 140, 255, 255), buf);
    row("PW", frac(At<int>(st + ST_POWER), POWER_MAX), IM_COL32(120, 230, 120, 255), nullptr);
    row("FL", frac(At<int>(st + ST_FLASH), FLASH_MAX), IM_COL32(255, 220, 80, 255), nullptr);
    snprintf(buf, sizeof buf, "%d", BombCount(seat, st));
    row("Bomb", -1, 0, buf);
}

} // namespace

// 联机状态：一行白字，在场地上方的边框里（游戏中）或画面左下角（菜单）。出问题时是红字 / 黄字
void DrawNetUi()
{
    NetStatusInfo n;
    NetStatus(&n);
    if (n.role != NET_HOST && n.role != NET_GUEST) return;
    ImGuiIO& io = ImGui::GetIO();
    float sc = io.DisplaySize.x / 640.0f;
    char buf[256];
    ImU32 col = IM_COL32(255, 255, 255, 255);
    const char* seatName = SEAT_NAME[n.seat >= 0 && n.seat < MAX_SEATS ? n.seat : 0];
    if (n.buildMismatch && !n.connected) {
        snprintf(buf, sizeof buf, "%s", Tr(TX_O_MISMATCH));
        col = IM_COL32(255, 90, 90, 255);
    } else if (n.full && !n.connected) {
        snprintf(buf, sizeof buf, "%s", Tr(TX_O_FULL));
        col = IM_COL32(255, 90, 90, 255);
    } else if (!n.connected && !n.inGame) {
        if (n.role == NET_HOST) snprintf(buf, sizeof buf, Tr(TX_O_WAIT_GUESTS), n.guests, n.seats - 1, n.port);
        else snprintf(buf, sizeof buf, "%s", Tr(TX_O_CONNECTING));
        col = IM_COL32(255, 220, 120, 255);
    } else if (n.broken || g_netSolo) {
        snprintf(buf, sizeof buf, "%s", Tr(TX_O_LOST));
        col = IM_COL32(255, 90, 90, 255);
    } else if (n.desyncFrame >= 0) {
        snprintf(buf, sizeof buf, Tr(TX_O_DESYNC), SEAT_NAME[n.desyncSeat >= 0 && n.desyncSeat < MAX_SEATS ? n.desyncSeat : 0], n.desyncFrame);
        col = IM_COL32(255, 90, 90, 255);
    } else {
        char who[48], delay[64], rtt[48];
        if (n.role == NET_HOST) snprintf(who, sizeof who, "%s", Tr(TX_O_HOST));
        else snprintf(who, sizeof who, "%s %s", Tr(TX_O_GUEST), seatName);
        snprintf(delay, sizeof delay, Tr(TX_O_DELAY), n.delay);
        if (n.role == NET_HOST && n.seats > 2) snprintf(rtt, sizeof rtt, "%d / %d", n.rtt[1], n.rtt[2]);
        else snprintf(rtt, sizeof rtt, "%d", n.role == NET_HOST ? n.rtt[1] : n.rtt[0]);
        snprintf(buf, sizeof buf, "%s   %s%s   RTT %s ms", who, delay, n.autoDelay ? Tr(TX_O_AUTO) : "", rtt);
        if (n.inGame && n.rollback) {   // 回滚模式：最近 1 秒最深回滚了几帧
            char rb[48];
            snprintf(rb, sizeof rb, Tr(TX_O_ROLLBACK), g_rbDepthShown);
            size_t len = strlen(buf);
            snprintf(buf + len, sizeof buf - len, "   %s", rb);
        }
        for (int s = 1; s < MAX_SEATS; ++s)   // 客机：和另一个客机直连通了
            if (n.direct >> s & 1) {
                char d[48];
                snprintf(d, sizeof d, Tr(TX_O_DIRECT), SEAT_NAME[s], n.rtt[s]);
                size_t len = strlen(buf);
                snprintf(buf + len, sizeof buf - len, "   %s", d);
            }
        for (int s = 1; s < MAX_SEATS; ++s)
            if (n.left >> s & 1) {
                char left[48];
                snprintf(left, sizeof left, Tr(TX_O_LEFT), SEAT_NAME[s]);
                size_t len = strlen(buf);
                snprintf(buf + len, sizeof buf - len, "   %s", left);
            }
    }
    ImVec2 ts = ImGui::CalcTextSize(buf);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 at;
    if (g_stg) {   // 场地上方的边框（场地从 y = STG+0x24 开始）
        float top = At<float>(g_stg + STG_FIELD_Y) * sc;
        at = ImVec2((At<float>(g_stg + STG_FIELD_X) + 4) * sc, (top - ts.y) / 2 > 0 ? (top - ts.y) / 2 : 0);
    } else {
        at = ImVec2(6 * sc, io.DisplaySize.y - ts.y - 6 * sc);
        dl->AddRectFilled(ImVec2(at.x - 3, at.y - 1), ImVec2(at.x + ts.x + 3, at.y + ts.y + 1), IM_COL32(0, 0, 0, 140), 3);
    }
    dl->AddText(ImVec2(at.x + 1, at.y + 1), IM_COL32(0, 0, 0, 200), buf);
    dl->AddText(at, col, buf);
}

void DrawCoopUi()
{
    DrawNetUi();
    if (!Coop()) return;
    ImGuiIO& io = ImGui::GetIO();
    float sc = io.DisplaySize.x / 640.0f;
    float fx = At<float>(g_stg + STG_FIELD_X), fy = At<float>(g_stg + STG_FIELD_Y);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    bool anyAlive = AliveCount() > 0;
    for (int seat = 0; seat < g_seats; ++seat) {
        bool ghost = Ghost(seat);
        if (!Alive(seat) && !(ghost && anyAlive)) continue;
        float* p = PosOf(seat);
        ImU32 col = ghost ? (SEAT_COL[seat] & 0x00FFFFFF) | 0x80000000 : SEAT_COL[seat];
        if (p) Label(dl, ImVec2((fx + p[0]) * sc, (fy + p[1] - 34) * sc), SEAT_NAME[seat], col);
    }
    // 给残机：按住低速的进度圈（紫色）
    for (int seat = 0; seat < g_seats; ++seat) {
        float* p = Alive(seat) ? PosOf(seat) : nullptr;
        if (!p || g_give[seat].hold <= 15) continue;
        ImVec2 c((fx + p[0]) * sc, (fy + p[1]) * sc);
        float a0 = -1.5707963f, frac = (float)g_give[seat].hold / LIFE_HOLD;
        dl->PathArcTo(c, 20 * sc, a0, a0 + 6.2831853f * frac, 32);
        dl->PathStroke(IM_COL32(190, 110, 255, 235), 0, 3 * sc);
    }
    // 其他人的面板（本机自己的在游戏的 HUD 上）。只有一个面板（2 人）时放在副事件框和敌部隊名的框之间（640×480 时：
    // 副事件第 k 格的下沿 ≈ 277.5 + 20.5k，敌部隊名的框从 y≈421 起），不缩放。3 人时两个面板从左下角往上叠
    int viewer = ViewerSeat();
    if (g_seats == 2) {
        for (int seat = 0; seat < g_seats; ++seat)
            if (seat != viewer) Panel(dl, seat, 415 * sc, sc);
    } else {
        float bottom = 474 * sc;
        for (int seat = 0; seat < g_seats; ++seat) {
            if (seat == viewer) continue;
            Panel(dl, seat, bottom, sc);
            bottom -= PanelHeight(sc) + 12 * sc;
        }
    }
}

