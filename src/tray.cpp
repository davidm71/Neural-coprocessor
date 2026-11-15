// ---------------------------------------------------------------------------
// tray.cpp - TR1. See tray.hpp.
// ---------------------------------------------------------------------------
#include "tray.hpp"
#include "gpu1_context.hpp"
#include "diag.hpp"
#include "worker.hpp"

#include <shellapi.h>
#include <process.h>
#include <atomic>
#include <cstdio>
#include <initializer_list>
#include <cstring>
#include <cwchar>

#pragma comment(lib, "shell32.lib")

namespace mgpu
{
HMODULE module_handle();   // dllmain.cpp
const char *about_version();   // dllmain.cpp (TR3)
const char *about_build();

namespace tray
{
namespace
{
const UINT kCallback = WM_APP + 0x4D;   // the icon's messages to the bridge window
const UINT kId       = 1;

HWND  g_owner = nullptr;
bool  g_added = false;
int   g_cfg   = -1;
int   g_shown_state = -1;
wchar_t g_shown_tip[128] = {};
char  g_last_error[200] = {};
HICON g_icon[4] = {};
UINT  g_taskbar_created = 0;
char  g_status[200] = {};
int   g_state = 0;

enum { MENU_STATUS = 1, MENU_PICTURE, MENU_ARM, MENU_LOG, MENU_DETAILS, MENU_ABOUT, MENU_EXIT };

void to_wide(const char *s, wchar_t *w, int n)
{
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) == 0) w[0] = 0;
    w[n - 1] = 0;
}

// TR3: Status and About are message boxes on a thread of their own - a modal
// box on the bridge thread would stop the picture while it is open. One of
// each at a time.
std::atomic<bool> g_box_open[3] = {};   // status, about, exit confirmation
struct box_job { int which; wchar_t title[96]; wchar_t text[1200]; };
unsigned __stdcall box_thread(void *p)
{
    box_job *j = static_cast<box_job *>(p);
    if (j->which == 2)
    {
        // TR4: Exit asks first - it cannot be undone without restarting the game.
        if (MessageBoxW(nullptr, j->text, j->title,
                        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_TOPMOST | MB_SETFOREGROUND) == IDYES)
            mgpu::worker::user_exit();
    }
    else
        MessageBoxW(nullptr, j->text, j->title,
                    MB_OK | (j->which == 0 ? MB_ICONINFORMATION : 0u) | MB_TOPMOST | MB_SETFOREGROUND);
    g_box_open[j->which].store(false);
    delete j;
    return 0;
}
void open_box(int which, const wchar_t *title, const char *text)
{
    if (g_box_open[which].exchange(true)) return;
    box_job *j = new box_job{};
    j->which = which;
    wcsncpy_s(j->title, title, _TRUNCATE);
    to_wide(text, j->text, (int)(sizeof j->text / sizeof j->text[0]));
    uintptr_t h = _beginthreadex(nullptr, 0, box_thread, j, 0, nullptr);
    if (h == 0) { g_box_open[which].store(false); delete j; return; }
    CloseHandle((HANDLE)h);
}
void show_status_box()
{
    char t[1200];
    mgpu::gpu1::tray_details(t, sizeof t);
    open_box(0, L"Atomic MGPU Bridge - status", t);
}
void show_about_box()
{
    char t[900];
    snprintf(t, sizeof t,
             "Atomic MGPU Bridge %s\n"
             "Build %s\n"
             "\n"
             "Renders the game on one graphics card and runs neural rendering, frame "
             "generation and the display on a second one.\n"
             "\n"
             "By davidm71 - a fork of MGPU Bridge by Marcelo Guibout.\n"
             "github.com/davidm71/Neural-coprocessor\n"
             "\n"
             "MIT licence. NVIDIA DLSS files are not included and are not redistributed.",
             mgpu::about_version(), mgpu::about_build());
    open_box(1, L"About Atomic MGPU Bridge", t);
}

bool enabled()
{
    if (g_cfg < 0) g_cfg = mgpu::gpu1::ui_ini_read("TrayIcon", 1) != 0 ? 1 : 0;
    return g_cfg != 0;
}

#include "tray_draw.inc"   // TR2: tr_render

HICON make_icon(unsigned char r, unsigned char g, unsigned char b)
{
    int n = GetSystemMetrics(SM_CXSMICON);
    if (n < 16) n = 16;
    if (n > 64) n = 64;
    BITMAPV5HEADER bh{};
    bh.bV5Size = sizeof bh;
    bh.bV5Width = n;
    bh.bV5Height = -n;   // top-down
    bh.bV5Planes = 1;
    bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS;
    bh.bV5RedMask = 0x00FF0000; bh.bV5GreenMask = 0x0000FF00;
    bh.bV5BlueMask = 0x000000FF; bh.bV5AlphaMask = 0xFF000000;
    void *bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP colour = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO *>(&bh), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (colour == nullptr || bits == nullptr) { if (colour) DeleteObject(colour); return nullptr; }
    tr_render(n, r, g, b, static_cast<unsigned *>(bits));
    HBITMAP mask = CreateBitmap(n, n, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = colour;
    ii.hbmMask = mask;
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(colour);
    if (mask) DeleteObject(mask);
    return ic;
}

void make_icons()
{
    if (g_icon[0] != nullptr) return;
    g_icon[0] = make_icon(150, 150, 150);   // waiting
    g_icon[1] = make_icon(255, 176, 0);     // arming
    g_icon[2] = make_icon(60, 200, 90);     // running
    g_icon[3] = make_icon(230, 50, 50);     // error
}


bool shell(DWORD op, int state, const wchar_t *tip, const char *info)
{
    NOTIFYICONDATAW d{};
    d.cbSize = sizeof d;
    d.hWnd = g_owner;
    d.uID = kId;
    if (op != NIM_DELETE)
    {
        d.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        d.uCallbackMessage = kCallback;
        d.hIcon = g_icon[(state >= 0 && state < 4) ? state : 0];
        wcsncpy_s(d.szTip, tip, _TRUNCATE);
        if (info != nullptr)
        {
            d.uFlags |= NIF_INFO;
            d.dwInfoFlags = NIIF_ERROR;
            wcsncpy_s(d.szInfoTitle, L"Atomic MGPU Bridge", _TRUNCATE);
            to_wide(info, d.szInfo, (int)(sizeof d.szInfo / sizeof d.szInfo[0]));
        }
    }
    return Shell_NotifyIconW(op, &d) != FALSE;
}

void add()
{
    wchar_t tip[128];
    swprintf_s(tip, L"Atomic MGPU Bridge - starting");
    if (shell(NIM_ADD, 0, tip, nullptr))
    {
        g_added = true;
        g_shown_state = 0;
        wcsncpy_s(g_shown_tip, tip, _TRUNCATE);
    }
}

void open_log_folder()
{
    wchar_t p[MAX_PATH * 2];
    const DWORD got = GetModuleFileNameW(mgpu::module_handle(), p, MAX_PATH * 2 - 16);
    if (got == 0 || got >= MAX_PATH * 2 - 16) return;
    wchar_t *cut = wcsrchr(p, L'\\');
    if (cut == nullptr) return;
    wcscpy_s(cut + 1, (size_t)(MAX_PATH * 2 - (cut + 1 - p)), L"ReShade.log");
    if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES)
    {
        wchar_t arg[MAX_PATH * 2 + 16];
        swprintf_s(arg, L"/select,\"%s\"", p);
        ShellExecuteW(nullptr, L"open", L"explorer.exe", arg, nullptr, SW_SHOWNORMAL);
    }
    else
    {
        cut[1] = 0;
        ShellExecuteW(nullptr, L"open", p, nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void show_menu(HWND hwnd)
{
    char status[200];
    const int state = mgpu::gpu1::tray_status(status, sizeof status);
    mgpu::gpu1::ui_state ast;
    mgpu::gpu1::ui_read(ast);
    const bool hidden = mgpu::gpu1::bridge_window_hidden();
    const bool picture = state == 2;

    HMENU m = CreatePopupMenu();
    if (m == nullptr) return;
    wchar_t head[200], ws[180];
    to_wide(status, ws, 180);
    swprintf_s(head, L"Atomic MGPU Bridge: %s", ws);
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, head);
    AppendMenuW(m, MF_STRING, MENU_DETAILS, L"Status...");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    if (!picture)
        AppendMenuW(m, MF_STRING | (mgpu::gpu1::dcomp_overlay_mode() ? MF_GRAYED : 0), MENU_STATUS,
                    hidden ? L"Show status window" : L"Hide status window");
    else
        AppendMenuW(m, MF_STRING, MENU_PICTURE, L"Hide / show the picture\tCTRL+ALT+F6");
    AppendMenuW(m, MF_STRING | (ast.armed ? MF_GRAYED : 0), MENU_ARM, L"Arm now\tCTRL+ALT+F10");
    AppendMenuW(m, MF_STRING, MENU_LOG, L"Open the log folder");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, MENU_ABOUT, L"About Atomic MGPU Bridge...");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, MENU_EXIT, L"Exit (stop the bridge)");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);   // so the menu closes when you click elsewhere
    const UINT cmd = (UINT)TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                          pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);

    switch (cmd)
    {
    case MENU_STATUS:
        mgpu::gpu1::hw_status_window(hidden);
        break;
    case MENU_PICTURE:
        (void)mgpu::gpu1::dcomp_peek_toggle();
        break;
    case MENU_ARM:
        mgpu::diag::info("[MGPU][TR1] Arm now - from the tray icon.");
        mgpu::gpu1::stream_request();
        break;
    case MENU_LOG:
        open_log_folder();
        break;
    case MENU_DETAILS:
        show_status_box();
        break;
    case MENU_ABOUT:
        show_about_box();
        break;
    case MENU_EXIT:
        open_box(2, L"Atomic MGPU Bridge - exit",
                 "Stop the bridge for the rest of this game session?\n\n"
                 "The second card stops, the picture window and this icon close, and the game "
                 "keeps running on its own card (without neural rendering or frame generation).\n\n"
                 "To use the bridge again, restart the game.");
        break;
    default:
        break;
    }
}
} // namespace

void install(HWND owner)
{
    if (owner == nullptr || g_added || !enabled()) return;
    g_owner = owner;
    make_icons();
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    // An elevated game would otherwise never hear from Explorer (UIPI).
    ChangeWindowMessageFilterEx(owner, kCallback, MSGFLT_ALLOW, nullptr);
    if (g_taskbar_created != 0) ChangeWindowMessageFilterEx(owner, g_taskbar_created, MSGFLT_ALLOW, nullptr);
    add();
    mgpu::diag::info(g_added ? "[MGPU][TR1] tray icon added: grey waiting, amber arming, green running, red error. "
                               "Click it for the menu. TrayIcon=0 turns it off."
                             : "[MGPU][TR1] the tray icon could not be added (Shell_NotifyIcon refused) - "
                               "the status is in the log and the panel as before.");
}

void remove()
{
    if (g_added) shell(NIM_DELETE, 0, L"", nullptr);
    g_added = false;
    g_owner = nullptr;
    for (HICON &i : g_icon) if (i != nullptr) { DestroyIcon(i); i = nullptr; }
}

void tick()
{
    if (!g_added) return;
    g_state = mgpu::gpu1::tray_status(g_status, sizeof g_status);
    wchar_t tip[128], ws[112];
    to_wide(g_status, ws, 112);
    swprintf_s(tip, L"Atomic MGPU Bridge - %s", ws);
    const char *info = nullptr;
    if (g_state == 3 && std::strcmp(g_last_error, g_status) != 0)
    {
        strncpy_s(g_last_error, g_status, _TRUNCATE);
        info = g_status;   // a notification, once per error
    }
    if (g_state == g_shown_state && info == nullptr && std::wcscmp(tip, g_shown_tip) == 0) return;
    if (shell(NIM_MODIFY, g_state, tip, info))
    {
        g_shown_state = g_state;
        wcsncpy_s(g_shown_tip, tip, _TRUNCATE);
    }
}

bool on_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT *res)
{
    (void)wp;
    if (!g_added && msg != g_taskbar_created) return false;
    if (msg == kCallback)
    {
        const UINT ev = LOWORD(lp);
        if (ev == WM_RBUTTONUP || ev == WM_LBUTTONUP || ev == WM_CONTEXTMENU)
            show_menu(hwnd);
        *res = 0;
        return true;
    }
    if (g_taskbar_created != 0 && msg == g_taskbar_created && g_owner != nullptr)
    {
        // Explorer restarted: the icon is gone, add it again.
        g_added = false;
        add();
        *res = 0;
        return true;
    }
    return false;
}

} // namespace tray
} // namespace mgpu
