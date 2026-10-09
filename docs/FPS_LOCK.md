# FPS cap (FpsLock)

The game resets its frame-rate cap to 60 on every launch (also without mods) and does not persist the
menu choice to `GameUserSettings.ini`. `t.MaxFPS` in `Engine.ini [SystemSettings]` has no effect: the
game uses its own limiter.

## How the fields were found

Run `20261009_080941`, SanAndreas.exe `1.0.113.21181`, developer value scanner (`src/devtools/value_scanner.*`):
first scan 60, then 90, 120, 180, 240, 0 (unlimited), 30 as set in the game menu. Five candidates were
left; writing 120 to all of them made the game run at 120 while the menu still showed 30.

| vtable RVA   | field offset | type  | notes |
|--------------|--------------|-------|-------|
| `0x041F9D90` | `0x220`      | int32 | two objects; menu value in FPS, 0 = unlimited |
| `0x041F9A60` | `0x0B0`      | float | common class (~700 objects); next float is the frame time in ms (60 -> 16.67) |
| `0x0445B050` | `0x048`      | float | same object as below |
| `0x0445B050` | `0x04C`      | float | |

All four vtables are in `.rdata` and point into `.text`.

## Runtime

`src/game/fps_lock.*` scans private read-write memory for objects whose first qword is one of these
vtables (every 5 s until found, then every 30 s, and sooner when an object disappears), and every
250 ms rewrites the fields when they differ from `[Game] MaxFps` (0 = off, -1 = unlimited, written as 0). It refuses to write if more than 16
matches are found. Only runs on the supported build (`supported_internal_trace_fingerprint`).

After a game update: enable developer mode, use *Setting finder (FPS cap)* in the add-on panel, and
update the table above and `kFields` in `fps_lock.cpp`.

Run `20261009_082707`: the vtable `0x041F9A60` matched 702 objects, so a plain vtable match is not
enough for that field. A field with more than 8 vtable matches is only used where the value is a
menu option and `value * next_float ~= 1000` (cap x frame time), at most 4 objects. Objects found
once are kept across rescans while their vtable is intact, because after the mod writes its cap the
signature no longer holds.

Run `20261009_090926` corrected that: `0x041F9A60` had only 4 objects, while `0x0445B050` (the float
pair) had 347. Writing only the int32 and the `+0xB0` float did **not** change the frame rate, so the
float pair is the limiter. Its signature: `+0x48 == +0x4C`, the value is a menu option, and `+0x50`
holds a pointer to `exe+0x445B108`.
