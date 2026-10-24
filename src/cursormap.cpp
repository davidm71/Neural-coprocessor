// ---------------------------------------------------------------------------
// cursormap.cpp - MC1. See cursormap.hpp.
// ---------------------------------------------------------------------------
#include "cursormap.hpp"
#include "diag.hpp"

#include <atomic>
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

HCURSOR WINAPI hk_set_cursor(HCURSOR h)
{
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
    if (g_want.load(std::memory_order_relaxed) == nullptr) return false;
    return !g_have_show || g_count.load(std::memory_order_relaxed) >= 0;
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

} // namespace

bool install(HWND game)
{
    if (g_installed.load()) return true;
    if (game == nullptr) return false;
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
    char l[300];
    snprintf(l, sizeof l, "[MGPU][MC1] pointer mapping ready (SetCursor%s hooked in the game's exe). In menus "
             "the pointer is kept inside the game window and drawn on the enlarged picture.",
             g_have_show ? " and ShowCursor" : "");
    mgpu::diag::info(l);
    return true;
}

void uninstall()
{
    if (!g_installed.exchange(false)) return;
    g_map_on.store(false);
    restore(g_slot_set, (void *)&hk_set_cursor, (void *)g_real_set);
    restore(g_slot_show, (void *)&hk_show_cursor, (void *)g_real_show);
    g_slot_set = g_slot_show = nullptr;
    if (g_clipped)
    {
        RECT cur{};
        if (GetClipCursor(&cur) && std::memcmp(&cur, &g_clip, sizeof cur) == 0) ClipCursor(nullptr);
        g_clipped = false;
    }
    nudge(g_game.load());
}

bool installed() { return g_installed.load(std::memory_order_relaxed); }

void tick(bool shown)
{
    if (!g_installed.load(std::memory_order_relaxed)) return;
    HWND game = g_game.load();
    const bool on = shown;
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
    const bool want_clip = on && game != nullptr;
    if (want_clip)
    {
        RECT rc{};
        POINT o{0, 0};
        if (GetClientRect(game, &rc) && ClientToScreen(game, &o))
        {
            OffsetRect(&rc, o.x, o.y);
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

bool snapshot(sprite &out)
{
    out = sprite{};
    if (!g_installed.load(std::memory_order_relaxed) || !g_map_on.load(std::memory_order_relaxed)) return false;
    if (!visible_now()) return false;
    HWND game = g_game.load();
    const HCURSOR c = g_want.load(std::memory_order_relaxed);
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
