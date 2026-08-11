#include "pch.h"
#include "Telemetry.h"

#include <cstdio>
#include <dwmapi.h>
#include <dxgi.h>
#include <psapi.h>
#include <shellscalingapi.h>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shcore.lib")

namespace mutemic {
namespace {

FILE* g_file = nullptr;
bool g_enabled = false;
ULONGLONG g_startMs = 0;
CRITICAL_SECTION g_lock;
bool g_lockInit = false;
ULONGLONG g_lastSampleMs = 0;

// Tope de tamaño: si el archivo crece más que esto al arrancar, se rota.
// Sin esto, semanas de sesiones lo vuelven ilegible (y caro de leer).
constexpr long long kMaxBytes = 4 * 1024 * 1024;

std::wstring PathNextToExe(const wchar_t* name) {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s = path;
    return s.substr(0, s.find_last_of(L'\\') + 1) + name;
}

void WriteLine(const char* category, const char* event, const char* detail) {
    if (!g_enabled || !g_file) return;
    EnterCriticalSection(&g_lock);
    const ULONGLONG t = GetTickCount64() - g_startMs;
    fprintf(g_file, "[%7llu ms] %-6s %-22s %s\n",
            static_cast<unsigned long long>(t), category, event,
            detail ? detail : "");
    fflush(g_file);   // que sobreviva a un crash: es cuando más importa
    LeaveCriticalSection(&g_lock);
}

// Nombre legible del adaptador gráfico: saber si es iGPU o dedicada
// cambia por completo la lectura de cualquier medición de rendimiento.
void DumpAdapter() {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                  reinterpret_cast<void**>(&factory))))
        return;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(adapter->GetDesc1(&d))) {
            char buf[512];
            char name[256];
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name),
                                nullptr, nullptr);
            sprintf_s(buf, "index=%u name=\"%s\" vram_mb=%llu software=%d",
                      i, name,
                      static_cast<unsigned long long>(d.DedicatedVideoMemory / (1024 * 1024)),
                      (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? 1 : 0);
            WriteLine("ENV", "gpu.adapter", buf);
        }
        adapter->Release();
    }
    factory->Release();
}

BOOL CALLBACK MonitorEnumProc(HMONITOR mon, HDC, LPRECT, LPARAM) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return TRUE;

    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);

    // Frecuencia de refresco: define el presupuesto por frame.
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm);

    char buf[256];
    sprintf_s(buf, "rect=%d,%d,%d,%d work=%d,%d,%d,%d dpi=%u hz=%u primary=%d",
              mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom,
              mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom,
              dpiX, dm.dmDisplayFrequency,
              (mi.dwFlags & MONITORINFOF_PRIMARY) ? 1 : 0);
    WriteLine("ENV", "monitor", buf);
    return TRUE;
}

void DumpEnvironment() {
    char buf[512];

    // Build real de Windows (la versión "amable" miente desde Win8).
    typedef LONG(WINAPI * RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW os{};
    os.dwOSVersionInfoSize = sizeof(os);
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        auto fn = reinterpret_cast<RtlGetVersionPtr>(
            GetProcAddress(ntdll, "RtlGetVersion"));
        if (fn) fn(&os);
    }
    sprintf_s(buf, "windows=%lu.%lu build=%lu", os.dwMajorVersion,
              os.dwMinorVersion, os.dwBuildNumber);
    WriteLine("ENV", "os", buf);

    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    sprintf_s(buf, "cores=%lu ram_mb=%llu", si.dwNumberOfProcessors,
              static_cast<unsigned long long>(ms.ullTotalPhys / (1024 * 1024)));
    WriteLine("ENV", "machine", buf);

    // ¿DWM compuesto? Sin composición no hay acrylic ni cristal.
    BOOL comp = FALSE;
    DwmIsCompositionEnabled(&comp);
    sprintf_s(buf, "dwm_composition=%d process_dpi_aware=%d", comp ? 1 : 0,
              IsProcessDPIAware() ? 1 : 0);
    WriteLine("ENV", "compositor", buf);

    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, 0);
    DumpAdapter();

#ifdef NDEBUG
    WriteLine("ENV", "build", "config=Release");
#else
    WriteLine("ENV", "build", "config=Debug");
#endif
    WriteLine("ENV", "cmdline", nullptr);
}

}  // namespace

void Telemetry::Init(bool force) {
#ifdef NDEBUG
    if (!force) return;     // Release sin --diag: coste exactamente cero
#else
    (void)force;
#endif

    if (!g_lockInit) { InitializeCriticalSection(&g_lock); g_lockInit = true; }

    const std::wstring path = PathNextToExe(L"mutemic-telemetry.log");

    // Rotación: un archivo enorme es tan inútil como no tenerlo.
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        const long long size =
            (static_cast<long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        if (size > kMaxBytes) {
            const std::wstring old = PathNextToExe(L"mutemic-telemetry.old.log");
            DeleteFileW(old.c_str());
            MoveFileW(path.c_str(), old.c_str());
        }
    }

    if (_wfopen_s(&g_file, path.c_str(), L"a") != 0 || !g_file) return;
    g_enabled = true;
    g_startMs = GetTickCount64();

    SYSTEMTIME st;
    GetLocalTime(&st);
    char hdr[256];
    sprintf_s(hdr, "=== SESSION %04u-%02u-%02u %02u:%02u:%02u  pid=%lu ===",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
              GetCurrentProcessId());
    EnterCriticalSection(&g_lock);
    fprintf(g_file, "\n%s\n", hdr);
    fflush(g_file);
    LeaveCriticalSection(&g_lock);

    DumpEnvironment();
    SampleResources();
}

void Telemetry::Shutdown(const char* reason) {
    if (!g_enabled) return;
    SampleResources();
    char buf[128];
    sprintf_s(buf, "reason=%s uptime_s=%llu", reason ? reason : "?",
              static_cast<unsigned long long>((GetTickCount64() - g_startMs) / 1000));
    WriteLine("APP", "shutdown", buf);
    EnterCriticalSection(&g_lock);
    if (g_file) { fclose(g_file); g_file = nullptr; }
    g_enabled = false;
    LeaveCriticalSection(&g_lock);
}

bool Telemetry::Enabled() { return g_enabled; }

void Telemetry::Event(const char* category, const char* event, const char* detail) {
    WriteLine(category, event, detail);
}

void Telemetry::Fail(const char* category, const char* event, HRESULT hr,
                     const char* detail) {
    char buf[512];
    sprintf_s(buf, "FAILED hr=0x%08X %s", static_cast<unsigned>(hr),
              detail ? detail : "");
    WriteLine(category, event, buf);
}

void Telemetry::Metric(const char* category, const char* name, double value,
                       const char* unit) {
    char buf[128];
    sprintf_s(buf, "value=%.3f%s%s", value, unit ? " unit=" : "", unit ? unit : "");
    WriteLine(category, name, buf);
}

void Telemetry::SampleResources() {
    if (!g_enabled) return;
    // Auto-límite: como mucho una muestra por segundo.
    const ULONGLONG now = GetTickCount64();
    if (g_lastSampleMs && now - g_lastSampleMs < 1000) return;
    g_lastSampleMs = now;

    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(),
                         reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));

    DWORD handles = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles);

    char buf[320];
    sprintf_s(buf,
              "working_set_mb=%.1f private_mb=%.1f peak_ws_mb=%.1f "
              "handles=%lu gdi=%lu user=%lu",
              pmc.WorkingSetSize / 1048576.0,
              pmc.PrivateUsage / 1048576.0,
              pmc.PeakWorkingSetSize / 1048576.0,
              handles,
              GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS),
              GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS));
    WriteLine("PERF", "resources", buf);
}

ULONGLONG Telemetry::Now() { return GetTickCount64(); }

void Telemetry::Duration(const char* category, const char* event,
                         ULONGLONG since, const char* detail) {
    char buf[320];
    sprintf_s(buf, "ms=%llu %s",
              static_cast<unsigned long long>(GetTickCount64() - since),
              detail ? detail : "");
    WriteLine(category, event, buf);
}

TelemetryScope::TelemetryScope(const char* category, const char* event,
                               const char* detail)
    : cat_(category), ev_(event), detail_(detail ? detail : ""),
      start_(GetTickCount64()) {}

TelemetryScope::~TelemetryScope() {
    Telemetry::Duration(cat_, ev_, start_, detail_.empty() ? nullptr : detail_.c_str());
}

}  // namespace mutemic
