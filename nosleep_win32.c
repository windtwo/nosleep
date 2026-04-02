// NoSleep - 纯 Win32 API 版本
// 编译：gcc -static -o nosleep_pure.exe nosleep_win32.c nosleep.res -mwindows -municode -lcomctl32

#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>

#define IDI_APP_ICON 1

#define ID_TRAY 1
#define ID_TIMER 1
#define ID_EXIT 1001
#define WM_TRAYICON (WM_USER + 1)

static int g_minutes = 0;
static int g_seconds = 0;
static HWND g_hwnd = NULL;
static HICON g_hicon = NULL;
static NOTIFYICONDATAW g_nid = {0};

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
static void UpdateTrayTip(void)
{
    wchar_t tip[128];
    swprintf(tip, 128, L"NoSleep: %dm %02ds", g_seconds / 60, g_seconds % 60);

    NOTIFYICONDATAW nid = {0};
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = g_hwnd;
    nid.uID = ID_TRAY;
    nid.uFlags = NIF_TIP;
    wcsncpy(nid.szTip, tip, 127);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// 添加托盘图标
static void AddTrayIcon(void)
{
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = ID_TRAY;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = g_hicon;
    wcsncpy(g_nid.szTip, L"NoSleep", 63);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

// 移除托盘图标
static void RemoveTrayIcon(void)
{
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

// 托盘窗口过程
static LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_TRAYICON:
            if (lParam == WM_RBUTTONUP)
            {
                HMENU hmenu = CreatePopupMenu();
                AppendMenuW(hmenu, MF_STRING, ID_EXIT, L"Exit");
                POINT pt;
                GetCursorPos(&pt);
                SetForegroundWindow(hwnd);
                TrackPopupMenu(hmenu, 0, pt.x, pt.y, 0, hwnd, NULL);
                DestroyMenu(hmenu);
            }
            else if (lParam == WM_LBUTTONDBLCLK)
            {
                // 双击显示剩余时间
                if (g_seconds > 0)
                {
                    wchar_t info[64];
                    swprintf(info, 64, L"%dm %02ds remaining", g_seconds / 60, g_seconds % 60);
                    ShowBalloon(L"Time Remaining", info);
                }
            }
            break;

        case WM_COMMAND:
            if (LOWORD(wParam) == ID_EXIT)
            {
                DestroyWindow(hwnd);
            }
            break;

        case WM_TIMER:
            if (wParam == ID_TIMER && g_seconds > 0)
            {
                g_seconds--;
                if (g_seconds == 0)
                {
                    KillTimer(hwnd, ID_TIMER);
                    ShowBalloon(L"Done", L"Time is up!");
                    SetTimer(hwnd, 2, 2000, NULL);
                }
                else
                {
                    UpdateTrayTip();
                }
            }
            else if (wParam == 2)
            {
                // 2 秒后退出
                KillTimer(hwnd, 2);
                DestroyWindow(hwnd);
            }
            break;

        case WM_CLOSE:
            RemoveTrayIcon();
            PostQuitMessage(0);
            break;

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// 输入框窗口过程
static LRESULT CALLBACK InputWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    static HWND hedit = NULL;

    switch (msg)
    {
        case WM_CREATE:
        {
            CreateWindowW(L"STATIC", L"Enter minutes to keep awake:",
                WS_CHILD | WS_VISIBLE | SS_CENTER,
                20, 20, 210, 25, hwnd, NULL, GetModuleHandle(NULL), NULL);

            hedit = CreateWindowW(L"EDIT", L"30",
                WS_CHILD | WS_VISIBLE | WS_BORDER | ES_CENTER,
                45, 55, 160, 35, hwnd, (HMENU)101, GetModuleHandle(NULL), NULL);

            CreateWindowW(L"BUTTON", L"OK",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                75, 105, 100, 35, hwnd, (HMENU)102, GetModuleHandle(NULL), NULL);

            SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)g_hicon);
            SetFocus(hedit);
            return 0;
        }

        case WM_COMMAND:
            if (LOWORD(wParam) == 102)
            {
                wchar_t buf[16] = {0};
                GetWindowTextW(hedit, buf, 16);
                g_minutes = _wtoi(buf);
                if (g_minutes > 0 && g_minutes <= 1440)
                {
                    DestroyWindow(hwnd);
                }
                else
                {
                    MessageBoxW(hwnd, L"Please enter 1-1440", L"Error", MB_OK | MB_ICONWARNING);
                }
            }
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
    wc.lpfnWndProc = InputWndProc;
    wc.hInstance = hinst;
    wc.lpszClassName = L"NoSleepInput";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        L"NoSleepInput", L"NoSleep",
        WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        (sw - 250) / 2, (sh - 180) / 2, 250, 180,
        NULL, NULL, hinst, NULL);

    if (!hwnd) return 0;

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        if (g_minutes != 0) break;
    }

    return g_minutes;
}

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE hprev, LPWSTR szcmd, int ncmd)
{
    (void)hprev;
    (void)szcmd;
    (void)ncmd;

    InitCommonControls();

    g_hicon = LoadAppIcon();

    // 检查命令行参数
    int argc;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc > 1)
    {
        g_minutes = _wtoi(argv[1]);
        LocalFree(argv);
    }
    else
    {
        g_minutes = ShowInputDialog(hinst);
    }

    if (g_minutes <= 0 || g_minutes > 1440)
    {
        return 0;
    }

    // 创建托盘窗口
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = hinst;
    wc.lpszClassName = L"NoSleepTray";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon = g_hicon;
    RegisterClassW(&wc);

    g_hwnd = CreateWindowExW(0, L"NoSleepTray", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hinst, NULL);
    if (!g_hwnd) return 1;

    AddTrayIcon();

    // 阻止睡眠
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);

    // 启动倒计时
    g_seconds = g_minutes * 60;
    SetTimer(g_hwnd, ID_TIMER, 1000, NULL);

    // 延时显示初始通知，确保托盘图标已准备好
    Sleep(500);
    wchar_t infomsg[256];
    swprintf(infomsg, 256, L"Keeping awake for %d minutes", g_minutes);
    ShowBalloon(L"NoSleep", infomsg);

    // 消息循环
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}
