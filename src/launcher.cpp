// launcher.cpp —— 游戏启动前（WinMain 之前）弹出的设置窗口：单人 / 本地双人 / 本地三人 / 联机主机 / 联机客机，
// 规则、联机设置、界面语言（默认按 Windows 的界面语言；改了以后窗口按新语言重建）。
// 不读写任何设置文件；设置了环境变量 LE04MULTI_MODE 时不弹窗（自动测试用）。
#include "mod.h"
#include <cstdio>
#include <cstdlib>

namespace {

constexpr uint32_t CALL_WINMAIN = 0x864B93, VA_WINMAIN = 0x615B20;
using WinMainFn = int(__stdcall*)(HINSTANCE, HINSTANCE, LPSTR, int);

// 自机编号 → 名字（选人界面的顺序）
const wchar_t* const SHIP_NAMES[] = {
    L"鳳聯 藪雨", L"燕楽 玄鳥", L"シオン", L"平 文門", L"藤原 伊代真", L"馬立 ツグミ", L"瑞風 天堺",
    L"片埜 宿禰", L"乞骸 セセ", L"鵐 黒巫鳥", L"鵐 頬赤", L"鵐 蒿雀", L"國主 雀巳",
};
constexpr int SHIP_COUNT = sizeof SHIP_NAMES / sizeof SHIP_NAMES[0];

enum {
    ID_SOLO = 100, ID_LOCAL, ID_LOCAL3, ID_HOST, ID_GUEST,
    ID_SHIP0 = 200, ID_SHIP1, ID_SHIP2,   // 本地 2P 的主 / 副 / 支援；3P 的是 +10
    ID_ADDR = 300, ID_PORT, ID_DELAY, ID_SEATS, ID_ROLLBACK,
    ID_AIM = 400, ID_ROUTE, ID_SPELLS,
    ID_IPV6 = 450, ID_COPY_IPV6,
    ID_LANG = 500,
    ID_START = IDOK, ID_QUIT = IDCANCEL,
};

struct Result {
    int role = NET_LOCAL;
    int localSeats = 2;            // 本地几个人
    int ships[MAX_SEATS - 1][3] = { { -1, -1, -1 }, { -1, -1, -1 } };   // 本地 2P / 3P 的自机
    std::string addr;
    int port = 17723, delay = 0;   // 延迟 0 = 自动
    int seats = 2;                 // 主机：联机几个人
    bool rollback = false;         // 主机：回滚模式
    int rules = RULE_AIM_NEAREST | RULE_ROUTE_ALL | RULE_MULTI_SPELLS;
    bool start = false;
};

Result g_res;
constexpr int WINDOW_W = 420;   // 窗口内容宽度（规则的说明文字要放得下）
HFONT g_font;

std::string GetText(HWND dlg, int id)
{
    wchar_t w[256] = {};
    GetDlgItemTextW(dlg, id, w, 256);
    char a[512] = {};
    WideCharToMultiByte(CP_UTF8, 0, w, -1, a, sizeof a, nullptr, nullptr);
    return a;
}

void UpdateEnabled(HWND dlg)
{
    bool local3 = IsDlgButtonChecked(dlg, ID_LOCAL3) == BST_CHECKED;
    bool local = IsDlgButtonChecked(dlg, ID_LOCAL) == BST_CHECKED || local3;
    bool host = IsDlgButtonChecked(dlg, ID_HOST) == BST_CHECKED;
    bool guest = IsDlgButtonChecked(dlg, ID_GUEST) == BST_CHECKED;
    for (int id = ID_SHIP0; id <= ID_SHIP2; ++id) {
        EnableWindow(GetDlgItem(dlg, id), local);
        EnableWindow(GetDlgItem(dlg, id + 10), local3);
    }
    EnableWindow(GetDlgItem(dlg, ID_ADDR), guest);
    EnableWindow(GetDlgItem(dlg, ID_PORT), host || guest);
    EnableWindow(GetDlgItem(dlg, ID_DELAY), host || guest);
    EnableWindow(GetDlgItem(dlg, ID_SEATS), host);
    EnableWindow(GetDlgItem(dlg, ID_ROLLBACK), host);
}

HWND Add(HWND parent, const wchar_t* cls, const std::wstring& text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(0, cls, text.c_str(), WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent, (HMENU)(intptr_t)id, nullptr, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

// 窗口里的当前选择（换语言重建控件时保留）
struct UiState {
    int mode = ID_SOLO;
    int ships[2][3] = {};
    bool aim = true, route = true, spells = true;
    std::wstring addr, port = L"17723", delay = L"0";
    int seats = 0;
    bool rollback = true;   // 回滚默认开
};

UiState ReadState(HWND hwnd)
{
    UiState s;
    for (int id = ID_SOLO; id <= ID_GUEST; ++id)
        if (IsDlgButtonChecked(hwnd, id) == BST_CHECKED) s.mode = id;
    for (int p = 0; p < 2; ++p)
        for (int i = 0; i < 3; ++i) s.ships[p][i] = (int)SendDlgItemMessageW(hwnd, ID_SHIP0 + p * 10 + i, CB_GETCURSEL, 0, 0);
    s.aim = IsDlgButtonChecked(hwnd, ID_AIM) == BST_CHECKED;
    s.route = IsDlgButtonChecked(hwnd, ID_ROUTE) == BST_CHECKED;
    s.spells = IsDlgButtonChecked(hwnd, ID_SPELLS) == BST_CHECKED;
    wchar_t w[256];
    GetDlgItemTextW(hwnd, ID_ADDR, w, 256); s.addr = w;
    GetDlgItemTextW(hwnd, ID_PORT, w, 256); s.port = w;
    GetDlgItemTextW(hwnd, ID_DELAY, w, 256); s.delay = w;
    s.seats = (int)SendDlgItemMessageW(hwnd, ID_SEATS, CB_GETCURSEL, 0, 0);
    s.rollback = IsDlgButtonChecked(hwnd, ID_ROLLBACK) == BST_CHECKED;
    return s;
}

BOOL CALLBACK DestroyChild(HWND c, LPARAM) { DestroyWindow(c); return TRUE; }

// 按当前语言建全部控件
void BuildControls(HWND hwnd, const UiState& st)
{
    SetWindowTextW(hwnd, TrW(TX_TITLE).c_str());
    int y = 12;
    Add(hwnd, L"STATIC", TrW(TX_LANGUAGE), 0, 16, y + 3, 120, 20, -1);
    HWND lang = Add(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 140, y, 120, 200, ID_LANG);
    for (int l = 0; l < LG_COUNT; ++l) SendMessageW(lang, CB_ADDSTRING, 0, (LPARAM)Widen(LangName(l)).c_str());
    SendMessageW(lang, CB_SETCURSEL, g_lang, 0);
    y += 34;

    Add(hwnd, L"STATIC", TrW(TX_MODE), 0, 16, y, 300, 20, -1);
    y += 24;
    static const int MODE_TEXT[] = { TX_SOLO, TX_LOCAL2, TX_LOCAL3, TX_HOST, TX_GUEST };
    for (int i = 0; i < 5; ++i) {
        Add(hwnd, L"BUTTON", TrW(MODE_TEXT[i]), BS_AUTORADIOBUTTON | (i == 0 ? WS_GROUP : 0), 28, y, 330, 22, ID_SOLO + i);
        y += 24;
    }
    y += 10;

    for (int p = 0; p < 2; ++p) {
        Add(hwnd, L"STATIC", TrW(p ? TX_SHIPS_P3 : TX_SHIPS_P2), 0, 16, y, 340, 20, -1);
        y += 24;
        for (int i = 0; i < 3; ++i) {
            HWND cb = Add(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP | WS_GROUP, 28 + i * 118, y, 112, 300,
                ID_SHIP0 + p * 10 + i);
            SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)TrW(TX_SAME_AS_P1).c_str());
            for (int s = 0; s < SHIP_COUNT; ++s) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)SHIP_NAMES[s]);
            SendMessageW(cb, CB_SETCURSEL, st.ships[p][i], 0);
        }
        y += 34;
    }
    y += 4;

    Add(hwnd, L"STATIC", TrW(TX_RULES), 0, 16, y, 340, 20, -1); y += 24;
    Add(hwnd, L"BUTTON", TrW(TX_RULE_AIM), BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP, 28, y, 384, 22, ID_AIM); y += 24;
    Add(hwnd, L"BUTTON", TrW(TX_RULE_ROUTE), BS_AUTOCHECKBOX | WS_TABSTOP, 28, y, 384, 22, ID_ROUTE); y += 24;
    Add(hwnd, L"BUTTON", TrW(TX_RULE_SPELLS), BS_AUTOCHECKBOX | WS_TABSTOP, 28, y, 384, 22, ID_SPELLS); y += 34;
    CheckDlgButton(hwnd, ID_AIM, st.aim ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hwnd, ID_ROUTE, st.route ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hwnd, ID_SPELLS, st.spells ? BST_CHECKED : BST_UNCHECKED);

    Add(hwnd, L"STATIC", TrW(TX_ONLINE), 0, 16, y, 300, 20, -1); y += 24;
    Add(hwnd, L"STATIC", TrW(TX_ADDR), 0, 28, y + 3, 110, 20, -1);
    Add(hwnd, L"EDIT", st.addr, WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP | WS_GROUP, 140, y, 210, 24, ID_ADDR); y += 30;
    Add(hwnd, L"STATIC", TrW(TX_PORT), 0, 28, y + 3, 110, 20, -1);
    Add(hwnd, L"EDIT", st.port, WS_BORDER | ES_NUMBER | WS_TABSTOP, 140, y, 70, 24, ID_PORT);
    Add(hwnd, L"STATIC", TrW(TX_DELAY), 0, 220, y + 3, 80, 20, -1);
    Add(hwnd, L"EDIT", st.delay, WS_BORDER | ES_NUMBER | WS_TABSTOP, 300, y, 50, 24, ID_DELAY); y += 30;
    Add(hwnd, L"STATIC", TrW(TX_SEATS), 0, 28, y + 3, 110, 20, -1);
    {
        HWND cb = Add(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 140, y, 100, 200, ID_SEATS);
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)TrW(TX_SEATS2).c_str());
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)TrW(TX_SEATS3).c_str());
        SendMessageW(cb, CB_SETCURSEL, st.seats, 0);
    }
    Add(hwnd, L"BUTTON", TrW(TX_ROLLBACK), BS_AUTOCHECKBOX | WS_TABSTOP, 250, y, 120, 24, ID_ROLLBACK);
    CheckDlgButton(hwnd, ID_ROLLBACK, st.rollback ? BST_CHECKED : BST_UNCHECKED);
    y += 30;
    // 本机的公网 IPv6：做主机时把它告诉客机（客机在"主机地址"里直接填）
    std::string v6 = NetLocalIPv6();
    Add(hwnd, L"STATIC", TrW(TX_MY_IPV6), 0, 28, y + 3, 110, 20, -1);
    Add(hwnd, L"EDIT", v6.empty() ? TrW(TX_NO_IPV6) : Widen(v6.c_str()), WS_BORDER | ES_AUTOHSCROLL | ES_READONLY | WS_TABSTOP,
        140, y, 160, 24, ID_IPV6);
    HWND copy = Add(hwnd, L"BUTTON", TrW(TX_COPY), WS_TABSTOP, 304, y, 46, 24, ID_COPY_IPV6);
    EnableWindow(copy, !v6.empty());
    y += 30;
    Add(hwnd, L"STATIC", TrW(TX_DELAY_NOTE), 0, 28, y, 330, 36, -1); y += 42;

    Add(hwnd, L"BUTTON", TrW(TX_START), BS_DEFPUSHBUTTON | WS_TABSTOP | WS_GROUP, 150, y, 96, 30, ID_START);
    Add(hwnd, L"BUTTON", TrW(TX_QUIT), WS_TABSTOP, 256, y, 96, 30, ID_QUIT);
    CheckRadioButton(hwnd, ID_SOLO, ID_GUEST, st.mode);
    UpdateEnabled(hwnd);
    // 窗口高度按内容收紧（位置不变）
    RECT rc { 0, 0, WINDOW_W, y + 30 + 14 };
    DWORD style = (DWORD)GetWindowLongW(hwnd, GWL_STYLE);
    AdjustWindowRect(&rc, style, FALSE);
    SetWindowPos(hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOMOVE);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        BuildControls(hwnd, UiState {});
        RECT rc;
        GetWindowRect(hwnd, &rc);
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        SetWindowPos(hwnd, nullptr, (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2, 0, 0,
            SWP_NOZORDER | SWP_NOSIZE);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_LANG:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int l = (int)SendDlgItemMessageW(hwnd, ID_LANG, CB_GETCURSEL, 0, 0);
                if (l >= 0 && l < LG_COUNT && l != g_lang) PostMessageW(hwnd, WM_APP, l, 0);   // 不在下拉框自己的通知里销毁它
            }
            return 0;
        case ID_SOLO: case ID_LOCAL: case ID_LOCAL3: case ID_HOST: case ID_GUEST:
            UpdateEnabled(hwnd);
            return 0;
        case ID_COPY_IPV6: {   // 本机 IPv6 地址复制到剪贴板
            wchar_t w[128] = {};
            GetDlgItemTextW(hwnd, ID_IPV6, w, 128);
            size_t bytes = (wcslen(w) + 1) * sizeof(wchar_t);
            if (OpenClipboard(hwnd)) {
                EmptyClipboard();
                if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
                    memcpy(GlobalLock(mem), w, bytes);
                    GlobalUnlock(mem);
                    if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
                }
                CloseClipboard();
            }
            return 0;
        }
        case ID_START: {
            Result r;
            r.role = IsDlgButtonChecked(hwnd, ID_SOLO) ? NET_SOLO : IsDlgButtonChecked(hwnd, ID_HOST) ? NET_HOST
                : IsDlgButtonChecked(hwnd, ID_GUEST) ? NET_GUEST : NET_LOCAL;
            r.localSeats = IsDlgButtonChecked(hwnd, ID_LOCAL3) ? 3 : 2;
            for (int p = 0; p < 2; ++p)
                for (int i = 0; i < 3; ++i) r.ships[p][i] = (int)SendDlgItemMessageW(hwnd, ID_SHIP0 + p * 10 + i, CB_GETCURSEL, 0, 0) - 1;
            r.seats = (int)SendDlgItemMessageW(hwnd, ID_SEATS, CB_GETCURSEL, 0, 0) + 2;
            r.rollback = IsDlgButtonChecked(hwnd, ID_ROLLBACK) == BST_CHECKED;
            r.addr = GetText(hwnd, ID_ADDR);
            r.port = atoi(GetText(hwnd, ID_PORT).c_str());
            r.delay = atoi(GetText(hwnd, ID_DELAY).c_str());
            r.rules = (IsDlgButtonChecked(hwnd, ID_AIM) == BST_CHECKED ? RULE_AIM_NEAREST : 0)
                | (IsDlgButtonChecked(hwnd, ID_ROUTE) == BST_CHECKED ? RULE_ROUTE_ALL : 0)
                | (IsDlgButtonChecked(hwnd, ID_SPELLS) == BST_CHECKED ? RULE_MULTI_SPELLS : 0);
            if (r.role == NET_GUEST && r.addr.empty()) {
                MessageBoxW(hwnd, TrW(TX_NEED_ADDR).c_str(), TrW(TX_TITLE).c_str(), MB_ICONINFORMATION);
                return 0;
            }
            r.start = true;
            g_res = r;
            DestroyWindow(hwnd);
            return 0;
        }
        case ID_QUIT:
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_APP: {   // 换语言：记下当前选择，按新语言重建控件
        UiState st = ReadState(hwnd);
        g_lang = (int)wp;
        EnumChildWindows(hwnd, DestroyChild, 0);
        BuildControls(hwnd, st);
        SetFocus(GetDlgItem(hwnd, ID_LANG));
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool ShowLauncher(HINSTANCE inst)
{
    NONCLIENTMETRICSW ncm { sizeof ncm };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    WNDCLASSW wc {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"le04multi_launcher";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);
    RECT rc { 0, 0, WINDOW_W, 520 };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&rc, style, FALSE);
    HWND hwnd = CreateWindowW(wc.lpszClassName, TrW(TX_TITLE).c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, inst, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(hwnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    UnregisterClassW(wc.lpszClassName, inst);
    DeleteObject(g_font);
    return g_res.start;
}

int __stdcall WinMainHook(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    g_lang = DetectLang();
    if (const char* l = getenv("LE04MULTI_LANG")) g_lang = atoi(l) % LG_COUNT;
    Result r;
    if (const char* mode = getenv("LE04MULTI_MODE")) {   // 自动测试：不弹窗
        r.role = !strcmp(mode, "host") ? NET_HOST : !strcmp(mode, "guest") ? NET_GUEST : !strcmp(mode, "solo") ? NET_SOLO : NET_LOCAL;
        if (!strcmp(mode, "local3")) r.localSeats = 3;
        if (const char* n = getenv("LE04MULTI_SEATS")) r.seats = atoi(n);
        if (const char* a = getenv("LE04MULTI_ADDR")) r.addr = a;
        if (const char* p = getenv("LE04MULTI_PORT")) r.port = atoi(p);
        if (const char* d = getenv("LE04MULTI_DELAY")) r.delay = atoi(d);
        if (const char* e = getenv("LE04MULTI_P2")) sscanf_s(e, "%d,%d,%d", &r.ships[0][0], &r.ships[0][1], &r.ships[0][2]);
        if (const char* e = getenv("LE04MULTI_P3")) sscanf_s(e, "%d,%d,%d", &r.ships[1][0], &r.ships[1][1], &r.ships[1][2]);
        if (const char* a = getenv("LE04MULTI_RULES")) r.rules = atoi(a);
        if (const char* b = getenv("LE04MULTI_ROLLBACK")) r.rollback = atoi(b) != 0;
    } else {
        if (!ShowLauncher(inst)) return 0;
        r = g_res;
    }
    Log("launcher: role %d local %d seats %d addr %s port %d delay %d rules %d lang %d%s", r.role, r.localSeats, r.seats, r.addr.c_str(),
        r.port, r.delay, r.rules, g_lang, r.rollback ? " rollback" : "");
    NetConfigure(r.role, r.addr, r.port, r.delay, r.seats, r.rollback);
    if (!NetStart()) {
        MessageBoxW(nullptr, TrW(TX_NET_FAIL).c_str(), TrW(TX_TITLE).c_str(), MB_ICONWARNING);
        r.role = NET_SOLO;
        NetConfigure(r.role, r.addr, r.port, r.delay, r.seats, false);
    }
    CoopConfigure(r.role, r.localSeats, r.ships, r.rules);
    ArenaUse((r.role == NET_HOST || r.role == NET_GUEST || g_debug) && !getenv("LE04MULTI_NOPOOL"));
    return ((WinMainFn)Va(VA_WINMAIN))(inst, prev, cmd, show);
}

} // namespace

bool InstallLauncher()
{
    return PatchCall(CALL_WINMAIN - 0x400000, (void*)WinMainHook, VA_WINMAIN - 0x400000);
}
