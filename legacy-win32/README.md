# MuteMic

Toggle de micrófono por hotkey global para Windows. Vive en el system tray, mutea/desmutea el micrófono por defecto con **Ctrl+Alt+M**, y consume 0% CPU en reposo.

C++ puro / Win32 API. Sin frameworks, sin dependencias externas, un solo `.exe` de ~100 KB.

## Features

- **Hotkey global** `Ctrl+Alt+M` — funciona sin importar qué app tenga el foco (`RegisterHotKey`, sin polling).
- **Tray icon con estado**: verde = mic activo, rojo = muteado, gris = sin dispositivo. Íconos generados en runtime con GDI — no hay archivos `.ico`.
- **Click izquierdo** en el ícono también togglea. Tooltip con el estado actual.
- **Feedback sonoro**: tonos distintos para mute y unmute (`MessageBeep`).
- **Iniciar con Windows**: checkbox en el menú contextual (escribe en `HKCU\...\Run`).
- **Manejo de errores**: si el hotkey ya está tomado por otra app, avisa con una notificación en vez de fallar en silencio; si no hay micrófono, muestra el ícono gris en vez de crashear.
- **Instancia única** y re-registro del tray icon si explorer.exe se reinicia.

## Cómo funciona

- **Mute**: Core Audio API (`IMMDeviceEnumerator` → `GetDefaultAudioEndpoint(eCapture)` → `IAudioEndpointVolume::SetMute`). El endpoint se re-adquiere en cada toggle, así que conectar/desconectar micrófonos no rompe nada.
- **Cero CPU en reposo**: el message loop bloquea en `GetMessage` hasta que llega el hotkey o un evento del tray.

## Build

Con MSVC (desde *x64 Native Tools Command Prompt*):

```
build.bat
```

O con CMake:

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

## Estructura

```
main.cpp       — WinMain, message loop, hotkey handler
tray.cpp/.h    — Shell_NotifyIcon, menú contextual, balloons
audio.cpp/.h   — wrapper de Core Audio (mute toggle)
icons.cpp/.h   — generación de íconos en runtime vía GDI
autostart.cpp/.h — entrada en la Run key de HKCU
```
