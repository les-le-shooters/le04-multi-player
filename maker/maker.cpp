// Le04_Multi_Maker —— 由游戏文件夹里的 Le04.exe 生成 Le04_Multi.exe（和 tools/patch_exe.py 做的一样，不用装 Python）：
//   - 追加一个 .mult 节，放新的导入表（原有导入 + le04multi.dll!MultiVersion），使游戏启动时加载 le04multi.dll；
//   - 去掉 ASLR（DYNAMIC_BASE），基址固定 0x400000，与反汇编地址一致；
//   - 打开大地址（LARGE_ADDRESS_AWARE）；
//   - 清掉绑定导入目录。
// Le04.exe 本身不改。放进游戏文件夹双击运行
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace {

constexpr uint32_t ORIG_SIZE = 8440320, ORIG_CRC = 0x57359005;   // Le04.exe 1.20f
const char DLL[] = "le04multi.dll", FUNC[] = "MultiVersion";
const wchar_t TITLE[] = L"Le04_Multi";

enum { M_NOT_FOUND, M_BAD_FORMAT, M_VERSION, M_WRITE_FAIL, M_DONE, M_NO_DLL, M_COUNT };
const wchar_t* const TEXT[][M_COUNT] = {
    {   // 中文
        L"没有找到 Le04.exe。\n请把本程序放进游戏文件夹（和 Le04.exe 放在一起）再运行。",
        L"无法识别 Le04.exe 的格式，没有生成 Le04_Multi.exe。",
        L"这个 Le04.exe 不是 1.20f 版（或者已经改过）。\n本 mod 只支持 1.20f，用其他版本生成的 Le04_Multi.exe 多半无法正常运行。\n\n仍然要生成吗？",
        L"无法写入 Le04_Multi.exe（错误 %u）。\n如果 Le04_Multi.exe 正在运行，请先关掉它。如果游戏在 Program Files 之类的文件夹里，请以管理员身份运行本程序。",
        L"已生成 Le04_Multi.exe。",
        L"\n\n注意：这个文件夹里还没有 le04multi.dll，请把它也放进来。",
    },
    {   // English
        L"Le04.exe was not found.\nPut this program in the game folder (next to Le04.exe) and run it again.",
        L"Le04.exe could not be read as a game executable. Le04_Multi.exe was not created.",
        L"This Le04.exe is not version 1.20f (or it has been modified).\nThe mod only supports 1.20f; a Le04_Multi.exe made from another version will most likely not work.\n\nCreate it anyway?",
        L"Could not write Le04_Multi.exe (error %u).\nIf Le04_Multi.exe is running, close it first. If the game is in a folder such as Program Files, run this program as administrator.",
        L"Le04_Multi.exe has been created.",
        L"\n\nNote: le04multi.dll is not in this folder yet. Put it here as well.",
    },
    {   // 日本語
        L"Le04.exe が見つかりません。\nこのプログラムをゲームのフォルダ（Le04.exe と同じ場所）に置いてから実行してください。",
        L"Le04.exe の形式を認識できませんでした。Le04_Multi.exe は作成していません。",
        L"この Le04.exe はバージョン 1.20f ではありません（または改変されています）。\nこの mod は 1.20f 専用です。他のバージョンから作った Le04_Multi.exe はおそらく正常に動きません。\n\nそれでも作成しますか？",
        L"Le04_Multi.exe を書き込めませんでした（エラー %u）。\nLe04_Multi.exe を起動中なら終了してください。ゲームが Program Files などのフォルダにある場合は、このプログラムを管理者として実行してください。",
        L"Le04_Multi.exe を作成しました。",
        L"\n\n注意：このフォルダにはまだ le04multi.dll がありません。一緒に置いてください。",
    },
    {   // 한국어
        L"Le04.exe를 찾을 수 없습니다.\n이 프로그램을 게임 폴더(Le04.exe가 있는 곳)에 넣고 다시 실행하세요.",
        L"Le04.exe의 형식을 인식할 수 없어 Le04_Multi.exe를 만들지 않았습니다.",
        L"이 Le04.exe는 1.20f 버전이 아닙니다(또는 수정된 파일입니다).\n이 모드는 1.20f만 지원하므로, 다른 버전으로 만든 Le04_Multi.exe는 제대로 동작하지 않을 가능성이 높습니다.\n\n그래도 만들까요?",
        L"Le04_Multi.exe를 쓸 수 없습니다(오류 %u).\nLe04_Multi.exe가 실행 중이면 먼저 종료하세요. 게임이 Program Files 같은 폴더에 있으면 이 프로그램을 관리자 권한으로 실행하세요.",
        L"Le04_Multi.exe를 만들었습니다.",
        L"\n\n주의: 이 폴더에 아직 le04multi.dll이 없습니다. 함께 넣어 주세요.",
    },
};

int Lang()
{
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
    case LANG_CHINESE: return 0;
    case LANG_JAPANESE: return 2;
    case LANG_KOREAN: return 3;
    default: return 1;
    }
}

const wchar_t* Tx(int id) { return TEXT[Lang()][id]; }

uint32_t Crc32(const std::vector<uint8_t>& d)
{
    uint32_t table[256];
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
        table[i] = c;
    }
    uint32_t c = 0xFFFFFFFF;
    for (uint8_t b : d) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
    return ~c;
}

bool ReadAll(const std::wstring& path, std::vector<uint8_t>& out)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart < 0x10000000;
    DWORD got = 0;
    if (ok) {
        out.resize((size_t)size.QuadPart);
        ok = ReadFile(f, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size();
    }
    CloseHandle(f);
    return ok;
}

DWORD WriteAll(const std::wstring& path, const std::vector<uint8_t>& data)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD put = 0;
    DWORD err = WriteFile(f, data.data(), (DWORD)data.size(), &put, nullptr) && put == data.size() ? 0 : GetLastError();
    CloseHandle(f);
    if (err) DeleteFileW(path.c_str());
    return err;
}

template <class T> bool Get(const std::vector<uint8_t>& d, size_t off, T* out)
{
    if (off > d.size() || d.size() - off < sizeof(T)) return false;
    memcpy(out, &d[off], sizeof(T));
    return true;
}
template <class T> void Put(std::vector<uint8_t>& d, size_t off, T v) { memcpy(&d[off], &v, sizeof v); }
uint32_t Align(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

// 按 tools/patch_exe.py 生成 Le04_Multi.exe 的内容
bool Patch(const std::vector<uint8_t>& in, std::vector<uint8_t>& out)
{
    IMAGE_DOS_HEADER dos;
    if (!Get(in, 0, &dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) return false;
    size_t nt = (size_t)dos.e_lfanew;
    IMAGE_NT_HEADERS32 h;
    if (!Get(in, nt, &h) || h.Signature != IMAGE_NT_SIGNATURE || h.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        return false;
    size_t opt = nt + 4 + sizeof(IMAGE_FILE_HEADER);
    size_t secTab = opt + h.FileHeader.SizeOfOptionalHeader;
    int nsec = h.FileHeader.NumberOfSections;
    if (nsec <= 0) return false;
    std::vector<IMAGE_SECTION_HEADER> secs(nsec);
    for (int i = 0; i < nsec; ++i)
        if (!Get(in, secTab + i * sizeof(IMAGE_SECTION_HEADER), &secs[i])) return false;
    uint32_t fa = h.OptionalHeader.FileAlignment, sa = h.OptionalHeader.SectionAlignment;
    if (!fa || !sa) return false;
    const IMAGE_SECTION_HEADER& last = secs.back();
    uint32_t newRva = Align(last.VirtualAddress + last.Misc.VirtualSize, sa);
    uint32_t newRaw = Align((uint32_t)in.size(), fa);

    // 原导入描述符
    auto offsetOf = [&](uint32_t rva, size_t* off) {
        for (const auto& s : secs)
            if (rva >= s.VirtualAddress && rva < s.VirtualAddress + std::max(s.Misc.VirtualSize, s.SizeOfRawData)) {
                *off = rva - s.VirtualAddress + s.PointerToRawData;
                return true;
            }
        return false;
    };
    size_t off;
    if (!offsetOf(h.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress, &off)) return false;
    std::vector<IMAGE_IMPORT_DESCRIPTOR> descs;
    for (;;) {
        IMAGE_IMPORT_DESCRIPTOR d;
        if (!Get(in, off, &d) || descs.size() > 1000) return false;
        static const IMAGE_IMPORT_DESCRIPTOR zero = {};
        if (!memcmp(&d, &zero, sizeof d)) break;
        descs.push_back(d);
        off += sizeof d;
    }

    // 新节：[描述符表 (n+2)*20] [ILT 2*4] [IAT 2*4] [hint/name] [dll 名]
    uint32_t n = (uint32_t)descs.size();
    uint32_t tbl = (n + 2) * 20, ilt = tbl, iat = ilt + 8, hint = iat + 8;
    uint32_t name = Align(hint + 2 + (uint32_t)strlen(FUNC) + 1, 2);
    uint32_t size = name + (uint32_t)strlen(DLL) + 1;
    std::vector<uint8_t> sec(Align(size, fa), 0);
    for (uint32_t i = 0; i < n; ++i) memcpy(&sec[i * 20], &descs[i], 20);
    IMAGE_IMPORT_DESCRIPTOR mine = {};
    mine.OriginalFirstThunk = newRva + ilt;
    mine.Name = newRva + name;
    mine.FirstThunk = newRva + iat;
    memcpy(&sec[n * 20], &mine, 20);
    Put<uint32_t>(sec, ilt, newRva + hint);
    Put<uint32_t>(sec, iat, newRva + hint);
    memcpy(&sec[hint + 2], FUNC, strlen(FUNC));
    memcpy(&sec[name], DLL, strlen(DLL));

    // 节表追加一项
    size_t shOff = secTab + nsec * sizeof(IMAGE_SECTION_HEADER);
    if (shOff + sizeof(IMAGE_SECTION_HEADER) > secs[0].PointerToRawData) return false;
    out = in;
    IMAGE_SECTION_HEADER sh = {};
    memcpy(sh.Name, ".mult", 5);
    sh.Misc.VirtualSize = (DWORD)sec.size();
    sh.VirtualAddress = newRva;
    sh.SizeOfRawData = (DWORD)sec.size();
    sh.PointerToRawData = newRaw;
    sh.Characteristics = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE | IMAGE_SCN_CNT_INITIALIZED_DATA;   // 加载器要写 IAT
    Put(out, shOff, sh);
    Put<WORD>(out, nt + 6, (WORD)(nsec + 1));

    // 可选头：SizeOfImage、导入目录、清绑定导入目录、大地址、去 ASLR
    Put<DWORD>(out, opt + offsetof(IMAGE_OPTIONAL_HEADER32, SizeOfImage), Align(newRva + (uint32_t)sec.size(), sa));
    size_t dd = opt + offsetof(IMAGE_OPTIONAL_HEADER32, DataDirectory);
    Put<DWORD>(out, dd + IMAGE_DIRECTORY_ENTRY_IMPORT * 8, newRva);
    Put<DWORD>(out, dd + IMAGE_DIRECTORY_ENTRY_IMPORT * 8 + 4, tbl);
    Put<DWORD>(out, dd + IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT * 8, 0);
    Put<DWORD>(out, dd + IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT * 8 + 4, 0);
    Put<WORD>(out, nt + 4 + offsetof(IMAGE_FILE_HEADER, Characteristics),
        (WORD)(h.FileHeader.Characteristics | IMAGE_FILE_LARGE_ADDRESS_AWARE));
    Put<WORD>(out, opt + offsetof(IMAGE_OPTIONAL_HEADER32, DllCharacteristics),
        (WORD)(h.OptionalHeader.DllCharacteristics & ~IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE));

    out.resize(newRaw, 0);
    out.insert(out.end(), sec.begin(), sec.end());
    return true;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    wchar_t self[MAX_PATH * 2];
    DWORD len = GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
    std::wstring dir(self, len);
    dir.resize(dir.find_last_of(L"\\/") + 1);

    std::vector<uint8_t> in, out;
    if (!ReadAll(dir + L"Le04.exe", in)) {
        MessageBoxW(nullptr, Tx(M_NOT_FOUND), TITLE, MB_ICONERROR);
        return 1;
    }
    if ((in.size() != ORIG_SIZE || Crc32(in) != ORIG_CRC)
        && MessageBoxW(nullptr, Tx(M_VERSION), TITLE, MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES)
        return 1;
    if (!Patch(in, out)) {
        MessageBoxW(nullptr, Tx(M_BAD_FORMAT), TITLE, MB_ICONERROR);
        return 1;
    }
    if (DWORD err = WriteAll(dir + L"Le04_Multi.exe", out)) {
        wchar_t msg[512];
        swprintf(msg, 512, Tx(M_WRITE_FAIL), (unsigned)err);
        MessageBoxW(nullptr, msg, TITLE, MB_ICONERROR);
        return 1;
    }
    std::wstring msg = Tx(M_DONE);
    if (GetFileAttributesW((dir + L"le04multi.dll").c_str()) == INVALID_FILE_ATTRIBUTES) msg += Tx(M_NO_DLL);
    MessageBoxW(nullptr, msg.c_str(), TITLE, MB_ICONINFORMATION);
    return 0;
}
