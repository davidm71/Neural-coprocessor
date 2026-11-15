// MGPU Bridge - ID1. See dllid.hpp.
#include "dllid.hpp"

#include <wintrust.h>
#include <softpub.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <thread>

#include "diag.hpp"
#include "sl_probe.hpp"

namespace mgpu::dllid
{
namespace
{
std::atomic<int> g_done[5];   // once per slot per launch
const char *const kSlot[5] = { "NR", "SR", "FG", "AMD FSR", "AMD rebuilt" };
const char *const kWant[5] = { "NVIDIA NGX feature", "NVIDIA NGX feature", "NVIDIA NGX feature or ANR FG",
                               "AMD FidelityFX API", "AMD FidelityFX API" };

// Loaded at run time, so the build needs no new libraries.
typedef LONG (WINAPI *pf_wvt)(HWND, GUID *, LPVOID);
typedef CRYPT_PROVIDER_DATA *(WINAPI *pf_prov)(HANDLE);
typedef CRYPT_PROVIDER_SGNR *(WINAPI *pf_sgnr)(CRYPT_PROVIDER_DATA *, DWORD, BOOL, DWORD);
typedef DWORD (WINAPI *pf_name)(PCCERT_CONTEXT, DWORD, DWORD, void *, LPWSTR, DWORD);

// "signed by <name>", "signed by <name> - SIGNATURE NOT VALID (0x...)", "not signed",
// or "no signature in the file" for driver files that may be catalog-signed.
void signer(const wchar_t *path, char *out, size_t n)
{
    snprintf(out, n, "signature not checked");
    HMODULE wt = LoadLibraryExW(L"wintrust.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE cr = LoadLibraryExW(L"crypt32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    pf_wvt wvt = wt ? (pf_wvt)(void *)GetProcAddress(wt, "WinVerifyTrust") : nullptr;
    pf_prov prov = wt ? (pf_prov)(void *)GetProcAddress(wt, "WTHelperProvDataFromStateData") : nullptr;
    pf_sgnr sg = wt ? (pf_sgnr)(void *)GetProcAddress(wt, "WTHelperGetProvSignerFromChain") : nullptr;
    pf_name nm = cr ? (pf_name)(void *)GetProcAddress(cr, "CertGetNameStringW") : nullptr;
    if (wvt == nullptr || prov == nullptr || sg == nullptr || nm == nullptr) return;

    WINTRUST_FILE_INFO fi{};
    fi.cbStruct = sizeof fi;
    fi.pcwszFilePath = path;
    WINTRUST_DATA wd{};
    wd.cbStruct = sizeof wd;
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;          // offline: no certificate revocation lookup
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;     // never goes to the network
    GUID act = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG r = wvt((HWND)INVALID_HANDLE_VALUE, &act, &wd);

    wchar_t who[160] = {};
    if (wd.hWVTStateData != nullptr)
        if (CRYPT_PROVIDER_DATA *pd = prov(wd.hWVTStateData))
            if (CRYPT_PROVIDER_SGNR *s = sg(pd, 0, FALSE, 0))
                if (s->csCertChain > 0 && s->pasCertChain != nullptr && s->pasCertChain[0].pCert != nullptr)
                    nm(s->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, who, 160);
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    (void)wvt((HWND)INVALID_HANDLE_VALUE, &act, &wd);

    char w8[200] = {};
    WideCharToMultiByte(CP_UTF8, 0, who, -1, w8, (int)sizeof w8, nullptr, nullptr);
    if (r == 0 && w8[0]) snprintf(out, n, "signed by %s", w8);
    else if (w8[0])      snprintf(out, n, "signed by %s - SIGNATURE NOT VALID (0x%08lX): the file was changed after signing",
                                  w8, (unsigned long)r);
    else if ((unsigned long)r == 0x800B0100ul)   // TRUST_E_NOSIGNATURE
    {
        wchar_t low[MAX_PATH * 2] = {};
        wcsncpy_s(low, path, _TRUNCATE);
        for (wchar_t *c = low; *c; ++c) *c = (wchar_t)towlower(*c);
        const bool drv = wcsstr(low, L"\\driverstore\\") || wcsstr(low, L"\\system32\\") ||
                         wcsstr(low, L"\\programdata\\nvidia");
        snprintf(out, n, drv ? "no signature in the file (driver files can be signed by the driver package instead)"
                             : "not signed");
    }
    else snprintf(out, n, "signature unreadable (0x%08lX)", (unsigned long)r);
}
}   // namespace

bool is_anr(HMODULE m)
{
    return m != nullptr && GetProcAddress(m, "NRFG_PresentationControl") != nullptr;
}

const char *kind(HMODULE m)
{
    if (m == nullptr) return "unknown";
    if (is_anr(m)) return "ANR FG";
    if (GetProcAddress(m, "NVSDK_NGX_D3D12_CreateFeature") != nullptr) return "NVIDIA NGX feature";
    if (GetProcAddress(m, "ffxCreateContext") != nullptr) return "AMD FidelityFX API";
    return "unknown";
}

void check(int slot, HMODULE m)
{
    if (slot < 0 || slot > 4 || m == nullptr) return;
    int z = 0;
    if (!g_done[slot].compare_exchange_strong(z, 1)) return;

    // Everything that needs the module is read here; the thread gets copies.
    wchar_t path[MAX_PATH * 2] = {};
    if (GetModuleFileNameW(m, path, MAX_PATH * 2) == 0) return;
    char ver[32] = "";
    (void)mgpu::slprobe::file_version(m, ver, sizeof ver);
    if (ver[0] == 0) snprintf(ver, sizeof ver, "none");   // no version resource in the file
    const char *k = kind(m);
    const bool ok = (slot == 2) ? (strcmp(k, "NVIDIA NGX feature") == 0 || strcmp(k, "ANR FG") == 0)
                  : (slot <= 1) ? strcmp(k, "NVIDIA NGX feature") == 0
                                : strcmp(k, "AMD FidelityFX API") == 0;

    struct job { int slot; wchar_t path[MAX_PATH * 2]; char ver[32]; const char *kind; bool ok; };
    job *j = new job{};
    j->slot = slot;
    wcsncpy_s(j->path, path, _TRUNCATE);
    snprintf(j->ver, sizeof j->ver, "%s", ver);
    j->kind = k;
    j->ok = ok;
    std::thread([j]() {
        char sig[320];
        signer(j->path, sig, sizeof sig);
        char p8[MAX_PATH * 2] = {};
        WideCharToMultiByte(CP_UTF8, 0, j->path, -1, p8, (int)sizeof p8, nullptr, nullptr);
        char line[1100];
        snprintf(line, sizeof line, "[MGPU][ID1] %s: %s | version %s | %s | %s%s%s", kSlot[j->slot], p8, j->ver,
                 j->kind, sig, j->ok ? "" : " | NOT THE KIND OF FILE THIS SLOT EXPECTS (", j->ok ? "" : kWant[j->slot]);
        if (!j->ok) strncat_s(line, sizeof line, ")", _TRUNCATE);
        if (j->ok && strstr(sig, "NOT VALID") == nullptr) mgpu::diag::info(line);
        else mgpu::diag::warn(line);
        delete j;
    }).detach();
}
}   // namespace mgpu::dllid
