// ---------------------------------------------------------------------------
// cursormap.cpp - MC1. See cursormap.hpp.
// ---------------------------------------------------------------------------
#include "cursormap.hpp"
#include <cstdlib>   // MC6: std::abs
#include "diag.hpp"
#include <cwchar>
#include <intrin.h>
#include <tlhelp32.h>

#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <vector>

namespace mgpu
{
namespace cursormap
{
namespace
{

typedef HCURSOR (WINAPI *pfn_set_cursor)(HCURSOR);
typedef int     (WINAPI *pfn_show_cursor)(BOOL);

std::atomic<HWND>    g_game{nullptr};
std::atomic<bool>    g_installed{false};
std::atomic<bool>    g_map_on{false};      // picture shown: hide the real pointer
std::atomic<bool>    g_panel_up{false};    // PO2: the MGPU panel is up - draw nothing
std::atomic<bool>    g_po_restore{false};  // PO3: put the pointer back before the next clip
std::atomic<long>    g_po_x{0}, g_po_y{0}; // PO3: where it was when the panel opened
std::atomic<unsigned long long> g_po_down_ms{0};   // PO3 (IN87): when the panel went down
std::atomic<HCURSOR> g_want{nullptr};      // the pointer the game asked for
std::atomic<int>     g_count{0};           // the game's ShowCursor display count
bool                 g_have_show = false;
pfn_set_cursor       g_real_set  = nullptr;
pfn_show_cursor      g_real_show = nullptr;
void               **g_slot_set  = nullptr;
void               **g_slot_show = nullptr;
HCURSOR              g_blank     = nullptr;
bool                 g_clipped   = false;  // bridge thread only
RECT                 g_clip{};

std::atomic<unsigned> g_n_set{0}, g_n_set_null{0}, g_n_show{0};
std::atomic<unsigned> g_n_other{0};   // MC7: calls from a module other than the exe
bool game_wants_pointer();           // MC7   // MC4: what the game does with its pointer

HCURSOR WINAPI hk_set_cursor(HCURSOR h)
{
    g_n_set.fetch_add(1, std::memory_order_relaxed);
    {
        HMODULE from = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)_ReturnAddress(), &from) && from != GetModuleHandleW(nullptr))
            g_n_other.fetch_add(1, std::memory_order_relaxed);
    }
    if (h == nullptr) g_n_set_null.fetch_add(1, std::memory_order_relaxed);
    const HCURSOR prev = g_want.exchange(h);
    const HCURSOR actual = (h != nullptr && g_map_on.load(std::memory_order_relaxed) && g_blank != nullptr)
                         ? g_blank : h;
    const HCURSOR r = g_real_set(actual);
    // Answer with the game's own previous pointer, never our blank one, so a
    // game that restores "whatever was there before" restores its own.
    if (prev != nullptr) return prev;
    return (r == g_blank) ? nullptr : r;
}

int WINAPI hk_show_cursor(BOOL show)
{
    g_n_show.fetch_add(1, std::memory_order_relaxed);
    {
        HMODULE from = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)_ReturnAddress(), &from) && from != GetModuleHandleW(nullptr))
            g_n_other.fetch_add(1, std::memory_order_relaxed);
    }
    const int c = g_real_show(show);
    g_count.store(c, std::memory_order_relaxed);
    return c;
}

// Swap one named import of user32.dll in `mod`. Returns the slot, or null.
void **patch_import(HMODULE mod, const char *name, void *repl, void **orig)
{
    BYTE *base = (BYTE *)mod;
    __try
    {
        IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
        const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (dir.VirtualAddress == 0) return nullptr;
        void *by_value = (void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), name);
        for (IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress);
             imp->Name != 0; ++imp)
        {
            if (_stricmp((const char *)(base + imp->Name), "user32.dll") != 0) continue;
            IMAGE_THUNK_DATA *ft = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
            IMAGE_THUNK_DATA *ot = imp->OriginalFirstThunk ? (IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk)
                                                           : nullptr;
            for (; ft->u1.Function != 0; ++ft, ot = ot ? ot + 1 : nullptr)
            {
                bool match = false;
                if (ot != nullptr && !IMAGE_SNAP_BY_ORDINAL(ot->u1.Ordinal))
                {
                    const IMAGE_IMPORT_BY_NAME *ibn = (const IMAGE_IMPORT_BY_NAME *)(base + ot->u1.AddressOfData);
                    match = std::strcmp((const char *)ibn->Name, name) == 0;
                }
                else if (by_value != nullptr)
                    match = (void *)(uintptr_t)ft->u1.Function == by_value;
                if (!match) continue;
                void **slot = (void **)&ft->u1.Function;
                if (*slot == repl) { *orig = repl; return slot; }   // RV2: already ours - no write, no VirtualProtect
                DWORD old = 0;
                if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) return nullptr;
                *orig = *slot;
                *slot = repl;
                VirtualProtect(slot, sizeof(void *), old, &old);
                return slot;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
    return nullptr;
}

void restore(void **slot, void *ours, void *orig)
{
    if (slot == nullptr || orig == nullptr) return;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) return;
    if (*slot == ours) *slot = orig;
    VirtualProtect(slot, sizeof(void *), old, &old);
}

bool visible_now()
{
    return game_wants_pointer();   // MC8
}

// Ask the game to set its pointer again, so a change of g_map_on takes effect
// without waiting for the next mouse move.
void nudge(HWND game)
{
    if (game != nullptr)
        PostMessageW(game, WM_SETCURSOR, (WPARAM)game, MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
}

// One pointer image -> BGRA top-down with alpha. Present thread.
std::vector<uint32_t> g_px;
HCURSOR  g_px_of = nullptr;
unsigned g_px_w = 0, g_px_h = 0, g_serial = 0;
int      g_hot_x = 0, g_hot_y = 0;

bool read_bits(HDC dc, HBITMAP bm, unsigned w, unsigned h, std::vector<uint32_t> &out)
{
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = (LONG)w;
    bi.bmiHeader.biHeight = -(LONG)h;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    out.assign((size_t)w * h, 0u);
    return GetDIBits(dc, bm, 0, h, out.data(), &bi, DIB_RGB_COLORS) == (int)h;
}

bool convert(HCURSOR c)
{
    ICONINFO ii{};
    if (!GetIconInfo(c, &ii)) return false;
    BITMAP bm{};
    bool ok = false;
    HDC dc = GetDC(nullptr);
    if (ii.hbmColor != nullptr && GetObjectW(ii.hbmColor, sizeof bm, &bm) != 0)
    {
        const unsigned w = (unsigned)bm.bmWidth, h = (unsigned)bm.bmHeight;
        std::vector<uint32_t> col, msk;
        if (w > 0 && h > 0 && w <= 256 && h <= 256 && read_bits(dc, ii.hbmColor, w, h, col))
        {
            bool has_alpha = false;
            for (uint32_t p : col) if ((p >> 24) != 0) { has_alpha = true; break; }
            if (!has_alpha && ii.hbmMask != nullptr && read_bits(dc, ii.hbmMask, w, h, msk))
                for (size_t i = 0; i < col.size(); ++i)
                    col[i] = (col[i] & 0x00FFFFFFu) | (((msk[i] & 0x00FFFFFFu) == 0u) ? 0xFF000000u : 0u);
            else if (!has_alpha)
                for (uint32_t &p : col) p |= 0xFF000000u;
            g_px.swap(col); g_px_w = w; g_px_h = h; ok = true;
        }
    }
    else if (ii.hbmMask != nullptr && GetObjectW(ii.hbmMask, sizeof bm, &bm) != 0)
    {
        // Monochrome: the mask is twice as tall - AND on top, XOR below.
        const unsigned w = (unsigned)bm.bmWidth, h2 = (unsigned)bm.bmHeight, h = h2 / 2u;
        std::vector<uint32_t> m;
        if (w > 0 && h > 0 && w <= 256 && h <= 256 && read_bits(dc, ii.hbmMask, w, h2, m))
        {
            std::vector<uint32_t> out((size_t)w * h, 0u);
            for (unsigned y = 0; y < h; ++y)
                for (unsigned x = 0; x < w; ++x)
                {
                    const bool a = (m[(size_t)y * w + x] & 0xFFFFFFu) != 0u;
                    const bool o = (m[(size_t)(y + h) * w + x] & 0xFFFFFFu) != 0u;
                    uint32_t p = 0u;
                    if (!a) p = o ? 0xFFFFFFFFu : 0xFF000000u;   // white / black
                    else if (o) p = 0xFF000000u;                  // "inverted" - drawn black
                    out[(size_t)y * w + x] = p;
                }
            g_px.swap(out); g_px_w = w; g_px_h = h; ok = true;
        }
    }
    ReleaseDC(nullptr, dc);
    if (ok) { g_hot_x = (int)ii.xHotspot; g_hot_y = (int)ii.yHotspot; }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask)  DeleteObject(ii.hbmMask);
    return ok;
}


// MC7. REPORTED 2026-11-18 (Rise of the Tomb Raider, IN39): no pointer at all,
// while the MGPU panel's pointer works. The picture window sits on top and
// takes the mouse (forwarding it to the game), so the pointer Windows shows is
// the PICTURE window's - and its WM_SETCURSOR answered "no pointer" (MC1 draws
// one instead). The game's own ShowCursor/SetCursor therefore never reached the
// screen, and MC1 only drew when its view of the game said "visible" - a view
// built from the exe's imports alone, which Rise bypasses after loading a save.
// Now: the game's pointer calls are caught in EVERY module of the process (not
// ours, not ReShade's, not the mgpu folder), and the picture window shows the
// game's own pointer whenever the game wants one and nothing is mapped.
std::vector<void **> g_more_slots;          // extra import slots patched (restored on uninstall)
unsigned long long g_next_scan = 0;
unsigned long long g_scan_t0 = 0;           // RV2: when scanning began (2 s apart for a minute, then 30 s)
// RV2: tick() runs on the bridge thread AND (tick(false)) from the tray path;
// install/uninstall from others. One lock for the shared state they touch
// (g_more_slots, the clip, the statics in tick). Never taken by the hooks.
std::mutex g_mx;


// MC10. What the game's WINDOW THREAD itself shows. REPORTED 2026-11-18 (Rise,
// IN42): neither the game's ShowCursor calls (the loading thread hid it, -1,
// and nothing ever showed it again) nor Windows' flag (that one reports the
// thread under the pointer - ours, since the picture is on top) told the
// truth. Windows keeps a pointer state PER THREAD: display count and current
// shape. The game's window thread's own state is what Windows would show over
// the game without the bridge. So read it ON that thread: a WH_GETMESSAGE hook
// on it (in-process, our DLL is already loaded) samples its display count
// (ShowCursor(TRUE) then ShowCursor(FALSE): net zero) and GetCursor() - the
// shape its WM_SETCURSOR handling set - at most every 30 ms.
HHOOK                g_gm_hook = nullptr;
std::atomic<int>     g_gt_count{0};
std::atomic<HCURSOR> g_gt_cursor{nullptr};
std::atomic<bool>    g_gt_known{false};
std::atomic<unsigned long long> g_gt_at{0};

LRESULT CALLBACK gm_proc(int code, WPARAM wp, LPARAM lp)
{
    const unsigned long long now = GetTickCount64();
    if (code >= 0 && now - g_gt_at.load(std::memory_order_relaxed) >= 30ull)
    {
        g_gt_at.store(now, std::memory_order_relaxed);
        // Our own module's imports are not hooked: these are Windows' real calls.
        ShowCursor(TRUE);
        const int c = ShowCursor(FALSE);
        g_gt_count.store(c, std::memory_order_relaxed);
        g_gt_cursor.store(GetCursor(), std::memory_order_relaxed);
        g_gt_known.store(true, std::memory_order_relaxed);
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void gm_install(HWND game)
{
    if (g_gm_hook != nullptr || game == nullptr) return;
    const DWORD tid = GetWindowThreadProcessId(game, nullptr);
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&gm_proc, &self);
    g_gm_hook = SetWindowsHookExW(WH_GETMESSAGE, &gm_proc, self, tid);
    char l[200];
    snprintf(l, sizeof l, "[MGPU][MC10] reading the pointer state on the game's window thread %lu: %s.",
             (unsigned long)tid, g_gm_hook ? "on" : "FAILED - falling back to the earlier signals");
    mgpu::diag::info(l);
}

bool mc7_skip(HMODULE m)
{
    if (m == nullptr || m == GetModuleHandleW(nullptr)) return true;    // the exe: done by install
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&mc7_skip, &self);
    if (m == self) return true;
    if (GetProcAddress(m, "ReShadeRegisterAddon") != nullptr) return true;   // ReShade itself
    wchar_t p[MAX_PATH * 2] = {};
    if (GetModuleFileNameW(m, p, MAX_PATH * 2) == 0) return true;
    for (wchar_t *c = p; *c; ++c) if (*c >= L'A' && *c <= L'Z') *c = (wchar_t)(*c + 32);
    if (std::wcsstr(p, L"\\mgpu\\") != nullptr) return true;
    if (std::wcsstr(p, L"\\windows\\") != nullptr) return true;          // Windows' own DLLs
    return false;
}

void mc7_scan()
{
    if (g_real_set == nullptr) return;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W me{}; me.dwSize = sizeof me;
    unsigned added = 0;
    if (Module32FirstW(snap, &me))
        do
        {
            if (mc7_skip(me.hModule)) continue;
            void *o = nullptr;
            if (void **sl = patch_import(me.hModule, "SetCursor", (void *)&hk_set_cursor, &o))
            {
                if (o == (void *)&hk_set_cursor) continue;   // already ours (MC8: no re-patching every 2 s)
                g_more_slots.push_back(sl); ++added;
            }
            o = nullptr;
            if (g_real_show != nullptr)
                if (void **sl = patch_import(me.hModule, "ShowCursor", (void *)&hk_show_cursor, &o))
                    if (o != (void *)&hk_show_cursor) { g_more_slots.push_back(sl); ++added; }
        } while (Module32NextW(snap, &me));
    CloseHandle(snap);
    if (added != 0)
    {
        char l[160];
        snprintf(l, sizeof l, "[MGPU][MC7] pointer calls now also caught in other game modules: %u more import slot(s).", added);
        mgpu::diag::info(l);
    }
}

bool game_wants_pointer()
{
    // MC8. IN40's log (Rise, windowed): after a save loads the game re-shows
    // its pointer with NO ShowCursor call in any of its modules (MC4: 0 calls,
    // count stuck at -1), yet Windows' own "pointer showing" flag followed the
    // pause menu exactly (FW1: 577 frames visible while it was open, 3 before).
    // That flag is the display count - our blank / null SHAPE does not change
    // it - so it is the game's real wish. The tracked count is the fallback.
    //
    // MC9. IN41 (that flag alone) showed NO pointer even in the main menu: in
    // the main menu the flag reads hidden (IN40: 3 frames) while the game's
    // own count says shown; in the pause menu it is the other way round. Each
    // signal covers one menu, and in gameplay both say hidden - so either one
    // saying "shown" means shown.
    if (g_gt_known.load(std::memory_order_relaxed))   // MC10: the game's window thread, when known
        return g_gt_count.load(std::memory_order_relaxed) >= 0 && g_gt_cursor.load(std::memory_order_relaxed) != nullptr;
    const bool by_count = !g_have_show || g_count.load(std::memory_order_relaxed) >= 0;
    CURSORINFO ci{};
    ci.cbSize = sizeof ci;
    const bool by_flag = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) != 0;
    return by_count || by_flag;
}

HCURSOR game_cursor()
{
    if (g_gt_known.load(std::memory_order_relaxed))   // MC10
        if (HCURSOR t = g_gt_cursor.load(std::memory_order_relaxed)) if (t != g_blank) return t;
    HCURSOR c = g_want.load(std::memory_order_relaxed);
    if (c != nullptr && c != g_blank) return c;
    HWND g = g_game.load();
    if (g != nullptr) if (HCURSOR k = (HCURSOR)GetClassLongPtrW(g, GCLP_HCURSOR)) return k;
    return LoadCursorW(nullptr, IDC_ARROW);
}

} // namespace

bool install(HWND game)
{
    std::lock_guard<std::mutex> lk(g_mx);   // RV2
    if (game == nullptr) return false;
    if (g_installed.load())
    {
        // RV2: a game that made a new window (resolution or mode change on some
        // engines) - follow it, and move the window-thread hook (MC10) with it.
        if (g_game.load() != game)
        {
            g_game.store(game);
            if (g_gm_hook != nullptr) { UnhookWindowsHookEx(g_gm_hook); g_gm_hook = nullptr; }
            g_gt_known.store(false);
            gm_install(game);
        }
        return true;
    }
    g_game.store(game);
    HMODULE exe = GetModuleHandleW(nullptr);
    void *orig = nullptr;
    g_slot_set = patch_import(exe, "SetCursor", (void *)&hk_set_cursor, &orig);
    g_real_set = (pfn_set_cursor)orig;
    if (g_slot_set == nullptr || g_real_set == nullptr)
    {
        g_slot_set = nullptr;
        mgpu::diag::warn("[MGPU][MC1] the game does not import SetCursor directly - the pointer cannot be "
                         "moved onto the enlarged picture, so the picture steps aside in menus instead.");
        return false;
    }
    orig = nullptr;
    g_slot_show = patch_import(exe, "ShowCursor", (void *)&hk_show_cursor, &orig);
    g_real_show = (pfn_show_cursor)orig;
    g_have_show = (g_slot_show != nullptr && g_real_show != nullptr);

    // A fully transparent pointer: AND mask all ones, XOR mask all zeros.
    const int cw = GetSystemMetrics(SM_CXCURSOR), ch = GetSystemMetrics(SM_CYCURSOR);
    std::vector<BYTE> andm((size_t)((cw + 15) / 16 * 2) * ch, 0xFF), xorm(andm.size(), 0x00);
    g_blank = CreateCursor(GetModuleHandleW(nullptr), 0, 0, cw, ch, andm.data(), xorm.data());

    // What the game is showing right now, until its next SetCursor call.
    CURSORINFO ci{}; ci.cbSize = sizeof ci;
    if (GetCursorInfo(&ci)) { g_want.store((ci.flags & CURSOR_SHOWING) ? ci.hCursor : nullptr); }

    g_installed.store(true);
    mc7_scan();   // MC7
    gm_install(game);   // MC10
    g_scan_t0 = GetTickCount64();
    g_next_scan = g_scan_t0 + 2000ull;
    char l[300];
    snprintf(l, sizeof l, "[MGPU][MC1] pointer mapping ready (SetCursor%s hooked in the game's exe). In menus "
             "the pointer is kept inside the game window and drawn on the enlarged picture.",
             g_have_show ? " and ShowCursor" : "");
    mgpu::diag::info(l);
    return true;
}

void uninstall()
{
    std::lock_guard<std::mutex> lk(g_mx);   // RV2
    if (!g_installed.exchange(false)) return;
    g_map_on.store(false);
    restore(g_slot_set, (void *)&hk_set_cursor, (void *)g_real_set);
    restore(g_slot_show, (void *)&hk_show_cursor, (void *)g_real_show);
    g_slot_set = g_slot_show = nullptr;
    for (void **sl : g_more_slots)   // MC7
    {
        restore(sl, (void *)&hk_set_cursor, (void *)g_real_set);
        restore(sl, (void *)&hk_show_cursor, (void *)g_real_show);
    }
    g_more_slots.clear();
    if (g_gm_hook != nullptr) { UnhookWindowsHookEx(g_gm_hook); g_gm_hook = nullptr; g_gt_known.store(false); }   // MC10
    if (g_clipped)
    {
        RECT cur{};
        if (GetClipCursor(&cur) && std::memcmp(&cur, &g_clip, sizeof cur) == 0) ClipCursor(nullptr);
        g_clipped = false;
    }
    nudge(g_game.load());
}

bool installed() { return g_installed.load(std::memory_order_relaxed); }

// MC10: the picture window got WM_SETCURSOR - pass it on to the game's window,
// so the game sets its pointer on ITS thread exactly as without the bridge.
void forward_setcursor(LPARAM lp)
{
    HWND g = g_game.load();
    if (g != nullptr) PostMessageW(g, WM_SETCURSOR, (WPARAM)g, lp);
}

// MC7: what the picture window should show under the pointer. Null = nothing
// (MC1 is drawing it on the picture, or the game has its pointer hidden).
HCURSOR picture_cursor()
{
    if (!g_installed.load(std::memory_order_relaxed)) return nullptr;
    if (g_map_on.load(std::memory_order_relaxed)) return nullptr;
    return game_wants_pointer() ? game_cursor() : nullptr;
}

void tick(bool shown)
{
    if (!g_installed.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lk(g_mx);   // RV2
    if (!g_installed.load(std::memory_order_relaxed)) return;
    {
        // MC7: late modules. RV2: a module walk is 1-3 ms on the bridge's
        // present thread - every 2 s for the first minute (start-up loads),
        // then every 30 s, instead of every 2 s for the whole session.
        const unsigned long long now = GetTickCount64();
        if (now >= g_next_scan)
        {
            g_next_scan = now + ((now - g_scan_t0) < 60000ull ? 2000ull : 30000ull);
            mc7_scan();
        }
    }
    HWND game = g_game.load();
    // MC6. REPORTED 2026-11-18 (Rise of the Tomb Raider, IN38): still no
    // pointer in the pause menu. When the game's window already fills its
    // monitor - the picture on top is the same size and in the same place -
    // Windows' own pointer is already where it belongs on the picture, so
    // there is nothing to map: leave the real pointer alone (no blank shape,
    // nothing drawn) and the game shows and hides it exactly as without the
    // bridge. Mapping is only for a game window SMALLER than the picture.
    bool one_to_one = false;
    if (game != nullptr)
    {
        RECT rc{};
        POINT o{0, 0};
        MONITORINFO mi{};
        mi.cbSize = sizeof mi;
        if (GetClientRect(game, &rc) && ClientToScreen(game, &o) &&
            GetMonitorInfoW(MonitorFromWindow(game, MONITOR_DEFAULTTONEAREST), &mi))
        {
            OffsetRect(&rc, o.x, o.y);
            one_to_one = std::abs(rc.left - mi.rcMonitor.left) <= 2 && std::abs(rc.top - mi.rcMonitor.top) <= 2 &&
                         std::abs(rc.right - mi.rcMonitor.right) <= 2 && std::abs(rc.bottom - mi.rcMonitor.bottom) <= 2;
        }
    }
    {
        static int s_said = -1;
        if (shown && s_said != (int)one_to_one)
        {
            s_said = (int)one_to_one;
            mgpu::diag::info(one_to_one
                ? "[MGPU][MC6] the game's window fills its monitor: Windows' own pointer is used as is (no mapping)."
                : "[MGPU][MC6] the game's window is smaller than the picture: its pointer is mapped onto the picture.");
        }
    }
    bool on = shown && !one_to_one;
    {
        // MC8: a "not shown" has to last 500 ms before the mapping stops -
        // in IN39's windowed run it flapped every few hundred ms and the
        // pointer was dropped each time.
        static unsigned long long off_since = 0;
        const unsigned long long now = GetTickCount64();
        if (on) off_since = 0;
        else if (g_map_on.load() && !one_to_one && !g_panel_up.load())   // PO2: no grace for the panel
        {
            if (off_since == 0) off_since = now;
            if (now - off_since < 500ull) on = true;
        }
    }
    if (g_map_on.exchange(on) != on)
    {
        nudge(game);
        char l[160];
        snprintf(l, sizeof l, "[MGPU][MC1] pointer %s.", on ? "drawn on the enlarged picture"
                                                            : "back to normal (picture not shown)");
        mgpu::diag::info(l);
    }

    // Confine the real pointer to the game's window whenever the enlarged
    // picture is up and the game is in front. MC3: this used to require the
    // game to be SHOWING a pointer, and right after the MGPU panel closed the
    // game had not re-announced its menu pointer yet - the real pointer was
    // free, sat outside the game's window behind the click-through picture,
    // the menu got no mouse and a click landed on another program (log: SC8
    // "Chrome_WidgetWin_1" seconds after the panel closed). In gameplay the
    // game captures the mouse itself, so the clip changes nothing there. It
    // is released whenever the picture steps aside (alt-tab, other window).
    // MC4. REPORTED 2026-11-18 (Rise of the Tomb Raider): menus react to the
    // mouse but no pointer is seen. Every 30 s while the picture is up, say
    // what the game did with its pointer, so it is clear whether it shows a
    // Windows pointer (SetCursor/ShowCursor - MC1 draws it) or hides it and
    // draws its own inside the frame.
    {
        static unsigned long long s_next = 0;
        const unsigned long long now = GetTickCount64();
        if (on && now >= s_next)
        {
            const bool first = (s_next == 0);
            s_next = now + 30000ull;
            if (!first)
            {
                CURSORINFO ci{}; ci.cbSize = sizeof ci;
                GetCursorInfo(&ci);
                char l[420];
                snprintf(l, sizeof l, "[MGPU][MC4] last 30 s: game SetCursor calls %u (null %u), ShowCursor calls %u (from other modules %u), "
                         "display count %d (%s), pointer asked for %s | game window thread: count %d, shape %s | Windows pointer now %s. %s",
                         g_n_set.exchange(0), g_n_set_null.exchange(0), g_n_show.exchange(0), g_n_other.exchange(0),
                         g_count.load(), g_have_show ? "hooked" : "not imported",
                         g_want.load() != nullptr ? "yes" : "none",
                         g_gt_known.load() ? g_gt_count.load() : 99,
                         !g_gt_known.load() ? "unknown" : (g_gt_cursor.load() ? "set" : "none"),
                         (ci.flags & CURSOR_SHOWING) ? "showing" : "hidden",
                         !game_wants_pointer() ? "The game has its pointer hidden."
                         : g_map_on.load() ? "MC1 is drawing it on the picture."
                                           : "The picture window shows the game's own pointer (MC7).");
                mgpu::diag::info(l);
            }
        }
    }

    const bool want_clip = shown && game != nullptr;   // MC6: confine whenever the picture is up
    if (want_clip)
    {
        RECT rc{};
        POINT o{0, 0};
        if (GetClientRect(game, &rc) && ClientToScreen(game, &o))
        {
            OffsetRect(&rc, o.x, o.y);
            // PO3. REPORTED 2026-11-22 (RoboCop, IN71): closing the panel with
            // Esc made the game's menu shake for a second. The pointer had
            // roamed the 4K picture while the panel was up; the clip below then
            // snapped it to the game window's edge and the game saw one big jump.
            // Put it back where it was when the panel opened, before clipping.
            if (!g_panel_up.load(std::memory_order_relaxed) && g_po_restore.exchange(false))
            {
                const POINT p{ g_po_x.load(), g_po_y.load() };
                POINT now{};
                if (!GetPhysicalCursorPos(&now)) (void)GetCursorPos(&now);
                const bool in = PtInRect(&rc, p) != FALSE;
                if (in) SetCursorPos(p.x, p.y);
                // PO3 (IN87): one line per panel close - RoboCop's menu shook
                // again on IN86 and the restore left no trace in the log.
                char l[300];
                snprintf(l, sizeof l, "[MGPU][PO3] panel closed: the pointer was at (%ld,%ld); %s (%ld,%ld) | game window "
                         "(%ld,%ld)-(%ld,%ld) | %ld ms after the panel went down",
                         now.x, now.y, in ? "put back where it was when the panel opened:" : "NOT put back - where it was "
                         "when the panel opened is outside the game window:", p.x, p.y, rc.left, rc.top, rc.right, rc.bottom,
                         (long)(GetTickCount64() - g_po_down_ms.load(std::memory_order_relaxed)));
                mgpu::diag::info(l);
            }
            RECT cur{};
            if (!GetClipCursor(&cur) || std::memcmp(&cur, &rc, sizeof rc) != 0)
            {
                ClipCursor(&rc);
                g_clip = rc;
                g_clipped = true;
            }
        }
    }
    else if (g_clipped)
    {
        // Release only our own clip - in gameplay the game may have set its own.
        RECT cur{};
        if (GetClipCursor(&cur) && std::memcmp(&cur, &g_clip, sizeof cur) == 0) ClipCursor(nullptr);
        g_clipped = false;
    }
}

void panel_up(bool up)
{
    // PO3: remember where the pointer was when the panel opened (inside the
    // game's window - it was confined there). Taken before ReShade starts
    // blocking, from the physical position (not the hooked GetCursorPos).
    if (up && !g_panel_up.load(std::memory_order_relaxed))
    {
        POINT p{};
        if (GetPhysicalCursorPos(&p) || GetCursorPos(&p))
        {
            g_po_x.store(p.x, std::memory_order_relaxed);
            g_po_y.store(p.y, std::memory_order_relaxed);
            g_po_restore.store(true, std::memory_order_relaxed);
        }
    }
    if (!up && g_panel_up.load(std::memory_order_relaxed))
        g_po_down_ms.store(GetTickCount64(), std::memory_order_relaxed);
    g_panel_up.store(up, std::memory_order_relaxed);
}

bool snapshot(sprite &out)
{
    out = sprite{};
    if (!g_installed.load(std::memory_order_relaxed) || !g_map_on.load(std::memory_order_relaxed)) return false;
    if (g_panel_up.load(std::memory_order_relaxed)) return false;   // PO2
    if (!visible_now()) return false;
    HWND game = g_game.load();
    const HCURSOR c = game_cursor();   // MC10: the window thread's own shape when known
    if (c == nullptr || c == g_blank || game == nullptr) return false;
    if (c != g_px_of)
    {
        if (!convert(c)) return false;
        g_px_of = c;
        ++g_serial;
    }
    POINT p{};
    if (!GetCursorPos(&p) || !ScreenToClient(game, &p)) return false;
    out.visible = true;
    out.x = p.x; out.y = p.y;
    out.hot_x = g_hot_x; out.hot_y = g_hot_y;
    out.w = g_px_w; out.h = g_px_h;
    out.serial = g_serial;
    out.pixels = g_px.data();
    return true;
}

} // namespace cursormap
} // namespace mgpu
