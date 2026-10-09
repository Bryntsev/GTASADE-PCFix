# Changelog

## v0.2.0 - 2026-10-09

First release with a complete installation package.

- Renamed from "GTASADE Raw Mouse Fix" (`SADE.HighFpsRawMouseFix.*`) to **GTASADE PC Fix**
  (`GTASADE.PCFix.*`). Delete the v0.1.0 files when updating; the new version stays inactive while
  the old one is loaded.
- Fusion Fix is now a required part of the installation: it fixes the camera behaviour (vertical
  axis) and provides the ASI loader.
- Files: `Win64\GTASADE.PCFix.addon64`, and `GTASADE.PCFix.asi`, `.RawInputHelper.exe`, `.ini` in
  `Win64\scripts`. A core `.asi` directly in `Win64` crashed the game on start together with the ASI
  loader (`version.dll`), ReShade and the Steam overlay.
- Mouse input is read inside the game process at the mouse's full polling rate (1000 Hz tested).
  Windows 11 coalesces raw input for background processes to ~125 Hz, so the helper process is now
  only a fallback. A hidden in-process window holds the raw input registration while the game does
  not, and the game's own `WM_INPUT` messages are read while it does.
- Sub-pixel accumulation: non-integer sensitivity no longer drops movement.
- Hooks are installed only into `SanAndreas.exe`. Earlier versions re-patched every loaded module
  each second, which could corrupt memory together with ReShade add-ons that are loaded and
  unloaded at runtime (crash seen with RenoDX).
- No diagnostics on game threads by default. The previous version wrote CSV traces on every cursor
  call and raw input event during normal play, a likely source of micro-stutter.
- Faster hand-over from the gameplay camera to the menu (adaptive to the frame rate instead of 2 s).
- FPS cap: the game resets its frame-rate cap to 60 on every launch; the mod keeps the chosen cap
  (default 120). The old `ForceUnlimitedFps` option wrote invalid values and is off.
- ReShade add-on (`.addon64`) with a settings panel: on/off, sensitivity (slider and exact value),
  FPS cap, status, mouse device and polling rate, developer mode.
- On-screen info overlay (off by default) and hotkeys: `Ctrl+PgUp/PgDn` FPS cap, `Ctrl+End` mouse
  fix on/off, `Ctrl+Insert` overlay.
- Developer mode (off by default) records mouse, camera, frame and input traces per run.
- The log is written line by line (useful after a crash) and says `RAW MOUSE FIX ACTIVE` when the
  camera hooks are in place. One log file is kept instead of one per launch.
- The mod stays inactive on an unknown `SanAndreas.exe` build.
- Defaults are built in, so the mod works even without its ini file.

## v0.1.0-test - 2026-07-05

Initial public test release candidate.

- Adds the `v27-rawinput-companion` mouse fix path.
- Uses a companion process to collect physical Raw Input mouse deltas.
- Applies input to the gameplay camera path from the ASI plugin.
- Tested by the author with a 1000 Hz mouse polling rate.
- Known issue: menu item highlighting can behave oddly, but menu actions remain usable in testing.
- Known limitation: the in-game FPS cap is not reliably changed by the mod.
- VirusTotal at release time:
  - Release ZIP: `1/66`, MaxSecure `Trojan.Malware.300983.susgen`.
  - ASI plugin: `1/70`, Microsoft `PUA:Win32/Puwaders.C!ml`.
  - Raw Input companion EXE: `1/70`, MaxSecure `Trojan.Malware.300983.susgen`.
