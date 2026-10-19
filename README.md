# SWTORFix

Client-side fixes for the SWTOR client, injected into `swtor.exe` by
[SWTORWrapper](https://github.com/jedi95/SWTORWrapper). Built as an x64 DLL
that `SWTORWrapper.exe` loads with `LoadLibraryA` once the game process appears.

**Exactly what the patch does and how it works, with the disassembly, the measurements and
what it deliberately does not touch: [HOW-IT-WORKS.md](HOW-IT-WORKS.md).**

## Jobs

Each job can be switched off in `SWTORFix.ini`, placed **beside the DLL**:

```ini
[jobs]
BlockDisplayChanges=1
TeleportSnap=1
```

A missing file, missing section or missing key leaves that job **enabled**, so
the DLL works with no configuration at all. Set a value to `0` to disable it.

| job | default | what it does |
|---|---|---|
| `BlockDisplayChanges` | on | Hooks `ChangeDisplaySettingsEx` and reports success without changing the display mode. The client probes display modes at startup; this skips that. |
| `TeleportSnap` | on | Fixes teleport desync. See below. |

Nothing is written to disk. Failures go to `OutputDebugString` only, visible in
DebugView if you want them.

## TeleportSnap

The client integrates actor motion in `FUN_140604f20(actor, dt)`:

```
dt  = clamp(dt, 0, [actor+0x350])         remaining time budget
pos = base + dt * [actor+0x408..0x410]    velocity, memory units per second
```

A teleport is a high velocity (~16.6 units/s) with a budget of roughly 0.05 s,
normally consumed over about five frames. When that sequence is cut short the
actor stops partway along the path — the client renders the boss short of where
the server put it, and melee range checks then fail. This is the desync players
have reported on teleport-only bosses for years.

The fix: when the velocity is teleport-sized, consume the whole budget in a
single frame. The motion lands on its endpoint instead of being abandoned.

Measured on solo Dread Master Tyrans, same fight, same analysis:

| | unpatched | patched |
|---|---|---|
| teleports | 46 | 43 |
| exact landings (<5 cm) | 24 (52%) | **43 (100%)** |
| worst residual | 4.23 m | **0.00 m** |

Threshold is 5.0 units/s (50 m/s): walking is ~0.4 (4 m/s), teleports ~16.6
(166 m/s), so it separates them with margin and stays clear of charge-type
abilities.

The hook validates the first 16 bytes of the target before attaching and refuses
to install on an unrecognised client:

```
40 53 48 83 ec 50 f7 81 f8 02 00 00 00 00 01 00
```

Note the entry point is `0x604F20`, not `0x604F22` — `0x604F22` is the second
instruction, and hooking there skips the `push rbx`, leaving the stack misaligned
and faulting on the first aligned SSE spill.

## Build

Visual Studio 2022, x64. Detours is vendored under `SWTORFix/lib` and
`SWTORFix/include`.

```
msbuild SWTORFix.sln /p:Configuration=Release /p:Platform=x64
```

Output is `SWTORFix.dll`. Drop it beside `SWTORWrapper.exe` — the wrapper loads
it by name from its own directory.

## Compatibility

The prologue check means an updated client fails closed: the hook simply does
not install, and the game runs unpatched. Nothing is patched in memory if the
bytes do not match.
