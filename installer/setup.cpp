// ---------------------------------------------------------------------------
// setup.cpp - IN1: the Atomic MGPU Bridge installer.
//
// A small Windows app that ships in the release folder beside the add-on and
// installs it into a game's folder:
//
//   - lists the Steam games on this PC (every Steam library) and finds the
//     folder with each game's .exe; "Browse..." picks any other game,
//   - checks that folder: both graphics cards, the add-on (up to date?),
//     ReShade with add-on support, the depth shader, ReShade.ini's shader
//     path, mgpu.ini, and the user's own nvngx_dlssnr.dll in mgpu\ (never
//     shipped - nothing from NVIDIA is redistributed),
//   - installs or updates: the add-on and its files, the bundled ReShade
//     (add-on support build) as dxgi.dll when the game has no ReShade, a
//     ReShade.ini that loads the shader; an existing mgpu.ini is KEPT (the new
//     one is written beside it as mgpu.ini.new),
//   - uninstalls exactly what it installed (mgpu\atomic_install.txt lists it).
//
// It copies files from its own folder, so it is run from the unpacked
// release folder. It does not need administrator rights unless the game is
// installed somewhere the user cannot write.
// ---------------------------------------------------------------------------
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dxgi.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cwchar>
#include <cstdio>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "gdi32.lib")

#include "../src/tray_draw.inc"   // the same artwork as the tray icon

namespace
{
// ---- the files a release folder carries (relative to the setup exe) ----
const wchar_t *kAddon    = L"nvngx.dll_mgpu_bridge.addon64";
const wchar_t *kShader   = L"reshade-shaders\\Shaders\\mgpu_depth_tap.fx";
const wchar_t *kReShade  = L"reshade\\ReShade64.dll";          // bundled, optional
const wchar_t *kReShadeL = L"reshade\\ReShade-LICENSE.txt";
const wchar_t *kManifest = L"mgpu\\atomic_install.txt";
// NA1: AMD's frame-generation DLL is not bundled any more (it was from IN6):
// the user copies it from AMD's FidelityFX SDK. The check still looks for it,
// and an uninstall still removes a copy an earlier install recorded.
const wchar_t *kAmdDst   = L"mgpu\\amd_fidelityfx_framegeneration_dx12.dll";
const wchar_t *kNrDst    = L"mgpu\\nvngx_dlssnr.dll";
// copied as-is (overwritten on update)
const wchar_t *kPlain[][2] = {
    { L"gpu1.ini",     L"gpu1.ini" },
    { L"ReShade2.ini", L"ReShade2.ini" },
    { L"README.txt",   L"Atomic MGPU Bridge README.txt" },
    { L"LICENSE",      L"Atomic MGPU Bridge LICENSE.txt" },
};

enum { ID_LIST = 101, ID_BROWSE, ID_STATUS, ID_INSTALL, ID_UNINSTALL, ID_OPEN, ID_NRDLL };

HWND g_wnd, g_list, g_status, g_btn_install, g_btn_uninstall, g_btn_open, g_btn_nr;
HFONT g_font, g_mono;
HFONT g_font_ref() { return g_font; }
std::wstring g_src;   // folder of this exe (the release files)

struct game { std::wstring name, root, exe_dir; bool searched = false; };
std::vector<game> g_games;
int g_sel = -1;

// ------------------------------------------------------------------ helpers
std::wstring join(const std::wstring &a, const std::wstring &b)
{
    if (a.empty()) return b;
    return (a.back() == L'\\' || a.back() == L'/') ? a + b : a + L"\\" + b;
}
bool exists(const std::wstring &p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
bool is_dir(const std::wstring &p)
{
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
std::wstring lower(std::wstring s) { for (auto &c : s) c = (wchar_t)towlower(c); return s; }
bool ends_with(const std::wstring &s, const std::wstring &e)
{
    return s.size() >= e.size() && lower(s.substr(s.size() - e.size())) == lower(e);
}
std::wstring dir_of(const std::wstring &p)
{
    const size_t k = p.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"" : p.substr(0, k);
}
void make_dirs(const std::wstring &d) { SHCreateDirectoryExW(nullptr, d.c_str(), nullptr); }

// IN8: folders Windows protects (C:\Program Files, ...) refuse writes from a
// program that is not running as administrator - error 5 on every file.
bool is_admin()
{
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    const bool r = GetTokenInformation(tok, TokenElevation, &e, sizeof e, &n) && e.TokenIsElevated != 0;
    CloseHandle(tok);
    return r;
}
// Can this program create a file in the folder (and in its mgpu subfolder, if there)?
bool can_write(const std::wstring &d)
{
    const std::wstring dirs[2] = { d, join(d, L"mgpu") };
    for (int i = 0; i < 2; ++i)
    {
        if (i == 1 && !is_dir(dirs[1])) continue;
        HANDLE f = CreateFileW(join(dirs[i], L"atomic_mgpu_write_test.tmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        CloseHandle(f);
    }
    return true;
}

// IN5: tell Explorer the game folder changed. REPORTED 2026-11-15 (DEATHLOOP):
// after Uninstall then Install, an open Explorer window showed only the two new
// folders, and after a second Install only the files that Install had just
// overwritten - dxgi.dll, ReShade.ini and mgpu.ini, written by the first
// Install, never appeared although the installer saw them (it kept mgpu.ini
// and wrote mgpu.ini.new). Explorer had missed the change notifications for
// files created in Program Files by another program; it looked as if Install
// had to be pressed twice and as if ReShade was never installed.
void tell_explorer(const std::wstring &t)
{
    for (const wchar_t *sub : { L"", L"mgpu", L"reshade-shaders", L"reshade-shaders\\Shaders" })
    {
        const std::wstring d = *sub ? join(t, sub) : t;
        if (is_dir(d)) SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, d.c_str(), nullptr);
    }
}

unsigned long long file_hash(const std::wstring &p, bool *ok)
{
    *ok = false;
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    unsigned long long x = 1469598103934665603ull;
    static unsigned char buf[1 << 16];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n > 0)
        for (DWORD i = 0; i < n; ++i) { x ^= buf[i]; x *= 1099511628211ull; }
    CloseHandle(h);
    *ok = true;
    return x;
}
bool same_file(const std::wstring &a, const std::wstring &b)
{
    bool oa, ob;
    const unsigned long long ha = file_hash(a, &oa), hb = file_hash(b, &ob);
    return oa && ob && ha == hb;
}

// ProductName and FileVersion from a DLL's version resource.
bool version_info(const std::wstring &p, std::wstring &product, std::wstring &ver)
{
    DWORD dummy = 0;
    const DWORD n = GetFileVersionInfoSizeW(p.c_str(), &dummy);
    if (n == 0) return false;
    std::vector<unsigned char> b(n);
    if (!GetFileVersionInfoW(p.c_str(), 0, n, b.data())) return false;
    struct lc { WORD lang, cp; } *tr = nullptr;
    UINT tl = 0;
    if (!VerQueryValueW(b.data(), L"\\VarFileInfo\\Translation", (void **)&tr, &tl) || tl < sizeof(lc)) return false;
    wchar_t key[96];
    wchar_t *v = nullptr;
    UINT vl = 0;
    swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\ProductName", tr[0].lang, tr[0].cp);
    if (VerQueryValueW(b.data(), key, (void **)&v, &vl) && v) product = v;
    swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileVersion", tr[0].lang, tr[0].cp);
    if (VerQueryValueW(b.data(), key, (void **)&v, &vl) && v) ver = v;
    return true;
}
bool is_reshade(const std::wstring &p, std::wstring *ver)
{
    std::wstring prod, v;
    if (!version_info(p, prod, v)) return false;
    if (lower(prod).find(L"reshade") == std::wstring::npos) return false;
    if (ver) *ver = v;
    return true;
}

std::wstring read_text(const std::wstring &p)
{
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::string s;
    char buf[8192];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n > 0) s.append(buf, n);
    CloseHandle(h);
    const int w = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(w, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], w);
    return out;
}
bool write_text(const std::wstring &p, const std::wstring &t)
{
    const int n = WideCharToMultiByte(CP_UTF8, 0, t.c_str(), (int)t.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, t.c_str(), (int)t.size(), &s[0], n, nullptr, nullptr);
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const bool ok = WriteFile(h, s.data(), (DWORD)s.size(), &w, nullptr) && w == s.size();
    CloseHandle(h);
    return ok;
}

// ------------------------------------------------------------------- Steam
// Every Steam library (libraryfolders.vdf), then every installed game in it
// (appmanifest_*.acf: "name" and "installdir").
std::wstring vdf_value(const std::wstring &line)
{
    // "key"  "value"  -> value, with \\ unescaped
    size_t q[4], at = 0;
    for (int i = 0; i < 4; ++i) { q[i] = line.find(L'"', at); if (q[i] == std::wstring::npos) return L""; at = q[i] + 1; }
    std::wstring v = line.substr(q[2] + 1, q[3] - q[2] - 1);
    std::wstring o;
    for (size_t i = 0; i < v.size(); ++i)
    {
        if (v[i] == L'\\' && i + 1 < v.size() && v[i + 1] == L'\\') { o += L'\\'; ++i; }
        else o += v[i];
    }
    return o;
}
std::wstring vdf_key(const std::wstring &line)
{
    const size_t a = line.find(L'"');
    if (a == std::wstring::npos) return L"";
    const size_t b = line.find(L'"', a + 1);
    return b == std::wstring::npos ? L"" : lower(line.substr(a + 1, b - a - 1));
}
std::vector<std::wstring> g_libs;   // IN6: every Steam library, for the nvngx_dlssnr.dll search
void scan_steam()
{
    g_games.clear();
    wchar_t sp[MAX_PATH] = {};
    DWORD n = sizeof sp;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, sp, &n) != ERROR_SUCCESS)
        return;
    std::wstring steam = sp;
    std::replace(steam.begin(), steam.end(), L'/', L'\\');
    std::vector<std::wstring> libs{ steam };
    const std::wstring vdf = read_text(join(steam, L"steamapps\\libraryfolders.vdf"));
    size_t at = 0;
    while (at < vdf.size())
    {
        size_t e = vdf.find(L'\n', at);
        if (e == std::wstring::npos) e = vdf.size();
        const std::wstring line = vdf.substr(at, e - at);
        if (vdf_key(line) == L"path")
        {
            std::wstring p = vdf_value(line);
            if (!p.empty() && std::find_if(libs.begin(), libs.end(), [&](const std::wstring &x) { return lower(x) == lower(p); }) == libs.end())
                libs.push_back(p);
        }
        at = e + 1;
    }
    g_libs = libs;
    static const wchar_t *const kSkip[] = { L"redistributable", L"proton", L"steam linux runtime", L"steamvr",
                                            L"steamworks", L"dedicated server" };
    for (const auto &lib : libs)
    {
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW(join(lib, L"steamapps\\appmanifest_*.acf").c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) continue;
        do
        {
            const std::wstring acf = read_text(join(lib, std::wstring(L"steamapps\\") + fd.cFileName));
            std::wstring name, dir;
            size_t a = 0;
            while (a < acf.size() && (name.empty() || dir.empty()))
            {
                size_t e = acf.find(L'\n', a);
                if (e == std::wstring::npos) e = acf.size();
                const std::wstring line = acf.substr(a, e - a);
                const std::wstring k = vdf_key(line);
                if (k == L"name" && name.empty()) name = vdf_value(line);
                else if (k == L"installdir" && dir.empty()) dir = vdf_value(line);
                a = e + 1;
            }
            if (name.empty() || dir.empty()) continue;
            bool skip = false;
            for (const wchar_t *k : kSkip) if (lower(name).find(k) != std::wstring::npos) skip = true;
            const std::wstring root = join(lib, L"steamapps\\common\\" + dir);
            if (skip || !is_dir(root)) continue;
            game g;
            g.name = name;
            g.root = root;
            g_games.push_back(g);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    std::sort(g_games.begin(), g_games.end(), [](const game &a, const game &b) { return lower(a.name) < lower(b.name); });
}

// The folder with the game's real .exe: Unreal games keep it in
// Binaries\Win64 as <Name>-Win64-Shipping.exe; otherwise the biggest .exe
// that is not a launcher, crash reporter or redistributable.
void find_exe(const std::wstring &d, int depth, std::wstring &best, long long &best_score)
{
    if (depth > 6) return;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(join(d, L"*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    static const wchar_t *const kNot[] = { L"crash", L"unins", L"redist", L"setup", L"dxsetup", L"prereq",
                                           L"easyanticheat", L"beservice", L"launcher", L"helper", L"vc_", L"dotnet",
                                           L"directx", L"report", L"install", L"cefprocess", L"webhelper", L"ue4prereq" };
    do
    {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        const std::wstring p = join(d, n);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            const std::wstring ln = lower(n);
            if (ln == L"engine" || ln == L"redist" || ln == L"_commonredist" || ln == L"directx" ||
                ln == L"easyanticheat" || ln == L"battleye" || ln == L"content" || ln == L"movies") continue;
            find_exe(p, depth + 1, best, best_score);
            continue;
        }
        if (!ends_with(n, L".exe")) continue;
        const std::wstring ln = lower(n);
        bool bad = false;
        for (const wchar_t *k : kNot) if (ln.find(k) != std::wstring::npos) bad = true;
        if (bad) continue;
        long long score = ((long long)fd.nFileSizeHigh << 32 | fd.nFileSizeLow) / 1024;   // KB
        if (ends_with(ln, L"-win64-shipping.exe")) score += 1ll << 40;
        if (lower(p).find(L"\\binaries\\win64\\") != std::wstring::npos) score += 1ll << 39;
        if (score > best_score) { best_score = score; best = d; }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

// ------------------------------------------------------------------ checks
struct gpu_info { int nvidia = 0; int rtx50 = 0; std::wstring names; };   // RQ1: rtx50
gpu_info gpus()
{
    gpu_info r;
    IDXGIFactory1 *f = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f))) || !f) return r;
    IDXGIAdapter1 *a = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
        {
            if (!r.names.empty()) r.names += L", ";
            r.names += d.Description;
            if (d.VendorId == 0x10DE) ++r.nvidia;
            // RQ1: an RTX 50-series (Blackwell) card - the generation NVIDIA documents for neural rendering
            if (d.VendorId == 0x10DE && (wcsstr(d.Description, L"RTX 50") != nullptr || wcsstr(d.Description, L"Blackwell") != nullptr ||
                                         wcsstr(d.Description, L"RTX 40") != nullptr)) ++r.rtx50;   // RQ3: an RTX 4090 ran it (field report #2)
        }
        a->Release();
    }
    f->Release();
    return r;
}

std::wstring manifest_value(const std::wstring &t, const std::wstring &key);

// IN3: which card each monitor is plugged into, and whether Windows HDR is on
// for it - from the Windows display configuration (QueryDisplayConfig), which
// names the card a monitor is cabled to even where DXGI says it has none.
struct monitor_info { std::wstring name, card; bool nvidia = false, hdr_ok = false, hdr_on = false; };
std::vector<monitor_info> monitors()
{
    std::vector<monitor_info> out;
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || np == 0) return out;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm ? nm : 1);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS) return out;
    IDXGIFactory1 *f = nullptr;
    CreateDXGIFactory1(IID_PPV_ARGS(&f));
    for (UINT32 i = 0; i < np; ++i)
    {
        monitor_info m;
        DISPLAYCONFIG_TARGET_DEVICE_NAME tn{};
        tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tn.header.size = sizeof tn;
        tn.header.adapterId = paths[i].targetInfo.adapterId;
        tn.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tn.header) == ERROR_SUCCESS) m.name = tn.monitorFriendlyDeviceName;
        if (m.name.empty()) m.name = L"monitor";
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO ac{};
        ac.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
        ac.header.size = sizeof ac;
        ac.header.adapterId = paths[i].targetInfo.adapterId;
        ac.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&ac.header) == ERROR_SUCCESS)
        {
            m.hdr_ok = (ac.value & 0x1) != 0;   // advancedColorSupported
            m.hdr_on = (ac.value & 0x2) != 0;   // advancedColorEnabled
        }
        const LUID l = paths[i].targetInfo.adapterId;
        m.card = L"an unknown card";
        IDXGIAdapter1 *a = nullptr;
        for (UINT k = 0; f && f->EnumAdapters1(k, &a) != DXGI_ERROR_NOT_FOUND; ++k)
        {
            DXGI_ADAPTER_DESC1 d{};
            if (SUCCEEDED(a->GetDesc1(&d)) && d.AdapterLuid.LowPart == l.LowPart && d.AdapterLuid.HighPart == l.HighPart)
            {
                m.card = d.Description;
                m.nvidia = d.VendorId == 0x10DE;
            }
            a->Release();
        }
        out.push_back(m);
    }
    if (f) f->Release();
    return out;
}
bool any_hdr_on()
{
    for (const auto &m : monitors()) if (m.hdr_on) return true;
    return false;
}

struct reshade_at { std::wstring file, ver; };   // the DLL ReShade is installed as, if any
// ReShade by its version info - or this installer's own copy: the same bytes as
// the bundled DLL, or the name the install record says it was put in as. Without
// the last two a ReShade build with odd version info was taken for "another
// mod", and a second Install put a second ReShade in as d3d12.dll (found in a
// Wine test, 2026-11-15).
reshade_at find_reshade(const std::wstring &t)
{
    reshade_at r;
    const std::wstring rec = lower(manifest_value(t, L"reshade"));
    const std::wstring bundled = join(g_src, kReShade);
    for (const wchar_t *n : { L"dxgi.dll", L"d3d12.dll", L"d3d11.dll" })
    {
        const std::wstring p = join(t, n);
        if (!exists(p)) continue;
        std::wstring v;
        if (is_reshade(p, &v) || lower(n) == rec || (exists(bundled) && same_file(p, bundled)))
        {
            r.file = n;
            r.ver = v.empty() ? L"bundled copy" : v;
            break;
        }
    }
    return r;
}

std::wstring manifest_value(const std::wstring &t, const std::wstring &key)
{
    const std::wstring m = read_text(join(t, kManifest));
    size_t at = 0;
    while (at < m.size())
    {
        size_t e = m.find(L'\n', at);
        if (e == std::wstring::npos) e = m.size();
        std::wstring line = m.substr(at, e - at);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.rfind(key + L"=", 0) == 0) return line.substr(key.size() + 1);
        at = e + 1;
    }
    return L"";
}

void say(std::wstring &out, const wchar_t *tag, const std::wstring &text) { out += tag; out += L"  "; out += text; out += L"\r\n"; }

// IN4: Windows' "VirtualStore". A program that writes into Program Files
// without permission (old 32-bit programs without a manifest) has its files
// quietly put in %LOCALAPPDATA%\VirtualStore\<same path> instead - such
// programs then see them in the game folder, Explorer and the game do not.
bool copy_in(const std::wstring &from, const std::wstring &to, std::wstring &log, std::vector<std::wstring> *placed,
             const std::wstring &rel);

std::wstring virtual_store(const std::wstring &t)
{
    wchar_t la[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH) == 0 || t.size() < 3 || t[1] != L':') return L"";
    const std::wstring vs = join(join(la, L"VirtualStore"), t.substr(3));
    if (!is_dir(vs)) return L"";
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(join(vs, L"*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return L"";
    std::wstring names;
    do
    {
        if (fd.cFileName[0] == L'.') continue;
        if (!names.empty()) names += L", ";
        names += fd.cFileName;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return names.empty() ? L"" : vs + L"  (" + names + L")";
}

std::wstring check(const std::wstring &t)
{
    std::wstring o;
    o += L"Folder: " + t + L"\r\n\r\n";
    {
        const std::wstring vs = virtual_store(t);
        if (!vs.empty())
            say(o, L"[!] ", L"Windows keeps a hidden copy of files for this folder in " + vs + L". Programs see those files "
                            L"in the game folder but the game does not - they came from an older program writing here "
                            L"without permission. Delete that VirtualStore folder, then Install again.");
    }
    const gpu_info g = gpus();
    if (g.nvidia >= 2) say(o, L"[OK]", L"Graphics cards: " + g.names);
    else say(o, L"[!] ", L"Graphics cards: " + g.names + L" - the bridge needs two NVIDIA cards.");
    if (g.nvidia >= 1 && g.rtx50 == 0)   // RQ1
        say(o, L"[!] ", L"No RTX 50- or 40-series card found. NVIDIA documents DLSS Neural Rendering for RTX 50-series cards; "
                        L"an RTX 4090 has been reported working. On a card that cannot run it, the driver refuses it "
                        L"(\"FeatureNotSupported\", 0xBAD00001) and the bridge shows the picture without neural rendering. "
                        L"Use an RTX 50-series card as the second (bridge) card.");
    // IN3: the recommended layout - the game's card headless, the monitor on the second card.
    {
        const auto mons = monitors();
        std::vector<std::wstring> nv_cards;
        for (const auto &m : mons)
        {
            say(o, L"    ", L"Monitor \"" + m.name + L"\" is plugged into " + m.card +
                (m.hdr_on ? L" (Windows HDR on)." : m.hdr_ok ? L" (HDR capable, Windows HDR off)." : L"."));
            if (m.nvidia && std::find(nv_cards.begin(), nv_cards.end(), m.card) == nv_cards.end()) nv_cards.push_back(m.card);
        }
        if (g.nvidia >= 2 && nv_cards.size() == 1)
            say(o, L"[OK]", L"Your NVIDIA monitor(s) hang off one card (" + nv_cards[0] + L") - the other NVIDIA card can "
                            L"run headless and render the game, as recommended.");
        else if (g.nvidia >= 2 && nv_cards.size() > 1)
            say(o, L"[!] ", L"Monitors are plugged into BOTH NVIDIA cards. Recommended: the game's card headless (no "
                            L"monitor), your monitor on the second card that does the neural work.");
    }

    const std::wstring add = join(t, kAddon);
    if (!exists(add)) say(o, L"[--]", L"Atomic MGPU Bridge: not installed.");
    else if (same_file(add, join(g_src, kAddon))) say(o, L"[OK]", L"Atomic MGPU Bridge: installed, up to date.");
    else say(o, L"[!] ", L"Atomic MGPU Bridge: installed, a different version - Install updates it.");

    const reshade_at rs = find_reshade(t);
    if (!rs.file.empty())
    {
        const bool ours = exists(join(g_src, kReShade)) && same_file(join(t, rs.file), join(g_src, kReShade));
        say(o, L"[OK]", L"ReShade " + rs.ver + L" as " + join(t, rs.file) + (ours ? L" (the bundled add-on version)." :
            L". It must be the version WITH ADD-ON SUPPORT - the effects-only version never loads the bridge."));
    }
    else if (exists(join(t, L"dxgi.dll")))
        say(o, L"[!] ", L"dxgi.dll is another mod, not ReShade. Install puts ReShade in as d3d12.dll instead.");
    else
        say(o, L"[--]", exists(join(g_src, kReShade)) ? L"ReShade: not installed - Install adds it (as dxgi.dll)."
                                                       : L"ReShade: not installed - get the version with add-on support from reshade.me.");

    if (exists(join(t, kShader))) say(o, L"[OK]", L"Depth shader mgpu_depth_tap.fx in place.");
    else say(o, L"[--]", L"Depth shader mgpu_depth_tap.fx: missing (the bridge never starts without it).");

    const std::wstring rini = join(t, L"ReShade.ini");
    if (exists(rini))
    {
        wchar_t esp[2048] = {};
        GetPrivateProfileStringW(L"GENERAL", L"EffectSearchPaths", L"", esp, 2048, rini.c_str());
        if (lower(esp).find(L"reshade-shaders\\shaders") != std::wstring::npos || lower(esp).find(L"reshade-shaders/shaders") != std::wstring::npos)
            say(o, L"[OK]", L"ReShade.ini looks for shaders in reshade-shaders\\Shaders.");
        else say(o, L"[!] ", L"ReShade.ini does not look in reshade-shaders\\Shaders - Install adds it.");
    }

    // IN5: the files, as Windows has them right now (Explorer may lag - press F5 there).
    {
        std::wstring have;
        for (const wchar_t *f : { L"dxgi.dll", L"d3d12.dll", L"ReShade.ini", L"mgpu.ini", L"gpu1.ini", L"ReShade2.ini", kAddon })
        {
            WIN32_FILE_ATTRIBUTE_DATA a{};
            if (!GetFileAttributesExW(join(t, f).c_str(), GetFileExInfoStandard, &a)) continue;
            wchar_t b[96];
            swprintf_s(b, L"%s%s (%llu KB%s)", have.empty() ? L"" : L", ", f,
                       (((unsigned long long)a.nFileSizeHigh << 32) | a.nFileSizeLow) / 1024ull,
                       (a.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) ? L", HIDDEN" : L"");
            have += b;
        }
        say(o, L"    ", L"In the folder now: " + (have.empty() ? std::wstring(L"none of the bridge's files") : have) + L".");
    }
    if (exists(join(t, L"mgpu.ini"))) say(o, L"[OK]", L"mgpu.ini present (your settings - Install keeps it).");
    else say(o, L"[--]", L"mgpu.ini: missing - Install adds the default one.");

    if (exists(join(t, L"mgpu\\nvngx_dlssnr.dll"))) say(o, L"[OK]", L"mgpu\\nvngx_dlssnr.dll in place.");
    else say(o, L"[!] ", L"mgpu\\nvngx_dlssnr.dll: missing - copy YOUR OWN nvngx_dlssnr.dll into the mgpu folder "
                         L"(\"Open game folder\"). It is not included and may not be redistributed.");
    if (exists(join(t, L"nvngx_dlssnr.dll")))
        say(o, L"[!] ", L"nvngx_dlssnr.dll is ALSO beside the game's .exe - remove that copy; some games load it onto the wrong card.");
    if (exists(join(t, kAmdDst)))
    {
        std::wstring prod, ver;
        version_info(join(t, kAmdDst), prod, ver);
        say(o, L"[OK]", L"AMD frame generation DLL in mgpu" + (ver.empty() ? std::wstring(L".") : L" (version " + ver + L")."));
    }
    else say(o, L"[--]", L"AMD frame generation DLL: not in mgpu - only needed for AMD FSR 3 frame generation. Copy "
                         L"amd_fidelityfx_framegeneration_dx12.dll from AMD's FidelityFX SDK (GitHub, Kits\\FidelityFX\\signedbin) "
                         L"into the mgpu folder.");
    return o;
}

bool manifest_has_file(const std::wstring &t, const std::wstring &rel)
{
    const std::wstring m = lower(read_text(join(t, kManifest)));
    return m.find(L"file=" + lower(rel) + L"\r") != std::wstring::npos || m.find(L"file=" + lower(rel) + L"\n") != std::wstring::npos;
}
void manifest_add_file(const std::wstring &t, const std::wstring &rel)
{
    if (manifest_has_file(t, rel)) return;
    std::wstring m = read_text(join(t, kManifest));
    if (m.empty()) m = L"; Atomic MGPU Bridge installer - what it placed in this folder\r\n";
    m += L"file=" + rel + L"\r\n";
    write_text(join(t, kManifest), m);
}

// ------------------------------------------------- the user's nvngx_dlssnr.dll
// IN6: it is never shipped (nothing from NVIDIA is redistributed) - but the
// user may already have a copy on this PC, e.g. in another game set up for
// the bridge. Copying their own file into this game is a local copy.
struct found_dll { std::wstring path, ver; unsigned long long v = 0; ULONGLONG mt = 0; };
unsigned long long ver_key(const std::wstring &v)
{
    unsigned a = 0, b = 0, c = 0, d = 0;
    swscanf_s(v.c_str(), L"%u.%u.%u.%u", &a, &b, &c, &d);
    if (swscanf_s(v.c_str(), L"%u, %u, %u, %u", &a, &b, &c, &d) == 4) {}
    return ((unsigned long long)(a & 0xFFFF) << 48) | ((unsigned long long)(b & 0xFFFF) << 32) |
           ((unsigned long long)(c & 0xFFFF) << 16) | (d & 0xFFFF);
}
void find_named(const std::wstring &d, int depth, int max_depth, const std::wstring &skip, std::vector<found_dll> &out,
                ULONGLONG deadline)
{
    if (depth > max_depth || GetTickCount64() > deadline) return;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(join(d, L"*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do
    {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        const std::wstring p = join(d, n);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            const std::wstring ln = lower(n);
            if (ln == L"content" || ln == L"movies" || ln == L"paks" || ln == L"shadercache" || ln == L"_commonredist") continue;
            if (!skip.empty() && lower(p) == skip) continue;
            find_named(p, depth + 1, max_depth, skip, out, deadline);
            continue;
        }
        if (lower(n) != L"nvngx_dlssnr.dll") continue;
        found_dll x;
        x.path = p;
        std::wstring prod;
        version_info(p, prod, x.ver);
        x.v = ver_key(x.ver);
        x.mt = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        out.push_back(x);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}
std::vector<found_dll> search_dlssnr(const std::wstring &t)
{
    std::vector<found_dll> out;
    const ULONGLONG deadline = GetTickCount64() + 20000;   // a long Steam library is not searched forever
    const std::wstring skip = lower(t);
    for (const auto &lib : g_libs) find_named(join(lib, L"steamapps\\common"), 0, 6, skip, out, deadline);
    wchar_t up[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH))
        for (const wchar_t *sub : { L"Downloads", L"Desktop", L"Documents" }) find_named(join(up, sub), 0, 4, skip, out, deadline);
    wchar_t pd[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH)) find_named(join(pd, L"NVIDIA\\NGX"), 0, 6, skip, out, deadline);
    std::sort(out.begin(), out.end(), [](const found_dll &a, const found_dll &b) { return a.v != b.v ? a.v > b.v : a.mt > b.mt; });
    return out;
}
// Offers the best copy found (or a file dialog when asked from the button).
// Returns true when mgpu\nvngx_dlssnr.dll is in place afterwards.
bool offer_dlssnr(HWND owner, const std::wstring &t, bool browse_if_none, std::wstring &log)
{
    if (exists(join(t, kNrDst))) return true;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    const auto found = search_dlssnr(t);
    std::wstring from;
    if (!found.empty())
    {
        std::wstring q = L"Found your nvngx_dlssnr.dll on this PC:\n\n" + found[0].path +
                         (found[0].ver.empty() ? L"" : L"\n(version " + found[0].ver + L")") +
                         L"\n\nCopy it into this game's mgpu folder?\n\n(A copy of your own file - nothing is downloaded.)";
        if (found.size() > 1) q += L"\n\n" + std::to_wstring(found.size()) + L" copies were found; this is the newest.";
        const int r = MessageBoxW(owner, q.c_str(), L"Atomic MGPU Bridge", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r == IDYES) from = found[0].path;
        else if (r == IDCANCEL) return false;
    }
    if (from.empty() && browse_if_none)
    {
        wchar_t file[MAX_PATH] = {};
        OPENFILENAMEW of{};
        of.lStructSize = sizeof of;
        of.hwndOwner = owner;
        of.lpstrFilter = L"nvngx_dlssnr.dll\0nvngx_dlssnr.dll\0";
        of.lpstrFile = file;
        of.nMaxFile = MAX_PATH;
        of.lpstrTitle = found.empty() ? L"No copy found on this PC - where is your nvngx_dlssnr.dll?" : L"Choose your nvngx_dlssnr.dll";
        of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&of)) from = file;
    }
    if (from.empty()) return false;
    if (copy_in(from, join(t, kNrDst), log, nullptr, kNrDst))
    {
        manifest_add_file(t, kNrDst);
        log += L"  (copied from " + from + L")\r\n";
        return true;
    }
    return false;
}

// ------------------------------------------------------------- install etc.
bool copy_in(const std::wstring &from, const std::wstring &to, std::wstring &log, std::vector<std::wstring> *placed,
             const std::wstring &rel)
{
    make_dirs(dir_of(to));
    if (!CopyFileW(from.c_str(), to.c_str(), FALSE))
    {
        const DWORD e = GetLastError();
        wchar_t m[260];
        swprintf_s(m, L"  FAILED to copy %s (error %lu%s)\r\n", rel.c_str(), e,
                   e == ERROR_SHARING_VIOLATION ? L" - close the game first" :
                   e == ERROR_ACCESS_DENIED ? L" - run the installer as administrator" : L"");
        log += m;
        return false;
    }
    log += L"  " + rel + L"\r\n";
    if (placed) placed->push_back(rel);
    return true;
}

// ---------------------------------------------------------------- settings
// IN2: what the user wants to run, asked before installing and written into
// the game's mgpu.ini (all of it can be changed later in the panel). On an
// update the default is to keep the user's own settings.
struct settings
{
    bool apply = true;   // false = keep the mgpu.ini that is there
    int card = 0;        // 0 mid-range (reduced, self-adjusting neural resolution), 1 high-end (full)
    int scaler = 3;      // Scaler: 0 none, 1 bilinear, 2 FSR 1, 3 NIS
    int fg = 4;          // FrameGen: 0 off, 1 built-in, 3 AMD FSR 3, 4 NVIDIA DLSS FG
    int mult = 4;        // 2..4
    bool cap_on = true;
    int cap = 40;
    bool hudless = true;
    int reflex = 1;      // 0 off, 1 on, 2 on + boost
    int hdr = 0;         // HDR: 0 follow the game, 1 also turn SDR games into HDR
};

int monitor_hz()
{
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) return (int)dm.dmDisplayFrequency;
    return 60;
}
// The game frame rate the second card can feed at this multiplier: what fits
// the monitor, and no more than a card of this class keeps up with (an RTX
// 5060 Ti at 1440p: about 40 fps for 4x, 50 for 3x - see the README).
int suggest_cap(int card, int mult)
{
    static const int mid[5] = { 0, 0, 60, 50, 40 }, high[5] = { 0, 0, 100, 80, 60 };
    if (mult < 2) mult = 2;
    if (mult > 4) mult = 4;
    const int fit = monitor_hz() / mult;
    const int lim = card == 0 ? mid[mult] : high[mult];
    return std::max(20, std::min(fit, lim));
}

bool g_put_ok = true;   // IN8: every settings write landed
void put(const std::wstring &ini, const wchar_t *k, int v)
{
    wchar_t b[16];
    swprintf_s(b, L"%d", v);
    if (!WritePrivateProfileStringW(L"MGPU", k, b, ini.c_str())) g_put_ok = false;
}
bool apply_settings(const std::wstring &t, const settings &s, std::wstring &log)
{
    const std::wstring ini = join(t, L"mgpu.ini");
    if (!exists(ini)) { log += L"  mgpu.ini settings NOT written: there is no mgpu.ini in the game folder.\r\n"; return false; }
    g_put_ok = true;
    // The second card's class. Mid-range: the panel's "Quality" pair (scale
    // and low-resolution vectors are ONE setting - written together, as the
    // panel does) plus the dynamic resolution driven by late frame pairs.
    if (s.card == 0)
    {
        put(ini, L"SRUpscale", 1); put(ini, L"SRQuality", 2); put(ini, L"SRScale", 67); put(ini, L"SRMvLowRes", 1);
        put(ini, L"SRDynamic", 1); put(ini, L"SRDynLatePairs", 1);
    }
    else
    {
        put(ini, L"SRUpscale", 0); put(ini, L"SRDynamic", 0);
    }
    put(ini, L"Scaler", s.scaler);
    put(ini, L"FrameGen", s.fg);
    if (s.fg == 4) put(ini, L"FGMultiplier", s.mult);
    if (s.fg == 3) put(ini, L"FSRMult", s.mult);
    put(ini, L"FGCapOn", (s.fg != 0 && s.cap_on) ? 1 : 0);
    put(ini, L"FGCap", std::max(20, std::min(240, s.cap)));
    put(ini, L"HudLess", s.hudless ? 1 : 0);
    put(ini, L"Reflex", s.reflex);
    put(ini, L"HDR", s.hdr);   // IN3. A game's own HDR is followed either way (HD2).
    // IN9: the panel on the picture (Home) - only when the file does not say
    // either way (an mgpu.ini from the diagnostic package lacked it).
    if (GetPrivateProfileIntW(L"MGPU", L"PanelOverPicture", -1, ini.c_str()) == -1) put(ini, L"PanelOverPicture", 1);
    // The transfer settings every measured run used.
    put(ini, L"CopyQueue", 2); put(ini, L"LateLatch", 1); put(ini, L"LateLatchAge", 2); put(ini, L"StageCopy", 1);
    static const wchar_t *const kSc[] = { L"none", L"bilinear", L"FSR 1", L"NIS" };
    static const wchar_t *const kFg[] = { L"off", L"built-in", L"?", L"AMD FSR 3", L"NVIDIA DLSS FG" };
    wchar_t l[400];
    swprintf_s(l, L"  mgpu.ini settings: %s second card, upscaling %s, frame generation %s%s, game cap %s, "
                  L"HUD-less %s, Reflex %s\r\n",
               s.card == 0 ? L"mid-range" : L"high-end", kSc[s.scaler & 3], kFg[(s.fg >= 0 && s.fg <= 4) ? s.fg : 0],
               (s.fg == 3 || s.fg == 4) ? (s.mult == 2 ? L" 2x" : s.mult == 3 ? L" 3x" : L" 4x") : L"",
               (s.fg != 0 && s.cap_on) ? std::to_wstring(s.cap).append(L" fps").c_str() : L"off",
               s.hudless ? L"on" : L"off", s.reflex == 0 ? L"off" : s.reflex == 1 ? L"on" : L"on + boost");
    log += s.hdr ? L"  HDR: games in HDR stay HDR; SDR games are turned into HDR.\r\n"
                 : L"  HDR: follows each game (HDR games in HDR, SDR games in SDR).\r\n";
    if (!g_put_ok)
    {
        log += L"  mgpu.ini settings NOT written (error " + std::to_wstring(GetLastError()) + L").\r\n";
        return false;
    }
    log += l;
    return true;
}

// The settings window: modal, owned by the main window.
enum { SD_KEEP = 300, SD_APPLY, SD_MID, SD_HIGH, SD_SC0, SD_SC1, SD_SC2, SD_SC3,
       SD_FG0, SD_FG1, SD_FG3, SD_FG4, SD_MULT, SD_CAPON, SD_CAP, SD_HUD, SD_RF0, SD_RF1, SD_RF2, SD_HDR0, SD_HDR1, SD_OK, SD_CANCEL };
struct sd_state { settings s; bool have_ini = false; bool done = false; bool ok = false; };
sd_state *g_sd = nullptr;
int S(int v);
HFONT g_font_ref();

void sd_sync(HWND h)
{
    const bool apply = !g_sd->have_ini || IsDlgButtonChecked(h, SD_APPLY) == BST_CHECKED;
    for (int id = SD_MID; id <= SD_HDR1; ++id) EnableWindow(GetDlgItem(h, id), apply);
    const bool fg_on = IsDlgButtonChecked(h, SD_FG0) != BST_CHECKED;
    const bool has_mult = IsDlgButtonChecked(h, SD_FG3) == BST_CHECKED || IsDlgButtonChecked(h, SD_FG4) == BST_CHECKED;
    EnableWindow(GetDlgItem(h, SD_MULT), apply && has_mult);
    EnableWindow(GetDlgItem(h, SD_CAPON), apply && fg_on);
    EnableWindow(GetDlgItem(h, SD_CAP), apply && fg_on && IsDlgButtonChecked(h, SD_CAPON) == BST_CHECKED);
    EnableWindow(GetDlgItem(h, SD_HUD), apply && fg_on);
}
void sd_suggest(HWND h)
{
    const int card = IsDlgButtonChecked(h, SD_HIGH) == BST_CHECKED ? 1 : 0;
    const bool has_mult = IsDlgButtonChecked(h, SD_FG3) == BST_CHECKED || IsDlgButtonChecked(h, SD_FG4) == BST_CHECKED;
    const int mult = has_mult ? (int)SendDlgItemMessageW(h, SD_MULT, CB_GETCURSEL, 0, 0) + 2 : 2;
    SetDlgItemInt(h, SD_CAP, (UINT)suggest_cap(card, mult), FALSE);
}

LRESULT CALLBACK sd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        int y = 10;
        auto mk = [&](const wchar_t *cls, const wchar_t *text, DWORD style, int x, int yy, int w, int hh, int id) {
            HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(yy), S(w), S(hh), h,
                                     (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g_font_ref(), TRUE);
            return c;
        };
        const DWORD R1 = BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, R = BS_AUTORADIOBUTTON;
        mk(L"STATIC", L"Recommended setup: the card that renders the game runs HEADLESS (no monitor), and your monitor is "
                      L"plugged into the SECOND card, which does the neural work, the frame generation and the display.",
           0, 12, y, 604, 36, 0);
        y += 42;
        mk(L"STATIC", L"These go into the game's mgpu.ini. All of them can be changed later in the panel (Home).", 0, 12, y, 600, 20, 0);
        y += 26;
        if (g_sd->have_ini)
        {
            mk(L"BUTTON", L"Keep my current settings (only the program files are updated)", R1, 12, y, 600, 22, SD_KEEP); y += 24;
            mk(L"BUTTON", L"Use the settings below", R, 12, y, 600, 22, SD_APPLY); y += 32;
        }
        mk(L"BUTTON", L"Your second graphics card (the one that runs neural rendering)", BS_GROUPBOX, 8, y, 612, 76, 0);
        mk(L"BUTTON", L"Mid-range, about an RTX 5060 Ti: neural rendering at a lower resolution that adjusts itself (recommended)", R1, 20, y + 22, 590, 22, SD_MID);
        mk(L"BUTTON", L"High-end, RTX 5070 Ti / 5080 / 5090: neural rendering at full resolution", R, 20, y + 46, 590, 22, SD_HIGH);
        y += 84;
        mk(L"BUTTON", L"Enlarging the picture to your monitor (when the game runs below the monitor's resolution)", BS_GROUPBOX, 8, y, 612, 52, 0);
        mk(L"BUTTON", L"None", R1, 20, y + 22, 90, 22, SD_SC0);
        mk(L"BUTTON", L"NIS (sharp, recommended)", R, 115, y + 22, 200, 22, SD_SC3);
        mk(L"BUTTON", L"FSR 1", R, 320, y + 22, 90, 22, SD_SC2);
        mk(L"BUTTON", L"Bilinear (soft)", R, 415, y + 22, 150, 22, SD_SC1);
        y += 60;
        mk(L"BUTTON", L"Frame generation on the second card", BS_GROUPBOX, 8, y, 612, 108, 0);
        mk(L"BUTTON", L"Off", R1, 20, y + 22, 60, 22, SD_FG0);
        mk(L"BUTTON", L"NVIDIA DLSS FG (recommended)", R, 85, y + 22, 230, 22, SD_FG4);
        mk(L"BUTTON", L"AMD FSR 3", R, 320, y + 22, 110, 22, SD_FG3);
        mk(L"BUTTON", L"Built-in (2x)", R, 435, y + 22, 150, 22, SD_FG1);
        mk(L"STATIC", L"Multiplier:", 0, 20, y + 52, 80, 20, 0);
        HWND cb = mk(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_GROUP, 100, y + 48, 80, 200, SD_MULT);
        for (const wchar_t *m : { L"2x", L"3x", L"4x" }) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)m);
        mk(L"BUTTON", L"Cap the game at", BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP, 20, y + 78, 130, 22, SD_CAPON);
        mk(L"EDIT", L"", ES_NUMBER | WS_BORDER | WS_TABSTOP, 152, y + 78, 50, 22, SD_CAP);
        wchar_t hz[160];
        swprintf_s(hz, L"fps  (game fps x multiplier should fit your %d Hz monitor)", monitor_hz());
        mk(L"STATIC", hz, 0, 208, y + 81, 410, 20, 0);
        y += 116;
        {
            const bool hdr_now = any_hdr_on();
            mk(L"BUTTON", hdr_now ? L"HDR (Windows HDR is on for your monitor)"
                                  : L"HDR (Windows HDR is off - turn it on in Windows display settings to use HDR)",
               BS_GROUPBOX, 8, y, 612, 76, 0);
            mk(L"BUTTON", L"Follow the game: HDR games in HDR, SDR games in SDR (recommended)", R1, 20, y + 22, 590, 22, SD_HDR0);
            mk(L"BUTTON", L"Also turn SDR games into HDR (the mod's HDR output; a game's own HDR still comes first)", R, 20, y + 46, 590, 22, SD_HDR1);
            y += 84;
        }
        mk(L"BUTTON", L"Other", BS_GROUPBOX, 8, y, 612, 78, 0);
        mk(L"BUTTON", L"Keep the HUD sharp in generated frames (HUD-less)", BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP, 20, y + 22, 400, 22, SD_HUD);
        mk(L"STATIC", L"NVIDIA Reflex on the game's card:", 0, 20, y + 50, 220, 20, 0);
        mk(L"BUTTON", L"Off", R1, 245, y + 48, 60, 22, SD_RF0);
        mk(L"BUTTON", L"On (recommended)", R, 310, y + 48, 150, 22, SD_RF1);
        mk(L"BUTTON", L"On + boost", R, 465, y + 48, 120, 22, SD_RF2);
        y += 90;
        mk(L"BUTTON", L"Install", BS_DEFPUSHBUTTON | WS_TABSTOP | WS_GROUP, 400, y, 105, 30, SD_OK);
        mk(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 515, y, 105, 30, SD_CANCEL);

        const settings &s = g_sd->s;
        if (g_sd->have_ini) CheckRadioButton(h, SD_KEEP, SD_APPLY, s.apply ? SD_APPLY : SD_KEEP);
        CheckRadioButton(h, SD_MID, SD_HIGH, s.card == 0 ? SD_MID : SD_HIGH);
        CheckDlgButton(h, s.scaler == 0 ? SD_SC0 : s.scaler == 1 ? SD_SC1 : s.scaler == 2 ? SD_SC2 : SD_SC3, BST_CHECKED);
        CheckDlgButton(h, s.fg == 0 ? SD_FG0 : s.fg == 1 ? SD_FG1 : s.fg == 3 ? SD_FG3 : SD_FG4, BST_CHECKED);
        SendMessageW(cb, CB_SETCURSEL, (WPARAM)(std::max(2, std::min(4, s.mult)) - 2), 0);
        CheckDlgButton(h, SD_CAPON, s.cap_on ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemInt(h, SD_CAP, (UINT)s.cap, FALSE);
        CheckDlgButton(h, SD_HUD, s.hudless ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(h, s.reflex == 0 ? SD_RF0 : s.reflex == 1 ? SD_RF1 : SD_RF2, BST_CHECKED);
        CheckRadioButton(h, SD_HDR0, SD_HDR1, s.hdr ? SD_HDR1 : SD_HDR0);
        sd_sync(h);
        RECT r{ 0, 0, S(628), S(y + 42) };
        AdjustWindowRect(&r, WS_CAPTION | WS_SYSMENU, FALSE);
        SetWindowPos(h, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
        return 0;
    }
    case WM_COMMAND:
    {
        const int id = LOWORD(wp);
        if (id == SD_OK)
        {
            settings &s = g_sd->s;
            s.apply = !g_sd->have_ini || IsDlgButtonChecked(h, SD_APPLY) == BST_CHECKED;
            s.card = IsDlgButtonChecked(h, SD_HIGH) == BST_CHECKED ? 1 : 0;
            s.scaler = IsDlgButtonChecked(h, SD_SC0) ? 0 : IsDlgButtonChecked(h, SD_SC1) ? 1 : IsDlgButtonChecked(h, SD_SC2) ? 2 : 3;
            s.fg = IsDlgButtonChecked(h, SD_FG0) ? 0 : IsDlgButtonChecked(h, SD_FG1) ? 1 : IsDlgButtonChecked(h, SD_FG3) ? 3 : 4;
            s.mult = (s.fg == 1) ? 2 : (int)SendDlgItemMessageW(h, SD_MULT, CB_GETCURSEL, 0, 0) + 2;
            s.cap_on = IsDlgButtonChecked(h, SD_CAPON) == BST_CHECKED;
            BOOL got = FALSE;
            const UINT c = GetDlgItemInt(h, SD_CAP, &got, FALSE);
            s.cap = got ? (int)std::max(20u, std::min(240u, c)) : suggest_cap(s.card, s.mult);
            s.hudless = IsDlgButtonChecked(h, SD_HUD) == BST_CHECKED;
            s.reflex = IsDlgButtonChecked(h, SD_RF0) ? 0 : IsDlgButtonChecked(h, SD_RF2) ? 2 : 1;
            s.hdr = IsDlgButtonChecked(h, SD_HDR1) == BST_CHECKED ? 1 : 0;
            g_sd->ok = true;
            DestroyWindow(h);
            return 0;
        }
        if (id == SD_CANCEL) { DestroyWindow(h); return 0; }
        if (id == SD_MID || id == SD_HIGH || (id == SD_MULT && HIWORD(wp) == CBN_SELCHANGE) ||
            id == SD_FG3 || id == SD_FG4 || id == SD_FG1 || id == SD_FG0)
            sd_suggest(h);
        sd_sync(h);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        g_sd->done = true;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Shows the settings window; false = cancelled.
bool ask_settings(HWND owner, const std::wstring &t, settings &out)
{
    static bool reg = false;
    if (!reg)
    {
        WNDCLASSEXW wc{ sizeof wc };
        wc.lpfnWndProc = sd_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"AtomicMGPUSettings";
        RegisterClassExW(&wc);
        reg = true;
    }
    sd_state st;
    st.have_ini = exists(join(t, L"mgpu.ini"));
    st.s.apply = !st.have_ini;
    st.s.cap = suggest_cap(st.s.card, st.s.mult);
    g_sd = &st;
    RECT orc{};
    GetWindowRect(owner, &orc);
    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME, L"AtomicMGPUSettings", L"Atomic MGPU Bridge - your settings",
                             WS_CAPTION | WS_SYSMENU, orc.left + S(40), orc.top + S(30), S(640), S(560),
                             owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    EnableWindow(owner, FALSE);
    ShowWindow(h, SW_SHOW);
    MSG m;
    while (!st.done && GetMessageW(&m, nullptr, 0, 0) > 0)
    {
        if (IsDialogMessageW(h, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    g_sd = nullptr;
    if (st.ok) out = st.s;
    return st.ok;
}

struct install_result { bool ok = false; std::wstring summary; };
install_result g_last_install;

void install(const std::wstring &t, const settings *cfg = nullptr)
{
    std::wstring log = L"Installing into " + t + L"\r\n";
    if (!can_write(t))   // IN8 (the window offers a restart before getting here; scripts get this line)
        log += L"  This folder needs administrator rights - run the installer as administrator.\r\n";
    std::vector<std::wstring> placed;
    bool ok = copy_in(join(g_src, kAddon), join(t, kAddon), log, &placed, kAddon);
    for (auto &p : kPlain)
        if (exists(join(g_src, p[0]))) ok &= copy_in(join(g_src, p[0]), join(t, p[1]), log, &placed, p[1]);
    ok &= copy_in(join(g_src, kShader), join(t, kShader), log, &placed, kShader);

    // mgpu.ini: never overwrite the user's settings.
    if (!exists(join(t, L"mgpu.ini"))) ok &= copy_in(join(g_src, L"mgpu.ini"), join(t, L"mgpu.ini"), log, &placed, L"mgpu.ini");
    else if (copy_in(join(g_src, L"mgpu.ini"), join(t, L"mgpu.ini.new"), log, &placed, L"mgpu.ini.new"))
        log += L"  (your mgpu.ini was kept; the new default is beside it as mgpu.ini.new)\r\n";

    // ReShade, only when the game has none - or ours, when it is out of date.
    reshade_at rs = find_reshade(t);
    std::wstring rs_ours = manifest_value(t, L"reshade");
    const bool have_bundled = exists(join(g_src, kReShade));
    bool rs_failed = false;   // IN8: ours could not be copied (say so, not "get it from reshade.me")
    if (rs.file.empty() && have_bundled)
    {
        const wchar_t *as = exists(join(t, L"dxgi.dll")) ? L"d3d12.dll" : L"dxgi.dll";
        if (exists(join(t, as)))
            log += L"  ReShade NOT installed: both dxgi.dll and d3d12.dll are taken by other mods.\r\n";
        else if (copy_in(join(g_src, kReShade), join(t, as), log, nullptr, as))
        {
            rs_ours = as;
            if (exists(join(g_src, kReShadeL)))
                copy_in(join(g_src, kReShadeL), join(t, L"ReShade-LICENSE.txt"), log, &placed, L"ReShade-LICENSE.txt");
        }
        else { rs_failed = true; ok = false; }
    }
    else if (!rs_ours.empty() && rs.file == rs_ours && have_bundled && !same_file(join(t, rs_ours), join(g_src, kReShade)))
        copy_in(join(g_src, kReShade), join(t, rs_ours), log, nullptr, rs_ours + L" (ReShade updated)");
    else if (rs.file.empty())
        log += L"  ReShade is not installed and this release does not carry it - get the add-on version from reshade.me.\r\n";
    else
        // IN4: say it - a silent skip here looked like "did not install dxgi.dll".
        log += L"  ReShade already there as " + rs.file + L" (" + rs.ver + L", " + join(t, rs.file) +
               (rs_ours == rs.file ? L", put in by this installer) - left as it is.\r\n" : L") - left as it is.\r\n");

    // ReShade.ini: create it, or make sure it looks in reshade-shaders\Shaders.
    const std::wstring rini = join(t, L"ReShade.ini");
    std::wstring ini_ours = manifest_value(t, L"reshade_ini");
    if (!exists(rini))
    {
        const bool w = WritePrivateProfileStringW(L"GENERAL", L"EffectSearchPaths", L".\\reshade-shaders\\Shaders\\**", rini.c_str()) &&
                       WritePrivateProfileStringW(L"GENERAL", L"TextureSearchPaths", L".\\reshade-shaders\\Textures\\**", rini.c_str());
        if (w) { log += L"  ReShade.ini (created)\r\n"; ini_ours = L"1"; }
        else { log += L"  ReShade.ini could NOT be created (error " + std::to_wstring(GetLastError()) + L")\r\n"; ok = false; }
    }
    else
    {
        wchar_t esp[2048] = {};
        GetPrivateProfileStringW(L"GENERAL", L"EffectSearchPaths", L"", esp, 2048, rini.c_str());
        std::wstring e = esp;
        if (lower(e).find(L"reshade-shaders\\shaders") == std::wstring::npos && lower(e).find(L"reshade-shaders/shaders") == std::wstring::npos)
        {
            e = e.empty() ? L".\\reshade-shaders\\Shaders\\**" : e + L",.\\reshade-shaders\\Shaders\\**";
            WritePrivateProfileStringW(L"GENERAL", L"EffectSearchPaths", e.c_str(), rini.c_str());
            log += L"  ReShade.ini: added reshade-shaders\\Shaders to EffectSearchPaths\r\n";
        }
        else log += L"  ReShade.ini already there and already looks in reshade-shaders\\Shaders.\r\n";
    }
    // RS3. ReShade's built-in "Effect Runtime Sync" add-on only acts when
    // [ADDON] SyncEffectRuntimes=1; with the bridge's picture window it then
    // loops game <-> bridge until the game crashes. Set it to 0 (the add-on
    // itself stays available). The bridge add-on also does this at start-up.
    if (exists(rini))
    {
        wchar_t sv[16] = {};
        GetPrivateProfileStringW(L"ADDON", L"SyncEffectRuntimes", L"", sv, 16, rini.c_str());
        if (sv[0] != L'0')
        {
            if (WritePrivateProfileStringW(L"ADDON", L"SyncEffectRuntimes", L"0", rini.c_str()))
                log += L"  ReShade.ini: SyncEffectRuntimes=0 (ReShade's runtime sync loops with the bridge)\r\n";
            else { log += L"  ReShade.ini: could NOT set SyncEffectRuntimes=0\r\n"; ok = false; }
        }
    }
    make_dirs(join(t, L"mgpu"));
    const bool settings_ok = cfg != nullptr && cfg->apply && apply_settings(t, *cfg, log);   // IN2
    if (cfg != nullptr && cfg->apply && !settings_ok) ok = false;

    // What to remove on uninstall - merged with the record of earlier installs,
    // so an update does not forget what the first install placed.
    {
        const std::wstring old = read_text(join(t, kManifest));
        size_t a = 0;
        while (a < old.size())
        {
            size_t e = old.find(L'\n', a);
            if (e == std::wstring::npos) e = old.size();
            std::wstring line = old.substr(a, e - a);
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            if (line.rfind(L"file=", 0) == 0)
            {
                const std::wstring f = line.substr(5);
                if (std::find_if(placed.begin(), placed.end(), [&](const std::wstring &x) { return lower(x) == lower(f); }) == placed.end())
                    placed.push_back(f);
            }
            a = e + 1;
        }
    }
    std::wstring m = L"; Atomic MGPU Bridge installer - what it placed in this folder\r\n";
    for (auto &p : placed) m += L"file=" + p + L"\r\n";
    if (!rs_ours.empty()) m += L"reshade=" + rs_ours + L"\r\n";
    if (!ini_ours.empty()) m += L"reshade_ini=1\r\n";
    if (!write_text(join(t, kManifest), m)) { log += L"  could NOT write mgpu\\atomic_install.txt (the uninstall record)\r\n"; ok = false; }

    log += ok ? L"\r\nDone.\r\n" : L"\r\nFinished with errors - see above.\r\n";
    {   // IN4: the confirmation window's text
        const reshade_at now = find_reshade(t);
        g_last_install.ok = ok;
        g_last_install.summary = L"Installed into:\n" + t + L"\n\nReShade: " +
            (!now.file.empty() ? now.file + L" (" + now.ver + L")"
             : rs_failed ? std::wstring(L"NOT installed - it could not be copied into the game folder")
                         : std::wstring(L"NOT installed - get the add-on version from reshade.me")) +
            (exists(join(t, kAmdDst)) ? L"\nAMD frame generation DLL: in mgpu" : L"") +
            (!(cfg && cfg->apply) ? L"\nSettings: your mgpu.ini was kept"
             : settings_ok ? L"\nSettings: written to mgpu.ini" : L"\nSettings: NOT written");
    }
    tell_explorer(t);   // IN5
    log += L"\r\n--- check after install ---\r\n" + check(t);
    SetWindowTextW(g_status, log.c_str());
}

bool g_quiet = false;   // command-line mode: no questions, settings kept

// IN7: delete a folder and everything in it (the bridge's own capture and
// benchmark folders - only when the user says so).
void delete_tree(const std::wstring &d)
{
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(join(d, L"*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE)
    {
        do
        {
            const std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            const std::wstring p = join(d, n);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                delete_tree(p);
            else
            {
                SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(p.c_str());
            }
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    RemoveDirectoryW(d.c_str());
}
// What is still in a folder, for the log ("crash.txt, captures\, ...").
std::wstring dir_listing(const std::wstring &d)
{
    std::wstring names;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(join(d, L"*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return names;
    do
    {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (!names.empty()) names += L", ";
        names += n + ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? L"\\" : L"");
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return names;
}

void uninstall(const std::wstring &t)
{
    if (!g_quiet && MessageBoxW(g_wnd, L"Remove Atomic MGPU Bridge from this game?", L"Atomic MGPU Bridge",
                                MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    const bool drop_ini = !g_quiet &&
        MessageBoxW(g_wnd, L"Also remove your settings (mgpu.ini)?\r\n\r\nNo keeps them for next time.",
                    L"Atomic MGPU Bridge", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
    // IN7: the bridge's own screenshots, videos and benchmark results live in mgpu\ too.
    const std::wstring mg = join(t, L"mgpu");
    const bool have_media = is_dir(join(mg, L"captures")) || is_dir(join(mg, L"benchmarks"));
    const bool drop_media = have_media && !g_quiet &&
        MessageBoxW(g_wnd, L"Also delete your screenshots, videos and benchmark results?\r\n\r\n"
                           L"(mgpu\\captures and mgpu\\benchmarks in the game folder. No keeps them.)",
                    L"Atomic MGPU Bridge", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
    std::wstring log = L"Removing from " + t + L"\r\n";
    std::vector<std::wstring> files;
    const std::wstring m = read_text(join(t, kManifest));
    std::wstring rs = manifest_value(t, L"reshade");
    const bool ini_ours = manifest_value(t, L"reshade_ini") == L"1";
    size_t at = 0;
    while (at < m.size())
    {
        size_t e = m.find(L'\n', at);
        if (e == std::wstring::npos) e = m.size();
        std::wstring line = m.substr(at, e - at);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.rfind(L"file=", 0) == 0) files.push_back(line.substr(5));
        at = e + 1;
    }
    if (files.empty())   // installed by hand: the mod's own files only, never ReShade
        files = { kAddon, L"gpu1.ini", L"ReShade2.ini", kShader };
    for (auto &f : files)
    {
        if (lower(f) == L"mgpu.ini" && !drop_ini) continue;
        if (DeleteFileW(join(t, f).c_str())) log += L"  removed " + f + L"\r\n";
    }
    if (drop_ini && DeleteFileW(join(t, L"mgpu.ini").c_str())) log += L"  removed mgpu.ini\r\n";
    if (!rs.empty() && DeleteFileW(join(t, rs).c_str())) log += L"  removed ReShade (" + rs + L")\r\n";
    if (ini_ours && DeleteFileW(join(t, L"ReShade.ini").c_str())) log += L"  removed ReShade.ini\r\n";
    DeleteFileW(join(t, kManifest).c_str());
    // IN7: what the add-on itself writes while the game runs. REPORTED 2026-11-15:
    // Uninstall left the mgpu folder behind - the crash catcher creates
    // mgpu\crash.txt at every launch, so the folder was never empty.
    for (const wchar_t *f : { L"mgpu\\crash.txt", L"mgpu\\mgpu.ini.new", L"mgpu.ini.new" })
        if (DeleteFileW(join(t, f).c_str())) log += std::wstring(L"  removed ") + f + L"\r\n";
    if (drop_media)
    {
        for (const wchar_t *d : { L"captures", L"benchmarks" })
            if (is_dir(join(mg, d))) { delete_tree(join(mg, d)); log += std::wstring(L"  removed mgpu\\") + d + L"\\\r\n"; }
    }
    if (!rs.empty())   // ReShade was ours: its log files go with it
        for (const wchar_t *f : { L"ReShade.log", L"ReShade.log1", L"ReShade.log2" })
            if (DeleteFileW(join(t, f).c_str())) log += std::wstring(L"  removed ") + f + L"\r\n";
    RemoveDirectoryW(join(t, L"reshade-shaders\\Shaders").c_str());   // only if empty
    RemoveDirectoryW(join(t, L"reshade-shaders").c_str());
    RemoveDirectoryW(mg.c_str());
    if (is_dir(mg))
        log += L"  kept the mgpu folder - it still holds: " + dir_listing(mg) +
               (exists(join(mg, L"nvngx_dlssnr.dll")) && !manifest_has_file(t, kNrDst) ? L" (nvngx_dlssnr.dll is yours)" : L"") +
               L"\r\n";
    else
        log += L"  removed the mgpu folder\r\n";
    if (rs.empty() && !find_reshade(t).file.empty()) log += L"  ReShade left in place (it was not installed by this installer)\r\n";
    tell_explorer(t);   // IN5
    log += L"\r\nDone.\r\n";
    SetWindowTextW(g_status, log.c_str());
}

// --------------------------------------------------------------------- UI
std::wstring selected_dir()
{
    if (g_sel < 0 || g_sel >= (int)g_games.size()) return L"";
    game &g = g_games[g_sel];
    if (!g.searched)
    {
        g.searched = true;
        std::wstring best;
        long long score = -1;
        SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        find_exe(g.root, 0, best, score);
        g.exe_dir = best.empty() ? g.root : best;
    }
    return g.exe_dir;
}

void refresh()
{
    const std::wstring t = selected_dir();
    EnableWindow(g_btn_install, !t.empty());
    EnableWindow(g_btn_uninstall, !t.empty() && (exists(join(t, kAddon)) || exists(join(t, kManifest))));
    EnableWindow(g_btn_open, !t.empty());
    EnableWindow(g_btn_nr, !t.empty());
    if (t.empty()) return;
    SetWindowTextW(g_status, check(t).c_str());
}

void fill_list()
{
    ListView_DeleteAllItems(g_list);
    for (size_t i = 0; i < g_games.size(); ++i)
    {
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        it.pszText = const_cast<wchar_t *>(g_games[i].name.c_str());
        ListView_InsertItem(g_list, &it);
    }
}

void browse()
{
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW of{};
    of.lStructSize = sizeof of;
    of.hwndOwner = g_wnd;
    of.lpstrFilter = L"Game program (*.exe)\0*.exe\0";
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH;
    of.lpstrTitle = L"Choose the game's .exe (for Unreal games: ...\\Binaries\\Win64\\...-Win64-Shipping.exe)";
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&of)) return;
    game g;
    g.root = g.exe_dir = dir_of(file);
    const size_t k = std::wstring(file).find_last_of(L'\\');
    g.name = std::wstring(file).substr(k + 1) + L"  (" + g.root + L")";
    g.searched = true;
    g_games.insert(g_games.begin(), g);
    fill_list();
    g_sel = 0;
    ListView_SetItemState(g_list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    refresh();
}

// IN8: a protected folder (C:\Program Files ...) - offer to restart as
// administrator, with the same folder chosen. true = the caller stops here.
bool g_admin = false;
std::wstring g_preselect;   // "/select <folder>" from that restart
bool elevate_if_needed(HWND h, const std::wstring &t, const wchar_t *what)
{
    if (can_write(t)) return false;
    if (g_admin)
    {
        MessageBoxW(h, (L"Windows refuses changes to this folder, even as administrator:\n\n" + t +
                        L"\n\nIs the game (or its launcher) still running? Close it and try again.").c_str(),
                    L"Atomic MGPU Bridge - Setup", MB_OK | MB_ICONERROR);
        return true;
    }
    const std::wstring msg = L"This game is in a folder Windows protects:\n\n" + t + L"\n\nTo " + what +
        L" there, the installer needs administrator rights.\n\nRestart the installer as administrator now? "
        L"Windows will ask for permission, and this game stays selected.";
    if (MessageBoxW(h, msg.c_str(), L"Atomic MGPU Bridge - Setup", MB_YESNO | MB_ICONINFORMATION) != IDYES)
    {
        SetWindowTextW(g_status, (L"Nothing was changed: this folder needs administrator rights.\r\n\r\n" + check(t)).c_str());
        return true;
    }
    wchar_t self[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
    std::wstring d = t;
    while (d.size() > 3 && d.back() == L'\\') d.pop_back();   // a trailing \ would swallow the closing quote
    const std::wstring params = L"/select \"" + d + L"\"";
    SHELLEXECUTEINFOW se{};
    se.cbSize = sizeof se;
    se.fMask = SEE_MASK_NOASYNC;
    se.hwnd = h;
    se.lpVerb = L"runas";
    se.lpFile = self;
    se.lpParameters = params.c_str();
    se.lpDirectory = g_src.c_str();
    se.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&se)) { DestroyWindow(h); return true; }
    const DWORD e = GetLastError();
    SetWindowTextW(g_status, ((e == ERROR_CANCELLED
        ? std::wstring(L"Not restarted: the Windows permission prompt was declined.")
        : L"Could not restart as administrator (error " + std::to_wstring(e) + L").") +
        L"\r\n\r\nRight-click AtomicMGPU-Setup.exe and choose \"Run as administrator\".\r\n\r\n" + check(t)).c_str());
    return true;
}

HICON make_window_icon(int n)
{
    BITMAPV5HEADER bh{};
    bh.bV5Size = sizeof bh; bh.bV5Width = n; bh.bV5Height = -n; bh.bV5Planes = 1; bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS;
    bh.bV5RedMask = 0x00FF0000; bh.bV5GreenMask = 0x0000FF00; bh.bV5BlueMask = 0x000000FF; bh.bV5AlphaMask = 0xFF000000;
    void *bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP c = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO *>(&bh), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!c || !bits) return nullptr;
    tr_render(n, 60, 200, 90, static_cast<unsigned *>(bits));   // green dot: ready
    HBITMAP m = CreateBitmap(n, n, 1, 1, nullptr);
    ICONINFO ii{ TRUE, 0, 0, m, c };
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(c); DeleteObject(m);
    return ic;
}

int g_dpi = 96;
int S(int v) { return MulDiv(v, g_dpi, 96); }

LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        auto mk = [&](const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int hh, int id, DWORD ex = 0) {
            HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(hh), h,
                                     (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
            return c;
        };
        mk(L"STATIC", L"1. Choose the game:", 0, 12, 12, 360, 20, 0);
        g_list = mk(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOCOLUMNHEADER | WS_TABSTOP,
                    12, 34, 360, 350, ID_LIST, WS_EX_CLIENTEDGE);
        ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        LVCOLUMNW col{};
        col.mask = LVCF_WIDTH;
        col.cx = S(336);
        ListView_InsertColumn(g_list, 0, &col);
        mk(L"BUTTON", L"Browse for another game's .exe...", BS_PUSHBUTTON | WS_TABSTOP, 12, 392, 360, 30, ID_BROWSE);
        g_btn_nr = mk(L"BUTTON", L"Add my nvngx_dlssnr.dll...", BS_PUSHBUTTON | WS_TABSTOP, 12, 428, 360, 30, ID_NRDLL);
        EnableWindow(g_btn_nr, FALSE);
        mk(L"STATIC", L"2. Check, then install:", 0, 386, 12, 480, 20, 0);
        g_status = mk(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 386, 34, 520, 386, ID_STATUS, WS_EX_CLIENTEDGE);
        SendMessageW(g_status, WM_SETFONT, (WPARAM)g_mono, TRUE);
        g_btn_install   = mk(L"BUTTON", L"Install / Update", BS_DEFPUSHBUTTON | WS_TABSTOP, 386, 428, 170, 30, ID_INSTALL);
        g_btn_uninstall = mk(L"BUTTON", L"Uninstall", BS_PUSHBUTTON | WS_TABSTOP, 564, 428, 140, 30, ID_UNINSTALL);
        g_btn_open      = mk(L"BUTTON", L"Open game folder", BS_PUSHBUTTON | WS_TABSTOP, 712, 428, 194, 30, ID_OPEN);
        EnableWindow(g_btn_install, FALSE); EnableWindow(g_btn_uninstall, FALSE); EnableWindow(g_btn_open, FALSE);

        std::wstring intro;
        if (!exists(join(g_src, kAddon)))
            intro = L"The release files are not beside this installer.\r\n\r\nUnpack the whole release zip and run "
                    L"AtomicMGPU-Setup.exe from that folder.";
        else
        {
            scan_steam();
            fill_list();
            intro = L"Atomic MGPU Bridge - installer\r\n\r\nPick a game on the left (Steam games are listed; "
                    L"\"Browse...\" for any other), then Install.\r\n\r\nThe game must be closed while installing.\r\n"
                    L"\r\nRecommended setup: the card that renders the game runs headless (no monitor), and your "
                    L"monitor is plugged into the second card, which does the neural work, frame generation and the "
                    L"display.\r\n";
            if (g_games.empty() && g_preselect.empty()) intro += L"\r\nNo Steam games were found - use \"Browse...\".\r\n";
            if (!exists(join(g_src, kReShade)))
                intro += L"\r\nThis release does not carry ReShade: install ReShade with add-on support from reshade.me "
                         L"into the game first.\r\n";
        }
        SetWindowTextW(g_status, intro.c_str());
        if (!g_preselect.empty() && exists(join(g_src, kAddon)))
        {   // IN8: restarted as administrator for this folder - choose it again
            int at = -1;
            const std::wstring want = lower(g_preselect);
            for (size_t i = 0; i < g_games.size() && at < 0; ++i)
            {
                const std::wstring r = lower(g_games[i].root);
                if (want == r || (want.size() > r.size() && want.compare(0, r.size(), r) == 0 && want[r.size()] == L'\\'))
                    at = (int)i;
            }
            if (at < 0)
            {
                game g;
                g.root = g_preselect;
                g.name = g_preselect;
                g_games.insert(g_games.begin(), g);
                fill_list();
                at = 0;
            }
            g_games[at].exe_dir = g_preselect;
            g_games[at].searched = true;
            g_sel = at;
            ListView_SetItemState(g_list, at, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(g_list, at, FALSE);
            refresh();
            SetWindowTextW(g_status, (std::wstring(g_admin ? L"Running as administrator" : L"Game folder chosen") +
                                      L" - press Install / Update (or Uninstall) again.\r\n\r\n" + check(g_preselect)).c_str());
        }
        return 0;
    }
    case WM_NOTIFY:
    {
        const NMHDR *n = reinterpret_cast<NMHDR *>(lp);
        if (n->idFrom == ID_LIST && n->code == LVN_ITEMCHANGED)
        {
            const NMLISTVIEW *v = reinterpret_cast<NMLISTVIEW *>(lp);
            if ((v->uNewState & LVIS_SELECTED) && !(v->uOldState & LVIS_SELECTED))
            {
                g_sel = v->iItem;
                SetWindowTextW(g_status, L"Looking for the game's .exe...");
                UpdateWindow(g_status);
                refresh();
            }
        }
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case ID_BROWSE: browse(); return 0;
        case ID_INSTALL: { const std::wstring t = selected_dir(); settings cfg;
            if (!t.empty() && elevate_if_needed(h, t, L"install")) return 0;   // IN8
            if (!t.empty() && ask_settings(h, t, cfg))
            {
                install(t, &cfg);
                // IN6: the user's own nvngx_dlssnr.dll - offer a copy found on this PC.
                {
                    std::wstring more;
                    if (!exists(join(t, kNrDst)) && offer_dlssnr(h, t, false, more))
                    {
                        const int n = GetWindowTextLengthW(g_status);
                        std::wstring cur(n + 1, L'\0');
                        GetWindowTextW(g_status, &cur[0], n + 1);
                        cur.resize(n);
                        SetWindowTextW(g_status, (cur + L"\r\n" + more + L"\r\n--- check ---\r\n" + check(t)).c_str());
                        tell_explorer(t);
                    }
                    g_last_install.summary += exists(join(t, kNrDst))
                        ? L"\nnvngx_dlssnr.dll: in mgpu"
                        : L"\n\nLast step: your own nvngx_dlssnr.dll is not in the mgpu folder yet - use \"Add my nvngx_dlssnr.dll...\" "
                          L"or copy it there yourself (Open game folder).";
                }
                // IN4: say how it went: [Exit] or [Go back] (to the check, another game, or Uninstall).
                TASKDIALOG_BUTTON btn[2] = { { 100, L"Exit" }, { 101, L"Go back" } };
                TASKDIALOGCONFIG tc{};
                tc.cbSize = sizeof tc;
                tc.hwndParent = h;
                tc.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW;
                tc.pszWindowTitle = L"Atomic MGPU Bridge - Setup";
                tc.pszMainIcon = g_last_install.ok ? TD_INFORMATION_ICON : TD_WARNING_ICON;
                tc.pszMainInstruction = g_last_install.ok ? L"Installation successful." : L"Installation finished with errors.";
                const std::wstring body = g_last_install.ok ? g_last_install.summary
                                        : g_last_install.summary + L"\n\nGo back to see what failed.";
                tc.pszContent = body.c_str();
                tc.cButtons = 2;
                tc.pButtons = btn;
                tc.nDefaultButton = g_last_install.ok ? 100 : 101;
                int pressed = 0;
                if (SUCCEEDED(TaskDialogIndirect(&tc, &pressed, nullptr, nullptr)) && pressed == 100)
                {
                    DestroyWindow(h);
                    return 0;
                }
            }
            EnableWindow(g_btn_uninstall, !t.empty() && (exists(join(t, kAddon)) || exists(join(t, kManifest)))); return 0; }
        case ID_UNINSTALL: { const std::wstring t = selected_dir();
            if (!t.empty() && elevate_if_needed(h, t, L"uninstall")) return 0;   // IN8
            if (!t.empty()) uninstall(t);
            EnableWindow(g_btn_uninstall, !t.empty() && (exists(join(t, kAddon)) || exists(join(t, kManifest)))); return 0; }
        case ID_NRDLL: { const std::wstring t = selected_dir();
            if (t.empty()) return 0;
            if (!exists(join(t, kNrDst)) && elevate_if_needed(h, t, L"copy nvngx_dlssnr.dll")) return 0;   // IN8
            std::wstring log = L"nvngx_dlssnr.dll for " + t + L"\r\n";
            if (exists(join(t, kNrDst))) log += L"  already in the mgpu folder.\r\n";
            else if (offer_dlssnr(h, t, true, log)) tell_explorer(t);
            else log += L"  not copied.\r\n";
            SetWindowTextW(g_status, (log + L"\r\n--- check ---\r\n" + check(t)).c_str());
            return 0; }
        case ID_OPEN: { const std::wstring t = selected_dir();
            if (!t.empty()) { make_dirs(join(t, L"mgpu")); ShellExecuteW(h, L"open", t.c_str(), nullptr, nullptr, SW_SHOWNORMAL); } return 0; }
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    INITCOMMONCONTROLSEX icc{ sizeof icc, ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    wchar_t self[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
    g_src = dir_of(self);

    // Command line, for scripts and testing:  /check|/install|/uninstall "<game exe folder>"
    // The result goes to setup-result.txt beside this exe. Uninstall keeps mgpu.ini.
    {
        int argc = 0;
        wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        g_admin = is_admin();   // IN8
        if (argv != nullptr && argc >= 3 && lower(argv[1]) == L"/select") g_preselect = argv[2];   // IN8
        if (argv != nullptr && argc >= 3)
        {
            const std::wstring op = lower(argv[1]), dir = argv[2];
            if (op == L"/check" || op == L"/install" || op == L"/uninstall")
            {
                HWND dummy = CreateWindowExW(0, L"EDIT", L"", ES_MULTILINE, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
                g_status = dummy;
                g_quiet = true;
                settings rec;   // IN2: "/install <dir> recommended" also writes the recommended settings
                rec.cap = suggest_cap(rec.card, rec.mult);
                rec.apply = argc >= 4 && lower(argv[3]) == L"recommended";
                if (op == L"/install") install(dir, &rec);
                else if (op == L"/uninstall") uninstall(dir);
                else SetWindowTextW(g_status, check(dir).c_str());
                const int n = GetWindowTextLengthW(g_status);
                std::wstring txt(n + 1, L'\0');
                GetWindowTextW(g_status, &txt[0], n + 1);
                txt.resize(n);
                write_text(join(g_src, L"setup-result.txt"), txt);
                LocalFree(argv);
                return 0;
            }
        }
        if (argv) LocalFree(argv);
    }

    HDC dc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW mono = ncm.lfMessageFont;
    wcscpy_s(mono.lfFaceName, L"Consolas");
    g_mono = CreateFontIndirectW(&mono);

    WNDCLASSEXW wc{ sizeof wc };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"AtomicMGPUSetup";
    wc.hIcon = make_window_icon(GetSystemMetrics(SM_CXICON));
    wc.hIconSm = make_window_icon(GetSystemMetrics(SM_CXSMICON));
    RegisterClassExW(&wc);

    RECT r{ 0, 0, S(918), S(470) };
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    g_wnd = CreateWindowExW(0, wc.lpszClassName, g_admin ? L"Atomic MGPU Bridge - Setup (administrator)" : L"Atomic MGPU Bridge - Setup", style, CW_USEDEFAULT, CW_USEDEFAULT,
                            r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    ShowWindow(g_wnd, show);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0)
    {
        if (IsDialogMessageW(g_wnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    CoUninitialize();
    return 0;
}
