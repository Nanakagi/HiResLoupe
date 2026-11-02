// 高解像ルーペ (HiResLoupe) 最小版
// 流れ: 画面を Desktop Duplication で取る -> マウス周りを RTX Video SDK (VSR) で拡大 -> レンズ窓に出す
// SDK の呼び出しは SDK 同梱の samples\RTX_Video_API (rtx_video_api_dx11_impl.cpp) をそのまま使う。
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <d3dcompiler.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#pragma comment(lib, "shcore.lib")
#include <wrl/client.h>
#include <cmath>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <string>
#include <vector>
#include <climits>
#include <atomic>
#include "rtx_video_api.h"

using Microsoft::WRL::ComPtr;

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

static const UINT WM_APP_WHEEL = WM_APP + 1;   // wParam=ホイール量, lParam=Ctrlが押されていたか
static const UINT WM_APP_EXIT = WM_APP + 2;
static const UINT WM_APP_TRAY = WM_APP + 3;    // タスクトレイのアイコンからの通知
static const UINT_PTR TIMER_MENU = 1;           // トレイのメニューが開いている間、描画を続けるタイマー
static const int HOTKEY_TOGGLE = 1, HOTKEY_TOGGLE2 = 2, HOTKEY_EXIT = 3;
static const int HOTKEY_PRESET1 = 4;   // 4,5,6 = 数字キー 1,2,3（レンズの形のプリセット。レンズが出ている間だけ登録）
static const int HOTKEY_LATCH = 7;     // 既定 Ctrl+Space（固定のオン/オフ。レンズが出ている間だけ登録）
static const int HOTKEY_TEST = 8;      // ショートカットの変更で、使えるかを試しに登録するだけ
static const UINT_PTR CMD_HK = 120;    // CMD_HK + 0..5 = ショートカットの変更（出す・消す / 閉じる / 固定 / 形 1..3）
static const int kNHk = 6;
static const UINT_PTR CMD_LIST = 108;  // ショートカット一覧
static const UINT_PTR CMD_TOGGLE = 101, CMD_INI = 102, CMD_LOG = 103, CMD_QUIT = 104, CMD_STARTUP = 105, CMD_RELOAD = 106, CMD_PINCUR = 107, CMD_LANG = 110;   // CMD_LANG + 0..3 = 言語の選択

// ---------------------------------------------------------------- 設定
struct Settings
{
    UINT toggleMods = MOD_WIN | MOD_ALT | MOD_NOREPEAT, toggleKey = 0x5A;
    UINT toggle2Mods = MOD_NOREPEAT, toggle2Key = 0;
    UINT exitMods = MOD_NOREPEAT, exitKey = VK_ESCAPE;
    UINT latchMods = MOD_CONTROL | MOD_NOREPEAT, latchKey = VK_SPACE;
    UINT presetMods[3] = { MOD_NOREPEAT, MOD_NOREPEAT, MOD_NOREPEAT }, presetKey[3] = { '1', '2', '3' };   // 形のプリセット
    int zoomDefault = 200, zoomStep = 25, zoomMax = 400;
    int lensSize = 1024;
    // 設定ファイルに出さない固定値
    static constexpr int zoomMin = 100, lensStep = 10, lensMin = 128;
    static constexpr int vsrQuality = 4;
    static constexpr bool excludeSelf = true;
    static constexpr double lateMs = 3.0;     // 次の合成の何 ms 前に、最新のカーソル位置で位置を直すか
    bool logTiming = false;
    int ahead = 30;              // レンズの見える範囲の先を、何px(画面上)まで描いておくか
    int pad = 8;                 // タイルを超解像するとき、端にとなりの絵を見せるための余白（拡大元のpx。4の倍数）
    bool followMonitor = true;   // カーソルが別のモニターに出たら、レンズもそのモニターへ移す
    bool border = true;          // レンズに銀の縁を付ける
    int borderW = 4;             // 縁の太さ(px)
    bool pinCursor = true;       // 固定中、ルーペの中のマウスの点に自前のカーソルを出す（[lens] pinned_cursor。トレイから切り替え）。
                                 // 既定はオン: 固定したルーペの下に本物のカーソルが入ると、どこにあるか分からなくなるため
    // トレイのメニューなどの文字（[text]。空欄なら英語の既定）
    int lang = 0;   // 0=English 1=日本語 2=中文(简体) 3=한국어（[ui] language。空欄なら Windows の表示言語）
    std::wstring txShow, txHide, txSettings, txLog, txStartup, txQuit, txTip, txLang, txReload, txPinCur;
};

static std::wstring g_dir;   // exe のあるフォルダ（末尾は \）
static Settings g_set;

static void Log(const wchar_t* fmt, ...)
{
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    FILE* f = nullptr;
    if (_wfopen_s(&f, (g_dir + L"HiResLoupe.log").c_str(), L"a, ccs=UTF-8") == 0 && f)
    {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fwprintf(f, L"%04d-%02d-%02d %02d:%02d:%02d %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, buf);
        fclose(f);
    }
}

static std::wstring Trim(std::wstring s)
{
    while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    return s;
}

static UINT KeyFromName(std::wstring t)
{
    std::transform(t.begin(), t.end(), t.begin(), towlower);
    if (t.size() == 1 && iswalnum(t[0])) return (UINT)towupper(t[0]);
    if (t[0] == L'f' && t.size() >= 2 && iswdigit(t[1])) return VK_F1 + (UINT)wcstol(t.c_str() + 1, nullptr, 10) - 1;
    if (t == L"multiply") return VK_MULTIPLY;
    if (t == L"esc" || t == L"escape") return VK_ESCAPE;
    if (t == L"space") return VK_SPACE;
    if (t == L"enter") return VK_RETURN;
    if (t == L"tab") return VK_TAB;
    if (t == L"oem1") return VK_OEM_1;
    if (t == L"oem2") return VK_OEM_2;
    if (t.rfind(L"0x", 0) == 0) return (UINT)wcstoul(t.c_str(), nullptr, 16);
    return 0;
}

// KeyFromName の逆。ini に書いて読み戻せる名前にする
static std::wstring KeyName(UINT vk)
{
    wchar_t b[16];
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::wstring(1, (wchar_t)vk);
    if (vk >= VK_F1 && vk <= VK_F24) { swprintf_s(b, L"F%u", vk - VK_F1 + 1); return b; }
    switch (vk)
    {
    case VK_MULTIPLY: return L"Multiply";
    case VK_ESCAPE: return L"Esc";
    case VK_SPACE: return L"Space";
    case VK_RETURN: return L"Enter";
    case VK_TAB: return L"Tab";
    case VK_OEM_1: return L"Oem1";
    case VK_OEM_2: return L"Oem2";
    }
    swprintf_s(b, L"0x%02X", vk);
    return b;
}

static std::wstring HotkeyName(UINT mods, UINT key)
{
    std::wstring s;
    if (mods & MOD_WIN) s += L"Win+";
    if (mods & MOD_CONTROL) s += L"Ctrl+";
    if (mods & MOD_ALT) s += L"Alt+";
    if (mods & MOD_SHIFT) s += L"Shift+";
    return key ? s + KeyName(key) : s;
}

// "Win+Shift+Oem1" のような文字列を修飾キーとキーに分ける。空や不明なら false
static bool ParseHotkey(std::wstring s, UINT& mods, UINT& key)
{
    mods = MOD_NOREPEAT;
    key = 0;
    while (!s.empty())
    {
        size_t pos = s.find(L'+');
        std::wstring t = Trim(s.substr(0, pos));
        s = (pos == std::wstring::npos) ? L"" : s.substr(pos + 1);
        std::wstring l = t;
        std::transform(l.begin(), l.end(), l.begin(), towlower);
        if (l == L"ctrl" || l == L"control") mods |= MOD_CONTROL;
        else if (l == L"alt") mods |= MOD_ALT;
        else if (l == L"shift") mods |= MOD_SHIFT;
        else if (l == L"win") mods |= MOD_WIN;
        else if (!l.empty()) key = KeyFromName(l);
    }
    return key != 0;
}

// トレイのメニューなどの文字。言語ごとの表（[text] で個別に上書きできる）
static const wchar_t* kLangCodes[4] = { L"en", L"ja", L"zh", L"ko" };
static const wchar_t* kLangNames[4] = { L"English", L"日本語", L"中文(简体)", L"한국어" };   // 言語の一覧は常に自分の言語名で出す
static const wchar_t* kTexts[4][10] = {
    // show, hide, settings, log, startup, quit, tooltip, language, reload, pinned cursor
    { L"Show loupe", L"Hide loupe", L"Open settings file", L"Open log", L"Start with Windows", L"Quit", L"HiResLoupe (Win+Alt+Z)", L"Language", L"Reload settings", L"Show pointer in pinned loupe" },
    { L"ルーペを出す", L"ルーペを消す", L"設定ファイルを開く", L"ログを開く", L"Windows と同時に起動", L"アプリを終了", L"高解像ルーペ (Win+Alt+Z)", L"言語", L"設定を再読み込み", L"固定中のルーペにカーソルを出す" },
    { L"显示放大镜", L"隐藏放大镜", L"打开设置文件", L"打开日志", L"随 Windows 启动", L"退出", L"HiResLoupe (Win+Alt+Z)", L"语言", L"重新加载设置", L"固定时在放大镜中显示指针" },
    { L"돋보기 표시", L"돋보기 숨기기", L"설정 파일 열기", L"로그 열기", L"Windows와 함께 시작", L"종료", L"HiResLoupe (Win+Alt+Z)", L"언어", L"설정 다시 불러오기", L"고정 중 돋보기에 포인터 표시" },
};

// ショートカットの変更の文字（[text] では差し替えない）
static const wchar_t* kHkTexts[4][15] = {
    // menu, toggle, exit, latch, prompt, save, cancel, default, used by other app, used in this app, needs a modifier, shape 1, 2, 3, common shortcut
    { L"Change shortcuts", L"Show / hide loupe", L"Close loupe", L"Pin loupe", L"Press the new shortcut.", L"Save", L"Cancel", L"Default",
      L"Another app is using this shortcut.", L"This shortcut is already used in HiResLoupe.", L"Add Ctrl, Alt, Shift or Win (an F key can be used alone).",
      L"Shape: square", L"Shape: wide 16:9", L"Shape: tall 16:9",
      L"Many apps use this shortcut (copy, paste, etc.). Choose another one." },
    { L"ショートカットを変更", L"ルーペを出す／消す", L"ルーペを閉じる", L"ルーペを固定", L"新しいショートカットを押してください。", L"保存", L"キャンセル", L"初期値",
      L"ほかのアプリがこのショートカットを使っています。", L"このショートカットは HiResLoupe の別の操作で使っています。", L"Ctrl・Alt・Shift・Win のどれかと組み合わせてください（F キーは単独で可）。",
      L"形: 正方形", L"形: 横 16:9", L"形: 縦 16:9",
      L"多くのアプリが使うショートカットです（コピー・貼り付けなど）。別のキーを選んでください。" },
    { L"更改快捷键", L"显示/隐藏放大镜", L"关闭放大镜", L"固定放大镜", L"请按下新的快捷键。", L"保存", L"取消", L"默认",
      L"其他应用正在使用此快捷键。", L"此快捷键已在 HiResLoupe 中使用。", L"请与 Ctrl、Alt、Shift 或 Win 组合（F 键可单独使用）。",
      L"形状：正方形", L"形状：横向 16:9", L"形状：纵向 16:9",
      L"许多应用都使用此快捷键（复制、粘贴等）。请选择其他按键。" },
    { L"단축키 변경", L"돋보기 표시/숨기기", L"돋보기 닫기", L"돋보기 고정", L"새 단축키를 누르세요.", L"저장", L"취소", L"기본값",
      L"다른 앱이 이 단축키를 사용 중입니다.", L"이 단축키는 HiResLoupe에서 이미 사용 중입니다.", L"Ctrl, Alt, Shift, Win 중 하나와 함께 누르세요(F 키는 단독 가능).",
      L"모양: 정사각형", L"모양: 가로 16:9", L"모양: 세로 16:9",
      L"많은 앱이 사용하는 단축키입니다(복사, 붙여넣기 등). 다른 키를 선택하세요." },
};

// アプリの中だけで効くショートカット（Ctrl+C など）は RegisterHotKey では見つからない（ほかのアプリが登録していないので通ってしまう）。
// よく使われるものだけ表で持ち、登録させない
static bool IsCommonShortcut(UINT mods, UINT key)
{
    mods &= ~MOD_NOREPEAT;
    if (mods == MOD_CONTROL && key >= 'A' && key <= 'Z' && wcschr(L"ACVXZYSFPNOWTR", (wchar_t)key)) return true;
    if (mods == (MOD_CONTROL | MOD_SHIFT) && (key == 'Z' || key == 'T' || key == 'N')) return true;
    if (mods == MOD_ALT && (key == VK_F4 || key == VK_TAB || key == VK_SPACE)) return true;
    if (mods == MOD_CONTROL && (key == VK_TAB || key == VK_F4)) return true;
    if (mods == 0 && (key == VK_RETURN || key == VK_TAB || key == VK_SPACE)) return true;
    return false;
}

// 変更できるショートカットの名前（0=出す・消す 1=閉じる 2=固定 3..5=形 1..3）
static const wchar_t* HkLabel(int i)
{
    return kHkTexts[g_set.lang][i < 3 ? 1 + i : 11 + (i - 3)];
}

// ショートカット一覧の文字（[text] では差し替えない）
static const wchar_t* kListTexts[4][17] = {
    // title, toggle, exit, latch, wheel, ., ctrl+wheel, ., hold ctrl, ., ctrl+drag, ., not while pinned, ctrl+right-click, changeable, fixed, wheel while pinned
    { L"Shortcut list", L"Show / hide loupe", L"Close loupe", L"Pin loupe in place (press again to unpin)",
      L"Wheel", L"Zoom (down = in, up = out)", L"Ctrl+Wheel", L"Resize loupe (down = larger)",
      L"Hold Ctrl", L"Freeze the view and move the pointer inside the magnified image", L"Ctrl+drag the frame", L"Resize loupe",
      L" (not while pinned)", L"Ctrl+Right-click", L"[Can be changed] (tray menu > Change shortcuts)", L"[Fixed]",
      L"While pinned: Ctrl+Wheel = zoom, Wheel = scroll the app underneath" },
    { L"ショートカット一覧", L"ルーペを出す／消す", L"ルーペを閉じる", L"ルーペをその場に固定（もう一度で解除）",
      L"ホイール", L"倍率（下で拡大、上で縮小）", L"Ctrl+ホイール", L"ルーペの大きさ（下で大きく）",
      L"Ctrl を押している間", L"絵を止め、拡大像の中でカーソルを動かす", L"Ctrl+枠をドラッグ", L"ルーペの大きさを変える",
      L"（固定中は使えない）", L"Ctrl+右クリック", L"【変更可能】（トレイのメニュー →「ショートカットを変更」）", L"【変更不可】",
      L"固定中: Ctrl+ホイールで倍率、ホイールは下のアプリのスクロール" },
    { L"快捷键一览", L"显示/隐藏放大镜", L"关闭放大镜", L"将放大镜固定在原位（再按一次解除）",
      L"滚轮", L"缩放（向下放大，向上缩小）", L"Ctrl+滚轮", L"调整放大镜大小（向下变大）",
      L"按住 Ctrl", L"冻结画面，在放大图像中移动指针", L"Ctrl+拖动边框", L"调整放大镜大小",
      L"（固定时不可用）", L"Ctrl+右键", L"【可更改】（托盘菜单 →“更改快捷键”）", L"【不可更改】",
      L"固定时：Ctrl+滚轮调整缩放，滚轮用于滚动下方的应用" },
    { L"단축키 목록", L"돋보기 표시/숨기기", L"돋보기 닫기", L"돋보기를 제자리에 고정(다시 누르면 해제)",
      L"휠", L"배율(아래로 확대, 위로 축소)", L"Ctrl+휠", L"돋보기 크기(아래로 크게)",
      L"Ctrl 누르고 있기", L"화면을 멈추고 확대 이미지 안에서 포인터 이동", L"Ctrl+테두리 드래그", L"돋보기 크기 변경",
      L" (고정 중에는 사용 불가)", L"Ctrl+오른쪽 클릭", L"[변경 가능] (트레이 메뉴 → '단축키 변경')", L"[변경 불가]",
      L"고정 중: Ctrl+휠로 배율, 휠은 아래 앱 스크롤" },
};

// "ja" などの文字列から言語番号へ。空や不明なら Windows の表示言語（日・中・韓以外は英語）
static int LangFromName(std::wstring t)
{
    std::transform(t.begin(), t.end(), t.begin(), towlower);
    for (int i = 0; i < 4; i++) if (t == kLangCodes[i]) return i;
    switch (PRIMARYLANGID(GetUserDefaultUILanguage()))
    {
    case LANG_JAPANESE: return 1;
    case LANG_CHINESE: return 2;
    case LANG_KOREAN: return 3;
    }
    return 0;
}

static void LoadTexts()
{
    std::wstring ini = g_dir + L"HiResLoupe.ini";
    std::wstring* dst[10] = { &g_set.txShow, &g_set.txHide, &g_set.txSettings, &g_set.txLog, &g_set.txStartup, &g_set.txQuit, &g_set.txTip, &g_set.txLang, &g_set.txReload, &g_set.txPinCur };
    static const wchar_t* keys[10] = { L"show_loupe", L"hide_loupe", L"open_settings_file", L"open_log", L"start_with_windows", L"quit", L"tray_tooltip", L"language", L"reload_settings", L"pinned_cursor" };
    // [text]: メモ帳が UTF-8 で保存してもそのまま読めるよう、バイト列で取って UTF-8 → だめなら ANSI の順に解釈する
    char iniA[MAX_PATH * 2];
    WideCharToMultiByte(CP_ACP, 0, ini.c_str(), -1, iniA, sizeof iniA, nullptr, nullptr);
    for (int i = 0; i < 10; i++)
    {
        *dst[i] = kTexts[g_set.lang][i];
        char buf[256];
        std::string k(keys[i], keys[i] + wcslen(keys[i]));
        DWORD n = GetPrivateProfileStringA("text", k.c_str(), "", buf, sizeof buf, iniA);
        if (n == 0) continue;
        int w = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, buf, (int)n, nullptr, 0);
        UINT cp = CP_UTF8;
        if (w <= 0) { cp = CP_ACP; w = MultiByteToWideChar(CP_ACP, 0, buf, (int)n, nullptr, 0); }
        if (w <= 0) continue;
        std::wstring r(w, L'\0');
        MultiByteToWideChar(cp, cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0, buf, (int)n, &r[0], w);
        *dst[i] = r;
    }
}

static void LoadSettings()
{
    std::wstring ini = g_dir + L"HiResLoupe.ini";
    auto str = [&](const wchar_t* sec, const wchar_t* key, const wchar_t* def)
    {
        wchar_t buf[128];
        GetPrivateProfileStringW(sec, key, def, buf, _countof(buf), ini.c_str());
        return std::wstring(buf);
    };
    auto num = [&](const wchar_t* sec, const wchar_t* key, int def) { return (int)GetPrivateProfileIntW(sec, key, def, ini.c_str()); };

    UINT m, k;
    if (ParseHotkey(str(L"hotkey", L"toggle", L"Win+Alt+Z"), m, k)) { g_set.toggleMods = m; g_set.toggleKey = k; }
    if (ParseHotkey(str(L"hotkey", L"toggle2", L""), m, k)) { g_set.toggle2Mods = m; g_set.toggle2Key = k; }
    else g_set.toggle2Key = 0;
    if (ParseHotkey(str(L"hotkey", L"exit", L"Esc"), m, k)) { g_set.exitMods = m; g_set.exitKey = k; }
    if (ParseHotkey(str(L"hotkey", L"latch", L"Ctrl+Space"), m, k)) { g_set.latchMods = m; g_set.latchKey = k; }
    for (int i = 0; i < 3; ++i)
    {
        const std::wstring n = std::to_wstring(i + 1);
        if (ParseHotkey(str(L"hotkey", (L"preset" + n).c_str(), n.c_str()), m, k)) { g_set.presetMods[i] = m; g_set.presetKey[i] = k; }
    }
    g_set.zoomDefault = num(L"zoom", L"default", 200);
    g_set.zoomStep = std::max(1, num(L"zoom", L"step", 25));
    g_set.zoomMax = std::max(g_set.zoomMin, num(L"zoom", L"max", 400));
    g_set.lensSize = num(L"lens", L"size", 1024);
    g_set.followMonitor = num(L"lens", L"follow_monitor", 1) != 0;
    g_set.border = num(L"lens", L"border", 1) != 0;
    g_set.borderW = std::clamp(num(L"lens", L"border_width", 4), 1, 16);
    g_set.pinCursor = num(L"lens", L"pinned_cursor", 1) != 0;
    g_set.logTiming = num(L"debug", L"log_timing", 0) != 0;
    g_set.ahead = std::clamp(num(L"debug", L"ahead", 30), 0, 1024);
    g_set.pad = std::clamp(num(L"debug", L"pad", 8), 0, 32) / 4 * 4;

    g_set.lang = LangFromName(str(L"ui", L"language", L""));
    LoadTexts();
}

// 1フレームの各段階の時間（ms）を溜めて、120フレームごとにログへ出す
static double NowMs()
{
    static LARGE_INTEGER fq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart * 1000.0 / fq.QuadPart;
}
struct Timing
{
    double pos = 0, cap = 0, up = 0, present = 0, wall0 = 0;
    double gapMax = 0, lagSum = 0, lagMax = 0;   // フレーム間隔の最大 / 描画の元にした位置と描画後のカーソル位置の差
    int n = 0, slow = 0;                          // 間隔が 25ms 超(vsync を1回以上取りこぼし)の回数
};
static Timing g_tm;
// GPU が拡大+コピーに実際にかけた時間(ms)。タイムスタンプ問い合わせの結果は数フレーム遅れて取れる
struct GpuTimer
{
    static const int N = 4;
    ComPtr<ID3D11Query> dj[N], t0[N], t1[N];
    bool pending[N]{}, open = false;
    int cur = 0;
    double sum = 0, mx = 0;
    int cnt = 0;
    void Init(ID3D11Device* d)
    {
        D3D11_QUERY_DESC q{};
        for (int i = 0; i < N; ++i)
        {
            q.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            d->CreateQuery(&q, &dj[i]);
            q.Query = D3D11_QUERY_TIMESTAMP;
            d->CreateQuery(&q, &t0[i]);
            d->CreateQuery(&q, &t1[i]);
        }
    }
    void Begin(ID3D11DeviceContext* c)
    {
        if (!dj[0] || open || pending[cur]) return;
        c->Begin(dj[cur].Get());
        c->End(t0[cur].Get());
        open = true;
    }
    void End(ID3D11DeviceContext* c)
    {
        if (!open) return;
        c->End(t1[cur].Get());
        c->End(dj[cur].Get());
        pending[cur] = true;
        cur = (cur + 1) % N;
        open = false;
    }
    void Poll(ID3D11DeviceContext* c)
    {
        for (int i = 0; i < N; ++i)
        {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d;
            if (!pending[i] || c->GetData(dj[i].Get(), &d, sizeof d, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
            UINT64 a, b;
            if (!d.Disjoint && c->GetData(t0[i].Get(), &a, sizeof a, 0) == S_OK && c->GetData(t1[i].Get(), &b, sizeof b, 0) == S_OK)
            {
                double ms = (b - a) * 1000.0 / d.Frequency;
                sum += ms;
                mx = std::max(mx, ms);
                ++cnt;
            }
            pending[i] = false;
        }
    }
};
static GpuTimer g_gt;
static double g_prevFrame = 0;
static double g_actT = 0;                         // レンズを出した時刻(ms)。ログの「起動後」の基準
static DWORD g_hookLagMax = 0;                    // マウス移動がフックに届くまでの遅れの最大(ms)

// ---------------------------------------------------------------- GPU（画面取得・超解像・DirectComposition）
struct Gpu
{
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    // 同じ GPU につながったモニターを全部取り込み、デスクトップの座標どおりに 1 枚（desktopTex）へ貼り合わせる
    // （DuplicateOutput の公式の説明: デスクトップ全体を取り込むなら、有効な出力ごとに取り込みを作る）
    struct Out
    {
        ComPtr<IDXGIOutput1> output;
        ComPtr<IDXGIOutputDuplication> dup;
        HMONITOR mon = nullptr;
        RECT rect{};          // デスクトップ座標
        bool got = false;     // 1 枚目が取れたか
    };
    std::vector<Out> outs;
    ComPtr<ID3D11Texture2D> desktopTex;   // 最後に取れた画面（BGRA）
    ComPtr<ID3D11Texture2D> outTex;       // 超解像の結果
    ComPtr<IDCompositionDevice> dcDev;    // DirectComposition のデバイス
    ComPtr<IDCompositionTarget> dcTarget;
    ComPtr<IDCompositionVisual> dcRoot;           // dcVisual（拡大像）と dcBorder（縁）の親
    ComPtr<IDCompositionVisual> dcVisual;
    ComPtr<IDCompositionVisual> dcBorder;         // レンズの縁。拡大像の上に重ねる
    ComPtr<IDCompositionSurface> dcBorderSurf;
    ComPtr<IDCompositionVisual> dcArrow;          // Ctrl 中、レンズの中で出す自前の矢印（本物のカーソルは隠す）
    ComPtr<IDCompositionSurface> dcArrowSurf;
    ComPtr<IDCompositionVisual> dcInv;            // 実験: 反転カーソルの反転部分。DESTINATION_INVERT で下の拡大像を反転する（dcArrow と同じ切り出し・位置）
    ComPtr<IDCompositionSurface> dcInvSurf;
    int borderForW = 0, borderForH = 0;           // 縁を描いたレンズの大きさ
    ComPtr<IDCompositionVirtualSurface> dcSurf;   // 画面全体の拡大像（世界に固定）
    int surfZ = 0;                                // サーフェスを作ったときの倍率(%)
    ComPtr<ID3D11Texture2D> stageTex;             // 部分拡大の出力置き場（必要に応じて大きくするだけ）
    UINT stageW = 0, stageH = 0;
    std::vector<RECT> dirty;                      // 前回から画面が変わった四角（Desktop Duplication の dirty/move rects）
    bool allDirty = false;                        // 変わった所が分からない/最初の1枚。全部描き直す
    // 画素の比較（dirty rects は除外した自分の窓の場所も毎フレーム「変わった」と通知するため、本当に変わったタイルだけを見つける）
    ComPtr<ID3D11Texture2D> prevTex;              // 1つ前の画面
    ComPtr<ID3D11ShaderResourceView> srvNow, srvPrev;
    ComPtr<ID3D11ComputeShader> diffCs;
    ComPtr<ID3D11Buffer> diffBuf, diffStage, diffCb;
    ComPtr<ID3D11UnorderedAccessView> diffUav;
    ID3D11Texture2D* diffFor = nullptr;           // 上の SRV を作ったときの desktopTex
    UINT diffTiles = 0;
    bool diffOk = false, diffFailed = false, newFrame = false;
    LUID luid{};
    RECT monRect{};                               // 取り込んでいる範囲（全モニターを囲む四角。拡大元の画面。サーフェスとタイルもこの大きさ）
    HMONITOR winMon = nullptr;
    RECT winRect{};                               // 窓とレンズのいるモニター。固定中はマウスが別のモニターへ行っても動かさない（取り込みだけ monRect が追う）
    UINT texW = 0, texH = 0, outW = 0, outH = 0;
    bool ngx = false, haveFrame = false;

    void ReleaseSwap();
    bool EnsureFor(HMONITOR hm);
    bool Covers(HMONITOR hm) const;
    bool RectOf(HMONITOR hm, RECT& r) const;
    void Capture(UINT timeoutMs);
    bool Upscale(const RECT& src, UINT w, UINT h, int quality);
    bool UpscaleSub(const RECT& src, UINT w, UINT h, int quality);
    bool MakeSurface(HWND hwnd, int zq);
    bool MakeDiff(int tx, int ty);
    bool DiffTiles(int tx, int ty, std::vector<uint8_t>& changed);
    void Shutdown();
};

void Gpu::ReleaseSwap()
{
    dcSurf.Reset();
    surfZ = 0;
    dcBorderSurf.Reset();
    borderForW = borderForH = 0;
    dcBorder.Reset();
    dcArrowSurf.Reset();
    dcArrow.Reset();
    dcInvSurf.Reset();
    dcInv.Reset();
    dcRoot.Reset();
    dcVisual.Reset();
    dcTarget.Reset();
    dcDev.Reset();
}

void Gpu::Shutdown()
{
    ReleaseSwap();
    outs.clear();
    desktopTex.Reset();
    prevTex.Reset(); srvNow.Reset(); srvPrev.Reset(); diffUav.Reset(); diffBuf.Reset(); diffStage.Reset(); diffCb.Reset(); diffCs.Reset();
    diffFor = nullptr; diffOk = false;
    outTex.Reset();
    if (ngx) { rtx_video_api_dx11_shutdown(); ngx = false; }
    ctx.Reset();
    dev.Reset();
    adapter.Reset();
    haveFrame = false;
}

bool Gpu::Covers(HMONITOR hm) const
{
    for (const Out& o : outs) if (o.mon == hm) return true;
    return false;
}

bool Gpu::RectOf(HMONITOR hm, RECT& r) const
{
    for (const Out& o : outs) if (o.mon == hm) { r = o.rect; return true; }
    return false;
}

// hm のつながった GPU の、デスクトップに出ているモニターを全部取り込む用意をする
bool Gpu::EnsureFor(HMONITOR hm)
{
    if (dev && Covers(hm)) return true;

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IDXGIAdapter1> foundAd;
    for (UINT a = 0; !foundAd; ++a)
    {
        ComPtr<IDXGIAdapter1> ad;
        if (factory->EnumAdapters1(a, &ad) != S_OK) break;
        for (UINT o = 0;; ++o)
        {
            ComPtr<IDXGIOutput> out;
            if (ad->EnumOutputs(o, &out) != S_OK) break;
            DXGI_OUTPUT_DESC d;
            out->GetDesc(&d);
            if (d.Monitor == hm) { foundAd = ad; break; }
        }
    }
    if (!foundAd) { Log(L"モニターに対応する出力が見つからない"); return false; }

    DXGI_ADAPTER_DESC1 ad;
    foundAd->GetDesc1(&ad);
    if (!dev || ad.AdapterLuid.LowPart != luid.LowPart || ad.AdapterLuid.HighPart != luid.HighPart)
    {
        Shutdown();
        HRESULT hr = D3D11CreateDevice(foundAd.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
        if (FAILED(hr)) { Log(L"D3D11CreateDevice 失敗 hr=0x%08X", (unsigned)hr); return false; }
        adapter = foundAd;
        luid = ad.AdapterLuid;
        if (rtx_video_api_dx11_create(dev.Get(), API_BOOL_FAIL, API_BOOL_SUCCESS) != API_BOOL_SUCCESS)
        {
            Log(L"VSR の初期化に失敗（nvngx_vsr.dll が exe の隣にあるか、RTXのGPU・ドライバか確認）");
            return false;
        }
        ngx = true;
    }

    // この GPU の出力ごとに取り込みを作る。hm 以外で作れなかったモニターは外す（その場所は黒のまま）
    outs.clear();
    for (UINT o = 0;; ++o)
    {
        ComPtr<IDXGIOutput> out;
        if (foundAd->EnumOutputs(o, &out) != S_OK) break;
        DXGI_OUTPUT_DESC d;
        out->GetDesc(&d);
        if (!d.AttachedToDesktop) continue;
        Out x;
        x.mon = d.Monitor;
        x.rect = d.DesktopCoordinates;
        if (FAILED(out.As(&x.output))) continue;
        HRESULT hr = x.output->DuplicateOutput(dev.Get(), &x.dup);
        if (FAILED(hr))
        {
            Log(L"DuplicateOutput 失敗 hr=0x%08X (%ld,%ld)-(%ld,%ld)", (unsigned)hr, x.rect.left, x.rect.top, x.rect.right, x.rect.bottom);
            if (d.Monitor == hm) { outs.clear(); return false; }
            continue;
        }
        outs.push_back(std::move(x));
    }
    if (!Covers(hm)) { outs.clear(); return false; }
    monRect = outs[0].rect;
    for (const Out& x : outs) UnionRect(&monRect, &monRect, &x.rect);

    texW = (UINT)(monRect.right - monRect.left);
    texH = (UINT)(monRect.bottom - monRect.top);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = texW;
    td.Height = texH;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;   // RENDER_TARGET はモニターの無いすき間を黒で埋めるため
    desktopTex.Reset();
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &desktopTex))) return false;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (SUCCEEDED(dev->CreateRenderTargetView(desktopTex.Get(), nullptr, &rtv)))
    {
        const float black[4] = { 0, 0, 0, 1 };
        ctx->ClearRenderTargetView(rtv.Get(), black);
    }
    haveFrame = false;
    Log(L"取り込みの範囲: (%ld,%ld)-(%ld,%ld) モニター%u台", monRect.left, monRect.top, monRect.right, monRect.bottom, (unsigned)outs.size());
    return true;
}

// 各モニターの新しい絵を desktopTex のその場所へ貼る。1 枚目が取れていないモニターだけ timeoutMs まで待つ
void Gpu::Capture(UINT timeoutMs)
{
    bool prevSaved = false;
    for (Out& o : outs)
    {
        if (!o.dup)   // 画面の切り替え(UACなど)のあと取り直す。変わった所は分からない
        {
            allDirty = true;
            if (FAILED(o.output->DuplicateOutput(dev.Get(), &o.dup))) { o.dup.Reset(); continue; }
        }
        DXGI_OUTDUPL_FRAME_INFO info;
        ComPtr<IDXGIResource> res;
        HRESULT hr = o.dup->AcquireNextFrame(o.got ? 0 : timeoutMs, &info, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (hr == DXGI_ERROR_ACCESS_LOST) { o.dup.Reset(); continue; }
        if (FAILED(hr)) continue;
        if (info.LastPresentTime.QuadPart != 0)
        {
            ComPtr<ID3D11Texture2D> t;
            if (SUCCEEDED(res.As(&t)))
            {
                if (!prevSaved && haveFrame && prevTex && diffFor == desktopTex.Get()) ctx->CopyResource(prevTex.Get(), desktopTex.Get());
                prevSaved = true;
                const int dx = o.rect.left - monRect.left, dy = o.rect.top - monRect.top;
                D3D11_TEXTURE2D_DESC sd;
                t->GetDesc(&sd);
                D3D11_BOX box{ 0, 0, 0, std::min<UINT>(sd.Width, (UINT)(o.rect.right - o.rect.left)), std::min<UINT>(sd.Height, (UINT)(o.rect.bottom - o.rect.top)), 1 };
                ctx->CopySubresourceRegion(desktopTex.Get(), 0, (UINT)dx, (UINT)dy, 0, t.Get(), 0, &box);
                if (!o.got) allDirty = true;
                o.got = true;
                newFrame = true;
                // 変わった四角を溜める（描き直すタイルを絞るため。取れなければ全部描き直す）。座標は貼り合わせた絵の上へずらす
                UINT meta = info.TotalMetadataBufferSize;
                if (meta == 0) allDirty = true;
                else
                {
                    std::vector<BYTE> buf(meta);
                    UINT used = 0;
                    auto add = [&](RECT r) { OffsetRect(&r, dx, dy); dirty.push_back(r); };
                    bool mok = SUCCEEDED(o.dup->GetFrameMoveRects(meta, (DXGI_OUTDUPL_MOVE_RECT*)buf.data(), &used));
                    if (mok)
                        for (UINT i = 0; i < used / sizeof(DXGI_OUTDUPL_MOVE_RECT); ++i) add(((DXGI_OUTDUPL_MOVE_RECT*)buf.data())[i].DestinationRect);
                    used = 0;
                    mok = mok && SUCCEEDED(o.dup->GetFrameDirtyRects(meta, (RECT*)buf.data(), &used));
                    if (mok)
                        for (UINT i = 0; i < used / sizeof(RECT); ++i) add(((RECT*)buf.data())[i]);
                    else allDirty = true;
                }
            }
        }
        o.dup->ReleaseFrame();
    }
    if (!haveFrame && !outs.empty())
    {
        bool all = true;
        for (const Out& o : outs) all = all && o.got;
        haveFrame = all;
    }
}

bool Gpu::Upscale(const RECT& src, UINT w, UINT h, int quality)
{
    if (!outTex || outW != w || outH != h)
    {
        outTex.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = outW = w;
        td.Height = outH = h;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &outTex))) return false;
    }
    API_RECT in{ (uint32_t)src.left, (uint32_t)src.top, (uint32_t)src.right, (uint32_t)src.bottom };
    API_RECT out{ 0, 0, w, h };
    API_VSR_Setting s{ (uint32_t)quality };
    return rtx_video_api_dx11_evaluate(desktopTex.Get(), outTex.Get(), in, out, &s, nullptr) == API_BOOL_SUCCESS;
}

// 拡大元の四角 src を (0,0)-(w,h) に拡大する。出力先 stageTex は大きさが足りなければ作り直す（小さくはしない）
bool Gpu::UpscaleSub(const RECT& src, UINT w, UINT h, int quality)
{
    if (!stageTex || stageW < w || stageH < h)
    {
        stageTex.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = stageW = std::max<UINT>((w + 63) / 64 * 64, stageW);
        td.Height = stageH = std::max<UINT>((h + 63) / 64 * 64, stageH);
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &stageTex))) { stageW = stageH = 0; return false; }
    }
    API_RECT in{ (uint32_t)src.left, (uint32_t)src.top, (uint32_t)src.right, (uint32_t)src.bottom };
    API_RECT out{ 0, 0, w, h };
    API_VSR_Setting s{ (uint32_t)quality };
    return rtx_video_api_dx11_evaluate(desktopTex.Get(), stageTex.Get(), in, out, &s, nullptr) == API_BOOL_SUCCESS;
}

// Ctrl 中、レンズの中で出す自前のカーソル。システムの標準カーソル14種の見た目を1枚の絵（横に並べた表）に写し取り、
// 今出ているカーソルの種類に合わせて、その1マスだけを切り出して出す
static const int kCurN = 64;      // 1マスの大きさ(px)
static const int kNCur = 14;      // 種類の数
static const DWORD kCurIds[kNCur] = { 32512, 32649, 32513, 32642, 32643, 32644, 32645, 32646, 32514, 32650, 32648, 32515, 32516, 32651 };
// 並び: 0=矢印 1=手 2=縦棒 3=⤡ 4=⤢ 5=↔ 6=↕ 7=移動 8=待ち 9=起動中 10=禁止 11=十字 12=上矢印 13=ヘルプ（OCR_NORMAL ほか。OEMRESOURCE を定義せず数値で書く）
// 「待ち」(8)と「起動中」(9)はアニメーションカーソル（.ani、18コマ・50ms ごとなど）。全コマを表の後ろへ並べ、時間で切り替える:
//   マス kNCur.. = 待ちのコマ、kNCur+kMaxFrames.. = 起動中のコマ
static const int kIdxWait = 8, kIdxApp = 9;
static const int kMaxFrames = 24;
static const int kNCells = kNCur + 2 * kMaxFrames;   // 表のマスの数
static std::vector<uint8_t> g_invMask;        // 実験: 表の画素ごとの印（1=反転する画素 / 2=反転カーソルの代用の白縁）。g_curAtlas と同じ並び
static std::vector<uint32_t> g_arrowAtlas;    // 実験: 画面へ上げる表。g_curAtlas から反転の画素と代用の縁を抜いたもの
static std::vector<uint32_t> g_invAtlas;      // 実験: 反転用の表。反転する画素だけ不透明の白
static std::vector<uint32_t> g_curAtlas;   // 写し取った表（premultiplied BGRA。幅 kCurN*kNCells、高さ kCurN）
static int g_curHot[kNCells][2];             // 各マスのホットスポット
static int g_aniFrames[2] = {};              // [0]=待ち [1]=起動中 のコマ数（0 = アニメなし、1コマ目だけ使う）
static int g_aniMs[2] = { 50, 50 };          // 1コマの長さ(ms)
static wchar_t g_aniPath[2][MAX_PATH] = {};  // 待ち・起動中の .ani のパス（戻すとき、複製ではなくファイルから読み直す。CopyCursor の複製は動かなくなる）
static std::atomic<bool> g_atlasDirty{false};   // 表をサーフェスへ上げ直す必要がある
static std::atomic<bool> g_arrowFresh{false};   // 表示部品を作り直した（UpdateArrow の記憶を捨てる合図）

// dcomp: 窓ではなく DirectComposition の仮想サーフェスに画面全体の拡大像を持つ。
// 仮想サーフェスは描いた場所だけメモリを使い、描き直した四角だけが更新される（差分だけを足せる）
bool Gpu::MakeSurface(HWND hwnd, int zq)
{
    ReleaseSwap();
    ComPtr<IDXGIDevice> dxgiDev;
    HRESULT hr = dev.As(&dxgiDev);
    if (SUCCEEDED(hr)) hr = DCompositionCreateDevice(dxgiDev.Get(), IID_PPV_ARGS(&dcDev));
    if (SUCCEEDED(hr)) hr = dcDev->CreateTargetForHwnd(hwnd, TRUE, &dcTarget);
    if (SUCCEEDED(hr)) hr = dcDev->CreateVisual(&dcRoot);
    if (SUCCEEDED(hr)) hr = dcDev->CreateVisual(&dcVisual);
    if (SUCCEEDED(hr) && g_set.border) hr = dcDev->CreateVisual(&dcBorder);
    UINT sw = texW * (UINT)zq / 100, sh = texH * (UINT)zq / 100;
    stageTex.Reset();
    stageW = stageH = 0;
    if (SUCCEEDED(hr)) hr = dcDev->CreateVirtualSurface(sw, sh, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_IGNORE, &dcSurf);
    if (FAILED(hr) && dcDev) hr = dcDev->CreateVirtualSurface(sw, sh, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &dcSurf);
    if (SUCCEEDED(hr)) hr = dcVisual->SetContent(dcSurf.Get());
    if (SUCCEEDED(hr)) hr = dcRoot->AddVisual(dcVisual.Get(), TRUE, nullptr);
    if (SUCCEEDED(hr) && dcBorder) hr = dcRoot->AddVisual(dcBorder.Get(), TRUE, dcVisual.Get());   // 縁は拡大像の上
    if (SUCCEEDED(hr))
    {
        // 自前のカーソルは失敗してもルーペ自体は動かす（出せないだけ）。中身（表）は UpdateArrow が上げる
        HRESULT ha = dcDev->CreateVisual(&dcArrow);
        if (SUCCEEDED(ha)) ha = dcDev->CreateSurface(kCurN * kNCells, kCurN, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &dcArrowSurf);
        if (SUCCEEDED(ha)) ha = dcArrow->SetContent(dcArrowSurf.Get());
        g_atlasDirty = true;
        g_arrowFresh = true;
        if (SUCCEEDED(ha)) dcArrow->SetClip(D2D_RECT_F{ 0.0f, 0.0f, (float)kCurN, (float)kCurN });   // 表の1マス目だけ見せておく（全部が並んで出ないように）
        if (SUCCEEDED(ha)) ha = dcRoot->AddVisual(dcArrow.Get(), TRUE, dcBorder ? dcBorder.Get() : dcVisual.Get());
        if (SUCCEEDED(ha)) dcArrow->SetOffsetX(-1000.0f);   // 出すまでは画面の外に置く
        if (FAILED(ha)) { Log(L"矢印の準備に失敗 hr=0x%08X", (unsigned)ha); dcArrowSurf.Reset(); dcArrow.Reset(); }
        else
        {
            // 実験: 反転カーソルの反転部分（白い不透明の画素＝反転する所）。矢印の上に重ねて、下の絵を反転させる
            HRESULT hi = dcDev->CreateVisual(&dcInv);
            if (SUCCEEDED(hi)) hi = dcDev->CreateSurface(kCurN * kNCells, kCurN, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &dcInvSurf);
            if (SUCCEEDED(hi)) hi = dcInv->SetContent(dcInvSurf.Get());
            if (SUCCEEDED(hi)) hi = dcInv->SetCompositeMode(DCOMPOSITION_COMPOSITE_MODE_DESTINATION_INVERT);
            if (SUCCEEDED(hi)) dcInv->SetClip(D2D_RECT_F{ 0.0f, 0.0f, (float)kCurN, (float)kCurN });
            if (SUCCEEDED(hi)) hi = dcRoot->AddVisual(dcInv.Get(), TRUE, dcArrow.Get());
            if (SUCCEEDED(hi)) dcInv->SetOffsetX(-1000.0f);
            if (FAILED(hi)) { Log(L"反転の準備に失敗 hr=0x%08X", (unsigned)hi); dcInvSurf.Reset(); dcInv.Reset(); }
        }
    }
    if (SUCCEEDED(hr)) hr = dcTarget->SetRoot(dcRoot.Get());
    if (SUCCEEDED(hr)) hr = dcDev->Commit();
    if (FAILED(hr)) { Log(L"DirectComposition のサーフェスの準備に失敗 hr=0x%08X", (unsigned)hr); ReleaseSwap(); return false; }
    surfZ = zq;
    Log(L"サーフェスを作成 %ux%u (倍率%d%%)", sw, sh, zq);
    return true;
}

// ---------------------------------------------------------------- レンズ
struct Lens
{
    HWND hwnd = nullptr;
    HANDLE hookThread = nullptr;   // マウスフック専用スレッド
    DWORD hookThreadId = 0;
    bool active = false, frozen = false;
    bool latched = false;   // 固定のキー（既定 Ctrl+Space）で入れる固定。レンズの置き場所だけ止め、中身は今まで通りマウスのまわりを映す
    POINT pin{};            // 固定中のレンズの中心（画面座標）
    int zoom = 200, lensW = 1024, lensH = 1024;
    POINT anchor{};
    RECT lastPos{};
};

static Lens g;
static Gpu gpu;
static HINSTANCE g_inst;
static bool g_presented = false;   // 直近の Frame() が実際に描画まで進んだか
// dcomp: 画面全体の拡大像を「世界に固定した」DirectComposition の仮想サーフェスに持つ。
// サーフェスの (u,v) は、画面の点 ((u/Z)+モニター左, (v/Z)+モニター上) の拡大像で、描き直しても動かない。
// 毎フレーム描くのはマウスのまわり(広め)だけで、レンズは最新のマウス位置に合わせてサーフェスの上を切り出し位置で滑る
struct RenderInfo
{
    bool valid = false;
    int ru = 0, rv = 0;        // 直近に描いた四角のサーフェス上の左上
    int full = 0, fullV = 0;   // その幅と高さ（出力px）
    int w = 0, h = 0;          // レンズの大きさ
    int zq = 200;              // 拡大率(%)。25%刻み
};
static RenderInfo g_ri;

// タイルごとに、1つ前の画面と画素が違うかを GPU で調べる（1タイル = 1スレッドグループ）
static const char kDiffHlsl[] =
    "Texture2D<float4> A : register(t0);\n"
    "Texture2D<float4> B : register(t1);\n"
    "RWStructuredBuffer<uint> O : register(u0);\n"
    "cbuffer C : register(b0) { uint W; uint H; uint TX; uint pad; };\n"
    "groupshared uint f;\n"
    "[numthreads(8,8,1)]\n"
    "void main(uint3 g : SV_GroupID, uint3 t : SV_GroupThreadID, uint gi : SV_GroupIndex)\n"
    "{\n"
    "  if (gi == 0) f = 0;\n"
    "  GroupMemoryBarrierWithGroupSync();\n"
    "  [loop] for (uint j = 0; j < 8; j++)\n"
    "    for (uint i = 0; i < 8; i++)\n"
    "    {\n"
    "      uint2 p = g.xy * 64 + t.xy + uint2(i * 8, j * 8);\n"
    "      if (p.x < W && p.y < H && any(A[p].rgb != B[p].rgb)) { uint o; InterlockedOr(f, 1, o); }\n"
    "    }\n"
    "  GroupMemoryBarrierWithGroupSync();\n"
    "  if (gi == 0) O[g.y * TX + g.x] = f;\n"
    "}\n";

bool Gpu::MakeDiff(int tx, int ty)
{
    if (diffFailed || !desktopTex) return false;
    if (diffOk && diffFor == desktopTex.Get() && diffTiles == (UINT)(tx * ty)) return true;
    diffOk = false;
    prevTex.Reset(); srvNow.Reset(); srvPrev.Reset(); diffUav.Reset(); diffBuf.Reset(); diffStage.Reset(); diffCb.Reset();
    if (!diffCs)
    {
        ComPtr<ID3DBlob> blob, err;
        HRESULT hr = D3DCompile(kDiffHlsl, sizeof(kDiffHlsl) - 1, "diff", nullptr, nullptr, "main", "cs_5_0", 0, 0, &blob, &err);
        if (FAILED(hr) || FAILED(dev->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &diffCs)))
        {
            Log(L"画素比較シェーダーの作成に失敗 hr=0x%08X %S", (unsigned)hr, err ? (const char*)err->GetBufferPointer() : "");
            diffFailed = true;
            return false;
        }
    }
    D3D11_TEXTURE2D_DESC td{};
    desktopTex->GetDesc(&td);
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = td.MiscFlags = 0;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = (UINT)(tx * ty * 4);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = 4;
    D3D11_BUFFER_DESC sd = bd;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    D3D11_BUFFER_DESC cd{};
    cd.ByteWidth = 16;
    cd.Usage = D3D11_USAGE_DEFAULT;
    cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_UNKNOWN;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = (UINT)(tx * ty);
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &prevTex)) ||
        FAILED(dev->CreateShaderResourceView(desktopTex.Get(), nullptr, &srvNow)) ||
        FAILED(dev->CreateShaderResourceView(prevTex.Get(), nullptr, &srvPrev)) ||
        FAILED(dev->CreateBuffer(&bd, nullptr, &diffBuf)) ||
        FAILED(dev->CreateBuffer(&sd, nullptr, &diffStage)) ||
        FAILED(dev->CreateBuffer(&cd, nullptr, &diffCb)) ||
        FAILED(dev->CreateUnorderedAccessView(diffBuf.Get(), &ud, &diffUav)))
    {
        Log(L"画素比較の資源の作成に失敗");
        diffFailed = true;
        return false;
    }
    diffFor = desktopTex.Get();
    diffTiles = (UINT)(tx * ty);
    diffOk = true;
    allDirty = true;   // 前の画面がまだ無いので、最初の1回は全部描き直す
    return true;
}

bool Gpu::DiffTiles(int tx, int ty, std::vector<uint8_t>& changed)
{
    const UINT cb[4] = { texW, texH, (UINT)tx, 0 };
    ctx->UpdateSubresource(diffCb.Get(), 0, nullptr, cb, 0, 0);
    ID3D11ShaderResourceView* srvs[2] = { srvNow.Get(), srvPrev.Get() };
    ID3D11UnorderedAccessView* uav = diffUav.Get();
    ID3D11Buffer* cbp = diffCb.Get();
    ctx->CSSetShader(diffCs.Get(), nullptr, 0);
    ctx->CSSetShaderResources(0, 2, srvs);
    ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    ctx->CSSetConstantBuffers(0, 1, &cbp);
    ctx->Dispatch((UINT)tx, (UINT)ty, 1);
    ID3D11ShaderResourceView* nullSrv[2] = {};
    ID3D11UnorderedAccessView* nullUav = nullptr;
    ctx->CSSetShaderResources(0, 2, nullSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    ctx->CSSetShader(nullptr, nullptr, 0);
    ctx->CopyResource(diffStage.Get(), diffBuf.Get());
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(ctx->Map(diffStage.Get(), 0, D3D11_MAP_READ, 0, &ms))) return false;
    const UINT* p = (const UINT*)ms.pData;
    changed.resize((size_t)tx * ty);
    for (size_t i = 0; i < changed.size(); ++i) changed[i] = p[i] ? 1 : 0;
    ctx->Unmap(diffStage.Get(), 0);
    return true;
}
static const int TILE = 64;   // 世界(画面)を区切るタイルの一辺（拡大元のpx。4の倍数）
static int g_tx = 0, g_ty = 0;                    // タイルの数（横・縦）
static std::vector<uint8_t> g_tile;               // 1=サーフェスに最新の絵が描いてある
static int g_osTiles = 0, g_osRects = 0;          // 描いたタイルの数 / 描いた四角の数
static int g_dgFrames = 0, g_dgDirtyFrames = 0, g_dgRects = 0, g_dgAll = 0;   // 診断: 変化の通知(dirty rects)の集計
static double g_dgArea = 0;
static int g_dgDiff = 0;                          // 診断: 画素比較で本当に変わっていたタイルの数
static RECT g_dgBox{ LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN };
static bool g_forceCommit = false;                // 絵を描いたので、位置が同じでも Commit する
static int g_osN = 0, g_osClamp = 0;              // 切り出し位置を直した回数 / 描いた範囲では足りなかった回数
static double g_osShift = 0;                      // 切り出し位置の直しの大きさの合計(px)
static double g_period = 16.9, g_prevWake = 0;    // 描いてよい合図の間隔(ms)の平均。合成の周期の見積もり
static HANDLE g_lateTimer = nullptr;
static double g_dbgErrX = 0, g_dbgErrY = 0;       // 診断: 計算上、カーソル位置の絵が画面上のカーソルからどれだけずれて出るか(0が正しい)
static int g_dbgSx = 0, g_dbgSy = 0;

// Ctrl+左ボタンでレンズの縁をつかんで大きさを変える。窓はクリックが通るので、つかむ判定はマウスフックで行う。
// 直近に出したレンズの画面上の四角（描画スレッドが書き、フックスレッドが読む）
static std::atomic<int> g_lensL{0}, g_lensT{0}, g_lensWd{0}, g_lensHt{0};
static std::atomic<int> g_dragMode{0};   // 0=つかんでいない, 1=左右の縁, 2=上下の縁（角は3）
static bool g_hot = false;                  // 縁に居て、窓のクリック透過を外している（描画スレッドだけが触る）
static LPCWSTR g_hotCur = IDC_SIZEWE;       // そのとき出すカーソル
static std::atomic<int> g_dragCx{0}, g_dragCy{0};   // つかんだ時点のレンズの中心。ここを動かさず、そこからの距離×2を大きさにする

// 縁で出すカーソル（両端が矢印の白抜き＋黒い縁3px）。システムのカーソルは明るい背景で見えにくいため自前で描く。
// kind: 0=↔, 1=↕, 2=⤡(左上-右下), 3=⤢(右上-左下)。32x32、中心がホットスポット
static HCURSOR EdgeCursor(LPCWSTR id)
{
    static HCURSOR cache[4] = {};
    int kind = id == IDC_SIZEWE ? 0 : id == IDC_SIZENS ? 1 : id == IDC_SIZENWSE ? 2 : 3;
    if (cache[kind]) return cache[kind];
    const int N = 32;
    std::vector<uint8_t> fill(N * N, 0);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x)
        {
            double dx = x - N / 2, dy = y - N / 2;   // 中心を画素 (16,16) の真ん中に置く（棒の太さを奇数pxにできる）
            double u, v;   // u = 矢印の軸に沿った座標, v = 軸と直角
            const double r = std::sqrt(0.5);
            if (kind == 0) { u = dx; v = dy; }
            else if (kind == 1) { u = dy; v = dx; }
            else if (kind == 2) { u = (dx + dy) * r; v = (dy - dx) * r; }
            else { u = (dx - dy) * r; v = (dx + dy) * r; }
            double au = std::fabs(u), av = std::fabs(v);
            bool shaft = au <= 9.0 && av <= 0.5;   // 白い棒は1px
            bool head = au >= 7.5 && au <= 12.5 && av <= (12.5 - au) * 5.5 / 7.5;   // 先が尖った三角（黒縁3pxが32pxの枠からはみ出さない大きさ）
            fill[y * N + x] = shaft || head;
        }
    std::vector<uint32_t> px(N * N, 0);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x)
        {
            if (fill[y * N + x]) { px[y * N + x] = 0xFFFFFFFFu; continue; }
            bool edge = false;
            for (int j = -3; j <= 3 && !edge; ++j)
                for (int i = -3; i <= 3; ++i)
                {
                    if (i * i + j * j > 10) continue;   // 丸く3px広げる
                    int xx = x + i, yy = y + j;
                    if (xx >= 0 && xx < N && yy >= 0 && yy < N && fill[yy * N + xx]) { edge = true; break; }
                }
            if (edge) px[y * N + x] = 0xFF000000u;   // 黒（不透明）
        }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = N;
    bi.bmiHeader.biHeight = -N;   // 上から下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    HBITMAP mask = CreateBitmap(N, N, 1, 1, nullptr);
    if (color && mask && bits)
    {
        memcpy(bits, px.data(), px.size() * 4);
        ICONINFO ii{};
        ii.fIcon = FALSE;
        ii.xHotspot = ii.yHotspot = N / 2;
        ii.hbmMask = mask;
        ii.hbmColor = color;
        cache[kind] = CreateIconIndirect(&ii);
    }
    if (color) DeleteObject(color);
    if (mask) DeleteObject(mask);
    return cache[kind] ? cache[kind] : LoadCursorW(nullptr, id);
}

// 点 p がレンズの縁のどこにあるか。0=縁の近くではない, 1=左右の縁, 2=上下の縁, 3=角。縁から内外それぞれ 24px までをつかめる。
// cur には、そこで出すカーソル（↔ ↕ ⤡ ⤢）を返す
static int EdgeHit(POINT p, LPCWSTR* cur)
{
    const int tol = 24;
    const int L = g_lensL, T = g_lensT, R = L + g_lensWd, B = T + g_lensHt;
    if (g_lensWd <= 0) return 0;
    bool inX = p.x >= L - tol && p.x <= R + tol, inY = p.y >= T - tol && p.y <= B + tol;
    int mode = 0;
    if (inY && (std::abs(p.x - L) <= tol || std::abs(p.x - R) <= tol)) mode |= 1;
    if (inX && (std::abs(p.y - T) <= tol || std::abs(p.y - B) <= tol)) mode |= 2;
    if (cur)
    {
        bool nearL = std::abs(p.x - L) <= std::abs(p.x - R), nearT = std::abs(p.y - T) <= std::abs(p.y - B);
        *cur = mode == 1 ? IDC_SIZEWE : mode == 2 ? IDC_SIZENS : (nearL == nearT ? IDC_SIZENWSE : IDC_SIZENESW);
    }
    return mode;
}

// Ctrl 中（レンズが固定されている間）は、レンズの中のポインタを「拡大像の中を動くカーソル」にする。
// 見えている矢印の位置 V をフックが持ち、本物のポインタ R は V の下に見えている絵の元の位置へ置く（ホバーもクリックも絵と一致する）。
// 縁の帯（縁から内側 24px）とレンズの外では R = V（今まで通り）。マウスの動きはフックで握りつぶし、SetCursorPos で R を置く
static std::atomic<bool> g_frozenA{false};          // g.frozen のフック側コピー
static std::atomic<bool> g_ptOn{false};             // V を使っている
static std::atomic<bool> g_inLens{false};           // V がレンズの内側（縁の帯の内）にあり、R が写像されている
static std::atomic<double> g_vx{0}, g_vy{0};        // V（画面座標）
static std::atomic<double> g_mapBx{0}, g_mapBy{0}, g_mapZ{1};   // R = base + V / Z（描画スレッドが DcompCommit で更新）

static POINT VirtualPos()
{
    return POINT{ (LONG)std::lround(g_vx.load()), (LONG)std::lround(g_vy.load()) };
}

static bool MapMove(POINT pt)
{
    POINT cur;
    GetCursorPos(&cur);
    if (!g_ptOn) { g_vx = cur.x; g_vy = cur.y; g_ptOn = true; }
    const int vl = GetSystemMetrics(SM_XVIRTUALSCREEN), vt = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vr = vl + GetSystemMetrics(SM_CXVIRTUALSCREEN), vb = vt + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    double vx = std::clamp(g_vx + (pt.x - cur.x), (double)vl, (double)(vr - 1));
    double vy = std::clamp(g_vy + (pt.y - cur.y), (double)vt, (double)(vb - 1));
    const int tol = 24;   // 縁をつかめる幅（EdgeHit）と同じ
    const double L = g_lensL, T = g_lensT, R = L + g_lensWd, B = T + g_lensHt;
    const bool inner = vx >= L + tol && vx < R - tol && vy >= T + tol && vy < B - tol;
    POINT np;
    if (inner)
    {
        const double Z = g_mapZ;
        np.x = (LONG)std::lround(g_mapBx + vx / Z);
        np.y = (LONG)std::lround(g_mapBy + vy / Z);
    }
    else np = POINT{ (LONG)std::lround(vx), (LONG)std::lround(vy) };
    g_vx = vx;
    g_vy = vy;
    g_inLens = inner;
    if (np.x != cur.x || np.y != cur.y) SetCursorPos(np.x, np.y);
    return true;
}

static bool g_rbSwallowed = false;   // 右ボタンの DOWN を握りつぶした間 true。UP も握りつぶすまでフックを外さない
static LRESULT CALLBACK MouseHook(int code, WPARAM wp, LPARAM lp)
{
    // Deactivate は g.active を先に下ろすので、閉じたあとに来る UP もここで握りつぶす
    if (code == HC_ACTION && wp == WM_RBUTTONUP && g_rbSwallowed) { g_rbSwallowed = false; return 1; }
    if (code == HC_ACTION && g.active)
    {
        const MSLLHOOKSTRUCT* m = (const MSLLHOOKSTRUCT*)lp;
        if (g_set.logTiming && wp == WM_MOUSEMOVE)
            g_hookLagMax = std::max<DWORD>(g_hookLagMax, GetTickCount() - m->time);
        if (!(m->flags & LLMHF_INJECTED))
        {
            if (wp == WM_MOUSEMOVE && g_frozenA && g_lensWd > 0 && !g_dragMode && (GetAsyncKeyState(VK_CONTROL) & 0x8000))
                return MapMove(m->pt) ? 1 : CallNextHookEx(nullptr, code, wp, lp);
            if (wp == WM_MOUSEWHEEL)
            {
                const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                // 固定中はホイールを下のアプリに通し（スクロールできる）、Ctrl+ホイールを倍率にする。大きさは縁のドラッグだけで変える
                if (g.latched)
                {
                    if (!ctrl) return CallNextHookEx(nullptr, code, wp, lp);
                    PostMessageW(g.hwnd, WM_APP_WHEEL, (WPARAM)(int)(short)HIWORD(m->mouseData), 0);
                    return 1;
                }
                PostMessageW(g.hwnd, WM_APP_WHEEL, (WPARAM)(int)(short)HIWORD(m->mouseData), ctrl ? 1 : 0);
                return 1;
            }
            if (wp == WM_LBUTTONDOWN && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && g_lensWd > 0)
            {
                if (int mode = EdgeHit(g_ptOn ? VirtualPos() : m->pt, nullptr))
                {
                    g_dragCx = g_lensL + g_lensWd / 2;
                    g_dragCy = g_lensT + g_lensHt / 2;
                    g_dragMode = mode;
                    return 1;
                }
            }
            if (wp == WM_LBUTTONUP && g_dragMode) { g_dragMode = 0; g_ptOn = false; return 1; }   // つかんでいる間は本物のポインタが動いたので、V を取り直す
            // 右クリックは下のアプリに通す（固定中・追従中も右クリックメニューを使える）。Ctrl+右クリックで閉じる。DOWN と UP は同じ扱いにそろえる
            if (wp == WM_RBUTTONDOWN && (GetAsyncKeyState(VK_CONTROL) & 0x8000))
            {
                g_rbSwallowed = true;
                PostMessageW(g.hwnd, WM_APP_EXIT, 0, 0);
                return 1;
            }
            if (wp == WM_RBUTTONUP && g_rbSwallowed) { g_rbSwallowed = false; return 1; }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// ローレベルフックは「入れたスレッド」のメッセージループで呼ばれる。描画スレッドが塞がるとマウス入力ごと止まるので、
// 専用スレッド(ブロッキングの GetMessage だけ回す)に入れる。Microsoft の LowLevelMouseProc の注記に従う
static DWORD WINAPI HookThread(LPVOID ready)
{
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);   // メッセージキューを先に作る
    HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, g_inst, 0);
    SetEvent((HANDLE)ready);
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {}
    if (hook) UnhookWindowsHookEx(hook);
    return 0;
}

static void StartHook()
{
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g.hookThread = CreateThread(nullptr, 0, HookThread, ready, 0, &g.hookThreadId);
    if (g.hookThread) WaitForSingleObject(ready, 1000);
    CloseHandle(ready);
}

static void StopHook()
{
    if (!g.hookThread) return;
    // 右クリックで閉じたときは、ボタンを離す(UP)まで待つ。先に外すと UP が下のアプリに届き、右クリックメニューが出る
    for (int i = 0; i < 60 && g_rbSwallowed; ++i) Sleep(5);
    PostThreadMessageW(g.hookThreadId, WM_QUIT, 0, 0);
    WaitForSingleObject(g.hookThread, 1000);
    CloseHandle(g.hookThread);
    g.hookThread = nullptr;
}

// ツールチップなど、あとから出た最前面(topmost)の窓はレンズの上に重なる。topmost 同士は新しく出た方が上になるため、
// 他の topmost の窓が表示されたら次のフレームで自分を最前面に付け直す（Raymond Chen の「常に最前面は保証できない」記事の定番の対処）
static HWINEVENTHOOK g_zHook = nullptr;
static bool g_raise = false;

static void CALLBACK ZOrderEvent(HWINEVENTHOOK, DWORD, HWND h, LONG idObject, LONG, DWORD, DWORD)
{
    if (!g.active || !h || h == g.hwnd || idObject != OBJID_WINDOW) return;
    if (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOPMOST) g_raise = true;
}

// タスクバーと「^」の窓は、Explorer が表示のイベントなしで自分を上げ直す（起動時にレンズがタスクバーの下に出る・「^」の窓がレンズの上に残る）。
// そのため、自分より上に表示中のこれらの窓（とメニュー）があるかを毎フレーム見て、あれば付け直す。他のアプリの窓は見ない（取り合いを避ける）
static bool ShellWindowAboveUs()
{
    wchar_t cls[64];
    for (HWND w = GetWindow(g.hwnd, GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV))
    {
        if (!IsWindowVisible(w) || !GetClassNameW(w, cls, 64)) continue;
        if (!wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd") || !wcscmp(cls, L"TopLevelWindowForOverflowXamlIsland") ||
            !wcscmp(cls, L"#32768")) return true;   // #32768 = メニュー。サブメニューは表示のイベントが来ないことがあるので、ここでも見る
    }
    return false;
}

// レンズの縁（銀の斜面）を w x h の透明サーフェスに描く。レンズの大きさが変わったときだけ作り直す。
// 縁の太さ bw の外側から内側へ 暗い輪郭→明るい→だんだん暗く、のグラデーション。光は左上から当て、上・左は明るく右・下は暗くする（角は斜めに分ける）。
// 戻り値: 作り直したか
static bool EnsureBorder(int w, int h)
{
    if (!gpu.dcBorder || !gpu.dcDev) return false;
    if (gpu.dcBorderSurf && gpu.borderForW == w && gpu.borderForH == h) return false;
    gpu.dcBorderSurf.Reset();
    gpu.borderForW = gpu.borderForH = 0;
    HRESULT hr = gpu.dcDev->CreateSurface(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &gpu.dcBorderSurf);
    if (FAILED(hr)) { Log(L"縁のサーフェスの作成に失敗 hr=0x%08X", (unsigned)hr); return false; }

    const int bw = std::min(g_set.borderW, std::min(w, h) / 2);
    static const float kBase[4] = { 0.0f, 1.00f, 0.80f, 0.58f };   // 外側から内側への明るさ（0は輪郭で別扱い）
    static const float kSide[4] = { 1.00f, 0.85f, 0.55f, 0.45f };  // 上, 左, 右, 下の光の当たり方
    std::vector<uint32_t> px((size_t)w * h, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            int d[4] = { y, x, w - 1 - x, h - 1 - y };
            int side = 0;
            for (int i = 1; i < 4; ++i) if (d[i] < d[side]) side = i;
            if (d[side] >= bw) continue;
            // 太さが bw のとき、外側からの位置を 4 段のグラデーションに写す
            int k = bw == 1 ? 1 : d[side] * 4 / bw;
            float v;
            if (k == 0) v = 0.17f;   // 外の輪郭（暗い）
            else v = 0.93f * kBase[k] * kSide[side] + 0.06f;
            auto c = [&](float t) { return (uint32_t)std::clamp(v * t * 255.0f + 0.5f, 0.0f, 255.0f); };
            px[(size_t)y * w + x] = 0xFF000000u | (c(0.96f) << 16) | (c(0.98f) << 8) | c(1.0f);   // 少し青みのある銀。不透明なので premultiplied のまま
        }

    POINT off{};
    ComPtr<ID3D11Texture2D> tex;
    hr = gpu.dcBorderSurf->BeginDraw(nullptr, IID_PPV_ARGS(&tex), &off);
    if (FAILED(hr)) { Log(L"縁の BeginDraw に失敗 hr=0x%08X", (unsigned)hr); gpu.dcBorderSurf.Reset(); return false; }
    D3D11_BOX box{ (UINT)off.x, (UINT)off.y, 0, (UINT)(off.x + w), (UINT)(off.y + h), 1 };
    gpu.ctx->UpdateSubresource(tex.Get(), 0, &box, px.data(), (UINT)w * 4, 0);
    gpu.dcBorderSurf->EndDraw();
    gpu.dcBorder->SetContent(gpu.dcBorderSurf.Get());
    gpu.borderForW = w;
    gpu.borderForH = h;
    Log(L"縁を作成 %dx%d 太さ%d", w, h, bw);
    return true;
}

// dcomp: 最新のカーソル位置 a に合わせて、サーフェスの切り出し位置とレンズの置き場所を直す。絵は描き直さない。
// サーフェスは世界に固定されているので、切り出し位置はどのフレームの絵が出ていても同じ式になる（絵と位置の直しがずれる余地が無い）。
// 直近に描いた範囲の外へ出る分は切り出せないので、止める（そのぶんだけマウスの真下の絵がずれる）
static void DcompCommit(POINT a)
{
    if (!g_ri.valid || !gpu.dcVisual || !gpu.dcDev) return;
    const RECT& m = gpu.monRect;      // 取り込み側（拡大元の座標の原点）
    const RECT& wr = gpu.winRect;     // 窓・レンズ側
    const int w = g_ri.w, h = g_ri.h;
    const double Z = g_ri.zq / 100.0;
    auto axis = [&](int cur, int pin, int wmn, int wmx, int cmn, int vis, int ru, int full, double& off, int& crop, double& err, bool& clamped, std::atomic<double>& base)
    {
        int left = std::clamp<int>((g.latched ? pin : cur) - vis / 2, wmn, wmx - vis);   // 端で止まるレンズの左（固定中は止めた中心から）
        double c1 = left + vis / 2.0;
        double s1 = g.latched ? cur : cur - (cur - c1) / Z;        // レンズに見せたい拡大元の中心（画面座標）。固定中はマウスの点そのもの
        double want = (s1 - cmn) * Z - vis / 2.0;                    // 欲しい切り出しの左上（サーフェス座標）
        double cu = std::clamp(want, (double)ru, (double)(ru + full - vis));
        clamped = cu != want;
        crop = (int)std::lround(cu);
        off = (left - wmn) - crop;                                  // visual の位置（窓＝レンズのいるモニターの左上が原点）
        base = cmn + (crop - left) / Z;                              // レンズ上の画面座標 p に見えている絵の元の位置 = base + p / Z（Ctrl 中のポインタの写像）
        err = (left - wmn) + (cur - cmn) * Z - crop - (cur - wmn);     // マウスの点の絵が、画面上のマウスからずれる量
        g_osShift += std::fabs(crop - (ru + (full - vis) / 2.0));
    };
    double ox, oy, ex, ey;
    int cx, cy;
    bool kx, ky;
    axis(a.x, g.pin.x, wr.left, wr.right, m.left, w, g_ri.ru, g_ri.full, ox, cx, ex, kx, g_mapBx);
    axis(a.y, g.pin.y, wr.top, wr.bottom, m.top, h, g_ri.rv, g_ri.fullV, oy, cy, ey, ky, g_mapBy);
    g_mapZ = Z;
    static double lastOx = 1e9, lastOy = 1e9;
    static int lastCx = INT_MIN, lastCy = INT_MIN, lastW = 0, lastH = 0;
    g_lensL = wr.left + (int)std::lround(ox + cx);   // 縁をつかむ判定用に、レンズの画面上の四角を残す
    g_lensT = wr.top + (int)std::lround(oy + cy);
    g_lensWd = w;
    g_lensHt = h;
    bool borderNew = EnsureBorder(w, h);
    bool same = !borderNew && !g_forceCommit && ox == lastOx && oy == lastOy && cx == lastCx && cy == lastCy && w == lastW && h == lastH;
    g_forceCommit = false;
    g_dbgErrX = ex;
    g_dbgErrY = ey;
    g_dbgSx = cx;
    g_dbgSy = cy;
    ++g_osN;
    if (kx || ky) ++g_osClamp;
    if (same) return;   // 位置が同じで描き足しも無ければ、合成担当に何も送らない
    lastOx = ox; lastOy = oy; lastCx = cx; lastCy = cy; lastW = w; lastH = h;
    D2D_RECT_F clip{ (float)cx, (float)cy, (float)(cx + w), (float)(cy + h) };
    gpu.dcVisual->SetClip(clip);
    gpu.dcVisual->SetOffsetX((float)ox);
    gpu.dcVisual->SetOffsetY((float)oy);
    if (gpu.dcBorder)
    {
        gpu.dcBorder->SetOffsetX((float)(ox + cx));   // 切り出しの左上が画面に出る場所 = レンズの左上
        gpu.dcBorder->SetOffsetY((float)(oy + cy));
    }
    gpu.dcDev->Commit();
}

// 指定の時刻(ms)まで、メッセージを処理しながら待つ。最後の 1ms ほどは空回しで合わせる
static void WaitUntil(double targetMs)
{
    if (!g_lateTimer) g_lateTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    for (;;)
    {
        double rem = targetMs - NowMs();
        if (rem <= 0) return;
        if (g_lateTimer && rem > 1.5)
        {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)((rem - 1.0) * 10000.0);
            SetWaitableTimer(g_lateTimer, &due, 0, nullptr, nullptr, FALSE);
            if (MsgWaitForMultipleObjects(1, &g_lateTimer, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0 + 1)
            {
                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
                {
                    if (msg.message == WM_QUIT) { PostQuitMessage((int)msg.wParam); return; }
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
        }
        else YieldProcessor();
    }
}

// DWM が持つ画面の更新周期(ms)。qpcRefreshPeriod が異常に小さいときは rateRefresh から計算する（Chromium の vsync_provider_win.cc と同じ考え方）
static bool DwmPeriodMs(double& ms)
{
    DWM_TIMING_INFO ti{};
    ti.cbSize = sizeof(ti);
    if (FAILED(DwmGetCompositionTimingInfo(nullptr, &ti))) return false;
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    double p = (double)ti.qpcRefreshPeriod * 1000.0 / (double)freq.QuadPart;
    if (p < 1.0 && ti.rateRefresh.uiNumerator != 0) p = 1000.0 * ti.rateRefresh.uiDenominator / ti.rateRefresh.uiNumerator;
    if (p < 4.0 || p > 50.0) return false;
    ms = p;
    return true;
}

// 標準カーソルの見た目を表に写し取り、元のカーソルも取っておく（戻すとき、設定の読み直しをせず付け直すため）。
// 白地と黒地に DrawIconEx で描いて、差から透明度を求める（半透明の縁もそのまま）。反転カーソル（縦棒・十字）は黒で描き、白い縁を足す
static HCURSOR g_curOrig[kNCur] = {};
static bool g_curCaptured = false;
static UINT g_curDpi = 0;     // 表を写し取ったときの画面の DPI（ポインタのいる画面と違えば、写し直す）

// 本物のカーソルの大きさは画面ごとの DPI で決まる（100% の画面は 32px、150% の画面は 48px）。表もその大きさで写す（1マス kCurN が上限）
static UINT MonitorDpi(HMONITOR m)
{
    UINT x = 0, y = 0;
    if (m && SUCCEEDED(GetDpiForMonitor(m, MDT_EFFECTIVE_DPI, &x, &y)) && x) return x;
    return GetDpiForSystem();
}
static int CursorSizeFor(UINT dpi) { return std::clamp(GetSystemMetricsForDpi(SM_CXCURSOR, dpi), 16, kCurN); }

// アニメーションカーソル（待ち・起動中）の全コマを表へ写す。システムのハンドル(LoadCursor)ではコマを選べないので、
// レジストリ [Control Panel\Cursors] の .ani ファイルを LoadCursorFromFile で読み、DrawIconEx の istepIfAniCur でコマを描く（コマ数は .ani の anih、1コマの長さは JifRate(1/60秒)）。
// 透明度は、白地と黒地に描いた2枚の差から求める（白-黒 = 255 - α。黒地の色がそのまま掛け済みの色）
static void CaptureAnimated(HDC sdc, int cs)
{
    static const wchar_t* names[2] = { L"Wait", L"AppStarting" };
    const int N = kCurN;
    for (int a = 0; a < 2; ++a)
    {
        g_aniFrames[a] = 0;
        wchar_t raw[MAX_PATH] = {}, path[MAX_PATH] = {};
        DWORD sz = sizeof raw;
        HKEY key;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Cursors", 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
        LONG rr = RegQueryValueExW(key, names[a], nullptr, nullptr, (BYTE*)raw, &sz);
        RegCloseKey(key);
        if (rr != ERROR_SUCCESS || !raw[0]) continue;
        ExpandEnvironmentStringsW(raw, path, MAX_PATH);
        wcscpy_s(g_aniPath[a], path);
        uint8_t head[128] = {};
        size_t hn = 0;
        if (FILE* fp = _wfopen(path, L"rb")) { hn = fread(head, 1, sizeof head, fp); fclose(fp); }
        int steps = 0, jif = 0;
        for (size_t p = 0; p + 40 <= hn; ++p)
            if (memcmp(head + p, "anih", 4) == 0) { memcpy(&steps, head + p + 16, 4); memcpy(&jif, head + p + 36, 4); break; }   // タグ+サイズ(8) の後: cbSize, cFrames, cSteps(+16) ... JifRate(+36)
        if (steps < 2 || steps > kMaxFrames) continue;
        HCURSOR ac = (HCURSOR)LoadImageW(nullptr, path, IMAGE_CURSOR, cs, cs, LR_LOADFROMFILE);   // ポインタのいる画面の大きさで読む
        if (!ac) continue;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = N; bi.bmiHeader.biHeight = -N;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        uint32_t* bits = nullptr;
        HBITMAP dib = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0);
        HDC mdc = CreateCompatibleDC(sdc);
        int got = 0;
        if (dib && mdc)
        {
            HGDIOBJ old = SelectObject(mdc, dib);
            std::vector<uint32_t> white((size_t)N * N);
            for (int s = 0; s < steps; ++s)
            {
                std::fill(bits, bits + (size_t)N * N, 0xFFFFFFFFu);
                if (!DrawIconEx(mdc, 0, 0, ac, 0, 0, (UINT)s, nullptr, DI_NORMAL)) break;
                GdiFlush();
                std::copy(bits, bits + (size_t)N * N, white.begin());
                std::fill(bits, bits + (size_t)N * N, 0xFF000000u);
                if (!DrawIconEx(mdc, 0, 0, ac, 0, 0, (UINT)s, nullptr, DI_NORMAL)) break;
                GdiFlush();
                const int cell = kNCur + a * kMaxFrames + s;
                for (int y = 0; y < N; ++y)
                    for (int x = 0; x < N; ++x)
                    {
                        uint32_t w = white[(size_t)y * N + x], b = bits[(size_t)y * N + x];
                        int diff = (int)(((w >> 16) & 0xFF) - ((b >> 16) & 0xFF) + ((w >> 8) & 0xFF) - ((b >> 8) & 0xFF) + (w & 0xFF) - (b & 0xFF)) / 3;
                        uint32_t al = (uint32_t)std::clamp(255 - diff, 0, 255);
                        g_curAtlas[(size_t)y * N * kNCells + (size_t)cell * N + x] = al ? ((al << 24) | (b & 0xFFFFFF)) : 0;
                    }
                g_curHot[cell][0] = g_curHot[kIdxWait + a][0];
                g_curHot[cell][1] = g_curHot[kIdxWait + a][1];
                ++got;
            }
            SelectObject(mdc, old);
        }
        if (mdc) DeleteDC(mdc);
        if (dib) DeleteObject(dib);
        DestroyCursor(ac);
        if (got == steps)
        {
            g_aniFrames[a] = steps;
            g_aniMs[a] = jif > 0 ? std::max(16, jif * 1000 / 60) : 50;
        }
    }
}

// 標準カーソルを、指定の大きさでファイルから読む（LoadCursor や LR_SHARED の OEM 読みは、システム DPI の大きさのまま返ってくる）。
// レジストリ [Control Panel\Cursors] のパスが空（IBeam・Crosshair など組み込み）なら nullptr
static HCURSOR LoadCursorFileSized(int idx, int cs)
{
    static const wchar_t* names[kNCur] = { L"Arrow", L"Hand", L"IBeam", L"SizeNWSE", L"SizeNESW", L"SizeWE", L"SizeNS", L"SizeAll",
                                            L"Wait", L"AppStarting", L"No", L"Crosshair", L"UpArrow", L"Help" };   // kCurIds と同じ並び
    wchar_t raw[MAX_PATH] = {}, path[MAX_PATH] = {};
    DWORD sz = sizeof raw;
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Cursors", 0, KEY_READ, &key) != ERROR_SUCCESS) return nullptr;
    LONG rr = RegQueryValueExW(key, names[idx], nullptr, nullptr, (BYTE*)raw, &sz);
    RegCloseKey(key);
    if (rr != ERROR_SUCCESS || !raw[0]) return nullptr;
    ExpandEnvironmentStringsW(raw, path, MAX_PATH);
    return (HCURSOR)LoadImageW(nullptr, path, IMAGE_CURSOR, cs, cs, LR_LOADFROMFILE);
}

static void CaptureCursors()
{
    const int N = kCurN;
    g_curDpi = MonitorDpi(gpu.winMon);
    const int cs = CursorSizeFor(g_curDpi);
    g_curAtlas.assign((size_t)N * kNCells * N, 0);
    g_invMask.assign((size_t)N * kNCells * N, 0);
    HDC sdc = GetDC(nullptr);
    BITMAPINFO dbi{};
    dbi.bmiHeader.biSize = sizeof dbi.bmiHeader; dbi.bmiHeader.biWidth = N; dbi.bmiHeader.biHeight = -N;
    dbi.bmiHeader.biPlanes = 1; dbi.bmiHeader.biBitCount = 32; dbi.bmiHeader.biCompression = BI_RGB;
    uint32_t* bits = nullptr;
    HBITMAP dib = CreateDIBSection(sdc, &dbi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0);
    HDC mdc = CreateCompatibleDC(sdc);
    HGDIOBJ oldBmp = (dib && mdc) ? SelectObject(mdc, dib) : nullptr;
    for (int i = 0; i < kNCur && oldBmp; ++i)
    {
        HCURSOR sys = LoadCursorW(nullptr, MAKEINTRESOURCEW(kCurIds[i]));
        if (!sys) continue;
        if (g_curOrig[i]) DestroyCursor(g_curOrig[i]);
        g_curOrig[i] = CopyCursor(sys);   // 戻す用（システムの元の大きさのまま）
        HCURSOR own = LoadCursorFileSized(i, cs);   // 描く用（ポインタのいる画面の大きさ）。読めなければシステムのものを拡縮して描く
        HCURSOR src = own ? own : sys;
        ICONINFO ii{};
        if (!GetIconInfo(src, &ii)) { if (own) DestroyCursor(own); continue; }
        int nat = cs;   // 絵の元の大きさ（色画像があればその幅、無ければ mask の幅）
        {
            BITMAP bm{};
            if (GetObject(ii.hbmColor ? ii.hbmColor : ii.hbmMask, sizeof bm, &bm) && bm.bmWidth > 0) nat = bm.bmWidth;
        }
        const int dw = nat == cs ? 0 : cs;   // 大きさが違うときだけ、cs に拡縮して描く
        g_curHot[i][0] = (int)ii.xHotspot * cs / nat;
        g_curHot[i][1] = (int)ii.yHotspot * cs / nat;
        // 白地と黒地に DrawIconEx で描き、その差から透明度を求める（Windows が本物を描く方法そのまま。CaptureAnimated と同じ）:
        //   白-黒 = 255 - α / 黒地の色 = 掛け済みの色。反転（画面の色を反転）の画素は、白地で黒・黒地で白になるので差が負になる
        std::vector<uint8_t> inv((size_t)N * N, 0);
        bool anyInv = false;
        auto at = [&](int x, int y) -> uint32_t& { return g_curAtlas[(size_t)y * N * kNCells + (size_t)i * N + x]; };
        std::vector<uint32_t> white((size_t)N * N);
        std::fill(bits, bits + (size_t)N * N, 0xFFFFFFFFu);
        DrawIconEx(mdc, 0, 0, src, dw, dw, 0, nullptr, DI_NORMAL);
        GdiFlush();
        std::copy(bits, bits + (size_t)N * N, white.begin());
        std::fill(bits, bits + (size_t)N * N, 0xFF000000u);
        DrawIconEx(mdc, 0, 0, src, dw, dw, 0, nullptr, DI_NORMAL);
        GdiFlush();
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x)
            {
                uint32_t w = white[(size_t)y * N + x], b = bits[(size_t)y * N + x];
                int diff = (int)(((w >> 16) & 0xFF) - ((b >> 16) & 0xFF) + ((w >> 8) & 0xFF) - ((b >> 8) & 0xFF) + (w & 0xFF) - (b & 0xFF)) / 3;
                if (diff < -128) { at(x, y) = 0xFF000000u; inv[(size_t)y * N + x] = 1; g_invMask[(size_t)y * N * kNCells + (size_t)i * N + x] = 1; anyInv = true; continue; }
                uint32_t al = (uint32_t)std::clamp(255 - diff, 0, 255);
                at(x, y) = al ? ((al << 24) | (b & 0xFFFFFF)) : 0;
            }
        if (anyInv)
        {
            // 反転の画素は重ねて描けないので黒で描き、まわりに白の60%の縁を1px足す（白い背景では本物と同じ黒、黒い背景でも見える）
            for (int y = 0; y < N; ++y)
                for (int x = 0; x < N; ++x)
                {
                    if ((at(x, y) >> 24) != 0) continue;
                    bool touch = false;
                    for (int j = -1; j <= 1 && !touch; ++j)
                        for (int k = -1; k <= 1; ++k)
                        {
                            int xx = x + k, yy = y + j;
                            if (xx >= 0 && xx < N && yy >= 0 && yy < N && inv[(size_t)yy * N + xx]) { touch = true; break; }
                        }
                    if (touch) { at(x, y) = 0x99999999u; g_invMask[(size_t)y * N * kNCells + (size_t)i * N + x] = 2; }
                }
        }
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (own) DestroyCursor(own);
    }
    if (oldBmp) SelectObject(mdc, oldBmp);
    if (mdc) DeleteDC(mdc);
    if (dib) DeleteObject(dib);
    CaptureAnimated(sdc, cs);
    ReleaseDC(nullptr, sdc);
    // 実験: 反転の画素と代用の縁は矢印の表から抜き、反転用の表（白）へ移す。診断（--cursorcmp など）は g_curAtlas のまま
    g_arrowAtlas = g_curAtlas;
    g_invAtlas.assign(g_curAtlas.size(), 0);
    for (size_t k = 0; k < g_invMask.size(); ++k)
        if (g_invMask[k]) { g_arrowAtlas[k] = 0; if (g_invMask[k] == 1) g_invAtlas[k] = 0xFFFFFFFFu; }
    g_curCaptured = true;
    g_atlasDirty = true;
}

// 本物のカーソルを隠す／戻す。システムのカーソルを透明なものに差し替える（SetSystemCursor）。
// 透明なカーソルには、ホットスポットに目印を入れる（x=種類の番号, y=31）。ホットスポットは見た目に影響しないので、
// 今どの種類が出ているかを GetCursorInfo の結果から読み取れる。戻すときは、取っておいた元のカーソルを付け直す。
// 異常終了で透明のまま残ったときは、起動時に SPI_SETCURSORS（設定の読み直し）で戻す
static bool g_cursorsHidden = false;
static void SetCursorsHidden(bool hide)
{
    if (hide == g_cursorsHidden) return;
    g_cursorsHidden = hide;
    if (hide)
    {
        if (!g_curCaptured || g_curDpi != MonitorDpi(gpu.winMon)) CaptureCursors();   // 隠す前（本物が見えている間）に、ポインタのいる画面の大きさで写す
        BYTE andMask[32 * 32 / 8], xorMask[32 * 32 / 8];
        memset(andMask, 0xFF, sizeof andMask);   // AND=1, XOR=0 は「画面そのまま」＝透明
        memset(xorMask, 0x00, sizeof xorMask);
        for (int i = 0; i < kNCur; ++i)
            if (HCURSOR c = CreateCursor(g_inst, i, 31, 32, 32, andMask, xorMask)) SetSystemCursor(c, kCurIds[i]);   // 渡したカーソルはシステムが破棄する
        return;
    }
    bool all = g_curCaptured;
    for (int i = 0; i < kNCur && all; ++i) all = g_curOrig[i] != nullptr;
    if (!all) { SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0); return; }
    for (int i = 0; i < kNCur; ++i)
    {
        HCURSOR c = nullptr;
        if ((i == kIdxWait || i == kIdxApp) && g_aniPath[i - kIdxWait][0]) c = LoadCursorFromFileW(g_aniPath[i - kIdxWait]);   // アニメは、複製すると止まるので読み直す
        if (!c) c = CopyCursor(g_curOrig[i]);
        SetSystemCursor(c, kCurIds[i]);
    }
}

// 今出ているカーソルの種類。0..kNCur-1 = 目印から分かった種類, -1 = アプリ独自のカーソル（隠せていない）, -2 = アプリがカーソルを消している
static int CurrentCursorKind()
{
    CURSORINFO ci{ sizeof ci };
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return -2;
    static HCURSOR lastH = nullptr;
    static int lastKind = -1;
    if (ci.hCursor == lastH) return lastKind;
    int kind = -1;
    ICONINFO ii{};
    if (GetIconInfo(ci.hCursor, &ii))
    {
        if (!ii.hbmColor && ii.yHotspot == 31 && ii.xHotspot < (DWORD)kNCur) kind = (int)ii.xHotspot;
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
    }
    // 隠していないとき（固定中）は目印が無いので、システムの共有カーソルと同じ値かで見分ける
    for (int i = 0; i < kNCur && kind < 0; ++i)
        if (ci.hCursor == LoadCursorW(nullptr, MAKEINTRESOURCEW(kCurIds[i]))) kind = i;
    lastH = ci.hCursor;
    lastKind = kind;
    return kind;
}

// Ctrl 中にレンズの中へ入っている間だけ、自前のカーソルを V の位置に出して、本物のカーソルを隠す。
// 固定中（Ctrl なし）は本物を隠さず、ルーペの中でマウスの点が映っている場所に自前のカーソルを出す
static void UpdateArrow()
{
    if (!gpu.dcArrow || !gpu.dcDev) return;
    bool show = g.active && g_ptOn && g_inLens;
    SetCursorsHidden(show);
    POINT v{};
    if (show) v = VirtualPos();
    else if (g.active && g.latched && !g_ptOn && g_set.pinCursor && g_lensWd > 0)
    {
        POINT cur;
        GetCursorPos(&cur);
        const double Z = g_mapZ;
        v.x = (LONG)std::lround((cur.x - g_mapBx) * Z);   // R = base + V / Z の逆
        v.y = (LONG)std::lround((cur.y - g_mapBy) * Z);
        show = v.x >= g_lensL && v.x < g_lensL + g_lensWd && v.y >= g_lensT && v.y < g_lensT + g_lensHt;
        if (show && (!g_curCaptured || g_curDpi != MonitorDpi(gpu.winMon))) CaptureCursors();   // 本物が見えている今のうちに写す
    }
    static bool shown = false;
    static LONG lastX = LONG_MIN, lastY = LONG_MIN;
    static int lastCell = -1;
    if (g_arrowFresh.exchange(false)) { shown = false; lastX = lastY = LONG_MIN; lastCell = 0; }   // 作り直した部品は、1マス目・画面の外に戻っている
    int cell = 0;
    if (show)
    {
        int kind = CurrentCursorKind();
        if (kind < 0) show = false;       // -2 アプリが消している（ゲームなど）/ -1 アプリ独自のカーソルは写せないので自前の矢印は出さない（本物が見える）
        else cell = kind;
        if (kind == kIdxWait || kind == kIdxApp)
        {
            const int a = kind - kIdxWait;
            if (g_aniFrames[a] > 1) cell = kNCur + a * kMaxFrames + (int)((GetTickCount64() / g_aniMs[a]) % g_aniFrames[a]);   // 回るコマを時間で選ぶ
        }
    }
    if (!show)
    {
        if (!shown) return;
        shown = false;
        gpu.dcArrow->SetOffsetX(-1000.0f - (float)(std::max(lastCell, 0) * kCurN));   // 切り出しは表のマスの位置なので、マス番号の分も左へ送る（送らないと後ろのコマが画面内に残る）
        if (gpu.dcInv) gpu.dcInv->SetOffsetX(-1000.0f - (float)(std::max(lastCell, 0) * kCurN));
        gpu.dcDev->Commit();
        return;
    }
    bool changed = false;
    if (g_atlasDirty && gpu.dcArrowSurf && !g_arrowAtlas.empty())
    {
        auto upload = [&](IDCompositionSurface* surf, const std::vector<uint32_t>& src) -> bool
        {
            POINT off{};
            ComPtr<ID3D11Texture2D> atex;
            if (FAILED(surf->BeginDraw(nullptr, IID_PPV_ARGS(&atex), &off))) return false;
            D3D11_BOX box{ (UINT)off.x, (UINT)off.y, 0, (UINT)(off.x + kCurN * kNCells), (UINT)(off.y + kCurN), 1 };
            gpu.ctx->UpdateSubresource(atex.Get(), 0, &box, src.data(), kCurN * kNCells * 4, 0);
            surf->EndDraw();
            return true;
        };
        bool ok = upload(gpu.dcArrowSurf.Get(), g_arrowAtlas);
        if (ok && gpu.dcInvSurf) upload(gpu.dcInvSurf.Get(), g_invAtlas);
        if (ok) { g_atlasDirty = false; changed = true; }
    }
    if (cell != lastCell)
    {
        D2D_RECT_F clip{ (float)(cell * kCurN), 0.0f, (float)((cell + 1) * kCurN), (float)kCurN };
        gpu.dcArrow->SetClip(clip);
        if (gpu.dcInv) gpu.dcInv->SetClip(clip);
        lastCell = cell;
        changed = true;
    }
    if (!changed && shown && v.x == lastX && v.y == lastY) return;
    shown = true;
    lastX = v.x;
    lastY = v.y;
    gpu.dcArrow->SetOffsetX((float)(v.x - gpu.winRect.left - g_curHot[cell][0] - cell * kCurN));
    gpu.dcArrow->SetOffsetY((float)(v.y - gpu.winRect.top - g_curHot[cell][1]));
    if (gpu.dcInv)
    {
        gpu.dcInv->SetOffsetX((float)(v.x - gpu.winRect.left - g_curHot[cell][0] - cell * kCurN));
        gpu.dcInv->SetOffsetY((float)(v.y - gpu.winRect.top - g_curHot[cell][1]));
    }
    gpu.dcDev->Commit();
}

// 描いた直後から次の合成の lateMs 前まで待って、そのときの最新のカーソル位置で位置を直す
static void LateCommit(double wakeMs)
{
    WaitUntil(wakeMs + g_period - g_set.lateMs);
    if (!g.active) return;
    POINT a = g.anchor;
    if (!g.frozen) GetCursorPos(&a);
    DcompCommit(a);
    UpdateArrow();
}

static void SetFrozen(bool f)
{
    g.frozen = f;
    g_frozenA = f;
}

// 1フレームの各段階の時間を溜めて、30フレームごとにログへ出す（log_timing=1 のとき）
static void LogFrame(double tA, double tB, double tC, double tD, UINT cw, UINT ch)
{
    if (!g_set.logTiming) return;
    double tE = NowMs();
    POINT after;
    GetCursorPos(&after);
    double lag = std::hypot((double)(after.x - g.anchor.x), (double)(after.y - g.anchor.y));
    g_tm.lagSum += lag;
    g_tm.lagMax = std::max(g_tm.lagMax, lag);
    if (g_prevFrame > 0)
    {
        double gap = tA - g_prevFrame;
        g_tm.gapMax = std::max(g_tm.gapMax, gap);
        if (gap > 25.0) ++g_tm.slow;
    }
    g_prevFrame = tA;
    if (g_tm.n == 0) g_tm.wall0 = tA;
    g_tm.pos += tB - tA;
    g_tm.cap += tC - tB;
    g_tm.up += tD - tC;
    g_tm.present += tE - tD;
    if (++g_tm.n == 30)
    {
        double n = g_tm.n;
        Log(L"timing: 起動後%.1fs %.1f fps  窓移動 %.2f ms / 画面取得 %.2f ms / 拡大呼び出し %.2f ms / 描画+Present %.2f ms (描く大きさ %ux%u 倍率%d%%)",
            (tE - g_actT) / 1000.0, n * 1000.0 / (tE - g_tm.wall0), g_tm.pos / n, g_tm.cap / n, g_tm.up / n, g_tm.present / n, cw, ch, g.zoom);
        Log(L"ずれ: フレーム間隔の最大 %.1f ms / 25ms超 %d回 / 描画後のカーソルとの差 平均 %.1f px 最大 %.1f px / フックの遅れ最大 %lu ms",
            g_tm.gapMax, g_tm.slow, g_tm.lagSum / n, g_tm.lagMax, (unsigned long)g_hookLagMax);
        Log(L"位置: カーソル(%ld,%ld) 切り出しの左上(サーフェス上 %d,%d) 画面%ux%u / 計算上の表示ずれ x=%.1f y=%.1f px",
            g.anchor.x, g.anchor.y, g_dbgSx, g_dbgSy, gpu.texW, gpu.texH, g_dbgErrX, g_dbgErrY);
        Log(L"サーフェス: 先読み%dpx 描いたタイル %d枚(四角%d個) / 切り出し直し %d回 / 描いた範囲で足りなかった %d回 / 直しの大きさ 平均 %.1f px / 合成周期の見積もり %.2f ms",
            g_set.ahead, g_osTiles, g_osRects, g_osN, g_osClamp, g_osShift / (2.0 * g_osN), g_period);
        Log(L"診断(変化の通知): 30フレーム中 通知あり %d回 / 全面扱い %d回 / 四角 %d個 / 合計面積 %.0f px² (1フレーム平均 %.0f) / 範囲(%ld,%ld)-(%ld,%ld)",
            g_dgDirtyFrames, g_dgAll, g_dgRects, g_dgArea, g_dgArea / std::max(1, g_dgFrames), g_dgBox.left, g_dgBox.top, g_dgBox.right, g_dgBox.bottom);
        Log(L"診断(画素比較): %s 本当に変わったタイル %d枚 (通知された面積は上の行)", gpu.diffOk ? L"有効" : L"無効(dirty rects で代用)", g_dgDiff);
        g_dgFrames = g_dgDirtyFrames = g_dgRects = g_dgAll = g_dgDiff = 0;
        g_dgArea = 0;
        g_dgBox = RECT{ LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN };
        g_osN = g_osClamp = g_osTiles = g_osRects = 0;
        g_osShift = 0;
        if (g_gt.cnt) Log(L"GPU処理(拡大+コピー): 平均 %.2f ms 最大 %.2f ms (%d枚)", g_gt.sum / g_gt.cnt, g_gt.mx, g_gt.cnt);
        g_gt.sum = g_gt.mx = 0;
        g_gt.cnt = 0;
        g_hookLagMax = 0;
        g_tm = Timing{};
    }
}

// dcomp: 世界(画面)に固定したサーフェスに、必要なぶんだけ描き足す。レンズは切り出し位置で滑るだけ。
// 世界を TILE 四方のタイルに区切り、「サーフェスに最新の絵が描いてあるか」をタイルごとに持つ。毎フレームやることは次の3つだけ:
//   1. 画面が変わった所(Desktop Duplication の dirty rects)のタイルを古いことにする
//   2. レンズに見える範囲と、その先(ahead px)のタイルのうち、描いてない/古いものを描く（見える所は必ず、先は毎フレーム一部ずつ）
//   3. 最新のマウス位置に合わせて切り出し位置を直す（DcompCommit）
// 静止した画面で、マウスが止まっていれば、描く量はゼロ。動いていれば新しく先に入ってくる細い帯だけ
static void FrameSurface(double tA)
{
    const RECT& m = gpu.monRect;      // 取り込み側
    const RECT& wr = gpu.winRect;     // 窓・レンズ側
    const int monW = wr.right - wr.left, monH = wr.bottom - wr.top;
    const int texW = (int)gpu.texW, texH = (int)gpu.texH;
    const int zq = std::clamp((g.zoom + 12) / 25 * 25, 100, 400);   // 世界の座標をそろえるため25%刻み
    const double Z = zq / 100.0;
    int w = std::clamp<int>(g.lensW, 1, monW);
    int h = std::clamp<int>(g.lensH, 1, monH);

    if (!gpu.dcSurf || gpu.surfZ != zq)
    {
        if (!gpu.MakeSurface(g.hwnd, zq)) return;
        g_tx = (texW + TILE - 1) / TILE;
        g_ty = (texH + TILE - 1) / TILE;
        g_tile.assign((size_t)g_tx * g_ty, 0);
        g_ri.valid = false;
    }
    double tB = NowMs();
    gpu.MakeDiff(g_tx, g_ty);   // 前の画面と比べる準備（失敗したら dirty rects だけで続ける）
    gpu.Capture(0);
    if (!gpu.haveFrame) return;
    double tC = NowMs();

    // 診断: 通知された変化の集計
    ++g_dgFrames;
    if (gpu.allDirty) ++g_dgAll;
    if (!gpu.dirty.empty()) ++g_dgDirtyFrames;
    for (const RECT& r : gpu.dirty)
    {
        ++g_dgRects;
        g_dgArea += (double)std::max<LONG>(0, r.right - r.left) * std::max<LONG>(0, r.bottom - r.top);
        g_dgBox.left = std::min(g_dgBox.left, r.left);   g_dgBox.top = std::min(g_dgBox.top, r.top);
        g_dgBox.right = std::max(g_dgBox.right, r.right); g_dgBox.bottom = std::max(g_dgBox.bottom, r.bottom);
    }

    // 1. 画面が変わった所のタイルを古いことにする
    static std::vector<uint8_t> changed;
    if (gpu.allDirty) std::fill(g_tile.begin(), g_tile.end(), (uint8_t)0);
    else if (gpu.newFrame && gpu.diffOk && gpu.DiffTiles(g_tx, g_ty, changed))
    {
        // dirty rects は自分の窓の場所も毎フレーム通知するので、画素が本当に変わったタイルだけを古くする
        for (size_t i = 0; i < changed.size(); ++i)
            if (changed[i]) { g_tile[i] = 0; ++g_dgDiff; }
    }
    else if (gpu.newFrame)
    {
        for (RECT r : gpu.dirty)
        {
            r.left = std::clamp<LONG>(r.left, 0, texW);
            r.right = std::clamp<LONG>(r.right, 0, texW);
            r.top = std::clamp<LONG>(r.top, 0, texH);
            r.bottom = std::clamp<LONG>(r.bottom, 0, texH);
            if (r.left >= r.right || r.top >= r.bottom) continue;
            for (int y = r.top / TILE; y <= (r.bottom - 1) / TILE; ++y)
                for (int x = r.left / TILE; x <= (r.right - 1) / TILE; ++x) g_tile[(size_t)y * g_tx + x] = 0;
        }
    }
    gpu.dirty.clear();
    gpu.allDirty = false;
    gpu.newFrame = false;

    // レンズに見せたい拡大元(画面座標)。端で止まるレンズ中心とマウスのずれを反映する
    // 固定中はレンズの置き場所が止まり、拡大元はマウスの点そのもの（DcompCommit と同じ式）
    int lx = std::clamp<int>((g.latched ? g.pin.x : g.anchor.x) - w / 2, wr.left, wr.right - w);
    int ly = std::clamp<int>((g.latched ? g.pin.y : g.anchor.y) - h / 2, wr.top, wr.bottom - h);
    double s1x = g.latched ? g.anchor.x : g.anchor.x - (g.anchor.x - (lx + w / 2.0)) / Z;
    double s1y = g.latched ? g.anchor.y : g.anchor.y - (g.anchor.y - (ly + h / 2.0)) / Z;
    int lsw = std::min(texW, (int)std::ceil(w / Z)), lsh = std::min(texH, (int)std::ceil(h / Z));
    int lsl = std::clamp<int>((int)std::lround(s1x) - m.left - lsw / 2, 0, texW - lsw);
    int lst = std::clamp<int>((int)std::lround(s1y) - m.top - lsh / 2, 0, texH - lsh);

    // 2. 描くタイルを選ぶ。見える範囲(vx0..vx1, vy0..vy1)は必ず、その先は中心に近い順に1フレーム kAheadBudget 枚まで
    const int vx0 = lsl / TILE, vx1 = (lsl + lsw - 1) / TILE, vy0 = lst / TILE, vy1 = (lst + lsh - 1) / TILE;
    const int ax = (g_set.ahead + TILE - 1) / TILE;
    const int nx0 = std::max(0, vx0 - ax), nx1 = std::min(g_tx - 1, vx1 + ax);
    const int ny0 = std::max(0, vy0 - ax), ny1 = std::min(g_ty - 1, vy1 + ax);
    static std::vector<uint8_t> sel;
    sel.assign((size_t)g_tx * g_ty, 0);
    struct Cand { int d, x, y; };
    static std::vector<Cand> ahead;
    ahead.clear();
    const int cx = (vx0 + vx1) / 2, cy = (vy0 + vy1) / 2;
    for (int y = ny0; y <= ny1; ++y)
        for (int x = nx0; x <= nx1; ++x)
        {
            if (g_tile[(size_t)y * g_tx + x]) continue;
            if (x >= vx0 && x <= vx1 && y >= vy0 && y <= vy1) sel[(size_t)y * g_tx + x] = 1;
            else ahead.push_back({ std::max(std::abs(x - cx), std::abs(y - cy)), x, y });
        }
    const size_t kAheadBudget = 96;
    if (ahead.size() > kAheadBudget)
    {
        std::partial_sort(ahead.begin(), ahead.begin() + kAheadBudget, ahead.end(), [](const Cand& a, const Cand& b) { return a.d < b.d; });
        ahead.resize(kAheadBudget);
    }
    for (const Cand& c : ahead) sel[(size_t)c.y * g_tx + c.x] = 1;

    // 選んだタイルを四角にまとめる（横に連なる所を1本にして、同じ幅で縦に連なる分をつなぐ）
    struct TR { int x0, y0, x1, y1; };
    static std::vector<TR> rects;
    rects.clear();
    for (int y = ny0; y <= ny1; ++y)
    {
        int x = nx0;
        while (x <= nx1)
        {
            if (!sel[(size_t)y * g_tx + x]) { ++x; continue; }
            int x0 = x;
            while (x <= nx1 && sel[(size_t)y * g_tx + x]) ++x;
            bool joined = false;
            for (TR& r : rects)
                if (r.x0 == x0 && r.x1 == x && r.y1 == y) { r.y1 = y + 1; joined = true; break; }
            if (!joined) rects.push_back({ x0, y, x, y + 1 });
        }
    }

    static bool warned = false;
    if (g_set.logTiming) { g_gt.Poll(gpu.ctx.Get()); g_gt.Begin(gpu.ctx.Get()); }
    bool failed = false;
    for (const TR& r : rects)
    {
        // 拡大元の四角(画面)。端の超解像にもとなりの絵を見せるため g_set.pad だけ広げて拡大し、内側だけをサーフェスに置く
        int sl = r.x0 * TILE, st = r.y0 * TILE, sr = std::min(texW, r.x1 * TILE), sb = std::min(texH, r.y1 * TILE);
        RECT src{ std::max(0, sl - g_set.pad), std::max(0, st - g_set.pad), std::min(texW, sr + g_set.pad), std::min(texH, sb + g_set.pad) };
        UINT ow = (UINT)((src.right - src.left) * zq / 100), oh = (UINT)((src.bottom - src.top) * zq / 100);
        bool ok = gpu.UpscaleSub(src, ow, oh, g_set.vsrQuality);
        if (!ok && g_set.vsrQuality != 0) ok = gpu.UpscaleSub(src, ow, oh, 0);   // 超解像が通らない大きさ/倍率なら普通の拡大で代用
        if (!ok)
        {
            if (!warned) Log(L"拡大に失敗 src=%ldx%ld out=%ux%u", src.right - src.left, src.bottom - src.top, ow, oh);
            warned = true;
            failed = true;
            break;
        }
        // 世界に固定したサーフェスのこの場所に、描いた絵の内側をそのまま置く（ほかの場所は触らない）
        RECT ur{ sl * zq / 100, st * zq / 100, sr * zq / 100, sb * zq / 100 };
        ComPtr<ID3D11Texture2D> t;
        POINT off{};
        HRESULT hr = gpu.dcSurf->BeginDraw(&ur, IID_PPV_ARGS(&t), &off);
        if (FAILED(hr))
        {
            static bool warnedDraw = false;
            if (!warnedDraw) Log(L"サーフェスの BeginDraw に失敗 hr=0x%08X 範囲(%ld,%ld)-(%ld,%ld)", (unsigned)hr, ur.left, ur.top, ur.right, ur.bottom);
            warnedDraw = true;
            failed = true;
            break;
        }
        UINT ix = (UINT)((sl - src.left) * zq / 100), iy = (UINT)((st - src.top) * zq / 100);
        D3D11_BOX box{ ix, iy, 0, ix + (UINT)(ur.right - ur.left), iy + (UINT)(ur.bottom - ur.top), 1 };
        gpu.ctx->CopySubresourceRegion(t.Get(), 0, (UINT)off.x, (UINT)off.y, 0, gpu.stageTex.Get(), 0, &box);
        gpu.dcSurf->EndDraw();
        for (int y = r.y0; y < r.y1; ++y)
            for (int x = r.x0; x < r.x1; ++x)
            {
                g_tile[(size_t)y * g_tx + x] = 1;
                ++g_osTiles;
            }
        ++g_osRects;
        g_forceCommit = true;
    }
    g_gt.End(gpu.ctx.Get());
    double tD = NowMs();
    if (failed) return;

    // 見える範囲のまわりで、最新の絵が途切れずにそろっている範囲を、切り出し位置の動かせる範囲にする
    int k = 0;
    for (; k < ax; ++k)
    {
        int x0 = std::max(0, vx0 - (k + 1)), x1 = std::min(g_tx - 1, vx1 + (k + 1));
        int y0 = std::max(0, vy0 - (k + 1)), y1 = std::min(g_ty - 1, vy1 + (k + 1));
        bool allOk = true;
        for (int y = y0; y <= y1 && allOk; ++y)
            for (int x = x0; x <= x1; ++x)
                if (!g_tile[(size_t)y * g_tx + x]) { allOk = false; break; }
        if (!allOk) break;
    }
    int ax0 = std::max(0, vx0 - k), ax1 = std::min(g_tx - 1, vx1 + k), ay0 = std::max(0, vy0 - k), ay1 = std::min(g_ty - 1, vy1 + k);
    g_ri.valid = true;
    g_ri.ru = ax0 * TILE * zq / 100;
    g_ri.rv = ay0 * TILE * zq / 100;
    g_ri.full = (std::min(texW, (ax1 + 1) * TILE) - ax0 * TILE) * zq / 100;
    g_ri.fullV = (std::min(texH, (ay1 + 1) * TILE) - ay0 * TILE) * zq / 100;
    g_ri.w = std::min(w, g_ri.full);
    g_ri.h = std::min(h, g_ri.fullV);
    g_ri.zq = zq;
    POINT a = g.anchor;
    if (!g.frozen) GetCursorPos(&a);   // 描いたあとの最新のマウス位置で、まず1回位置を直す
    DcompCommit(a);
    g_presented = true;
    LogFrame(tA, tB, tC, tD, (UINT)g_ri.full, (UINT)g_ri.fullV);
}

static void Deactivate();

static void Frame()
{
    g_presented = false;
    if (!g.active) return;
    if (g_raise || ShellWindowAboveUs())
    {
        g_raise = false;
        SetWindowPos(g.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    double tA = NowMs();
    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    if (ctrl != g.frozen) SetFrozen(ctrl);
    if (!ctrl) { g_ptOn = false; g_inLens = false; }   // Ctrl を離したら V は使わない（本物のポインタがそのまま続きになる）
    // 縁の近くで Ctrl を押している間は、窓のクリック透過を外してカーソルを矢印（↔ など）にする。
    // クリックが通る窓は WM_SETCURSOR を受けないので、縁に居るときだけ受ける窓にする（Electron の setIgnoreMouseEvents を切り替える定番の作り）。
    // 縁に居る間のクリックは、フックが握りつぶして下に通さない
    {
        bool hot = false;
        if (ctrl)
        {
            if (g_dragMode) hot = true;   // つかんでいる間は、カーソルが縁から離れても矢印のまま
            else
            {
                POINT p;
                if (g_ptOn) p = VirtualPos();   // 見えている矢印の位置で判定する
                else GetCursorPos(&p);
                LPCWSTR c = nullptr;
                if (EdgeHit(p, &c)) { g_hotCur = c; hot = true; }
            }
        }
        if (hot != g_hot)
        {
            g_hot = hot;
            LONG_PTR ex = GetWindowLongPtrW(g.hwnd, GWL_EXSTYLE);
            SetWindowLongPtrW(g.hwnd, GWL_EXSTYLE, hot ? (ex & ~(LONG_PTR)WS_EX_TRANSPARENT) : (ex | WS_EX_TRANSPARENT));
        }
        if (g_hot) SetCursor(EdgeCursor(g_hotCur));
    }
    if (int dm = g_dragMode)
    {
        if (!ctrl) g_dragMode = 0;   // Ctrl を離したらつかみ終わり
        else
        {
            POINT p;
            GetCursorPos(&p);
            // 縁をつかんだ軸だけ、中心からの距離×2を大きさにする（比率は自由に変わる）
            const int maxW = gpu.winRect.right - gpu.winRect.left, maxH = gpu.winRect.bottom - gpu.winRect.top;
            if (dm & 1) g.lensW = std::clamp<int>(2 * std::abs(p.x - g_dragCx.load()), g_set.lensMin, maxW);
            if (dm & 2) g.lensH = std::clamp<int>(2 * std::abs(p.y - g_dragCy.load()), g_set.lensMin, maxH);
        }
    }
    if (!g.frozen) GetCursorPos(&g.anchor);
    if (g_set.followMonitor && !g.frozen)
    {
        // カーソルが別のモニターに出たら、窓をそのモニターへ移す（固定中は今のモニターに残し、固定を解いたら移す）。
        // 取り込みは同じ GPU のモニターを全部貼り合わせて持っているので、境目をまたいでも絵はつながったまま
        HMONITOR hm = MonitorFromPoint(g.anchor, MONITOR_DEFAULTTONEAREST);
        const bool moveWin = !g.latched && hm != gpu.winMon;
        const bool recap = !gpu.Covers(hm);   // 取り込みは同じ GPU のモニターを全部持つので、取り直すのは別の GPU のモニターへ出たときだけ
        if (recap || moveWin)
        {
            double t0 = NowMs();
            if (recap)
            {
                if (!gpu.EnsureFor(hm)) { Deactivate(); return; }
                gpu.ReleaseSwap();   // サーフェスはモニターの大きさで作り直す
                g_ri.valid = false;
                gpu.haveFrame = false;
                gpu.Capture(200);
            }
            if (moveWin)
            {
                gpu.winMon = hm;
                gpu.RectOf(hm, gpu.winRect);
                const RECT& wr = gpu.winRect;
                SetWindowPos(g.hwnd, HWND_TOPMOST, wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top, SWP_NOACTIVATE);
            }
            g_forceCommit = true;
            const RECT& mr = gpu.monRect;
            Log(L"モニター切り替え(%s): (%ld,%ld)-(%ld,%ld) %ux%u  %.1f ms", recap ? (moveWin ? L"取り込みと窓" : L"取り込みのみ") : L"窓のみ", mr.left, mr.top, mr.right, mr.bottom, gpu.texW, gpu.texH, NowMs() - t0);
        }
    }
    FrameSurface(tA);
}

static void Deactivate()
{
    if (!g.active) return;
    g.active = false;
    if (g_zHook) { UnhookWinEvent(g_zHook); g_zHook = nullptr; }
    StopHook();
    UnregisterHotKey(g.hwnd, HOTKEY_EXIT);
    for (int i = 0; i < 3; ++i) UnregisterHotKey(g.hwnd, HOTKEY_PRESET1 + i);
    UnregisterHotKey(g.hwnd, HOTKEY_LATCH);
    g.latched = false;
    g_dragMode = 0;
    g_lensWd = 0;
    g_ptOn = false;
    g_inLens = false;
    g_frozenA = false;
    SetCursorsHidden(false);
    if (g_hot)
    {
        g_hot = false;
        SetWindowLongPtrW(g.hwnd, GWL_EXSTYLE, GetWindowLongPtrW(g.hwnd, GWL_EXSTYLE) | WS_EX_TRANSPARENT);
    }
    ShowWindow(g.hwnd, SW_HIDE);
    g.frozen = false;
}

static void Activate()
{
    POINT c;
    GetCursorPos(&c);
    if (!gpu.EnsureFor(MonitorFromPoint(c, MONITOR_DEFAULTTOPRIMARY)))
    {
        MessageBoxW(nullptr, L"Failed to capture the screen or initialize super resolution. See HiResLoupe.log.", L"HiResLoupe", MB_ICONERROR);
        return;
    }
    gpu.ReleaseSwap();   // 大きさが変わっていてもよいよう作り直す
    g_ri.valid = false;
    g.frozen = false;
    g_frozenA = false;
    g.latched = false;
    g.anchor = c;
    g_actT = NowMs();
    g_gt = GpuTimer{};
    g_gt.Init(gpu.dev.Get());
    g_prevFrame = 0;
    g_tm = Timing{};
    g.active = true;
    gpu.haveFrame = false;
    gpu.Capture(200);   // 最初の1枚は待ってでも取る
    gpu.winMon = MonitorFromPoint(c, MONITOR_DEFAULTTOPRIMARY);
    gpu.RectOf(gpu.winMon, gpu.winRect);
    const RECT& mr = gpu.winRect;
    SetWindowPos(g.hwnd, HWND_TOPMOST, mr.left, mr.top, mr.right - mr.left, mr.bottom - mr.top, SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_FRAMECHANGED);
    RegisterHotKey(g.hwnd, HOTKEY_EXIT, g_set.exitMods, g_set.exitKey);
    for (int i = 0; i < 3; ++i) RegisterHotKey(g.hwnd, HOTKEY_PRESET1 + i, g_set.presetMods[i], g_set.presetKey[i]);
    RegisterHotKey(g.hwnd, HOTKEY_LATCH, g_set.latchMods, g_set.latchKey);
    StartHook();
    g_raise = false;
    g_zHook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, ZOrderEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
}

// レンズの大きさをモニターの中・最小以上に収める。幅と高さは別々に挟まず、同じ比率で縮める
// （別々だと最大でモニターの比率になり、戻すときも元の形に戻らない）
static void FitLens()
{
    const int maxW = gpu.winRect.right - gpu.winRect.left, maxH = gpu.winRect.bottom - gpu.winRect.top;
    g.lensW = std::max(g.lensW, 1);
    g.lensH = std::max(g.lensH, 1);
    if (g.lensW > maxW || g.lensH > maxH)
    {
        double s = std::min((double)maxW / g.lensW, (double)maxH / g.lensH);
        g.lensW = (int)(g.lensW * s);
        g.lensH = (int)(g.lensH * s);
    }
    if (g.lensW < g_set.lensMin || g.lensH < g_set.lensMin)
    {
        double s = std::max((double)g_set.lensMin / g.lensW, (double)g_set.lensMin / g.lensH);
        g.lensW = (int)(g.lensW * s + 0.5);
        g.lensH = (int)(g.lensH * s + 0.5);
    }
}

// 数字キーのプリセット: 1=正方形, 2=横の16:9, 3=縦の16:9。いずれも今の長辺に合わせる
static void ApplyPreset(int n)
{
    const int L = std::max(g.lensW, g.lensH);
    const int S = (L * 9 + 8) / 16;
    g.lensW = n == 3 ? S : L;
    g.lensH = n == 2 ? S : L;
    FitLens();
}

static void OnWheel(int delta, bool ctrl)
{
    int notches = delta / WHEEL_DELTA;
    if (notches == 0) notches = delta > 0 ? 1 : -1;
    if (ctrl)
    {
        // 下に回すと大きく（拡大率と同じ向き）。10%ずつ
        double f = 1.0 + (delta < 0 ? 1 : -1) * g_set.lensStep / 100.0;
        for (int i = 0; i < std::abs(notches); ++i)
        {
            g.lensW = (int)(g.lensW * f + 0.5);
            g.lensH = (int)(g.lensH * f + 0.5);
        }
        FitLens();
    }
    else
    {
        // 下に回すと増える（+25%）、上に回すと減る
        g.zoom = std::clamp(g.zoom + (delta < 0 ? 1 : -1) * std::abs(notches) * g_set.zoomStep, g_set.zoomMin, g_set.zoomMax);
    }
}

// ---------------------------------------------------------------- タスクトレイ
static UINT g_taskbarCreated = 0;   // エクスプローラーが再起動したときの通知。アイコンを付け直す

// ツールチップの既定の文字にある起動キーを、今の起動キーに置き換える（[text] で書き換えた文字はそのまま）
static std::wstring TrayTip()
{
    std::wstring t = g_set.txTip;
    const size_t p = t.find(L"Win+Alt+Z");
    if (p != std::wstring::npos) t.replace(p, 9, HotkeyName(g_set.toggleMods, g_set.toggleKey));
    return t;
}

static void AddTray()
{
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof nid;
    nid.hWnd = g.hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);   // exe に埋め込んだアイコン(assets/HiResLoupe.rc)
    wcsncpy_s(nid.szTip, TrayTip().c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &nid);
}

static void UpdateTrayTip()
{
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof nid;
    nid.hWnd = g.hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_TIP;
    wcsncpy_s(nid.szTip, TrayTip().c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void RemoveTray()
{
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof nid;
    nid.hWnd = g.hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void ToggleLoupe()
{
    if (g.active) Deactivate(); else Activate();
}

// Windows 起動時の自動起動。現在のユーザーの Run キーに登録する（管理者権限は要らない）
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunName = L"HiResLoupe";

static std::wstring StartupCommand()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::wstring(L"\"") + path + L"\" --startup";
}

static bool StartupRegistered()
{
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunName, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

static void SetStartup(bool on)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) { Log(L"自動起動: Run キーを開けない"); return; }
    if (on)
    {
        std::wstring cmd = StartupCommand();
        LONG r = RegSetValueExW(k, kRunName, 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
        Log(L"自動起動: 登録 %s (r=%ld)", cmd.c_str(), r);
    }
    else
    {
        RegDeleteValueW(k, kRunName);
        Log(L"自動起動: 解除");
    }
    RegCloseKey(k);
}

// ini を読み直す。ルーペが出ていれば一度消して、新しい設定（倍率・大きさ・縁・起動キー・文字）で出し直す
static void ReloadSettings()
{
    const bool wasActive = g.active;
    if (wasActive) Deactivate();
    UnregisterHotKey(g.hwnd, HOTKEY_TOGGLE);
    UnregisterHotKey(g.hwnd, HOTKEY_TOGGLE2);
    LoadSettings();
    bool ok1 = RegisterHotKey(g.hwnd, HOTKEY_TOGGLE, g_set.toggleMods, g_set.toggleKey) != 0;
    bool ok2 = g_set.toggle2Key && RegisterHotKey(g.hwnd, HOTKEY_TOGGLE2, g_set.toggle2Mods, g_set.toggle2Key);
    Log(L"設定を再読み込み toggle=%s toggle2=%s", ok1 ? L"OK" : L"NG", ok2 ? L"OK" : L"NG");
    g.zoom = std::clamp(g_set.zoomDefault, g_set.zoomMin, g_set.zoomMax);
    g.lensW = g.lensH = std::max(g_set.lensSize, g_set.lensMin);
    UpdateTrayTip();
    if (wasActive) Activate();
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

// メニューの持ち主にする見えない窓。持たれた窓(owned window)は必ず持ち主より上に出るため、ルーペの窓を持ち主にすると
// メニューがルーペの上から下ろせない。別の窓に持たせて、メニューの知らせだけルーペの窓へ回す
static LRESULT CALLBACK MenuOwnerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_COMMAND || msg == WM_ENTERMENULOOP || msg == WM_EXITMENULOOP) return WndProc(g.hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND MenuOwner()
{
    static HWND s_owner = nullptr;
    if (!s_owner)
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = MenuOwnerProc;
        wc.hInstance = g_inst;
        wc.lpszClassName = L"HiResLoupeMenuOwner";
        RegisterClassW(&wc);
        s_owner = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, g_inst, nullptr);
    }
    return s_owner ? s_owner : g.hwnd;
}

// ---------------------------------------------------------------- ショートカットの変更
// 標準のホットキー入力欄(HOTKEY_CLASS)は Win キーを受け取れない（押すとスタートメニューに取られる）ので、
// 窓が前面にある間だけキーボードのローレベルフックを入れて、押したキーをここで受けて握りつぶす（スタートメニューも開かない）
static const UINT WM_APP_HKSHOW = WM_APP + 10;
static HWND g_hkWnd = nullptr, g_hkCombo = nullptr, g_hkMsg = nullptr;
static HHOOK g_kbHook = nullptr;
static HFONT g_hkFont = nullptr, g_hkBig = nullptr;
static int g_hkWhich = 0;                           // 0=出す・消す 1=閉じる 2=固定 3..5=形 1..3
static UINT g_hkHeld = 0, g_hkMods = 0, g_hkKey = 0; // 押している修飾キー / 受けた組み合わせ
enum { IDC_HK_SAVE = 1, IDC_HK_CANCEL, IDC_HK_DEFAULT };

static UINT ModBit(DWORD vk)
{
    switch (vk)
    {
    case VK_LWIN: case VK_RWIN: return MOD_WIN;
    case VK_LCONTROL: case VK_RCONTROL: case VK_CONTROL: return MOD_CONTROL;
    case VK_LMENU: case VK_RMENU: case VK_MENU: return MOD_ALT;
    case VK_LSHIFT: case VK_RSHIFT: case VK_SHIFT: return MOD_SHIFT;
    }
    return 0;
}

static LRESULT CALLBACK HotkeyKbHook(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && g_hkWnd && GetForegroundWindow() == g_hkWnd)
    {
        const KBDLLHOOKSTRUCT* k = (const KBDLLHOOKSTRUCT*)lp;
        const bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
        if (UINT bit = ModBit(k->vkCode))
        {
            if (down) { if (!(g_hkHeld & bit) && g_hkKey) g_hkKey = 0; g_hkHeld |= bit; }   // 決めたあとに修飾キーを押し直したら、やり直し
            else g_hkHeld &= ~bit;
            if (!g_hkKey) g_hkMods = g_hkHeld;
        }
        else if (down)
        {
            g_hkMods = g_hkHeld;
            g_hkKey = k->vkCode;
        }
        PostMessageW(g_hkWnd, WM_APP_HKSHOW, 0, 0);
        return 1;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static void HotkeyCurrent(int which, UINT& mods, UINT& key)
{
    if (which == 0) { mods = g_set.toggleMods; key = g_set.toggleKey; }
    else if (which == 1) { mods = g_set.exitMods; key = g_set.exitKey; }
    else if (which == 2) { mods = g_set.latchMods; key = g_set.latchKey; }
    else { mods = g_set.presetMods[which - 3]; key = g_set.presetKey[which - 3]; }
}

static void HotkeyShow(const wchar_t* msg)
{
    SetWindowTextW(g_hkCombo, HotkeyName(g_hkMods, g_hkKey).c_str());
    SetWindowTextW(g_hkMsg, msg ? msg : L"");
}

// 保存できれば true。だめなら理由を窓に出す
static bool HotkeySave()
{
    const wchar_t* const* tx = kHkTexts[g_set.lang];
    const UINT mods = (g_hkMods & (MOD_WIN | MOD_CONTROL | MOD_ALT | MOD_SHIFT)) | MOD_NOREPEAT, key = g_hkKey;
    if (!key) return false;
    const bool bare = (mods & ~MOD_NOREPEAT) == 0;
    if (g_hkWhich == 0 && bare && !(key >= VK_F1 && key <= VK_F24)) { HotkeyShow(tx[10]); return false; }   // 出す・消すは常に登録しているので、1キーだけは断る
    for (int i = 0; i < kNHk; ++i)
    {
        UINT m, k;
        HotkeyCurrent(i, m, k);
        if (i != g_hkWhich && m == mods && k == key) { HotkeyShow(tx[9]); return false; }
    }
    if (IsCommonShortcut(mods, key)) { HotkeyShow(tx[14]); return false; }
    if (g_hkWhich == 0)
    {
        UnregisterHotKey(g.hwnd, HOTKEY_TOGGLE);
        if (!RegisterHotKey(g.hwnd, HOTKEY_TOGGLE, mods, key))
        {
            RegisterHotKey(g.hwnd, HOTKEY_TOGGLE, g_set.toggleMods, g_set.toggleKey);
            HotkeyShow(tx[8]);
            return false;
        }
        g_set.toggleMods = mods; g_set.toggleKey = key;
    }
    else
    {
        // 閉じる・固定・形はルーペが出ている間だけ登録する。今使えるかだけ試す
        if (!RegisterHotKey(g.hwnd, HOTKEY_TEST, mods, key)) { HotkeyShow(tx[8]); return false; }
        UnregisterHotKey(g.hwnd, HOTKEY_TEST);
        if (g_hkWhich == 1) { g_set.exitMods = mods; g_set.exitKey = key; }
        else if (g_hkWhich == 2) { g_set.latchMods = mods; g_set.latchKey = key; }
        else { g_set.presetMods[g_hkWhich - 3] = mods; g_set.presetKey[g_hkWhich - 3] = key; }
    }
    static const wchar_t* kIniKeys[kNHk] = { L"toggle", L"exit", L"latch", L"preset1", L"preset2", L"preset3" };
    WritePrivateProfileStringW(L"hotkey", kIniKeys[g_hkWhich], HotkeyName(mods, key).c_str(), (g_dir + L"HiResLoupe.ini").c_str());
    Log(L"ショートカットを変更 %s=%s", kIniKeys[g_hkWhich], HotkeyName(mods, key).c_str());
    UpdateTrayTip();
    return true;
}

static LRESULT CALLBACK HotkeyWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_APP_HKSHOW:
        HotkeyShow(nullptr);
        return 0;
    case WM_ACTIVATE:
        g_hkHeld = 0;   // 前面でない間の修飾キーは見ていないので、数え直す
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_HK_SAVE) { if (HotkeySave()) DestroyWindow(hwnd); }
        else if (LOWORD(wp) == IDC_HK_CANCEL) DestroyWindow(hwnd);
        else if (LOWORD(wp) == IDC_HK_DEFAULT)
        {
            static const wchar_t* kDefaults[kNHk] = { L"Win+Alt+Z", L"Esc", L"Ctrl+Space", L"1", L"2", L"3" };
            ParseHotkey(kDefaults[g_hkWhich], g_hkMods, g_hkKey);
            HotkeyShow(nullptr);
        }
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        if ((HWND)lp == g_hkMsg) SetTextColor((HDC)wp, RGB(200, 40, 40));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_kbHook) { UnhookWindowsHookEx(g_kbHook); g_kbHook = nullptr; }
        if (g_hkFont) { DeleteObject(g_hkFont); g_hkFont = nullptr; }
        if (g_hkBig) { DeleteObject(g_hkBig); g_hkBig = nullptr; }
        g_hkWnd = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void OpenHotkeyDialog(int which)
{
    if (g_hkWnd) { SetForegroundWindow(g_hkWnd); return; }
    if (g.active) Deactivate();   // ルーペの数字キー・Esc などを外しておく
    g_hkWhich = which;
    g_hkHeld = 0;
    HotkeyCurrent(which, g_hkMods, g_hkKey);
    g_hkMods &= ~MOD_NOREPEAT;

    static bool registered = false;
    if (!registered)
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = HotkeyWndProc;
        wc.hInstance = g_inst;
        wc.lpszClassName = L"HiResLoupeHotkey";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(1));
        RegisterClassW(&wc);
        registered = true;
    }
    POINT c;
    GetCursorPos(&c);
    HMONITOR mon = MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST);
    const UINT dpi = MonitorDpi(mon);
    auto S = [&](int v) { return MulDiv(v, (int)dpi, 96); };
    MONITORINFO mi{ sizeof mi };
    GetMonitorInfoW(mon, &mi);
    RECT rc{ 0, 0, S(380), S(170) };
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectExForDpi(&rc, style, FALSE, WS_EX_TOPMOST, dpi);
    const int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    const int x = (mi.rcWork.left + mi.rcWork.right - ww) / 2, y = (mi.rcWork.top + mi.rcWork.bottom - wh) / 2;
    const wchar_t* const* tx = kHkTexts[g_set.lang];
    g_hkWnd = CreateWindowExW(WS_EX_TOPMOST, L"HiResLoupeHotkey", (std::wstring(L"HiResLoupe - ") + HkLabel(which)).c_str(), style,
                              x, y, ww, wh, nullptr, nullptr, g_inst, nullptr);
    if (!g_hkWnd) return;

    NONCLIENTMETRICSW ncm{ sizeof ncm };
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi);
    g_hkFont = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW big = ncm.lfMessageFont;
    big.lfHeight = big.lfHeight * 8 / 5;
    big.lfWeight = FW_BOLD;
    g_hkBig = CreateFontIndirectW(&big);
    auto add = [&](const wchar_t* cls, const wchar_t* text, DWORD st, int x0, int y0, int w0, int h0, int id, HFONT f)
    {
        HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | st, S(x0), S(y0), S(w0), S(h0), g_hkWnd, (HMENU)(INT_PTR)id, g_inst, nullptr);
        SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE);
        return h;
    };
    add(L"STATIC", tx[4], SS_LEFT, 14, 12, 352, 20, 0, g_hkFont);
    g_hkCombo = add(L"STATIC", L"", SS_CENTER | SS_CENTERIMAGE, 14, 36, 352, 34, 0, g_hkBig);
    g_hkMsg = add(L"STATIC", L"", SS_LEFT, 14, 74, 352, 40, 0, g_hkFont);
    add(L"BUTTON", tx[7], BS_PUSHBUTTON, 14, 126, 90, 30, IDC_HK_DEFAULT, g_hkFont);
    add(L"BUTTON", tx[5], BS_DEFPUSHBUTTON, 180, 126, 90, 30, IDC_HK_SAVE, g_hkFont);
    add(L"BUTTON", tx[6], BS_PUSHBUTTON, 276, 126, 90, 30, IDC_HK_CANCEL, g_hkFont);
    HotkeyShow(nullptr);
    g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, HotkeyKbHook, g_inst, 0);
    ShowWindow(g_hkWnd, SW_SHOW);
    SetForegroundWindow(g_hkWnd);
}

// 全部のショートカットを一覧で出す。表示中もルーペの描画を続ける（トレイのメニューと同じタイマー）
static void ShowShortcutList()
{
    const wchar_t* const* t = kListTexts[g_set.lang];
    auto line = [](const std::wstring& k, const wchar_t* d) { return k + L"  :  " + d + L"\n"; };
    std::wstring s = std::wstring(t[14]) + L"\n";
    s += line(HotkeyName(g_set.toggleMods, g_set.toggleKey), t[1]);
    s += line(HotkeyName(g_set.exitMods, g_set.exitKey), t[2]);
    s += line(HotkeyName(g_set.latchMods, g_set.latchKey), t[3]);
    for (int i = 0; i < 3; ++i)
        s += line(HotkeyName(g_set.presetMods[i], g_set.presetKey[i]), (std::wstring(HkLabel(3 + i)) + t[12]).c_str());
    s += L"\n" + std::wstring(t[15]) + L"\n";
    s += line(t[13], t[2]);
    s += line(t[4], t[5]);
    s += line(t[6], (std::wstring(t[7]) + t[12]).c_str());
    s += L"  " + std::wstring(t[16]) + L"\n";
    s += line(t[10], t[11]);
    s += line(t[8], t[9]);
    SetTimer(g.hwnd, TIMER_MENU, USER_TIMER_MINIMUM, nullptr);
    MessageBoxW(MenuOwner(), s.c_str(), (std::wstring(L"HiResLoupe - ") + t[0]).c_str(), MB_OK | MB_TOPMOST | MB_SETFOREGROUND);
    KillTimer(g.hwnd, TIMER_MENU);
}

static void ShowTrayMenu()
{
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CMD_TOGGLE, (g.active ? g_set.txHide : g_set.txShow).c_str());
    AppendMenuW(m, MF_STRING | (g_set.pinCursor ? MF_CHECKED : 0), CMD_PINCUR, g_set.txPinCur.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_INI, g_set.txSettings.c_str());
    AppendMenuW(m, MF_STRING, CMD_RELOAD, g_set.txReload.c_str());
    AppendMenuW(m, MF_STRING, CMD_LOG, g_set.txLog.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (StartupRegistered() ? MF_CHECKED : 0), CMD_STARTUP, g_set.txStartup.c_str());
    HMENU lm = CreatePopupMenu();
    for (int i = 0; i < 4; i++) AppendMenuW(lm, MF_STRING | (g_set.lang == i ? MF_CHECKED : 0), CMD_LANG + i, kLangNames[i]);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)lm, g_set.txLang.c_str());
    AppendMenuW(m, MF_STRING, CMD_LIST, kListTexts[g_set.lang][0]);
    HMENU hm = CreatePopupMenu();
    for (int i = 0; i < kNHk; i++)
    {
        UINT mods, key;
        HotkeyCurrent(i, mods, key);
        AppendMenuW(hm, MF_STRING, CMD_HK + i, (std::wstring(HkLabel(i)) + L"...\t" + HotkeyName(mods, key)).c_str());
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR)hm, kHkTexts[g_set.lang][0]);
    AppendMenuW(m, MF_STRING, CMD_QUIT, g_set.txQuit.c_str());
    POINT p;
    GetCursorPos(&p);
    HWND owner = MenuOwner();
    SetForegroundWindow(owner);   // これが無いと、メニューの外をクリックしても閉じない
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, owner, nullptr);
    PostMessageW(owner, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_taskbarCreated && msg == g_taskbarCreated)
    {
        AddTray();
        return 0;
    }
    switch (msg)
    {
    case WM_APP_TRAY:
        if (lp == WM_LBUTTONUP || lp == WM_RBUTTONUP)
            Log(L"トレイ: %s ctrl=%d V使用=%d レンズ内=%d", lp == WM_LBUTTONUP ? L"左クリック" : L"右クリック",
                (GetAsyncKeyState(VK_CONTROL) & 0x8000) ? 1 : 0, g_ptOn.load() ? 1 : 0, g_inLens.load() ? 1 : 0);
        if (lp == WM_LBUTTONUP) ToggleLoupe();
        else if (lp == WM_RBUTTONUP) ShowTrayMenu();
        return 0;
    // メニューの間は TrackPopupMenu の中でメッセージが回り、下の for ループ（Frame）が止まる。タイマーで描画を続け、
    // 自分のメニューも他のアプリと同じようにルーペに映す（SDL などがモーダルループの間に使う定番の方法）
    case WM_ENTERMENULOOP:
        SetTimer(hwnd, TIMER_MENU, USER_TIMER_MINIMUM, nullptr);
        return 0;
    case WM_EXITMENULOOP:
        KillTimer(hwnd, TIMER_MENU);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_MENU)
        {
            // 待ち（WaitUntil はメッセージを処理する）は入れず、1フレーム描いて矢印を直すだけ。入れ子にならないよう印を立てる
            static bool s_busy = false;
            if (!s_busy)
            {
                s_busy = true;
                Frame();
                if (g_presented) UpdateArrow();
                s_busy = false;
            }
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case CMD_TOGGLE: ToggleLoupe(); break;
        case CMD_INI: ShellExecuteW(nullptr, L"open", (g_dir + L"HiResLoupe.ini").c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case CMD_LOG: ShellExecuteW(nullptr, L"open", (g_dir + L"HiResLoupe.log").c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case CMD_RELOAD: ReloadSettings(); break;
        case CMD_STARTUP: SetStartup(!StartupRegistered()); break;
        case CMD_LIST: ShowShortcutList(); break;
        case CMD_HK: case CMD_HK + 1: case CMD_HK + 2: case CMD_HK + 3: case CMD_HK + 4: case CMD_HK + 5: OpenHotkeyDialog((int)(LOWORD(wp) - CMD_HK)); break;
        case CMD_PINCUR:
            g_set.pinCursor = !g_set.pinCursor;
            WritePrivateProfileStringW(L"lens", L"pinned_cursor", g_set.pinCursor ? L"1" : L"0", (g_dir + L"HiResLoupe.ini").c_str());
            break;
        case CMD_LANG: case CMD_LANG + 1: case CMD_LANG + 2: case CMD_LANG + 3:
            g_set.lang = (int)(LOWORD(wp) - CMD_LANG);
            WritePrivateProfileStringW(L"ui", L"language", kLangCodes[g_set.lang], (g_dir + L"HiResLoupe.ini").c_str());
            LoadTexts();
            UpdateTrayTip();
            break;
        case CMD_QUIT: DestroyWindow(hwnd); break;
        }
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_SETCURSOR:
        if (g_hot) { SetCursor(EdgeCursor(g_hotCur)); return TRUE; }
        break;
    case WM_HOTKEY:
        if (wp == HOTKEY_TOGGLE || wp == HOTKEY_TOGGLE2) { if (g.active) Deactivate(); else Activate(); }
        else if (wp == HOTKEY_EXIT) Deactivate();
        else if (wp == HOTKEY_LATCH && g.active)
        {
            g.latched = !g.latched;
            if (g.latched) g.pin = POINT{ g_lensL + g_lensWd / 2, g_lensT + g_lensHt / 2 };   // 今のレンズの中心で止める
            // 固定中は下のアプリでキーを打てるよう、数字キー（形のプリセット）を返す
            for (int i = 0; i < 3; ++i)
            {
                if (g.latched) UnregisterHotKey(g.hwnd, HOTKEY_PRESET1 + i);
                else RegisterHotKey(g.hwnd, HOTKEY_PRESET1 + i, g_set.presetMods[i], g_set.presetKey[i]);
            }
        }
        else if (wp >= HOTKEY_PRESET1 && wp < HOTKEY_PRESET1 + 3) { if (g.active) ApplyPreset((int)(wp - HOTKEY_PRESET1) + 1); }
        return 0;
    case WM_APP_WHEEL:
        if (g.active) OnWheel((int)wp, lp != 0);
        return 0;
    case WM_APP_EXIT:
        Deactivate();
        return 0;
    case WM_DESTROY:
        SetCursorsHidden(false);
        RemoveTray();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- 自己診断（--selftest）
// 画面を1枚取って各画質で拡大し、速さと画像(BMP)を残す。窓は出さない。
static bool SaveTexBmp(ID3D11Texture2D* tex, RECT r, const std::wstring& path)
{
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    UINT w = r.right - r.left, h = r.bottom - r.top;
    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = w;
    sd.Height = h;
    sd.MipLevels = sd.ArraySize = 1;
    sd.Format = d.Format;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stg;
    if (FAILED(gpu.dev->CreateTexture2D(&sd, nullptr, &stg))) return false;
    D3D11_BOX box{ (UINT)r.left, (UINT)r.top, 0, (UINT)r.right, (UINT)r.bottom, 1 };
    gpu.ctx->CopySubresourceRegion(stg.Get(), 0, 0, 0, 0, tex, 0, &box);
    D3D11_MAPPED_SUBRESOURCE mp;
    if (FAILED(gpu.ctx->Map(stg.Get(), 0, D3D11_MAP_READ, 0, &mp))) return false;
    FILE* f = nullptr;
    bool ok = _wfopen_s(&f, path.c_str(), L"wb") == 0 && f;
    if (ok)
    {
        BITMAPFILEHEADER bf{ 0x4D42, (DWORD)(54 + w * h * 4), 0, 0, 54 };
        BITMAPINFOHEADER bi{ 40, (LONG)w, -(LONG)h, 1, 32, BI_RGB, w * h * 4, 0, 0, 0, 0 };
        fwrite(&bf, sizeof bf, 1, f);
        fwrite(&bi, sizeof bi, 1, f);
        for (UINT y = 0; y < h; ++y) fwrite((BYTE*)mp.pData + (size_t)y * mp.RowPitch, 4, w, f);
        fclose(f);
    }
    gpu.ctx->Unmap(stg.Get(), 0);
    return ok;
}

static void WaitGpu()
{
    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = sd.Height = 1;
    sd.MipLevels = sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stg;
    gpu.dev->CreateTexture2D(&sd, nullptr, &stg);
    D3D11_BOX box{ 0, 0, 0, 1, 1, 1 };
    gpu.ctx->CopySubresourceRegion(stg.Get(), 0, 0, 0, 0, gpu.outTex.Get(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE mp;
    if (SUCCEEDED(gpu.ctx->Map(stg.Get(), 0, D3D11_MAP_READ, 0, &mp))) gpu.ctx->Unmap(stg.Get(), 0);
}

static int SelfTest()
{
    POINT c;
    GetCursorPos(&c);
    if (!gpu.EnsureFor(MonitorFromPoint(c, MONITOR_DEFAULTTOPRIMARY))) { Log(L"selftest: 準備に失敗"); return 1; }
    for (int i = 0; i < 5 && !gpu.haveFrame; ++i) gpu.Capture(300);
    if (!gpu.haveFrame) { Log(L"selftest: 画面が取れない"); return 1; }

    const int lens = g_set.lensSize;
    int srcW = std::min<int>((int)(lens * 100.0 / g_set.zoomDefault + 0.5), (int)gpu.texW);
    int sx = std::clamp<int>(c.x - gpu.monRect.left - srcW / 2, 0, (int)gpu.texW - srcW);
    int sy = std::clamp<int>(c.y - gpu.monRect.top - srcW / 2, 0, (int)gpu.texH - srcW);
    RECT src{ sx, sy, sx + srcW, sy + srcW };
    SaveTexBmp(gpu.desktopTex.Get(), src, g_dir + L"selftest_input.bmp");
    Log(L"selftest: モニター %ldx%ld 入力範囲 %dx%d -> 出力 %dx%d (倍率 %d%%)", gpu.monRect.right - gpu.monRect.left, gpu.monRect.bottom - gpu.monRect.top, srcW, srcW, lens, lens, g_set.zoomDefault);

    LARGE_INTEGER fq, t0, t1;
    QueryPerformanceFrequency(&fq);
    for (int q = 0; q <= 4; ++q)
    {
        if (!gpu.Upscale(src, lens, lens, q)) { Log(L"selftest: 画質%d は失敗", q); continue; }
        WaitGpu();
        const int N = 30;
        QueryPerformanceCounter(&t0);
        for (int i = 0; i < N; ++i) gpu.Upscale(src, lens, lens, q);
        WaitGpu();
        QueryPerformanceCounter(&t1);
        Log(L"selftest: 画質%d  1枚あたり %.2f ms", q, (t1.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart / N);
        if (q == 0 || q == 4)
        {
            gpu.Upscale(src, lens, lens, q);
            SaveTexBmp(gpu.outTex.Get(), RECT{ 0, 0, lens, lens }, g_dir + (q == 0 ? L"selftest_q0_bicubic.bmp" : L"selftest_q4_vsr.bmp"));
        }
    }

    // 倍率の自由度を確かめる（225% / 150% / 400% / 800% の入力サイズ）
    for (int z : { 100, 110, 125, 133, 150, 175, 190, 200, 225, 400, 800 })
    {
        int sw = std::max(8, (int)(lens * 100.0 / z + 0.5));
        RECT s2{ sx, sy, sx + sw, sy + sw };
        bool ok = gpu.Upscale(s2, lens, lens, 4);
        Log(L"selftest: 倍率%d%% (入力%dx%d) -> %s", z, sw, sw, ok ? L"成功" : L"失敗");
    }
    // 小さい入力
    for (int sw : { 16, 32, 64, 128 })
    {
        RECT s2{ sx, sy, sx + sw, sy + sw };
        bool ok = gpu.Upscale(s2, lens, lens, 4);
        Log(L"selftest: 入力%dx%d -> %dx%d は %s", sw, sw, lens, lens, ok ? L"成功" : L"失敗");
    }
    gpu.Shutdown();
    return 0;
}

// 診断（--shifttest）: 拡大元を1pxずつずらして拡大した結果が、ずらした分だけ平行移動しただけの同じ絵になるか調べる。
// 毎フレーム描き直しても、描いた位置が違うだけで絵が変わらないかの確認用。shift_N.bmp を出す
static int ShiftTest()
{
    POINT c;
    GetCursorPos(&c);
    if (!gpu.EnsureFor(MonitorFromPoint(c, MONITOR_DEFAULTTOPRIMARY))) { Log(L"shifttest: 準備に失敗"); return 1; }
    for (int i = 0; i < 5 && !gpu.haveFrame; ++i) gpu.Capture(300);
    if (!gpu.haveFrame) { Log(L"shifttest: 画面が取れない"); return 1; }
    const int lens = g_set.lensSize;
    int srcW = std::min<int>((int)(lens * 100.0 / g_set.zoomDefault + 0.5), (int)gpu.texW - 16);
    int sx = std::clamp<int>(c.x - gpu.monRect.left - srcW / 2, 0, (int)gpu.texW - srcW - 16);
    int sy = std::clamp<int>(c.y - gpu.monRect.top - srcW / 2, 0, (int)gpu.texH - srcW);
    for (int s = 0; s <= 8; ++s)
    {
        RECT src{ sx + s, sy, sx + s + srcW, sy + srcW };
        bool ok = gpu.Upscale(src, lens, lens, g_set.vsrQuality);
        WaitGpu();
        if (ok) SaveTexBmp(gpu.outTex.Get(), RECT{ 0, 0, lens, lens }, g_dir + L"shift_" + std::to_wstring(s) + L".bmp");
        Log(L"shifttest: 拡大元を右へ%dpx %s (入力%dx%d -> 出力%dx%d 画質%d)", s, ok ? L"成功" : L"失敗", srcW, srcW, lens, lens, g_set.vsrQuality);
    }
    gpu.Shutdown();
    return 0;
}

// ---------------------------------------------------------------- 起動
// 診断(--cursortest): 写し取ったカーソルの表を、白地と黒地に載せて2倍にした BMP(cursor_atlas.bmp)に出す。GPU は使わない
static int CursorTest()
{
    CaptureCursors();
    if (g_curAtlas.empty()) return 1;
    const int W = kCurN * kNCells, H = kCurN, S = 2;
    const int ow = kCurN * kNCur * S, oh = H * 2 * S;
    const int stride = (ow * 3 + 3) & ~3;
    std::vector<uint8_t> img((size_t)stride * oh);
    for (int y = 0; y < oh; ++y)
        for (int x = 0; x < ow; ++x)
        {
            const int row = y / S, srcY = row % H, bg = row < H ? 255 : 0;
            uint32_t p = g_curAtlas[(size_t)srcY * W + x / S];
            uint32_t a = p >> 24;
            uint8_t* d = &img[(size_t)(oh - 1 - y) * stride + (size_t)x * 3];
            d[2] = (uint8_t)(((p >> 16) & 0xFF) + bg * (255 - a) / 255);
            d[1] = (uint8_t)(((p >> 8) & 0xFF) + bg * (255 - a) / 255);
            d[0] = (uint8_t)((p & 0xFF) + bg * (255 - a) / 255);
        }
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof ih; ih.biWidth = ow; ih.biHeight = oh; ih.biPlanes = 1; ih.biBitCount = 24; ih.biCompression = BI_RGB;
    ih.biSizeImage = (DWORD)img.size();
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + ih.biSizeImage;
    FILE* f = _wfopen((g_dir + L"cursor_atlas.bmp").c_str(), L"wb");
    if (!f) return 1;
    fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f); fwrite(img.data(), 1, img.size(), f);
    fclose(f);
    SetCursorsHidden(false);
    return 0;
}

// 診断(--cursorcmp): 写し取った表のカーソルと、Windows が本物を描く方法(DrawIconEx)で描いたカーソルを、白地・黒地に載せて画素で比べ、
// cursor_compare.txt に出す。GPU は使わない。「縁が太い・色が濃い」の原因が、表の作り方か表示側かを分けるための診断
static int CursorCompare()
{
    CaptureCursors();
    if (g_curAtlas.empty()) return 1;
    const int N = kCurN, W = kCurN * kNCells;
    FILE* f = _wfopen((g_dir + L"cursor_compare.txt").c_str(), L"wb");
    if (!f) return 1;
    fprintf(f, "SM_CXCURSOR=%d SM_CYCURSOR=%d\r\n", GetSystemMetrics(SM_CXCURSOR), GetSystemMetrics(SM_CYCURSOR));
    fprintf(f, "aniFrames wait=%d(%dms) appstarting=%d(%dms)\r\n", g_aniFrames[0], g_aniMs[0], g_aniFrames[1], g_aniMs[1]);
    fprintf(f, "id,srcSize,bg,pixelsA(atlas>0),pixelsRef(ref differs from bg),diffPixels(>8),maxDiff,sumAtlasDarker,sumRefDarker,meanSigned(atlas-ref)\r\n");
    HDC sdc = GetDC(nullptr);
    HDC mdc = CreateCompatibleDC(sdc);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = N; bi.bmiHeader.biHeight = -N;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    for (int i = 0; i < kNCur; ++i)
    {
        HCURSOR cur = LoadCursorW(nullptr, MAKEINTRESOURCEW(kCurIds[i]));
        if (!cur) continue;
        ICONINFO ii{};
        int srcSize = 0;
        if (GetIconInfo(cur, &ii))
        {
            BITMAP bm{};
            if (ii.hbmMask && GetObject(ii.hbmMask, sizeof bm, &bm)) srcSize = bm.bmWidth;
            if (ii.hbmMask) DeleteObject(ii.hbmMask);
            if (ii.hbmColor) DeleteObject(ii.hbmColor);
        }
        for (int bgi = 0; bgi < 2; ++bgi)
        {
            const uint32_t bg = bgi == 0 ? 255 : 0;
            uint32_t* bits = nullptr;
            HBITMAP dib = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0);
            if (!dib) continue;
            HGDIOBJ old = SelectObject(mdc, dib);
            for (int k = 0; k < N * N; ++k) bits[k] = 0xFF000000u | (bg * 0x010101u);
            DrawIconEx(mdc, 0, 0, cur, 0, 0, 0, nullptr, DI_NORMAL);
            GdiFlush();
            int pa = 0, pr = 0, dp = 0, maxd = 0;
            long long darkA = 0, darkR = 0, sgn = 0;
            for (int y = 0; y < N; ++y)
                for (int x = 0; x < N; ++x)
                {
                    uint32_t p = g_curAtlas[(size_t)y * W + (size_t)i * N + x];
                    uint32_t a = p >> 24;
                    if (a) ++pa;
                    uint32_t ref = bits[y * N + x] & 0xFFFFFF;
                    if (ref != bg * 0x010101u) ++pr;
                    int ours[3] = { (int)((p >> 16) & 0xFF) + (int)(bg * (255 - a) / 255), (int)((p >> 8) & 0xFF) + (int)(bg * (255 - a) / 255), (int)(p & 0xFF) + (int)(bg * (255 - a) / 255) };
                    int theirs[3] = { (int)((ref >> 16) & 0xFF), (int)((ref >> 8) & 0xFF), (int)(ref & 0xFF) };
                    int d = 0;
                    for (int c = 0; c < 3; ++c)
                    {
                        int e = ours[c] - theirs[c];
                        d = std::max(d, std::abs(e));
                        sgn += e;
                        if (e < 0) darkA += -e; else darkR += e;
                    }
                    maxd = std::max(maxd, d);
                    if (d > 8) ++dp;
                }
            fprintf(f, "%u,%d,%s,%d,%d,%d,%d,%lld,%lld,%.2f\r\n", (unsigned)kCurIds[i], srcSize, bg ? "white" : "black", pa, pr, dp, maxd, darkA, darkR, (double)sgn / 3.0 / (double)(N * N));
            SelectObject(mdc, old);
            DeleteObject(dib);
        }
    }
    DeleteDC(mdc);
    ReleaseDC(nullptr, sdc);
    fclose(f);
    SetCursorsHidden(false);
    return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int)
{
    g_inst = inst;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    g_dir = path;
    g_dir.resize(g_dir.find_last_of(L'\\') + 1);
    LoadSettings();
    g.zoom = std::clamp(g_set.zoomDefault, g_set.zoomMin, g_set.zoomMax);
    g.lensW = g.lensH = std::max(g_set.lensSize, g_set.lensMin);

    if (wcsstr(GetCommandLineW(), L"--selftest")) return SelfTest();
    if (wcsstr(GetCommandLineW(), L"--shifttest")) return ShiftTest();
    if (wcsstr(GetCommandLineW(), L"--cursortest")) return CursorTest();
    if (wcsstr(GetCommandLineW(), L"--cursorcmp")) return CursorCompare();

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"HiResLoupe_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0);   // 前回、異常終了でカーソルが透明のまま残っていたときの戻し

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"HiResLoupeLens";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT | WS_EX_LAYERED;   // layered も付けるとクリックが下に通る
    g.hwnd = CreateWindowExW(exStyle, wc.lpszClassName, L"HiResLoupe", WS_POPUP, 0, 0, g.lensW, g.lensH, nullptr, nullptr, inst, nullptr);
    if (!g.hwnd) return 1;
    SetLayeredWindowAttributes(g.hwnd, 0, 255, LWA_ALPHA);
    if (g_set.excludeSelf && !SetWindowDisplayAffinity(g.hwnd, WDA_EXCLUDEFROMCAPTURE)) Log(L"SetWindowDisplayAffinity 失敗 err=%lu", GetLastError());

    bool ok1 = RegisterHotKey(g.hwnd, HOTKEY_TOGGLE, g_set.toggleMods, g_set.toggleKey) != 0;
    bool ok2 = g_set.toggle2Key && RegisterHotKey(g.hwnd, HOTKEY_TOGGLE2, g_set.toggle2Mods, g_set.toggle2Key);
    if (!ok1 && !ok2)
    {
        Log(L"ホットキーを登録できなかった");
        MessageBoxW(nullptr, L"Could not register the hotkey. Change [hotkey] in HiResLoupe.ini.", L"HiResLoupe", MB_ICONERROR);
        return 1;
    }
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    AddTray();
    Log(L"起動 toggle=%s toggle2=%s", ok1 ? L"OK" : L"NG", ok2 ? L"OK" : L"NG");
    if (StartupRegistered()) SetStartup(true);   // exe を動かしたあとでも、今の場所で登録し直す
    // exe を起動した直後からレンズを出す（トレイ・ホットキーでの切り替えはその後も使える）。Windows 起動時の自動起動(--startup)ではトレイだけ
    if (!wcsstr(GetCommandLineW(), L"--startup")) Activate();

    MSG msg;
    for (;;)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) { Deactivate(); gpu.Shutdown(); CloseHandle(mutex); return 0; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g.active) { WaitMessage(); continue; }
        // dcomp で合成の直前に位置を直すときは、1フレームごとに DwmFlush で合成の周期にそろえる（下）。
        // スワップチェーンの合図に任せると、こちらの待ちで決まる周期(62fps)になり、画面の更新(60Hz)とずれて数十フレームに1回コマ飛びした
        double tWake = NowMs();
        double dwmMs = 0;
        if (DwmPeriodMs(dwmMs)) g_period = dwmMs;   // 画面の実際の周期。自分の間隔から平均すると、伸びたまま戻らなくなる
        else if (g_prevWake > 0)
        {
            double d = tWake - g_prevWake;
            if (d > 8.0 && d < 40.0) g_period = g_period * 0.9 + d * 0.1;
        }
        g_prevWake = tWake;
        Frame();
        if (g_presented)
        {
            if (g_set.lateMs > 0) LateCommit(tWake);
            DwmFlush();   // 次の合成が終わるまで待つ。これで1フレームが合成の周期にそろう
        }
        if (!g_presented) Sleep(1);   // 描画できなかったときに空回りしない
    }
}
