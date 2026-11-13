# SWTORFix

Client-side fixes for the SWTOR client, injected into `swtor.exe` by
[SWTORWrapper](https://git.jedi95.com/jedi95/SWTORWrapper). Built as an x64 DLL
that `SWTORWrapper.exe` loads with `LoadLibraryA` once the game process appears.

**Exactly what the patch does and how it works, with the disassembly, the measurements and
what it deliberately does not touch: [HOW-IT-WORKS.md](HOW-IT-WORKS.md).**

## Jobs

Each job can be switched off in `SWTORFix.ini`, placed **beside the DLL**:

```ini
[jobs]
BlockDisplayChanges=1
TeleportSnap=1
LogFlush=1

[log]
FlushMillis=1000

[teleport]
MinVelocity=10

[debug]
LogFires=0
```

A missing file, missing section or missing key leaves that job **enabled**, so
the DLL works with no configuration at all. Set a value to `0` to disable it.

| job | default | what it does |
|---|---|---|
| `BlockDisplayChanges` | on | Hooks `ChangeDisplaySettingsEx` and reports success without changing the display mode. The client probes display modes at startup; this skips that. |
| `TeleportSnap` | on | Fixes teleport desync. See below. |
| `LogFlush` | on | Shortens the combat log flush timer so live parsers see events sooner. Interval in `[log] FlushMillis`, default `1000`, clamped to 100–10000. |

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

The threshold is 10 units/s (100 m/s). Teleports measure ~16.6 units/s (166 m/s).
Bestia's Force Charge — the fastest non-teleport motion measured so far — runs at
1.5–5.8 units/s across 111 samples (14.8–57.9 m/s over 0.49–0.83 s), and its speed
scales with distance, so the long charges are the fast ones. 10 sits above every
charge observed and below every teleport.

Verified in game on a council run: at 10, Bestia's Force Charge arcs smoothly and
teleports still land exactly.

`[teleport] MinVelocity` overrides it — lower it if teleports stop being caught.
`[debug] LogFires=1` reports every snap decision with the actor's flags and
velocity through `OutputDebugString`, so you can see exactly what is being caught.

The hook validates the first 16 bytes of the target before attaching, then checks that every
actor field it reads is still addressed inside the function (see [Compatibility](#compatibility)),
and refuses to install on an unrecognised client:

```
40 53 48 83 ec 50 f7 81 f8 02 00 00 00 00 01 00
```

Note the entry point is `0x604F20`, not `0x604F22` — `0x604F22` is the second
instruction, and hooking there skips the `push rbx`, leaving the stack misaligned
and faulting on the first aligned SSE spill.

## LogFlush

The client flushes its combat log on a three-second countdown. This job patches the reset
constant so the countdown is `[log] FlushMillis` long instead — 1000 ms by default — cutting
worst-case latency for anything parsing the log live from about 3 s to about 1.

Nothing is added, removed or reordered: the data is in the file's buffer either way, and the
CRT still flushes on its own when its 4096-byte buffer fills.

Verified in game: the log now updates on a ~1 s cadence.

## Build

Visual Studio with the **v145** platform toolset, x64. Detours is vendored under
`SWTORFix/lib` and `SWTORFix/include`.

```
msbuild SWTORFix.sln /p:Configuration=Release /p:Platform=x64
```

Output is `SWTORFix.dll`. Drop it beside `SWTORWrapper.exe` — the wrapper loads
it by name from its own directory.

## Verifying an install

`tools/verify_install.py` launches a client, injects the DLL into it exactly the way the
wrapper does, and reads the patched bytes back out of the live process:

```
python tools/verify_install.py --exe "C:\Games\Star Wars - The Old Republic\swtor\retailclient\swtor.exe" --dll x64\Release\SWTORFix.dll
python tools/verify_install.py --exe "...\swtor_RC2.exe" --dll ... --motion-rva 0x60B9C0 --log-rva 0x5380B0
```

```
  [PASS] LoadLibraryA in target returned a module handle - 0xFA610000
  [PASS] target module base - 0x7ff6bda00000
  [PASS] TeleportSnap hook at RVA 0x604F20 - bytes e953b2febf
  [PASS] LogFlush interval at RVA 0x530A52 - 1.000 s
```

`--expect-none` inverts the last two checks, for a binary the DLL is supposed to refuse.
The process is launched suspended and killed afterwards; the game never gets past its entry
point, so no login or asset load is involved.

## Compatibility

Both jobs locate their site the same way: verify the known RVA against the exact bytes, fall
back to a scan that only accepts a single unique match, and refuse to install otherwise.
`TeleportSnap` additionally verifies that every actor field it reads (`+0x350` budget,
`+0x408..0x410` velocity, `+0x420` motion bits) is still addressed inside the function, so a
build that moves a field fails closed instead of snapping on a value read from the wrong
offset.

That means the DLL is not tied to one build. Verified:

| client build | date | `TeleportSnap` | `LogFlush` | result |
|---|---|---|---|---|
| `swtor.exe` `0x6AC6A73E` | 2026-10-07 | RVA `0x604F20` | RVA `0x530A52` | known RVAs |
| `swtor.exe` `0x6AF60939` | 2026-11-12 | RVA `0x604F20` | RVA `0x530A52` | known RVAs (byte-identical to the above at both sites) |
| `swtor_RC2.exe` | 2026-11-12 | RVA `0x60B9C0` | RVA `0x5380B0` | scan |

Each job logs one line to `OutputDebugString` when it installs, starting with the client's PE
build stamp, so the first thing to check after a game patch is which binary the DLL is in:

```
[SWTORFix] client build stamp 6AF60939, image 0x21D8000 bytes
[SWTORFix] BlockDisplayChanges installed
[SWTORFix] LogFlush set to 1.000 s at RVA 0x530A52 (known RVA)
[SWTORFix] TeleportSnap installed at RVA 0x604F20 (known RVA), threshold 10 units/s
```

A refused job says why — `prologue absent or ambiguous`, `layout check failed on
velocity.x [actor+0x408]`, `pattern absent or ambiguous`.

## License

MIT — see [LICENSE](LICENSE).
