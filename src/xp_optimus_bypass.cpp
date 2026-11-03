// XP Optimus Bypass — a Vulkan implicit layer for X-Plane 12 on Optimus laptops without a MUX switch.
//
// Problem: on laptops whose built-in screen is wired to the Intel iGPU, NVIDIA's driver has to route every
// Vulkan frame from the GeForce to the iGPU. On the author's laptop (i5-10300H + GTX 1650, no MUX, no
// Windows CASO support) that path cost ~47 ms per frame under load, capping X-Plane at ~20 FPS.
//
// What this layer does (only inside X-Plane.exe):
//   * X-Plane renders into ordinary images owned by the layer instead of a real NVIDIA swapchain.
//   * A worker thread copies each finished frame from the GeForce into pinned system memory
//     (VK_EXT_external_memory_host), and an Intel Vulkan device the layer creates itself copies it into
//     an Intel swapchain on X-Plane's window and presents it. NVIDIA's own presentation path is never used.
//
// Config: xp_optimus_bypass.ini next to the DLL (see README). Log: xp_optimus_bypass.log (rewritten each run).
// License: MIT (see LICENSE).

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define LAYER_EXPORT extern "C" __declspec(dllexport)
#define XPOB_VERSION "0.9.1.1-beta"

// ================================================================ config & log

enum Mode { MODE_OFF = 0, MODE_AUTO = 1, MODE_ON = 2 };
static int g_mode = MODE_AUTO;
static bool g_vsync = false;
static bool g_stats = true;
static bool g_isXPlane = false;
static std::wstring g_dir;
static std::once_flag g_initOnce;
static std::mutex g_logLock;
static LARGE_INTEGER g_freq;
static thread_local bool g_internalCreate = false;
// set when the Intel side fails while flying: X-Plane is told to recreate its swapchain and gets NVIDIA's normal one
static std::atomic<bool> g_intelBroken{false};

static void logf(const char* fmt, ...) {
    if (!g_isXPlane) return;
    std::lock_guard<std::mutex> lk(g_logLock);
    FILE* f = _wfopen((g_dir + L"xp_optimus_bypass.log").c_str(), L"a");
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static double nowMs() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) * 1000.0 / double(g_freq.QuadPart);
}

// Windows build and X-Plane.exe file version, for bug reports
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
static void fmtDriver(const VkPhysicalDeviceProperties& p, char* out, size_t n) {
    uint32_t v = p.driverVersion;
    if (p.vendorID == 0x10DE)
        snprintf(out, n, "%u.%02u", (v >> 22) & 0x3ff, (v >> 14) & 0xff);
    else if (p.vendorID == 0x8086)
        snprintf(out, n, "%u.%u", v >> 14, v & 0x3fff);
    else
        snprintf(out, n, "0x%08x", v);
}

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

    FILE* f = _wfopen((g_dir + L"xp_optimus_bypass.ini").c_str(), L"r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char s[64];
            unsigned v;
            if (line[0] == ';' || line[0] == '#') continue;
            if (sscanf(line, " mode = %63s", s) == 1) {
                if (!_stricmp(s, "off")) g_mode = MODE_OFF;
                else if (!_stricmp(s, "on")) g_mode = MODE_ON;
                else g_mode = MODE_AUTO;
            } else if (sscanf(line, " vsync = %u", &v) == 1) g_vsync = (v != 0);
            else if (sscanf(line, " stats = %u", &v) == 1) g_stats = (v != 0);
        }
        fclose(f);
    }
    // fresh log every run
    FILE* lf = _wfopen((g_dir + L"xp_optimus_bypass.log").c_str(), L"w");
    if (lf) fclose(lf);
    logf("XP Optimus Bypass %s loaded in X-Plane.exe | mode=%s vsync=%d stats=%d", XPOB_VERSION,
         g_mode == MODE_OFF ? "off" : g_mode == MODE_ON ? "on" : "auto", g_vsync ? 1 : 0, g_stats ? 1 : 0);
    // X-Plane-only install: the loader only loads the layer when this variable is set (start_xplane.bat)
    char en[8] = {0};
    bool viaEnv = GetEnvironmentVariableA("XP_OPTIMUS_BYPASS_ENABLE", en, sizeof(en)) > 0;
    logf("started with XP_OPTIMUS_BYPASS_ENABLE=1 (start_xplane.bat or launch option): %s", viaEnv ? "yes" : "no");
    logSystemInfo(e);
}

// ---- which adapter drives a display (Optimus: the built-in screen is on the Intel iGPU)

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
    void flush() {
        std::unique_lock<std::mutex> lk(m_);
        uint64_t target = pushed_;
        done_.wait(lk, [&] { return finished_ >= target; });
    }

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
};

struct DevData {
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    PFN_vkSetDeviceLoaderData setLoaderData = nullptr;
    VkPhysicalDeviceMemoryProperties memProps{};
    bool internal = false;   // a device this layer created itself
    bool active = false;     // bypass prepared (extra queue + host-memory extension)
    uint32_t copyFamily = UINT32_MAX;
    VkQueue copyQueue = VK_NULL_HANDLE;
    std::mutex copyQLock;
    Worker* worker = nullptr;
    std::mutex semLock;
    std::unordered_multiset<VkSemaphore> pendingWaits;  // X-Plane semaphores a queued frame still waits on
    // FPS stats when the bypass is not used (mode=off, or not applicable) — X-Plane's main thread only
    uint64_t pFrames = 0;
    double pLast = 0, pSumInterval = 0, pSumPresent = 0, pMaxInterval = 0, pMaxPresent = 0;
    int pHitches = 0;

#define DFN(n) PFN_##n n = nullptr
    DFN(vkDestroyDevice); DFN(vkGetDeviceQueue); DFN(vkQueueSubmit); DFN(vkQueueSubmit2); DFN(vkQueueSubmit2KHR);
    DFN(vkQueueWaitIdle); DFN(vkDeviceWaitIdle); DFN(vkCreateSwapchainKHR); DFN(vkDestroySwapchainKHR);
    DFN(vkGetSwapchainImagesKHR); DFN(vkAcquireNextImageKHR); DFN(vkAcquireNextImage2KHR); DFN(vkQueuePresentKHR);
    DFN(vkCreateImage); DFN(vkDestroyImage); DFN(vkGetImageMemoryRequirements); DFN(vkAllocateMemory);
    DFN(vkFreeMemory); DFN(vkBindImageMemory); DFN(vkCreateSemaphore); DFN(vkDestroySemaphore);
    DFN(vkCreateFence); DFN(vkDestroyFence); DFN(vkWaitForFences); DFN(vkResetFences); DFN(vkCreateCommandPool);
    DFN(vkDestroyCommandPool); DFN(vkAllocateCommandBuffers); DFN(vkBeginCommandBuffer); DFN(vkEndCommandBuffer);
    DFN(vkResetCommandBuffer); DFN(vkCmdPipelineBarrier); DFN(vkCmdCopyImageToBuffer); DFN(vkCreateBuffer);
    DFN(vkDestroyBuffer); DFN(vkGetBufferMemoryRequirements); DFN(vkBindBufferMemory);
    DFN(vkGetMemoryHostPointerPropertiesEXT); DFN(vkSetDebugUtilsObjectNameEXT); DFN(vkSetDebugUtilsObjectTagEXT);
    DFN(vkGetSwapchainStatusKHR); DFN(vkSetHdrMetadataEXT); DFN(vkWaitForPresentKHR);
    DFN(vkGetRefreshCycleDurationGOOGLE); DFN(vkGetPastPresentationTimingGOOGLE);
    DFN(vkAcquireFullScreenExclusiveModeEXT); DFN(vkReleaseFullScreenExclusiveModeEXT);
    DFN(vkReleaseSwapchainImagesEXT); DFN(vkSetLatencySleepModeNV); DFN(vkLatencySleepNV);
    DFN(vkSetLatencyMarkerNV); DFN(vkGetLatencyTimingsNV); DFN(vkSignalSemaphore); DFN(vkSignalSemaphoreKHR);
#undef DFN
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
#define IFN(n) PFN_##n n = nullptr
    IFN(vkGetInstanceProcAddr); IFN(vkGetDeviceProcAddr); IFN(vkDestroyInstance);
    IFN(vkEnumeratePhysicalDevices); IFN(vkGetPhysicalDeviceProperties);
    IFN(vkGetPhysicalDeviceQueueFamilyProperties); IFN(vkGetPhysicalDeviceMemoryProperties);
    IFN(vkGetPhysicalDeviceSurfaceSupportKHR); IFN(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    IFN(vkGetPhysicalDeviceSurfaceFormatsKHR); IFN(vkGetPhysicalDeviceSurfacePresentModesKHR);
    IFN(vkCreateWin32SurfaceKHR); IFN(vkDestroySurfaceKHR); IFN(vkCreateDevice);
    IFN(vkDestroyDevice); IFN(vkGetDeviceQueue); IFN(vkCreateSwapchainKHR); IFN(vkDestroySwapchainKHR);
    IFN(vkGetSwapchainImagesKHR); IFN(vkAcquireNextImageKHR); IFN(vkQueuePresentKHR); IFN(vkQueueSubmit);
    IFN(vkCreateCommandPool); IFN(vkDestroyCommandPool); IFN(vkAllocateCommandBuffers); IFN(vkFreeCommandBuffers);
    IFN(vkBeginCommandBuffer); IFN(vkEndCommandBuffer); IFN(vkResetCommandBuffer); IFN(vkCmdPipelineBarrier);
    IFN(vkCmdCopyBufferToImage); IFN(vkCreateBuffer); IFN(vkDestroyBuffer); IFN(vkGetBufferMemoryRequirements);
    IFN(vkAllocateMemory); IFN(vkFreeMemory); IFN(vkBindBufferMemory); IFN(vkGetMemoryHostPointerPropertiesEXT);
    IFN(vkCreateSemaphore); IFN(vkDestroySemaphore); IFN(vkCreateFence); IFN(vkDestroyFence);
    IFN(vkWaitForFences); IFN(vkResetFences); IFN(vkDeviceWaitIdle);
#undef IFN
};

static IntelCtx* g_intel = nullptr;  // X-Plane has a single window

static IntelCtx* intelCreate(HWND hwnd) {
    HMODULE lib = GetModuleHandleW(L"vulkan-1.dll");
    if (!lib) lib = LoadLibraryW(L"vulkan-1.dll");
    if (!lib) { logf("Intel: vulkan-1.dll not found"); return nullptr; }
    auto* c = new IntelCtx();
    c->hwnd = hwnd;
    c->vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(lib, "vkGetInstanceProcAddr");
    auto createInstance = c->vkGetInstanceProcAddr ?
        (PFN_vkCreateInstance)c->vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance") : nullptr;
    if (!createInstance) { delete c; return nullptr; }

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
    if (r != VK_SUCCESS) { logf("Intel: vkCreateInstance failed (%d)", (int)r); delete c; return nullptr; }

#define LI(n) c->n = (PFN_##n)c->vkGetInstanceProcAddr(c->inst, #n)
    LI(vkGetDeviceProcAddr); LI(vkDestroyInstance); LI(vkEnumeratePhysicalDevices); LI(vkGetPhysicalDeviceProperties);
    LI(vkGetPhysicalDeviceQueueFamilyProperties); LI(vkGetPhysicalDeviceMemoryProperties);
    LI(vkGetPhysicalDeviceSurfaceSupportKHR); LI(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    LI(vkGetPhysicalDeviceSurfaceFormatsKHR); LI(vkGetPhysicalDeviceSurfacePresentModesKHR);
    LI(vkCreateWin32SurfaceKHR); LI(vkDestroySurfaceKHR); LI(vkCreateDevice);
#undef LI
    auto fail = [&](const char* why) -> IntelCtx* {
        logf("Intel: %s", why);
        if (c->pool && c->dev) c->vkDestroyCommandPool(c->dev, c->pool, nullptr);
        if (c->dev) c->vkDestroyDevice(c->dev, nullptr);
        if (c->surface) c->vkDestroySurfaceKHR(c->inst, c->surface, nullptr);
        c->vkDestroyInstance(c->inst, nullptr);
        delete c;
        return nullptr;
    };

    uint32_t n = 0;
    c->vkEnumeratePhysicalDevices(c->inst, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    c->vkEnumeratePhysicalDevices(c->inst, &n, pds.data());
    for (auto p : pds) {
        VkPhysicalDeviceProperties pr;
        c->vkGetPhysicalDeviceProperties(p, &pr);
        if (pr.vendorID == 0x8086) {
            char dv[32];
            fmtDriver(pr, dv, sizeof(dv));
            c->pd = p;
            logf("Intel: using %s, driver %s, Vulkan %u.%u", pr.deviceName, dv, VK_API_VERSION_MAJOR(pr.apiVersion),
                 VK_API_VERSION_MINOR(pr.apiVersion));
            break;
        }
    }
    if (!c->pd) return fail("no Intel Vulkan device found");
    c->vkGetPhysicalDeviceMemoryProperties(c->pd, &c->mem);

    VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = GetModuleHandleW(nullptr);
    sci.hwnd = hwnd;
    if (c->vkCreateWin32SurfaceKHR(c->inst, &sci, nullptr, &c->surface) != VK_SUCCESS)
        return fail("could not create a surface on X-Plane's window");

    c->vkGetPhysicalDeviceQueueFamilyProperties(c->pd, &n, nullptr);
    std::vector<VkQueueFamilyProperties> fams(n);
    c->vkGetPhysicalDeviceQueueFamilyProperties(c->pd, &n, fams.data());
    bool found = false;
    for (uint32_t f = 0; f < n && !found; ++f) {
        VkBool32 sup = VK_FALSE;
        c->vkGetPhysicalDeviceSurfaceSupportKHR(c->pd, f, c->surface, &sup);
        if (sup && (fams[f].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT))) { c->fam = f; found = true; }
    }
    if (!found) return fail("Intel GPU cannot present to X-Plane's window");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = c->fam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* dexts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 2;
    dci.ppEnabledExtensionNames = dexts;
    g_internalCreate = true;
    r = c->vkCreateDevice(c->pd, &dci, nullptr, &c->dev);
    g_internalCreate = false;
    if (r != VK_SUCCESS) return fail("vkCreateDevice failed (VK_EXT_external_memory_host missing?)");
#define LD(n) c->n = (PFN_##n)c->vkGetDeviceProcAddr(c->dev, #n)
    LD(vkDestroyDevice); LD(vkGetDeviceQueue); LD(vkCreateSwapchainKHR); LD(vkDestroySwapchainKHR);
    LD(vkGetSwapchainImagesKHR); LD(vkAcquireNextImageKHR); LD(vkQueuePresentKHR); LD(vkQueueSubmit);
    LD(vkCreateCommandPool); LD(vkDestroyCommandPool); LD(vkAllocateCommandBuffers); LD(vkFreeCommandBuffers);
    LD(vkBeginCommandBuffer); LD(vkEndCommandBuffer); LD(vkResetCommandBuffer); LD(vkCmdPipelineBarrier);
    LD(vkCmdCopyBufferToImage); LD(vkCreateBuffer); LD(vkDestroyBuffer); LD(vkGetBufferMemoryRequirements);
    LD(vkAllocateMemory); LD(vkFreeMemory); LD(vkBindBufferMemory); LD(vkGetMemoryHostPointerPropertiesEXT);
    LD(vkCreateSemaphore); LD(vkDestroySemaphore); LD(vkCreateFence); LD(vkDestroyFence);
    LD(vkWaitForFences); LD(vkResetFences); LD(vkDeviceWaitIdle);
#undef LD
    c->vkGetDeviceQueue(c->dev, c->fam, 0, &c->q);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = c->fam;
    if (c->vkCreateCommandPool(c->dev, &pci, nullptr, &c->pool) != VK_SUCCESS) return fail("command pool failed");
    logf("Intel: device ready (queue family %u)", c->fam);
    return c;
}

static void intelDestroy(IntelCtx* c) {
    if (!c) return;
    if (c->dev) {
        c->vkDeviceWaitIdle(c->dev);
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

struct VSwap {
    static const int R = 3;  // frames in flight inside the layer
    DevData* d = nullptr;
    IntelCtx* ic = nullptr;
    VkExtent2D ext{};
    std::vector<VImage> v;
    uint32_t nextV = 0;
    std::mutex m;
    std::condition_variable cv;
    std::atomic<int> err{0};
    std::atomic<bool> suggestResize{false};
    // NVIDIA side
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb[R]{};
    VkFence fence[R]{};
    bool fenceUsed[R]{};
    uint32_t ring = 0;
    // pinned host memory shared by both GPUs
    VkDeviceSize bytes = 0;
    void* host[R]{};
    VkBuffer nvBuf[R]{};
    VkDeviceMemory nvMem[R]{};
    VkBuffer iBuf[R]{};
    VkDeviceMemory iMem[R]{};
    // Intel side
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
    // stats (worker thread)
    uint64_t frames = 0;
    double lastT0 = 0, sumInterval = 0, nvWait = 0, iAcqT = 0, iPresT = 0;
    double maxInterval = 0, maxNv = 0, maxAcq = 0, maxPres = 0;
    int hitches = 0;
    std::atomic<uint64_t> xpWaitUs{0};
};

static std::mutex g_vsLock;
static std::unordered_map<VkSwapchainKHR, VSwap*> g_vs;

static VSwap* vsOf(VkSwapchainKHR sc) {
    if (!sc) return nullptr;
    std::lock_guard<std::mutex> lk(g_vsLock);
    auto it = g_vs.find(sc);
    return it == g_vs.end() ? nullptr : it->second;
}

static uint32_t memType(const VkPhysicalDeviceMemoryProperties& mp, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if (bits & (1u << i)) return i;
    return UINT32_MAX;
}

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
    uint32_t type = memType(mp, mr.memoryTypeBits & hp.memoryTypeBits,
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

static void waitIntelIdle(VSwap* s) {
    IntelCtx* c = s->ic;
    for (int k = 0; k < VSwap::R; ++k)
        if (s->iFenceUsed[k]) {
            c->vkWaitForFences(c->dev, 1, &s->iFence[k], VK_TRUE, UINT64_MAX);
            c->vkResetFences(c->dev, 1, &s->iFence[k]);
            s->iFenceUsed[k] = false;
        }
}

static void dropIntelSwapchain(VSwap* s) {
    if (!s->ic || !s->iswap) return;
    waitIntelIdle(s);
    s->ic->vkDeviceWaitIdle(s->ic->dev);
    s->ic->vkDestroySwapchainKHR(s->ic->dev, s->iswap, nullptr);
    s->iswap = VK_NULL_HANDLE;
    s->iImgs.clear();
    s->iRecreate = true;
}

static void destroyVSwap(VSwap* s) {
    DevData* d = s->d;
    VkDevice dev = d->device;
    for (int k = 0; k < VSwap::R; ++k)
        if (s->fenceUsed[k]) d->vkWaitForFences(dev, 1, &s->fence[k], VK_TRUE, UINT64_MAX);
    if (s->ic) {
        IntelCtx* c = s->ic;
        waitIntelIdle(s);
        for (int k = 0; k < VSwap::R; ++k) {
            if (s->iCb[k]) c->vkFreeCommandBuffers(c->dev, c->pool, 1, &s->iCb[k]);
            if (s->iFence[k]) c->vkDestroyFence(c->dev, s->iFence[k], nullptr);
            if (s->iAcq[k]) c->vkDestroySemaphore(c->dev, s->iAcq[k], nullptr);
            if (s->iReady[k]) c->vkDestroySemaphore(c->dev, s->iReady[k], nullptr);
            if (s->iBuf[k]) c->vkDestroyBuffer(c->dev, s->iBuf[k], nullptr);
            if (s->iMem[k]) c->vkFreeMemory(c->dev, s->iMem[k], nullptr);
        }
        if (s->iswap) {
            c->vkDeviceWaitIdle(c->dev);
            c->vkDestroySwapchainKHR(c->dev, s->iswap, nullptr);
        }
    }
    for (int k = 0; k < VSwap::R; ++k) {
        if (s->nvBuf[k]) d->vkDestroyBuffer(dev, s->nvBuf[k], nullptr);
        if (s->nvMem[k]) d->vkFreeMemory(dev, s->nvMem[k], nullptr);
        if (s->fence[k]) d->vkDestroyFence(dev, s->fence[k], nullptr);
        if (s->host[k]) VirtualFree(s->host[k], 0, MEM_RELEASE);
    }
    if (s->pool) d->vkDestroyCommandPool(dev, s->pool, nullptr);
    for (auto& vi : s->v) {
        if (vi.img) d->vkDestroyImage(dev, vi.img, nullptr);
        if (vi.mem) d->vkFreeMemory(dev, vi.mem, nullptr);
        if (vi.copyDone) d->vkDestroySemaphore(dev, vi.copyDone, nullptr);
    }
    delete s;
}

// An Intel-side failure while flying: hard errors switch back to NVIDIA's normal path at once, others after a run
// of consecutive failed frames. X-Plane is then told its swapchain is out of date and recreates it, and
// XP_CreateSwapchainKHR hands it a normal NVIDIA swapchain. Lower FPS, but no crash.
static int g_intelFailRun = 0;  // consecutive failed Intel frames (worker thread / swapchain creation only)

static void intelFailure(VSwap* s, const char* what, VkResult r) {
    bool hard = r == VK_ERROR_DEVICE_LOST || r == VK_ERROR_SURFACE_LOST_KHR;
    if (++g_intelFailRun <= 5 || hard) logf("Intel: %s failed (%d)", what, (int)r);
    if ((hard || g_intelFailRun >= 30) && !g_intelBroken.exchange(true))
        logf("Intel side not working -> switching back to NVIDIA's normal path (FPS will drop, X-Plane keeps running)");
    if (g_intelBroken) s->err = (int)VK_ERROR_OUT_OF_DATE_KHR;
}

// (Re)create the Intel swapchain if needed. Worker thread or swapchain-creation time only.
static bool ensureIntelSwapchain(VSwap* s) {
    IntelCtx* c = s->ic;
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
    if (r != VK_SUCCESS) { intelFailure(s, "swapchain creation", r); return false; }
    if (s->iswap) {
        c->vkDeviceWaitIdle(c->dev);
        c->vkDestroySwapchainKHR(c->dev, s->iswap, nullptr);
    }
    s->iswap = ns;
    uint32_t n = 0;
    c->vkGetSwapchainImagesKHR(c->dev, ns, &n, nullptr);
    s->iImgs.resize(n);
    c->vkGetSwapchainImagesKHR(c->dev, ns, &n, s->iImgs.data());
    s->iext = e;
    s->iRecreate = false;
    if (e.width != s->ext.width || e.height != s->ext.height) {
        // ask X-Plane to resize once per new window size (avoids a resize loop if sizes never match)
        static uint64_t lastAsked = 0;
        uint64_t key2 = ((uint64_t)e.width << 32) | e.height;
        if (key2 != lastAsked) { lastAsked = key2; s->suggestResize = true; }
    }
    const char* mn = s->iMode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" :
                     s->iMode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE" : "FIFO (vsync)";
    logf("Intel: swapchain %ux%u, %u images, %s", e.width, e.height, n, mn);
    return true;
}

static bool buildVSwap(VSwap* s, const VkSwapchainCreateInfoKHR& ci, VkSwapchainKHR oldIntel) {
    DevData* d = s->d;
    IntelCtx* c = s->ic;
    VkDevice dev = d->device;
    s->ext = ci.imageExtent;

    // images X-Plane renders into
    VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    s->v.resize(3);
    for (auto& vi : s->v) {
        VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ic.imageType = VK_IMAGE_TYPE_2D;
        ic.format = ci.imageFormat;
        ic.extent = {ci.imageExtent.width, ci.imageExtent.height, 1};
        ic.mipLevels = 1;
        ic.arrayLayers = 1;
        ic.samples = VK_SAMPLE_COUNT_1_BIT;
        ic.tiling = VK_IMAGE_TILING_OPTIMAL;
        ic.usage = ci.imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ic.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (d->vkCreateImage(dev, &ic, nullptr, &vi.img) != VK_SUCCESS) return false;
        VkMemoryRequirements mr;
        d->vkGetImageMemoryRequirements(dev, vi.img, &mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = memType(d->memProps, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (ai.memoryTypeIndex == UINT32_MAX) return false;
        if (d->vkAllocateMemory(dev, &ai, nullptr, &vi.mem) != VK_SUCCESS) return false;
        if (d->vkBindImageMemory(dev, vi.img, vi.mem, 0) != VK_SUCCESS) return false;
        if (d->vkCreateSemaphore(dev, &semci, nullptr, &vi.copyDone) != VK_SUCCESS) return false;
    }

    // NVIDIA copy resources
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = d->copyFamily;
    if (d->vkCreateCommandPool(dev, &pci, nullptr, &s->pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = s->pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = VSwap::R;
    if (d->vkAllocateCommandBuffers(dev, &cai, s->cb) != VK_SUCCESS) return false;
    for (int k = 0; k < VSwap::R; ++k)
        if (d->setLoaderData(dev, s->cb[k]) != VK_SUCCESS) return false;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};

    // shared pinned memory + Intel resources
    VkDeviceSize raw = (VkDeviceSize)ci.imageExtent.width * ci.imageExtent.height * 4;
    s->bytes = (raw + 65535) & ~(VkDeviceSize)65535;
    VkCommandBufferAllocateInfo icai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    icai.commandPool = c->pool;
    icai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    icai.commandBufferCount = 1;
    for (int k = 0; k < VSwap::R; ++k) {
        if (d->vkCreateFence(dev, &fci, nullptr, &s->fence[k]) != VK_SUCCESS) return false;
        s->host[k] = VirtualAlloc(nullptr, (SIZE_T)s->bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!s->host[k]) { logf("VirtualAlloc failed"); return false; }
        if (!importHost(dev, s->host[k], s->bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, d->memProps, d->vkCreateBuffer,
                        d->vkGetBufferMemoryRequirements, d->vkGetMemoryHostPointerPropertiesEXT, d->vkAllocateMemory,
                        d->vkBindBufferMemory, &s->nvBuf[k], &s->nvMem[k])) {
            logf("NVIDIA could not import host memory");
            return false;
        }
        if (!importHost(c->dev, s->host[k], s->bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, c->mem, c->vkCreateBuffer,
                        c->vkGetBufferMemoryRequirements, c->vkGetMemoryHostPointerPropertiesEXT, c->vkAllocateMemory,
                        c->vkBindBufferMemory, &s->iBuf[k], &s->iMem[k])) {
            logf("Intel could not import host memory");
            return false;
        }
        if (c->vkAllocateCommandBuffers(c->dev, &icai, &s->iCb[k]) != VK_SUCCESS) return false;
        if (c->vkCreateFence(c->dev, &fci, nullptr, &s->iFence[k]) != VK_SUCCESS) return false;
        if (c->vkCreateSemaphore(c->dev, &semci, nullptr, &s->iAcq[k]) != VK_SUCCESS) return false;
        if (c->vkCreateSemaphore(c->dev, &semci, nullptr, &s->iReady[k]) != VK_SUCCESS) return false;
    }

    // Intel swapchain format / mode (raw byte copy, so use X-Plane's BGRA8 layout)
    uint32_t n = 0;
    c->vkGetPhysicalDeviceSurfaceFormatsKHR(c->pd, c->surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(n);
    c->vkGetPhysicalDeviceSurfaceFormatsKHR(c->pd, c->surface, &n, fmts.data());
    s->iFmt = {VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    for (auto& f : fmts) if (f.format == ci.imageFormat) s->iFmt = f;
    if (s->iFmt.format == VK_FORMAT_UNDEFINED)
        for (auto& f : fmts)
            if (f.format == VK_FORMAT_B8G8R8A8_SRGB || f.format == VK_FORMAT_B8G8R8A8_UNORM) s->iFmt = f;
    if (s->iFmt.format == VK_FORMAT_UNDEFINED) { logf("Intel: no BGRA8 surface format"); return false; }
    c->vkGetPhysicalDeviceSurfacePresentModesKHR(c->pd, c->surface, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    c->vkGetPhysicalDeviceSurfacePresentModesKHR(c->pd, c->surface, &n, modes.data());
    s->iMode = VK_PRESENT_MODE_FIFO_KHR;
    if (!g_vsync) {
        for (auto m : modes) if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) s->iMode = m;
        for (auto m : modes) if (m == VK_PRESENT_MODE_MAILBOX_KHR) s->iMode = m;
    }
    s->iswap = oldIntel;  // used as oldSwapchain, then replaced
    s->iRecreate = true;
    if (!ensureIntelSwapchain(s)) {
        if (s->iswap == oldIntel) s->iswap = VK_NULL_HANDLE;  // still owned by the old VSwap
        logf("Intel: swapchain deferred (window minimized or busy)");
    }
    return true;
}

// ---- command recording

static void recordToHost(DevData* d, VSwap* s, VkCommandBuffer cb, VkImage src, VkBuffer dst) {
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    d->vkResetCommandBuffer(cb, 0);
    d->vkBeginCommandBuffer(cb, &bi);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = src;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    d->vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                            nullptr, 1, &b);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {s->ext.width, s->ext.height, 1};
    d->vkCmdCopyImageToBuffer(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &c);
    VkImageMemoryBarrier a = b;
    a.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    a.dstAccessMask = 0;
    a.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    a.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    d->vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 1, &mb, 0, nullptr,
                            1, &a);
    d->vkEndCommandBuffer(cb);
}

static void recordFromHost(IntelCtx* c, VSwap* s, VkCommandBuffer cb, VkBuffer src, VkImage dst) {
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    c->vkResetCommandBuffer(cb, 0);
    c->vkBeginCommandBuffer(cb, &bi);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = dst;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    c->vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 1, &b);
    VkBufferImageCopy r{};
    r.bufferRowLength = s->ext.width;
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {s->iext.width < s->ext.width ? s->iext.width : s->ext.width,
                     s->iext.height < s->ext.height ? s->iext.height : s->ext.height, 1};
    c->vkCmdCopyBufferToImage(cb, src, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    VkImageMemoryBarrier a = b;
    a.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    a.dstAccessMask = 0;
    a.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    a.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    c->vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                            nullptr, 1, &a);
    c->vkEndCommandBuffer(cb);
}

// ---- one frame, on the worker thread

static void releasePending(DevData* d, const std::vector<VkSemaphore>& waits) {
    std::lock_guard<std::mutex> lk(d->semLock);
    for (VkSemaphore sem : waits) {
        auto it = d->pendingWaits.find(sem);
        if (it != d->pendingWaits.end()) d->pendingWaits.erase(it);
    }
}

static void frameJob(VSwap* s, uint32_t i, std::shared_ptr<std::vector<VkSemaphore>> waits, double t0) {
    DevData* d = s->d;
    IntelCtx* c = s->ic;
    VkDevice dev = d->device;
    uint32_t k = s->ring++ % VSwap::R;
    if (s->fenceUsed[k]) {
        d->vkWaitForFences(dev, 1, &s->fence[k], VK_TRUE, UINT64_MAX);
        d->vkResetFences(dev, 1, &s->fence[k]);
        s->fenceUsed[k] = false;
    }
    if (s->iFenceUsed[k]) {
        c->vkWaitForFences(c->dev, 1, &s->iFence[k], VK_TRUE, UINT64_MAX);
        c->vkResetFences(c->dev, 1, &s->iFence[k]);
        s->iFenceUsed[k] = false;
    }
    bool okImg = i < s->v.size();

    // 1) GeForce: X-Plane's frame -> pinned host memory (waits for X-Plane's rendering on the GPU)
    std::vector<VkSemaphore> w(*waits);
    std::vector<VkPipelineStageFlags> stages(w.size(), VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkSemaphore sig = okImg ? s->v[i].copyDone : VK_NULL_HANDLE;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = (uint32_t)w.size();
    si.pWaitSemaphores = w.data();
    si.pWaitDstStageMask = stages.data();
    if (okImg) {
        recordToHost(d, s, s->cb[k], s->v[i].img, s->nvBuf[k]);
        si.commandBufferCount = 1;
        si.pCommandBuffers = &s->cb[k];
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &sig;
    }
    VkResult sr;
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
    if (!okImg || !s->fenceUsed[k]) return;

    // 2) wait until the frame is in host memory
    double a = nowMs();
    d->vkWaitForFences(dev, 1, &s->fence[k], VK_TRUE, UINT64_MAX);
    d->vkResetFences(dev, 1, &s->fence[k]);
    s->fenceUsed[k] = false;
    double tNv = nowMs() - a;

    // 3) Intel: acquire, copy, present
    if (g_intelBroken) { s->err = (int)VK_ERROR_OUT_OF_DATE_KHR; return; }  // waiting for X-Plane to recreate
    if (!ensureIntelSwapchain(s)) return;  // minimized (or failed, see intelFailure): drop this frame
    uint32_t r = 0;
    a = nowMs();
    VkResult ar = c->vkAcquireNextImageKHR(c->dev, s->iswap, UINT64_MAX, s->iAcq[k], VK_NULL_HANDLE, &r);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) {
        s->iRecreate = true;
        if (!ensureIntelSwapchain(s)) return;
        ar = c->vkAcquireNextImageKHR(c->dev, s->iswap, UINT64_MAX, s->iAcq[k], VK_NULL_HANDLE, &r);
    }
    double tAcq = nowMs() - a;
    if (ar < 0 || r >= s->iImgs.size()) {
        s->iRecreate = true;
        intelFailure(s, "acquire", ar < 0 ? ar : VK_ERROR_UNKNOWN);
        return;
    }
    if (ar == VK_SUBOPTIMAL_KHR) s->iRecreate = true;
    recordFromHost(c, s, s->iCb[k], s->iBuf[k], s->iImgs[r]);
    VkPipelineStageFlags st = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo is{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    is.waitSemaphoreCount = 1;
    is.pWaitSemaphores = &s->iAcq[k];
    is.pWaitDstStageMask = &st;
    is.commandBufferCount = 1;
    is.pCommandBuffers = &s->iCb[k];
    is.signalSemaphoreCount = 1;
    is.pSignalSemaphores = &s->iReady[k];
    VkResult isr = c->vkQueueSubmit(c->q, 1, &is, s->iFence[k]);
    if (isr != VK_SUCCESS) { intelFailure(s, "submit", isr); return; }
    s->iFenceUsed[k] = true;
    VkPresentInfoKHR p{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    p.waitSemaphoreCount = 1;
    p.pWaitSemaphores = &s->iReady[k];
    p.swapchainCount = 1;
    p.pSwapchains = &s->iswap;
    p.pImageIndices = &r;
    a = nowMs();
    VkResult pr = c->vkQueuePresentKHR(c->q, &p);
    double tPres = nowMs() - a;
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) s->iRecreate = true;
    if (pr < 0 && pr != VK_ERROR_OUT_OF_DATE_KHR) intelFailure(s, "present", pr);
    else g_intelFailRun = 0;

    // stats: averages + the worst frame of each window, so hitches are visible
    if (s->frames > 0) {
        double iv = t0 - s->lastT0;
        s->sumInterval += iv;
        if (iv > s->maxInterval) s->maxInterval = iv;
        if (iv > 50.0) s->hitches++;
    }
    s->lastT0 = t0;
    s->frames++;
    s->nvWait += tNv;
    s->iAcqT += tAcq;
    s->iPresT += tPres;
    if (tNv > s->maxNv) s->maxNv = tNv;
    if (tAcq > s->maxAcq) s->maxAcq = tAcq;
    if (tPres > s->maxPres) s->maxPres = tPres;
    if (g_stats && s->frames % 300 == 0) {
        double iv = s->sumInterval / 300.0;
        logf("FPS %.1f | avg per frame: X-Plane waited %.2f ms for a free image, GeForce frame+copy %.2f ms, Intel acquire %.2f ms, Intel present %.2f ms",
             iv > 0 ? 1000.0 / iv : 0.0, s->xpWaitUs.exchange(0) / 300000.0, s->nvWait / 300.0, s->iAcqT / 300.0,
             s->iPresT / 300.0);
        logf("    worst frame %.1f ms, frames over 50 ms: %d | max GeForce frame+copy %.1f ms, max Intel acquire %.1f ms, max Intel present %.1f ms",
             s->maxInterval, s->hitches, s->maxNv, s->maxAcq, s->maxPres);
        s->sumInterval = s->nvWait = s->iAcqT = s->iPresT = 0;
        s->maxInterval = s->maxNv = s->maxAcq = s->maxPres = 0;
        s->hitches = 0;
    }
}

// ---- what X-Plane calls

static VkResult vAcquire(VSwap* s, VkSemaphore sem, VkFence fence, uint32_t* pIndex) {
    DevData* d = s->d;
    if (g_intelBroken) return VK_ERROR_OUT_OF_DATE_KHR;  // X-Plane recreates -> normal NVIDIA swapchain
    int e = s->err.exchange(0);
    if (e < 0) return (VkResult)e;
    uint32_t i = 0;
    bool needWait = false;
    {
        std::unique_lock<std::mutex> lk(s->m);
        double t0 = nowMs();
        const uint32_t V = (uint32_t)s->v.size();
        for (;;) {
            bool found = false;
            for (uint32_t n = 0; n < V; ++n) {
                uint32_t j = (s->nextV + n) % V;
                if (s->v[j].state == 0) { i = j; found = true; break; }
            }
            if (found) break;
            s->cv.wait(lk);
        }
        s->xpWaitUs += (uint64_t)((nowMs() - t0) * 1000.0);
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
        std::lock_guard<std::mutex> lk(d->copyQLock);
        VkResult r = d->vkQueueSubmit(d->copyQueue, 1, &si, fence);
        if (r < 0) return r;
    }
    *pIndex = i;
    return VK_SUCCESS;
}

static VkResult vPresent(VSwap* s, const VkPresentInfoKHR* pi) {
    DevData* d = s->d;
    double t0 = nowMs();
    uint32_t i = pi->pImageIndices[0];
    auto waits = std::make_shared<std::vector<VkSemaphore>>(pi->pWaitSemaphores, pi->pWaitSemaphores + pi->waitSemaphoreCount);
    {
        std::lock_guard<std::mutex> lk(d->semLock);
        for (VkSemaphore sem : *waits) d->pendingWaits.insert(sem);
    }
    {
        std::lock_guard<std::mutex> lk(s->m);
        if (i < s->v.size()) s->v[i].state = 2;
    }
    d->worker->push([s, i, waits, t0] { frameJob(s, i, waits, t0); });
    VkResult res = VK_SUCCESS;
    int e = s->err.exchange(0);
    if (g_intelBroken) res = VK_ERROR_OUT_OF_DATE_KHR;
    else if (e < 0) res = (VkResult)e;
    else if (s->suggestResize.exchange(false)) res = VK_SUBOPTIMAL_KHR;  // window size changed: let X-Plane resize
    if (pi->pResults) pi->pResults[0] = res;
    return res;
}

// ================================================================ instance hooks

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
        InstData d{*pInstance, next, (PFN_vkDestroyInstance)next(*pInstance, "vkDestroyInstance")};
        std::lock_guard<std::mutex> lk(g_lock);
        g_inst[key(*pInstance)] = d;
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

static PFN_vkGetInstanceProcAddr gipaOf(VkInstance instance) {
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_inst.find(key(instance));
    return it == g_inst.end() ? nullptr : it->second.gipa;
}

static VKAPI_ATTR VkResult VKAPI_CALL XP_CreateWin32SurfaceKHR(VkInstance instance, const VkWin32SurfaceCreateInfoKHR* ci,
                                                                const VkAllocationCallbacks* a, VkSurfaceKHR* pSurface) {
    PFN_vkGetInstanceProcAddr gipa = gipaOf(instance);
    auto next = gipa ? (PFN_vkCreateWin32SurfaceKHR)gipa(instance, "vkCreateWin32SurfaceKHR") : nullptr;
    if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = next(instance, ci, a, pSurface);
    if (r == VK_SUCCESS && g_isXPlane) {
        std::lock_guard<std::mutex> lk(g_surfLock);
        g_surfHwnd[*pSurface] = ci->hwnd;
    }
    return r;
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

    VkInstance inst = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        auto it = g_inst.find(key(physicalDevice));
        if (it != g_inst.end()) inst = it->second.instance;
    }
    auto create = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;

    // Decide whether to prepare the bypass: X-Plane, rendering on NVIDIA, and an Intel GPU drives a display.
    bool want = false;
    VkDeviceCreateInfo dci = *pCreateInfo;
    std::vector<VkDeviceQueueCreateInfo> qcis(pCreateInfo->pQueueCreateInfos,
                                              pCreateInfo->pQueueCreateInfos + pCreateInfo->queueCreateInfoCount);
    std::vector<std::vector<float>> prios(qcis.size());
    std::vector<const char*> exts(pCreateInfo->ppEnabledExtensionNames,
                                  pCreateInfo->ppEnabledExtensionNames + pCreateInfo->enabledExtensionCount);
    uint32_t fam = UINT32_MAX, idx = 0;
    if (g_isXPlane && !g_internalCreate && g_mode == MODE_OFF)
        logf("mode=off: bypass disabled, only measuring FPS");
    if (g_isXPlane && !g_internalCreate && g_mode != MODE_OFF && setLoaderData) {
        auto getProps = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
        VkPhysicalDeviceProperties pr{};
        if (getProps) getProps(physicalDevice, &pr);
        bool nvidia = pr.vendorID == 0x10DE;
        bool intelDisplay = anyDisplayOnIntel();
        char dv[32];
        fmtDriver(pr, dv, sizeof(dv));
        logf("render GPU: %s, driver %s | a display is driven by an Intel GPU: %s", pr.deviceName, dv,
             intelDisplay ? "yes" : "no");
        want = nvidia && (intelDisplay || g_mode == MODE_ON);
        if (!nvidia) logf("not an NVIDIA GPU -> bypass disabled");
        else if (!want) logf("no display on an Intel GPU -> bypass disabled (set mode=on to force)");
    }
    if (want) {
        auto getQF = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties");
        std::vector<VkQueueFamilyProperties> fams;
        if (getQF) {
            uint32_t n = 0;
            getQF(physicalDevice, &n, nullptr);
            fams.resize(n);
            getQF(physicalDevice, &n, fams.data());
        }
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
        if (fam == UINT32_MAX) { logf("no free queue in the graphics family -> bypass disabled"); want = false; }
    }
    if (want) {
        bool have = false;
        for (const char* e : exts)
            if (!strcmp(e, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) have = true;
        if (!have) exts.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
        dci.pQueueCreateInfos = qcis.data();
        dci.enabledExtensionCount = (uint32_t)exts.size();
        dci.ppEnabledExtensionNames = exts.data();
    }

    VkResult r = create(physicalDevice, want ? &dci : pCreateInfo, pAllocator, pDevice);
    if (r != VK_SUCCESS && want) {
        logf("driver refused the extra queue / VK_EXT_external_memory_host (%d) -> bypass disabled", (int)r);
        want = false;
        r = create(physicalDevice, pCreateInfo, pAllocator, pDevice);
    }
    if (r != VK_SUCCESS) return r;

    auto* d = new DevData();
    VkDevice dev = *pDevice;
    d->device = dev;
    d->gdpa = gdpa;
    d->setLoaderData = setLoaderData;
    d->internal = g_internalCreate;
#define GD(n) d->n = (PFN_##n)gdpa(dev, #n)
    GD(vkDestroyDevice); GD(vkGetDeviceQueue); GD(vkQueueSubmit); GD(vkQueueSubmit2); GD(vkQueueSubmit2KHR);
    GD(vkQueueWaitIdle); GD(vkDeviceWaitIdle); GD(vkCreateSwapchainKHR); GD(vkDestroySwapchainKHR);
    GD(vkGetSwapchainImagesKHR); GD(vkAcquireNextImageKHR); GD(vkAcquireNextImage2KHR); GD(vkQueuePresentKHR);
    GD(vkCreateImage); GD(vkDestroyImage); GD(vkGetImageMemoryRequirements); GD(vkAllocateMemory); GD(vkFreeMemory);
    GD(vkBindImageMemory); GD(vkCreateSemaphore); GD(vkDestroySemaphore); GD(vkCreateFence); GD(vkDestroyFence);
    GD(vkWaitForFences); GD(vkResetFences); GD(vkCreateCommandPool); GD(vkDestroyCommandPool);
    GD(vkAllocateCommandBuffers); GD(vkBeginCommandBuffer); GD(vkEndCommandBuffer); GD(vkResetCommandBuffer);
    GD(vkCmdPipelineBarrier); GD(vkCmdCopyImageToBuffer); GD(vkCreateBuffer); GD(vkDestroyBuffer);
    GD(vkGetBufferMemoryRequirements); GD(vkBindBufferMemory); GD(vkGetMemoryHostPointerPropertiesEXT);
    GD(vkSetDebugUtilsObjectNameEXT); GD(vkSetDebugUtilsObjectTagEXT); GD(vkGetSwapchainStatusKHR);
    GD(vkSetHdrMetadataEXT); GD(vkWaitForPresentKHR); GD(vkGetRefreshCycleDurationGOOGLE);
    GD(vkGetPastPresentationTimingGOOGLE); GD(vkAcquireFullScreenExclusiveModeEXT);
    GD(vkReleaseFullScreenExclusiveModeEXT); GD(vkReleaseSwapchainImagesEXT); GD(vkSetLatencySleepModeNV);
    GD(vkLatencySleepNV); GD(vkSetLatencyMarkerNV); GD(vkGetLatencyTimingsNV); GD(vkSignalSemaphore);
    GD(vkSignalSemaphoreKHR);
#undef GD
    if (!d->vkQueueSubmit2) d->vkQueueSubmit2 = d->vkQueueSubmit2KHR;
    if (!d->vkSignalSemaphore) d->vkSignalSemaphore = d->vkSignalSemaphoreKHR;
    if (want && d->vkGetMemoryHostPointerPropertiesEXT) {
        auto gmp = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
        if (gmp) gmp(physicalDevice, &d->memProps);
        VkQueue q = VK_NULL_HANDLE;
        d->vkGetDeviceQueue(dev, fam, idx, &q);
        if (q && setLoaderData(dev, q) == VK_SUCCESS) {
            d->copyQueue = q;
            d->copyFamily = fam;
            d->worker = new Worker();
            d->active = true;
            logf("bypass prepared (extra GeForce queue: family %u index %u)", fam, idx);
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
    if (d->worker) {
        d->worker->flush();
        // swapchains X-Plane did not destroy itself
        std::vector<VSwap*> left;
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
    if (d->worker && signalsPending(d, n, s)) d->worker->flush();
    return d->vkQueueSubmit(queue, n, s, f);
}
static VKAPI_ATTR VkResult VKAPI_CALL XP_QueueSubmit2(VkQueue queue, uint32_t n, const VkSubmitInfo2* s, VkFence f) {
    DevData* d = devOf(key(queue));
    if (!d || !d->vkQueueSubmit2) return VK_ERROR_INITIALIZATION_FAILED;
    if (d->worker && signalsPending2(d, n, s)) d->worker->flush();
    return d->vkQueueSubmit2(queue, n, s, f);
}
static VKAPI_ATTR VkResult VKAPI_CALL XP_QueueSubmit2KHR(VkQueue queue, uint32_t n, const VkSubmitInfo2* s, VkFence f) {
    DevData* d = devOf(key(queue));
    if (!d || !d->vkQueueSubmit2KHR) return VK_ERROR_INITIALIZATION_FAILED;
    if (d->worker && signalsPending2(d, n, s)) d->worker->flush();
    return d->vkQueueSubmit2KHR(queue, n, s, f);
}
static VKAPI_ATTR VkResult VKAPI_CALL XP_QueueWaitIdle(VkQueue queue) {
    DevData* d = devOf(key(queue));
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    if (d->worker) d->worker->flush();
    return d->vkQueueWaitIdle(queue);
}
static VKAPI_ATTR VkResult VKAPI_CALL XP_DeviceWaitIdle(VkDevice device) {
    DevData* d = devOf(key(device));
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    if (d->worker) d->worker->flush();
    return d->vkDeviceWaitIdle(device);
}

// ---- swapchain

static VKAPI_ATTR VkResult VKAPI_CALL XP_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* ci,
                                                             const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain) {
    DevData* d = devOf(key(device));
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    if (!d->active || d->internal) return d->vkCreateSwapchainKHR(device, ci, pAllocator, pSwapchain);
    d->worker->flush();

    VSwap* oldVs = vsOf(ci->oldSwapchain);
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_surfLock);
        auto it = g_surfHwnd.find(ci->surface);
        if (it != g_surfHwnd.end()) hwnd = it->second;
    }
    bool use = hwnd && ci->flags == 0 && ci->imageArrayLayers <= 1 && (g_mode == MODE_ON || windowOnIntel(hwnd));
    if (use && g_intelBroken) {
        use = false;
        logf("Intel side failed earlier -> normal NVIDIA path for the rest of this session");
    } else if (hwnd && !use && g_mode == MODE_AUTO) logf("X-Plane's window is not on an Intel-driven display -> normal path");
    if (use && !g_intel) g_intel = intelCreate(hwnd);
    if (use && g_intel && g_intel->hwnd == hwnd) {
        auto* vs = new VSwap();
        vs->d = d;
        vs->ic = g_intel;
        VkSwapchainKHR oldI = oldVs ? oldVs->iswap : VK_NULL_HANDLE;
        if (buildVSwap(vs, *ci, oldI)) {
            if (oldVs && vs->iswap && oldI) oldVs->iswap = VK_NULL_HANDLE;  // retired into the new one
            VkSwapchainKHR fake = (VkSwapchainKHR)(uintptr_t)vs;
            {
                std::lock_guard<std::mutex> lk(g_vsLock);
                g_vs[fake] = vs;
            }
            *pSwapchain = fake;
            logf("ACTIVE: X-Plane %ux%u -> GeForce -> host memory -> Intel -> screen", ci->imageExtent.width,
                 ci->imageExtent.height);
            return VK_SUCCESS;
        }
        logf("setup FAILED -> normal path");
        destroyVSwap(vs);
    }

    // normal path; the window must not still have an Intel swapchain on it
    if (oldVs) dropIntelSwapchain(oldVs);
    VkSwapchainCreateInfoKHR c2 = *ci;
    if (oldVs) c2.oldSwapchain = VK_NULL_HANDLE;
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
        d->worker->flush();
        destroyVSwap(vs);
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
    if (VSwap* vs = vsOf(swapchain)) return vAcquire(vs, sem, fence, pIndex);
    DevData* d = devOf(key(device));
    return d ? d->vkAcquireNextImageKHR(device, swapchain, timeout, sem, fence, pIndex) : VK_ERROR_INITIALIZATION_FAILED;
}

static VKAPI_ATTR VkResult VKAPI_CALL XP_AcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR* info,
                                                               uint32_t* pIndex) {
    if (VSwap* vs = vsOf(info->swapchain)) return vAcquire(vs, info->semaphore, info->fence, pIndex);
    DevData* d = devOf(key(device));
    return d ? d->vkAcquireNextImage2KHR(device, info, pIndex) : VK_ERROR_INITIALIZATION_FAILED;
}

static VKAPI_ATTR VkResult VKAPI_CALL XP_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pi) {
    DevData* d = devOf(key(queue));
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    if (d->active && pi->swapchainCount == 1)
        if (VSwap* vs = vsOf(pi->pSwapchains[0])) return vPresent(vs, pi);
    if (!g_isXPlane || !g_stats || d->internal) return d->vkQueuePresentKHR(queue, pi);

    // bypass not in use: still measure, so mode=off and mode=auto runs can be compared with the same tool
    double t0 = nowMs();
    VkResult r = d->vkQueuePresentKHR(queue, pi);
    double block = nowMs() - t0;
    if (d->pFrames > 0) {
        double iv = t0 - d->pLast;
        d->pSumInterval += iv;
        if (iv > d->pMaxInterval) d->pMaxInterval = iv;
        if (iv > 50.0) d->pHitches++;
    }
    d->pLast = t0;
    d->pFrames++;
    d->pSumPresent += block;
    if (block > d->pMaxPresent) d->pMaxPresent = block;
    if (d->pFrames % 300 == 0) {
        double iv = d->pSumInterval / 300.0;
        logf("FPS %.1f | bypass NOT active: NVIDIA present blocked X-Plane avg %.2f ms (max %.1f ms)",
             iv > 0 ? 1000.0 / iv : 0.0, d->pSumPresent / 300.0, d->pMaxPresent);
        logf("    worst frame %.1f ms, frames over 50 ms: %d", d->pMaxInterval, d->pHitches);
        d->pSumInterval = d->pSumPresent = d->pMaxInterval = d->pMaxPresent = 0;
        d->pHitches = 0;
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
    if (vsOf(sc)) { p->refreshDuration = 16666667; return VK_SUCCESS; }
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
    return next;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL XP_GetInstanceProcAddr(VkInstance instance, const char* pName) {
    if (!strcmp(pName, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)XP_GetInstanceProcAddr;
    if (!strcmp(pName, "vkCreateInstance")) return (PFN_vkVoidFunction)XP_CreateInstance;
    if (!strcmp(pName, "vkDestroyInstance")) return (PFN_vkVoidFunction)XP_DestroyInstance;
    if (!strcmp(pName, "vkCreateDevice")) return (PFN_vkVoidFunction)XP_CreateDevice;
    if (!instance) return nullptr;
    PFN_vkGetInstanceProcAddr next = gipaOf(instance);
    if (!next) return nullptr;
    PFN_vkVoidFunction n = next(instance, pName);
    if (!n) return nullptr;
    if (!strcmp(pName, "vkCreateWin32SurfaceKHR")) return (PFN_vkVoidFunction)XP_CreateWin32SurfaceKHR;
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
