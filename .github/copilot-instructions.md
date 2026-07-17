# Copilot instructions — MuteMic (WinUI 3 C++/WinRT, unpackaged)

## Tu rol

Eres el **build debugger**. Claude (Cowork) es el **coder/arquitecto**: diseña features, escribe el código nuevo y decide la arquitectura. Tu trabajo es conseguir que `MuteMic.sln` compile y corra:

- Arregla errores de compilación/link **mecánicos**: includes faltantes, typos, firmas que no coinciden, rutas del vcxproj, archivos generados desincronizados.
- NO rediseñes componentes, NO cambies la arquitectura, NO reescribas features. Si un fix requiere decisión de diseño, deja el error documentado con diagnóstico y no lo "arregles" a medias.
- Cambios mínimos: prefiere el fix de 1 línea sobre la refactorización.

## Arquitectura del build (leer antes de tocar nada)

Proyecto espejo de `NUCSoftwareStudio` (el proyecto hermano en `Desktop\NUCSoftwareStudioC++`, que compila y funciona). Ante la duda, compara con ese proyecto.

- **Unpackaged WinUI 3 desktop**: `WindowsPackageType=None`, sin `ApplicationType`, sin `CppWinRTEnabled` (props UWP prohibidas). NuGet solo en `Directory.Build.props`, nunca en el vcxproj.
- **Entry point manual**: `DISABLE_XAML_GENERATED_MAIN=1`; `WinMain` está en `App.xaml.cpp` (bootstrap `MddBootstrapInitialize` + exports WinRT manuales). El `wWinMain` de `App.xaml.g.hpp` queda excluido por ese define.
- **DPI**: `SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)` es la PRIMERA línea de WinMain + `app.manifest` PerMonitorV2. No mover ni quitar.

### El sistema de archivos generados (la parte frágil)

El XAML compiler en este setup **no genera** la familia `*.xaml.g.h`. Por eso están **versionados** en `Generated Files\` (raíz del proyecto), patrón copiado de NUCS:

| Archivo (raíz `Generated Files\`) | Qué es | Mantenimiento |
|---|---|---|
| `App.xaml.g.h` | template `AppT` (IXamlMetadataProvider) | estable, no tocar |
| `MainWindow.xaml.g.h` | template `MainWindowT`: accessor + campo por cada `x:Name` | **actualizar a mano si cambia MainWindow.xaml** |
| `XamlTypeInfo.xaml.g.h`, `XamlBindingInfo.xaml.g.h`, `XamlMetaDataProvider.h/.cpp`, `XamlTypeInfo.Impl.g.cpp` | infraestructura genérica del metadata provider | estable, no tocar |

Lo que SÍ se genera en cada build (en `$(IntDir)` = `MuteMic\x64\<Config>\`):

- `App.xaml.g.hpp`, `MainWindow.xaml.g.hpp` (cuerpos Connect/InitializeComponent) — compilados vía `XamlGeneratedBodies.cpp`
- `XamlTypeInfo.g.cpp` (tablas de tipos) — referenciado como `$(IntDir)XamlTypeInfo.g.cpp` en el vcxproj
- `$(IntDir)Generated Files\` — salida CppWinRT (`App.g.h`, `MainWindow.g.h`, `winrt/MuteMic.h`, `XamlMetaDataProvider.g.h`)

Include dirs (orden importa): `$(ProjectDir)` → `$(IntDir)` → `$(IntDir)Generated Files` → `$(ProjectDir)Generated Files`.

### Recetas para fallos conocidos

- **C1083 `*.xaml.g.h` no encontrado** → falta el archivo versionado en `Generated Files\` o se rompió el include path del vcxproj. No intentes que el XAML compiler lo genere: en este setup no lo hace.
- **LNK2001 `InitializeComponent`/`Connect`/`GetBindingConnector`** → el `.g.hpp` de `$(IntDir)` no se compiló. `XamlGeneratedBodies.cpp` debe incluirlos y el target `_XamlPass2BeforeClCompile` de `Directory.Build.targets` debe estar intacto.
- **Se añadió/renombró un `x:Name` o handler en `MainWindow.xaml`** → sincronizar a mano `Generated Files\MainWindow.xaml.g.h`: un accessor par (get/set) + campo `_Nombre{nullptr}` del tipo correcto. Los connection IDs del `.g.hpp` se regeneran solos.
- **Error de tipos duplicados XamlTypeInfo** → probablemente alguien añadió `$(IntDir)XamlTypeInfo.g.cpp` Y una copia en raíz. Solo debe compilarse el de `$(IntDir)`.
- **`Windows App Runtime no encontrado` al ejecutar** → `winget install Microsoft.WindowsAppRuntime.1.6` (es framework-dependent).
- **Mojibake en strings (Ã³, â€")** → falta `/utf-8` en las opciones del compilador. Está en `AdditionalOptions`; no quitarlo.

## Liquid Glass (Core\LiquidGlassBackdrop.* + Core\LiquidGlass.hlsl)

Backdrop de refracción real: Windows.Graphics.Capture del monitor (con la
ventana excluida vía `WDA_EXCLUDEFROMCAPTURE`) → Win2D `PixelShaderEffect`
(shader portado del repo OverShifted/LiquidGlass) → **`CanvasControl` de
Win2D insertado como primer hijo del RootGrid** (capa inferior). NO usar
SystemBackdrop ni CanvasComposition: la composición de Win2D es
Windows.UI.Composition y el backdrop de WinUI 3 es Microsoft.UI.Composition —
árboles incompatibles (ya se intentó; C2665). El FramePool de captura debe
crearse con `canvas.Device()` (mismo device que envuelve los frames).
Fallos probables y sus fixes:

- **fxc / FxCompile falla** (no encuentra `d2d1effecthelpers.hlsli`): añadir
  `<AdditionalIncludeDirectories>$(WindowsSdkDir)Include\$(WindowsTargetPlatformVersion)\um</AdditionalIncludeDirectories>`
  al item FxCompile. Si se queja del entry point/target: el blob debe ser
  `ps_4_0` con `/D D2D_FULL_SHADER /D D2D_ENTRY=main /E main`.
- **PixelShaderEffect lanza al crear**: el .cso no es un custom effect D2D
  válido (revisar defines de arriba) o falta `LiquidGlass.cso` junto al exe
  (lo emite FxCompile a `$(OutDir)`).
- **Falta Microsoft.Graphics.Canvas.dll en runtime**: el Target
  `CopyWin2DRuntime` del vcxproj lo copia desde el cache NuGet; si cambia la
  versión de Win2D, actualizar ruta ahí Y en Directory.Build.props (winmd).
- **Las propiedades del efecto no matchean**: los nombres en
  `props.Insert(L"...")` deben coincidir EXACTO con el cbuffer del .hlsl;
  float2 se pasa como `PropertyValue::CreateSingleArray` de 2 floats.
- **REGLA DE ORO de compositores**: `CompositionTarget::GetCompositorForCurrentThread()`
  devuelve un `Microsoft::UI::Composition::Compositor`. NUNCA castearlo a
  `Windows::UI::Composition::Compositor` (QI falla y el catch degrada a
  acrylic — el clásico "sigue con blur"), y NUNCA construir `Compositor()`
  suelto (pertenece a otro árbol visual → negro).
- La app queda excluida de screenshots del usuario mientras glass está activo
  (efecto colateral de WDA_EXCLUDEFROMCAPTURE). Es by design; se restaura en
  `Stop()`.

## Visual cues (Core/VisualCue.cpp) — arquitectura v2, NO regresar a v1

- Los cues se presentan por **DirectComposition**: ventanas
  `WS_EX_NOREDIRECTIONBITMAP` + swapchain flip-model premultiplicado
  (`CreateSwapChainForComposition`) por región, con un hilo de render
  cadenciado con `DwmFlush()` (= vsync al refresh real del monitor).
- **NUNCA volver a `UpdateLayeredWindow` como camino principal**: la
  superficie de redirección saca al juego de independent flip (165→112 fps
  medido). ULW existe solo como fallback si `InitGfx()` falla.
- El artwork se dibuja con GDI+ en un DIB persistente y se sube por
  `CopyFromMemory` a un `ID2D1Bitmap1` → backbuffer. Los bits del DIB se
  tratan como BGRA premultiplicado (igual que exigía ULW).
- Los "edges" son UN anillo redondeado global partido en 4 franjas sin
  traslape (arcos en top/bottom, laterales solo la parte recta): si se tocan
  los rects de región hay que conservar esa propiedad o salen seams.
- Libs: `d2d1.lib` y `dcomp.lib` van por `#pragma comment(lib, ...)` en el
  propio .cpp (no en el vcxproj).
- El hilo se detiene SIEMPRE con `StopThread()` (flag + join) antes de tocar
  regiones; `Term()` puede llamarse desde el propio wndproc vía
  `kMsgAnimDone`.

## Invariantes (no cambiar sin justificación fuerte)

1. Los WinMD `HintPath` de `Directory.Build.props` apuntan al cache NuGet real de esta máquina. Si se actualiza WindowsAppSDK, actualizar TODAS las rutas a la vez.
2. `pch.h` termina incluyendo `App.xaml.h` y `MainWindow.xaml.h` (lo exige `XamlTypeInfo.g.cpp` vía static_assert). No reordenar.
3. `Core/` es Win32/COM puro (Core Audio, hook LL, GDI+, tray). No introducir dependencias WinRT ahí salvo las ya presentes en `MuteMicCore.cpp`.
4. El hook de teclado (`HotkeyHook.cpp`) no debe hacer trabajo pesado en el callback — solo `PostMessage`. Cualquier fix debe conservar eso.
5. Español en strings de UI, comentarios explican el *porqué*.

## Al terminar

Reporta en un comentario/commit: qué falló, causa raíz, fix aplicado (archivo:línea) — así Claude retoma el contexto sin re-diagnosticar.

En CADA build, incluye en el resumen el TEXTO COMPLETO de todos los warnings
(no solo el conteo): filtra la salida de msbuild con `": warning "` además de
`": error "`. El usuario quiere verlos siempre.
