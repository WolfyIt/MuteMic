# Copilot instructions — MuteMic (WinUI 3 C++/WinRT, unpackaged)

## Your role

You are the **build debugger**. Claude (Cowork) is the **coder/architect**: designs features, writes new code, and decides architecture. Your job is to get `MuteMic.sln` to compile and run:

- Fix **mechanical** compile/link errors: missing includes, typos, mismatched signatures, vcxproj paths, desynchronized generated files.
- Do NOT redesign components, do NOT change architecture, do NOT rewrite features. If a fix requires a design decision, leave the error documented with a diagnosis and do not half-fix it.
- Minimal changes: prefer the one-line fix over the refactor.

## Working protocol (read FIRST)

- **Before diagnosing any symptom, read the logs.** The app writes
  `mutemic-glass.log` (Debug only, glass pipeline) and `mutemic-crash.log`
  (always: module + offset + stack of an AV) next to the exe. They are in the
  project folder: they get READ, not requested. A `tail` costs less than a
  build cycle. This rule exists because the glass pipeline was diagnosed blind
  for days while the log already said
  `FramePool create FAILED hr=0x80070057`.
- **Project context lives in `vault/`**: `INDEX.md` (map and owners),
  `STATE.md` (where we are), `DECISIONS.md` (why it is this way),
  `ATTEMPTS.md` (**what was already tried and failed — check before
  proposing**), `LESSONS.md` (errors paid for and their recipe). Read before
  architectural work; update afterwards.
- **When you finish, write the lesson down**: if something cost a cycle, it
  goes in `LESSONS.md`; if an approach was discarded, it goes in
  `ATTEMPTS.md`. Write it when it is paid for, not "later".

## Build architecture (read before touching anything)

Mirror of `NUCSoftwareStudio` (the sibling project in `Desktop\NUCSoftwareStudioC++`, which compiles and works). When in doubt, compare against that project.

- **Unpackaged WinUI 3 desktop**: `WindowsPackageType=None`, no `ApplicationType`, no `CppWinRTEnabled` (UWP props are forbidden). NuGet only in `Directory.Build.props`, never in the vcxproj.
- **Manual entry point**: `DISABLE_XAML_GENERATED_MAIN=1`; `WinMain` lives in `App.xaml.cpp` (bootstrap `MddBootstrapInitialize` + manual WinRT exports). The `wWinMain` from `App.xaml.g.hpp` is excluded by that define.
- **DPI**: `SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)` is the FIRST line of WinMain, plus `app.manifest` PerMonitorV2. Do not move or remove it.

### The generated-files system (the fragile part)

In this setup the XAML compiler **does not generate** the `*.xaml.g.h` family. That is why they are **checked into source control** in `Generated Files\` (project root), a pattern copied from NUCS:

| File (in `Generated Files\`) | What it is | Maintenance |
|---|---|---|
| `App.xaml.g.h` | `AppT` template (IXamlMetadataProvider) | stable, do not touch |
| `MainWindow.xaml.g.h` | `MainWindowT` template: accessor + field per `x:Name` | **update by hand if MainWindow.xaml changes** |
| `XamlTypeInfo.xaml.g.h`, `XamlBindingInfo.xaml.g.h`, `XamlMetaDataProvider.h/.cpp`, `XamlTypeInfo.Impl.g.cpp` | generic metadata-provider infrastructure | stable, do not touch |

What IS generated on every build (into `$(IntDir)` = `MuteMic\x64\<Config>\`):

- `App.xaml.g.hpp`, `MainWindow.xaml.g.hpp` (Connect/InitializeComponent bodies) — compiled via `XamlGeneratedBodies.cpp`
- `XamlTypeInfo.g.cpp` (type tables) — referenced as `$(IntDir)XamlTypeInfo.g.cpp` in the vcxproj
- `$(IntDir)Generated Files\` — CppWinRT output (`App.g.h`, `MainWindow.g.h`, `winrt/MuteMic.h`, `XamlMetaDataProvider.g.h`)

Include dirs (order matters): `$(ProjectDir)` → `$(IntDir)` → `$(IntDir)Generated Files` → `$(ProjectDir)Generated Files`.

### Recipes for known failures

- **C1083 `*.xaml.g.h` not found** → the checked-in file in `Generated Files\` is missing, or the vcxproj include path broke. Do not try to make the XAML compiler generate it: in this setup it does not.
- **LNK2001 `InitializeComponent`/`Connect`/`GetBindingConnector`** → the `.g.hpp` in `$(IntDir)` was not compiled. `XamlGeneratedBodies.cpp` must include them, and the `_XamlPass2BeforeClCompile` target in `Directory.Build.targets` must be intact.
- **An `x:Name` or handler was added/renamed in `MainWindow.xaml`** → hand-sync `Generated Files\MainWindow.xaml.g.h`: one accessor pair (get/set) plus a `_Name{nullptr}` field of the correct type. The connection IDs in the `.g.hpp` regenerate themselves.
- **Duplicate-type XamlTypeInfo error** → someone probably added both `$(IntDir)XamlTypeInfo.g.cpp` AND a copy at the root. Only the `$(IntDir)` one should compile.
- **`midl : command line error MIDL1003: error returned by the C preprocessor (2)`**
  → NOT a code error: the NuGet restore is missing. Happens every time after
  deleting `MuteMic\` (the `$(IntDir)`) or on a clean clone, because the
  CppWinRT/WindowsAppSDK targets that configure MIDL live in the package
  cache. Fix: `msbuild MuteMic.sln /t:Restore /p:Platform=x64` and rebuild.
  Rule of thumb: **after any deep clean, always `/t:Restore` before the
  build.**
- **`Windows App Runtime not found` at launch** → `winget install Microsoft.WindowsAppRuntime.1.6` (it is framework-dependent).
- **Mojibake in strings (Ã³, â€")** → `/utf-8` is missing from the compiler options. It is in `AdditionalOptions`; do not remove it.

## Liquid Glass (Core\LiquidGlassBackdrop.* + Core\LiquidGlass.hlsl)

Real refraction backdrop: Windows.Graphics.Capture of the monitor (with our
window excluded via `WDA_EXCLUDEFROMCAPTURE`) → Win2D `PixelShaderEffect`
(shader ported from the OverShifted/LiquidGlass repo) → **Win2D `CanvasControl`
inserted as the first child of RootGrid** (bottom layer). Do NOT use
SystemBackdrop or CanvasComposition: Win2D's composition is
Windows.UI.Composition and the WinUI 3 backdrop is Microsoft.UI.Composition —
incompatible trees (already tried; C2665). The capture FramePool must be
created with `canvas.Device()` (the same device that wraps the frames).

Likely failures and their fixes:

- **fxc / FxCompile fails** (cannot find `d2d1effecthelpers.hlsli`): add
  `<AdditionalIncludeDirectories>$(WindowsSdkDir)Include\$(WindowsTargetPlatformVersion)\um</AdditionalIncludeDirectories>`
  to the FxCompile item. If it complains about the entry point/target: the blob
  must be `ps_4_0` with `/D D2D_FULL_SHADER /D D2D_ENTRY=main /E main`.
- **PixelShaderEffect throws on creation**: the .cso is not a valid D2D custom
  effect (check the defines above), or `LiquidGlass.cso` is missing next to the
  exe (FxCompile emits it to `$(OutDir)`).
- **Microsoft.Graphics.Canvas.dll missing at runtime**: the vcxproj
  `CopyWin2DRuntime` target copies it from the NuGet cache; if the Win2D
  version changes, update the path there AND in Directory.Build.props (winmd).
- **Effect properties do not match**: the names in `props.Insert(L"...")` must
  match the .hlsl cbuffer EXACTLY; a float2 is passed as
  `PropertyValue::CreateSingleArray` of 2 floats.
- **v2 architecture: ONE shared capture + N LENSES.** `Shared` holds the
  snapshot, the capture session and the hooks; each `Lens` (main window, tray
  flyout, future overlays) has its own hwnd, host, CanvasControl and its OWN
  effect graph (the shader receives ITS window's size). Adding a glass window
  = `AttachLens(hwnd, grid)`, never copy the pipeline.
- **NEVER recapture when a window moves.** The snapshot is taken with ALL our
  windows excluded (`WDA_EXCLUDEFROMCAPTURE`), so it stays valid wherever you
  move them: moving only changes which region is sampled. Recapturing on every
  move (v1's 150 ms loop) was the cause of the **drag jitter**.
- **The snapshot refreshes on EVENTS**: `SetWinEventHook` on
  `EVENT_SYSTEM_FOREGROUND` and `EVENT_OBJECT_LOCATIONCHANGE`, filtering out
  our own process and our lenses, with a 220 ms debounce
  (`ScheduleRecapture`). That fixes the "frozen frame" at zero idle cost.
- A lens host can change (the flyout is rebuilt entirely on theme change):
  `AttachLens` detects the new host and moves the canvas.
- **A COVERED window must cost nothing.** `WindowIsCovered()` walks the Z-order
  upward looking for a foreign window that fully contains it (skipping cloaked,
  `WS_EX_TRANSPARENT` and our own). That cuts capture, shader and repaints.
  `IsWindowOccluded()` is the public version with a 180 ms cache for PER-FRAME
  callers (the level bar). Being "visible" (`IsWindowVisible`) does NOT mean it
  can be seen.
- Background sharpness depends on three things together — break none of them:
  offset rounded to a whole pixel, `InterpolationMode` NearestNeighbor on
  `fxShift`, and the snapshot bitmap created with the canvas DPI (with a fixed
  96, D2D inserts a `DpiCompensationEffect` that rescales everything with
  bilinear filtering).
- The lens samples with **motion prediction** (velocity × latency, smoothed and
  clamped): without it the glass lags on fast movement, because DWM moves the
  frame and our frame arrives 1–2 behind.
- **GOLDEN RULE for compositors**: `CompositionTarget::GetCompositorForCurrentThread()`
  returns a `Microsoft::UI::Composition::Compositor`. NEVER cast it to
  `Windows::UI::Composition::Compositor` (the QI fails and the catch degrades to
  acrylic — the classic "it still has blur"), and NEVER construct a bare
  `Compositor()` (it belongs to another visual tree → black).
- The app is excluded from the user's screenshots while glass is active (side
  effect of WDA_EXCLUDEFROMCAPTURE). This is by design; it is restored in
  `Stop()`.

## Visual cues (Core/VisualCue.cpp) — v2 architecture, do NOT go back to v1

- Cues are presented via **DirectComposition**: `WS_EX_NOREDIRECTIONBITMAP`
  windows + premultiplied flip-model swapchain
  (`CreateSwapChainForComposition`) per region, with a render thread paced by
  `DwmFlush()` (= vsync at the monitor's real refresh rate).
- **NEVER go back to `UpdateLayeredWindow` as the main path**: the redirection
  surface drops the game out of independent flip (165→112 fps, measured). ULW
  exists only as a fallback if `InitGfx()` fails.
- Artwork is drawn with GDI+ into a persistent DIB and uploaded via
  `CopyFromMemory` to an `ID2D1Bitmap1` → backbuffer. The DIB bits are treated
  as premultiplied BGRA (same as ULW required).
- The "edges" are ONE global rounded ring split into 4 non-overlapping strips
  (arcs on top/bottom, sides only the straight part): if the region rects are
  touched, that property must be preserved or seams appear.
- Libs: `d2d1.lib` and `dcomp.lib` come from `#pragma comment(lib, ...)` in the
  .cpp itself (not in the vcxproj).
- The thread is ALWAYS stopped with `StopThread()` (flag + join) before touching
  regions; `Term()` may be called from the wndproc itself via `kMsgAnimDone`.

## Invariants (do not change without strong justification)

1. The WinMD `HintPath` entries in `Directory.Build.props` point at this machine's real NuGet cache. If WindowsAppSDK is updated, update ALL the paths at once.
2. `pch.h` ends by including `App.xaml.h` and `MainWindow.xaml.h` (required by `XamlTypeInfo.g.cpp` via static_assert). Do not reorder.
3. `Core/` is pure Win32/COM (Core Audio, LL hook, GDI+, tray). Do not introduce WinRT dependencies there beyond the ones already present in `MuteMicCore.cpp`.
4. **EVERY low-level hook (keyboard AND mouse) only calls `PostMessage`.**
   It runs inside the system input queue: window operations, file I/O, or
   unhooking itself from inside the callback freeze the mouse and keyboard of
   the ENTIRE MACHINE. This already happened once (frozen screen + forced
   shutdown). Applies to `HotkeyHook.cpp` and `NativeFlyout.cpp`.
5. UI strings in English (the shipped product is English). Comments explain the
   *why* and the cost avoided, not the what.

## When you finish

Report in a comment/commit: what failed, root cause, fix applied (file:line) — so Claude picks the context back up without re-diagnosing.

### Warning reporting — full text, always

On EVERY build, include the **FULL TEXT** of every warning. A count is
useless: `12 Warning(s)` does not say what broke.

**Real failure mode (11 Aug 2026):** the output was filtered with
`Select-String "…|Warning\(s\)|Error\(s\)"`. That pattern catches the
*summary* line (`0 Warning(s)`) but **discards every individual warning**,
because msbuild emits them as `path(line,col): warning C4100: …`, which does
not contain the string `Warning(s)`. The build was reported clean without a
single warning having been looked at — right after 481 lines were deleted,
which is exactly when orphaned includes are most likely.

**Filter on `": warning "` and `": error "`, with the colon and the spaces.**
That is what distinguishes a diagnostic line from the summary line.

Recipes that actually work:

```powershell
# Option A: save the full log and extract the diagnostics
msbuild MuteMic.sln /p:Configuration=Debug /p:Platform=x64 /m `
  /fl /flp:logfile=build.log`;verbosity=normal
Select-String -Path build.log -Pattern ': (warning|error) ' |
  ForEach-Object { $_.Line.Trim() } | Sort-Object -Unique
```

```powershell
# Option B: inline, no intermediate file
msbuild MuteMic.sln /p:Configuration=Debug /p:Platform=x64 /m 2>&1 |
  Select-String -Pattern ': (warning|error) ', 'Build (succeeded|FAILED)'
```

If the result is zero `: warning ` lines, say so explicitly ("zero warnings,
verified with the correct filter"). **Never "0 errors" on its own.**

### How to test a build (this matters more than it sounds)

**`MuteMic.exe` with no flag starts the daemon only.** The daemon has the
tray, the hotkeys and the audio polling timer — but no window, no XAML, no
render hook. Most bugs live in the settings window and a bare-exe run does not
touch them. A 60-second daemon run proves almost nothing.

To exercise the settings window: `MuteMic.exe --settings`, or launch the
daemon and open the window from the tray flyout.

Do not `Start-Process` + `Sleep 60` + `Stop-Process` and call it validated.
That path never opens a window and never reproduces a UI bug. Run it, use it
the way the report describes, and watch for the described symptoms.

**Symptoms that will not show up in Task Manager.** If the report says the
desktop got sluggish while MuteMic's own CPU, RAM and GPU stayed flat, that
combination means the process is *blocked waiting* — usually a synchronous
COM/RPC round-trip to a system service — not computing. Do not dismiss it
because the counters look fine; flat counters are the evidence, not the
absence of it.

### Reading a crash with the .map file

`GenerateMapFile` is enabled in `MuteMic.vcxproj`, so `x64\<Config>\MuteMic.map`
can symbolize `mutemic-crash.log` without a debugger session. Two cautions:

- A `.map` resolves an address to the **nearest preceding symbol**, not the
  real function. Header-inlined WinRT projections are scattered throughout the
  image, so a mid-image offset often lands on an unrelated neighbour.
- Frames come from `RtlCaptureStackBackTrace` called **inside** the crash
  handler, so the first several frames are exception-dispatch machinery
  (`KERNELBASE!RaiseException`, ntdll unwinders), not the fault path.

Trust adjacent frames that form a coherent caller/callee pair. Treat isolated
distant frames as noise unless a debugger confirms them. Report what is
plausible and label what is uncertain — a confident wrong stack costs more
than an honest "frames 12-15 are unreliable".

### Restore points

Before a risky change lands, commit the working tree locally so there is
something to return to. Claude cannot commit (its sandbox can create files on
the mounted folder but not delete them, so git locking fails), so committing
is your job. Local commits are enough; pushing is Joel's call.

### Deletions

Deleting files is your job, by agreement — Claude cannot delete, and routing
deletions through you keeps them reviewable and reversible. Deletion requests
arrive in `vault/BUILD-TASK.md` under **Pre-steps**, each with the reason it is
safe. Review the reason before running it.

### Git locks

If `.git/index.lock` blocks a git operation, check whether a git process is
actually running before removing it. Claude's sandbox can create files on the
mounted folder but cannot delete them, so a plain `git status` from that side
leaves a lock behind. Claude now runs git with `GIT_OPTIONAL_LOCKS=0` to avoid
creating it at all, but pre-existing locks still need you to remove them.
