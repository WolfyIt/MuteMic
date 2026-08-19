#include "pch.h"
#include "Telemetry.h"

#include <atomic>
#include <cstdio>
#include <share.h>
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
// PID en CADA línea. El daemon y la ventana escriben en el MISMO archivo con
// relojes independientes, así que sus líneas se intercalan fuera de orden: se
// llegó a leer un FLYOUT de 160473 ms entre dos latidos de 153112 y 154564.
// Con la cabecera de sesión sola no había forma de saber de quién era cada
// línea. Se cachea una vez: GetCurrentProcessId por línea sería una llamada
// al kernel en un camino que ya escribe a disco.
DWORD g_pid = 0;
// Reloj de alta resolución. GetTickCount64 tiene ~15,6 ms de granularidad —
// MÁS que un frame entero a 165 Hz (6 ms). Con él, todo lo que este proyecto
// presupuesta mide "0" o salta de 0 a 16. QPC da microsegundos.
LARGE_INTEGER g_qpcFreq{};
LONGLONG g_startTicks = 0;

double ElapsedMs() {
    if (!g_qpcFreq.QuadPart) return 0.0;
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart - g_startTicks) * 1000.0 /
           static_cast<double>(g_qpcFreq.QuadPart);
}
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
    fprintf(g_file, "[%10.1f ms][%5lu] %-6s %-22s %s\n",
            ElapsedMs(), g_pid, category, event, detail ? detail : "");
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

    // SOSPECHOSO DE ARRANQUE LENTO. Enumerar adaptadores DXGI en un portátil
    // híbrido puede despertar la GPU dedicada, y esto corre ANTES de que la
    // app bombee mensajes — o sea, con el cursor de espera en pantalla. Es
    // dato de diagnóstico, no funcionalidad: si cuesta, se difiere.
    {
        const ULONGLONG t = Telemetry::Now();
        DumpAdapter();
        Telemetry::Duration("ENV", "dxgi.enumerate", t);
    }

    // SELLO DE BUILD. Sin esto es imposible saber si un log salió del
    // binario que se acaba de compilar o de uno viejo que seguía corriendo
    // — y se acaba diagnosticando síntomas ya arreglados.
    // __DATE__/__TIME__ es el momento de COMPILAR este archivo; la fecha
    // del exe es la del enlazado. Si no coinciden, algo no se recompiló.
    {
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        SYSTEMTIME st{};
        if (GetFileAttributesExW(exePath, GetFileExInfoStandard, &fad)) {
            FILETIME local{};
            FileTimeToLocalFileTime(&fad.ftLastWriteTime, &local);
            FileTimeToSystemTime(&local, &st);
        }
        char buf[256];
        sprintf_s(buf,
                  "config=%s compiled=\"%s %s\" exe_linked=%04u-%02u-%02u %02u:%02u:%02u",
#ifdef NDEBUG
                  "Release",
#else
                  "Debug",
#endif
                  __DATE__, __TIME__,
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        WriteLine("ENV", "build", buf);
    }

    {
        // Línea de comandos completa: distingue daemon de --settings y
        // deja ver si venía con --diag o con un verbo del CLI.
        char cmd[512] = "";
        WideCharToMultiByte(CP_UTF8, 0, GetCommandLineW(), -1, cmd, sizeof(cmd),
                            nullptr, nullptr);
        WriteLine("ENV", "cmdline", cmd);
    }
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

    // _SH_DENYNO: OTROS pueden LEER el archivo mientras la app lo escribe.
    // Con fopen normal queda bloqueado en exclusiva y el log se vuelve
    // inservible justo cuando más se necesita: durante la ejecución.
    g_file = _wfsopen(path.c_str(), L"a", _SH_DENYNO);
    if (!g_file) return;
    g_enabled = true;
    g_pid = GetCurrentProcessId();
    g_startMs = GetTickCount64();
    QueryPerformanceFrequency(&g_qpcFreq);
    LARGE_INTEGER start{};
    QueryPerformanceCounter(&start);
    g_startTicks = start.QuadPart;

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

    // Cuánto tardó el proceso desde que Windows lo creó hasta aquí. Es el
    // número que el usuario percibe como "tarda en abrir": mientras tanto el
    // shell muestra el cursor de espera porque todavía no bombeamos mensajes.
    {
        FILETIME c{}, e{}, k{}, u{};
        if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
            FILETIME nowFt{};
            GetSystemTimeAsFileTime(&nowFt);
            const ULONGLONG created =
                (static_cast<ULONGLONG>(c.dwHighDateTime) << 32) | c.dwLowDateTime;
            const ULONGLONG now =
                (static_cast<ULONGLONG>(nowFt.dwHighDateTime) << 32) | nowFt.dwLowDateTime;
            char buf[96];
            sprintf_s(buf, "since_process_start_ms=%.1f",
                      static_cast<double>(now - created) / 10000.0);
            WriteLine("ENV", "startup", buf);
        }
    }

    const ULONGLONG tEnv = Telemetry::Now();
    DumpEnvironment();
    Telemetry::Duration("ENV", "dump.total", tEnv);
    SampleResources();
}

void Telemetry::Shutdown(const char* reason) {
    if (!g_enabled) return;
    Probes::Snapshot("shutdown");
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

// Ticks de QPC, no milisegundos: la conversión la hace Duration, que es
// quien conoce la frecuencia. Devolver ms aquí perdería la precisión que
// justamente venimos a ganar.
void Telemetry::Memory(const char* category, const char* event, const char* tag) {
    if (!g_enabled) return;
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(),
                         reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    char buf[256];
    sprintf_s(buf, "at=%s private_mb=%.1f working_set_mb=%.1f",
              tag ? tag : "?",
              pmc.PrivateUsage / 1048576.0,
              pmc.WorkingSetSize / 1048576.0);
    WriteLine(category, event, buf);
}

ULONGLONG Telemetry::Now() {
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return static_cast<ULONGLONG>(c.QuadPart);
}

double Telemetry::ElapsedMsSince(ULONGLONG since) {
    if (!g_qpcFreq.QuadPart) return 0.0;
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart - static_cast<LONGLONG>(since)) *
           1000.0 / static_cast<double>(g_qpcFreq.QuadPart);
}

void Telemetry::Duration(const char* category, const char* event,
                         ULONGLONG since, const char* detail) {
    double ms = 0.0;
    if (g_qpcFreq.QuadPart) {
        LARGE_INTEGER c{};
        QueryPerformanceCounter(&c);
        ms = static_cast<double>(c.QuadPart - static_cast<LONGLONG>(since)) *
             1000.0 / static_cast<double>(g_qpcFreq.QuadPart);
    }
    char buf[320];
    sprintf_s(buf, "ms=%.3f %s", ms, detail ? detail : "");
    WriteLine(category, event, buf);
}

TelemetryScope::TelemetryScope(const char* category, const char* event,
                               const char* detail)
    : cat_(category), ev_(event), detail_(detail ? detail : ""),
      start_(Telemetry::Now()) {}

TelemetryScope::~TelemetryScope() {
    Telemetry::Duration(cat_, ev_, start_, detail_.empty() ? nullptr : detail_.c_str());
}

// ── Capa de contadores ──
//
// Array ESTÁTICO: sin asignaciones, sin crecimiento, tamaño conocido en
// compilación. 20 sondas x 24 bytes = ~480 bytes para toda la vida del
// proceso — irrelevante frente al presupuesto de 50 MB, que es justamente la
// condición que Joel puso: medir todo sin pagar memoria por ello.
namespace {

struct ProbeSlot {
    std::atomic<uint64_t> hits{ 0 };
    std::atomic<uint64_t> totalUs{ 0 };
    std::atomic<uint64_t> maxUs{ 0 };
};
ProbeSlot g_probes[static_cast<size_t>(Probe::_Count)];

// Nombres para el volcado. El orden DEBE coincidir con el enum.
const char* const kProbeNames[] = {
    "hotkey.recv", "hotkey.toggle", "hotkey.dedupe",
    "cue.play", "cue.visual.show", "cue.visual.frame",
    "glass.recapture", "glass.draw", "glass.occluded.skip",
    "audio.ensure", "audio.acquire", "audio.fail", "audio.peek",
    "flyout.show", "flyout.render",
    "render.frame", "levelbar.update",
    "settings.save", "settings.reload",
    "tray.icon.render",
};
static_assert(sizeof(kProbeNames) / sizeof(kProbeNames[0]) ==
                  static_cast<size_t>(Probe::_Count),
              "kProbeNames desincronizado con el enum Probe");

}  // namespace

void Probes::Hit(Probe p) {
    g_probes[static_cast<size_t>(p)].hits.fetch_add(1, std::memory_order_relaxed);
}

void Probes::HitTimed(Probe p, unsigned long long since) {
    auto& slot = g_probes[static_cast<size_t>(p)];
    slot.hits.fetch_add(1, std::memory_order_relaxed);
    if (!g_qpcFreq.QuadPart) return;
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    const uint64_t us = static_cast<uint64_t>(
        (c.QuadPart - static_cast<LONGLONG>(since)) * 1000000LL / g_qpcFreq.QuadPart);
    slot.totalUs.fetch_add(us, std::memory_order_relaxed);
    // Peor caso sin bloqueo: reintentar mientras alguien nos gane la carrera.
    uint64_t prev = slot.maxUs.load(std::memory_order_relaxed);
    while (us > prev &&
           !slot.maxUs.compare_exchange_weak(prev, us, std::memory_order_relaxed)) {}
}

void Probes::Snapshot(const char* reason) {
    if (!g_enabled) return;
    char hdr[128];
    sprintf_s(hdr, "reason=%s", reason ? reason : "?");
    WriteLine("PROBE", "snapshot.begin", hdr);
    for (size_t i = 0; i < static_cast<size_t>(Probe::_Count); ++i) {
        const uint64_t n = g_probes[i].hits.load(std::memory_order_relaxed);
        if (n == 0) continue;   // el silencio también es información: no ocurrió
        const uint64_t tot = g_probes[i].totalUs.load(std::memory_order_relaxed);
        const uint64_t mx = g_probes[i].maxUs.load(std::memory_order_relaxed);
        char buf[256];
        if (tot || mx)
            sprintf_s(buf, "%-20s n=%llu total_ms=%.3f avg_ms=%.4f max_ms=%.3f",
                      kProbeNames[i], static_cast<unsigned long long>(n),
                      tot / 1000.0, (tot / 1000.0) / static_cast<double>(n),
                      mx / 1000.0);
        else
            sprintf_s(buf, "%-20s n=%llu", kProbeNames[i],
                      static_cast<unsigned long long>(n));
        WriteLine("PROBE", "count", buf);
    }
    WriteLine("PROBE", "snapshot.end", nullptr);
}

void Probes::Reset() {
    for (auto& s : g_probes) {
        s.hits.store(0, std::memory_order_relaxed);
        s.totalUs.store(0, std::memory_order_relaxed);
        s.maxUs.store(0, std::memory_order_relaxed);
    }
}

}  // namespace mutemic
