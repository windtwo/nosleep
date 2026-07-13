// NoSleep - 纯 Win32 API 版本
// 编译：gcc -s -static -o nosleep_pure.exe nosleep_win32.c nosleep.res -mwindows -municode -lcomctl32 -lshell32

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

#define IDI_APP_ICON 1

#define ID_TRAY 1
#define ID_COUNTDOWN_TIMER 1
#define ID_EXIT_TIMER 2
#define ID_INITIAL_BALLOON_TIMER 3
#define ID_EXIT 1001
#define ID_POSTPONE_30 1002
#define ID_EDIT_MINUTES 101
#define ID_START_CUSTOM 102
#define ID_LABEL_TITLE 103
#define ID_LABEL_PRESETS 104
#define ID_LABEL_CUSTOM 105
#define ID_LABEL_MINUTES 106
#define ID_PRESET_30 1101
#define ID_PRESET_60 1102
#define ID_PRESET_90 1103
#define ID_PRESET_120 1104
#define ID_PRESET_180 1105
#define ID_PRESET_240 1106
#define WM_TRAYICON (WM_USER + 1)
#define MAX_MINUTES 1440
#define POSTPONE_MILLISECONDS (30ULL * 60ULL * 1000ULL)
#define COUNTDOWN_REFRESH_MILLISECONDS 10000U
#define DEFAULT_DPI 96U
#define INPUT_CLIENT_WIDTH 360
#define INPUT_CLIENT_HEIGHT 254

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

// 单实例互斥量
static HANDLE g_hMutex = NULL;

static int g_minutes = 0;
static ULONGLONG g_deadlineMs = 0;
static HWND g_hwnd = NULL;
static HICON g_hicon = NULL;
static NOTIFYICONDATAW g_nid = {0};
static UINT g_taskbarCreatedMessage = 0;
static BOOL g_trayIconAdded = FALSE;
static HFONT g_inputTitleFont = NULL;
static HFONT g_inputSectionFont = NULL;
static HFONT g_inputBodyFont = NULL;

typedef BOOL (WINAPI *SetProcessDpiAwarenessContextFunc)(HANDLE);
typedef HRESULT (WINAPI *SetProcessDpiAwarenessFunc)(int);
typedef BOOL (WINAPI *SetProcessDPIAwareFunc)(void);
typedef UINT (WINAPI *GetDpiForWindowFunc)(HWND);
typedef BOOL (WINAPI *AdjustWindowRectExForDpiFunc)(LPRECT, DWORD, BOOL, DWORD, UINT);

typedef struct PresetButton
{
    int id;
    const wchar_t *text;
    int minutes;
} PresetButton;

static const PresetButton g_presetButtons[] = {
    {ID_PRESET_30, L"30 分钟", 30},
    {ID_PRESET_60, L"1 小时", 60},
    {ID_PRESET_90, L"90 分钟", 90},
    {ID_PRESET_120, L"2 小时", 120},
    {ID_PRESET_180, L"3 小时", 180},
    {ID_PRESET_240, L"4 小时", 240}
};

static int ScaleForDpi(int value, UINT dpi)
{
    return MulDiv(value, (int)dpi, (int)DEFAULT_DPI);
}

static void LoadFunctionAddress(HMODULE module, const char *name, void *target, size_t targetSize)
{
    FARPROC address = module ? GetProcAddress(module, name) : NULL;
    if (targetSize == sizeof(address))
    {
        memcpy(target, &address, targetSize);
    }
}

static void EnableBestDpiAwareness(void)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    SetProcessDpiAwarenessContextFunc setContext = NULL;
    SetProcessDPIAwareFunc setAware = NULL;

    if (user32)
    {
        LoadFunctionAddress(user32, "SetProcessDpiAwarenessContext",
            &setContext, sizeof(setContext));
        if (setContext)
        {
            SetLastError(ERROR_SUCCESS);
            if (setContext((HANDLE)(LONG_PTR)-4) || GetLastError() == ERROR_ACCESS_DENIED)
            {
                return;
            }
        }
    }

    // Windows 8.1 supports per-monitor DPI through Shcore.dll.
    {
        HMODULE shcore = LoadLibraryW(L"shcore.dll");
        if (shcore)
        {
            SetProcessDpiAwarenessFunc setAwareness = NULL;
            LoadFunctionAddress(shcore, "SetProcessDpiAwareness",
                &setAwareness, sizeof(setAwareness));
            if (setAwareness)
            {
                HRESULT result = setAwareness(2);
                FreeLibrary(shcore);
                if (SUCCEEDED(result) || result == E_ACCESSDENIED)
                {
                    return;
                }
            }
            else
            {
                FreeLibrary(shcore);
            }
        }
    }

    if (user32)
    {
        LoadFunctionAddress(user32, "SetProcessDPIAware", &setAware, sizeof(setAware));
        if (setAware)
        {
            setAware();
        }
    }
}

static UINT GetWindowDpiCompat(HWND hwnd)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    GetDpiForWindowFunc getDpi = NULL;
    UINT dpi = 0;

    if (user32)
    {
        LoadFunctionAddress(user32, "GetDpiForWindow", &getDpi, sizeof(getDpi));
        if (getDpi)
        {
            dpi = getDpi(hwnd);
        }
    }

    if (dpi == 0)
    {
        HDC hdc = GetDC(hwnd);
        if (hdc)
        {
            dpi = (UINT)GetDeviceCaps(hdc, LOGPIXELSX);
            ReleaseDC(hwnd, hdc);
        }
    }

    return dpi == 0 ? DEFAULT_DPI : dpi;
}

static void StartInputWithMinutes(HWND hwnd, int minutes)
{
    g_minutes = minutes;
    DestroyWindow(hwnd);
}

static BOOL ParseMinutes(const wchar_t *text, int *minutes)
{
    wchar_t *end = NULL;
    long value;

    errno = 0;
    value = wcstol(text, &end, 10);
    if (end == text || errno == ERANGE)
    {
        return FALSE;
    }

    while (*end != L'\0' && iswspace(*end))
    {
        end++;
    }

    if (*end != L'\0' || value < 1 || value > MAX_MINUTES)
    {
        return FALSE;
    }

    *minutes = (int)value;
    return TRUE;
}

// 持续阻止系统睡眠和显示器关闭
static BOOL EnableKeepAwake(void)
{
    return SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED) != 0;
}

static void DisableKeepAwake(void)
{
    SetThreadExecutionState(ES_CONTINUOUS);
}

// 加载图标
static HICON LoadAppIcon(void)
{
    HICON hicon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APP_ICON));
    return hicon ? hicon : LoadIcon(NULL, IDI_APPLICATION);
}

// 显示气球提示
static void ShowBalloon(const wchar_t *title, const wchar_t *info)
{
    NOTIFYICONDATAW nid = {0};
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = g_hwnd;
    nid.uID = ID_TRAY;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    nid.hIcon = g_hicon;
    wcsncpy(nid.szInfoTitle, title, 63);
    wcsncpy(nid.szInfo, info, 255);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// 更新托盘文本
static int GetRemainingSeconds(void)
{
    ULONGLONG now = GetTickCount64();
    ULONGLONG remainingMs;
    ULONGLONG remainingSeconds;

    if (now >= g_deadlineMs)
    {
        return 0;
    }

    remainingMs = g_deadlineMs - now;
    remainingSeconds = (remainingMs + 999ULL) / 1000ULL;
    return remainingSeconds > INT_MAX ? INT_MAX : (int)remainingSeconds;
}

static void UpdateTrayTip(int seconds)
{
    wchar_t tip[128];
    swprintf(tip, 128, L"NoSleep: 还剩 %d 分 %02d 秒", seconds / 60, seconds % 60);

    NOTIFYICONDATAW nid = {0};
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = g_hwnd;
    nid.uID = ID_TRAY;
    nid.uFlags = NIF_TIP;
    wcsncpy(nid.szTip, tip, 127);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static BOOL ScheduleCountdownTimer(HWND hwnd)
{
    ULONGLONG now = GetTickCount64();
    ULONGLONG remainingMs = g_deadlineMs > now ? g_deadlineMs - now : 0;
    UINT interval;

    if (remainingMs == 0)
    {
        return FALSE;
    }

    interval = remainingMs > COUNTDOWN_REFRESH_MILLISECONDS
        ? COUNTDOWN_REFRESH_MILLISECONDS
        : (UINT)remainingMs;
    if (interval < USER_TIMER_MINIMUM)
    {
        interval = USER_TIMER_MINIMUM;
    }

    KillTimer(hwnd, ID_COUNTDOWN_TIMER);
    return SetTimer(hwnd, ID_COUNTDOWN_TIMER, interval, NULL) != 0;
}

// 添加托盘图标
static BOOL AddTrayIcon(void)
{
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = ID_TRAY;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = g_hicon;
    wcsncpy(g_nid.szTip, L"NoSleep", 63);
    g_trayIconAdded = Shell_NotifyIconW(NIM_ADD, &g_nid);
    return g_trayIconAdded;
}

// 移除托盘图标
static void RemoveTrayIcon(void)
{
    if (g_trayIconAdded)
    {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayIconAdded = FALSE;
    }
}

static void HandleCountdownTimer(HWND hwnd)
{
    int seconds = GetRemainingSeconds();

    if (seconds == 0)
    {
        KillTimer(hwnd, ID_COUNTDOWN_TIMER);
        UpdateTrayTip(0);
        ShowBalloon(L"已完成", L"防休眠时间已结束");
        if (SetTimer(hwnd, ID_EXIT_TIMER, 2000, NULL) == 0)
        {
            DestroyWindow(hwnd);
        }
        return;
    }

    UpdateTrayTip(seconds);
    if (!ScheduleCountdownTimer(hwnd))
    {
        MessageBoxW(hwnd, L"无法继续运行倒计时。", L"NoSleep 错误", MB_OK | MB_ICONERROR);
        DestroyWindow(hwnd);
    }
}

static void PostponeThirtyMinutes(HWND hwnd)
{
    ULONGLONG now = GetTickCount64();

    if (g_deadlineMs < now)
    {
        g_deadlineMs = now;
    }
    g_deadlineMs += POSTPONE_MILLISECONDS;

    KillTimer(hwnd, ID_EXIT_TIMER);
    if (!EnableKeepAwake() || !ScheduleCountdownTimer(hwnd))
    {
        MessageBoxW(hwnd, L"无法继续保持系统唤醒。", L"NoSleep 错误", MB_OK | MB_ICONERROR);
        DestroyWindow(hwnd);
        return;
    }

    UpdateTrayTip(GetRemainingSeconds());
    ShowBalloon(L"NoSleep", L"已推迟 30 分钟");
}

// 托盘窗口过程
static LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (g_taskbarCreatedMessage != 0 && msg == g_taskbarCreatedMessage)
    {
        g_trayIconAdded = FALSE;
        if (AddTrayIcon())
        {
            UpdateTrayTip(GetRemainingSeconds());
        }
        return 0;
    }

    switch (msg)
    {
        case WM_TRAYICON:
            if (lParam == WM_RBUTTONUP)
            {
                HMENU hmenu = CreatePopupMenu();
                AppendMenuW(hmenu, MF_STRING, ID_POSTPONE_30, L"推迟半小时");
                AppendMenuW(hmenu, MF_SEPARATOR, 0, NULL);
                AppendMenuW(hmenu, MF_STRING, ID_EXIT, L"退出");
                POINT pt;
                GetCursorPos(&pt);
                SetForegroundWindow(hwnd);
                TrackPopupMenu(hmenu, 0, pt.x, pt.y, 0, hwnd, NULL);
                PostMessageW(hwnd, WM_NULL, 0, 0);
                DestroyMenu(hmenu);
            }
            else if (lParam == WM_LBUTTONDBLCLK)
            {
                // 双击显示剩余时间
                int seconds = GetRemainingSeconds();
                if (seconds > 0)
                {
                    wchar_t info[64];
                    swprintf(info, 64, L"还剩 %d 分 %02d 秒", seconds / 60, seconds % 60);
                    ShowBalloon(L"剩余时间", info);
                }
            }
            break;

        case WM_COMMAND:
            if (LOWORD(wParam) == ID_POSTPONE_30)
            {
                PostponeThirtyMinutes(hwnd);
            }
            else if (LOWORD(wParam) == ID_EXIT)
            {
                DestroyWindow(hwnd);
            }
            break;

        case WM_TIMER:
            if (wParam == ID_COUNTDOWN_TIMER)
            {
                HandleCountdownTimer(hwnd);
            }
            else if (wParam == ID_EXIT_TIMER)
            {
                // 2 秒后退出
                KillTimer(hwnd, ID_EXIT_TIMER);
                if (GetRemainingSeconds() == 0)
                {
                    DestroyWindow(hwnd);
                }
            }
            else if (wParam == ID_INITIAL_BALLOON_TIMER)
            {
                wchar_t info[128];
                KillTimer(hwnd, ID_INITIAL_BALLOON_TIMER);
                swprintf(info, 128, L"将保持唤醒 %d 分钟", g_minutes);
                ShowBalloon(L"NoSleep", info);
            }
            break;

        case WM_POWERBROADCAST:
            if (wParam == PBT_APMRESUMEAUTOMATIC && GetRemainingSeconds() > 0)
            {
                if (!EnableKeepAwake())
                {
                    MessageBoxW(hwnd, L"系统恢复后无法继续保持唤醒。", L"NoSleep 错误", MB_OK | MB_ICONERROR);
                    DestroyWindow(hwnd);
                }
            }
            return TRUE;

        case WM_DESTROY:
            KillTimer(hwnd, ID_COUNTDOWN_TIMER);
            KillTimer(hwnd, ID_EXIT_TIMER);
            KillTimer(hwnd, ID_INITIAL_BALLOON_TIMER);
            RemoveTrayIcon();
            DisableKeepAwake();
            PostQuitMessage(0);
            break;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            break;

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

static void DeleteInputFonts(void)
{
    if (g_inputTitleFont)
    {
        DeleteObject(g_inputTitleFont);
        g_inputTitleFont = NULL;
    }
    if (g_inputSectionFont)
    {
        DeleteObject(g_inputSectionFont);
        g_inputSectionFont = NULL;
    }
    if (g_inputBodyFont)
    {
        DeleteObject(g_inputBodyFont);
        g_inputBodyFont = NULL;
    }
}

static HFONT CreateInputFont(UINT dpi, int pointSize, int weight)
{
    NONCLIENTMETRICSW metrics = {0};
    LOGFONTW font = {0};

    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
    {
        font = metrics.lfMessageFont;
    }
    else
    {
        wcsncpy(font.lfFaceName, L"Microsoft YaHei UI", LF_FACESIZE - 1);
        font.lfCharSet = DEFAULT_CHARSET;
    }

    font.lfHeight = -MulDiv(pointSize, (int)dpi, 72);
    font.lfWidth = 0;
    font.lfWeight = weight;
    font.lfQuality = CLEARTYPE_QUALITY;
    font.lfItalic = FALSE;
    font.lfUnderline = FALSE;
    font.lfStrikeOut = FALSE;
    return CreateFontIndirectW(&font);
}

static void SetInputControlFont(HWND hwnd, int id, HFONT font)
{
    HWND control = GetDlgItem(hwnd, id);
    if (control)
    {
        SendMessageW(control, WM_SETFONT, (WPARAM)font, FALSE);
    }
}

static void ApplyInputFonts(HWND hwnd)
{
    size_t i;
    HFONT title = g_inputTitleFont
        ? g_inputTitleFont
        : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HFONT section = g_inputSectionFont
        ? g_inputSectionFont
        : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HFONT body = g_inputBodyFont
        ? g_inputBodyFont
        : (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    SetInputControlFont(hwnd, ID_LABEL_TITLE, title);
    SetInputControlFont(hwnd, ID_LABEL_PRESETS, section);
    SetInputControlFont(hwnd, ID_LABEL_CUSTOM, section);
    SetInputControlFont(hwnd, ID_LABEL_MINUTES, body);
    SetInputControlFont(hwnd, ID_EDIT_MINUTES, body);
    SetInputControlFont(hwnd, ID_START_CUSTOM, body);

    for (i = 0; i < sizeof(g_presetButtons) / sizeof(g_presetButtons[0]); i++)
    {
        SetInputControlFont(hwnd, g_presetButtons[i].id, body);
    }
}

static void RecreateInputFonts(HWND hwnd, UINT dpi)
{
    DeleteInputFonts();
    g_inputTitleFont = CreateInputFont(dpi, 15, 600);
    g_inputSectionFont = CreateInputFont(dpi, 9, 600);
    g_inputBodyFont = CreateInputFont(dpi, 10, FW_NORMAL);
    ApplyInputFonts(hwnd);
}

static void MoveInputControl(HWND hwnd, int id, int x, int y, int width, int height, UINT dpi)
{
    HWND control = GetDlgItem(hwnd, id);
    if (control)
    {
        SetWindowPos(control, NULL,
            ScaleForDpi(x, dpi), ScaleForDpi(y, dpi),
            ScaleForDpi(width, dpi), ScaleForDpi(height, dpi),
            SWP_NOACTIVATE | SWP_NOZORDER);
    }
}

static void LayoutInputControls(HWND hwnd, UINT dpi)
{
    size_t i;

    MoveInputControl(hwnd, ID_LABEL_TITLE, 24, 20, 312, 28, dpi);
    MoveInputControl(hwnd, ID_LABEL_PRESETS, 24, 61, 120, 18, dpi);

    for (i = 0; i < sizeof(g_presetButtons) / sizeof(g_presetButtons[0]); i++)
    {
        int column = (int)(i % 3);
        int row = (int)(i / 3);
        MoveInputControl(hwnd, g_presetButtons[i].id,
            24 + column * 108, 84 + row * 47, 96, 40, dpi);
    }

    MoveInputControl(hwnd, ID_LABEL_CUSTOM, 24, 195, 52, 36, dpi);
    MoveInputControl(hwnd, ID_EDIT_MINUTES, 84, 197, 80, 30, dpi);
    MoveInputControl(hwnd, ID_LABEL_MINUTES, 172, 195, 44, 36, dpi);
    MoveInputControl(hwnd, ID_START_CUSTOM, 250, 193, 86, 38, dpi);
}

static void SizeAndCenterInputWindow(HWND hwnd, UINT dpi)
{
    RECT rect = {
        0,
        0,
        ScaleForDpi(INPUT_CLIENT_WIDTH, dpi),
        ScaleForDpi(INPUT_CLIENT_HEIGHT, dpi)
    };
    DWORD style = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
    DWORD exStyle = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    AdjustWindowRectExForDpiFunc adjustForDpi = NULL;
    HMONITOR monitor;
    MONITORINFO monitorInfo = {0};
    int width;
    int height;
    int x;
    int y;

    if (user32)
    {
        LoadFunctionAddress(user32, "AdjustWindowRectExForDpi",
            &adjustForDpi, sizeof(adjustForDpi));
    }
    if (adjustForDpi)
    {
        adjustForDpi(&rect, style, FALSE, exStyle, dpi);
    }
    else
    {
        AdjustWindowRectEx(&rect, style, FALSE, exStyle);
    }

    width = rect.right - rect.left;
    height = rect.bottom - rect.top;
    monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (GetMonitorInfoW(monitor, &monitorInfo))
    {
        x = monitorInfo.rcWork.left + (monitorInfo.rcWork.right - monitorInfo.rcWork.left - width) / 2;
        y = monitorInfo.rcWork.top + (monitorInfo.rcWork.bottom - monitorInfo.rcWork.top - height) / 2;
    }
    else
    {
        x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
        y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
    }

    SetWindowPos(hwnd, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
}

static BOOL CreateInputControls(HWND hwnd)
{
    HINSTANCE instance = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
    HWND edit;
    size_t i;

    if (!CreateWindowExW(0, L"STATIC", L"请选择保持唤醒时长",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
            0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_LABEL_TITLE, instance, NULL))
    {
        return FALSE;
    }

    if (!CreateWindowExW(0, L"STATIC", L"常用时长",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
            0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_LABEL_PRESETS, instance, NULL))
    {
        return FALSE;
    }

    for (i = 0; i < sizeof(g_presetButtons) / sizeof(g_presetButtons[0]); i++)
    {
        DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON;
        if (i == 0)
        {
            style |= WS_GROUP;
        }
        if (!CreateWindowExW(0, L"BUTTON", g_presetButtons[i].text, style,
                0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)g_presetButtons[i].id, instance, NULL))
        {
            return FALSE;
        }
    }

    if (!CreateWindowExW(0, L"STATIC", L"自定义",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX,
            0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_LABEL_CUSTOM, instance, NULL))
    {
        return FALSE;
    }

    edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"30",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | ES_CENTER | ES_NUMBER | ES_AUTOHSCROLL,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_EDIT_MINUTES, instance, NULL);
    if (!edit)
    {
        return FALSE;
    }
    SendMessageW(edit, EM_SETLIMITTEXT, 4, 0);

    if (!CreateWindowExW(0, L"STATIC", L"分钟",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX,
            0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_LABEL_MINUTES, instance, NULL))
    {
        return FALSE;
    }

    if (!CreateWindowExW(0, L"BUTTON", L"开始",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_START_CUSTOM, instance, NULL))
    {
        return FALSE;
    }

    return TRUE;
}

static void StartCustomMinutes(HWND hwnd)
{
    wchar_t buffer[16] = {0};
    int minutes;
    HWND edit = GetDlgItem(hwnd, ID_EDIT_MINUTES);

    if (edit)
    {
        GetWindowTextW(edit, buffer, (int)(sizeof(buffer) / sizeof(buffer[0])));
    }
    if (edit && ParseMinutes(buffer, &minutes))
    {
        StartInputWithMinutes(hwnd, minutes);
    }
    else
    {
        MessageBoxW(hwnd, L"请输入 1 到 1440 之间的分钟数。", L"输入有误", MB_OK | MB_ICONWARNING);
        if (edit)
        {
            SetFocus(edit);
            SendMessageW(edit, EM_SETSEL, 0, -1);
        }
    }
}

// 输入框窗口过程
static LRESULT CALLBACK InputWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_CREATE:
        {
            UINT dpi = GetWindowDpiCompat(hwnd);
            if (!CreateInputControls(hwnd))
            {
                return -1;
            }
            RecreateInputFonts(hwnd, dpi);
            LayoutInputControls(hwnd, dpi);
            SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)g_hicon);
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)g_hicon);
            return 0;
        }

        case WM_COMMAND:
        {
            int command = LOWORD(wParam);
            size_t i;

            for (i = 0; i < sizeof(g_presetButtons) / sizeof(g_presetButtons[0]); i++)
            {
                if (command == g_presetButtons[i].id)
                {
                    StartInputWithMinutes(hwnd, g_presetButtons[i].minutes);
                    return 0;
                }
            }

            if (command == ID_START_CUSTOM || command == IDOK)
            {
                StartCustomMinutes(hwnd);
            }
            else if (command == IDCANCEL)
            {
                SendMessageW(hwnd, WM_CLOSE, 0, 0);
            }
            return 0;
        }

        case DM_GETDEFID:
            return MAKELRESULT(ID_START_CUSTOM, DC_HASDEFID);

        case WM_DPICHANGED:
        {
            UINT dpi = HIWORD(wParam);
            RECT *suggested = (RECT *)lParam;
            SetWindowPos(hwnd, NULL,
                suggested->left, suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOACTIVATE | SWP_NOZORDER);
            RecreateInputFonts(hwnd, dpi);
            LayoutInputControls(hwnd, dpi);
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }

        case WM_SETTINGCHANGE:
            RecreateInputFonts(hwnd, GetWindowDpiCompat(hwnd));
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;

        case WM_THEMECHANGED:
            RedrawWindow(hwnd, NULL, NULL,
                RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
            return 0;

        case WM_CTLCOLORSTATIC:
        {
            HDC hdc = (HDC)wParam;
            HWND control = (HWND)lParam;
            int id = GetDlgCtrlID(control);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc,
                (id == ID_LABEL_PRESETS || id == ID_LABEL_CUSTOM)
                    ? RGB(82, 88, 98)
                    : RGB(28, 31, 36));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }

        case WM_DESTROY:
            DeleteInputFonts();
            return 0;

        case WM_CLOSE:
            g_minutes = 0;
            DestroyWindow(hwnd);
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// 显示输入对话框
static int ShowInputDialog(HINSTANCE hinst)
{
    WNDCLASSW wc = {0};
    POINT cursor = {0, 0};
    HMONITOR monitor;
    MONITORINFO monitorInfo = {0};
    int initialWidth = 360;
    int initialHeight = 300;
    int initialX;
    int initialY;
    HWND hwnd;
    MSG msg;
    BOOL done = FALSE;

    wc.lpfnWndProc = InputWndProc;
    wc.hInstance = hinst;
    wc.lpszClassName = L"NoSleepInput";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.hIcon = g_hicon;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        MessageBoxW(NULL, L"无法创建输入窗口。", L"NoSleep 错误", MB_OK | MB_ICONERROR);
        return 0;
    }

    GetCursorPos(&cursor);
    monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (GetMonitorInfoW(monitor, &monitorInfo))
    {
        initialX = monitorInfo.rcWork.left
            + (monitorInfo.rcWork.right - monitorInfo.rcWork.left - initialWidth) / 2;
        initialY = monitorInfo.rcWork.top
            + (monitorInfo.rcWork.bottom - monitorInfo.rcWork.top - initialHeight) / 2;
    }
    else
    {
        initialX = (GetSystemMetrics(SM_CXSCREEN) - initialWidth) / 2;
        initialY = (GetSystemMetrics(SM_CYSCREEN) - initialHeight) / 2;
    }

    hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST | WS_EX_CONTROLPARENT,
        L"NoSleepInput", L"NoSleep 防休眠",
        WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
        initialX, initialY, initialWidth, initialHeight,
        NULL, NULL, hinst, NULL);

    if (!hwnd)
    {
        MessageBoxW(NULL, L"无法创建输入窗口。", L"NoSleep 错误", MB_OK | MB_ICONERROR);
        return 0;
    }

    SizeAndCenterInputWindow(hwnd, GetWindowDpiCompat(hwnd));
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    SetFocus(GetDlgItem(hwnd, ID_EDIT_MINUTES));
    SendMessageW(GetDlgItem(hwnd, ID_EDIT_MINUTES), EM_SETSEL, 0, -1);

    while (!done)
    {
        int result = GetMessageW(&msg, NULL, 0, 0);
        if (result <= 0)
        {
            break;
        }
        if (!IsDialogMessageW(hwnd, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_minutes != 0 || !IsWindow(hwnd))
        {
            done = TRUE;
        }
    }

    return g_minutes;
}

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE hprev, LPWSTR szcmd, int ncmd)
{
    int exitCode = 0;
    INITCOMMONCONTROLSEX controls = {0};
    (void)hprev;
    (void)szcmd;
    (void)ncmd;

    EnableBestDpiAwareness();

    // 检查是否已有实例运行
    g_hMutex = CreateMutexW(NULL, TRUE, L"NoSleep_SingleInstance_Mutex");
    if (!g_hMutex)
    {
        MessageBoxW(NULL, L"无法创建单实例锁。", L"NoSleep 错误", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        MessageBoxW(NULL, L"NoSleep 已经在运行中！\n\n请勿同时运行多个实例。", L"提示", MB_OK | MB_ICONINFORMATION);
        CloseHandle(g_hMutex);
        return 0;
    }

    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    g_hicon = LoadAppIcon();

    // 检查命令行参数
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
    {
        MessageBoxW(NULL, L"无法读取命令行参数。", L"NoSleep 错误", MB_OK | MB_ICONERROR);
        exitCode = 1;
        goto cleanup;
    }
    if (argc > 1)
    {
        if (!ParseMinutes(argv[1], &g_minutes))
        {
            MessageBoxW(NULL, L"请输入 1 到 1440 之间的分钟数。", L"输入有误", MB_OK | MB_ICONWARNING);
        }
        LocalFree(argv);
    }
    else
    {
        g_minutes = ShowInputDialog(hinst);
        LocalFree(argv);
    }

    if (g_minutes <= 0)
    {
        goto cleanup;
    }

    // 创建托盘窗口
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = hinst;
    wc.lpszClassName = L"NoSleepTray";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon = g_hicon;
    if (!RegisterClassW(&wc))
    {
        MessageBoxW(NULL, L"无法注册托盘窗口。", L"NoSleep 错误", MB_OK | MB_ICONERROR);
        exitCode = 1;
        goto cleanup;
    }

    g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    if (g_taskbarCreatedMessage == 0)
    {
        MessageBoxW(NULL, L"无法注册托盘恢复消息。", L"NoSleep 错误", MB_OK | MB_ICONERROR);
        exitCode = 1;
        goto cleanup;
    }

    // 使用隐藏顶层窗口，以便接收 Explorer 重启和电源恢复广播。
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"NoSleepTray", L"", 0, 0, 0, 0, 0, NULL, NULL, hinst, NULL);
    if (!g_hwnd)
    {
        MessageBoxW(NULL, L"无法创建托盘窗口。", L"NoSleep 错误", MB_OK | MB_ICONERROR);
        exitCode = 1;
        goto cleanup;
    }

    if (!EnableKeepAwake())
    {
        MessageBoxW(NULL, L"系统拒绝了防休眠请求。", L"NoSleep 错误", MB_OK | MB_ICONERROR);
        DestroyWindow(g_hwnd);
        exitCode = 1;
        goto cleanup;
    }

    // 启动倒计时
    g_deadlineMs = GetTickCount64() + (ULONGLONG)g_minutes * 60ULL * 1000ULL;
    if (!ScheduleCountdownTimer(g_hwnd))
    {
        MessageBoxW(NULL, L"无法启动倒计时。", L"NoSleep 错误", MB_OK | MB_ICONERROR);
        DestroyWindow(g_hwnd);
        exitCode = 1;
        goto cleanup;
    }

    if (!AddTrayIcon())
    {
        MessageBoxW(NULL, L"无法创建托盘图标。", L"NoSleep 错误", MB_OK | MB_ICONERROR);
        DestroyWindow(g_hwnd);
        exitCode = 1;
        goto cleanup;
    }
    UpdateTrayTip(GetRemainingSeconds());

    // 通过定时器延迟通知，避免阻塞主消息循环。
    if (SetTimer(g_hwnd, ID_INITIAL_BALLOON_TIMER, 500, NULL) == 0)
    {
        wchar_t info[128];
        swprintf(info, 128, L"将保持唤醒 %d 分钟", g_minutes);
        ShowBalloon(L"NoSleep", info);
    }

    // 消息循环
    MSG msg;
    while (TRUE)
    {
        int result = GetMessage(&msg, NULL, 0, 0);
        if (result == 0)
        {
            break;
        }
        if (result == -1)
        {
            exitCode = 1;
            break;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

cleanup:
    // 释放互斥量
    if (g_hMutex)
    {
        ReleaseMutex(g_hMutex);
        CloseHandle(g_hMutex);
    }

    return exitCode;
}
