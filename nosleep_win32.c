// NoSleep - 纯 Win32 API 版本
// 编译：make（编译参数统一定义在 Makefile 中）

// 目标系统为 Windows 7 及以上；更新的 API 一律通过 GetProcAddress 动态加载。
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#define IDI_APP_ICON 1

#define ID_TRAY 1
#define ID_COUNTDOWN_TIMER 1
#define ID_EXIT_TIMER 2
#define ID_INITIAL_BALLOON_TIMER 3
#define ID_TRAY_RETRY_TIMER 4
#define ID_EXIT 1001
#define ID_EDIT_MINUTES 101
#define ID_START_CUSTOM 102
#define ID_LABEL_TITLE 103
#define ID_LABEL_PRESETS 104
#define ID_LABEL_CUSTOM 105
#define ID_LABEL_MINUTES 106
#define ID_CHECK_DISPLAY 107
#define ID_PRESET_30 1101
#define ID_PRESET_60 1102
#define ID_PRESET_90 1103
#define ID_PRESET_120 1104
#define ID_PRESET_180 1105
#define ID_PRESET_240 1106
#define ID_SETDUR_FIRST 1201
#define ID_SETDUR_CUSTOM 1301
#define ID_TOGGLE_DISPLAY 1302
#define ID_MENU_STATUS 1303
#define WM_TRAYICON (WM_USER + 1)
#define WM_INSTANCE_REQUEST (WM_USER + 2)
#define MAX_MINUTES 1440
#define MINUTE_MILLISECONDS 60000ULL
#define TRAY_RETRY_MILLISECONDS 2000U
#define TRAY_RETRY_LIMIT 30
#define DEFAULT_DPI 96U
#define INPUT_CLIENT_WIDTH 360
#define INPUT_CLIENT_HEIGHT 294
#define INPUT_CLASS_NAME L"NoSleepInput"
#define TRAY_CLASS_NAME L"NoSleepTray"
#define ARG_ALLOW_DISPLAY_OFF L"--allow-display-off"
#define INSTANCE_REQUEST_MAGIC 0x4E534C50UL
#define PRESET_COUNT ((int)(sizeof(g_presetButtons) / sizeof(g_presetButtons[0])))

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

// 单实例互斥量
static HANDLE g_hMutex = NULL;

static int g_minutes = 0;
static BOOL g_keepDisplayOn = TRUE;
static ULONGLONG g_deadlineMs = 0;
static HWND g_hwnd = NULL;
static HWND g_inputHwnd = NULL;
static BOOL g_inputResetMode = FALSE;
static HICON g_hiconSmall = NULL;
static HICON g_hiconLarge = NULL;
static UINT g_taskbarCreatedMessage = 0;
static BOOL g_trayIconAdded = FALSE;
static BOOL g_trayVersion4 = FALSE;
static int g_trayRetryCount = 0;
static BOOL g_startBalloonPending = FALSE;
static HANDLE g_powerRequest = NULL;
static BOOL g_powerRequestDisplay = FALSE;
static BOOL g_highContrast = FALSE;

enum
{
    FONT_TITLE,
    FONT_SECTION,
    FONT_BODY,
    FONT_COUNT
};

static HFONT g_inputFonts[FONT_COUNT] = {0};

typedef BOOL (WINAPI *SetProcessDpiAwarenessContextFunc)(HANDLE);
typedef HRESULT (WINAPI *SetProcessDpiAwarenessFunc)(int);
typedef BOOL (WINAPI *SetProcessDPIAwareFunc)(void);
typedef UINT (WINAPI *GetDpiForWindowFunc)(HWND);
typedef BOOL (WINAPI *AdjustWindowRectExForDpiFunc)(LPRECT, DWORD, BOOL, DWORD, UINT);
typedef HRESULT (WINAPI *LoadIconMetricFunc)(HINSTANCE, PCWSTR, int, HICON *);

static GetDpiForWindowFunc g_pGetDpiForWindow = NULL;
static AdjustWindowRectExForDpiFunc g_pAdjustWindowRectExForDpi = NULL;

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

// 第二个实例通过 WM_COPYDATA 转交给已运行实例的请求。
typedef struct InstanceRequest
{
    int minutes;        // 0 表示打开设置窗口，1..MAX_MINUTES 表示直接重设时长
    int keepDisplayOn;  // -1 表示保持现状，0/1 表示是否保持屏幕常亮
} InstanceRequest;

static void ApplyNewDuration(HWND hwnd, int minutes);
static BOOL ShowInputWindow(BOOL resetMode);

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

static void CopyText(wchar_t *dest, size_t capacity, const wchar_t *src)
{
    wcsncpy(dest, src, capacity - 1);
    dest[capacity - 1] = L'\0';
}

// 托盘窗口是隐藏的，以它为所有者的消息框可能被其他窗口挡住，因此统一带上 MB_SETFOREGROUND。
static void ShowError(HWND owner, const wchar_t *text)
{
    MessageBoxW(owner, text, L"NoSleep 错误", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

// 兜底：nosleep.manifest 已声明 PerMonitorV2 DPI 感知，正常情况下由 manifest 生效，
// 此处的 API 调用会因 manifest 优先而返回 ACCESS_DENIED（已按成功处理）。仅当 manifest
// 被剥离（反编译/重打包）时，这段代码才会真正设置 DPI 感知。
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

// 启动时解析一次高 DPI 相关函数指针，避免每次调用都 GetProcAddress。
static void InitDpiFunctions(void)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32)
    {
        LoadFunctionAddress(user32, "GetDpiForWindow",
            &g_pGetDpiForWindow, sizeof(g_pGetDpiForWindow));
        LoadFunctionAddress(user32, "AdjustWindowRectExForDpi",
            &g_pAdjustWindowRectExForDpi, sizeof(g_pAdjustWindowRectExForDpi));
    }
}

static UINT GetWindowDpiCompat(HWND hwnd)
{
    UINT dpi = 0;

    if (g_pGetDpiForWindow)
    {
        dpi = g_pGetDpiForWindow(hwnd);
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

// 计算 width×height 的窗口在指定显示器工作区内居中时的左上角坐标。
static POINT CenterInMonitor(HMONITOR monitor, int width, int height)
{
    MONITORINFO monitorInfo = {0};
    POINT origin;

    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo))
    {
        SetRect(&monitorInfo.rcWork, 0, 0,
            GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    }

    origin.x = monitorInfo.rcWork.left
        + (monitorInfo.rcWork.right - monitorInfo.rcWork.left - width) / 2;
    origin.y = monitorInfo.rcWork.top
        + (monitorInfo.rcWork.bottom - monitorInfo.rcWork.top - height) / 2;
    return origin;
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

// 解析命令行：[分钟数] [--allow-display-off]。存在无法识别的参数时返回 FALSE。
static BOOL ParseCommandLine(int *minutes, BOOL *allowDisplayOff)
{
    int argc = 0;
    int i;
    BOOL valid = TRUE;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    *minutes = 0;
    *allowDisplayOff = FALSE;
    if (!argv)
    {
        return FALSE;
    }

    for (i = 1; i < argc; i++)
    {
        if (_wcsicmp(argv[i], ARG_ALLOW_DISPLAY_OFF) == 0)
        {
            *allowDisplayOff = TRUE;
        }
        else if (*minutes != 0 || !ParseMinutes(argv[i], minutes))
        {
            valid = FALSE;
        }
    }

    LocalFree(argv);
    return valid;
}

static ULONGLONG GetRemainingMs(void)
{
    ULONGLONG now = GetTickCount64();
    return g_deadlineMs > now ? g_deadlineMs - now : 0;
}

// 将剩余时间格式化为 "X 小时 Y 分钟"，不足 1 分钟按 1 分钟计。
static void FormatRemainingMinutes(wchar_t *out, size_t capacity, ULONGLONG remainingMs)
{
    ULONGLONG totalMinutes = (remainingMs + MINUTE_MILLISECONDS - 1) / MINUTE_MILLISECONDS;
    unsigned hours = (unsigned)(totalMinutes / 60);
    unsigned minutes = (unsigned)(totalMinutes % 60);

    if (hours > 0 && minutes > 0)
    {
        swprintf(out, capacity, L"%u 小时 %u 分钟", hours, minutes);
    }
    else if (hours > 0)
    {
        swprintf(out, capacity, L"%u 小时", hours);
    }
    else
    {
        swprintf(out, capacity, L"%u 分钟", minutes);
    }
}

// 将剩余时间格式化为 "X 小时 Y 分 ZZ 秒"（不足 1 小时则省略小时）。
static void FormatRemainingSeconds(wchar_t *out, size_t capacity, ULONGLONG remainingMs)
{
    ULONGLONG totalSeconds = (remainingMs + 999ULL) / 1000ULL;
    unsigned hours = (unsigned)(totalSeconds / 3600);
    unsigned minutes = (unsigned)((totalSeconds % 3600) / 60);
    unsigned seconds = (unsigned)(totalSeconds % 60);

    if (hours > 0)
    {
        swprintf(out, capacity, L"%u 小时 %u 分 %02u 秒", hours, minutes, seconds);
    }
    else
    {
        swprintf(out, capacity, L"%u 分 %02u 秒", minutes, seconds);
    }
}

// 将计时结束的本地时刻格式化为 "14:30"，跨天时为 "明天 08:30"。
static void FormatEndTime(wchar_t *out, size_t capacity)
{
    FILETIME fileTime;
    ULARGE_INTEGER endTime;
    SYSTEMTIME utcEnd;
    SYSTEMTIME localEnd;
    SYSTEMTIME localNow;

    GetSystemTimeAsFileTime(&fileTime);
    endTime.LowPart = fileTime.dwLowDateTime;
    endTime.HighPart = fileTime.dwHighDateTime;
    endTime.QuadPart += GetRemainingMs() * 10000ULL;
    fileTime.dwLowDateTime = endTime.LowPart;
    fileTime.dwHighDateTime = endTime.HighPart;

    if (!FileTimeToSystemTime(&fileTime, &utcEnd)
        || !SystemTimeToTzSpecificLocalTime(NULL, &utcEnd, &localEnd))
    {
        CopyText(out, capacity, L"--:--");
        return;
    }

    GetLocalTime(&localNow);
    swprintf(out, capacity, L"%ls%02u:%02u",
        (localEnd.wYear == localNow.wYear && localEnd.wMonth == localNow.wMonth
            && localEnd.wDay == localNow.wDay) ? L"" : L"明天 ",
        localEnd.wHour, localEnd.wMinute);
}

static void ReleasePowerRequest(void)
{
    if (g_powerRequest)
    {
        if (g_powerRequestDisplay)
        {
            PowerClearRequest(g_powerRequest, PowerRequestDisplayRequired);
        }
        PowerClearRequest(g_powerRequest, PowerRequestSystemRequired);
        CloseHandle(g_powerRequest);
        g_powerRequest = NULL;
        g_powerRequestDisplay = FALSE;
    }
}

static void DisableKeepAwake(void)
{
    ReleasePowerRequest();
    SetThreadExecutionState(ES_CONTINUOUS);
}

// 按当前截止时间和显示器设置创建带原因说明的电源请求，失败返回 NULL。
// 管理员命令行执行 powercfg /requests 时能看到 "NoSleep：保持唤醒至 14:30"。
static HANDLE CreatePowerRequest(void)
{
    wchar_t endTime[32];
    wchar_t reason[96];
    REASON_CONTEXT context = {0};
    HANDLE request;

    FormatEndTime(endTime, 32);
    swprintf(reason, 96, L"NoSleep：保持唤醒至 %ls%ls",
        endTime, g_keepDisplayOn ? L"" : L"（允许关闭显示器）");
    context.Version = POWER_REQUEST_CONTEXT_VERSION;
    context.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    context.Reason.SimpleReasonString = reason;

    request = PowerCreateRequest(&context);
    if (request == INVALID_HANDLE_VALUE || request == NULL)
    {
        return NULL;
    }
    if (!PowerSetRequest(request, PowerRequestSystemRequired))
    {
        CloseHandle(request);
        return NULL;
    }
    if (g_keepDisplayOn && !PowerSetRequest(request, PowerRequestDisplayRequired))
    {
        PowerClearRequest(request, PowerRequestSystemRequired);
        CloseHandle(request);
        return NULL;
    }
    return request;
}

// 申请保持唤醒。优先使用电源请求；不可用时（如 Wine 等兼容层）退回
// SetThreadExecutionState，效果相同，只是没有原因说明。
// 新状态生效后才释放旧状态，切换过程中不会出现可以睡眠的空档。
static BOOL EnableKeepAwake(void)
{
    HANDLE request = CreatePowerRequest();

    if (request)
    {
        ReleasePowerRequest();
        SetThreadExecutionState(ES_CONTINUOUS);
        g_powerRequest = request;
        g_powerRequestDisplay = g_keepDisplayOn;
        return TRUE;
    }

    if (SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED
            | (g_keepDisplayOn ? ES_DISPLAY_REQUIRED : 0)) == 0)
    {
        return FALSE;
    }
    ReleasePowerRequest();
    return TRUE;
}

// 按系统图标尺寸加载应用图标。托盘和标题栏需要小图标，LoadIcon 只能取到 32×32
// 再被系统缩小，会发虚；LoadIconMetric 会从 .ico 中挑选或高质量缩放到合适的尺寸。
static HICON LoadAppIcon(BOOL small)
{
    HINSTANCE instance = GetModuleHandleW(NULL);
    LoadIconMetricFunc loadIconMetric = NULL;
    HICON icon = NULL;

    LoadFunctionAddress(GetModuleHandleW(L"comctl32.dll"), "LoadIconMetric",
        &loadIconMetric, sizeof(loadIconMetric));
    if (loadIconMetric
        && SUCCEEDED(loadIconMetric(instance, MAKEINTRESOURCEW(IDI_APP_ICON),
            small ? LIM_SMALL : LIM_LARGE, &icon)))
    {
        return icon;
    }

    icon = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
        GetSystemMetrics(small ? SM_CXSMICON : SM_CXICON),
        GetSystemMetrics(small ? SM_CYSMICON : SM_CYICON), 0);
    return icon ? icon : CopyIcon(LoadIcon(NULL, IDI_APPLICATION));
}

// 填写托盘图标的公共字段。使用 NOTIFYICON_VERSION_4 时须带 NIF_SHOWTIP 才会显示标准提示。
static void InitTrayData(NOTIFYICONDATAW *nid, UINT flags)
{
    ZeroMemory(nid, sizeof(*nid));
    nid->cbSize = sizeof(*nid);
    nid->hWnd = g_hwnd;
    nid->uID = ID_TRAY;
    nid->uFlags = flags | NIF_SHOWTIP;
}

// 显示气泡通知。realtime 为 TRUE 时，若无法立即显示（如专注助手开启）则直接丢弃，
// 避免过时的状态信息事后堆积在通知中心。
static void ShowBalloon(const wchar_t *title, const wchar_t *info, BOOL realtime)
{
    NOTIFYICONDATAW nid;

    if (!g_trayIconAdded)
    {
        return;
    }

    InitTrayData(&nid, NIF_INFO | (realtime ? NIF_REALTIME : 0));
    nid.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON | NIIF_RESPECT_QUIET_TIME;
    nid.hBalloonIcon = g_hiconLarge;
    CopyText(nid.szInfoTitle, sizeof(nid.szInfoTitle) / sizeof(nid.szInfoTitle[0]), title);
    CopyText(nid.szInfo, sizeof(nid.szInfo) / sizeof(nid.szInfo[0]), info);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// 生成状态文本，例如 "还剩 1 小时 23 分钟（至 14:30）"。
static void FormatStatus(wchar_t *out, size_t capacity)
{
    wchar_t remaining[48];
    wchar_t endTime[32];
    ULONGLONG remainingMs = GetRemainingMs();

    if (remainingMs == 0)
    {
        CopyText(out, capacity, L"防休眠已结束");
        return;
    }

    FormatRemainingMinutes(remaining, 48, remainingMs);
    FormatEndTime(endTime, 32);
    swprintf(out, capacity, L"还剩 %ls（至 %ls）", remaining, endTime);
}

static void FormatTrayTip(wchar_t *out, size_t capacity)
{
    wchar_t status[96];

    FormatStatus(status, 96);
    swprintf(out, capacity, L"NoSleep: %ls", status);
}

static void UpdateTrayTip(void)
{
    NOTIFYICONDATAW nid;

    if (!g_trayIconAdded)
    {
        return;
    }

    InitTrayData(&nid, NIF_TIP);
    FormatTrayTip(nid.szTip, sizeof(nid.szTip) / sizeof(nid.szTip[0]));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// 托盘提示只显示到分钟，因此把下一次刷新安排在剩余分钟数变化的那一刻：
// 提示始终准确，后台每分钟只唤醒一次。
static BOOL ScheduleCountdownTimer(HWND hwnd)
{
    ULONGLONG remainingMs = GetRemainingMs();
    UINT interval;

    if (remainingMs == 0)
    {
        return FALSE;
    }

    interval = (UINT)((remainingMs - 1) % MINUTE_MILLISECONDS + 1);
    if (interval < USER_TIMER_MINIMUM)
    {
        interval = USER_TIMER_MINIMUM;
    }

    return SetTimer(hwnd, ID_COUNTDOWN_TIMER, interval, NULL) != 0;
}

// 添加托盘图标并切换到 NOTIFYICON_VERSION_4（支持键盘打开菜单）。
static BOOL AddTrayIcon(void)
{
    NOTIFYICONDATAW nid;

    if (g_trayIconAdded)
    {
        return TRUE;
    }

    InitTrayData(&nid, NIF_ICON | NIF_MESSAGE | NIF_TIP);
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = g_hiconSmall;
    FormatTrayTip(nid.szTip, sizeof(nid.szTip) / sizeof(nid.szTip[0]));

    // Explorer 繁忙时 NIM_ADD 可能超时返回失败但图标实际已添加，此时 NIM_MODIFY 能成功。
    if (!Shell_NotifyIconW(NIM_ADD, &nid) && !Shell_NotifyIconW(NIM_MODIFY, &nid))
    {
        return FALSE;
    }

    g_trayIconAdded = TRUE;
    nid.uVersion = NOTIFYICON_VERSION_4;
    g_trayVersion4 = Shell_NotifyIconW(NIM_SETVERSION, &nid);
    return TRUE;
}

// 移除托盘图标
static void RemoveTrayIcon(void)
{
    NOTIFYICONDATAW nid;

    if (g_trayIconAdded)
    {
        InitTrayData(&nid, 0);
        Shell_NotifyIconW(NIM_DELETE, &nid);
        g_trayIconAdded = FALSE;
    }
}

// 启动通知需要托盘图标；若图标稍后才添加成功（如开机自启时），届时再显示。
static void ShowStartBalloon(void)
{
    wchar_t endTime[32];
    wchar_t info[128];

    if (!g_startBalloonPending || !g_trayIconAdded)
    {
        return;
    }

    g_startBalloonPending = FALSE;
    FormatEndTime(endTime, 32);
    swprintf(info, 128, L"将保持唤醒 %d 分钟（至 %ls）", g_minutes, endTime);
    ShowBalloon(L"NoSleep", info, TRUE);
}

static void ShowRemainingBalloon(void)
{
    wchar_t remaining[48];
    wchar_t endTime[32];
    wchar_t info[128];
    ULONGLONG remainingMs = GetRemainingMs();

    if (remainingMs > 0)
    {
        FormatRemainingSeconds(remaining, 48, remainingMs);
        FormatEndTime(endTime, 32);
        swprintf(info, 128, L"还剩 %ls，预计 %ls 结束", remaining, endTime);
        ShowBalloon(L"剩余时间", info, TRUE);
    }
}

// 以指定时长（重新）开始计时并申请保持唤醒。
static BOOL StartCountdown(HWND hwnd, int minutes)
{
    g_minutes = minutes;
    g_deadlineMs = GetTickCount64() + (ULONGLONG)minutes * MINUTE_MILLISECONDS;
    KillTimer(hwnd, ID_EXIT_TIMER);
    return EnableKeepAwake() && ScheduleCountdownTimer(hwnd);
}

static void HandleCountdownTimer(HWND hwnd)
{
    if (GetRemainingMs() == 0)
    {
        KillTimer(hwnd, ID_COUNTDOWN_TIMER);
        // 时间到立即恢复系统正常睡眠，不必等到 2 秒后退出。
        DisableKeepAwake();
        UpdateTrayTip();
        ShowBalloon(L"已完成", L"防休眠时间已结束", FALSE);
        if (SetTimer(hwnd, ID_EXIT_TIMER, 2000, NULL) == 0)
        {
            DestroyWindow(hwnd);
        }
        return;
    }

    UpdateTrayTip();
    if (!ScheduleCountdownTimer(hwnd))
    {
        ShowError(hwnd, L"无法继续运行倒计时。");
        DestroyWindow(hwnd);
    }
}

// 重新设定一个新的时长（托盘菜单、设置窗口或第二个实例转交）。
static void ApplyNewDuration(HWND hwnd, int minutes)
{
    wchar_t endTime[32];
    wchar_t info[96];

    if (!StartCountdown(hwnd, minutes))
    {
        ShowError(hwnd, L"无法重新开始计时。");
        DestroyWindow(hwnd);
        return;
    }

    UpdateTrayTip();
    FormatEndTime(endTime, 32);
    swprintf(info, 96, L"已设置为 %d 分钟（至 %ls）", minutes, endTime);
    ShowBalloon(L"NoSleep", info, TRUE);
}

static void SetKeepDisplayOn(HWND hwnd, BOOL keepDisplayOn)
{
    if (g_keepDisplayOn == keepDisplayOn)
    {
        return;
    }

    g_keepDisplayOn = keepDisplayOn;
    if (GetRemainingMs() > 0 && !EnableKeepAwake())
    {
        ShowError(hwnd, L"无法更新防休眠设置。");
        DestroyWindow(hwnd);
    }
}

static void ShowTrayMenu(HWND hwnd, POINT pt)
{
    HMENU menu = CreatePopupMenu();
    HMENU durationMenu = CreatePopupMenu();
    wchar_t status[96];
    UINT flags = TPM_RIGHTBUTTON;
    int i;

    if (!menu || !durationMenu)
    {
        if (menu)
        {
            DestroyMenu(menu);
        }
        if (durationMenu)
        {
            DestroyMenu(durationMenu);
        }
        return;
    }

    for (i = 0; i < PRESET_COUNT; i++)
    {
        AppendMenuW(durationMenu, MF_STRING, (UINT_PTR)(ID_SETDUR_FIRST + i), g_presetButtons[i].text);
    }
    AppendMenuW(durationMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(durationMenu, MF_STRING, ID_SETDUR_CUSTOM, L"自定义...");

    FormatStatus(status, 96);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, ID_MENU_STATUS, status);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)durationMenu, L"重新设置时长");
    AppendMenuW(menu, MF_STRING | (g_keepDisplayOn ? MF_CHECKED : MF_UNCHECKED),
        ID_TOGGLE_DISPLAY, L"保持屏幕常亮");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"退出");

    flags |= GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, flags, pt.x, pt.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

// 处理第二个实例发来的 WM_COPYDATA。数据来自其他进程，逐项校验后再投递到
// 自身消息队列处理，让发送方的 SendMessage 立即返回。
static BOOL HandleInstanceRequest(HWND hwnd, const COPYDATASTRUCT *data)
{
    InstanceRequest request;

    if (!data || data->dwData != INSTANCE_REQUEST_MAGIC
        || data->cbData != sizeof(request) || !data->lpData)
    {
        return FALSE;
    }

    memcpy(&request, data->lpData, sizeof(request));
    if (request.minutes < 0 || request.minutes > MAX_MINUTES
        || request.keepDisplayOn < -1 || request.keepDisplayOn > 1)
    {
        return FALSE;
    }

    return PostMessageW(hwnd, WM_INSTANCE_REQUEST,
        (WPARAM)request.minutes, (LPARAM)request.keepDisplayOn);
}

// 托盘窗口过程
static LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (g_taskbarCreatedMessage != 0 && msg == g_taskbarCreatedMessage)
    {
        g_trayIconAdded = FALSE;
        if (AddTrayIcon())
        {
            KillTimer(hwnd, ID_TRAY_RETRY_TIMER);
            ShowStartBalloon();
        }
        return 0;
    }

    switch (msg)
    {
        case WM_TRAYICON:
        {
            UINT event = g_trayVersion4 ? LOWORD(lParam) : (UINT)lParam;

            // VERSION_4 下右键和键盘（菜单键 / Shift+F10）都会发送 WM_CONTEXTMENU，
            // wParam 中带有菜单锚点坐标。
            if (event == WM_CONTEXTMENU || (!g_trayVersion4 && event == WM_RBUTTONUP))
            {
                POINT pt;
                if (g_trayVersion4)
                {
                    pt.x = GET_X_LPARAM(wParam);
                    pt.y = GET_Y_LPARAM(wParam);
                }
                else
                {
                    GetCursorPos(&pt);
                }
                ShowTrayMenu(hwnd, pt);
            }
            else if (event == WM_LBUTTONDBLCLK || event == NIN_KEYSELECT)
            {
                // 双击或用键盘选中图标时显示剩余时间
                ShowRemainingBalloon();
            }
            break;
        }

        case WM_COMMAND:
        {
            int cmd = LOWORD(wParam);

            if (cmd == ID_EXIT)
            {
                DestroyWindow(hwnd);
            }
            else if (cmd >= ID_SETDUR_FIRST && cmd < ID_SETDUR_FIRST + PRESET_COUNT)
            {
                ApplyNewDuration(hwnd, g_presetButtons[cmd - ID_SETDUR_FIRST].minutes);
            }
            else if (cmd == ID_SETDUR_CUSTOM)
            {
                ShowInputWindow(TRUE);
            }
            else if (cmd == ID_TOGGLE_DISPLAY)
            {
                SetKeepDisplayOn(hwnd, !g_keepDisplayOn);
            }
            break;
        }

        case WM_COPYDATA:
            return HandleInstanceRequest(hwnd, (const COPYDATASTRUCT *)lParam);

        case WM_INSTANCE_REQUEST:
        {
            int minutes = (int)wParam;
            int keepDisplayOn = (int)lParam;

            if (minutes > 0)
            {
                if (keepDisplayOn >= 0)
                {
                    g_keepDisplayOn = keepDisplayOn != 0;
                }
                ApplyNewDuration(hwnd, minutes);
            }
            else
            {
                if (keepDisplayOn >= 0)
                {
                    SetKeepDisplayOn(hwnd, keepDisplayOn != 0);
                }
                ShowInputWindow(TRUE);
            }
            break;
        }

        case WM_TIMER:
            if (wParam == ID_COUNTDOWN_TIMER)
            {
                HandleCountdownTimer(hwnd);
            }
            else if (wParam == ID_EXIT_TIMER)
            {
                // 用户正在设置窗口里输入新时长时先不退出，定时器继续每 2 秒检查一次。
                if (g_inputHwnd)
                {
                    break;
                }
                KillTimer(hwnd, ID_EXIT_TIMER);
                if (GetRemainingMs() == 0)
                {
                    DestroyWindow(hwnd);
                }
            }
            else if (wParam == ID_INITIAL_BALLOON_TIMER)
            {
                KillTimer(hwnd, ID_INITIAL_BALLOON_TIMER);
                ShowStartBalloon();
            }
            else if (wParam == ID_TRAY_RETRY_TIMER)
            {
                // 开机自启时任务栏可能尚未就绪：定时重试添加托盘图标，
                // 超过次数后交给 TaskbarCreated 广播处理。
                if (AddTrayIcon())
                {
                    KillTimer(hwnd, ID_TRAY_RETRY_TIMER);
                    ShowStartBalloon();
                }
                else if (++g_trayRetryCount >= TRAY_RETRY_LIMIT)
                {
                    KillTimer(hwnd, ID_TRAY_RETRY_TIMER);
                }
            }
            break;

        case WM_POWERBROADCAST:
            // 系统即将进入睡眠或休眠（手动睡眠、合盖等都会触发）：
            // 解除防休眠并退出，把系统交还给正常的睡眠流程。
            // 若某些设备（如部分 Modern Standby 机型）没有投递挂起通知，
            // 则在随后的恢复通知中同样退出，保持"睡眠即退出"的行为一致。
            if (wParam == PBT_APMSUSPEND || wParam == PBT_APMRESUMESUSPEND
                || wParam == PBT_APMRESUMEAUTOMATIC)
            {
                DisableKeepAwake();
                DestroyWindow(hwnd);
            }
            return TRUE;

        case WM_DESTROY:
            if (g_inputHwnd)
            {
                DestroyWindow(g_inputHwnd);
            }
            KillTimer(hwnd, ID_COUNTDOWN_TIMER);
            KillTimer(hwnd, ID_EXIT_TIMER);
            KillTimer(hwnd, ID_INITIAL_BALLOON_TIMER);
            KillTimer(hwnd, ID_TRAY_RETRY_TIMER);
            RemoveTrayIcon();
            DisableKeepAwake();
            g_hwnd = NULL;
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

static void DeleteFonts(HFONT *fonts)
{
    int i;

    for (i = 0; i < FONT_COUNT; i++)
    {
        if (fonts[i])
        {
            DeleteObject(fonts[i]);
            fonts[i] = NULL;
        }
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
        CopyText(font.lfFaceName, LF_FACESIZE, L"Microsoft YaHei UI");
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

static void SetInputControlFont(HWND hwnd, int id, int font)
{
    HWND control = GetDlgItem(hwnd, id);
    HFONT handle = g_inputFonts[font]
        ? g_inputFonts[font]
        : (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    if (control)
    {
        SendMessageW(control, WM_SETFONT, (WPARAM)handle, FALSE);
    }
}

static void ApplyInputFonts(HWND hwnd)
{
    int i;

    SetInputControlFont(hwnd, ID_LABEL_TITLE, FONT_TITLE);
    SetInputControlFont(hwnd, ID_LABEL_PRESETS, FONT_SECTION);
    SetInputControlFont(hwnd, ID_LABEL_CUSTOM, FONT_SECTION);
    SetInputControlFont(hwnd, ID_LABEL_MINUTES, FONT_BODY);
    SetInputControlFont(hwnd, ID_EDIT_MINUTES, FONT_BODY);
    SetInputControlFont(hwnd, ID_START_CUSTOM, FONT_BODY);
    SetInputControlFont(hwnd, ID_CHECK_DISPLAY, FONT_BODY);

    for (i = 0; i < PRESET_COUNT; i++)
    {
        SetInputControlFont(hwnd, g_presetButtons[i].id, FONT_BODY);
    }
}

static void RecreateInputFonts(HWND hwnd, UINT dpi)
{
    HFONT oldFonts[FONT_COUNT];

    memcpy(oldFonts, g_inputFonts, sizeof(oldFonts));
    g_inputFonts[FONT_TITLE] = CreateInputFont(dpi, 15, 600);
    g_inputFonts[FONT_SECTION] = CreateInputFont(dpi, 9, 600);
    g_inputFonts[FONT_BODY] = CreateInputFont(dpi, 10, FW_NORMAL);
    ApplyInputFonts(hwnd);

    // 先让控件换用新字体，再删除旧字体，避免控件持有已删除的字体句柄。
    DeleteFonts(oldFonts);
}

static void UpdateHighContrast(void)
{
    HIGHCONTRASTW highContrast = {0};

    highContrast.cbSize = sizeof(highContrast);
    g_highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(highContrast), &highContrast, 0)
        && (highContrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
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
    int i;

    MoveInputControl(hwnd, ID_LABEL_TITLE, 24, 20, 312, 28, dpi);
    MoveInputControl(hwnd, ID_LABEL_PRESETS, 24, 61, 120, 18, dpi);

    for (i = 0; i < PRESET_COUNT; i++)
    {
        int column = i % 3;
        int row = i / 3;
        MoveInputControl(hwnd, g_presetButtons[i].id,
            24 + column * 108, 84 + row * 47, 96, 40, dpi);
    }

    MoveInputControl(hwnd, ID_LABEL_CUSTOM, 24, 195, 52, 36, dpi);
    MoveInputControl(hwnd, ID_EDIT_MINUTES, 84, 197, 80, 30, dpi);
    MoveInputControl(hwnd, ID_LABEL_MINUTES, 172, 195, 44, 36, dpi);
    MoveInputControl(hwnd, ID_START_CUSTOM, 250, 193, 86, 38, dpi);
    MoveInputControl(hwnd, ID_CHECK_DISPLAY, 24, 247, 312, 24, dpi);
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
    POINT origin;
    int width;
    int height;

    if (g_pAdjustWindowRectExForDpi)
    {
        g_pAdjustWindowRectExForDpi(&rect, style, FALSE, exStyle, dpi);
    }
    else
    {
        AdjustWindowRectEx(&rect, style, FALSE, exStyle);
    }

    width = rect.right - rect.left;
    height = rect.bottom - rect.top;
    origin = CenterInMonitor(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), width, height);
    SetWindowPos(hwnd, HWND_TOPMOST, origin.x, origin.y, width, height, SWP_NOACTIVATE);
}

static BOOL CreateInputControls(HWND hwnd)
{
    HINSTANCE instance = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
    HWND edit;
    int i;

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

    for (i = 0; i < PRESET_COUNT; i++)
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

    if (!CreateWindowExW(0, L"BUTTON", L"保持屏幕常亮（不勾选则允许关闭显示器）",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTOCHECKBOX,
            0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_CHECK_DISPLAY, instance, NULL))
    {
        return FALSE;
    }

    return TRUE;
}

// 用户选定时长：启动阶段记录下来交给 wWinMain，从托盘打开时直接重设正在运行的计时。
static void ChooseMinutes(HWND hwnd, int minutes)
{
    BOOL keepDisplayOn = IsDlgButtonChecked(hwnd, ID_CHECK_DISPLAY) == BST_CHECKED;
    BOOL resetMode = g_inputResetMode;

    DestroyWindow(hwnd);
    g_keepDisplayOn = keepDisplayOn;
    if (resetMode && g_hwnd)
    {
        ApplyNewDuration(g_hwnd, minutes);
    }
    else
    {
        g_minutes = minutes;
    }
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
        ChooseMinutes(hwnd, minutes);
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
            UpdateHighContrast();
            RecreateInputFonts(hwnd, dpi);
            LayoutInputControls(hwnd, dpi);
            return 0;
        }

        case WM_COMMAND:
        {
            int command = LOWORD(wParam);
            int i;

            for (i = 0; i < PRESET_COUNT; i++)
            {
                if (command == g_presetButtons[i].id)
                {
                    ChooseMinutes(hwnd, g_presetButtons[i].minutes);
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
            if (wParam == SPI_SETHIGHCONTRAST)
            {
                UpdateHighContrast();
                RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
            }
            // 仅在界面字体/非客户区度量相关的设置变化时才重建字体，
            // 避免每次系统设置广播都无谓地重建 GDI 字体对象。
            else if (wParam == SPI_SETNONCLIENTMETRICS ||
                (lParam && wcscmp((const wchar_t *)lParam, L"WindowMetrics") == 0))
            {
                RecreateInputFonts(hwnd, GetWindowDpiCompat(hwnd));
                InvalidateRect(hwnd, NULL, TRUE);
            }
            return 0;

        case WM_SYSCOLORCHANGE:
        case WM_THEMECHANGED:
            UpdateHighContrast();
            RedrawWindow(hwnd, NULL, NULL,
                RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
            return 0;

        case WM_CTLCOLORSTATIC:
        {
            HDC hdc = (HDC)wParam;
            HWND control = (HWND)lParam;
            int id = GetDlgCtrlID(control);
            COLORREF color;

            // 高对比度主题下背景可能是黑色，必须使用系统文字颜色，否则文字不可见。
            if (g_highContrast)
            {
                color = GetSysColor(COLOR_WINDOWTEXT);
            }
            else
            {
                color = (id == ID_LABEL_PRESETS || id == ID_LABEL_CUSTOM)
                    ? RGB(82, 88, 98)
                    : RGB(28, 31, 36);
            }
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, color);
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }

        case WM_DESTROY:
            DeleteFonts(g_inputFonts);
            g_inputHwnd = NULL;
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// 打开输入窗口；已打开时只把它带到前台。启动阶段和托盘"自定义..."共用这个窗口，
// 由主消息循环通过 IsDialogMessage 处理 Tab / Enter / Esc。
static BOOL ShowInputWindow(BOOL resetMode)
{
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSEXW wc = {0};
    POINT cursor = {0, 0};
    POINT origin;
    HWND hwnd;
    HWND edit;

    if (g_inputHwnd)
    {
        ShowWindow(g_inputHwnd, SW_SHOWNORMAL);
        SetForegroundWindow(g_inputHwnd);
        return TRUE;
    }

    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = InputWndProc;
    wc.hInstance = instance;
    wc.lpszClassName = INPUT_CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon = g_hiconLarge;
    wc.hIconSm = g_hiconSmall;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        ShowError(NULL, L"无法创建输入窗口。");
        return FALSE;
    }

    GetCursorPos(&cursor);
    origin = CenterInMonitor(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY),
        INPUT_CLIENT_WIDTH, INPUT_CLIENT_HEIGHT);

    g_inputResetMode = resetMode;
    hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST | WS_EX_CONTROLPARENT,
        INPUT_CLASS_NAME, resetMode ? L"NoSleep 重新设置时长" : L"NoSleep 防休眠",
        WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
        origin.x, origin.y, INPUT_CLIENT_WIDTH, INPUT_CLIENT_HEIGHT,
        NULL, NULL, instance, NULL);

    if (!hwnd)
    {
        ShowError(NULL, L"无法创建输入窗口。");
        return FALSE;
    }

    g_inputHwnd = hwnd;
    if (resetMode && g_minutes > 0)
    {
        SetDlgItemInt(hwnd, ID_EDIT_MINUTES, (UINT)g_minutes, FALSE);
    }
    CheckDlgButton(hwnd, ID_CHECK_DISPLAY, g_keepDisplayOn ? BST_CHECKED : BST_UNCHECKED);

    SizeAndCenterInputWindow(hwnd, GetWindowDpiCompat(hwnd));
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    edit = GetDlgItem(hwnd, ID_EDIT_MINUTES);
    SetFocus(edit);
    SendMessageW(edit, EM_SETSEL, 0, -1);
    return TRUE;
}

// 运行消息循环。untilInputClosed 为 TRUE 时（启动阶段）在输入窗口关闭后返回 1；
// 否则一直运行到 WM_QUIT（返回 0）或 GetMessage 出错（返回 -1）。
static int RunMessageLoop(BOOL untilInputClosed)
{
    MSG msg;

    while (!untilInputClosed || g_inputHwnd)
    {
        int result = GetMessageW(&msg, NULL, 0, 0);
        if (result <= 0)
        {
            return result;
        }
        if (g_inputHwnd && IsDialogMessageW(g_inputHwnd, &msg))
        {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 1;
}

// 已有实例在运行时，把本次的命令行转交给它：带时长则直接重设，否则让它打开设置窗口。
static BOOL ForwardToRunningInstance(int minutes, int keepDisplayOn)
{
    HWND target = FindWindowW(TRAY_CLASS_NAME, NULL);
    InstanceRequest request;
    COPYDATASTRUCT data;
    DWORD processId = 0;
    DWORD_PTR result = 0;

    if (!target)
    {
        // 已运行的实例可能还停留在启动时的输入窗口，直接把它带到前台。
        HWND input = FindWindowW(INPUT_CLASS_NAME, NULL);
        if (!input)
        {
            return FALSE;
        }
        ShowWindow(input, SW_SHOWNORMAL);
        SetForegroundWindow(input);
        return TRUE;
    }

    request.minutes = minutes;
    request.keepDisplayOn = keepDisplayOn;
    data.dwData = INSTANCE_REQUEST_MAGIC;
    data.cbData = sizeof(request);
    data.lpData = &request;

    // 本进程由用户刚刚启动，拥有前台权限；转让给已运行实例，使其设置窗口能显示在最前面。
    GetWindowThreadProcessId(target, &processId);
    AllowSetForegroundWindow(processId);
    return SendMessageTimeoutW(target, WM_COPYDATA, 0, (LPARAM)&data,
            SMTO_ABORTIFHUNG, 2000, &result) && result;
}

// 创建托盘窗口并开始计时；成功返回 NULL，失败返回错误提示。
static const wchar_t *StartTrayMode(HINSTANCE hinst)
{
    WNDCLASSW wc = {0};

    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = hinst;
    wc.lpszClassName = TRAY_CLASS_NAME;
    if (!RegisterClassW(&wc))
    {
        return L"无法注册托盘窗口。";
    }

    g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    if (g_taskbarCreatedMessage == 0)
    {
        return L"无法注册托盘恢复消息。";
    }

    // 使用隐藏顶层窗口，以便接收 Explorer 重启和电源广播。
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, TRAY_CLASS_NAME, L"", 0, 0, 0, 0, 0, NULL, NULL, hinst, NULL);
    if (!g_hwnd)
    {
        return L"无法创建托盘窗口。";
    }

    // 以管理员身份运行时，允许接收普通权限 Explorer 发出的 TaskbarCreated 广播。
    ChangeWindowMessageFilterEx(g_hwnd, g_taskbarCreatedMessage, MSGFLT_ALLOW, NULL);

    if (!StartCountdown(g_hwnd, g_minutes))
    {
        return L"系统拒绝了防休眠请求，或无法启动倒计时。";
    }

    // 开机自启时任务栏可能尚未就绪：添加失败不退出，改为定时重试并等待 TaskbarCreated。
    if (!AddTrayIcon())
    {
        g_trayRetryCount = 0;
        SetTimer(g_hwnd, ID_TRAY_RETRY_TIMER, TRAY_RETRY_MILLISECONDS, NULL);
    }

    // 通过定时器延迟通知，避免阻塞主消息循环。
    g_startBalloonPending = TRUE;
    if (SetTimer(g_hwnd, ID_INITIAL_BALLOON_TIMER, 500, NULL) == 0)
    {
        ShowStartBalloon();
    }
    return NULL;
}

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE hprev, LPWSTR szcmd, int ncmd)
{
    int exitCode = 0;
    int minutes = 0;
    BOOL allowDisplayOff = FALSE;
    BOOL argsValid;
    const wchar_t *error;
    INITCOMMONCONTROLSEX controls = {0};
    (void)hprev;
    (void)szcmd;
    (void)ncmd;

    EnableBestDpiAwareness();
    InitDpiFunctions();
    argsValid = ParseCommandLine(&minutes, &allowDisplayOff);
    if (!argsValid)
    {
        minutes = 0;
    }

    // 检查是否已有实例运行
    g_hMutex = CreateMutexW(NULL, TRUE, L"Local\\NoSleep_SingleInstance_Mutex");
    if (!g_hMutex)
    {
        ShowError(NULL, L"无法创建单实例锁。");
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        // 命令行给出时长时完整地描述了期望的状态，未带开关即表示保持屏幕常亮。
        int keepDisplayOn = allowDisplayOff ? 0 : (minutes > 0 ? 1 : -1);
        if (!ForwardToRunningInstance(minutes, keepDisplayOn))
        {
            MessageBoxW(NULL, L"NoSleep 已经在运行中！\n\n请勿同时运行多个实例。", L"提示", MB_OK | MB_ICONINFORMATION);
        }
        CloseHandle(g_hMutex);
        return 0;
    }

    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    g_hiconSmall = LoadAppIcon(TRUE);
    g_hiconLarge = LoadAppIcon(FALSE);
    g_keepDisplayOn = !allowDisplayOff;

    if (minutes > 0)
    {
        g_minutes = minutes;
    }
    else
    {
        if (!argsValid)
        {
            // 命令行参数无效时，回退到输入窗口而不是直接退出。
            MessageBoxW(NULL, L"命令行参数无效，请重新选择时长。", L"输入有误", MB_OK | MB_ICONWARNING);
        }
        if (ShowInputWindow(FALSE))
        {
            RunMessageLoop(TRUE);
        }
    }

    if (g_minutes > 0)
    {
        error = StartTrayMode(hinst);
        if (error)
        {
            ShowError(NULL, error);
            if (g_hwnd)
            {
                DestroyWindow(g_hwnd);
            }
            exitCode = 1;
        }
        else if (RunMessageLoop(FALSE) < 0)
        {
            exitCode = 1;
        }
    }

    if (g_hiconSmall)
    {
        DestroyIcon(g_hiconSmall);
    }
    if (g_hiconLarge)
    {
        DestroyIcon(g_hiconLarge);
    }

    // 释放互斥量
    ReleaseMutex(g_hMutex);
    CloseHandle(g_hMutex);
    return exitCode;
}
