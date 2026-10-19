# How the teleport patch works

This document describes exactly what `SWTORFix.dll` changes in the client, why, and what it
does not do. It is written so that anyone — including the game's own developers — can verify
the claims against the binary.

## The problem being fixed

A class of bosses moves only by teleporting: they disappear from one position and reappear at
another, with no walk or leap animation in between. On the client, these actors frequently
appear **short of their destination**. The gap varies — tens of centimetres up to tens of
metres — and it is permanent: the boss keeps attacking from a position that does not match
where the client draws it, until the next teleport.

Because melee range is enforced by the server, the visible symptom is that a player standing
next to the boss cannot use melee abilities: the client believes the target is out of range.

The fix is client-side only and concerns how the client animates a move it has already been
given. It does not change what the server believes, and it cannot.

## What the client already knows

When the server moves an actor, the client is told the move. The client then integrates that
motion locally, frame by frame, in `FUN_140604f20` — a virtual method on the actor object.

Signature (Win64): `void FUN_140604f20(actor, float dt)` — `rcx` is the actor, `xmm1` is the
frame time step.

The relevant part of the body:

```asm
movss  xmm0, [rbx+0x350]        ; remaining time budget
movaps xmm3, xmm6               ; xmm3 = dt
comiss xmm6, xmm0
jbe    .keep_dt
movaps xmm3, xmm0               ; dt > budget: use the budget
maxss  xmm3, xmm7               ; clamp to >= 0

movaps xmm1, xmm3               ; xmm3 is now the effective step
mulss  xmm1, [rbx+0x408]        ; * velocity.x
; ... same for 0x40C (z) and 0x410 (y)
addss  xmm1, [rax]              ; + base.x
; ... and so on, then a threshold filter and setPosition
```

In C terms:

```
dt  = clamp(dt, 0, [actor+0x350])
pos = base + dt * [actor+0x408..0x410]
```

- `[actor+0x350]` is a **time budget** for the current move, in seconds. It counts down each
  call: `[actor+0x350] -= dt`.
- `[actor+0x408..0x410]` is a **velocity** in memory units per second. One memory unit is
  0.1 m of world distance.
- `base` comes from a virtual call on the actor; it is the position the move started from.

## What a teleport actually looks like

Captured live during a teleport, one line per call to the function:

```
maxT=0.0482  velocity=(16.6136, 0, 0)  position=-16.700
maxT=0.0382  velocity=(16.6136, 0, 0)  position=-16.535
maxT=0.0288  velocity=(16.6136, 0, 0)  position=-16.379
maxT=0.0191  velocity=(16.6136, 0, 0)  position=-16.218
maxT=0.0091  velocity=(16.6136, 0, 0)  position=-16.051
```

Read it as: the move has a budget of about 0.05 s, a velocity of 16.61 units/s (166 m/s), and
is applied over roughly five frames. Each frame advances the actor about 0.16 units — 1.6 m —
and five frames total about 8 m, which is exactly the spacing of the encounter's floor squares.

So a teleport is not a position assignment. It is a short, very fast movement, integrated like
any other movement, over several frames.

## Why it ends up short

If the integration is abandoned before the budget is spent — a new move arriving, a state
change, a frame where the actor is skipped — the actor stays wherever the last integration
left it. Nothing re-sends the destination afterwards, so the error persists.

That is the observed signature. Across 14 historical combat logs of the Council encounter,
43 of 46 anomalous landings lie exactly on the straight line between origin and destination,
with perpendicular error of essentially zero. The fraction of the path completed has a median
of 0.68 and ranges from 0.075 to 0.961. There is **no overshoot** in any sample — the error is
always an undershoot, which is what a truncated forward integration produces and what a
physics or spring model would not.

The exact reason a given integration is abandoned has not been established; it is not needed
to fix the visible result.

## The patch

A Detours hook on `FUN_140604f20`. When the velocity is teleport-sized, it consumes the whole
budget in a single frame instead of spreading it across several:

```c
if (flags & 0x00010000)                 // the same gate the original function uses
    if (velocity.x² + velocity.y² + velocity.z² > 25.0)
        if (dt < budget)
            dt = budget;                // consume the whole budget this frame
```

With `dt = budget`, the original arithmetic evaluates `base + budget * velocity`, which is the
destination the client was already given. The move completes in one frame, so there is no
window left in which it can be cut short.

### Why the threshold is 5.0 units/s

| motion | velocity |
|---|---|
| walking | ~0.4 units/s (4 m/s) |
| charge-type abilities | between walking and teleporting |
| teleport | ~16.6 units/s (166 m/s) |

5.0 units/s (50 m/s) sits clear of normal movement and of charge abilities, and well below
teleport velocity, so only teleport-scale motion is affected.

### What is actually modified

One value: the `dt` argument, on the stack, for the duration of one call. The function then
runs unmodified and writes the position itself, exactly as it always does.

## What the patch does not do

- **No network traffic.** It does not send, modify, delay or inspect a single packet.
- **No new information.** The destination is already in the client's motion state; the patch
  only stops the client from stopping early on the way there.
- **No server-side effect.** Range checks, line of sight, ability legality and combat
  resolution are server-side and untouched. This is why the fix is cosmetic in the strict
  sense: it makes the client draw what the server already told it.
- **No writes to game state.** It does not write positions, velocities, budgets or flags. It
  changes one argument and calls through.
- **No bypass of any check.** It hooks one function, not an anti-cheat or integrity routine.

## Safety

**Prologue verification.** Before attaching, the hook compares the first 16 bytes of the target
against a known-good copy:

```
40 53 48 83 ec 50 f7 81 f8 02 00 00 00 00 01 00
```

If they differ — a client update, a different build — the hook **does not install** and the
game runs completely unpatched. It fails closed by design.

**Fault tolerance.** The reads are wrapped in structured exception handling. If an object
reaches the hook with an unexpected layout, the read faults, the handler swallows it, and the
original function is called untouched.

**The entry point matters.** The function starts at RVA `0x604F20`. `0x604F22` is the *second*
instruction (`sub rsp, 0x50`); the first is `push rbx`. Hooking at `0x604F22` skips the push,
leaves the stack 8 bytes out of alignment, and the first aligned SSE spill faults immediately.
This is recorded here because it is the exact mistake that produced a client crash during
development.

## Measured result

Solo Dread Master Tyrans, same encounter, same analysis, before and after:

| | unpatched | patched |
|---|---|---|
| teleports observed | 46 | 43 |
| landings exact to <5 cm | 24 (52%) | **43 (100%)** |
| worst residual | 4.23 m | **0.00 m** |
| mean residual | ~0.6 m | **0.000 m** |

In the patched run every landing is an exact lattice point — the combat log prints
`(-167.00, -84.00)` rather than `(-166.96, -106.40)`.

## Compatibility

The patch is tied to a specific client build by the prologue check. The RVA and expected bytes
live in `SWTORFix/dllmain.cpp`; updating them for a new build is a matter of re-verifying the
function's entry and first instructions.
