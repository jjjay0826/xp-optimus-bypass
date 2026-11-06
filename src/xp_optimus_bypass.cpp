// XP Optimus Bypass — a Vulkan implicit layer for X-Plane 12 on Optimus laptops without a MUX switch.
//
// Problem: on laptops whose built-in screen is wired to the Intel iGPU, NVIDIA's driver has to route every
// Vulkan frame from the GeForce to the iGPU. On the author's laptop (i5-10300H + GTX 1650, no MUX, no
// Windows CASO support) that path cost ~47 ms per frame under load, capping X-Plane at ~20 FPS.
//
// What this layer does (only inside X-Plane.exe):
//   * X-Plane renders into ordinary images owned by the layer (a "virtual swapchain").
//   * Each finished frame then takes one of two paths, chosen and switched inside the layer:
//       bypass: a worker thread copies the frame from the GeForce into system memory, and an Intel Vulkan
//               device the layer creates itself copies it into an Intel swapchain on X-Plane's window.
//       native: the frame is copied into a real NVIDIA swapchain owned by the layer and presented normally
//               (for windows on a screen driven by the GeForce, or when the bypass is not faster / fails).
//   * X-Plane never has to recreate its swapchain for a path change.
//
// Config: xp_optimus_bypass.ini next to the DLL (see README). Log: xp_optimus_bypass.log (rewritten each run).
// License: MIT (see LICENSE).

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>
#include <dxgi1_5.h>
#include <d3d12.h>
#include <intrin.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <share.h>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define LAYER_EXPORT extern "C" __declspec(dllexport)
#define XPOB_VERSION "0.9.3.3"
#ifndef XPOB_BUILD
#define XPOB_BUILD "dev"
#endif

// ================================================================ config & log

enum Mode { MODE_OFF = 0, MODE_AUTO = 1, MODE_ON = 2 };
enum PathPref { PREF_AUTO = 0, PREF_BYPASS = 1, PREF_NATIVE = 2, PREF_CPU_COPY = 3 };
enum Path { PATH_NATIVE = 0, PATH_BYPASS = 1 };

struct Config {
    int mode = MODE_AUTO;
    bool vsync = false;
    bool stats = true;
    int path = PREF_AUTO;
    bool probe = true;            // measure native vs bypass before settling (path=auto; only with the DXGI
                                  // Intel output or test_bypass_after_native, see g_nativeShown)
    // test switches: let one laptop exercise every code path
    bool forceBlit = false;       // convert/scale through blits even when formats and sizes match
    bool forceCpuCopy = false;    // do not use VK_EXT_external_memory_host
    bool noExtraQueue = false;    // pretend the graphics family has no spare queue
    int failIntelAfter = 0;       // simulate an Intel failure after this many bypass frames
    bool measure = false;         // measure_driver=1: time X-Plane's Vulkan calls into the driver (diagnostics)
    int intelOut = 0;             // Intel output: 0 auto (DXGI flip, else Vulkan), 1 DXGI only, 2 Vulkan only
    int bypassAfterNative = 0;    // 0: never return to the bypass once native output was shown on the window,
                                  // 1: return anyway (same Intel surface), 2: return with a fresh Intel surface
};
static Config g_cfg;

static bool g_isXPlane = false;
static std::wstring g_dir;
static std::wstring g_logPath;
static std::wstring g_profile;
static std::once_flag g_initOnce;
static std::mutex g_logLock;
static LARGE_INTEGER g_freq;
static thread_local bool g_internalCreate = false;
static std::atomic<bool> g_intelBroken{false};    // Intel side failed: native path for the rest of the session
static std::atomic<bool> g_intelHung{false};      // an Intel wait timed out: never wait on the Intel device again
static std::atomic<bool> g_nvHung{false};
static std::atomic<bool> g_workerStuck{false};    // a flush gave no answer in 30 s: worker treated as lost         // a GeForce fence gave no answer for 10 s: device treated as lost
// NVIDIA's native output puts the window into DXGI flip mode. Older Intel Vulkan drivers present the old way, which
// Windows does not show on a window that has used flip mode: after native output the bypass would keep running
// but the picture would stay frozen. So once native output was shown, the bypass does not come back (by default).
static std::atomic<bool> g_nativeShown{false};

// Where each thread is, for the hang watchdog (see startWatchdog).
enum { XS_OUTSIDE, XS_ACQUIRE_WAIT, XS_PRESENT, XS_SWITCH, XS_N_ACQUIRE, XS_N_PRESENT, XS_N_FENCE };
static const char* kXStage[] = {"in X-Plane itself (outside the layer)", "waiting for a free image (worker busy)",
                                "in the layer's present", "switching path", "NVIDIA acquire (native)",
                                "NVIDIA present (native)", "waiting for an earlier native copy"};
enum { WS_IDLE, WS_OLD_GF, WS_OLD_INTEL, WS_GF_SUBMIT, WS_GF_FENCE, WS_CPU_COPY, WS_I_SWAPCHAIN, WS_I_ACQUIRE,
       WS_I_SUBMIT, WS_I_PRESENT };
static const char* kWStage[] = {"idle", "waiting for an earlier GeForce copy", "waiting for an earlier Intel copy",
                                "GeForce submit", "waiting for the GeForce copy", "CPU copy",
                                "Intel swapchain (re)creation", "Intel acquire", "Intel submit", "Intel present"};
static std::atomic<int> g_xStage{XS_OUTSIDE}, g_wStage{WS_IDLE}, g_curPath{-1};
static std::atomic<long long> g_lastFrameMs{0}, g_wStageMs{0};
static double nowMs();
static void setWStage(int st);
static std::atomic<bool> g_virtualBroken{false};  // native path inside the layer failed: real swapchains only
static bool g_haveIntelVk = false;                // X-Plane's Vulkan instance sees an Intel GPU

static void logf(const char* fmt, ...) {
    if (!g_isXPlane) return;
    std::lock_guard<std::mutex> lk(g_logLock);
    FILE* f = _wfsopen(g_logPath.c_str(), L"a", _SH_DENYNO);
    if (!f) {
        OutputDebugStringW((L"XP Optimus Bypass: could not open log: " + g_logPath).c_str());
        return;
    }
    setvbuf(f, nullptr, _IONBF, 0);  // publish every message; a viewer may read the file at the same time
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);
    fclose(f);
}

static double nowMs() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) * 1000.0 / double(g_freq.QuadPart);
}

static void setWStage(int st) {
    g_wStage = st;
    g_wStageMs = (long long)nowMs();
}

static std::string utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// Paths in the log must not reveal the Windows user name, so the log can be posted publicly.
static std::string cleanPath(std::wstring s) {
    if (!g_profile.empty() && s.size() >= g_profile.size() && !_wcsnicmp(s.c_str(), g_profile.c_str(), g_profile.size()))
        s = L"%USERPROFILE%" + s.substr(g_profile.size());
    for (size_t pos = 0;;) {
        std::wstring low = s;
        for (auto& c : low) c = (wchar_t)towlower(c);
        size_t u = low.find(L"\\users\\", pos);
        if (u == std::wstring::npos) break;
        size_t start = u + 7, end = s.find(L'\\', start);
        if (end == std::wstring::npos) end = s.size();
        if (s.compare(start, end - start, L"<user>") != 0 && s.compare(start, end - start, L"Public") != 0)
            s.replace(start, end - start, L"<user>");
        pos = start + 1;
    }
    return utf8(s);
}

static void logSystemInfo(const std::wstring& exePath) {
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW*);
    OSVERSIONINFOW ov{};
    ov.dwOSVersionInfoSize = sizeof(ov);
    auto rgv = (RtlGetVersionFn)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (rgv && rgv(&ov) == 0)
        logf("Windows %lu.%lu build %lu", ov.dwMajorVersion, ov.dwMinorVersion, ov.dwBuildNumber);
    DWORD h = 0, sz = GetFileVersionInfoSizeW(exePath.c_str(), &h);
    if (sz) {
        std::vector<char> buf(sz);
        VS_FIXEDFILEINFO* fi = nullptr;
        UINT len = 0;
        if (GetFileVersionInfoW(exePath.c_str(), 0, sz, buf.data()) &&
            VerQueryValueW(buf.data(), L"\\", (void**)&fi, &len) && fi && len)
            logf("X-Plane.exe file version %u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                 HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
    }
}

// driverVersion is vendor-encoded
static void fmtDriver(uint32_t vendor, uint32_t v, char* out, size_t n) {
    if (vendor == 0x10DE)
        snprintf(out, n, "%u.%02u", (v >> 22) & 0x3ff, (v >> 14) & 0xff);
    else if (vendor == 0x8086)
        snprintf(out, n, "%u.%u", v >> 14, v & 0x3fff);
    else
        snprintf(out, n, "0x%08x", v);
}

static void fmtLuid(const uint8_t* l, char* out, size_t n) {
    uint32_t lo, hi;
    memcpy(&lo, l, 4);
    memcpy(&hi, l + 4, 4);
    snprintf(out, n, "%08x:%08x", hi, lo);
}

static const char* pathName(int p) { return p == PATH_BYPASS ? "bypass" : "native"; }

static void readIni() {
    FILE* f = _wfopen((g_dir + L"xp_optimus_bypass.ini").c_str(), L"r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char k[64] = {0}, v[64] = {0};
        if (line[0] == ';' || line[0] == '#') continue;
        if (sscanf(line, " %63[^= \t] = %63s", k, v) != 2) continue;
        int iv = atoi(v);
        if (!_stricmp(k, "mode")) g_cfg.mode = !_stricmp(v, "off") ? MODE_OFF : !_stricmp(v, "on") ? MODE_ON : MODE_AUTO;
        else if (!_stricmp(k, "vsync")) g_cfg.vsync = iv != 0;
        else if (!_stricmp(k, "stats")) g_cfg.stats = iv != 0;
        else if (!_stricmp(k, "path"))
            g_cfg.path = !_stricmp(v, "bypass") ? PREF_BYPASS : !_stricmp(v, "native") ? PREF_NATIVE
                       : !_stricmp(v, "cpu-copy") ? PREF_CPU_COPY : PREF_AUTO;
        else if (!_stricmp(k, "probe")) g_cfg.probe = iv != 0;
        else if (!_stricmp(k, "test_force_blit")) g_cfg.forceBlit = iv != 0;
        else if (!_stricmp(k, "test_force_cpu_copy")) g_cfg.forceCpuCopy = iv != 0;
        else if (!_stricmp(k, "test_no_extra_queue")) g_cfg.noExtraQueue = iv != 0;
        else if (!_stricmp(k, "test_fail_intel_after")) g_cfg.failIntelAfter = iv > 0 ? iv : 0;
        else if (!_stricmp(k, "measure_driver")) g_cfg.measure = iv != 0;
        else if (!_stricmp(k, "intel_output"))
            g_cfg.intelOut = !_stricmp(v, "dxgi") ? 1 : !_stricmp(v, "vulkan") ? 2 : 0;
        else if (!_stricmp(k, "test_bypass_after_native")) g_cfg.bypassAfterNative = iv < 0 ? 0 : iv > 2 ? 2 : iv;
    }
    fclose(f);
    if (g_cfg.path == PREF_CPU_COPY) g_cfg.forceCpuCopy = true;
}

static void logLayersAndOverlays();

static void initOnce() {
    QueryPerformanceFrequency(&g_freq);
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&initOnce), &self);
    wchar_t path[MAX_PATH * 2] = {0};
    GetModuleFileNameW(self, path, MAX_PATH * 2);
    std::wstring p(path);
    size_t slash = p.find_last_of(L"\\/");
    g_dir = (slash == std::wstring::npos) ? L"" : p.substr(0, slash + 1);

    wchar_t exe[MAX_PATH * 2] = {0};
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    std::wstring e(exe);
    size_t es = e.find_last_of(L"\\/");
    std::wstring name = (es == std::wstring::npos) ? e : e.substr(es + 1);
    g_isXPlane = (_wcsicmp(name.c_str(), L"X-Plane.exe") == 0);
    if (!g_isXPlane) return;

    wchar_t prof[MAX_PATH] = {0};
    if (GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH)) g_profile = prof;

    // Explicit path for the launcher; installed global mode uses the same root path automatically.
    std::vector<wchar_t> requestedLog(32768, 0);
    DWORD logLength = GetEnvironmentVariableW(L"XP_OPTIMUS_BYPASS_LOG", requestedLog.data(), 32768);
    g_logPath = g_dir + L"xp_optimus_bypass.log";
    if (logLength > 0 && logLength < 32768) {
        g_logPath.assign(requestedLog.data(), logLength);
    } else if (!g_dir.empty()) {
        std::wstring dir = g_dir.substr(0, g_dir.size() - 1);
        size_t parentSlash = dir.find_last_of(L"\\/");
        if (parentSlash != std::wstring::npos && _wcsicmp(dir.substr(parentSlash + 1).c_str(), L"app") == 0) {
            std::wstring root = dir.substr(0, parentSlash + 1);
            if (GetFileAttributesW((root + L"xpob-install.json").c_str()) != INVALID_FILE_ATTRIBUTES)
                g_logPath = root + L"xp_optimus_bypass.log";
        }
    }

    readIni();
    FILE* lf = _wfsopen(g_logPath.c_str(), L"w", _SH_DENYNO);  // fresh log every run
    if (lf) fclose(lf);
    static const char* prefNames[] = {"auto", "bypass", "native", "cpu-copy"};
    logf("XP Optimus Bypass %s (build %s) loaded in X-Plane.exe | mode=%s path=%s probe=%d vsync=%d stats=%d "
         "measure_driver=%d", XPOB_VERSION, XPOB_BUILD,
         g_cfg.mode == MODE_OFF ? "off" : g_cfg.mode == MODE_ON ? "on" : "auto", prefNames[g_cfg.path],
         g_cfg.probe ? 1 : 0, g_cfg.vsync ? 1 : 0, g_cfg.stats ? 1 : 0, g_cfg.measure ? 1 : 0);
    if (g_cfg.forceBlit || g_cfg.forceCpuCopy || g_cfg.noExtraQueue || g_cfg.failIntelAfter || g_cfg.bypassAfterNative)
        logf("[diag] TEST SWITCHES: test_force_blit=%d test_force_cpu_copy=%d test_no_extra_queue=%d "
             "test_fail_intel_after=%d test_bypass_after_native=%d", g_cfg.forceBlit, g_cfg.forceCpuCopy,
             g_cfg.noExtraQueue, g_cfg.failIntelAfter, g_cfg.bypassAfterNative);
    char en[8] = {0};
    bool viaEnv = GetEnvironmentVariableA("XP_OPTIMUS_BYPASS_ENABLE", en, sizeof(en)) > 0;
    logf("started with XP_OPTIMUS_BYPASS_ENABLE=1 (dedicated launcher or launch option): %s", viaEnv ? "yes" : "no");
    logSystemInfo(e);
    logLayersAndOverlays();
}

// ================================================================ statistics

struct Series {
    std::vector<float> v;
    void add(double x) { v.push_back((float)x); }
    void clear() { v.clear(); }
    bool empty() const { return v.empty(); }
    double pct(double p) const {
        if (v.empty()) return 0;
        std::vector<float> t(v);
        size_t i = (size_t)std::floor(p * (double)(t.size() - 1) + 0.5);
        if (i >= t.size()) i = t.size() - 1;
        std::nth_element(t.begin(), t.begin() + (ptrdiff_t)i, t.end());
        return t[i];
    }
    double maxv() const { return v.empty() ? 0 : *std::max_element(v.begin(), v.end()); }
    double mean() const {
        double s = 0;
        for (float x : v) s += x;
        return v.empty() ? 0 : s / (double)v.size();
    }
};

static void addSeries(std::string& out, const char* name, const Series& s) {
    char b[128];
    if (s.empty()) snprintf(b, sizeof(b), " | %s -", name);
    else snprintf(b, sizeof(b), " | %s p50 %.2f p95 %.2f max %.2f", name, s.pct(0.5), s.pct(0.95), s.maxv());
    out += b;
}

// ================================================================ displays (which GPU drives which screen)

static bool displayIsIntel(const char* devName) {
    DISPLAY_DEVICEA dd;
    for (DWORD i = 0;; ++i) {
        ZeroMemory(&dd, sizeof(dd));
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesA(nullptr, i, &dd, 0)) break;
        if (!_stricmp(dd.DeviceName, devName)) return strstr(dd.DeviceID, "VEN_8086") != nullptr;
    }
    return false;
}

static bool anyDisplayOnIntel() {
    DISPLAY_DEVICEA dd;
    for (DWORD i = 0;; ++i) {
        ZeroMemory(&dd, sizeof(dd));
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesA(nullptr, i, &dd, 0)) break;
        if ((dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) && strstr(dd.DeviceID, "VEN_8086")) return true;
    }
    return false;
}

static bool windowOnIntel(HWND h) {
    HMONITOR m = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXA mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(m, &mi)) return false;
    return displayIsIntel(mi.szDevice);
}

struct DxgiAdapter {
    LUID luid;
    UINT vendor;
    std::wstring name;
};
struct DxgiOutput {
    HMONITOR mon;
    size_t adapter;  // index into the adapter list
    std::wstring device;
    RECT rect;
};

static bool queryDxgi(std::vector<DxgiAdapter>& ads, std::vector<DxgiOutput>& outs) {
    typedef HRESULT(WINAPI * PFN_CreateFactory1)(REFIID, void**);
    static HMODULE lib = LoadLibraryW(L"dxgi.dll");
    if (!lib) return false;
    auto create = (PFN_CreateFactory1)(void*)GetProcAddress(lib, "CreateDXGIFactory1");
    if (!create) return false;
    IDXGIFactory1* fac = nullptr;
    if (FAILED(create(__uuidof(IDXGIFactory1), (void**)&fac)) || !fac) return false;
    for (UINT a = 0;; ++a) {
        IDXGIAdapter1* ad = nullptr;
        if (fac->EnumAdapters1(a, &ad) == DXGI_ERROR_NOT_FOUND || !ad) break;
        DXGI_ADAPTER_DESC1 dd;
        if (SUCCEEDED(ad->GetDesc1(&dd)) && !(dd.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            ads.push_back({dd.AdapterLuid, dd.VendorId, dd.Description});
            for (UINT o = 0;; ++o) {
                IDXGIOutput* op = nullptr;
                if (ad->EnumOutputs(o, &op) == DXGI_ERROR_NOT_FOUND || !op) break;
                DXGI_OUTPUT_DESC od;
                if (SUCCEEDED(op->GetDesc(&od)) && od.AttachedToDesktop)
                    outs.push_back({od.Monitor, ads.size() - 1, od.DeviceName, od.DesktopCoordinates});
                op->Release();
            }
        }
        ad->Release();
    }
    fac->Release();
    return true;
}

// LUID of the Intel adapter that drives this monitor. DXGI on hybrid laptops can list the built-in screen
// under the NVIDIA adapter, so fall back to "the only Intel adapter" when the output is not under an Intel one.
static bool intelLuidForMonitor(HMONITOR m, uint8_t out[8]) {
    std::vector<DxgiAdapter> ads;
    std::vector<DxgiOutput> outs;
    if (!queryDxgi(ads, outs)) return false;
    for (auto& o : outs)
        if (o.mon == m && ads[o.adapter].vendor == 0x8086) {
            memcpy(out, &ads[o.adapter].luid, 8);
            return true;
        }
    int intel = -1, count = 0;
    for (size_t i = 0; i < ads.size(); ++i)
        if (ads[i].vendor == 0x8086) { intel = (int)i; ++count; }
    if (count == 1) {
        memcpy(out, &ads[intel].luid, 8);
        return true;
    }
    return false;
}

static void logDisplays(HWND xpWindow) {
    std::vector<DxgiAdapter> ads;
    std::vector<DxgiOutput> outs;
    if (!queryDxgi(ads, outs)) { logf("[diag] DXGI not available"); return; }
    HMONITOR xm = xpWindow ? MonitorFromWindow(xpWindow, MONITOR_DEFAULTTONEAREST) : nullptr;
    for (size_t i = 0; i < ads.size(); ++i) {
        char l[32];
        fmtLuid((const uint8_t*)&ads[i].luid, l, sizeof(l));
        logf("[diag] DXGI adapter %zu: %s | vendor 0x%04x | LUID %s", i, utf8(ads[i].name).c_str(), ads[i].vendor, l);
    }
    for (auto& o : outs) {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        DWORD hz = EnumDisplaySettingsW(o.device.c_str(), ENUM_CURRENT_SETTINGS, &dm) ? dm.dmDisplayFrequency : 0;
        logf("[diag] screen %s: %ldx%ld at (%ld,%ld) %lu Hz | listed under DXGI adapter %zu | driven by Intel "
             "(EnumDisplayDevices): %s%s", utf8(o.device).c_str(), o.rect.right - o.rect.left, o.rect.bottom - o.rect.top,
             o.rect.left, o.rect.top, hz, o.adapter, displayIsIntel(utf8(o.device).c_str()) ? "yes" : "no",
             o.mon == xm ? " | X-Plane window is here" : "");
    }
    if (xpWindow) {
        typedef UINT(WINAPI * GetDpiForWindowFn)(HWND);
        auto gdw = (GetDpiForWindowFn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
        RECT r{};
        GetClientRect(xpWindow, &r);
        logf("[diag] X-Plane window client %ldx%ld, DPI %u", r.right - r.left, r.bottom - r.top, gdw ? gdw(xpWindow) : 96);
    }
}

// Implicit Vulkan layers (overlays, capture tools) registered on this PC, and overlay DLLs already loaded.
static void logLayersAndOverlays() {
    HKEY roots[2] = {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE};
    const char* rootNames[2] = {"HKCU", "HKLM"};
    for (int r = 0; r < 2; ++r) {
        HKEY k;
        if (RegOpenKeyExW(roots[r], L"SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers", 0, KEY_READ, &k) != ERROR_SUCCESS) continue;
        for (DWORD i = 0;; ++i) {
            wchar_t name[2048];
            DWORD nlen = 2048, type = 0, val = 0, vlen = sizeof(val);
            if (RegEnumValueW(k, i, name, &nlen, nullptr, &type, (LPBYTE)&val, &vlen) != ERROR_SUCCESS) break;
            logf("[diag] implicit layer (%s): %s%s", rootNames[r], cleanPath(name).c_str(),
                 type == REG_DWORD && val != 0 ? " (disabled)" : "");
        }
        RegCloseKey(k);
    }
    typedef BOOL(WINAPI * EnumModsFn)(HANDLE, HMODULE*, DWORD, LPDWORD);
    auto enumMods = (EnumModsFn)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules");
    if (!enumMods) return;
    std::vector<HMODULE> mods(1024);
    DWORD need = 0;
    if (!enumMods(GetCurrentProcess(), mods.data(), (DWORD)(mods.size() * sizeof(HMODULE)), &need)) return;
    size_t n = std::min(mods.size(), (size_t)need / sizeof(HMODULE));
    static const wchar_t* keys[] = {L"overlay", L"rtss", L"graphics-hook", L"obs", L"discord", L"overwolf",
                                    L"nvspcap", L"reshade", L"fraps", L"bandicam", L"medal", L"vklayer", L"hook"};
    for (size_t i = 0; i < n; ++i) {
        wchar_t fn[MAX_PATH * 2] = {0};
        if (!GetModuleFileNameW(mods[i], fn, MAX_PATH * 2)) continue;
        std::wstring low(fn);
        for (auto& c : low) c = (wchar_t)towlower(c);
        size_t s = low.find_last_of(L"\\/");
        std::wstring base = s == std::wstring::npos ? low : low.substr(s + 1);
        for (const wchar_t* key : keys)
            if (base.find(key) != std::wstring::npos) {
                logf("[diag] overlay-like module loaded: %s", cleanPath(fn).c_str());
                break;
            }
    }
}

// ================================================================ worker thread

class Worker {
public:
    Worker() : th_([this] { run(); }) {}
    ~Worker() {  // finishes queued work, then stops and joins the thread
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
        }
        cv_.notify_all();
        if (th_.joinable()) th_.join();
    }
    void push(std::function<void()> fn) {
        {
            std::lock_guard<std::mutex> lk(m_);
            q_.push_back(std::move(fn));
            ++pushed_;
        }
        cv_.notify_one();
    }
    // Wait until all work pushed so far has finished. This cannot return early: callers free resources the queued
    // work uses. It is still bounded, because every wait inside the work is time-limited (GeForce fence 2 x 10 s,
    // Intel fence 5 s, DXGI 2 s; after a GeForce or Intel hang the work returns at once). A long wait is logged.
    // Returns false if the work did not finish within kFlushSteps x 1 s (counted, so sleep / resume does not count).
    // That can only happen if a driver call inside the work itself never returns (every wait the layer makes is
    // limited, a job takes at most about 27 s). Then the worker counts as lost (g_workerStuck): callers do not free
    // anything the work may use (they leak it) and report VK_ERROR_DEVICE_LOST instead of blocking X-Plane.
    static constexpr int kFlushSteps = 30;
    bool flush() {
        if (g_workerStuck) return false;
        std::unique_lock<std::mutex> lk(m_);
        uint64_t target = pushed_;
        int secs = 0;
        while (!done_.wait_for(lk, std::chrono::seconds(1), [&] { return finished_ >= target; })) {
            if (++secs == 3) logf("[hang] waiting for the worker thread (%llu job(s) left)",
                                  (unsigned long long)(target - finished_));
            if (secs >= kFlushSteps) {
                if (!g_workerStuck.exchange(true))
                    logf("[hang] worker thread gave no answer for %d s (a driver call did not return) -> treated as "
                         "lost: its resources are left allocated and X-Plane gets a lost device", kFlushSteps);
                return false;
            }
        }
        if (secs >= 3) logf("[hang] worker thread finished after about %d s", secs + 1);
        return true;
    }
    // The thread is stuck inside a driver call: never join it (that would block forever); the object is leaked.
    void abandon() { if (th_.joinable()) th_.detach(); }

private:
    void run() {
        for (;;) {
            std::function<void()> fn;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [&] { return !q_.empty() || stop_; });
                if (q_.empty()) return;  // stop requested and nothing left to do
                fn = std::move(q_.front());
                q_.pop_front();
            }
            fn();
            {
                std::lock_guard<std::mutex> lk(m_);
                ++finished_;
            }
            done_.notify_all();
        }
    }
    std::mutex m_;
    std::condition_variable cv_, done_;
    std::deque<std::function<void()>> q_;
    uint64_t pushed_ = 0, finished_ = 0;
    bool stop_ = false;
    std::thread th_;  // declared last: starts after the other members exist
};

// ================================================================ dispatch data

template <typename T> static void* key(T h) { return *reinterpret_cast<void**>(h); }

struct InstData {
    VkInstance instance;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkDestroyInstance destroy;
    uint32_t apiVersion;
    bool props2;  // vkGetPhysicalDeviceProperties2 usable
};

struct DevData {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    PFN_vkSetDeviceLoaderData setLoaderData = nullptr;
    VkPhysicalDeviceMemoryProperties memProps{};
    VkPhysicalDeviceProperties props{};
    bool internal = false;      // a device this layer created itself
    bool active = false;        // virtual swapchains possible (extra queue prepared)
    bool hostImport = false;    // VK_EXT_external_memory_host enabled on this device
    VkDeviceSize hostAlign = 4096;
    uint32_t copyFamily = UINT32_MAX;
    bool copyIsGraphics = true;
    VkQueue copyQueue = VK_NULL_HANDLE;
    std::mutex copyQLock;
    std::vector<VkQueueFamilyProperties> families;
    std::vector<uint32_t> xpFamilies;  // queue families X-Plane created queues in
    std::mutex qLock;
    std::unordered_map<VkQueue, uint32_t> queueFamily;
    Worker* worker = nullptr;
    std::mutex semLock;
    std::unordered_multiset<VkSemaphore> pendingWaits;  // X-Plane semaphores a queued frame still waits on
    // stats when no virtual swapchain is used (mode=off, or not applicable)
    Series pFrame, pPresent;
    double pLast = 0;
    int pOver50 = 0;

#define DFN(n) PFN_##n n = nullptr
    DFN(vkDestroyDevice); DFN(vkGetDeviceQueue); DFN(vkGetDeviceQueue2); DFN(vkQueueSubmit); DFN(vkQueueSubmit2);
    DFN(vkQueueSubmit2KHR); DFN(vkQueueWaitIdle); DFN(vkDeviceWaitIdle); DFN(vkCreateSwapchainKHR);
    DFN(vkDestroySwapchainKHR); DFN(vkGetSwapchainImagesKHR); DFN(vkAcquireNextImageKHR); DFN(vkAcquireNextImage2KHR);
    DFN(vkQueuePresentKHR); DFN(vkCreateImage); DFN(vkDestroyImage); DFN(vkGetImageMemoryRequirements);
    DFN(vkAllocateMemory); DFN(vkFreeMemory); DFN(vkBindImageMemory); DFN(vkCreateSemaphore); DFN(vkDestroySemaphore);
    DFN(vkCreateFence); DFN(vkDestroyFence); DFN(vkWaitForFences); DFN(vkResetFences); DFN(vkCreateCommandPool);
    DFN(vkDestroyCommandPool); DFN(vkAllocateCommandBuffers); DFN(vkBeginCommandBuffer); DFN(vkEndCommandBuffer);
    DFN(vkResetCommandBuffer); DFN(vkCmdPipelineBarrier); DFN(vkCmdCopyImageToBuffer); DFN(vkCmdCopyImage);
    DFN(vkCmdBlitImage); DFN(vkCreateBuffer); DFN(vkDestroyBuffer); DFN(vkGetBufferMemoryRequirements);
    DFN(vkBindBufferMemory); DFN(vkMapMemory); DFN(vkUnmapMemory); DFN(vkInvalidateMappedMemoryRanges);
    DFN(vkGetMemoryHostPointerPropertiesEXT); DFN(vkCreateQueryPool); DFN(vkDestroyQueryPool);
    DFN(vkCmdResetQueryPool); DFN(vkCmdWriteTimestamp); DFN(vkGetQueryPoolResults);
    DFN(vkSetDebugUtilsObjectNameEXT); DFN(vkSetDebugUtilsObjectTagEXT);
    DFN(vkGetSwapchainStatusKHR); DFN(vkSetHdrMetadataEXT); DFN(vkWaitForPresentKHR);
    DFN(vkGetRefreshCycleDurationGOOGLE); DFN(vkGetPastPresentationTimingGOOGLE);
    DFN(vkAcquireFullScreenExclusiveModeEXT); DFN(vkReleaseFullScreenExclusiveModeEXT);
    DFN(vkReleaseSwapchainImagesEXT); DFN(vkSetLatencySleepModeNV); DFN(vkLatencySleepNV);
    DFN(vkSetLatencyMarkerNV); DFN(vkGetLatencyTimingsNV); DFN(vkSignalSemaphore); DFN(vkSignalSemaphoreKHR);
#undef DFN
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR surfCaps = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties formatProps = nullptr;
};

static std::mutex g_lock;
static std::unordered_map<void*, InstData> g_inst;
static std::unordered_map<void*, DevData*> g_dev;
static std::mutex g_surfLock;
static std::unordered_map<VkSurfaceKHR, HWND> g_surfHwnd;

static DevData* devOf(void* k) {
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_dev.find(k);
    return it == g_dev.end() ? nullptr : it->second;
}

static uint32_t familyOf(DevData* d, VkQueue q) {
    std::lock_guard<std::mutex> lk(d->qLock);
    auto it = d->queueFamily.find(q);
    return it == d->queueFamily.end() ? UINT32_MAX : it->second;
}

static uint32_t memType(const VkPhysicalDeviceMemoryProperties& mp, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}
static uint32_t memTypeAny(const VkPhysicalDeviceMemoryProperties& mp, uint32_t bits, VkMemoryPropertyFlags want) {
    uint32_t t = memType(mp, bits, want);
    if (t != UINT32_MAX) return t;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if (bits & (1u << i)) return i;
    return UINT32_MAX;
}

static bool isBgra8(VkFormat f) { return f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB; }
static bool isSrgb(VkFormat f) {
    return f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}
// SDR formats the bypass can convert to BGRA8 with a blit
static bool bypassFormat(VkFormat f) {
    switch (f) {
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_A8B8G8R8_UNORM_PACK32: case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
        return true;
    default:
        return false;
    }
}

// ================================================================ Intel side

struct IntelCtx {
    HWND hwnd = nullptr;
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue q = VK_NULL_HANDLE;
    uint32_t fam = 0;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mem{};
    bool hostImport = false;
    VkDeviceSize hostAlign = 4096;
    uint32_t tsBits = 0;
    float tsPeriod = 1.0f;
    bool famGraphics = false;  // blits need a graphics queue
    uint8_t luid[8]{};
    bool luidValid = false;
#define IFN(n) PFN_##n n = nullptr
    IFN(vkGetInstanceProcAddr); IFN(vkGetDeviceProcAddr); IFN(vkDestroyInstance);
    IFN(vkEnumeratePhysicalDevices); IFN(vkGetPhysicalDeviceProperties); IFN(vkGetPhysicalDeviceProperties2);
    IFN(vkEnumerateDeviceExtensionProperties); IFN(vkGetPhysicalDeviceFormatProperties);
    IFN(vkGetPhysicalDeviceQueueFamilyProperties); IFN(vkGetPhysicalDeviceMemoryProperties);
    IFN(vkGetPhysicalDeviceSurfaceSupportKHR); IFN(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    IFN(vkGetPhysicalDeviceSurfaceFormatsKHR); IFN(vkGetPhysicalDeviceSurfacePresentModesKHR);
    IFN(vkCreateWin32SurfaceKHR); IFN(vkDestroySurfaceKHR); IFN(vkCreateDevice);
    IFN(vkDestroyDevice); IFN(vkGetDeviceQueue); IFN(vkCreateSwapchainKHR); IFN(vkDestroySwapchainKHR);
    IFN(vkGetSwapchainImagesKHR); IFN(vkAcquireNextImageKHR); IFN(vkQueuePresentKHR); IFN(vkQueueSubmit);
    IFN(vkCreateCommandPool); IFN(vkDestroyCommandPool); IFN(vkAllocateCommandBuffers); IFN(vkFreeCommandBuffers);
    IFN(vkBeginCommandBuffer); IFN(vkEndCommandBuffer); IFN(vkResetCommandBuffer); IFN(vkCmdPipelineBarrier);
    IFN(vkCmdCopyBufferToImage); IFN(vkCmdBlitImage); IFN(vkCreateBuffer); IFN(vkDestroyBuffer);
    IFN(vkGetBufferMemoryRequirements); IFN(vkAllocateMemory); IFN(vkFreeMemory); IFN(vkBindBufferMemory);
    IFN(vkMapMemory); IFN(vkUnmapMemory); IFN(vkFlushMappedMemoryRanges); IFN(vkGetMemoryHostPointerPropertiesEXT);
    IFN(vkCreateImage); IFN(vkDestroyImage); IFN(vkGetImageMemoryRequirements); IFN(vkBindImageMemory);
    IFN(vkCreateSemaphore); IFN(vkDestroySemaphore); IFN(vkCreateFence); IFN(vkDestroyFence);
    IFN(vkWaitForFences); IFN(vkResetFences); IFN(vkDeviceWaitIdle);
    IFN(vkCreateQueryPool); IFN(vkDestroyQueryPool); IFN(vkCmdResetQueryPool); IFN(vkCmdWriteTimestamp);
    IFN(vkGetQueryPoolResults);
#undef IFN
};

static IntelCtx* g_intel = nullptr;  // X-Plane has a single window
static std::string g_intelWhyNot;

// ---------------------------------------------------------------- Intel output through DXGI (flip model)
// NVIDIA's native output uses DXGI flip mode, and Windows then no longer shows a window's old-style (blt) presents,
// which older Intel Vulkan drivers use. Presenting the Intel side through a D3D12 flip-model swapchain keeps the
// window in flip mode for both paths, so the layer can switch either way. D3D12 reads the transfer block in place
// (OpenExistingHeapFromAddress) when the driver supports it; otherwise through an upload buffer + CPU copy.
// MinGW's d3d12.h stops at ID3D12Device1; these two declarations add what we need (vtable order as in the SDK).
struct XD3D12Device2 : public ID3D12Device1 {
    virtual HRESULT STDMETHODCALLTYPE CreatePipelineState(const void* desc, REFIID riid, void** pp) = 0;
};
struct XD3D12Device3 : public XD3D12Device2 {
    virtual HRESULT STDMETHODCALLTYPE OpenExistingHeapFromAddress(const void* address, REFIID riid, void** heap) = 0;
    virtual HRESULT STDMETHODCALLTYPE OpenExistingHeapFromFileMapping(HANDLE file, REFIID riid, void** heap) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnqueueMakeResident(UINT flags, UINT n, ID3D12Pageable* const* objects,
                                                          ID3D12Fence* fence, UINT64 value) = 0;
};
static const GUID kIID_Device3 = {0x81dadc15, 0x2bad, 0x4392, {0x93, 0xc5, 0x10, 0x13, 0x45, 0xc4, 0xaa, 0x98}};

struct DxCtx {
    IDXGIFactory4* factory = nullptr;
    ID3D12Device* dev = nullptr;
    XD3D12Device3* dev3 = nullptr;  // null: no in-place import
    ID3D12CommandQueue* q = nullptr;
    bool tearing = false;
    std::string name;
};
static DxCtx* g_dx = nullptr;
static bool g_dxTried = false;
static std::string g_dxWhyNot;
static std::atomic<DWORD> g_xpThread{0};  // X-Plane's render thread (DXGI swapchains are created only there)

// ---------------------------------------------------------------- measure_driver=1
// How much of X-Plane's render thread goes into the Vulkan driver, by kind of call. Every device function X-Plane
// gets is wrapped (src/measure_hooks.inc, generated); a wrapper reads the time stamp counter around the call.
// Diagnostics only: adds roughly 10-20 ns per call, and nothing else changes.
enum { M_DRAW, M_DISPATCH, M_BIND, M_PASS, M_COPY, M_CMD, M_CB, M_DESC, M_SUBMIT, M_WAIT, M_MAP, M_PIPE, M_RES, M_N };
static const char* kMName[M_N] = {"draw", "dispatch", "bind", "pass", "copy/barrier", "other cmd", "cmd buffer",
                                  "descriptors", "submit", "wait", "map", "pipelines", "resources"};
struct MAcc {
    uint64_t t[M_N];
    uint32_t n[M_N];
};
static MAcc g_mMain{};  // written only by X-Plane's render thread
static std::atomic<uint64_t> g_mOtherT{0};
static std::atomic<uint32_t> g_mOtherN{0};
static inline void mAdd(int c, uint64_t dt) {
    if (GetCurrentThreadId() == g_xpThread.load(std::memory_order_relaxed)) {
        g_mMain.t[c] += dt;
        g_mMain.n[c]++;
    } else {
        g_mOtherT.fetch_add(dt, std::memory_order_relaxed);
        g_mOtherN.fetch_add(1, std::memory_order_relaxed);
    }
}
// X-Plane waits for the GPU by polling vkGetFenceStatus in a loop. From the first "not ready" until a poll says
// "ready" counts as waiting for the GPU (including X-Plane's own loop between the polls). Render thread only.
static uint64_t g_mSpin = 0, g_mSpinStart = 0, g_mPollT = 0;  // g_mPollT: time inside the polls themselves
static uint32_t g_mPolls = 0;
static inline void mPoll(bool ready, uint64_t t0, uint64_t t1) {
    if (GetCurrentThreadId() != g_xpThread.load(std::memory_order_relaxed)) return;
    ++g_mPolls;
    g_mPollT += t1 - t0;
    if (!ready) {
        if (!g_mSpinStart) g_mSpinStart = t0;
    } else if (g_mSpinStart) {
        g_mSpin += t1 - g_mSpinStart;
        g_mSpinStart = 0;
    }
}
struct MeasureHook {
    const char* name;
    PFN_vkVoidFunction fn;
    PFN_vkVoidFunction* next;
};
#include "measure_hooks.inc"
static const void* g_mDev = nullptr;  // the device the wrappers belong to (X-Plane's)
static const MeasureHook* findMeasure(const char* n) {
    for (const MeasureHook& h : kMeasureHooks)
        if (!strcmp(n, h.name)) return &h;
    return nullptr;
}

template <typename T> static void rel(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

static DxCtx* dxCreate(IntelCtx* c) {
    g_dxTried = true;
    auto fail = [&](const char* why, HRESULT hr) -> DxCtx* {
        char b[160];
        snprintf(b, sizeof(b), "%s (0x%08lx)", why, (unsigned long)hr);
        g_dxWhyNot = b;
        return nullptr;
    };
    if (!c->luidValid) return fail("the Intel Vulkan device reports no LUID", 0);
    HMODULE dxgiLib = LoadLibraryW(L"dxgi.dll");
    HMODULE d3dLib = LoadLibraryW(L"d3d12.dll");
    if (!dxgiLib || !d3dLib) return fail("dxgi.dll / d3d12.dll not available", 0);
    typedef HRESULT(WINAPI * PFN_Factory2)(UINT, REFIID, void**);
    auto mkFactory = (PFN_Factory2)(void*)GetProcAddress(dxgiLib, "CreateDXGIFactory2");
    auto mkDevice = (PFN_D3D12_CREATE_DEVICE)(void*)GetProcAddress(d3dLib, "D3D12CreateDevice");
    if (!mkFactory || !mkDevice) return fail("CreateDXGIFactory2 / D3D12CreateDevice missing", 0);
    auto* x = new DxCtx();
    HRESULT hr = mkFactory(0, __uuidof(IDXGIFactory4), (void**)&x->factory);
    if (FAILED(hr)) { delete x; return fail("CreateDXGIFactory2 failed", hr); }
    LUID luid;
    memcpy(&luid, c->luid, sizeof(luid));
    IDXGIAdapter1* ad = nullptr;
    hr = x->factory->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter1), (void**)&ad);
    if (FAILED(hr)) { rel(x->factory); delete x; return fail("the Intel adapter is not listed by DXGI", hr); }
    DXGI_ADAPTER_DESC1 ad1{};
    ad->GetDesc1(&ad1);
    x->name = utf8(ad1.Description);
    hr = mkDevice(ad, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&x->dev);
    rel(ad);
    if (FAILED(hr)) { rel(x->factory); delete x; return fail("D3D12CreateDevice on the Intel GPU failed", hr); }
    D3D12_FEATURE_DATA_EXISTING_HEAPS eh{};
    if (SUCCEEDED(x->dev->CheckFeatureSupport(D3D12_FEATURE_EXISTING_HEAPS, &eh, sizeof(eh))) && eh.Supported &&
        !g_cfg.forceCpuCopy)
        x->dev->QueryInterface(kIID_Device3, (void**)&x->dev3);
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = x->dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&x->q);
    if (FAILED(hr)) { rel(x->dev3); rel(x->dev); rel(x->factory); delete x; return fail("D3D12 queue", hr); }
    IDXGIFactory5* f5 = nullptr;
    if (SUCCEEDED(x->factory->QueryInterface(__uuidof(IDXGIFactory5), (void**)&f5))) {
        BOOL t = FALSE;
        if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &t, sizeof(t)))) x->tearing = t;
        rel(f5);
    }
    logf("[diag] Intel output: DXGI flip model through D3D12 on %s | in-place import of the transfer block: %s | "
         "tearing (no vsync): %s", x->name.c_str(), x->dev3 ? "yes" : "no (upload buffer + CPU copy)",
         x->tearing ? "yes" : "no");
    return x;
}

static IntelCtx* intelCreate(HWND hwnd) {
    HMODULE lib = GetModuleHandleW(L"vulkan-1.dll");
    if (!lib) lib = LoadLibraryW(L"vulkan-1.dll");
    if (!lib) { g_intelWhyNot = "vulkan-1.dll not found"; return nullptr; }
    auto* c = new IntelCtx();
    c->hwnd = hwnd;
    c->vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)(void*)GetProcAddress(lib, "vkGetInstanceProcAddr");
    auto createInstance = c->vkGetInstanceProcAddr ?
        (PFN_vkCreateInstance)c->vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance") : nullptr;
    if (!createInstance) { delete c; g_intelWhyNot = "no vkCreateInstance"; return nullptr; }

    // Our own instance must not load implicit layers (overlays, and this layer again).
    char oldDisable[512] = {0}, oldSelf[64] = {0};
    DWORD hadDisable = GetEnvironmentVariableA("VK_LOADER_LAYERS_DISABLE", oldDisable, sizeof(oldDisable));
    DWORD hadSelf = GetEnvironmentVariableA("XP_OPTIMUS_BYPASS_DISABLE", oldSelf, sizeof(oldSelf));
    SetEnvironmentVariableA("VK_LOADER_LAYERS_DISABLE", "~implicit~");
    SetEnvironmentVariableA("XP_OPTIMUS_BYPASS_DISABLE", "1");
    const char* iexts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "xp_optimus_bypass";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = iexts;
    g_internalCreate = true;
    VkResult r = createInstance(&ici, nullptr, &c->inst);
    g_internalCreate = false;
    SetEnvironmentVariableA("VK_LOADER_LAYERS_DISABLE", hadDisable ? oldDisable : nullptr);
    SetEnvironmentVariableA("XP_OPTIMUS_BYPASS_DISABLE", hadSelf ? oldSelf : nullptr);
    if (r != VK_SUCCESS) {
        char b[64];
        snprintf(b, sizeof(b), "Intel vkCreateInstance failed (%d)", (int)r);
        g_intelWhyNot = b;
        delete c;
        return nullptr;
    }

#define LI(n) c->n = (PFN_##n)c->vkGetInstanceProcAddr(c->inst, #n)
    LI(vkGetDeviceProcAddr); LI(vkDestroyInstance); LI(vkEnumeratePhysicalDevices); LI(vkGetPhysicalDeviceProperties);
    LI(vkGetPhysicalDeviceProperties2); LI(vkEnumerateDeviceExtensionProperties); LI(vkGetPhysicalDeviceFormatProperties);
    LI(vkGetPhysicalDeviceQueueFamilyProperties); LI(vkGetPhysicalDeviceMemoryProperties);
    LI(vkGetPhysicalDeviceSurfaceSupportKHR); LI(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    LI(vkGetPhysicalDeviceSurfaceFormatsKHR); LI(vkGetPhysicalDeviceSurfacePresentModesKHR);
    LI(vkCreateWin32SurfaceKHR); LI(vkDestroySurfaceKHR); LI(vkCreateDevice);
#undef LI
    auto fail = [&](const char* why) -> IntelCtx* {
        g_intelWhyNot = why;
        logf("Intel: %s", why);
        if (c->pool && c->dev) c->vkDestroyCommandPool(c->dev, c->pool, nullptr);
        if (c->dev) c->vkDestroyDevice(c->dev, nullptr);
        if (c->surface) c->vkDestroySurfaceKHR(c->inst, c->surface, nullptr);
        c->vkDestroyInstance(c->inst, nullptr);
        delete c;
        return nullptr;
    };

    VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = GetModuleHandleW(nullptr);
    sci.hwnd = hwnd;
    if (c->vkCreateWin32SurfaceKHR(c->inst, &sci, nullptr, &c->surface) != VK_SUCCESS)
        return fail("could not create a surface on X-Plane's window");

    // Pick the Intel GPU that drives X-Plane's screen (by LUID), not just the first Intel device.
    uint8_t want[8] = {0};
    bool haveWant = intelLuidForMonitor(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), want);
    uint32_t n = 0;
    c->vkEnumeratePhysicalDevices(c->inst, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    c->vkEnumeratePhysicalDevices(c->inst, &n, pds.data());
    VkPhysicalDevice byLuid = VK_NULL_HANDLE, firstOk = VK_NULL_HANDLE;
    for (auto p : pds) {
        VkPhysicalDeviceIDProperties idp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &idp;
        if (c->vkGetPhysicalDeviceProperties2) c->vkGetPhysicalDeviceProperties2(p, &p2);
        else c->vkGetPhysicalDeviceProperties(p, &p2.properties);
        if (p2.properties.vendorID != 0x8086) continue;
        uint32_t nf = 0;
        c->vkGetPhysicalDeviceQueueFamilyProperties(p, &nf, nullptr);
        bool canPresent = false;
        for (uint32_t f = 0; f < nf && !canPresent; ++f) {
            VkBool32 sup = VK_FALSE;
            c->vkGetPhysicalDeviceSurfaceSupportKHR(p, f, c->surface, &sup);
            canPresent = sup == VK_TRUE;
        }
        char l[32];
        fmtLuid(idp.deviceLUID, l, sizeof(l));
        bool match = haveWant && idp.deviceLUIDValid && !memcmp(idp.deviceLUID, want, 8);
        logf("[diag] Intel candidate: %s | LUID %s%s | can present to X-Plane's window: %s", p2.properties.deviceName,
             idp.deviceLUIDValid ? l : "n/a", match ? " (drives X-Plane's screen)" : "", canPresent ? "yes" : "no");
        if (!canPresent) continue;
        if (match && !byLuid) byLuid = p;
        if (!firstOk) firstOk = p;
    }
    c->pd = byLuid ? byLuid : firstOk;
    if (c->pd && c->vkGetPhysicalDeviceProperties2) {
        VkPhysicalDeviceIDProperties idp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &idp;
        c->vkGetPhysicalDeviceProperties2(c->pd, &p2);
        if (idp.deviceLUIDValid) { memcpy(c->luid, idp.deviceLUID, 8); c->luidValid = true; }
    }
    if (!c->pd) return fail("no Intel Vulkan device can present to X-Plane's window");
    if (!byLuid) logf("[diag] Intel device chosen without a LUID match (first Intel device that can present)");

    VkPhysicalDeviceProperties pr;
    c->vkGetPhysicalDeviceProperties(c->pd, &pr);
    char dv[32];
    fmtDriver(pr.vendorID, pr.driverVersion, dv, sizeof(dv));
    c->tsPeriod = pr.limits.timestampPeriod;
    logf("Intel: using %s, driver %s, Vulkan %u.%u", pr.deviceName, dv, VK_API_VERSION_MAJOR(pr.apiVersion),
         VK_API_VERSION_MINOR(pr.apiVersion));
    c->vkGetPhysicalDeviceMemoryProperties(c->pd, &c->mem);

    // is VK_EXT_external_memory_host there?
    uint32_t ne = 0;
    c->vkEnumerateDeviceExtensionProperties(c->pd, nullptr, &ne, nullptr);
    std::vector<VkExtensionProperties> eps(ne);
    c->vkEnumerateDeviceExtensionProperties(c->pd, nullptr, &ne, eps.data());
    for (auto& e : eps)
        if (!strcmp(e.extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) c->hostImport = true;
    if (c->hostImport && c->vkGetPhysicalDeviceProperties2) {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &hp;
        c->vkGetPhysicalDeviceProperties2(c->pd, &p2);
        if (hp.minImportedHostPointerAlignment) c->hostAlign = hp.minImportedHostPointerAlignment;
    }
    logf("[diag] Intel VK_EXT_external_memory_host: %s (alignment %llu)", c->hostImport ? "yes" : "no",
         (unsigned long long)c->hostAlign);

    c->vkGetPhysicalDeviceQueueFamilyProperties(c->pd, &n, nullptr);
    std::vector<VkQueueFamilyProperties> fams(n);
    c->vkGetPhysicalDeviceQueueFamilyProperties(c->pd, &n, fams.data());
    bool found = false;
    for (uint32_t f = 0; f < n && !found; ++f) {
        VkBool32 sup = VK_FALSE;
        c->vkGetPhysicalDeviceSurfaceSupportKHR(c->pd, f, c->surface, &sup);
        if (sup && (fams[f].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { c->fam = f; found = true; }
    }
    for (uint32_t f = 0; f < n && !found; ++f) {
        VkBool32 sup = VK_FALSE;
        c->vkGetPhysicalDeviceSurfaceSupportKHR(c->pd, f, c->surface, &sup);
        if (sup && (fams[f].queueFlags & VK_QUEUE_TRANSFER_BIT)) { c->fam = f; found = true; }
    }
    if (!found) return fail("Intel GPU cannot present to X-Plane's window");
    c->tsBits = fams[c->fam].timestampValidBits;
    c->famGraphics = (fams[c->fam].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = c->fam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    std::vector<const char*> dexts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (c->hostImport) dexts.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)dexts.size();
    dci.ppEnabledExtensionNames = dexts.data();
    g_internalCreate = true;
    r = c->vkCreateDevice(c->pd, &dci, nullptr, &c->dev);
    g_internalCreate = false;
    if (r != VK_SUCCESS && c->hostImport) {  // try again without host import: CPU copy can still work
        c->hostImport = false;
        dexts.pop_back();
        dci.enabledExtensionCount = (uint32_t)dexts.size();
        g_internalCreate = true;
        r = c->vkCreateDevice(c->pd, &dci, nullptr, &c->dev);
        g_internalCreate = false;
    }
    if (r != VK_SUCCESS) return fail("Intel vkCreateDevice failed");
#define LD(n) c->n = (PFN_##n)c->vkGetDeviceProcAddr(c->dev, #n)
    LD(vkDestroyDevice); LD(vkGetDeviceQueue); LD(vkCreateSwapchainKHR); LD(vkDestroySwapchainKHR);
    LD(vkGetSwapchainImagesKHR); LD(vkAcquireNextImageKHR); LD(vkQueuePresentKHR); LD(vkQueueSubmit);
    LD(vkCreateCommandPool); LD(vkDestroyCommandPool); LD(vkAllocateCommandBuffers); LD(vkFreeCommandBuffers);
    LD(vkBeginCommandBuffer); LD(vkEndCommandBuffer); LD(vkResetCommandBuffer); LD(vkCmdPipelineBarrier);
    LD(vkCmdCopyBufferToImage); LD(vkCmdBlitImage); LD(vkCreateBuffer); LD(vkDestroyBuffer);
    LD(vkGetBufferMemoryRequirements); LD(vkAllocateMemory); LD(vkFreeMemory); LD(vkBindBufferMemory);
    LD(vkMapMemory); LD(vkUnmapMemory); LD(vkFlushMappedMemoryRanges); LD(vkGetMemoryHostPointerPropertiesEXT);
    LD(vkCreateImage); LD(vkDestroyImage); LD(vkGetImageMemoryRequirements); LD(vkBindImageMemory);
    LD(vkCreateSemaphore); LD(vkDestroySemaphore); LD(vkCreateFence); LD(vkDestroyFence);
    LD(vkWaitForFences); LD(vkResetFences); LD(vkDeviceWaitIdle);
    LD(vkCreateQueryPool); LD(vkDestroyQueryPool); LD(vkCmdResetQueryPool); LD(vkCmdWriteTimestamp);
    LD(vkGetQueryPoolResults);
#undef LD
    if (!c->vkGetMemoryHostPointerPropertiesEXT) c->hostImport = false;
    c->vkGetDeviceQueue(c->dev, c->fam, 0, &c->q);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = c->fam;
    if (c->vkCreateCommandPool(c->dev, &pci, nullptr, &c->pool) != VK_SUCCESS) return fail("command pool failed");
    logf("Intel: device ready (queue family %u, GPU timestamps %s)", c->fam, c->tsBits ? "yes" : "no");
    return c;
}

static bool intelIdleDevice(IntelCtx* c);
static void intelDestroy(IntelCtx* c) {
    if (!c) return;
    if (g_intelHung) return;  // a hung driver could block here; the process ends soon anyway
    if (c->dev) {
        if (!intelIdleDevice(c)) return;  // no answer within 5 s: leak the device rather than block
        if (c->pool) c->vkDestroyCommandPool(c->dev, c->pool, nullptr);
        c->vkDestroyDevice(c->dev, nullptr);
    }
    if (c->surface) c->vkDestroySurfaceKHR(c->inst, c->surface, nullptr);
    if (c->inst) c->vkDestroyInstance(c->inst, nullptr);
    delete c;
}

// ================================================================ the swapchain X-Plane sees

struct VImage {
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkSemaphore copyDone = VK_NULL_HANDLE;  // signalled when the layer has finished reading the image
    bool copyPending = false;               // copyDone signalled but not yet waited on
    int state = 0;                          // 0 free, 1 held by X-Plane, 2 queued
};

enum MemMode { MEM_SHARED = 0, MEM_NV_IMPORT = 1, MEM_INTEL_IMPORT = 2, MEM_CPU = 3 };
static const char* memName(int m) {
    static const char* n[] = {"shared (both GPUs import host memory)", "GeForce imports, CPU copy to Intel",
                              "Intel imports, CPU copy from GeForce", "CPU copy (no host import)"};
    return n[m];
}

struct Perf {
    std::mutex m;
    Series frame, xpWait, layer, gfGpu, gfFence, cpuCopy, iGpu, iAcq, iPres, nAcq, nGpu, nPres;
    int over50 = 0;
    double lastT0 = 0;
    void clearAll() {
        for (Series* s : {&frame, &xpWait, &layer, &gfGpu, &gfFence, &cpuCopy, &iGpu, &iAcq, &iPres, &nAcq, &nGpu, &nPres})
            s->clear();
        over50 = 0;
    }
};

struct VSwap {
    static const int R = 3;  // frames in flight inside the layer
    DevData* d = nullptr;
    HWND hwnd = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainCreateInfoKHR ci{};  // X-Plane's request (pNext dropped)
    VkExtent2D ext{};
    std::vector<VImage> v;
    uint32_t nextV = 0;
    std::mutex m;
    std::condition_variable cv;
    std::atomic<int> err{0};
    std::atomic<bool> suggestResize{false};
    int path = PATH_NATIVE;  // current path (X-Plane's thread decides)

    // ---- bypass (resources built on first use; frames processed on the worker thread)
    bool bypassTried = false, bypassOk = false;
    std::string bypassWhyNot;
    IntelCtx* ic = nullptr;
    int mem = MEM_SHARED;
    bool conv = false;                  // GeForce blit to BGRA8 first
    VkFormat convFmt = VK_FORMAT_B8G8R8A8_UNORM;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb[R]{};
    VkFence fence[R]{};
    bool fenceUsed[R]{};
    uint32_t ring = 0;
    VkImage convImg[R]{};
    VkDeviceMemory convMem[R]{};
    VkDeviceSize bytes = 0;
    void* host[R]{};
    // trial host-import block: kept until the bypass is destroyed (older Intel drivers can lose the device when
    // an imported block is freed right after the import)
    void* trialHost = nullptr;
    VkBuffer trialNvBuf = VK_NULL_HANDLE, trialIBuf = VK_NULL_HANDLE;
    VkDeviceMemory trialNvMem = VK_NULL_HANDLE, trialIMem = VK_NULL_HANDLE;
    VkBuffer nvBuf[R]{};
    VkDeviceMemory nvMem[R]{};
    void* nvPtr[R]{};
    bool nvCoherent = true;
    VkBuffer iBuf[R]{};
    VkDeviceMemory iMem[R]{};
    void* iPtr[R]{};
    bool iCoherent = true;
    VkQueryPool nvQ = VK_NULL_HANDLE;
    float nvPeriod = 1.0f;
    uint64_t nvMask = ~0ull;
    VkQueryPool iQ = VK_NULL_HANDLE;
    uint64_t iMask = ~0ull;
    bool iTsPending[R]{};
    VkSwapchainKHR iswap = VK_NULL_HANDLE;
    bool iRecreate = true;
    VkSurfaceFormatKHR iFmt{};
    VkPresentModeKHR iMode = VK_PRESENT_MODE_FIFO_KHR;
    VkExtent2D iext{};
    std::vector<VkImage> iImgs;
    VkCommandBuffer iCb[R]{};
    VkFence iFence[R]{};
    bool iFenceUsed[R]{};
    VkSemaphore iAcq[R]{}, iReady[R]{};
    bool iBlitOk = false;               // Intel can scale with a blit
    VkImage iStage[R]{};
    VkDeviceMemory iStageMem[R]{};
    uint64_t bypassFrames = 0;
    int failRun = 0;
    uint32_t pitch = 0;                 // bytes per row in the transfer block (256-aligned for D3D12)
    // DXGI output (useDx): D3D12 copies the transfer block into a flip-model swapchain
    bool useDx = false;
    IDXGISwapChain3* dxSwap = nullptr;
    ID3D12Resource* dxBack[3]{};
    ID3D12Heap* dxHeap[R]{};
    ID3D12Resource* dxBuf[R]{};
    ID3D12CommandAllocator* dxAlloc[R]{};
    ID3D12GraphicsCommandList* dxList[R]{};
    ID3D12Fence* dxFence = nullptr;
    HANDLE dxEvent = nullptr;
    HANDLE dxLatency = nullptr;          // frame latency waitable object: a back buffer is free when signalled
    uint64_t dxValue = 0, dxSlotValue[R]{};
    ID3D12Heap* trialDxHeap = nullptr;

    // ---- native (X-Plane's thread)
    bool nativeTried = false, nativeOk = false;
    std::string nativeWhyNot;
    VkSwapchainKHR nswap = VK_NULL_HANDLE;
    bool nRecreate = false;  // the last acquire timed out: recreate the NVIDIA swapchain before the next frame
    int nAcqTimeouts = 0;    // acquire timeouts in a row
    std::vector<VkImage> nImgs;
    std::vector<VkSemaphore> nReady;  // one per real swapchain image
    VkExtent2D nExt{};
    uint32_t nFamily = UINT32_MAX;
    VkQueue nQueue = VK_NULL_HANDLE;  // the queue X-Plane presents on (we use it only inside its present call)
    VkCommandPool nPool = VK_NULL_HANDLE;
    VkCommandBuffer nCb[R]{};
    VkFence nFence[R]{};
    bool nFenceUsed[R]{};
    VkSemaphore nAcq[R]{};
    uint32_t nRing = 0;
    VkQueryPool nQ = VK_NULL_HANDLE;
    uint64_t nMask = ~0ull;
    bool nTsPending[R]{};
    bool nBlitOk = false;

    Perf perf;
    int skip = 0;  // frames not counted after a path switch
};

static std::mutex g_vsLock;
static std::unordered_map<VkSwapchainKHR, VSwap*> g_vs;

static VSwap* vsOf(VkSwapchainKHR sc) {
    if (!sc) return nullptr;
    std::lock_guard<std::mutex> lk(g_vsLock);
    auto it = g_vs.find(sc);
    return it == g_vs.end() ? nullptr : it->second;
}

// ================================================================ session: path decisions and the A/B probe

enum ProbeState { PROBE_OFF = 0, PROBE_WAIT_STABLE, PROBE_RUN, PROBE_DONE };
struct Session {
    std::atomic<int> want{PATH_NATIVE};  // requested path; X-Plane's thread switches at the next present
    bool onIntel = false;                 // X-Plane's window is on a screen driven by the Intel GPU
    HMONITOR lastMon = nullptr;
    double lastCheck = 0, lastSwitch = -1e18, lastDxgi = -1e18;
    int pendingOnIntel = -1;              // a changed screen must be seen twice before switching
    // probe
    int probe = PROBE_OFF;
    int step = 0;
    double p50[4]{}, p95[4]{};
    double prevP50 = 0;
    int stableTries = 0, retries = 0;
    double lastProbe = -1e18, baseP95 = 0;
    int bad = 0;
    double lastLongGap = 0;   // end of the last frame gap > 2 s (a loading screen); probes start only after one
    double lastPresentT = 0;
    double probeStart = 0;
    int rounds = 0;           // probe rounds discarded because the two runs of a path disagreed
    double firstFrame = 0;
    std::string cacheKey;
};
static Session g_sess;
static const int kProbeOrder[4] = {PATH_NATIVE, PATH_BYPASS, PATH_NATIVE, PATH_BYPASS};

static std::wstring cachePath() { return g_dir + L"xpob_path_cache.txt"; }

static int cacheLookup(const std::string& k) {
    FILE* f = _wfopen(cachePath().c_str(), L"r");
    if (!f) return -1;
    char line[1024];
    int found = -1;
    while (fgets(line, sizeof(line), f)) {
        char* bar = strrchr(line, '|');
        if (!bar) continue;
        *bar = 0;
        if (k == line) found = atoi(bar + 1) == PATH_BYPASS ? PATH_BYPASS : PATH_NATIVE;
    }
    fclose(f);
    return found;
}

static void cacheStore(const std::string& k, int path) {
    std::vector<std::string> keep;
    if (FILE* f = _wfopen(cachePath().c_str(), L"r")) {
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            size_t bar = s.rfind('|');
            if (bar != std::string::npos && s.compare(0, bar, k) != 0) keep.push_back(s);
        }
        fclose(f);
    }
    if (FILE* f = _wfopen(cachePath().c_str(), L"w")) {
        for (auto& s : keep) fprintf(f, "%s\n", s.c_str());
        fprintf(f, "%s|%d\n", k.c_str(), path);
        fclose(f);
    } else {
        logf("[diag] could not write %s (the probe will run again next time)", cleanPath(cachePath()).c_str());
    }
}

// ================================================================ bypass resources

template <typename CreateBuf, typename MemReq, typename HostProps, typename Alloc, typename Bind>
static bool importHost(VkDevice dev, void* ptr, VkDeviceSize size, VkBufferUsageFlags usage,
                       const VkPhysicalDeviceMemoryProperties& mp, CreateBuf createBuf, MemReq memReq,
                       HostProps hostProps, Alloc alloc, Bind bind, VkBuffer* buf, VkDeviceMemory* mem) {
    VkMemoryHostPointerPropertiesEXT hp{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (hostProps(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, ptr, &hp) != VK_SUCCESS) return false;
    VkExternalMemoryBufferCreateInfo ebi{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    ebi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.pNext = &ebi;
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (createBuf(dev, &bci, nullptr, buf) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    memReq(dev, *buf, &mr);
    uint32_t type = memTypeAny(mp, mr.memoryTypeBits & hp.memoryTypeBits,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) return false;
    VkImportMemoryHostPointerInfoEXT imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    imp.pHostPointer = ptr;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &imp;
    ai.allocationSize = size;
    ai.memoryTypeIndex = type;
    if (alloc(dev, &ai, nullptr, mem) != VK_SUCCESS) return false;
    return bind(dev, *buf, *mem, 0) == VK_SUCCESS;
}

// a buffer in the device's own host-visible memory, mapped; readback prefers cached memory (fast CPU reads)
template <typename CreateBuf, typename MemReq, typename Alloc, typename Bind, typename Map>
static bool mappedBuffer(VkDevice dev, VkDeviceSize size, VkBufferUsageFlags usage, bool readback,
                         const VkPhysicalDeviceMemoryProperties& mp, CreateBuf createBuf, MemReq memReq, Alloc alloc,
                         Bind bind, Map map, VkBuffer* buf, VkDeviceMemory* mem, void** ptr, bool* coherent) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (createBuf(dev, &bci, nullptr, buf) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    memReq(dev, *buf, &mr);
    const VkMemoryPropertyFlags V = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, C = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                K = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    uint32_t type = readback ? memType(mp, mr.memoryTypeBits, V | K | C) : memType(mp, mr.memoryTypeBits, V | C);
    if (type == UINT32_MAX && readback) type = memType(mp, mr.memoryTypeBits, V | K);
    if (type == UINT32_MAX) type = memType(mp, mr.memoryTypeBits, V);
    if (type == UINT32_MAX) return false;
    *coherent = (mp.memoryTypes[type].propertyFlags & C) != 0;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = type;
    if (alloc(dev, &ai, nullptr, mem) != VK_SUCCESS) return false;
    if (bind(dev, *buf, *mem, 0) != VK_SUCCESS) return false;
    return map(dev, *mem, 0, VK_WHOLE_SIZE, 0, ptr) == VK_SUCCESS && *ptr;
}

static void intelFailure(VSwap* s, const char* what, VkResult r);

// ---- DXGI output, per virtual swapchain
// Wait until D3D12 no longer reads slot k (before the GeForce overwrites it). Never forever: 5 s, then the Intel
// side counts as hung.
static bool dxWaitValue(VSwap* s, uint64_t v) {
    if (!v || s->dxFence->GetCompletedValue() >= v) return true;
    if (g_intelHung) return false;
    s->dxFence->SetEventOnCompletion(v, s->dxEvent);
    if (WaitForSingleObject(s->dxEvent, 5000) == WAIT_OBJECT_0) return true;
    if (!g_intelHung.exchange(true)) logf("[hang] Intel D3D12 copy did not finish within 5 s -> not waiting on Intel any more");
    return false;
}
static bool dxWaitSlot(VSwap* s, uint32_t k) {
    bool ok = dxWaitValue(s, s->dxSlotValue[k]);
    s->dxSlotValue[k] = 0;
    return ok;
}
static void dxIdle(VSwap* s) {
    if (!s->dxFence || !g_dx || g_intelHung) return;
    g_dx->q->Signal(s->dxFence, ++s->dxValue);
    dxWaitValue(s, s->dxValue);
}
static void dxDropSwapchain(VSwap* s) {
    if (!s->dxSwap) return;
    dxIdle(s);
    for (auto& b : s->dxBack) rel(b);
    rel(s->dxSwap);
    if (s->dxLatency) { CloseHandle(s->dxLatency); s->dxLatency = nullptr; }
    for (auto& v : s->dxSlotValue) v = 0;
}
// Flip-model swapchain with the frame's size; DXGI stretches it to the window. X-Plane's thread only (creating
// a swapchain can send messages to the window, and the worker must never wait for X-Plane's message loop).
static bool dxEnsureSwapchain(VSwap* s) {
    if (s->dxSwap) return true;
    if (g_intelHung || GetCurrentThreadId() != g_xpThread.load()) return false;
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = s->ext.width;
    sd.Height = s->ext.height;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  // bytes stay sRGB-encoded, like X-Plane's own image
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 3;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    // The waitable object lets the worker wait for a free back buffer with a time limit, so Present itself does
    // not have to block when the compositor falls behind.
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (g_dx->tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    IDXGISwapChain1* sc1 = nullptr;
    HRESULT hr = E_FAIL;
    for (int tryN = 0; tryN < 10; ++tryN) {  // the previous swapchain on the window may take a moment to go away
        hr = g_dx->factory->CreateSwapChainForHwnd(g_dx->q, s->hwnd, &sd, nullptr, nullptr, &sc1);
        if (hr != E_ACCESSDENIED) break;
        Sleep(30);
    }
    if (FAILED(hr)) {
        static int n = 0;
        if (++n <= 5) logf("Intel: DXGI swapchain creation failed (0x%08lx)", (unsigned long)hr);
        return false;
    }
    g_dx->factory->MakeWindowAssociation(s->hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    hr = sc1->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&s->dxSwap);
    rel(sc1);
    if (FAILED(hr)) { logf("Intel: IDXGISwapChain3 missing (0x%08lx)", (unsigned long)hr); return false; }
    s->dxSwap->SetMaximumFrameLatency(2);
    s->dxLatency = s->dxSwap->GetFrameLatencyWaitableObject();
    for (UINT i = 0; i < 3; ++i)
        if (FAILED(s->dxSwap->GetBuffer(i, __uuidof(ID3D12Resource), (void**)&s->dxBack[i]))) {
            logf("Intel: DXGI back buffer %u unavailable", i);
            dxDropSwapchain(s);
            return false;
        }
    s->iext = s->ext;
    logf("Intel: DXGI flip swapchain %ux%u, 3 buffers, %s", s->ext.width, s->ext.height,
         g_cfg.vsync ? "vsync" : g_dx->tearing ? "no vsync (tearing allowed)" : "no vsync");
    return true;
}
// Worker thread: copy slot k into the next back buffer and present.
static void dxFrame(VSwap* s, uint32_t k) {
    if (!s->dxSwap) return;  // being (re)created on X-Plane's thread: drop this frame
    setWStage(WS_I_ACQUIRE);
    if (s->dxLatency) {
        DWORD w = WaitForSingleObject(s->dxLatency, 2000);
        if (w != WAIT_OBJECT_0) {  // no back buffer free for 2 s: the Intel output is stuck
            logf("[hang] DXGI: no free back buffer within 2 s -> treating the Intel side as failed");
            intelFailure(s, "DXGI back buffer wait (timeout)", VK_ERROR_DEVICE_LOST);
            return;
        }
    }
    setWStage(WS_I_SUBMIT);
    UINT idx = s->dxSwap->GetCurrentBackBufferIndex();
    ID3D12Resource* back = s->dxBack[idx % 3];
    s->dxAlloc[k]->Reset();
    ID3D12GraphicsCommandList* l = s->dxList[k];
    l->Reset(s->dxAlloc[k], nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = back;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    l->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = back;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    src.pResource = s->dxBuf[k];
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = 0;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = s->ext.width;
    src.PlacedFootprint.Footprint.Height = s->ext.height;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = s->pitch;
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    l->ResourceBarrier(1, &b);
    if (FAILED(l->Close())) { intelFailure(s, "D3D12 command list", VK_ERROR_DEVICE_LOST); return; }
    ID3D12CommandList* lists[] = {l};
    g_dx->q->ExecuteCommandLists(1, lists);
    setWStage(WS_I_PRESENT);
    double a = nowMs();
    UINT flags = (!g_cfg.vsync && g_dx->tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    HRESULT hr = s->dxSwap->Present(g_cfg.vsync ? 1 : 0, flags);
    double tPres = nowMs() - a;
    g_dx->q->Signal(s->dxFence, ++s->dxValue);
    s->dxSlotValue[k] = s->dxValue;
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        logf("Intel: DXGI present: device removed (0x%08lx, reason 0x%08lx)", (unsigned long)hr,
             (unsigned long)g_dx->dev->GetDeviceRemovedReason());
        intelFailure(s, "present", VK_ERROR_DEVICE_LOST);
        return;
    }
    if (FAILED(hr)) { intelFailure(s, "DXGI present", VK_ERROR_SURFACE_LOST_KHR); return; }
    s->failRun = 0;
    std::lock_guard<std::mutex> lk(s->perf.m);
    s->perf.iPres.add(tPres);
}
// Per-slot D3D12 objects; the transfer block is imported in place when possible.
static bool dxBuildSlots(VSwap* s, bool import, std::string* why) {
    ID3D12Device* dev = g_dx->dev;
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&s->dxFence))) {
        *why = "D3D12 fence";
        return false;
    }
    s->dxEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    for (int k = 0; k < VSwap::R; ++k) {
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = s->bytes;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        HRESULT hr;
        if (import) {
            hr = g_dx->dev3->OpenExistingHeapFromAddress(s->host[k], __uuidof(ID3D12Heap), (void**)&s->dxHeap[k]);
            if (FAILED(hr)) { *why = "D3D12 in-place import failed"; return false; }
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
            hr = dev->CreatePlacedResource(s->dxHeap[k], 0, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                           __uuidof(ID3D12Resource), (void**)&s->dxBuf[k]);
            if (FAILED(hr)) {
                rd.Flags = D3D12_RESOURCE_FLAG_NONE;
                hr = dev->CreatePlacedResource(s->dxHeap[k], 0, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               __uuidof(ID3D12Resource), (void**)&s->dxBuf[k]);
            }
            if (FAILED(hr)) {
                char b[96];
                snprintf(b, sizeof(b), "D3D12 buffer on the imported block failed (0x%08lx)", (unsigned long)hr);
                *why = b;
                return false;
            }
            s->iPtr[k] = s->host[k];
        } else {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_UPLOAD;
            hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                              __uuidof(ID3D12Resource), (void**)&s->dxBuf[k]);
            if (FAILED(hr)) { *why = "D3D12 upload buffer"; return false; }
            D3D12_RANGE none{0, 0};
            if (FAILED(s->dxBuf[k]->Map(0, &none, &s->iPtr[k]))) { *why = "D3D12 map"; return false; }
            s->iCoherent = true;
        }
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                               (void**)&s->dxAlloc[k])) ||
            FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s->dxAlloc[k], nullptr,
                                          __uuidof(ID3D12GraphicsCommandList), (void**)&s->dxList[k]))) {
            *why = "D3D12 command list";
            return false;
        }
        s->dxList[k]->Close();
    }
    return true;
}
static void dxDestroy(VSwap* s) {
    if (!s->useDx) return;
    if (!g_intelHung) dxIdle(s);
    for (auto& b : s->dxBack) rel(b);
    rel(s->dxSwap);
    if (s->dxLatency) { CloseHandle(s->dxLatency); s->dxLatency = nullptr; }
    for (int k = 0; k < VSwap::R; ++k) {
        rel(s->dxList[k]);
        rel(s->dxAlloc[k]);
        rel(s->dxBuf[k]);
        rel(s->dxHeap[k]);
    }
    rel(s->trialDxHeap);
    rel(s->dxFence);
    if (s->dxEvent) { CloseHandle(s->dxEvent); s->dxEvent = nullptr; }
}

// Wait for a GeForce fence, for at most kNvFenceSteps x 1 s. Windows resets a GPU whose work runs for more than
// about 2 s (TDR), so a fence that stays unsignaled for 10 s means the NVIDIA side is stuck. Then g_nvHung is set:
// the layer never waits on NVIDIA again, never frees or reuses memory the GPU might still write (it leaks it), and
// reports VK_ERROR_DEVICE_LOST to X-Plane instead of freezing. Steps are counted instead of reading the clock, so a
// sleep / resume of the PC does not count as waiting.
static constexpr int kNvFenceSteps = 10;
static bool nvFenceWait(DevData* d, VkFence f, const char* what) {
    if (g_nvHung) return d->vkWaitForFences(d->device, 1, &f, VK_TRUE, 0) == VK_SUCCESS;
    bool told = false;
    for (int step = 0;; ++step) {
        VkResult r = d->vkWaitForFences(d->device, 1, &f, VK_TRUE, 1000ull * 1000 * 1000);
        if (r == VK_SUCCESS) {
            if (told) logf("[hang] GeForce %s finished after about %d s", what, step + 1);
            return true;
        }
        if (r != VK_TIMEOUT) {
            logf("GeForce %s: wait failed (%d)%s", what, (int)r, r == VK_ERROR_DEVICE_LOST ? " - the NVIDIA GPU was reset" : "");
            g_nvHung = true;
            return false;
        }
        if (!told && step + 1 >= 3) {
            told = true;
            logf("[hang] GeForce %s: still waiting after 3 s (the NVIDIA GPU has not finished)", what);
        }
        if (step + 1 >= kNvFenceSteps) {
            if (!g_nvHung.exchange(true))
                logf("[hang] GeForce %s: no answer for %d s -> NVIDIA GPU treated as lost; the layer stops waiting on it "
                     "and reports a lost device to X-Plane", what, kNvFenceSteps);
            return false;
        }
    }
}
// Acquire an image of the layer's NVIDIA swapchain, for at most 2 s (4 x 0.5 s, counted). Returns VK_TIMEOUT then;
// the caller skips the frame and recreates the swapchain instead of blocking X-Plane's thread.
static VkResult nvAcquire(DevData* d, VkSwapchainKHR sc, VkSemaphore sem, uint32_t* idx) {
    for (int step = 0; step < 4; ++step) {
        VkResult r = d->vkAcquireNextImageKHR(d->device, sc, 500ull * 1000 * 1000, sem, VK_NULL_HANDLE, idx);
        if (r != VK_TIMEOUT && r != VK_NOT_READY) return r;
    }
    return VK_TIMEOUT;
}
// vkQueueWaitIdle has no time limit: submit a fence after all earlier work on the queue and wait for it with the
// nvFenceWait limit. Same thread rules as vkQueueWaitIdle (the caller owns the queue). False: GPU stuck.
static bool nvQueueIdle(DevData* d, VkQueue q, const char* what) {
    if (g_nvHung) return false;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence f = VK_NULL_HANDLE;
    if (d->vkCreateFence(d->device, &fci, nullptr, &f) != VK_SUCCESS) return false;
    VkResult r = d->vkQueueSubmit(q, 0, nullptr, f);
    bool ok = r == VK_SUCCESS && nvFenceWait(d, f, what);
    if (r != VK_SUCCESS) logf("GeForce: idle submit failed (%d)", (int)r);
    if (ok || r != VK_SUCCESS) d->vkDestroyFence(d->device, f, nullptr);  // else still pending: leak it
    return ok;
}

// Wait for an Intel fence, but never forever: an old or broken Intel driver must not freeze X-Plane. After 5 s
// the Intel device counts as hung; the layer then never waits on it again and uses the native path.
// (10 x 0.5 s, counted, so sleep / resume does not count as waiting)
static bool intelFenceWaitC(IntelCtx* c, VkFence f, const char* what) {
    if (g_intelHung) return false;
    for (int step = 0;; ++step) {
        VkResult r = c->vkWaitForFences(c->dev, 1, &f, VK_TRUE, 500ull * 1000 * 1000);
        if (r == VK_SUCCESS) return true;
        if (r != VK_TIMEOUT || step + 1 >= 10) {
            if (!g_intelHung.exchange(true))
                logf("[hang] Intel GPU did not finish %s (%s after about %.1f s) -> not waiting on Intel any more", what,
                     r == VK_TIMEOUT ? "timeout" : "error", (step + 1) * 0.5);
            return false;
        }
    }
}
static bool intelFenceWait(VSwap* s, VkFence f) { return intelFenceWaitC(s->ic, f, "a copy"); }

// vkDeviceWaitIdle has no time limit. The Intel device has one queue, so "idle" = a fence submitted after all
// earlier work has signaled; that fence is waited on with the 5 s limit. Returns false if the Intel GPU is hung.
static bool intelIdleDevice(IntelCtx* c) {
    if (!c || g_intelHung) return false;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence f = VK_NULL_HANDLE;
    if (c->vkCreateFence(c->dev, &fci, nullptr, &f) != VK_SUCCESS) return false;
    VkResult r = c->vkQueueSubmit(c->q, 0, nullptr, f);
    bool ok = r == VK_SUCCESS && intelFenceWaitC(c, f, "its earlier work");
    if (r != VK_SUCCESS) logf("Intel: idle submit failed (%d)", (int)r);
    if (ok || r != VK_SUCCESS) c->vkDestroyFence(c->dev, f, nullptr);  // else still pending: leak it
    return ok;
}

static void waitIntelIdle(VSwap* s) {
    IntelCtx* c = s->ic;
    if (!c) return;
    for (int k = 0; k < VSwap::R; ++k)
        if (s->iFenceUsed[k]) {
            if (intelFenceWait(s, s->iFence[k])) c->vkResetFences(c->dev, 1, &s->iFence[k]);
            s->iFenceUsed[k] = false;
        }
}

static void dropIntelSwapchain(VSwap* s) {
    if (s->useDx) { dxDropSwapchain(s); return; }
    if (!s->ic || !s->iswap) return;
    waitIntelIdle(s);
    intelIdleDevice(s->ic);
    s->ic->vkDestroySwapchainKHR(s->ic->dev, s->iswap, nullptr);  // even when hung: the window is needed for native
    s->iswap = VK_NULL_HANDLE;
    s->iImgs.clear();
    s->iRecreate = true;
}

static void destroyIntelStage(VSwap* s) {
    IntelCtx* c = s->ic;
    for (int k = 0; k < VSwap::R; ++k) {
        if (s->iStage[k]) c->vkDestroyImage(c->dev, s->iStage[k], nullptr);
        if (s->iStageMem[k]) c->vkFreeMemory(c->dev, s->iStageMem[k], nullptr);
        s->iStage[k] = VK_NULL_HANDLE;
        s->iStageMem[k] = VK_NULL_HANDLE;
    }
}

static bool createIntelStage(VSwap* s) {
    IntelCtx* c = s->ic;
    for (int k = 0; k < VSwap::R; ++k) {
        VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ic.imageType = VK_IMAGE_TYPE_2D;
        ic.format = s->iFmt.format;
        ic.extent = {s->ext.width, s->ext.height, 1};
        ic.mipLevels = 1;
        ic.arrayLayers = 1;
        ic.samples = VK_SAMPLE_COUNT_1_BIT;
        ic.tiling = VK_IMAGE_TILING_OPTIMAL;
        ic.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (c->vkCreateImage(c->dev, &ic, nullptr, &s->iStage[k]) != VK_SUCCESS) return false;
        VkMemoryRequirements mr;
        c->vkGetImageMemoryRequirements(c->dev, s->iStage[k], &mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = memTypeAny(c->mem, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (ai.memoryTypeIndex == UINT32_MAX) return false;
        if (c->vkAllocateMemory(c->dev, &ai, nullptr, &s->iStageMem[k]) != VK_SUCCESS) return false;
        if (c->vkBindImageMemory(c->dev, s->iStage[k], s->iStageMem[k], 0) != VK_SUCCESS) return false;
    }
    return true;
}

static void intelFailure(VSwap* s, const char* what, VkResult r);

// (Re)create the Intel swapchain if needed. Worker thread, or X-Plane's thread while the worker is idle.
static bool ensureIntelSwapchain(VSwap* s) {
    IntelCtx* c = s->ic;
    if (g_intelHung) return false;
    if (s->useDx) return dxEnsureSwapchain(s);
    if (s->iswap && !s->iRecreate) return true;
    VkSurfaceCapabilitiesKHR caps;
    VkResult cr = c->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(c->pd, c->surface, &caps);
    if (cr != VK_SUCCESS) { intelFailure(s, "surface query", cr); return false; }
    VkExtent2D e = caps.currentExtent;
    if (e.width == 0xFFFFFFFF) e = s->ext;
    if (e.width == 0 || e.height == 0) return false;  // minimized: try again later
    uint32_t cnt = caps.minImageCount < 3 ? 3 : caps.minImageCount;
    if (caps.maxImageCount && cnt > caps.maxImageCount) cnt = caps.maxImageCount;
    waitIntelIdle(s);
    VkSwapchainCreateInfoKHR sc{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sc.surface = c->surface;
    sc.minImageCount = cnt;
    sc.imageFormat = s->iFmt.format;
    sc.imageColorSpace = s->iFmt.colorSpace;
    sc.imageExtent = e;
    sc.imageArrayLayers = 1;
    sc.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sc.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sc.preTransform = caps.currentTransform;
    sc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sc.presentMode = s->iMode;
    sc.clipped = VK_TRUE;
    sc.oldSwapchain = s->iswap;
    VkSwapchainKHR ns = VK_NULL_HANDLE;
    VkResult r = c->vkCreateSwapchainKHR(c->dev, &sc, nullptr, &ns);
    if (r != VK_SUCCESS) {
        logf("[diag] Intel swapchain request: %ux%u format %d colorspace %d images %u (caps min %u max %u) present mode %d "
             "transform 0x%x old %s", e.width, e.height, (int)sc.imageFormat, (int)sc.imageColorSpace, cnt,
             caps.minImageCount, caps.maxImageCount, (int)sc.presentMode, (unsigned)sc.preTransform,
             s->iswap ? "yes" : "no");
        intelFailure(s, "swapchain creation", r);
        return false;
    }
    if (s->iswap) {
        intelIdleDevice(c);
        c->vkDestroySwapchainKHR(c->dev, s->iswap, nullptr);
    }
    s->iswap = ns;
    uint32_t n = 0;
    c->vkGetSwapchainImagesKHR(c->dev, ns, &n, nullptr);
    s->iImgs.resize(n);
    c->vkGetSwapchainImagesKHR(c->dev, ns, &n, s->iImgs.data());
    bool sizeChanged = e.width != s->iext.width || e.height != s->iext.height;
    s->iext = e;
    s->iRecreate = false;
    if (e.width != s->ext.width || e.height != s->ext.height) {
        // ask X-Plane to resize once per new window size; until then (or if it never does) the Intel side scales
        static uint64_t lastAsked = 0;
        uint64_t key2 = ((uint64_t)e.width << 32) | e.height;
        if (key2 != lastAsked) { lastAsked = key2; s->suggestResize = true; }
    }
    bool scale = (e.width != s->ext.width || e.height != s->ext.height || g_cfg.forceBlit);
    if (scale && s->iBlitOk && !s->iStage[0] && !createIntelStage(s)) {
        destroyIntelStage(s);
        logf("Intel: could not create the scaling image; copying the overlapping part only");
    }
    const char* mn = s->iMode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" :
                     s->iMode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE" : "FIFO (vsync)";
    if (sizeChanged)
        logf("Intel: swapchain %ux%u, %u images, %s%s", e.width, e.height, n, mn,
             scale ? (s->iStage[0] ? ", scaled by blit" : ", NOT scaled (blit unavailable)") : "");
    return true;
}

static void destroyBypass(VSwap* s) {
    DevData* d = s->d;
    VkDevice dev = d->device;
    dxDestroy(s);
    bool nvBusy = false;  // GeForce copy still unfinished: its memory must stay allocated
    for (int k = 0; k < VSwap::R; ++k)
        if (s->fenceUsed[k] && !nvFenceWait(d, s->fence[k], "copy (teardown)")) nvBusy = true;
    if (nvBusy) {
        logf("bypass teardown: GeForce copy unfinished -> its buffers are left allocated (leaked) instead of freed");
        return;
    }
    if (s->ic && !g_intelHung) {  // hung Intel device: leak its objects rather than block X-Plane
        IntelCtx* c = s->ic;
        waitIntelIdle(s);
        if (s->iswap && intelIdleDevice(c)) {
            c->vkDestroySwapchainKHR(c->dev, s->iswap, nullptr);
            s->iswap = VK_NULL_HANDLE;
        }
        if (!g_intelHung) {  // the idle wait above may have found the Intel GPU hung
        destroyIntelStage(s);
        for (int k = 0; k < VSwap::R; ++k) {
            if (s->iCb[k]) c->vkFreeCommandBuffers(c->dev, c->pool, 1, &s->iCb[k]);
            if (s->iFence[k]) c->vkDestroyFence(c->dev, s->iFence[k], nullptr);
            if (s->iAcq[k]) c->vkDestroySemaphore(c->dev, s->iAcq[k], nullptr);
            if (s->iReady[k]) c->vkDestroySemaphore(c->dev, s->iReady[k], nullptr);
            if (s->iBuf[k]) c->vkDestroyBuffer(c->dev, s->iBuf[k], nullptr);
            if (s->iMem[k]) c->vkFreeMemory(c->dev, s->iMem[k], nullptr);  // also unmaps
        }
        if (s->iQ) c->vkDestroyQueryPool(c->dev, s->iQ, nullptr);
        if (s->trialIBuf) c->vkDestroyBuffer(c->dev, s->trialIBuf, nullptr);
        if (s->trialIMem) c->vkFreeMemory(c->dev, s->trialIMem, nullptr);
        }
    }
    if (s->trialNvBuf) d->vkDestroyBuffer(dev, s->trialNvBuf, nullptr);
    if (s->trialNvMem) d->vkFreeMemory(dev, s->trialNvMem, nullptr);
    if (s->trialHost) VirtualFree(s->trialHost, 0, MEM_RELEASE);
    s->trialNvBuf = s->trialIBuf = VK_NULL_HANDLE;
    s->trialNvMem = s->trialIMem = VK_NULL_HANDLE;
    s->trialHost = nullptr;
    for (int k = 0; k < VSwap::R; ++k) {
        if (s->nvBuf[k]) d->vkDestroyBuffer(dev, s->nvBuf[k], nullptr);
        if (s->nvMem[k]) d->vkFreeMemory(dev, s->nvMem[k], nullptr);
        if (s->fence[k]) d->vkDestroyFence(dev, s->fence[k], nullptr);
        if (s->convImg[k]) d->vkDestroyImage(dev, s->convImg[k], nullptr);
        if (s->convMem[k]) d->vkFreeMemory(dev, s->convMem[k], nullptr);
        if (s->host[k]) VirtualFree(s->host[k], 0, MEM_RELEASE);
    }
    if (s->nvQ) d->vkDestroyQueryPool(dev, s->nvQ, nullptr);
    if (s->pool) d->vkDestroyCommandPool(dev, s->pool, nullptr);
}

static bool formatHas(DevData* d, VkFormat f, VkFormatFeatureFlags want) {
    if (!d->formatProps) return false;
    VkFormatProperties fp{};
    d->formatProps(d->pd, f, &fp);
    return (fp.optimalTilingFeatures & want) == want;
}

// Build everything the bypass needs for this swapchain. X-Plane's thread, worker idle. Returns false + reason.
static bool buildBypass(VSwap* s) {
    s->bypassTried = true;
    DevData* d = s->d;
    VkDevice dev = d->device;
    auto no = [&](const std::string& why) { s->bypassWhyNot = why; return false; };
    if (g_intelBroken) return no("the Intel side failed earlier in this session");
    if (s->ci.imageColorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) return no("X-Plane uses an HDR / non-sRGB color space");
    if (!bypassFormat(s->ci.imageFormat)) {
        char b[96];
        snprintf(b, sizeof(b), "swapchain format %d cannot be converted for the Intel GPU", (int)s->ci.imageFormat);
        return no(b);
    }
    if (!g_intel) g_intel = intelCreate(s->hwnd);
    if (!g_intel) return no("no usable Intel Vulkan device (" + g_intelWhyNot + ")");
    if (g_intel->hwnd != s->hwnd) return no("the Intel device belongs to another window");
    s->ic = g_intel;
    IntelCtx* c = s->ic;
    if (g_cfg.intelOut != 2) {
        if (!g_dx && !g_dxTried) {
            g_dx = dxCreate(c);
            if (!g_dx) logf("[diag] DXGI output unavailable: %s%s", g_dxWhyNot.c_str(),
                            g_cfg.intelOut == 1 ? "" : " -> Intel Vulkan output");
        }
        if (!g_dx && g_cfg.intelOut == 1) return no("intel_output=dxgi, but " + g_dxWhyNot);
        s->useDx = g_dx != nullptr;
    }

    // conversion on the GeForce: X-Plane's format -> BGRA8 with the same sRGB-ness (bytes stay sRGB-encoded)
    s->convFmt = isSrgb(s->ci.imageFormat) ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;
    s->conv = !isBgra8(s->ci.imageFormat) || g_cfg.forceBlit;
    if (s->conv) {
        if (!d->copyIsGraphics) return no("conversion needs a blit, but the copy queue is transfer-only");
        if (!formatHas(d, s->ci.imageFormat, VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
            !formatHas(d, s->convFmt, VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT))
            return no("the GeForce cannot blit this format");
    }

    // Intel surface format: BGRA8, preferably with the same sRGB-ness
    uint32_t n = 0;
    c->vkGetPhysicalDeviceSurfaceFormatsKHR(c->pd, c->surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(n);
    c->vkGetPhysicalDeviceSurfaceFormatsKHR(c->pd, c->surface, &n, fmts.data());
    std::string list;
    s->iFmt = {VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    for (auto& f : fmts) {
        list += std::to_string((int)f.format) + "/" + std::to_string((int)f.colorSpace) + " ";
        if (f.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR || !isBgra8(f.format)) continue;
        if (s->iFmt.format == VK_FORMAT_UNDEFINED || f.format == s->convFmt) s->iFmt = f;
    }
    logf("[diag] Intel surface formats (format/colorspace): %s", list.c_str());
    if (s->iFmt.format == VK_FORMAT_UNDEFINED && !s->useDx) return no("the Intel GPU offers no BGRA8 sRGB surface format");
    VkFormatProperties ifp{};
    c->vkGetPhysicalDeviceFormatProperties(c->pd, s->iFmt.format, &ifp);
    s->iBlitOk = c->famGraphics &&
                 (ifp.optimalTilingFeatures & (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) ==
                     (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT);

    // memory path
    bool nvImp = d->hostImport && !g_cfg.forceCpuCopy;
    bool iImp = s->useDx ? (g_dx->dev3 != nullptr) : (c->hostImport && !g_cfg.forceCpuCopy);
    VkDeviceSize align = std::max<VkDeviceSize>(65536, std::max(d->hostAlign, s->useDx ? 4096 : c->hostAlign));
    if (align > 65536) {  // VirtualAlloc only guarantees 64 KB
        logf("[diag] host import alignment %llu > 64 KB: not using host import", (unsigned long long)align);
        nvImp = iImp = false;
        align = 65536;
    }
    // always BGRA8 in the transfer buffer; D3D12 copies need 256-byte rows
    s->pitch = s->useDx ? ((s->ext.width * 4 + 255u) & ~255u) : s->ext.width * 4;
    VkDeviceSize raw = (VkDeviceSize)s->pitch * s->ext.height;
    s->bytes = (raw + align - 1) & ~(align - 1);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = d->copyFamily;
    if (d->vkCreateCommandPool(dev, &pci, nullptr, &s->pool) != VK_SUCCESS) return no("command pool");
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = s->pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = VSwap::R;
    if (d->vkAllocateCommandBuffers(dev, &cai, s->cb) != VK_SUCCESS) return no("command buffers");
    for (int k = 0; k < VSwap::R; ++k)
        if (d->setLoaderData(dev, s->cb[k]) != VK_SUCCESS) return no("loader data");
    VkCommandBufferAllocateInfo icai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    icai.commandPool = c->pool;
    icai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    icai.commandBufferCount = 1;

    // try importing one small block first, so all slots take the same memory path
    if (nvImp || iImp) {
        void* t = VirtualAlloc(nullptr, (SIZE_T)align, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (t) {
            VkBuffer b = VK_NULL_HANDLE;
            VkDeviceMemory m = VK_NULL_HANDLE;
            if (nvImp) {
                if (!importHost(dev, t, align, VK_BUFFER_USAGE_TRANSFER_DST_BIT, d->memProps, d->vkCreateBuffer,
                                d->vkGetBufferMemoryRequirements, d->vkGetMemoryHostPointerPropertiesEXT,
                                d->vkAllocateMemory, d->vkBindBufferMemory, &b, &m)) {
                    logf("[diag] GeForce could not import host memory -> its own mapped memory + CPU copy");
                    nvImp = false;
                }
                s->trialNvBuf = b;
                s->trialNvMem = m;
                b = VK_NULL_HANDLE;
                m = VK_NULL_HANDLE;
            }
            if (iImp && s->useDx) {
                HRESULT hr = g_dx->dev3->OpenExistingHeapFromAddress(t, __uuidof(ID3D12Heap), (void**)&s->trialDxHeap);
                if (FAILED(hr)) {
                    logf("[diag] D3D12 could not import host memory (0x%08lx) -> upload buffer + CPU copy",
                         (unsigned long)hr);
                    iImp = false;
                }
            } else if (iImp) {
                if (!importHost(c->dev, t, align, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, c->mem, c->vkCreateBuffer,
                                c->vkGetBufferMemoryRequirements, c->vkGetMemoryHostPointerPropertiesEXT,
                                c->vkAllocateMemory, c->vkBindBufferMemory, &b, &m)) {
                    logf("[diag] Intel could not import host memory -> its own mapped memory + CPU copy");
                    iImp = false;
                }
                s->trialIBuf = b;
                s->trialIMem = m;
            }
            s->trialHost = t;  // freed in destroyBypass, after both devices released it
        } else {
            nvImp = iImp = false;
        }
    }
    for (int k = 0; k < VSwap::R; ++k) {
        if (d->vkCreateFence(dev, &fci, nullptr, &s->fence[k]) != VK_SUCCESS) return no("fence");
        if (nvImp || iImp) {
            s->host[k] = VirtualAlloc(nullptr, (SIZE_T)s->bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!s->host[k]) return no("VirtualAlloc failed");
        }
        if (nvImp && !importHost(dev, s->host[k], s->bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, d->memProps,
                                 d->vkCreateBuffer, d->vkGetBufferMemoryRequirements,
                                 d->vkGetMemoryHostPointerPropertiesEXT, d->vkAllocateMemory, d->vkBindBufferMemory,
                                 &s->nvBuf[k], &s->nvMem[k]))
            return no("GeForce host memory import failed");
        if (nvImp) s->nvPtr[k] = s->host[k];
        else if (!mappedBuffer(dev, s->bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, d->memProps, d->vkCreateBuffer,
                               d->vkGetBufferMemoryRequirements, d->vkAllocateMemory, d->vkBindBufferMemory,
                               d->vkMapMemory, &s->nvBuf[k], &s->nvMem[k], &s->nvPtr[k], &s->nvCoherent))
            return no("GeForce host-visible buffer");
        if (s->useDx) {
            // D3D12 objects are built after this loop
        } else if (iImp && !importHost(c->dev, s->host[k], s->bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, c->mem,
                                c->vkCreateBuffer, c->vkGetBufferMemoryRequirements,
                                c->vkGetMemoryHostPointerPropertiesEXT, c->vkAllocateMemory, c->vkBindBufferMemory,
                                &s->iBuf[k], &s->iMem[k]))
            return no("Intel host memory import failed");
        if (s->useDx) {
        } else {
            if (iImp) s->iPtr[k] = s->host[k];
            else if (!mappedBuffer(c->dev, s->bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false, c->mem, c->vkCreateBuffer,
                                   c->vkGetBufferMemoryRequirements, c->vkAllocateMemory, c->vkBindBufferMemory,
                                   c->vkMapMemory, &s->iBuf[k], &s->iMem[k], &s->iPtr[k], &s->iCoherent))
                return no("Intel host-visible buffer");
            if (c->vkAllocateCommandBuffers(c->dev, &icai, &s->iCb[k]) != VK_SUCCESS) return no("Intel command buffer");
            if (c->vkCreateFence(c->dev, &fci, nullptr, &s->iFence[k]) != VK_SUCCESS) return no("Intel fence");
            if (c->vkCreateSemaphore(c->dev, &semci, nullptr, &s->iAcq[k]) != VK_SUCCESS) return no("Intel semaphore");
            if (c->vkCreateSemaphore(c->dev, &semci, nullptr, &s->iReady[k]) != VK_SUCCESS) return no("Intel semaphore");
        }
        if (s->conv) {
            VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ic.imageType = VK_IMAGE_TYPE_2D;
            ic.format = s->convFmt;
            ic.extent = {s->ext.width, s->ext.height, 1};
            ic.mipLevels = 1;
            ic.arrayLayers = 1;
            ic.samples = VK_SAMPLE_COUNT_1_BIT;
            ic.tiling = VK_IMAGE_TILING_OPTIMAL;
            ic.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (d->vkCreateImage(dev, &ic, nullptr, &s->convImg[k]) != VK_SUCCESS) return no("conversion image");
            VkMemoryRequirements mr;
            d->vkGetImageMemoryRequirements(dev, s->convImg[k], &mr);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = mr.size;
            ai.memoryTypeIndex = memTypeAny(d->memProps, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (ai.memoryTypeIndex == UINT32_MAX ||
                d->vkAllocateMemory(dev, &ai, nullptr, &s->convMem[k]) != VK_SUCCESS ||
                d->vkBindImageMemory(dev, s->convImg[k], s->convMem[k], 0) != VK_SUCCESS)
                return no("conversion image memory");
        }
    }
    if (s->useDx) {
        std::string why;
        if (!dxBuildSlots(s, iImp, &why)) return no(why);
    }
    s->mem = (nvImp && iImp) ? MEM_SHARED : nvImp ? MEM_NV_IMPORT : iImp ? MEM_INTEL_IMPORT : MEM_CPU;

    // GPU timestamps on both sides (if the queue families support them)
    VkQueryPoolCreateInfo qp{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qp.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qp.queryCount = 2 * VSwap::R;
    uint32_t nvBits = d->copyFamily < d->families.size() ? d->families[d->copyFamily].timestampValidBits : 0;
    if (nvBits && d->vkCreateQueryPool && d->vkCreateQueryPool(dev, &qp, nullptr, &s->nvQ) == VK_SUCCESS) {
        s->nvPeriod = d->props.limits.timestampPeriod;
        s->nvMask = nvBits >= 64 ? ~0ull : ((1ull << nvBits) - 1);
    }
    if (!s->useDx && c->tsBits && c->vkCreateQueryPool(c->dev, &qp, nullptr, &s->iQ) == VK_SUCCESS)
        s->iMask = c->tsBits >= 64 ? ~0ull : ((1ull << c->tsBits) - 1);

    // present mode
    c->vkGetPhysicalDeviceSurfacePresentModesKHR(c->pd, c->surface, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    c->vkGetPhysicalDeviceSurfacePresentModesKHR(c->pd, c->surface, &n, modes.data());
    s->iMode = VK_PRESENT_MODE_FIFO_KHR;
    if (!g_cfg.vsync) {
        for (auto m : modes) if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) s->iMode = m;
        for (auto m : modes) if (m == VK_PRESENT_MODE_MAILBOX_KHR) s->iMode = m;
    }
    s->bypassOk = true;
    if (s->useDx)
        logf("[diag] bypass ready: memory %s | GeForce conversion %s | Intel output DXGI flip (D3D12), row pitch %u | "
             "GPU timestamps GeForce %s", memName(s->mem), s->conv ? "blit to BGRA8" : "none (BGRA8)", s->pitch,
             s->nvQ ? "yes" : "no");
    else
        logf("[diag] bypass ready: memory %s | GeForce conversion %s | Intel output Vulkan, format %d, scaling blit %s | "
             "GPU timestamps GeForce %s, Intel %s", memName(s->mem), s->conv ? "blit to BGRA8" : "none (BGRA8)",
             (int)s->iFmt.format, s->iBlitOk ? "available" : "unavailable", s->nvQ ? "yes" : "no", s->iQ ? "yes" : "no");
    return true;
}

// ================================================================ native path (layer-owned NVIDIA swapchain)

static void destroyNativeSwapchain(VSwap* s) {
    DevData* d = s->d;
    for (int k = 0; k < VSwap::R; ++k)
        if (s->nFenceUsed[k]) {
            if (!nvFenceWait(d, s->nFence[k], "native copy")) return;  // GPU stuck: leave everything allocated
            d->vkResetFences(d->device, 1, &s->nFence[k]);
            s->nFenceUsed[k] = false;
        }
    if (g_nvHung) return;
    if (s->nswap) {
        // presents of the old images must be finished; time-limited instead of vkQueueWaitIdle
        if (s->nQueue && !nvQueueIdle(d, s->nQueue, "native presents")) return;  // stuck: leave it allocated
        d->vkDestroySwapchainKHR(d->device, s->nswap, nullptr);
        s->nswap = VK_NULL_HANDLE;
    }
    for (VkSemaphore sem : s->nReady) d->vkDestroySemaphore(d->device, sem, nullptr);
    s->nReady.clear();
    s->nImgs.clear();
}

static void destroyNative(VSwap* s) {
    DevData* d = s->d;
    destroyNativeSwapchain(s);
    if (g_nvHung) return;  // fences may still be in use by the stuck GPU: leak them
    for (int k = 0; k < VSwap::R; ++k) {
        if (s->nFence[k]) d->vkDestroyFence(d->device, s->nFence[k], nullptr);
        if (s->nAcq[k]) d->vkDestroySemaphore(d->device, s->nAcq[k], nullptr);
    }
    if (s->nQ) d->vkDestroyQueryPool(d->device, s->nQ, nullptr);
    if (s->nPool) d->vkDestroyCommandPool(d->device, s->nPool, nullptr);
}

static bool createNativeSwapchain(VSwap* s) {
    DevData* d = s->d;
    s->nativeWhyNot.clear();
    VkSurfaceCapabilitiesKHR caps{};
    if (!d->surfCaps || d->surfCaps(d->pd, s->surface, &caps) != VK_SUCCESS) {
        s->nativeWhyNot = "surface capabilities";
        return false;
    }
    VkExtent2D e = caps.currentExtent;
    if (e.width == 0xFFFFFFFF) e = s->ext;
    if (e.width == 0 || e.height == 0) return false;  // minimized: try again later
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        s->nativeWhyNot = "NVIDIA swapchain images cannot be a copy destination";
        return false;
    }
    VkSwapchainCreateInfoKHR sc = s->ci;
    sc.pNext = nullptr;
    sc.surface = s->surface;
    sc.imageExtent = e;
    sc.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sc.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sc.queueFamilyIndexCount = 0;
    sc.pQueueFamilyIndices = nullptr;
    sc.oldSwapchain = s->nswap;
    if (sc.minImageCount < caps.minImageCount) sc.minImageCount = caps.minImageCount;
    if (caps.maxImageCount && sc.minImageCount > caps.maxImageCount) sc.minImageCount = caps.maxImageCount;
    VkSwapchainKHR ns = VK_NULL_HANDLE;
    VkResult r = d->vkCreateSwapchainKHR(d->device, &sc, nullptr, &ns);
    if (r != VK_SUCCESS) {
        char b[64];
        snprintf(b, sizeof(b), "NVIDIA swapchain creation failed (%d)", (int)r);
        s->nativeWhyNot = b;
        return false;
    }
    destroyNativeSwapchain(s);
    s->nswap = ns;
    uint32_t n = 0;
    d->vkGetSwapchainImagesKHR(d->device, ns, &n, nullptr);
    s->nImgs.resize(n);
    d->vkGetSwapchainImagesKHR(d->device, ns, &n, s->nImgs.data());
    VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    s->nReady.assign(n, VK_NULL_HANDLE);
    for (auto& sem : s->nReady) d->vkCreateSemaphore(d->device, &semci, nullptr, &sem);
    bool changed = e.width != s->nExt.width || e.height != s->nExt.height;
    s->nExt = e;
    if (changed) {
        bool scale = e.width != s->ext.width || e.height != s->ext.height;
        logf("NVIDIA (native): swapchain %ux%u, %u images%s", e.width, e.height, n,
             scale ? (s->nBlitOk ? ", scaled by blit" : ", NOT scaled (blit unavailable)") : "");
        if (scale) s->suggestResize = true;
    }
    return true;
}

// Resources that do not depend on the queue X-Plane presents on. X-Plane's thread.
static bool buildNativeBase(VSwap* s) {
    DevData* d = s->d;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (int k = 0; k < VSwap::R; ++k) {
        if (!s->nFence[k] && d->vkCreateFence(d->device, &fci, nullptr, &s->nFence[k]) != VK_SUCCESS) return false;
        if (!s->nAcq[k] && d->vkCreateSemaphore(d->device, &semci, nullptr, &s->nAcq[k]) != VK_SUCCESS) return false;
    }
    s->nBlitOk = formatHas(d, s->ci.imageFormat, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT);
    return true;
}

// Command pool + timestamps for the queue family X-Plane presents on (known only at the first present).
static bool nativeForFamily(VSwap* s, uint32_t fam) {
    DevData* d = s->d;
    if (s->nFamily == fam) return true;
    if (fam >= d->families.size() || !(d->families[fam].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
        s->nativeWhyNot = "X-Plane presents on a queue without graphics support";
        return false;
    }
    if (d->copyIsGraphics && fam != d->copyFamily) {
        s->nativeWhyNot = "X-Plane presents on another queue family than the one it renders with";
        return false;
    }
    if (s->nPool) {
        for (int k = 0; k < VSwap::R; ++k)
            if (s->nFenceUsed[k]) {
                if (!nvFenceWait(d, s->nFence[k], "native copy")) {
                    s->nativeWhyNot = "the NVIDIA GPU does not finish its work";
                    return false;
                }
                d->vkResetFences(d->device, 1, &s->nFence[k]);
                s->nFenceUsed[k] = false;
            }
        d->vkDestroyCommandPool(d->device, s->nPool, nullptr);
        s->nPool = VK_NULL_HANDLE;
    }
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = fam;
    if (d->vkCreateCommandPool(d->device, &pci, nullptr, &s->nPool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = s->nPool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = VSwap::R;
    if (d->vkAllocateCommandBuffers(d->device, &cai, s->nCb) != VK_SUCCESS) return false;
    for (int k = 0; k < VSwap::R; ++k)
        if (d->setLoaderData(d->device, s->nCb[k]) != VK_SUCCESS) return false;
    uint32_t bits = d->families[fam].timestampValidBits;
    if (!s->nQ && bits && d->vkCreateQueryPool) {
        VkQueryPoolCreateInfo qp{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qp.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qp.queryCount = 2 * VSwap::R;
        if (d->vkCreateQueryPool(d->device, &qp, nullptr, &s->nQ) == VK_SUCCESS)
            s->nMask = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
    }
    s->nFamily = fam;
    return true;
}

static void destroyVSwap(VSwap* s) {
    DevData* d = s->d;
    destroyNative(s);
    destroyBypass(s);
    for (auto& vi : s->v) {
        if (vi.img) d->vkDestroyImage(d->device, vi.img, nullptr);
        if (vi.mem) d->vkFreeMemory(d->device, vi.mem, nullptr);
        if (vi.copyDone) d->vkDestroySemaphore(d->device, vi.copyDone, nullptr);
    }
    delete s;
}

// ================================================================ command recording

static double tsMs(uint64_t a, uint64_t b, uint64_t mask, float period) {
    return (double)((b - a) & mask) * period / 1e6;
}

static void barrier(PFN_vkCmdPipelineBarrier fn, VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
                    VkAccessFlags srcA, VkAccessFlags dstA, VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = srcA;
    b.dstAccessMask = dstA;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    fn(cb, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
}

static VkImageBlit fullBlit(VkExtent2D a, VkExtent2D b) {
    VkImageBlit r{};
    r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.srcOffsets[1] = {(int32_t)a.width, (int32_t)a.height, 1};
    r.dstOffsets[1] = {(int32_t)b.width, (int32_t)b.height, 1};
    return r;
}

static void recordToHost(DevData* d, VSwap* s, uint32_t k, VkImage src) {
    VkCommandBuffer cb = s->cb[k];
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    d->vkResetCommandBuffer(cb, 0);
    d->vkBeginCommandBuffer(cb, &bi);
    if (s->nvQ) {
        d->vkCmdResetQueryPool(cb, s->nvQ, 2 * k, 2);
        d->vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s->nvQ, 2 * k);
    }
    barrier(d->vkCmdPipelineBarrier, cb, src, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImage from = src;
    if (s->conv) {
        barrier(d->vkCmdPipelineBarrier, cb, s->convImg[k], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageBlit r = fullBlit(s->ext, s->ext);
        d->vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->convImg[k],
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r, VK_FILTER_NEAREST);
        barrier(d->vkCmdPipelineBarrier, cb, s->convImg[k], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        from = s->convImg[k];
    }
    VkBufferImageCopy c{};
    c.bufferRowLength = s->pitch / 4;  // texels; equals the width unless rows are padded for D3D12
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {s->ext.width, s->ext.height, 1};
    d->vkCmdCopyImageToBuffer(cb, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->nvBuf[k], 1, &c);
    barrier(d->vkCmdPipelineBarrier, cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_READ_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    d->vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0,
                            nullptr);
    if (s->nvQ) d->vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s->nvQ, 2 * k + 1);
    d->vkEndCommandBuffer(cb);
}

static void recordFromHost(IntelCtx* c, VSwap* s, uint32_t k, VkImage dst) {
    VkCommandBuffer cb = s->iCb[k];
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    c->vkResetCommandBuffer(cb, 0);
    c->vkBeginCommandBuffer(cb, &bi);
    if (s->iQ) {
        c->vkCmdResetQueryPool(cb, s->iQ, 2 * k, 2);
        c->vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s->iQ, 2 * k);
    }
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    c->vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0,
                            nullptr);
    bool scale = s->iStage[0] && (s->iext.width != s->ext.width || s->iext.height != s->ext.height || g_cfg.forceBlit);
    barrier(c->vkCmdPipelineBarrier, cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy r{};
    r.bufferRowLength = s->ext.width;
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    if (scale) {
        barrier(c->vkCmdPipelineBarrier, cb, s->iStage[k], VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        r.imageExtent = {s->ext.width, s->ext.height, 1};
        c->vkCmdCopyBufferToImage(cb, s->iBuf[k], s->iStage[k], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        barrier(c->vkCmdPipelineBarrier, cb, s->iStage[k], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageBlit b = fullBlit(s->ext, s->iext);
        c->vkCmdBlitImage(cb, s->iStage[k], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
    } else {
        r.imageExtent = {std::min(s->iext.width, s->ext.width), std::min(s->iext.height, s->ext.height), 1};
        c->vkCmdCopyBufferToImage(cb, s->iBuf[k], dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    }
    barrier(c->vkCmdPipelineBarrier, cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    if (s->iQ) c->vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s->iQ, 2 * k + 1);
    c->vkEndCommandBuffer(cb);
}

static void recordNative(DevData* d, VSwap* s, uint32_t k, VkImage src, VkImage dst) {
    VkCommandBuffer cb = s->nCb[k];
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    d->vkResetCommandBuffer(cb, 0);
    d->vkBeginCommandBuffer(cb, &bi);
    if (s->nQ) {
        d->vkCmdResetQueryPool(cb, s->nQ, 2 * k, 2);
        d->vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s->nQ, 2 * k);
    }
    barrier(d->vkCmdPipelineBarrier, cb, src, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    barrier(d->vkCmdPipelineBarrier, cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    bool same = s->nExt.width == s->ext.width && s->nExt.height == s->ext.height && !g_cfg.forceBlit;
    if (!same && s->nBlitOk) {
        VkImageBlit b = fullBlit(s->ext, s->nExt);
        d->vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                          VK_FILTER_LINEAR);
    } else {
        VkImageCopy c{};
        c.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.extent = {std::min(s->ext.width, s->nExt.width), std::min(s->ext.height, s->nExt.height), 1};
        d->vkCmdCopyImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    }
    barrier(d->vkCmdPipelineBarrier, cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_READ_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    barrier(d->vkCmdPipelineBarrier, cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    if (s->nQ) d->vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s->nQ, 2 * k + 1);
    d->vkEndCommandBuffer(cb);
}

// ================================================================ the bypass frame, on the worker thread

static void releasePending(DevData* d, const std::vector<VkSemaphore>& waits) {
    std::lock_guard<std::mutex> lk(d->semLock);
    for (VkSemaphore sem : waits) {
        auto it = d->pendingWaits.find(sem);
        if (it != d->pendingWaits.end()) d->pendingWaits.erase(it);
    }
}

// An Intel-side failure while flying: hard errors switch to the native path at once, others after a run of
// consecutive failed frames. The switch happens inside the layer (X-Plane does not recreate anything).
static void intelFailure(VSwap* s, const char* what, VkResult r) {
    bool hard = r == VK_ERROR_DEVICE_LOST || r == VK_ERROR_SURFACE_LOST_KHR;
    if (++s->failRun <= 5 || hard) logf("Intel: %s failed (%d)", what, (int)r);
    if ((hard || s->failRun >= 30) && !g_intelBroken.exchange(true)) {
        logf("Intel side not working -> switching to the native NVIDIA path inside the layer (X-Plane keeps running)");
        g_sess.want = PATH_NATIVE;
    }
}

static void frameJob(VSwap* s, uint32_t i, std::shared_ptr<std::vector<VkSemaphore>> waits) {
    DevData* d = s->d;
    IntelCtx* c = s->ic;
    VkDevice dev = d->device;
    uint32_t k = s->ring++ % VSwap::R;
    if (s->fenceUsed[k]) {
        setWStage(WS_OLD_GF);
        if (!nvFenceWait(d, s->fence[k], "copy")) {  // NVIDIA stuck: drop this frame, slot stays in use
            s->err = (int)VK_ERROR_DEVICE_LOST;
            releasePending(d, *waits);
            {
                std::lock_guard<std::mutex> lk(s->m);
                if (i < s->v.size()) { s->v[i].copyPending = false; s->v[i].state = 0; }
            }
            s->cv.notify_all();
            setWStage(WS_IDLE);
            return;
        }
        d->vkResetFences(dev, 1, &s->fence[k]);
        s->fenceUsed[k] = false;
    }
    if (s->useDx && s->dxSlotValue[k]) {
        setWStage(WS_OLD_INTEL);
        if (!dxWaitSlot(s, k)) intelFailure(s, "D3D12 copy (no answer)", VK_ERROR_DEVICE_LOST);
    }
    if (s->iFenceUsed[k]) {
        setWStage(WS_OLD_INTEL);
        if (intelFenceWait(s, s->iFence[k])) c->vkResetFences(c->dev, 1, &s->iFence[k]);
        else { intelFailure(s, "copy (no answer)", VK_ERROR_DEVICE_LOST); s->iTsPending[k] = false; }
        s->iFenceUsed[k] = false;
    }
    if (s->iTsPending[k] && !g_intelHung) {  // Intel GPU time of the frame that used this slot last
        uint64_t t[2] = {0, 0};
        if (c->vkGetQueryPoolResults(c->dev, s->iQ, 2 * k, 2, sizeof(t), t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) ==
            VK_SUCCESS) {
            std::lock_guard<std::mutex> lk(s->perf.m);
            s->perf.iGpu.add(tsMs(t[0], t[1], s->iMask, c->tsPeriod));
        }
        s->iTsPending[k] = false;
    }
    bool okImg = i < s->v.size();

    // 1) GeForce: X-Plane's frame -> transfer buffer (waits for X-Plane's rendering on the GPU)
    std::vector<VkSemaphore> w(*waits);
    std::vector<VkPipelineStageFlags> stages(w.size(), VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkSemaphore sig = okImg ? s->v[i].copyDone : VK_NULL_HANDLE;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = (uint32_t)w.size();
    si.pWaitSemaphores = w.data();
    si.pWaitDstStageMask = stages.data();
    if (okImg) {
        recordToHost(d, s, k, s->v[i].img);
        si.commandBufferCount = 1;
        si.pCommandBuffers = &s->cb[k];
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &sig;
    }
    VkResult sr;
    setWStage(WS_GF_SUBMIT);
    {
        std::lock_guard<std::mutex> lk(d->copyQLock);
        sr = d->vkQueueSubmit(d->copyQueue, 1, &si, s->fence[k]);
    }
    s->fenceUsed[k] = (sr == VK_SUCCESS);
    if (sr < 0) { s->err = (int)sr; logf("GeForce copy submit failed (%d)", (int)sr); }
    releasePending(d, *waits);
    {
        std::lock_guard<std::mutex> lk(s->m);
        if (okImg) { s->v[i].copyPending = (sr == VK_SUCCESS); s->v[i].state = 0; }
    }
    s->cv.notify_all();
    if (!okImg || !s->fenceUsed[k]) { setWStage(WS_IDLE); return; }

    // 2) wait until the frame is in the transfer buffer
    setWStage(WS_GF_FENCE);
    double a = nowMs();
    if (!nvFenceWait(d, s->fence[k], "copy")) {  // fence stays marked used: the slot is never reused
        s->err = (int)VK_ERROR_DEVICE_LOST;
        setWStage(WS_IDLE);
        return;
    }
    d->vkResetFences(dev, 1, &s->fence[k]);
    s->fenceUsed[k] = false;
    double tFence = nowMs() - a, tGpu = -1, tCpu = -1;
    if (s->nvQ) {
        uint64_t t[2] = {0, 0};
        if (d->vkGetQueryPoolResults(dev, s->nvQ, 2 * k, 2, sizeof(t), t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) ==
            VK_SUCCESS)
            tGpu = tsMs(t[0], t[1], s->nvMask, s->nvPeriod);
    }
    if (g_intelBroken) { setWStage(WS_IDLE); return; }  // waiting for X-Plane's thread to switch to native

    // 3) CPU copy when the two GPUs do not share one block of memory
    if (s->nvPtr[k] != s->iPtr[k]) {
        setWStage(WS_CPU_COPY);
        a = nowMs();
        if (!s->nvCoherent) {
            VkMappedMemoryRange mr{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            mr.memory = s->nvMem[k];
            mr.size = VK_WHOLE_SIZE;
            d->vkInvalidateMappedMemoryRanges(dev, 1, &mr);
        }
        memcpy(s->iPtr[k], s->nvPtr[k], (size_t)((VkDeviceSize)s->pitch * s->ext.height));
        if (!s->iCoherent) {
            VkMappedMemoryRange mr{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            mr.memory = s->iMem[k];
            mr.size = VK_WHOLE_SIZE;
            c->vkFlushMappedMemoryRanges(c->dev, 1, &mr);
        }
        tCpu = nowMs() - a;
    }

    // 4) Intel: acquire, copy, present
    s->bypassFrames++;
    if (g_cfg.failIntelAfter && s->bypassFrames == (uint64_t)g_cfg.failIntelAfter) {
        logf("[diag] test_fail_intel_after=%d: simulating an Intel device loss", g_cfg.failIntelAfter);
        intelFailure(s, "simulated (test switch)", VK_ERROR_DEVICE_LOST);
        return;
    }
    if (s->useDx) {
        dxFrame(s, k);
        setWStage(WS_IDLE);
        std::lock_guard<std::mutex> lk(s->perf.m);
        s->perf.gfFence.add(tFence);
        if (tGpu >= 0) s->perf.gfGpu.add(tGpu);
        if (tCpu >= 0) s->perf.cpuCopy.add(tCpu);
        return;
    }
    setWStage(WS_I_SWAPCHAIN);
    if (!ensureIntelSwapchain(s)) { setWStage(WS_IDLE); return; }  // minimized (or failed): drop this frame
    uint32_t r = 0;
    setWStage(WS_I_ACQUIRE);
    a = nowMs();
    const uint64_t kAcqTimeout = 2000ull * 1000 * 1000;  // 2 s: an Intel acquire must never freeze X-Plane
    VkResult ar = c->vkAcquireNextImageKHR(c->dev, s->iswap, kAcqTimeout, s->iAcq[k], VK_NULL_HANDLE, &r);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) {
        s->iRecreate = true;
        setWStage(WS_I_SWAPCHAIN);
        if (!ensureIntelSwapchain(s)) { setWStage(WS_IDLE); return; }
        setWStage(WS_I_ACQUIRE);
        ar = c->vkAcquireNextImageKHR(c->dev, s->iswap, kAcqTimeout, s->iAcq[k], VK_NULL_HANDLE, &r);
    }
    double tAcq = nowMs() - a;
    if (ar == VK_TIMEOUT || ar == VK_NOT_READY) {
        logf("[hang] Intel acquire gave no image within 2 s -> treating the Intel side as failed");
        intelFailure(s, "acquire (timeout)", VK_ERROR_DEVICE_LOST);
        setWStage(WS_IDLE);
        return;
    }
    if (ar < 0 || r >= s->iImgs.size()) {
        s->iRecreate = true;
        intelFailure(s, "acquire", ar < 0 ? ar : VK_ERROR_UNKNOWN);
        setWStage(WS_IDLE);
        return;
    }
    if (ar == VK_SUBOPTIMAL_KHR) s->iRecreate = true;
    recordFromHost(c, s, k, s->iImgs[r]);
    VkPipelineStageFlags st = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo is{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    is.waitSemaphoreCount = 1;
    is.pWaitSemaphores = &s->iAcq[k];
    is.pWaitDstStageMask = &st;
    is.commandBufferCount = 1;
    is.pCommandBuffers = &s->iCb[k];
    is.signalSemaphoreCount = 1;
    is.pSignalSemaphores = &s->iReady[k];
    setWStage(WS_I_SUBMIT);
    VkResult isr = c->vkQueueSubmit(c->q, 1, &is, s->iFence[k]);
    if (isr != VK_SUCCESS) { intelFailure(s, "submit", isr); setWStage(WS_IDLE); return; }
    s->iFenceUsed[k] = true;
    s->iTsPending[k] = s->iQ != VK_NULL_HANDLE;
    VkPresentInfoKHR p{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    p.waitSemaphoreCount = 1;
    p.pWaitSemaphores = &s->iReady[k];
    p.swapchainCount = 1;
    p.pSwapchains = &s->iswap;
    p.pImageIndices = &r;
    setWStage(WS_I_PRESENT);
    a = nowMs();
    VkResult pr = c->vkQueuePresentKHR(c->q, &p);
    double tPres = nowMs() - a;
    setWStage(WS_IDLE);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) s->iRecreate = true;
    if (pr < 0 && pr != VK_ERROR_OUT_OF_DATE_KHR) intelFailure(s, "present", pr);
    else s->failRun = 0;

    std::lock_guard<std::mutex> lk(s->perf.m);
    s->perf.gfFence.add(tFence);
    if (tGpu >= 0) s->perf.gfGpu.add(tGpu);
    if (tCpu >= 0) s->perf.cpuCopy.add(tCpu);
    s->perf.iAcq.add(tAcq);
    s->perf.iPres.add(tPres);
}

// ================================================================ what X-Plane calls

// timeoutNs as given to vkAcquireNextImage(2)KHR: 0 = only check, UINT64_MAX = wait until an image is free.
static VkResult vAcquire(VSwap* s, uint64_t timeoutNs, VkSemaphore sem, VkFence fence, uint32_t* pIndex) {
    DevData* d = s->d;
    if (g_virtualBroken) return VK_ERROR_OUT_OF_DATE_KHR;  // X-Plane recreates -> real NVIDIA swapchain
    if (g_nvHung || g_workerStuck) return VK_ERROR_DEVICE_LOST;
    static bool toldAcq = false;
    if (!toldAcq) {  // what X-Plane asks for when it takes an image (does it wait on our fence before recording?)
        toldAcq = true;
        logf("[diag] X-Plane acquire: timeout %s, semaphore %s, fence %s", timeoutNs == UINT64_MAX ? "infinite" :
             std::to_string(timeoutNs).c_str(), sem ? "yes" : "no", fence ? "yes" : "no");
    }
    double t0 = nowMs();
    int e = s->err.exchange(0);
    if (e < 0) return (VkResult)e;
    uint32_t i = 0;
    bool needWait = false;
    double waited = 0;
    {
        std::unique_lock<std::mutex> lk(s->m);
        double w0 = nowMs();
        const uint32_t V = (uint32_t)s->v.size();
        const bool forever = timeoutNs == UINT64_MAX;
        const double limitMs = forever ? 0.0 : (double)timeoutNs / 1e6;
        for (;;) {
            bool found = false;
            for (uint32_t n = 0; n < V; ++n) {
                uint32_t j = (s->nextV + n) % V;
                if (s->v[j].state == 0) { i = j; found = true; break; }
            }
            if (found) break;
            double left = forever ? 200.0 : limitMs - (nowMs() - w0);
            if (left <= 0) {  // as the spec says: nothing signaled, no image index returned
                g_xStage = XS_OUTSIDE;
                return timeoutNs == 0 ? VK_NOT_READY : VK_TIMEOUT;
            }
            g_xStage = XS_ACQUIRE_WAIT;
            s->cv.wait_for(lk, std::chrono::microseconds((long long)(std::min(left, 200.0) * 1000.0) + 1));
        }
        g_xStage = XS_OUTSIDE;
        waited = nowMs() - w0;
        s->v[i].state = 1;
        s->nextV = (i + 1) % V;
        needWait = s->v[i].copyPending;
        s->v[i].copyPending = false;
    }
    if (sem || fence || needWait) {
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = needWait ? 1 : 0;
        si.pWaitSemaphores = &s->v[i].copyDone;
        si.pWaitDstStageMask = &stage;
        si.signalSemaphoreCount = sem ? 1 : 0;
        si.pSignalSemaphores = &sem;
        VkResult r;
        {
            std::lock_guard<std::mutex> lk(d->copyQLock);
            r = d->vkQueueSubmit(d->copyQueue, 1, &si, fence);
        }
        if (r < 0) {  // nothing was submitted: give the image back so it is not lost for the rest of the session
            {
                std::lock_guard<std::mutex> lk(s->m);
                s->v[i].state = 0;
                s->v[i].copyPending = needWait;
            }
            s->cv.notify_all();
            return r;
        }
    }
    *pIndex = i;
    std::lock_guard<std::mutex> lk(s->perf.m);
    s->perf.xpWait.add(waited);
    s->perf.layer.add(nowMs() - t0 - waited);
    return VK_SUCCESS;
}

// Consume X-Plane's wait semaphores and release the image when a frame cannot be shown.
static void dropFrame(VSwap* s, VkQueue queue, uint32_t i, const VkPresentInfoKHR* pi) {
    DevData* d = s->d;
    std::vector<VkPipelineStageFlags> stages(pi->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = pi->waitSemaphoreCount;
    si.pWaitSemaphores = pi->pWaitSemaphores;
    si.pWaitDstStageMask = stages.data();
    bool okImg = i < s->v.size();
    if (okImg) {
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &s->v[i].copyDone;
    }
    VkResult r = d->vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    std::lock_guard<std::mutex> lk(s->m);
    if (okImg) { s->v[i].copyPending = r == VK_SUCCESS; s->v[i].state = 0; }
    s->cv.notify_all();
}

// native path: copy into the layer's NVIDIA swapchain and present it, on X-Plane's thread and queue
static VkResult nativePresent(VSwap* s, VkQueue queue, uint32_t i, const VkPresentInfoKHR* pi, double* tDriver) {
    DevData* d = s->d;
    *tDriver = 0;
    uint32_t fam = familyOf(d, queue);
    if (fam == UINT32_MAX) fam = d->copyFamily;
    if (!nativeForFamily(s, fam)) { dropFrame(s, queue, i, pi); return VK_ERROR_SURFACE_LOST_KHR; }
    s->nQueue = queue;
    if (g_nvHung) return VK_ERROR_DEVICE_LOST;
    if (s->nRecreate && s->nswap) {
        s->nRecreate = false;
        if (!createNativeSwapchain(s)) {
            dropFrame(s, queue, i, pi);
            return s->nativeWhyNot.empty() ? VK_SUCCESS : VK_ERROR_SURFACE_LOST_KHR;
        }
    }
    if (!s->nswap && !createNativeSwapchain(s)) {
        dropFrame(s, queue, i, pi);
        return s->nativeWhyNot.empty() ? VK_SUCCESS : VK_ERROR_SURFACE_LOST_KHR;  // minimized: skip
    }
    uint32_t k = s->nRing++ % VSwap::R;
    if (s->nFenceUsed[k]) {
        g_xStage = XS_N_FENCE;
        if (!nvFenceWait(d, s->nFence[k], "native copy")) return VK_ERROR_DEVICE_LOST;
        d->vkResetFences(d->device, 1, &s->nFence[k]);
        s->nFenceUsed[k] = false;
    }
    if (s->nTsPending[k]) {
        uint64_t t[2] = {0, 0};
        if (d->vkGetQueryPoolResults(d->device, s->nQ, 2 * k, 2, sizeof(t), t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) ==
            VK_SUCCESS) {
            std::lock_guard<std::mutex> lk(s->perf.m);
            s->perf.nGpu.add(tsMs(t[0], t[1], s->nMask, d->props.limits.timestampPeriod));
        }
        s->nTsPending[k] = false;
    }
    uint32_t ri = 0;
    g_xStage = XS_N_ACQUIRE;
    double a = nowMs();
    VkResult ar = nvAcquire(d, s->nswap, s->nAcq[k], &ri);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) {
        if (!createNativeSwapchain(s)) {
            dropFrame(s, queue, i, pi);
            return s->nativeWhyNot.empty() ? VK_SUCCESS : VK_ERROR_SURFACE_LOST_KHR;
        }
        ar = nvAcquire(d, s->nswap, s->nAcq[k], &ri);
    }
    double tAcq = nowMs() - a;
    if (ar == VK_TIMEOUT) {  // no image in 2 s: skip this frame instead of blocking X-Plane
        dropFrame(s, queue, i, pi);
        if (++s->nAcqTimeouts >= 3) {
            logf("[hang] NVIDIA (native) acquire: no image in 2 s, 3 times in a row");
            s->nativeWhyNot = "the NVIDIA swapchain gives no images";
            return VK_ERROR_SURFACE_LOST_KHR;  // caller: X-Plane gets a real NVIDIA swapchain
        }
        logf("[hang] NVIDIA (native) acquire: no image in 2 s -> frame skipped, swapchain recreated");
        s->nRecreate = true;
        return VK_SUCCESS;
    }
    s->nAcqTimeouts = 0;
    if (ar < 0 || ri >= s->nImgs.size()) {
        dropFrame(s, queue, i, pi);
        return ar == VK_ERROR_DEVICE_LOST ? VK_ERROR_DEVICE_LOST : VK_ERROR_SURFACE_LOST_KHR;
    }
    bool recreate = ar == VK_SUBOPTIMAL_KHR;
    recordNative(d, s, k, s->v[i].img, s->nImgs[ri]);
    std::vector<VkSemaphore> w(pi->pWaitSemaphores, pi->pWaitSemaphores + pi->waitSemaphoreCount);
    w.push_back(s->nAcq[k]);
    std::vector<VkPipelineStageFlags> stages(w.size(), VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkSemaphore sigs[2] = {s->nReady[ri], s->v[i].copyDone};
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = (uint32_t)w.size();
    si.pWaitSemaphores = w.data();
    si.pWaitDstStageMask = stages.data();
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s->nCb[k];
    si.signalSemaphoreCount = 2;
    si.pSignalSemaphores = sigs;
    VkResult sr = d->vkQueueSubmit(queue, 1, &si, s->nFence[k]);
    if (sr != VK_SUCCESS) return sr;
    s->nFenceUsed[k] = true;
    s->nTsPending[k] = s->nQ != VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> lk(s->m);
        s->v[i].copyPending = true;
        s->v[i].state = 0;
    }
    s->cv.notify_all();
    VkPresentInfoKHR p{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    p.waitSemaphoreCount = 1;
    p.pWaitSemaphores = &s->nReady[ri];
    p.swapchainCount = 1;
    p.pSwapchains = &s->nswap;
    p.pImageIndices = &ri;
    g_xStage = XS_N_PRESENT;
    a = nowMs();
    VkResult pr = d->vkQueuePresentKHR(queue, &p);
    double tPres = nowMs() - a;
    g_xStage = XS_PRESENT;
    if (pr >= 0) g_nativeShown = true;
    *tDriver = tAcq + tPres;
    {
        std::lock_guard<std::mutex> lk(s->perf.m);
        s->perf.nAcq.add(tAcq);
        s->perf.nPres.add(tPres);
    }
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR || recreate) {
        if (!createNativeSwapchain(s) && !s->nativeWhyNot.empty()) return VK_ERROR_SURFACE_LOST_KHR;
        return VK_SUCCESS;
    }
    if (pr == VK_ERROR_DEVICE_LOST) return pr;
    return pr < 0 ? VK_ERROR_SURFACE_LOST_KHR : VK_SUCCESS;
}

// Switch the output inside the layer. X-Plane's thread, between frames.
static bool switchPath(VSwap* s, int to, const char* why) {
    DevData* d = s->d;
    if (!d->worker->flush()) return false;
    if (to == PATH_BYPASS) {
        if (!s->bypassTried && !buildBypass(s)) {
            logf("bypass unavailable: %s", s->bypassWhyNot.c_str());
            g_sess.want = PATH_NATIVE;
            return false;
        }
        if (!s->bypassOk) { g_sess.want = PATH_NATIVE; return false; }
        if (g_nativeShown && !s->useDx && !g_cfg.bypassAfterNative) {  // see g_nativeShown
            g_sess.want = PATH_NATIVE;
            return false;
        }
        destroyNativeSwapchain(s);  // one swapchain per window at a time
        if (g_nativeShown && g_cfg.bypassAfterNative == 2 && s->ic && !g_intelHung) {
            IntelCtx* c = s->ic;
            dropIntelSwapchain(s);
            c->vkDestroySurfaceKHR(c->inst, c->surface, nullptr);
            c->surface = VK_NULL_HANDLE;
            VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
            sci.hinstance = GetModuleHandleW(nullptr);
            sci.hwnd = s->hwnd;
            VkResult r = c->vkCreateWin32SurfaceKHR(c->inst, &sci, nullptr, &c->surface);
            logf("[diag] test_bypass_after_native=2: fresh Intel surface for the return to the bypass (%d)", (int)r);
            if (r != VK_SUCCESS) { c->surface = VK_NULL_HANDLE; g_sess.want = PATH_NATIVE; return false; }
        }
        s->iRecreate = true;
        if (!ensureIntelSwapchain(s)) {
            static int n = 0;
            if (++n <= 5) logf("bypass: Intel swapchain could not be created now -> staying native");
            g_sess.want = PATH_NATIVE;
            if (!createNativeSwapchain(s) && !s->nativeWhyNot.empty()) return false;
            return false;
        }
    } else {
        dropIntelSwapchain(s);
        if (!createNativeSwapchain(s) && !s->nativeWhyNot.empty()) {
            logf("native path unavailable: %s", s->nativeWhyNot.c_str());
            return false;
        }
    }
    int from = s->path;
    s->path = to;
    s->skip = 30;
    g_sess.lastSwitch = nowMs();
    {
        std::lock_guard<std::mutex> lk(s->perf.m);
        s->perf.clearAll();
    }
    if (from != to || why) logf("path -> %s (%s)", pathName(to), why ? why : "requested");
    if (to == PATH_BYPASS)
        logf("ACTIVE: X-Plane %ux%u -> GeForce -> host memory -> Intel -> screen", s->ext.width, s->ext.height);
    return true;
}

static std::string cacheKeyFor(VSwap* s) {
    DevData* d = s->d;
    char b[512], dv[32], iv[32] = "-";
    fmtDriver(d->props.vendorID, d->props.driverVersion, dv, sizeof(dv));
    std::string iname = "-";
    if (s->ic) {
        VkPhysicalDeviceProperties ip;
        s->ic->vkGetPhysicalDeviceProperties(s->ic->pd, &ip);
        fmtDriver(ip.vendorID, ip.driverVersion, iv, sizeof(iv));
        iname = ip.deviceName;
    }
    HMONITOR m = MonitorFromWindow(s->hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(m, &mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm);
    snprintf(b, sizeof(b), "%s %s|%s %s|%lux%lu@%lu|%ux%u|%s", d->props.deviceName, dv, iname.c_str(), iv,
             dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency, s->ext.width, s->ext.height,
             g_cfg.forceCpuCopy ? "cpu" : "auto");
    return b;
}

// Every ~30 frames: which screen is the window on? Decide the path; run the A/B probe. X-Plane's thread.
static void decide(VSwap* s, double now) {
    if (now - g_sess.lastCheck < 1000.0) return;
    g_sess.lastCheck = now;
    if (g_cfg.mode == MODE_ON || g_cfg.path == PREF_BYPASS || g_cfg.path == PREF_CPU_COPY) {
        if (!g_intelBroken) g_sess.want = PATH_BYPASS;
        return;
    }
    if (g_cfg.path == PREF_NATIVE) { g_sess.want = PATH_NATIVE; return; }
    if (s->bypassTried && !s->bypassOk) { g_sess.want = PATH_NATIVE; g_sess.probe = PROBE_DONE; }
    HMONITOR m = MonitorFromWindow(s->hwnd, MONITOR_DEFAULTTONEAREST);
    bool onIntel = g_sess.onIntel;
    if (m != g_sess.lastMon || now - g_sess.lastDxgi > 5000.0) {
        onIntel = windowOnIntel(s->hwnd);
        g_sess.lastDxgi = now;
        g_sess.lastMon = m;
    }
    if (onIntel != g_sess.onIntel) {  // must be seen twice, and not within 2 s of the last switch
        if (g_sess.pendingOnIntel != (int)onIntel) { g_sess.pendingOnIntel = (int)onIntel; return; }
        if (now - g_sess.lastSwitch < 2000.0) return;
        g_sess.onIntel = onIntel;
        g_sess.pendingOnIntel = -1;
        logf("X-Plane's window is now on a screen driven by %s", onIntel ? "the Intel GPU" : "another GPU");
        if (g_sess.probe == PROBE_RUN) g_sess.probe = PROBE_OFF;
        if (!onIntel) { g_sess.want = PATH_NATIVE; return; }
        g_sess.probe = PROBE_OFF;  // re-decide below
    }
    g_sess.pendingOnIntel = -1;
    if (!g_sess.onIntel || g_intelBroken || (s->bypassTried && !s->bypassOk)) { g_sess.want = PATH_NATIVE; return; }
    if (g_sess.probe == PROBE_OFF) {
        if (!g_cfg.probe) { g_sess.want = PATH_BYPASS; g_sess.probe = PROBE_DONE; return; }
        if (!g_cfg.bypassAfterNative && !(s->bypassOk && s->useDx)) {
            logf("probe=1 is ignored: the Intel output is not DXGI, so after measuring native the bypass could not "
                 "come back (set test_bypass_after_native=1 or 2 to test that)");
            g_sess.want = PATH_BYPASS;
            g_sess.probe = PROBE_DONE;
            return;
        }
        g_sess.cacheKey = cacheKeyFor(s);
        int cached = cacheLookup(g_sess.cacheKey);
        if (cached >= 0) {
            logf("path from earlier measurement on this setup: %s (delete xpob_path_cache.txt to measure again)",
                 pathName(cached));
            g_sess.want = cached;
            g_sess.probe = PROBE_DONE;
            g_sess.baseP95 = 0;
            return;
        }
        g_sess.want = PATH_BYPASS;  // start fast; measure once the scene is stable
        g_sess.probe = PROBE_WAIT_STABLE;
        g_sess.stableTries = 0;
        g_sess.prevP50 = 0;
        logf("probe: will compare native and bypass once the frame rate is stable");
    }
}

// Called for every 300-frame window (X-Plane's thread) with that window's frame-time percentiles.
static void probeWindow(VSwap* s, double p50, double p95, double maxv) {
    double now = nowMs();
    if ((s->bypassTried && !s->bypassOk) || g_intelBroken) {
        if (g_sess.probe == PROBE_RUN || g_sess.probe == PROBE_WAIT_STABLE) g_sess.probe = PROBE_DONE;
        return;
    }
    if (g_sess.probe == PROBE_WAIT_STABLE) {
        // Only in flight: after a loading screen (a > 2 s gap) plus 30 s, and not at menu frame rates (< 6 ms).
        // Without any loading gap (e.g. a flight resumed directly), wait 3 minutes.
        bool afterLoad = g_sess.lastLongGap > 0 ? now - g_sess.lastLongGap > 30000.0
                                                : g_sess.firstFrame > 0 && now - g_sess.firstFrame > 180000.0;
        if (!afterLoad || p50 < 6.0) { g_sess.prevP50 = 0; return; }
        bool stable = g_sess.prevP50 > 0 && std::fabs(p50 - g_sess.prevP50) < 0.10 * g_sess.prevP50 && maxv < 200.0;
        g_sess.prevP50 = p50;
        if (!stable) {
            if (++g_sess.stableTries > 20) {
                logf("probe: frame rate never became stable -> keeping bypass");
                g_sess.probe = PROBE_DONE;
                g_sess.baseP95 = p95;
            }
            return;
        }
        g_sess.probe = PROBE_RUN;
        g_sess.step = 0;
        g_sess.retries = 0;
        g_sess.probeStart = now;
        logf("probe: stable at %.1f ms per frame, measuring native / bypass / native / bypass (300 frames each)", p50);
        g_sess.want = kProbeOrder[0];
        return;
    }
    if (g_sess.probe == PROBE_RUN) {
        if (g_sess.lastLongGap > g_sess.probeStart) {  // the scene changed (loading screen): measure again later
            logf("probe: a loading pause happened during the measurement -> measuring again later");
            g_sess.probe = PROBE_WAIT_STABLE;
            g_sess.prevP50 = 0;
            g_sess.stableTries = 0;
            g_sess.want = PATH_BYPASS;
            return;
        }
        if (s->path != kProbeOrder[g_sess.step]) { g_sess.want = kProbeOrder[g_sess.step]; return; }
        if (maxv > 200.0 && g_sess.retries < 3) { ++g_sess.retries; return; }  // loading hitch: measure again
        g_sess.p50[g_sess.step] = p50;
        g_sess.p95[g_sess.step] = p95;
        logf("probe %d/4: %s p50 %.2f ms p95 %.2f ms", g_sess.step + 1, pathName(s->path), p50, p95);
        if (++g_sess.step < 4) { g_sess.want = kProbeOrder[g_sess.step]; return; }
        auto differ = [](double a, double b) { return std::fabs(a - b) > 0.30 * std::min(a, b); };
        if (differ(g_sess.p50[0], g_sess.p50[2]) || differ(g_sess.p50[1], g_sess.p50[3])) {
            if (++g_sess.rounds <= 2) {
                logf("probe: the two runs of a path differ by more than 30%% (scene changing?) -> measuring again");
                g_sess.probe = PROBE_WAIT_STABLE;
                g_sess.prevP50 = 0;
                g_sess.stableTries = 0;
                g_sess.want = PATH_BYPASS;
                return;
            }
            logf("probe: results stayed inconsistent -> keeping the bypass, not saving a result");
            g_sess.want = PATH_BYPASS;
            g_sess.probe = PROBE_DONE;
            g_sess.lastProbe = now;
            g_sess.baseP95 = 0;
            return;
        }
        double n50 = (g_sess.p50[0] + g_sess.p50[2]) / 2, b50 = (g_sess.p50[1] + g_sess.p50[3]) / 2;
        double n95 = (g_sess.p95[0] + g_sess.p95[2]) / 2, b95 = (g_sess.p95[1] + g_sess.p95[3]) / 2;
        int pick = (b50 <= 0.95 * n50 && b95 <= 1.10 * n95) ? PATH_BYPASS : PATH_NATIVE;
        logf("selected %s: p50 native %.2f -> bypass %.2f ms, p95 native %.2f -> bypass %.2f ms "
             "(bypass needs >= 5%% lower p50 and <= 10%% higher p95)", pathName(pick), n50, b50, n95, b95);
        cacheStore(g_sess.cacheKey, pick);
        g_sess.want = pick;
        g_sess.probe = PROBE_DONE;
        g_sess.lastProbe = now;
        g_sess.baseP95 = pick == PATH_BYPASS ? b95 : n95;
        g_sess.bad = 0;
        return;
    }
    if (g_sess.probe == PROBE_DONE && g_cfg.probe && g_cfg.path == PREF_AUTO && g_cfg.mode == MODE_AUTO &&
        g_sess.onIntel && !g_intelBroken) {
        if (g_sess.baseP95 <= 0) { g_sess.baseP95 = p95; return; }
        g_sess.bad = p95 > 1.3 * g_sess.baseP95 ? g_sess.bad + 1 : 0;
        if (g_sess.bad >= 3 && now - g_sess.lastProbe > 600000.0) {
            logf("probe: p95 %.2f ms is >30%% worse than when the path was chosen (%.2f ms) -> measuring again",
                 p95, g_sess.baseP95);
            g_sess.probe = PROBE_WAIT_STABLE;
            g_sess.stableTries = 0;
            g_sess.prevP50 = 0;
            g_sess.lastProbe = now;
        }
    }
}

static void reportWindow(VSwap* s) {
    Perf& p = s->perf;
    std::string line;
    double p50, p95, mx;
    {
        std::lock_guard<std::mutex> lk(p.m);
        double mean = p.frame.mean();
        p50 = p.frame.pct(0.5);
        p95 = p.frame.pct(0.95);
        mx = p.frame.maxv();
        char b[512];
        const char* mem = s->path == PATH_BYPASS && s->bypassOk ? (s->mem == MEM_SHARED ? "shared" : s->mem == MEM_CPU ? "cpu-copy" : "half-import") : "-";
        snprintf(b, sizeof(b), "FPS %.1f | path=%s mem=%s conv=%s | frame ms p50 %.2f p95 %.2f p99 %.2f max %.2f over50 %d",
                 mean > 0 ? 1000.0 / mean : 0.0, pathName(s->path), mem,
                 s->path == PATH_BYPASS && s->conv ? "blit" : "none", p50, p95, p.frame.pct(0.99), mx, p.over50);
        line = b;
        addSeries(line, "xp_wait", p.xpWait);
        addSeries(line, "layer_main", p.layer);
        if (s->path == PATH_BYPASS) {
            addSeries(line, "gf_gpu", p.gfGpu);
            addSeries(line, "gf_fence", p.gfFence);
            addSeries(line, "cpu_copy", p.cpuCopy);
            addSeries(line, "i_gpu", p.iGpu);
            addSeries(line, "i_acq", p.iAcq);
            addSeries(line, "i_pres", p.iPres);
        } else {
            addSeries(line, "n_gpu", p.nGpu);
            addSeries(line, "n_acq", p.nAcq);
            addSeries(line, "n_pres", p.nPres);
        }
        p.clearAll();
    }
    if (g_cfg.stats) logf("%s", line.c_str());
    probeWindow(s, p50, p95, mx);
}

// Once per presented frame on X-Plane's thread: move this frame's driver times into the 300-frame window.
static void mFrame() {
    if (!g_cfg.measure) return;
    if (!g_mDev) {  // X-Plane took no device function through vkGetDeviceProcAddr: nothing is timed
        static int n = 0;
        if (++n == 300)
            logf("[diag] measure_driver=1, but no X-Plane Vulkan call is timed (X-Plane did not get its device "
                 "functions through vkGetDeviceProcAddr)");
        return;
    }
    static uint64_t tsc0 = 0;
    static double ms0 = 0, otherMs = 0;
    static uint64_t otherCalls = 0;
    static std::vector<float> tot, calls, draws, cat[M_N], spin, polls, pollT, frameMs;
    static double lastMs = 0;
    static int frames = 0;
    uint64_t tsc = __rdtsc();
    double ms = nowMs();
    if (!tsc0) { tsc0 = tsc; ms0 = ms; }
    double tpm = ms - ms0 > 200.0 ? double(tsc - tsc0) / (ms - ms0) : 0.0;  // time stamp ticks per ms
    MAcc a = g_mMain;
    g_mMain = MAcc{};
    if (g_mSpinStart) { g_mSpin += tsc - g_mSpinStart; g_mSpinStart = tsc; }  // a wait still running: split it here
    uint64_t sp = g_mSpin, pt = g_mPollT;
    uint32_t np = g_mPolls;
    g_mSpin = g_mPollT = 0;
    g_mPolls = 0;
    uint64_t ot = g_mOtherT.exchange(0);
    uint32_t on = g_mOtherN.exchange(0);
    if (tpm <= 0) return;
    double sum = 0;
    uint32_t n = 0;
    for (int c = 0; c < M_N; ++c) {
        double v = a.t[c] / tpm;
        cat[c].push_back((float)v);
        sum += v;
        n += a.n[c];
    }
    tot.push_back((float)sum);
    spin.push_back((float)(sp / tpm));
    polls.push_back((float)np);
    pollT.push_back((float)(pt / tpm));
    if (lastMs > 0) frameMs.push_back((float)(ms - lastMs));
    lastMs = ms;
    calls.push_back((float)n);
    draws.push_back((float)a.n[M_DRAW]);
    otherMs += ot / tpm;
    otherCalls += on;
    if (++frames < 300) return;
    auto pct = [](std::vector<float> v, double p) {
        if (v.empty()) return 0.0;
        std::sort(v.begin(), v.end());
        return (double)v[std::min(v.size() - 1, (size_t)(p * (v.size() - 1) + 0.5))];
    };
    auto mean = [](const std::vector<float>& v) {
        double t = 0;
        for (float x : v) t += x;
        return v.empty() ? 0.0 : t / v.size();
    };
    std::string line;
    char b[256];
    snprintf(b, sizeof(b), "[drv] X-Plane's thread in the driver per frame: p50 %.2f p95 %.2f ms | calls p50 %.0f "
             "(draws %.0f)", pct(tot, 0.5), pct(tot, 0.95), pct(calls, 0.5), pct(draws, 0.5));
    line = b;
    for (int c = 0; c < M_N; ++c) {
        double m = mean(cat[c]);
        if (m < 0.005 && c != M_DRAW && c != M_SUBMIT) continue;
        snprintf(b, sizeof(b), " | %s %.2f", kMName[c], m);
        line += b;
        if (c == M_PIPE) {
            snprintf(b, sizeof(b), " (max %.1f)", pct(cat[c], 1.0));
            line += b;
        }
    }
    snprintf(b, sizeof(b), " | other threads %.2f ms, %.0f calls per frame", otherMs / frames,
             (double)otherCalls / frames);
    line += b;
    logf("%s", line.c_str());
    // frame = X-Plane's own CPU work + time in the driver + time polling for the GPU
    // (the polls' own time is inside both the polling span and the "wait" category: counted once, in the span)
    double fm = mean(frameMs), sm = mean(spin), wm = mean(cat[M_WAIT]) - mean(pollT), dm = mean(tot) - mean(pollT);
    logf("[drv] frame %.2f ms = waiting for the GPU %.2f ms (polling %.2f, %.0f polls; other waits %.2f) + other "
         "driver calls %.2f ms + X-Plane CPU %.2f ms | polling p50 %.2f p95 %.2f ms", fm, sm + wm, sm, mean(polls), wm,
         dm - wm, std::max(0.0, fm - sm - dm), pct(spin, 0.5), pct(spin, 0.95));
    tot.clear(); calls.clear(); draws.clear(); spin.clear(); polls.clear(); pollT.clear(); frameMs.clear();
    for (auto& v : cat) v.clear();
    frames = 0;
    otherMs = 0;
    otherCalls = 0;
}

// Logs where X-Plane's thread and the worker are when no frame has been presented for 3 s (and again at 15 s),
// so a freeze leaves a trace in the log. Runs once per process; never touches Vulkan.
static void startWatchdog() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread([] {
            int logged = 0;
            long long stallFrom = 0;
            for (;;) {
                Sleep(1000);
                long long last = g_lastFrameMs.load();
                if (last == 0) continue;
                double now = nowMs(), gap = now - (double)last;
                if (gap < 3000.0) {
                    if (logged && last > stallFrom)
                        logf("[hang] frames again after a %.1f s stall", (double)(last - stallFrom) / 1000.0);
                    logged = 0;
                    continue;
                }
                if (logged == 0) stallFrom = last;
                int xs = g_xStage.load(), ws = g_wStage.load(), cp = g_curPath.load();
                bool xpBusy = xs == XS_OUTSIDE && ws == WS_IDLE && !g_intelHung;  // X-Plane itself (e.g. loading)
                if (logged == 0 && xpBusy && gap < 15000.0) continue;
                if (logged == 0 && xpBusy) logged = 1;
                if ((logged == 0 && gap >= 3000.0) || (logged == 1 && gap >= 15000.0)) {
                    logf("[hang] no frame for %.1f s | X-Plane's thread: %s | worker: %s for %.1f s | path %s%s",
                         gap / 1000.0, kXStage[xs < 0 || xs > XS_N_FENCE ? 0 : xs],
                         kWStage[ws < 0 || ws > WS_I_PRESENT ? 0 : ws], (now - (double)g_wStageMs.load()) / 1000.0,
                         cp == PATH_BYPASS ? "bypass" : cp == PATH_NATIVE ? "native" : "-",
                         g_intelHung ? " | Intel device not answering"
                         : xs == XS_OUTSIDE && ws == WS_IDLE ? " (X-Plane busy, e.g. loading)" : "");
                    ++logged;
                }
            }
        }).detach();
    });
}

static VkResult vPresent(VSwap* s, VkQueue queue, const VkPresentInfoKHR* pi) {
    static bool toldPres = false;
    if (!toldPres) {
        toldPres = true;
        logf("[diag] X-Plane present: %u wait semaphore(s)", pi->waitSemaphoreCount);
    }
    DevData* d = s->d;
    if (g_nvHung || g_workerStuck) return VK_ERROR_DEVICE_LOST;  // nothing more is queued on a lost worker / GPU
    double t0 = nowMs();
    startWatchdog();
    g_xpThread = GetCurrentThreadId();
    g_xStage = XS_PRESENT;
    uint32_t i = pi->pImageIndices[0];
    // A gap of over 2 s between any two presents is a loading screen (measured even right after a path switch,
    // when frame intervals are not counted: X-Plane's start-up loading screen shows only a few frames).
    if (g_sess.lastPresentT > 0 && t0 - g_sess.lastPresentT > 2000.0) g_sess.lastLongGap = t0;
    g_sess.lastPresentT = t0;
    // frame interval (counted unless just after a switch)
    {
        std::lock_guard<std::mutex> lk(s->perf.m);
        if (s->skip > 0) { --s->skip; s->perf.lastT0 = 0; }
        else {
            if (s->perf.lastT0 > 0) {
                double iv = t0 - s->perf.lastT0;
                s->perf.frame.add(iv);
                if (iv > 50.0) s->perf.over50++;
            }
            if (g_sess.firstFrame == 0) g_sess.firstFrame = t0;
            s->perf.lastT0 = t0;
        }
    }
    decide(s, t0);
    double tDecide = nowMs() - t0;
    int want = g_sess.want.load();
    if (want == PATH_BYPASS && s->path != PATH_BYPASS && g_nativeShown && !g_cfg.bypassAfterNative && s->bypassTried &&
        !s->useDx) {
        static bool told = false;
        if (!told) {
            told = true;
            logf("bypass not resumed: native output was already shown on this window, and Windows would keep showing "
                 "the old picture with the Intel Vulkan output (flip mode) -> staying native; restart X-Plane to use "
                 "the bypass again");
        }
        g_sess.want = want = PATH_NATIVE;
        if (g_sess.probe == PROBE_RUN || g_sess.probe == PROBE_WAIT_STABLE) g_sess.probe = PROBE_DONE;
    }
    if (want != s->path && i < s->v.size()) {
        const char* why = s->path == PATH_BYPASS && g_intelBroken ? "Intel failure" :
                          g_sess.probe == PROBE_RUN ? "probe" : nullptr;
        g_xStage = XS_SWITCH;
        bool switched = switchPath(s, want, why);
        g_xStage = XS_PRESENT;
        if (!switched && want == PATH_NATIVE && s->path == PATH_BYPASS && g_intelBroken &&
            !g_virtualBroken) {
            logf("cannot switch to the native path either -> asking X-Plane to recreate its swapchain");
            g_virtualBroken = true;  // vAcquire now reports OUT_OF_DATE; X-Plane gets a real NVIDIA swapchain
        }
    }
    VkResult res = VK_SUCCESS;
    double driver = 0;
    if (s->path == PATH_BYPASS) {
        auto waits = std::make_shared<std::vector<VkSemaphore>>(pi->pWaitSemaphores, pi->pWaitSemaphores + pi->waitSemaphoreCount);
        {
            std::lock_guard<std::mutex> lk(d->semLock);
            for (VkSemaphore sem : *waits) d->pendingWaits.insert(sem);
        }
        {
            std::lock_guard<std::mutex> lk(s->m);
            if (i < s->v.size()) s->v[i].state = 2;
        }
        d->worker->push([s, i, waits] { frameJob(s, i, waits); });
        int e = s->err.exchange(0);
        if (e < 0) res = (VkResult)e;
        else if (g_virtualBroken) res = VK_ERROR_OUT_OF_DATE_KHR;
    } else {
        res = (i < s->v.size()) ? nativePresent(s, queue, i, pi, &driver) : VK_ERROR_OUT_OF_DATE_KHR;
        if (res == VK_ERROR_SURFACE_LOST_KHR) {
            logf("native path failed (%s) -> asking X-Plane to recreate its swapchain with a real NVIDIA one",
                 s->nativeWhyNot.empty() ? "present error" : s->nativeWhyNot.c_str());
            g_virtualBroken = true;
            res = VK_ERROR_OUT_OF_DATE_KHR;
        }
    }
    if (res == VK_SUCCESS && s->suggestResize.exchange(false)) res = VK_SUBOPTIMAL_KHR;  // let X-Plane resize
    if (pi->pResults) pi->pResults[0] = res;
    bool report = false;
    {
        std::lock_guard<std::mutex> lk(s->perf.m);
        s->perf.layer.add(nowMs() - t0 - driver);
        report = s->perf.frame.v.size() >= 300;
    }
    double tRep = nowMs();
    if (report) reportWindow(s);
    tRep = nowMs() - tRep;
    mFrame();
    double tAll = nowMs() - t0;
    if (tAll > 300.0) {  // a slow present on X-Plane's thread: say where the time went
        static int n = 0;
        if (++n <= 20)
            logf("[slow] layer present took %.0f ms (screen check / path decision %.0f, driver %.0f, log %.0f, path %s)",
                 tAll, tDecide, driver, tRep, pathName(s->path));
    }
    g_curPath = s->path;
    g_lastFrameMs = (long long)nowMs();
    g_xStage = XS_OUTSIDE;
    return res;
}

// ================================================================ instance hooks

static void logVkDevices(VkInstance inst, PFN_vkGetInstanceProcAddr next, bool props2) {
    auto enumPd = (PFN_vkEnumeratePhysicalDevices)next(inst, "vkEnumeratePhysicalDevices");
    auto getProps = (PFN_vkGetPhysicalDeviceProperties)next(inst, "vkGetPhysicalDeviceProperties");
    auto getProps2 = props2 ? (PFN_vkGetPhysicalDeviceProperties2)next(inst, "vkGetPhysicalDeviceProperties2") : nullptr;
    if (props2 && !getProps2) getProps2 = (PFN_vkGetPhysicalDeviceProperties2)next(inst, "vkGetPhysicalDeviceProperties2KHR");
    auto enumExt = (PFN_vkEnumerateDeviceExtensionProperties)next(inst, "vkEnumerateDeviceExtensionProperties");
    if (!enumPd || !getProps) return;
    uint32_t n = 0;
    enumPd(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    enumPd(inst, &n, pds.data());
    for (auto p : pds) {
        VkPhysicalDeviceIDProperties idp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &idp;
        if (getProps2) getProps2(p, &p2);
        else getProps(p, &p2.properties);
        bool host = false;
        if (enumExt) {
            uint32_t ne = 0;
            enumExt(p, nullptr, &ne, nullptr);
            std::vector<VkExtensionProperties> e(ne);
            enumExt(p, nullptr, &ne, e.data());
            for (auto& x : e)
                if (!strcmp(x.extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) host = true;
        }
        if (p2.properties.vendorID == 0x8086) g_haveIntelVk = true;
        char dv[32], l[32] = "n/a";
        fmtDriver(p2.properties.vendorID, p2.properties.driverVersion, dv, sizeof(dv));
        if (getProps2 && idp.deviceLUIDValid) fmtLuid(idp.deviceLUID, l, sizeof(l));
        logf("[diag] Vulkan GPU: %s | vendor 0x%04x | driver %s | Vulkan %u.%u | LUID %s | external_memory_host %s",
             p2.properties.deviceName, p2.properties.vendorID, dv, VK_API_VERSION_MAJOR(p2.properties.apiVersion),
             VK_API_VERSION_MINOR(p2.properties.apiVersion), l, host ? "yes" : "no");
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL XP_CreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                                         const VkAllocationCallbacks* pAllocator, VkInstance* pInstance) {
    std::call_once(g_initOnce, initOnce);
    auto* ci = (VkLayerInstanceCreateInfo*)pCreateInfo->pNext;
    while (ci && !(ci->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && ci->function == VK_LAYER_LINK_INFO))
        ci = (VkLayerInstanceCreateInfo*)ci->pNext;
    if (!ci) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr next = ci->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    ci->u.pLayerInfo = ci->u.pLayerInfo->pNext;
    auto create = (PFN_vkCreateInstance)next(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = create(pCreateInfo, pAllocator, pInstance);
    if (r == VK_SUCCESS) {
        uint32_t api = pCreateInfo->pApplicationInfo && pCreateInfo->pApplicationInfo->apiVersion
                           ? pCreateInfo->pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
        bool props2 = api >= VK_API_VERSION_1_1;
        for (uint32_t e = 0; e < pCreateInfo->enabledExtensionCount; ++e)
            if (!strcmp(pCreateInfo->ppEnabledExtensionNames[e], VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
                props2 = true;
        InstData d{*pInstance, next, (PFN_vkDestroyInstance)next(*pInstance, "vkDestroyInstance"), api, props2};
        {
            std::lock_guard<std::mutex> lk(g_lock);
            g_inst[key(*pInstance)] = d;
        }
        if (g_isXPlane && !g_internalCreate) {
            logf("[diag] X-Plane Vulkan instance: API %u.%u", VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api));
            logVkDevices(*pInstance, next, props2);
        }
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL XP_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator) {
    PFN_vkDestroyInstance fn = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        auto it = g_inst.find(key(instance));
        if (it != g_inst.end()) { fn = it->second.destroy; g_inst.erase(it); }
    }
    if (fn) fn(instance, pAllocator);
}

static bool instOf(void* k, InstData* out) {
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_inst.find(k);
    if (it == g_inst.end()) return false;
    *out = it->second;
    return true;
}

static VKAPI_ATTR VkResult VKAPI_CALL XP_CreateWin32SurfaceKHR(VkInstance instance, const VkWin32SurfaceCreateInfoKHR* ci,
                                                                const VkAllocationCallbacks* a, VkSurfaceKHR* pSurface) {
    InstData id;
    if (!instOf(key(instance), &id)) return VK_ERROR_INITIALIZATION_FAILED;
    auto next = (PFN_vkCreateWin32SurfaceKHR)id.gipa(instance, "vkCreateWin32SurfaceKHR");
    if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = next(instance, ci, a, pSurface);
    if (r == VK_SUCCESS && g_isXPlane && !g_internalCreate) {
        std::lock_guard<std::mutex> lk(g_surfLock);
        g_surfHwnd[*pSurface] = ci->hwnd;
    }
    return r;
}

// While the bypass may be used, offer X-Plane only SDR formats it can convert, so it never picks an HDR format
// the Intel side cannot show. Falls back to the full list if none would remain.
static bool keepFormat(const VkSurfaceFormatKHR& f) {
    return f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR && bypassFormat(f.format);
}
static bool filterSurface(VkSurfaceKHR surface) {
    if (!g_isXPlane || g_internalCreate || g_cfg.mode == MODE_OFF || !g_haveIntelVk || g_virtualBroken) return false;
    std::lock_guard<std::mutex> lk(g_surfLock);
    return g_surfHwnd.count(surface) != 0;
}

static VKAPI_ATTR VkResult VKAPI_CALL XP_GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice pd, VkSurfaceKHR surface,
                                                                             uint32_t* pCount, VkSurfaceFormatKHR* pFormats) {
    InstData id;
    if (!instOf(key(pd), &id)) return VK_ERROR_INITIALIZATION_FAILED;
    auto next = (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)id.gipa(id.instance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    if (!filterSurface(surface)) return next(pd, surface, pCount, pFormats);
    uint32_t n = 0;
    VkResult r = next(pd, surface, &n, nullptr);
    if (r != VK_SUCCESS) return r;
    std::vector<VkSurfaceFormatKHR> all(n);
    r = next(pd, surface, &n, all.data());
    if (r < 0) return r;
    all.resize(n);
    std::vector<VkSurfaceFormatKHR> kept;
    for (auto& f : all) if (keepFormat(f)) kept.push_back(f);
    static bool logged = false;
    if (!logged) {
        logged = true;
        std::string a, b;
        for (auto& f : all) a += std::to_string((int)f.format) + "/" + std::to_string((int)f.colorSpace) + " ";
        for (auto& f : kept) b += std::to_string((int)f.format) + "/" + std::to_string((int)f.colorSpace) + " ";
        logf("[diag] NVIDIA surface formats: %s| offered to X-Plane: %s", a.c_str(), kept.empty() ? "all (none convertible)" : b.c_str());
    }
    if (kept.empty()) kept = all;
    if (!pFormats) { *pCount = (uint32_t)kept.size(); return VK_SUCCESS; }
    uint32_t c = std::min(*pCount, (uint32_t)kept.size());
    for (uint32_t i = 0; i < c; ++i) pFormats[i] = kept[i];
    *pCount = c;
    return c < kept.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL XP_GetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice pd,
                                                                              const VkPhysicalDeviceSurfaceInfo2KHR* info,
                                                                              uint32_t* pCount, VkSurfaceFormat2KHR* pFormats) {
    InstData id;
    if (!instOf(key(pd), &id)) return VK_ERROR_INITIALIZATION_FAILED;
    auto next = (PFN_vkGetPhysicalDeviceSurfaceFormats2KHR)id.gipa(id.instance, "vkGetPhysicalDeviceSurfaceFormats2KHR");
    bool chained = false;
    if (pFormats)
        for (uint32_t i = 0; i < *pCount; ++i) chained |= pFormats[i].pNext != nullptr;
    if (!info || chained || !filterSurface(info->surface)) return next(pd, info, pCount, pFormats);
    uint32_t n = 0;
    VkResult r = next(pd, info, &n, nullptr);
    if (r != VK_SUCCESS) return r;
    std::vector<VkSurfaceFormat2KHR> all(n, VkSurfaceFormat2KHR{VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR});
    r = next(pd, info, &n, all.data());
    if (r < 0) return r;
    all.resize(n);
    std::vector<VkSurfaceFormat2KHR> kept;
    for (auto& f : all) if (keepFormat(f.surfaceFormat)) kept.push_back(f);
    if (kept.empty()) kept = all;
    if (!pFormats) { *pCount = (uint32_t)kept.size(); return VK_SUCCESS; }
    uint32_t c = std::min(*pCount, (uint32_t)kept.size());
    for (uint32_t i = 0; i < c; ++i) pFormats[i].surfaceFormat = kept[i].surfaceFormat;
    *pCount = c;
    return c < kept.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

// ================================================================ device hooks

static VKAPI_ATTR VkResult VKAPI_CALL XP_CreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo,
                                                       const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {
    auto* ci = (VkLayerDeviceCreateInfo*)pCreateInfo->pNext;
    while (ci && !(ci->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && ci->function == VK_LAYER_LINK_INFO))
        ci = (VkLayerDeviceCreateInfo*)ci->pNext;
    if (!ci) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = ci->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = ci->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    ci->u.pLayerInfo = ci->u.pLayerInfo->pNext;
    PFN_vkSetDeviceLoaderData setLoaderData = nullptr;
    for (auto* p = (VkLayerDeviceCreateInfo*)pCreateInfo->pNext; p; p = (VkLayerDeviceCreateInfo*)p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && p->function == VK_LOADER_DATA_CALLBACK)
            setLoaderData = p->u.pfnSetDeviceLoaderData;

    InstData id{};
    bool haveInst = instOf(key(physicalDevice), &id);
    VkInstance inst = haveInst ? id.instance : VK_NULL_HANDLE;
    auto create = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;

    bool mine = g_isXPlane && !g_internalCreate;
    auto getProps = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    VkPhysicalDeviceProperties pr{};
    if (getProps) getProps(physicalDevice, &pr);
    auto getQF = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    std::vector<VkQueueFamilyProperties> fams;
    if (getQF) {
        uint32_t n = 0;
        getQF(physicalDevice, &n, nullptr);
        fams.resize(n);
        getQF(physicalDevice, &n, fams.data());
    }

    // Prepare virtual swapchains when X-Plane renders on NVIDIA and an Intel GPU exists (the window may move).
    bool want = false;
    VkDeviceCreateInfo dci = *pCreateInfo;
    std::vector<VkDeviceQueueCreateInfo> qcis(pCreateInfo->pQueueCreateInfos,
                                              pCreateInfo->pQueueCreateInfos + pCreateInfo->queueCreateInfoCount);
    std::vector<std::vector<float>> prios(qcis.size() + 1);
    std::vector<const char*> exts(pCreateInfo->ppEnabledExtensionNames,
                                  pCreateInfo->ppEnabledExtensionNames + pCreateInfo->enabledExtensionCount);
    uint32_t fam = UINT32_MAX, idx = 0;
    bool famGraphics = true, hostExt = false;
    VkDeviceSize hostAlign = 4096;
    if (mine) {
        std::string q;
        for (size_t f = 0; f < fams.size(); ++f) {
            uint32_t used = 0;
            for (auto& x : qcis) if (x.queueFamilyIndex == f) used += x.queueCount;
            char b[96];
            snprintf(b, sizeof(b), "%zu:%s%s%s x%u (X-Plane uses %u, ts %u) ", f,
                     fams[f].queueFlags & VK_QUEUE_GRAPHICS_BIT ? "G" : "", fams[f].queueFlags & VK_QUEUE_COMPUTE_BIT ? "C" : "",
                     fams[f].queueFlags & VK_QUEUE_TRANSFER_BIT ? "T" : "", fams[f].queueCount, used, fams[f].timestampValidBits);
            q += b;
        }
        logf("[diag] render GPU queue families: %s", q.c_str());
    }
    if (mine && g_cfg.mode == MODE_OFF) logf("mode=off: bypass disabled, only measuring FPS");
    if (mine && g_cfg.mode != MODE_OFF && setLoaderData) {
        bool nvidia = pr.vendorID == 0x10DE;
        bool intelDisplay = anyDisplayOnIntel();
        char dv[32];
        fmtDriver(pr.vendorID, pr.driverVersion, dv, sizeof(dv));
        logf("render GPU: %s, driver %s | a display is driven by an Intel GPU: %s | Intel Vulkan device: %s",
             pr.deviceName, dv, intelDisplay ? "yes" : "no", g_haveIntelVk ? "yes" : "no");
        want = nvidia && (g_haveIntelVk || g_cfg.mode == MODE_ON);
        if (!nvidia) logf("not an NVIDIA GPU -> bypass disabled");
        else if (!want) logf("no Intel GPU in this system -> bypass disabled (set mode=on to force)");
    }
    if (want) {
        auto enumExt = (PFN_vkEnumerateDeviceExtensionProperties)gipa(inst, "vkEnumerateDeviceExtensionProperties");
        if (enumExt) {
            uint32_t ne = 0;
            enumExt(physicalDevice, nullptr, &ne, nullptr);
            std::vector<VkExtensionProperties> e(ne);
            enumExt(physicalDevice, nullptr, &ne, e.data());
            for (auto& x : e) if (!strcmp(x.extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) hostExt = true;
        }
        if (hostExt && id.props2) {
            auto p2fn = (PFN_vkGetPhysicalDeviceProperties2)gipa(inst, "vkGetPhysicalDeviceProperties2");
            if (!p2fn) p2fn = (PFN_vkGetPhysicalDeviceProperties2)gipa(inst, "vkGetPhysicalDeviceProperties2KHR");
            if (p2fn) {
                VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
                VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
                p2.pNext = &hp;
                p2fn(physicalDevice, &p2);
                if (hp.minImportedHostPointerAlignment) hostAlign = hp.minImportedHostPointerAlignment;
            }
        }
        // 1) a spare queue in a graphics family X-Plane uses (proven path)
        if (!g_cfg.noExtraQueue)
            for (size_t i = 0; i < qcis.size(); ++i) {
                uint32_t f = qcis[i].queueFamilyIndex;
                if (f < fams.size() && (fams[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) && qcis[i].flags == 0 &&
                    qcis[i].queueCount < fams[f].queueCount) {
                    prios[i].assign(qcis[i].pQueuePriorities, qcis[i].pQueuePriorities + qcis[i].queueCount);
                    prios[i].push_back(1.0f);
                    fam = f;
                    idx = qcis[i].queueCount;
                    qcis[i].queueCount += 1;
                    qcis[i].pQueuePriorities = prios[i].data();
                    break;
                }
            }
        // 2) otherwise a queue in a transfer-only family
        if (fam == UINT32_MAX)
            for (uint32_t f = 0; f < fams.size() && fam == UINT32_MAX; ++f) {
                if (!(fams[f].queueFlags & VK_QUEUE_TRANSFER_BIT) || (fams[f].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
                size_t at = qcis.size();
                for (size_t i = 0; i < qcis.size(); ++i) if (qcis[i].queueFamilyIndex == f) at = i;
                if (at < qcis.size()) {
                    if (qcis[at].flags != 0 || qcis[at].queueCount >= fams[f].queueCount) continue;
                    prios[at].assign(qcis[at].pQueuePriorities, qcis[at].pQueuePriorities + qcis[at].queueCount);
                    prios[at].push_back(1.0f);
                    idx = qcis[at].queueCount;
                    qcis[at].queueCount += 1;
                    qcis[at].pQueuePriorities = prios[at].data();
                } else {
                    prios[qcis.size()].assign(1, 1.0f);
                    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
                    q.queueFamilyIndex = f;
                    q.queueCount = 1;
                    q.pQueuePriorities = prios[qcis.size()].data();
                    qcis.push_back(q);
                    idx = 0;
                }
                fam = f;
                famGraphics = false;
                logf("[diag] no spare graphics queue%s -> using transfer-only family %u (no conversion blits)",
                     g_cfg.noExtraQueue ? " (test_no_extra_queue)" : "", f);
            }
        if (fam == UINT32_MAX) { logf("no free queue for the layer -> bypass disabled"); want = false; }
    }
    if (want) {
        bool have = false;
        for (const char* e : exts)
            if (!strcmp(e, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) have = true;
        if (hostExt && !have) exts.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
        dci.queueCreateInfoCount = (uint32_t)qcis.size();
        dci.pQueueCreateInfos = qcis.data();
        dci.enabledExtensionCount = (uint32_t)exts.size();
        dci.ppEnabledExtensionNames = exts.data();
        logf("[diag] GeForce VK_EXT_external_memory_host: %s (alignment %llu)", hostExt ? "yes" : "no",
             (unsigned long long)hostAlign);
    }

    VkResult r = create(physicalDevice, want ? &dci : pCreateInfo, pAllocator, pDevice);
    if (r != VK_SUCCESS && want && hostExt) {
        logf("driver refused VK_EXT_external_memory_host (%d) -> trying without it (CPU copy)", (int)r);
        exts.pop_back();
        dci.enabledExtensionCount = (uint32_t)exts.size();
        hostExt = false;
        r = create(physicalDevice, &dci, pAllocator, pDevice);
    }
    if (r != VK_SUCCESS && want) {
        logf("driver refused the extra queue (%d) -> bypass disabled", (int)r);
        want = false;
        r = create(physicalDevice, pCreateInfo, pAllocator, pDevice);
    }
    if (r != VK_SUCCESS) return r;

    auto* d = new DevData();
    VkDevice dev = *pDevice;
    d->device = dev;
    d->pd = physicalDevice;
    d->instance = inst;
    d->gipa = gipa;
    d->gdpa = gdpa;
    d->setLoaderData = setLoaderData;
    d->internal = g_internalCreate;
    d->props = pr;
    d->families = fams;
    for (auto& q : (want ? qcis : std::vector<VkDeviceQueueCreateInfo>(pCreateInfo->pQueueCreateInfos,
                                        pCreateInfo->pQueueCreateInfos + pCreateInfo->queueCreateInfoCount)))
        if (std::find(d->xpFamilies.begin(), d->xpFamilies.end(), q.queueFamilyIndex) == d->xpFamilies.end())
            d->xpFamilies.push_back(q.queueFamilyIndex);
#define GD(n) d->n = (PFN_##n)gdpa(dev, #n)
    GD(vkDestroyDevice); GD(vkGetDeviceQueue); GD(vkGetDeviceQueue2); GD(vkQueueSubmit); GD(vkQueueSubmit2);
    GD(vkQueueSubmit2KHR); GD(vkQueueWaitIdle); GD(vkDeviceWaitIdle); GD(vkCreateSwapchainKHR);
    GD(vkDestroySwapchainKHR); GD(vkGetSwapchainImagesKHR); GD(vkAcquireNextImageKHR); GD(vkAcquireNextImage2KHR);
    GD(vkQueuePresentKHR); GD(vkCreateImage); GD(vkDestroyImage); GD(vkGetImageMemoryRequirements);
    GD(vkAllocateMemory); GD(vkFreeMemory); GD(vkBindImageMemory); GD(vkCreateSemaphore); GD(vkDestroySemaphore);
    GD(vkCreateFence); GD(vkDestroyFence); GD(vkWaitForFences); GD(vkResetFences); GD(vkCreateCommandPool);
    GD(vkDestroyCommandPool); GD(vkAllocateCommandBuffers); GD(vkBeginCommandBuffer); GD(vkEndCommandBuffer);
    GD(vkResetCommandBuffer); GD(vkCmdPipelineBarrier); GD(vkCmdCopyImageToBuffer); GD(vkCmdCopyImage);
    GD(vkCmdBlitImage); GD(vkCreateBuffer); GD(vkDestroyBuffer); GD(vkGetBufferMemoryRequirements);
    GD(vkBindBufferMemory); GD(vkMapMemory); GD(vkUnmapMemory); GD(vkInvalidateMappedMemoryRanges);
    GD(vkGetMemoryHostPointerPropertiesEXT); GD(vkCreateQueryPool); GD(vkDestroyQueryPool);
    GD(vkCmdResetQueryPool); GD(vkCmdWriteTimestamp); GD(vkGetQueryPoolResults);
    GD(vkSetDebugUtilsObjectNameEXT); GD(vkSetDebugUtilsObjectTagEXT); GD(vkGetSwapchainStatusKHR);
    GD(vkSetHdrMetadataEXT); GD(vkWaitForPresentKHR); GD(vkGetRefreshCycleDurationGOOGLE);
    GD(vkGetPastPresentationTimingGOOGLE); GD(vkAcquireFullScreenExclusiveModeEXT);
    GD(vkReleaseFullScreenExclusiveModeEXT); GD(vkReleaseSwapchainImagesEXT); GD(vkSetLatencySleepModeNV);
    GD(vkLatencySleepNV); GD(vkSetLatencyMarkerNV); GD(vkGetLatencyTimingsNV); GD(vkSignalSemaphore);
    GD(vkSignalSemaphoreKHR);
#undef GD
    if (!d->vkQueueSubmit2) d->vkQueueSubmit2 = d->vkQueueSubmit2KHR;
    if (!d->vkSignalSemaphore) d->vkSignalSemaphore = d->vkSignalSemaphoreKHR;
    d->surfCaps = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)gipa(inst, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    d->formatProps = (PFN_vkGetPhysicalDeviceFormatProperties)gipa(inst, "vkGetPhysicalDeviceFormatProperties");
    if (want) {
        auto gmp = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
        if (gmp) gmp(physicalDevice, &d->memProps);
        d->hostImport = hostExt && d->vkGetMemoryHostPointerPropertiesEXT;
        d->hostAlign = hostAlign;
        VkQueue q = VK_NULL_HANDLE;
        d->vkGetDeviceQueue(dev, fam, idx, &q);
        bool fnsOk = d->vkCmdBlitImage && d->vkCmdCopyImage && d->vkMapMemory && d->vkInvalidateMappedMemoryRanges &&
                     d->surfCaps;
        if (q && fnsOk && setLoaderData(dev, q) == VK_SUCCESS) {
            d->copyQueue = q;
            d->copyFamily = fam;
            d->copyIsGraphics = famGraphics;
            d->worker = new Worker();
            d->active = true;
            logf("layer queue prepared (family %u index %u, %s)", fam, idx, famGraphics ? "graphics" : "transfer-only");
        } else {
            logf("could not set up the extra queue -> bypass disabled");
        }
    }
    std::lock_guard<std::mutex> lk(g_lock);
    g_dev[key(dev)] = d;
    return r;
}

static VKAPI_ATTR void VKAPI_CALL XP_DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) {
    DevData* d = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        auto it = g_dev.find(key(device));
        if (it != g_dev.end()) { d = it->second; g_dev.erase(it); }
    }
    if (!d) return;
    if (d->worker && !d->worker->flush()) {  // stuck worker: free nothing it may use, never join it
        d->worker->abandon();
        d->worker = nullptr;
        logf("device destroyed with the worker thread stuck: the layer's objects are left allocated");
        return;  // also skip vkDestroyDevice: the stuck call is still inside this device; the process is ending
    }
    if (d->worker) {
        std::vector<VSwap*> left;  // swapchains X-Plane did not destroy itself
        {
            std::lock_guard<std::mutex> lk(g_vsLock);
            for (auto it = g_vs.begin(); it != g_vs.end();) {
                if (it->second->d == d) { left.push_back(it->second); it = g_vs.erase(it); }
                else ++it;
            }
        }
        for (VSwap* vs : left) destroyVSwap(vs);
        delete d->worker;  // stops and joins the worker thread
        d->worker = nullptr;
    }
    if (d->active && g_intel) {
        intelDestroy(g_intel);
        g_intel = nullptr;
        logf("bypass shut down cleanly");
    }
    if (d->vkDestroyDevice) d->vkDestroyDevice(device, pAllocator);
    delete d;
}

static VKAPI_ATTR void VKAPI_CALL XP_GetDeviceQueue(VkDevice device, uint32_t fam, uint32_t index, VkQueue* pQueue) {
    DevData* d = devOf(key(device));
    if (!d) return;
    d->vkGetDeviceQueue(device, fam, index, pQueue);
    if (pQueue && *pQueue) {
        std::lock_guard<std::mutex> lk(d->qLock);
        d->queueFamily[*pQueue] = fam;
    }
}
static VKAPI_ATTR void VKAPI_CALL XP_GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* pQueue) {
    DevData* d = devOf(key(device));
    if (!d || !d->vkGetDeviceQueue2) return;
    d->vkGetDeviceQueue2(device, info, pQueue);
    if (pQueue && *pQueue) {
        std::lock_guard<std::mutex> lk(d->qLock);
        d->queueFamily[*pQueue] = info->queueFamilyIndex;
    }
}

// ---- queue operations: only need care when X-Plane re-signals a semaphore a queued frame still waits on

static bool signalsPending(DevData* d, uint32_t n, const VkSubmitInfo* s) {
    std::lock_guard<std::mutex> lk(d->semLock);
    if (d->pendingWaits.empty()) return false;
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t j = 0; j < s[i].signalSemaphoreCount; ++j)
            if (d->pendingWaits.count(s[i].pSignalSemaphores[j])) return true;
    return false;
}
static bool signalsPending2(DevData* d, uint32_t n, const VkSubmitInfo2* s) {
    std::lock_guard<std::mutex> lk(d->semLock);
    if (d->pendingWaits.empty()) return false;
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t j = 0; j < s[i].signalSemaphoreInfoCount; ++j)
            if (d->pendingWaits.count(s[i].pSignalSemaphoreInfos[j].semaphore)) return true;
    return false;
}

static VKAPI_ATTR VkResult VKAPI_CALL XP_QueueSubmit(VkQueue queue, uint32_t n, const VkSubmitInfo* s, VkFence f) {
    DevData* d = devOf(key(queue));
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    if (d->worker && signalsPending(d, n, s) && !d->worker->flush()) return VK_ERROR_DEVICE_LOST;
    if (!g_cfg.measure) return d->vkQueueSubmit(queue, n, s, f);
    uint64_t t0 = __rdtsc();
    VkResult r = d->vkQueueSubmit(queue, n, s, f);
    mAdd(M_SUBMIT, __rdtsc() - t0);
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL XP_QueueSubmit2(VkQueue queue, uint32_t n, const VkSubmitInfo2* s, VkFence f) {
    DevData* d = devOf(key(queue));
    if (!d || !d->vkQueueSubmit2) return VK_ERROR_INITIALIZATION_FAILED;
    if (d->worker && signalsPending2(d, n, s) && !d->worker->flush()) return VK_ERROR_DEVICE_LOST;
    if (!g_cfg.measure) return d->vkQueueSubmit2(queue, n, s, f);
    uint64_t t0 = __rdtsc();
    VkResult r = d->vkQueueSubmit2(queue, n, s, f);
    mAdd(M_SUBMIT, __rdtsc() - t0);
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL XP_QueueSubmit2KHR(VkQueue queue, uint32_t n, const VkSubmitInfo2* s, VkFence f) {
    DevData* d = devOf(key(queue));
    if (!d || !d->vkQueueSubmit2KHR) return VK_ERROR_INITIALIZATION_FAILED;
    if (d->worker && signalsPending2(d, n, s) && !d->worker->flush()) return VK_ERROR_DEVICE_LOST;
    if (!g_cfg.measure) return d->vkQueueSubmit2KHR(queue, n, s, f);
    uint64_t t0 = __rdtsc();
    VkResult r = d->vkQueueSubmit2KHR(queue, n, s, f);
    mAdd(M_SUBMIT, __rdtsc() - t0);
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL XP_QueueWaitIdle(VkQueue queue) {
    DevData* d = devOf(key(queue));
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    if (d->worker && !d->worker->flush()) return VK_ERROR_DEVICE_LOST;
    return d->vkQueueWaitIdle(queue);
}
static VKAPI_ATTR VkResult VKAPI_CALL XP_DeviceWaitIdle(VkDevice device) {
    DevData* d = devOf(key(device));
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    if (d->worker && !d->worker->flush()) return VK_ERROR_DEVICE_LOST;
    return d->vkDeviceWaitIdle(device);
}

// ---- swapchain

static bool createVirtualImages(VSwap* s) {
    DevData* d = s->d;
    VkDevice dev = d->device;
    VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    std::vector<uint32_t> fams = d->xpFamilies;
    if (std::find(fams.begin(), fams.end(), d->copyFamily) == fams.end()) fams.push_back(d->copyFamily);
    s->v.resize(3);
    for (auto& vi : s->v) {
        VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ic.imageType = VK_IMAGE_TYPE_2D;
        ic.format = s->ci.imageFormat;
        ic.extent = {s->ext.width, s->ext.height, 1};
        ic.mipLevels = 1;
        ic.arrayLayers = 1;
        ic.samples = VK_SAMPLE_COUNT_1_BIT;
        ic.tiling = VK_IMAGE_TILING_OPTIMAL;
        ic.usage = s->ci.imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        // the proven setup keeps EXCLUSIVE; a transfer-only layer queue needs CONCURRENT (no ownership transfers)
        if (!d->copyIsGraphics && fams.size() > 1) {
            ic.sharingMode = VK_SHARING_MODE_CONCURRENT;
            ic.queueFamilyIndexCount = (uint32_t)fams.size();
            ic.pQueueFamilyIndices = fams.data();
        } else {
            ic.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }
        ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (d->vkCreateImage(dev, &ic, nullptr, &vi.img) != VK_SUCCESS) return false;
        VkMemoryRequirements mr;
        d->vkGetImageMemoryRequirements(dev, vi.img, &mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = memTypeAny(d->memProps, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (ai.memoryTypeIndex == UINT32_MAX) return false;
        if (d->vkAllocateMemory(dev, &ai, nullptr, &vi.mem) != VK_SUCCESS) return false;
        if (d->vkBindImageMemory(dev, vi.img, vi.mem, 0) != VK_SUCCESS) return false;
        if (d->vkCreateSemaphore(dev, &semci, nullptr, &vi.copyDone) != VK_SUCCESS) return false;
    }
    return true;
}

static VKAPI_ATTR VkResult VKAPI_CALL XP_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* ci,
                                                             const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain) {
    DevData* d = devOf(key(device));
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    if (!d->active || d->internal) return d->vkCreateSwapchainKHR(device, ci, pAllocator, pSwapchain);
    g_xpThread = GetCurrentThreadId();
    if (!d->worker->flush()) return VK_ERROR_DEVICE_LOST;

    VSwap* oldVs = vsOf(ci->oldSwapchain);
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_surfLock);
        auto it = g_surfHwnd.find(ci->surface);
        if (it != g_surfHwnd.end()) hwnd = it->second;
    }
    logf("[diag] X-Plane swapchain request: %ux%u format %d colorspace %d usage 0x%x present mode %d images %u "
         "flags 0x%x layers %u", ci->imageExtent.width, ci->imageExtent.height, (int)ci->imageFormat,
         (int)ci->imageColorSpace, ci->imageUsage, (int)ci->presentMode, ci->minImageCount, ci->flags, ci->imageArrayLayers);
    // the window can have only one swapchain: retire the old virtual one's output first
    if (oldVs) {
        destroyNativeSwapchain(oldVs);
        dropIntelSwapchain(oldVs);
    }
    bool eligible = hwnd && ci->flags == 0 && ci->imageArrayLayers <= 1 && !g_virtualBroken;
    if (eligible) {
        static bool displaysLogged = false;
        if (!displaysLogged) { displaysLogged = true; logDisplays(hwnd); }
        auto* vs = new VSwap();
        vs->d = d;
        vs->hwnd = hwnd;
        vs->surface = ci->surface;
        vs->ci = *ci;
        vs->ci.pNext = nullptr;
        vs->ci.oldSwapchain = VK_NULL_HANDLE;
        vs->ext = ci->imageExtent;
        bool ok = createVirtualImages(vs) && buildNativeBase(vs);
        if (ok) {
            // first decision: the same rules as decide(), without waiting
            g_sess.lastCheck = 0;
            g_sess.onIntel = windowOnIntel(hwnd);
            g_sess.lastMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            g_sess.lastDxgi = nowMs();
            int first = PATH_NATIVE;
            if (g_cfg.mode == MODE_ON || g_cfg.path == PREF_BYPASS || g_cfg.path == PREF_CPU_COPY) first = PATH_BYPASS;
            else if (g_cfg.path == PREF_NATIVE) first = PATH_NATIVE;
            else if (g_sess.onIntel && g_sess.probe != PROBE_DONE) first = PATH_BYPASS;
            else if (g_sess.onIntel) first = g_sess.want.load();
            if (g_intelBroken) first = PATH_NATIVE;
            if (!g_sess.onIntel && g_cfg.mode == MODE_AUTO && g_cfg.path == PREF_AUTO)
                logf("X-Plane's window is not on an Intel-driven display -> native path");
            g_sess.want = first;
            vs->path = first == PATH_BYPASS ? PATH_NATIVE : PATH_BYPASS;  // force switchPath to set up
            ok = switchPath(vs, first, "start");
            if (!ok && first == PATH_BYPASS) ok = switchPath(vs, PATH_NATIVE, "bypass unavailable");
        }
        if (ok) {
            VkSwapchainKHR fake = (VkSwapchainKHR)(uintptr_t)vs;
            {
                std::lock_guard<std::mutex> lk(g_vsLock);
                g_vs[fake] = vs;
            }
            *pSwapchain = fake;
            logf("virtual swapchain %ux%u, %u images (path %s)", vs->ext.width, vs->ext.height, (unsigned)vs->v.size(),
                 pathName(vs->path));
            return VK_SUCCESS;
        }
        logf("virtual swapchain setup FAILED (%s) -> real NVIDIA swapchain",
             vs->nativeWhyNot.empty() ? "image creation" : vs->nativeWhyNot.c_str());
        destroyVSwap(vs);
    }
    VkSwapchainCreateInfoKHR c2 = *ci;
    if (oldVs) c2.oldSwapchain = VK_NULL_HANDLE;
    if (g_virtualBroken) logf("using a real NVIDIA swapchain (the layer's native path failed earlier)");
    return d->vkCreateSwapchainKHR(device, &c2, pAllocator, pSwapchain);
}

static VKAPI_ATTR void VKAPI_CALL XP_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                         const VkAllocationCallbacks* pAllocator) {
    DevData* d = devOf(key(device));
    if (!d) return;
    VSwap* vs = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_vsLock);
        auto it = g_vs.find(swapchain);
        if (it != g_vs.end()) { vs = it->second; g_vs.erase(it); }
    }
    if (vs) {
        if (d->worker->flush()) destroyVSwap(vs);  // stuck worker: leave the swapchain's objects allocated
        return;
    }
    d->vkDestroySwapchainKHR(device, swapchain, pAllocator);
}

static VKAPI_ATTR VkResult VKAPI_CALL XP_GetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t* pCount,
                                                                VkImage* pImages) {
    DevData* d = devOf(key(device));
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    if (VSwap* vs = vsOf(swapchain)) {
        uint32_t V = (uint32_t)vs->v.size();
        if (!pImages) { *pCount = V; return VK_SUCCESS; }
        uint32_t n = *pCount < V ? *pCount : V;
        for (uint32_t i = 0; i < n; ++i) pImages[i] = vs->v[i].img;
        *pCount = n;
        return n < V ? VK_INCOMPLETE : VK_SUCCESS;
    }
    return d->vkGetSwapchainImagesKHR(device, swapchain, pCount, pImages);
}

static VKAPI_ATTR VkResult VKAPI_CALL XP_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                                                              VkSemaphore sem, VkFence fence, uint32_t* pIndex) {
    if (VSwap* vs = vsOf(swapchain)) return vAcquire(vs, timeout, sem, fence, pIndex);
    DevData* d = devOf(key(device));
    return d ? d->vkAcquireNextImageKHR(device, swapchain, timeout, sem, fence, pIndex) : VK_ERROR_INITIALIZATION_FAILED;
}

static VKAPI_ATTR VkResult VKAPI_CALL XP_AcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR* info,
                                                               uint32_t* pIndex) {
    if (VSwap* vs = vsOf(info->swapchain)) return vAcquire(vs, info->timeout, info->semaphore, info->fence, pIndex);
    DevData* d = devOf(key(device));
    return d ? d->vkAcquireNextImage2KHR(device, info, pIndex) : VK_ERROR_INITIALIZATION_FAILED;
}

// One present with several swapchains, at least one of them the layer's (X-Plane does not do this; VR goes
// through OpenXR). Never hand a virtual handle to the driver: the real swapchains are presented, the virtual images
// are released unshown, and X-Plane is asked to recreate its swapchains as real NVIDIA ones from now on.
static VkResult presentMixed(DevData* d, VkQueue queue, const VkPresentInfoKHR* pi) {
    static bool told = false;
    if (!told) {
        told = true;
        logf("present with %u swapchains, including the layer's -> X-Plane gets real NVIDIA swapchains (no bypass)",
             pi->swapchainCount);
    }
    std::vector<VkSwapchainKHR> sc;
    std::vector<uint32_t> idx, where;
    VkPresentInfoKHR none{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    for (uint32_t k = 0; k < pi->swapchainCount; ++k) {
        if (VSwap* vs = vsOf(pi->pSwapchains[k])) {
            dropFrame(vs, queue, pi->pImageIndices[k], &none);  // release the image; the real present below waits
            if (pi->pResults) pi->pResults[k] = VK_ERROR_OUT_OF_DATE_KHR;
        } else {
            sc.push_back(pi->pSwapchains[k]);
            idx.push_back(pi->pImageIndices[k]);
            where.push_back(k);
        }
    }
    g_virtualBroken = true;  // vAcquire reports OUT_OF_DATE; X-Plane recreates -> real swapchains
    VkResult r = VK_SUCCESS;
    if (!sc.empty()) {
        VkPresentInfoKHR p2 = *pi;
        p2.pNext = nullptr;  // extension structs hold one entry per original swapchain
        p2.swapchainCount = (uint32_t)sc.size();
        p2.pSwapchains = sc.data();
        p2.pImageIndices = idx.data();
        std::vector<VkResult> res(sc.size(), VK_SUCCESS);
        p2.pResults = res.data();
        r = d->vkQueuePresentKHR(queue, &p2);
        if (pi->pResults)
            for (size_t k = 0; k < where.size(); ++k) pi->pResults[where[k]] = res[k];
    } else if (pi->waitSemaphoreCount) {  // nothing real to present: still consume X-Plane's wait semaphores
        std::vector<VkPipelineStageFlags> st(pi->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = pi->waitSemaphoreCount;
        si.pWaitSemaphores = pi->pWaitSemaphores;
        si.pWaitDstStageMask = st.data();
        d->vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    }
    return r < 0 ? r : VK_ERROR_OUT_OF_DATE_KHR;
}

static VKAPI_ATTR VkResult VKAPI_CALL XP_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pi) {
    DevData* d = devOf(key(queue));
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    if (d->active && pi->swapchainCount == 1)
        if (VSwap* vs = vsOf(pi->pSwapchains[0])) return vPresent(vs, queue, pi);
    if (pi->swapchainCount > 1)
        for (uint32_t k = 0; k < pi->swapchainCount; ++k)
            if (vsOf(pi->pSwapchains[k])) return presentMixed(d, queue, pi);
    if (!g_isXPlane || d->internal) return d->vkQueuePresentKHR(queue, pi);
    g_xpThread = GetCurrentThreadId();
    mFrame();
    if (!g_cfg.stats) return d->vkQueuePresentKHR(queue, pi);

    // no virtual swapchain: still measure, so mode=off and the layer's paths compare with the same numbers
    double t0 = nowMs();
    VkResult r = d->vkQueuePresentKHR(queue, pi);
    double block = nowMs() - t0;
    if (d->pLast > 0) {
        double iv = t0 - d->pLast;
        d->pFrame.add(iv);
        if (iv > 50.0) d->pOver50++;
    }
    d->pLast = t0;
    d->pPresent.add(block);
    if (d->pFrame.v.size() >= 300) {
        double mean = d->pFrame.mean();
        std::string line;
        char b[256];
        snprintf(b, sizeof(b), "FPS %.1f | path=real-swapchain (bypass NOT active) | frame ms p50 %.2f p95 %.2f p99 %.2f "
                 "max %.2f over50 %d", mean > 0 ? 1000.0 / mean : 0.0, d->pFrame.pct(0.5), d->pFrame.pct(0.95),
                 d->pFrame.pct(0.99), d->pFrame.maxv(), d->pOver50);
        line = b;
        addSeries(line, "nvidia_present_blocked", d->pPresent);
        logf("%s", line.c_str());
        d->pFrame.clear();
        d->pPresent.clear();
        d->pOver50 = 0;
    }
    return r;
}

// ---- other functions that take a swapchain: never hand the fake handle to the driver

static VKAPI_ATTR VkResult VKAPI_CALL XP_GetSwapchainStatusKHR(VkDevice device, VkSwapchainKHR sc) {
    if (vsOf(sc)) return VK_SUCCESS;
    DevData* d = devOf(key(device));
    return d->vkGetSwapchainStatusKHR(device, sc);
}
static VKAPI_ATTR void VKAPI_CALL XP_SetHdrMetadataEXT(VkDevice device, uint32_t n, const VkSwapchainKHR* scs,
                                                       const VkHdrMetadataEXT* md) {
    DevData* d = devOf(key(device));
    std::vector<VkSwapchainKHR> s2;
    std::vector<VkHdrMetadataEXT> m2;
    for (uint32_t i = 0; i < n; ++i)
        if (!vsOf(scs[i])) { s2.push_back(scs[i]); m2.push_back(md[i]); }
    if (!s2.empty()) d->vkSetHdrMetadataEXT(device, (uint32_t)s2.size(), s2.data(), m2.data());
}
static VKAPI_ATTR VkResult VKAPI_CALL XP_WaitForPresentKHR(VkDevice device, VkSwapchainKHR sc, uint64_t id, uint64_t t) {
    if (vsOf(sc)) return VK_SUCCESS;
    DevData* d = devOf(key(device));
    return d->vkWaitForPresentKHR(device, sc, id, t);
}
static VKAPI_ATTR VkResult VKAPI_CALL XP_GetRefreshCycleDurationGOOGLE(VkDevice device, VkSwapchainKHR sc,
                                                                        VkRefreshCycleDurationGOOGLE* p) {
    if (VSwap* vs = vsOf(sc)) {  // the refresh rate of the screen X-Plane's window is on (60 Hz if unknown)
        DWORD hz = 0;
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        HMONITOR m = vs->hwnd ? MonitorFromWindow(vs->hwnd, MONITOR_DEFAULTTONEAREST) : nullptr;
        if (m && GetMonitorInfoW(m, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
            hz = dm.dmDisplayFrequency;
        if (hz < 24 || hz > 1000) hz = 60;
        p->refreshDuration = 1000000000ull / hz;
        return VK_SUCCESS;
    }
    DevData* d = devOf(key(device));
    return d->vkGetRefreshCycleDurationGOOGLE(device, sc, p);
}
static VKAPI_ATTR VkResult VKAPI_CALL XP_GetPastPresentationTimingGOOGLE(VkDevice device, VkSwapchainKHR sc, uint32_t* n,
                                                                          VkPastPresentationTimingGOOGLE* p) {
    if (vsOf(sc)) { *n = 0; return VK_SUCCESS; }
    DevData* d = devOf(key(device));
    return d->vkGetPastPresentationTimingGOOGLE(device, sc, n, p);
}
static VKAPI_ATTR VkResult VKAPI_CALL XP_AcquireFullScreenExclusiveModeEXT(VkDevice device, VkSwapchainKHR sc) {
    if (vsOf(sc)) return VK_SUCCESS;
    DevData* d = devOf(key(device));
    return d->vkAcquireFullScreenExclusiveModeEXT(device, sc);
}
static VKAPI_ATTR VkResult VKAPI_CALL XP_ReleaseFullScreenExclusiveModeEXT(VkDevice device, VkSwapchainKHR sc) {
    if (vsOf(sc)) return VK_SUCCESS;
    DevData* d = devOf(key(device));
    return d->vkReleaseFullScreenExclusiveModeEXT(device, sc);
}
static VKAPI_ATTR VkResult VKAPI_CALL XP_ReleaseSwapchainImagesEXT(VkDevice device, const VkReleaseSwapchainImagesInfoEXT* info) {
    if (VSwap* vs = vsOf(info->swapchain)) {
        std::lock_guard<std::mutex> lk(vs->m);
        for (uint32_t i = 0; i < info->imageIndexCount; ++i)
            if (info->pImageIndices[i] < vs->v.size() && vs->v[info->pImageIndices[i]].state == 1)
                vs->v[info->pImageIndices[i]].state = 0;
        vs->cv.notify_all();
        return VK_SUCCESS;
    }
    DevData* d = devOf(key(device));
    return d->vkReleaseSwapchainImagesEXT(device, info);
}
static VKAPI_ATTR VkResult VKAPI_CALL XP_SetLatencySleepModeNV(VkDevice device, VkSwapchainKHR sc,
                                                                const VkLatencySleepModeInfoNV* info) {
    if (vsOf(sc)) return VK_SUCCESS;
    DevData* d = devOf(key(device));
    return d->vkSetLatencySleepModeNV(device, sc, info);
}
static VKAPI_ATTR VkResult VKAPI_CALL XP_LatencySleepNV(VkDevice device, VkSwapchainKHR sc, const VkLatencySleepInfoNV* info) {
    DevData* d = devOf(key(device));
    if (vsOf(sc)) {
        if (d->vkSignalSemaphore && info && info->signalSemaphore) {
            VkSemaphoreSignalInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
            si.semaphore = info->signalSemaphore;
            si.value = info->value;
            d->vkSignalSemaphore(device, &si);
        }
        return VK_SUCCESS;
    }
    return d->vkLatencySleepNV(device, sc, info);
}
static VKAPI_ATTR void VKAPI_CALL XP_SetLatencyMarkerNV(VkDevice device, VkSwapchainKHR sc, const VkSetLatencyMarkerInfoNV* info) {
    if (vsOf(sc)) return;
    DevData* d = devOf(key(device));
    d->vkSetLatencyMarkerNV(device, sc, info);
}
static VKAPI_ATTR void VKAPI_CALL XP_GetLatencyTimingsNV(VkDevice device, VkSwapchainKHR sc, VkGetLatencyMarkerInfoNV* info) {
    if (vsOf(sc)) { if (info) info->timingCount = 0; return; }
    DevData* d = devOf(key(device));
    d->vkGetLatencyTimingsNV(device, sc, info);
}
static VKAPI_ATTR VkResult VKAPI_CALL XP_SetDebugUtilsObjectNameEXT(VkDevice device, const VkDebugUtilsObjectNameInfoEXT* info) {
    if (info && info->objectType == VK_OBJECT_TYPE_SWAPCHAIN_KHR && vsOf((VkSwapchainKHR)info->objectHandle))
        return VK_SUCCESS;
    DevData* d = devOf(key(device));
    return d && d->vkSetDebugUtilsObjectNameEXT ? d->vkSetDebugUtilsObjectNameEXT(device, info) : VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL XP_SetDebugUtilsObjectTagEXT(VkDevice device, const VkDebugUtilsObjectTagInfoEXT* info) {
    if (info && info->objectType == VK_OBJECT_TYPE_SWAPCHAIN_KHR && vsOf((VkSwapchainKHR)info->objectHandle))
        return VK_SUCCESS;
    DevData* d = devOf(key(device));
    return d && d->vkSetDebugUtilsObjectTagEXT ? d->vkSetDebugUtilsObjectTagEXT(device, info) : VK_SUCCESS;
}

// ================================================================ proc addr

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL XP_GetDeviceProcAddr(VkDevice device, const char* pName);

struct Hook { const char* name; PFN_vkVoidFunction fn; };
static const Hook kDeviceHooks[] = {
    {"vkGetDeviceProcAddr", (PFN_vkVoidFunction)XP_GetDeviceProcAddr},
    {"vkDestroyDevice", (PFN_vkVoidFunction)XP_DestroyDevice},
    {"vkGetDeviceQueue", (PFN_vkVoidFunction)XP_GetDeviceQueue},
    {"vkGetDeviceQueue2", (PFN_vkVoidFunction)XP_GetDeviceQueue2},
    {"vkQueueSubmit", (PFN_vkVoidFunction)XP_QueueSubmit},
    {"vkQueueSubmit2", (PFN_vkVoidFunction)XP_QueueSubmit2},
    {"vkQueueSubmit2KHR", (PFN_vkVoidFunction)XP_QueueSubmit2KHR},
    {"vkQueueWaitIdle", (PFN_vkVoidFunction)XP_QueueWaitIdle},
    {"vkDeviceWaitIdle", (PFN_vkVoidFunction)XP_DeviceWaitIdle},
    {"vkCreateSwapchainKHR", (PFN_vkVoidFunction)XP_CreateSwapchainKHR},
    {"vkDestroySwapchainKHR", (PFN_vkVoidFunction)XP_DestroySwapchainKHR},
    {"vkGetSwapchainImagesKHR", (PFN_vkVoidFunction)XP_GetSwapchainImagesKHR},
    {"vkAcquireNextImageKHR", (PFN_vkVoidFunction)XP_AcquireNextImageKHR},
    {"vkAcquireNextImage2KHR", (PFN_vkVoidFunction)XP_AcquireNextImage2KHR},
    {"vkQueuePresentKHR", (PFN_vkVoidFunction)XP_QueuePresentKHR},
    {"vkGetSwapchainStatusKHR", (PFN_vkVoidFunction)XP_GetSwapchainStatusKHR},
    {"vkSetHdrMetadataEXT", (PFN_vkVoidFunction)XP_SetHdrMetadataEXT},
    {"vkWaitForPresentKHR", (PFN_vkVoidFunction)XP_WaitForPresentKHR},
    {"vkGetRefreshCycleDurationGOOGLE", (PFN_vkVoidFunction)XP_GetRefreshCycleDurationGOOGLE},
    {"vkGetPastPresentationTimingGOOGLE", (PFN_vkVoidFunction)XP_GetPastPresentationTimingGOOGLE},
    {"vkAcquireFullScreenExclusiveModeEXT", (PFN_vkVoidFunction)XP_AcquireFullScreenExclusiveModeEXT},
    {"vkReleaseFullScreenExclusiveModeEXT", (PFN_vkVoidFunction)XP_ReleaseFullScreenExclusiveModeEXT},
    {"vkReleaseSwapchainImagesEXT", (PFN_vkVoidFunction)XP_ReleaseSwapchainImagesEXT},
    {"vkSetLatencySleepModeNV", (PFN_vkVoidFunction)XP_SetLatencySleepModeNV},
    {"vkLatencySleepNV", (PFN_vkVoidFunction)XP_LatencySleepNV},
    {"vkSetLatencyMarkerNV", (PFN_vkVoidFunction)XP_SetLatencyMarkerNV},
    {"vkGetLatencyTimingsNV", (PFN_vkVoidFunction)XP_GetLatencyTimingsNV},
    {"vkSetDebugUtilsObjectNameEXT", (PFN_vkVoidFunction)XP_SetDebugUtilsObjectNameEXT},
    {"vkSetDebugUtilsObjectTagEXT", (PFN_vkVoidFunction)XP_SetDebugUtilsObjectTagEXT},
};

static PFN_vkVoidFunction ourDeviceFn(const char* n) {
    for (const Hook& h : kDeviceHooks)
        if (!strcmp(n, h.name)) return h.fn;
    return nullptr;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL XP_GetDeviceProcAddr(VkDevice device, const char* pName) {
    if (!strcmp(pName, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)XP_GetDeviceProcAddr;
    DevData* d = devOf(key(device));
    if (!d) return nullptr;
    PFN_vkVoidFunction next = d->gdpa(device, pName);
    if (next)
        if (PFN_vkVoidFunction f = ourDeviceFn(pName)) return f;  // only hook what the driver supports
    if (next && g_cfg.measure && g_isXPlane && !d->internal && (!g_mDev || g_mDev == d))
        if (const MeasureHook* h = findMeasure(pName)) {
            if (!g_mDev) {
                g_mDev = d;
                logf("[diag] measure_driver=1: timing X-Plane's Vulkan calls into the driver (%u functions available)",
                     (unsigned)(sizeof(kMeasureHooks) / sizeof(kMeasureHooks[0])));
            }
            *h->next = next;
            return h->fn;
        }
    return next;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL XP_GetInstanceProcAddr(VkInstance instance, const char* pName) {
    if (!strcmp(pName, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)XP_GetInstanceProcAddr;
    if (!strcmp(pName, "vkCreateInstance")) return (PFN_vkVoidFunction)XP_CreateInstance;
    if (!strcmp(pName, "vkDestroyInstance")) return (PFN_vkVoidFunction)XP_DestroyInstance;
    if (!strcmp(pName, "vkCreateDevice")) return (PFN_vkVoidFunction)XP_CreateDevice;
    if (!instance) return nullptr;
    InstData id;
    if (!instOf(key(instance), &id)) return nullptr;
    PFN_vkVoidFunction n = id.gipa(instance, pName);
    if (!n) return nullptr;
    if (!strcmp(pName, "vkCreateWin32SurfaceKHR")) return (PFN_vkVoidFunction)XP_CreateWin32SurfaceKHR;
    if (!strcmp(pName, "vkGetPhysicalDeviceSurfaceFormatsKHR")) return (PFN_vkVoidFunction)XP_GetPhysicalDeviceSurfaceFormatsKHR;
    if (!strcmp(pName, "vkGetPhysicalDeviceSurfaceFormats2KHR")) return (PFN_vkVoidFunction)XP_GetPhysicalDeviceSurfaceFormats2KHR;
    if (PFN_vkVoidFunction f = ourDeviceFn(pName)) return f;
    return n;
}

// ================================================================ exports

LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* p) {
    if (!p || p->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) return VK_ERROR_INITIALIZATION_FAILED;
    std::call_once(g_initOnce, initOnce);
    if (p->loaderLayerInterfaceVersion >= 2) {
        p->pfnGetInstanceProcAddr = XP_GetInstanceProcAddr;
        p->pfnGetDeviceProcAddr = XP_GetDeviceProcAddr;
        p->pfnGetPhysicalDeviceProcAddr = nullptr;
        p->loaderLayerInterfaceVersion = 2;
    }
    return VK_SUCCESS;
}

LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL XPOB_GetInstanceProcAddr(VkInstance i, const char* n) {
    return XP_GetInstanceProcAddr(i, n);
}
LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL XPOB_GetDeviceProcAddr(VkDevice d, const char* n) {
    return XP_GetDeviceProcAddr(d, n);
}
