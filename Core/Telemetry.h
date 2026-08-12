#pragma once
#include <windows.h>
#include <string>

namespace mutemic {

// ── Telemetría local: los ojos de Claude sobre la app ──
//
// PORQUÉ EXISTE: Claude no puede compilar, ejecutar ni ver la app. Su única
// vía de observación son los archivos que quedan en la carpeta del
// proyecto. Todo lo que no se escriba aquí, para él no ocurrió — y se
// diagnostica a ciegas (que ya costó semanas con el pipeline del cristal).
//
// QUÉ NO ES: no sale de la máquina, no hay red, no hay identificadores de
// usuario. Es un archivo de texto junto al exe.
//
// Formato: una línea por evento, legible y greppable
//   [   1234ms] GLASS  capture.start   monitor=1 w=2560 h=1440
//
// Activación: SIEMPRE en Debug. En Release solo con `--diag` (así un
// usuario final no paga ni un fopen).
class Telemetry {
public:
    // Abre el archivo y vuelca el entorno completo (OS, DPI, monitores,
    // GPU, versiones). `force` lo enciende en Release (flag --diag).
    static void Init(bool force = false);

    // Cierra la sesión con un resumen y el motivo de salida.
    static void Shutdown(const char* reason);

    static bool Enabled();

    // Evento puntual. detail admite "key=value key=value".
    static void Event(const char* category, const char* event,
                      const char* detail = nullptr);

    // Igual, pero con un HRESULT que se formatea y se marca como fallo.
    static void Fail(const char* category, const char* event, HRESULT hr,
                     const char* detail = nullptr);

    // Métrica numérica (se puede graficar después).
    static void Metric(const char* category, const char* name, double value,
                       const char* unit = nullptr);

    // Muestreo periódico de recursos del proceso (RAM, handles, GDI, GPU).
    // Llamar desde un timer de baja frecuencia; él mismo se auto-limita.
    static void SampleResources();

    // Marca de tiempo para medir duraciones: devuelve un token opaco.
    static ULONGLONG Now();
    static void Duration(const char* category, const char* event,
                         ULONGLONG since, const char* detail = nullptr);

    // Mide sin escribir. Necesario para los llamadores POR FRAME: registrar
    // incondicionalmente a 165 Hz con fflush por línea sería una tormenta de
    // E/S en el hilo de UI — el mismo patrón que ya nos costó el escritorio.
    // El llamador decide el umbral y solo entonces llama a Event/Duration.
    static double ElapsedMsSince(ULONGLONG since);
};

// RAII para medir un bloque:  { TelemetryScope s("GLASS","recapture"); ... }
class TelemetryScope {
public:
    TelemetryScope(const char* category, const char* event,
                   const char* detail = nullptr);
    ~TelemetryScope();
private:
    const char* cat_;
    const char* ev_;
    std::string detail_;
    ULONGLONG start_;
};

}  // namespace mutemic
