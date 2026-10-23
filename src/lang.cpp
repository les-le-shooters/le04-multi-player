// lang.cpp —— 界面文字（启动窗口、窗口标题、游戏里的叠加层）：中文 / English / 日本語 / 한국어。
// 默认按 Windows 的界面语言选，启动窗口里可以改（只在这次运行内有效，不写文件）。
#include "mod.h"

namespace {

struct Text { const char* s[LG_COUNT]; };   // UTF-8；顺序同 Lang

const Text TEXTS[TX_COUNT] = {
    // ---- 启动窗口
    /* TX_TITLE */ { u8"連縁天影戦記 多人模式", u8"Len'en 4 Multiplayer", u8"連縁天影戦記 マルチプレイ", u8"連縁天影戦記 멀티플레이" },
    /* TX_LANGUAGE */ { u8"语言", u8"Language", u8"言語", u8"언어" },
    /* TX_MODE */ { u8"模式", u8"Mode", u8"モード", u8"모드" },
    /* TX_SOLO */ { u8"单人", u8"Single player", u8"シングル", u8"싱글" },
    /* TX_LOCAL2 */ { u8"本地双人", u8"Local 2 players", u8"ローカル 2 人", u8"로컬 2인" },
    /* TX_LOCAL3 */ { u8"本地三人", u8"Local 3 players", u8"ローカル 3 人", u8"로컬 3인" },
    /* TX_HOST */ { u8"联机：主机", u8"Online: host", u8"オンライン：ホスト", u8"온라인: 호스트" },
    /* TX_GUEST */ { u8"联机：客机", u8"Online: guest", u8"オンライン：ゲスト", u8"온라인: 게스트" },
    /* TX_SHIPS_P2 */ { u8"本地：2P 的自机（主 / 副 / 支援）", u8"Local: 2P's ships (main / sub / support)",
                        u8"ローカル：2P の自機（メイン / サブ / サポート）", u8"로컬: 2P 기체 (메인 / 서브 / 서포트)" },
    /* TX_SHIPS_P3 */ { u8"本地：3P 的自机（主 / 副 / 支援）", u8"Local: 3P's ships (main / sub / support)",
                        u8"ローカル：3P の自機（メイン / サブ / サポート）", u8"로컬: 3P 기체 (메인 / 서브 / 서포트)" },
    /* TX_SAME_AS_P1 */ { u8"同 1P", u8"Same as 1P", u8"1P と同じ", u8"1P와 같음" },
    /* TX_RULES */ { u8"规则（联机时以主机为准）", u8"Rules (online: the host's settings apply)", u8"ルール（オンラインではホストの設定）",
                     u8"규칙 (온라인에서는 호스트 설정 적용)" },
    /* TX_RULE_AIM */ { u8"自机狙瞄准离敌人最近的机体（不勾选此项时仅瞄准1P）", u8"Aimed shots target the nearest ship (unchecked: 1P only)",
                        u8"自機狙いは一番近い機体を狙う（チェックなしは 1P のみ）", u8"조준탄은 가장 가까운 기체를 노림 (해제 시 1P만)" },
    /* TX_RULE_ROUTE */ { u8"所有自机都能决定路径方向（不勾选此项时仅1P可决定）", u8"Every ship decides the route direction (unchecked: 1P only)",
                          u8"全員で進路の方向を決める（チェックなしは 1P のみ）", u8"모든 기체가 진로 방향을 결정 (해제 시 1P만)" },
    /* TX_RULE_SPELLS */ { u8"部分符牌自机相关攻击对全员发动（不勾选此项时仅对一人）", u8"Some spells' player-targeted attacks hit all (unchecked: 1 ship)",
                           u8"一部スペルの自機対象の攻撃を全員に（チェックなしは 1 人のみ）", u8"일부 스펠의 자기 대상 공격이 전원에게 (해제 시 한 명만)" },
    /* TX_ONLINE */ { u8"联机", u8"Online", u8"オンライン", u8"온라인" },
    /* TX_ADDR */ { u8"主机地址", u8"Host address", u8"ホスト", u8"호스트 주소" },
    /* TX_PORT */ { u8"端口", u8"Port", u8"ポート", u8"포트" },
    /* TX_DELAY */ { u8"延迟（帧）", u8"Delay (frames)", u8"遅延（F）", u8"지연 (프레임)" },
    /* TX_SEATS */ { u8"人数（主机设置）", u8"Players (host)", u8"人数（ホスト）", u8"인원 (호스트 설정)" },
    /* TX_SEATS2 */ { u8"2 人", u8"2 players", u8"2 人", u8"2인" },
    /* TX_SEATS3 */ { u8"3 人", u8"3 players", u8"3 人", u8"3인" },
    /* TX_DELAY_NOTE */ { u8"延迟填0会按网络情况自动调整（推荐）", u8"Delay 0 = adjust automatically to the network (recommended)",
                          u8"遅延 0 = 回線に合わせて自動調整（推奨）", u8"지연 0 = 네트워크에 맞춰 자동 조정 (권장)" },
    /* TX_START */ { u8"开始", u8"Start", u8"開始", u8"시작" },
    /* TX_QUIT */ { u8"退出", u8"Quit", u8"終了", u8"종료" },
    /* TX_NEED_ADDR */ { u8"请填写主机地址（IP 或域名）。", u8"Please enter the host address (IP or domain name).",
                         u8"ホストのアドレス（IP またはドメイン名）を入力してください。", u8"호스트 주소(IP 또는 도메인)를 입력하세요." },
    /* TX_NET_FAIL */ { u8"无法建立网络连接（端口可能被占用）。将以单人模式继续。",
                        u8"Could not open the network connection (the port may be in use). Continuing in single-player mode.",
                        u8"ネットワーク接続を開始できませんでした（ポートが使用中かもしれません）。シングルで続行します。",
                        u8"네트워크 연결을 열 수 없습니다 (포트가 사용 중일 수 있음). 싱글 플레이로 계속합니다." },
    // ---- 开局会合时的窗口标题
    /* TX_W_WAIT_GUESTS */ { u8"等待客机连接…（%d/%d，按Esc取消并单人开始游戏）", u8"Waiting for guests… (%d/%d, press Esc to cancel and play alone)",
                             u8"ゲストの接続待ち…（%d/%d、Esc でキャンセルして 1 人で開始）", u8"게스트 접속 대기 중… (%d/%d, Esc를 누르면 취소하고 혼자 시작)" },
    /* TX_W_WAIT_READY */ { u8"等待客机开始游戏…（%d/%d，按Esc取消并单人开始游戏）", u8"Waiting for guests to start… (%d/%d, press Esc to cancel and play alone)",
                            u8"ゲストの開始待ち…（%d/%d、Esc でキャンセルして 1 人で開始）", u8"게스트 시작 대기 중… (%d/%d, Esc를 누르면 취소하고 혼자 시작)" },
    /* TX_W_FULL */ { u8"主机已满（按Esc取消并单人开始游戏）", u8"The host is full (press Esc to cancel and play alone)",
                      u8"ホストが満員です（Esc でキャンセルして 1 人で開始）", u8"호스트가 꽉 찼습니다 (Esc를 누르면 취소하고 혼자 시작)" },
    /* TX_W_CONNECTING */ { u8"正在连接主机…（按Esc取消并单人开始游戏）", u8"Connecting to the host… (press Esc to cancel and play alone)",
                            u8"ホストに接続中…（Esc でキャンセルして 1 人で開始）", u8"호스트에 접속 중… (Esc를 누르면 취소하고 혼자 시작)" },
    /* TX_W_STARTING */ { u8"正在开始…", u8"Starting…", u8"開始中…", u8"시작 중…" },
    /* TX_W_WAIT_HOST */ { u8"等待主机开始游戏…（按Esc取消并单人开始游戏）", u8"Waiting for the host to start… (press Esc to cancel and play alone)",
                           u8"ホストの開始待ち…（Esc でキャンセルして 1 人で開始）", u8"호스트 시작 대기 중… (Esc를 누르면 취소하고 혼자 시작)" },
    // ---- 游戏里的联机状态
    /* TX_O_HOST */ { u8"主机", u8"Host", u8"ホスト", u8"호스트" },
    /* TX_O_GUEST */ { u8"客机", u8"Guest", u8"ゲスト", u8"게스트" },
    /* TX_O_DELAY */ { u8"延迟：%d帧", u8"Delay: %d f", u8"遅延：%dF", u8"지연: %d프레임" },
    /* TX_O_AUTO */ { u8"（自动）", u8" (auto)", u8"（自動）", u8" (자동)" },
    /* TX_O_MISMATCH */ { u8"联机：版本不一致", u8"Online: version mismatch", u8"オンライン：バージョンが違います", u8"온라인: 버전 불일치" },
    /* TX_O_FULL */ { u8"联机：主机已满", u8"Online: the host is full", u8"オンライン：ホストが満員です", u8"온라인: 호스트가 꽉 찼습니다" },
    /* TX_O_WAIT_GUESTS */ { u8"联机（主机）：等待客机连接…（%d/%d，端口 %d）", u8"Online (host): waiting for guests… (%d/%d, port %d)",
                             u8"オンライン（ホスト）：ゲストの接続待ち…（%d/%d、ポート %d）", u8"온라인 (호스트): 게스트 접속 대기 중… (%d/%d, 포트 %d)" },
    /* TX_O_CONNECTING */ { u8"联机（客机）：正在连接主机…", u8"Online (guest): connecting to the host…", u8"オンライン（ゲスト）：ホストに接続中…",
                            u8"온라인 (게스트): 호스트에 접속 중…" },
    /* TX_O_LOST */ { u8"联机：连接中断，继续单人游戏", u8"Online: connection lost, continuing alone", u8"オンライン：接続が切れました。1 人で続行します",
                      u8"온라인: 연결이 끊겨 혼자 계속합니다" },
    /* TX_O_DESYNC */ { u8"联机：和 %s 已不同步（第 %d 帧）", u8"Online: out of sync with %s (frame %d)", u8"オンライン：%s と同期がずれました（%d フレーム）",
                        u8"온라인: %s와 동기화가 어긋남 (%d 프레임)" },
    /* TX_O_LEFT */ { u8"%s 已退出", u8"%s left", u8"%s 退出", u8"%s 퇴장" },
    // ---- IPv6、直连
    /* TX_MY_IPV6 */ { u8"本机 IPv6", u8"This PC's IPv6", u8"この PC の IPv6", u8"이 PC의 IPv6" },
    /* TX_COPY */ { u8"复制", u8"Copy", u8"コピー", u8"복사" },
    /* TX_NO_IPV6 */ { u8"（没有公网 IPv6 地址）", u8"(no public IPv6 address)", u8"（グローバル IPv6 アドレスなし）", u8"(공인 IPv6 주소 없음)" },
    /* TX_O_DIRECT */ { u8"%s 直连 %d ms", u8"%s direct %d ms", u8"%s 直結 %d ms", u8"%s 직결 %d ms" },
    /* TX_ROLLBACK */ { u8"回滚", u8"Rollback", u8"ロールバック", u8"롤백" },
    /* TX_O_ROLLBACK */ { u8"回滚 %d帧", u8"rollback %d f", u8"ロールバック %dF", u8"롤백 %d프레임" },
};

const char* const LANG_NAMES[LG_COUNT] = { u8"中文", u8"English", u8"日本語", u8"한국어" };

} // namespace

int g_lang = LG_EN;

int DetectLang()
{
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
    case LANG_CHINESE: return LG_ZH;
    case LANG_JAPANESE: return LG_JA;
    case LANG_KOREAN: return LG_KO;
    default: return LG_EN;
    }
}

const char* Tr(int id)
{
    if (id < 0 || id >= TX_COUNT) return "";
    return TEXTS[id].s[g_lang >= 0 && g_lang < LG_COUNT ? g_lang : LG_EN];
}

std::wstring Widen(const char* utf8)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &w[0], n);
    return w;
}

std::wstring TrW(int id) { return Widen(Tr(id)); }

const char* LangName(int lang) { return lang >= 0 && lang < LG_COUNT ? LANG_NAMES[lang] : ""; }

// 当前语言用到的全部文字（游戏里的叠加层按它建字体，只放用得到的字）
std::string AllText()
{
    std::string s;
    for (int id = 0; id < TX_COUNT; ++id) s += Tr(id);
    return s;
}
