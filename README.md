<p align="center">
  <img src="app-icon-preview.png" width="96" alt="MuteMic icon">
</p>

<h1 align="center">MuteMic</h1>

<p align="center">
  A native Windows mic-mute utility that stays out of your way — global shortcuts from <em>any</em> device,
  a volume-reactive neon tray icon, game-safe visual cues, and a real Liquid Glass UI.
  <br>WinUI 3 · C++/WinRT · zero Electron, zero background bloat.
</p>

<p align="center">
  <img alt="Platform" src="https://img.shields.io/badge/platform-Windows%2011-0078D4?logo=windows11&logoColor=white">
  <img alt="Stack" src="https://img.shields.io/badge/WinUI%203-C%2B%2B%2FWinRT-9B59B6">
  <img alt="License" src="https://img.shields.io/badge/license-MIT-39FF14">
  <img alt="Footprint" src="https://img.shields.io/badge/servers%20%2F%20open%20ports-none-FF3131">
</p>

---

## Download

Grab the latest zip from [Releases](https://github.com/WolfyIt/MuteMic/releases) — unzip anywhere and run `MuteMic.exe`.
One-time prerequisite: `winget install Microsoft.WindowsAppRuntime.1.6`.
Prefer building from source? See [Build](#build).

---

## Features

**Mute from anything.** Shortcut *cards* let you bind as many inputs as you want, each with its own name and mode: keyboard keys or combos (including macro keys like NZXT / Razer keys mapped to F13–F24), mouse buttons (M3/M4/M5), and XInput controller buttons. Cards auto-detect the device type on capture, show a presence dot for controllers, and can be added, renamed, tested, or removed on the fly.

**Three trigger modes per card** — toggle, push-to-talk, and push-to-mute (with debouncing so macro keys that emit instant down+up pairs still hold correctly). The keyboard toggle path also registers a real `RegisterHotKey`, so it keeps working over elevated apps like Task Manager.

**Volume-reactive tray icon.** Five neon states rendered at runtime with GDI+ (gray disabled, green idle, green speaking with level-reactive glow, red muted, red talking-while-muted), supersampled and DPI-aware. Rendering only happens when the level crosses a visual bucket — idle means zero redraws.

**Win11-style tray flyout.** A native-looking acrylic flyout (mute/unmute, device switcher, open, quit) built to match the system network flyout, with correct light/dark theming.

**Visual cues that don't cost you frames.** Optional on-screen feedback when muting/unmuting: full edge ring, corner brackets, or an iPhone-island style notch (any screen edge). Cues are rendered through DirectComposition swapchains on `WS_EX_NOREDIRECTIONBITMAP` windows — no redirection surface, MPO-eligible, animation vsynced to your monitor's real refresh rate. Your game keeps its independent-flip fast path.

**Sound cues.** Drop your own WAVs into `Sounds/mute` and `Sounds/unmute`, pick them in Settings with a volume slider. Playback is flagged as a sound effect, so it never hijacks the media overlay.

**Liquid Glass.** A real refraction shader (HLSL D2D custom effect over live screen capture via Win2D), not just acrylic — with a Frosting toggle and Light/Night themes. Screen-share friendly: the lens freezes to a snapshot and the window excludes itself from capture.

**Automation-ready CLI.** Perfect for Stream Deck / AutoHotkey / scripts — no open ports, no servers:

```
MuteMic.exe --toggle | --mute | --unmute | --show
```

If MuteMic is already running, the verb is forwarded to the live instance in milliseconds.

**Polite by design.** Restores your mic to its pre-launch state on exit (including system shutdown), starts minimized to tray if you want, second launch just opens the existing window, and it answers shutdown queries immediately so it never slows your PC down.

## Screenshots

### Main window — Night & Light

| Night | Light |
|:---:|:---:|
| <img src="docs/main-night.png" width="380"> | <img src="docs/main-light.png" width="380"> |

### Liquid Glass

Real refraction over your desktop — clear, or with Frosting.

| Clear glass | Frosted glass |
|:---:|:---:|
| <img src="docs/main-glass-night.png" width="380"> | <img src="docs/main-frosted-night.png" width="380"> |

<details>
<summary>Light theme variants</summary>

| Clear glass | Frosted glass |
|:---:|:---:|
| <img src="docs/main-glass-light.png" width="380"> | <img src="docs/main-frosted-light.png" width="380"> |

</details>

### Shortcut cards

One card per input — keyboard, mouse, controller — each with its own binding, mode, and name.

| Keyboard + controller | Keyboard + controller + mouse |
|:---:|:---:|
| <img src="docs/shortcut-cards-2.png" width="380"> | <img src="docs/shortcut-cards-3.png" width="380"> |

### Settings

| Night | Light |
|:---:|:---:|
| <img src="docs/settings-night.png" width="380"> | <img src="docs/settings-light.png" width="380"> |

<details>
<summary>Liquid Glass variants</summary>

| Clear · Night | Clear · Light |
|:---:|:---:|
| <img src="docs/settings-glass-night.png" width="380"> | <img src="docs/settings-glass-light.png" width="380"> |

| Frosted · Night | Frosted · Light |
|:---:|:---:|
| <img src="docs/settings-frosted-night.png" width="380"> | <img src="docs/settings-frosted-light.png" width="380"> |

</details>

### Tray flyout

| Night | Light | Glass · Night | Glass · Light |
|:---:|:---:|:---:|:---:|
| <img src="docs/flyout-night.png" width="200"> | <img src="docs/flyout-light.png" width="200"> | <img src="docs/flyout-glass-night.png" width="200"> | <img src="docs/flyout-glass-light.png" width="200"> |

### Visual cues in action

Edge ring, corner brackets, and the notch — vsynced to your monitor, game-safe.

| Edges | Corners |
|:---:|:---:|
| <img src="docs/cue-edges.gif" width="380"> | <img src="docs/cue-corners.gif" width="380"> |

<p align="center"><img src="docs/cue-notch.gif" width="500"></p>

## Build

- Visual Studio 2022 (v143) + Windows SDK 10.0.26100
- NuGet restores Windows App SDK 1.6, C++/WinRT, WIL and Win2D automatically
- Runtime to execute: `winget install Microsoft.WindowsAppRuntime.1.6`

```
msbuild MuteMic.sln /p:Configuration=Release /p:Platform=x64 /restore
```

> Note: this is an *unpackaged* WinUI 3 app (no MSIX). The XAML compiler does not
> emit `*.xaml.g.h` in this mode, so the ones in `Generated Files/` at the repo
> root are maintained by hand — see `.github/copilot-instructions.md` for the
> full build architecture and known-failure recipes.

Windows 11 is the target; Windows 10 2004+ works with graceful degradation (no rounded corners / acrylic).

## Architecture notes

- `Core/` is pure Win32/COM: Core Audio (`IAudioEndpointVolume`, `IAudioMeterInformation`), low-level keyboard/mouse hooks, XInput polling, tray, GDI+ icon rendering.
- `Core/LiquidGlass.hlsl` + `LiquidGlassBackdrop` implement the refraction backdrop (Windows.Graphics.Capture → Win2D `PixelShaderEffect` → `CanvasControl`).
- `Core/VisualCue.cpp` implements the anti-FPS-drop overlay architecture (DirectComposition presentation, per-region windows, compositor-clock pacing).
- `legacy-win32/` contains the original pure-Win32 prototype this project grew out of.

## License

[MIT](LICENSE) © Joel Santoro (WolfyIt)
