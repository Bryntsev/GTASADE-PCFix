# GTASADE PC Fix

Fixes for the PC version of **Grand Theft Auto: San Andreas – The Definitive Edition**: raw mouse
camera and an FPS cap that sticks. More is planned.

*Formerly "GTASADE Raw Mouse Fix" (`SADE.HighFpsRawMouseFix`).*

**[⬇ Download the latest release](https://github.com/Bryntsev/GTASADE-PCFix/releases/latest)**
(the `.zip` file under *Assets*) · [Русская версия](README.ru.md)

## What it does

- **Raw mouse camera.** The remaster turns mouse movement into something close to stick input: the
  camera feels steppy, slow or uneven, especially vertically and at high frame rates or mouse polling
  rates. The mod feeds the camera the mouse's real movement at its full report rate (tested at
  1000 Hz), with no acceleration and no lost movement. Menus keep the normal Windows cursor.
- **FPS cap that sticks.** The game resets its frame-rate setting to 60 on every launch (also
  without mods). The mod keeps the cap you choose: default **120**, the highest cap without the
  remaster's high-FPS animation glitches.
- **Settings panel and on-screen info** (with ReShade): sensitivity, on/off, FPS cap, and a small
  overlay with the mouse source, polling rate and frame times.

It does not use `SendInput`, does not automate anything and does not modify game files on disk.

## Tested with

| | |
|---|---|
| Game | Steam, `SanAndreas.exe` **1.0.113.21181** (Steam build 16391096) |
| OS | Windows 11 |
| Required | [Fusion Fix](https://github.com/ThirteenAG/WidescreenFixesPack/releases/tag/gtasade) (default settings, includes Ultimate ASI Loader 9.7.0) |
| Optional | ReShade 6.8.0 with add-on support (installed with [RHI](https://github.com/RankFTW/RHI)), RenoDX, NVIDIA Smooth Motion |
| Mouse | 1000 Hz polling |

On a game version it does not know, the mod stays inactive (the log says so) instead of risking a
crash. It then needs an update.

## Installation

Close the game first. Both archives below mirror the game folder: extract them into the **game
folder**, the one that contains `Gameface` (Steam: right-click the game → *Manage* → *Browse local
files*), and allow overwriting.

### 1. Fusion Fix (required)

Download **`GTASADE.FusionFix.zip`** from
[Fusion Fix for GTA SA DE](https://github.com/ThirteenAG/WidescreenFixesPack/releases/tag/gtasade)
([project page](https://thirteenag.github.io/wfp#gtasade)) and extract it into the game folder.

Why it is required: this mod fixes how mouse movement reaches the camera. How the camera then
behaves (the vertical axis above all, auto-centering behind the player, the on-foot camera) is the
game's camera logic, and Fusion Fix corrects it (`ImproveCameraPC = 1` in
`scripts\GTASADE.FusionFix.ini`, on by default). Without it the vertical axis still feels wrong. It
also installs the Ultimate ASI Loader (`version.dll`) that loads this mod.

### 2. GTASADE PC Fix

Extract **`GTASADE-PCFix-v0.2.0.zip`** into the game folder. You should then have:

```text
Gameface\Binaries\Win64\GTASADE.PCFix.addon64
Gameface\Binaries\Win64\scripts\GTASADE.PCFix.asi
Gameface\Binaries\Win64\scripts\GTASADE.PCFix.RawInputHelper.exe
Gameface\Binaries\Win64\scripts\GTASADE.PCFix.ini
```

Keep the `.asi` in `scripts`: an `.asi` directly in `Win64` made the game crash on start together
with the ASI loader, ReShade and the Steam overlay.

**Updating from v0.1.0 ("Raw Mouse Fix")?** Delete the old files first:
`scripts\SADE.HighFpsRawMouseFix.asi`, `.RawInputCompanion.exe`, `.ini` and old
`SADE.HighFpsRawMouseFix_*.log` / `.csv` files. If the old version is still there, the new one stays
inactive and says so in its log.

### 3. ReShade (optional, recommended)

Gives you the settings panel, the on-screen info and the hotkeys. The mouse fix and the FPS cap work
without it.

- Easiest: [RHI](https://github.com/RankFTW/RHI) (ReShade HDR Installer) sets it up per game in a
  couple of clicks. The mod was tested this way.
- Or the official installer from [reshade.me](https://reshade.me): download the version **with full
  add-on support**, select `Gameface\Binaries\Win64\SanAndreas.exe`, choose DirectX 10/11/12.

ReShade loads `GTASADE.PCFix.addon64` by itself.

### If it does not work

Add the Steam launch option `-dx12` (game → *Properties* → *Launch options*). The mod was tested
in DirectX 12.

## Check that it works

1. Start the game, load a save and play for about 20 seconds (the camera hooks start ~15 s after
   launch).
2. Open `Gameface\Binaries\Win64\scripts\GTASADE.PCFix.log`. It must contain:

   ```text
   RAW MOUSE FIX ACTIVE
   ```

3. With ReShade: press **Home** → tab **Add-ons** → **GTASADE PC Fix**. *Game build*,
   *Core*, *Raw Input helper* and *Camera hooks* should be green. Tick **Show mouse info over the
   game**: while you move the mouse in gameplay it should show `Mouse: game process ~1000 Hz` (or
   your mouse's polling rate).

## Default settings and how to change them

| Setting | Default | Change it |
|---|---|---|
| Raw mouse camera | On | panel, `Ctrl+End`, ini `[Mouse] Enabled` |
| Sensitivity X / Y | 1.00 / 1.00 | panel (slider or exact number), ini `SensitivityX` / `SensitivityY` |
| FPS cap | 120 | panel, `Ctrl+PgUp` / `Ctrl+PgDn`, ini `[Game] MaxFps` |
| On-screen info | Off | panel, `Ctrl+Insert` |
| Developer traces | Off | panel → *Developer*, ini `[Developer] Enabled` |

Panel = ReShade overlay (**Home**) → *Add-ons* → *GTASADE PC Fix*. Changes apply immediately.
FPS-cap changes are saved automatically; press *Save to ini* to keep sensitivity and on/off for the
next launch.

`MaxFps`: `0` = off (the game decides), `-1` = unlimited, otherwise 30 / 60 / 90 / 120 / 180 / 240.
The cap applies a few seconds after start and again right after the Rockstar sign-in, where the
game resets it.

Hotkeys (ReShade required, game window active):

| Keys | Action |
|---|---|
| `Ctrl+PgUp` / `Ctrl+PgDn` | FPS cap: Off → 30 → 60 → 90 → 120 → 180 → 240 → Unlimited |
| `Ctrl+End` | Raw mouse camera on/off (to compare with the game's own mouse) |
| `Ctrl+Insert` | Show/hide the on-screen info |

The on-screen info can be dragged while the ReShade menu is open.

## Uninstall

Close the game and delete the four files above and `scripts\GTASADE.PCFix.log`. ReShade or the ASI loader can stay or be removed separately.

## Compatibility

- **Fusion Fix** (required), **ReShade / RenoDX** (via RHI), **NVIDIA Smooth Motion**: work together.
- **RTX 40 MFG unlockers** (RTXMFG, MFGAdaUnlock): load without conflicts but have no effect in
  this game, because it has no native DLSS Frame Generation for them to extend.
- Single-player only. Do not use mods in online or anti-cheat protected games.

## Known issues

- The camera fix starts about 15 seconds after the game launches.
- In the pause menu the highlight returns to the open tab when the cursor leaves the items; this
  is the game's own behaviour.
- Occasional frame-time spikes come from the game/engine, not the mouse (measured equally with and
  without mouse movement).
- Animation glitches above 120 FPS are a remaster bug; the default cap avoids them.
- With ReShade and the Steam overlay the game can occasionally fail to start ("could not start the
  game" from the Rockstar launcher); this also happened without the mod. Start it again; if it keeps
  happening, disable the Steam overlay for the game.

## How it works, briefly

The game's camera reads the cursor position every frame. The mod answers those reads with the raw
mouse movement collected since the previous frame. Windows 11 limits raw mouse input for
background processes to about 125 Hz, so the mod reads the mouse inside the game process and uses
the small helper `.exe` only as a fallback.

## Antivirus

The mod is an unsigned plugin that hooks a game process, plus a helper `.exe` and shared memory
between them. That combination can trigger heuristic warnings. Download only from this page,
compare the SHA256 hashes and check the VirusTotal link in the release notes. The source code is
public and you can build it yourself.

## Problems and feedback

Open an [issue](https://github.com/Bryntsev/GTASADE-PCFix/issues) and attach:

- `Gameface\Binaries\Win64\scripts\GTASADE.PCFix.log`
- for mouse problems: enable *Developer mode* in the panel (or `[Developer] Enabled=1` in the ini),
  restart the game, reproduce the problem, then zip the newest folder from
  `Gameface\Binaries\Win64\scripts\GTASADE.PCFix_logs`.

## Building

Windows x64, Visual Studio 2022 Build Tools (MSVC), CMake 3.21+.

```powershell
.\tools\build.ps1 -Configuration Release
.\tools\package_release.ps1 -Version 0.2.0
```

## License

MIT, see [LICENSE](LICENSE). The ReShade add-on uses ReShade and Dear ImGui headers, see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
