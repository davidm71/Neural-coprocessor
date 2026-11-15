# Steam overlay guard (SO1 / SO2 / SO3)

Steam injects `gameoverlayrenderer64.dll` into every game it starts. Its hook on DXGI
Present sometimes faults in this setup: an access violation **reading
0xFFFFFFFFFFFFFFFF** at `gameoverlayrenderer64.dll+0x9B700` (the Steam build seen
Oct-Nov 2026). Before the guard, RoboCop: Rogue City closed about every five minutes
from it. The guard steps over that fault: the overlay's Present returns S_OK to
whoever called it, one frame is not shown, and the game goes on.

`SteamOverlayGuard` in mgpu.ini:

| Value | Meaning |
|---|---|
| `1` (default) | Step over the known fault only (read of 0xFFFFFFFFFFFFFFFF inside the overlay, at any offset). |
| `2` | Step over any access violation inside the overlay (SO1's original rule). Return checks still apply. |
| `3` | SO1's original guard, exactly: any access violation inside the overlay, unwound as SO1 did, no return checks. The new logging and SO2/SO3 verdicts stay, and each step-over says whether the return checks would have passed. |
| `0` | Off. Every fault goes on as before. |

## What it checks before stepping over

All of these must hold, or the fault is passed on untouched (and the CR1 crash line
is written as before):

1. **The signature.** Access violation (0xC0000005), continuable, faulting
   instruction inside the overlay's address range (cached from the loader, so ASLR
   and Steam updates are handled), a READ of exactly 0xFFFFFFFFFFFFFFFF. The offset is
   reported: `+0x9B700` is "the known offset", anything else is "a new offset - Steam
   was probably updated" and is still accepted.
2. **The unwind.** The overlay's frames are unwound with Windows' own unwind tables
   (`RtlLookupFunctionEntry` / `RtlVirtualUnwind`, which restore every non-volatile
   register the overlay saved). Only the faulting function may lack unwind data
   (a leaf). Each step must move the stack pointer up and stay inside the thread's
   stack. At most 64 overlay frames.
3. **The return destination.** The first frame outside the overlay must be
   committed executable memory, the bytes before it must be a CALL instruction
   (E8 rel32 or FF /2 in any addressing form), the stack must be 16-byte aligned
   there (as the x64 calling convention requires after a call), and looking further
   up the stack must not find the overlay again (that would mean the frames were a
   callback through another module, which does not expect a Present's S_OK).
4. **Is it a Present? (SO4)** A CALL in executable code says the return is a call
   site, not that the call was `IDXGISwapChain::Present`. Two pieces of evidence are
   gathered: the CALL's form (a COM call `call [reg+disp]` names its vtable slot:
   +0x40 Present, +0xB0 Present1) and whether the faulting thread passed ReShade's
   present event in the last 250 ms (that event runs on the presenting thread, just
   before Present). The step-over is refused only on **contrary** evidence: another
   vtable slot from a thread that was not presenting. Missing evidence is reported as
   "S_OK unverified", never treated as proof either way.
5. **The rate.** At most 5 step-overs in any 60 s (lock-free). The 6th is passed on,
   with a "LIMIT REACHED" line - the game will most likely close then.

## What it records

| Where | Line | When |
|---|---|---|
| crash.txt | `[MGPU][SO1] STEPPED OVER #n (attempt; verdict follows as SO2)` or `NOT stepped over` + reason | In the handler, at once (survives a crash right after). |
| ReShade.log | `[MGPU][SO2] Steam overlay guard on, ... build stamp 0x...` | Once: mode, overlay base, size, PE build stamp. |
| ReShade.log | `[MGPU][SO1] STEPPED OVER #n ...` | Fault offset, address read, thread, return destination (module+offset, image or private memory), overlay frames unwound and how many had their own exception handler, stack pointer, session counts. The first time per offset: all 16 registers and the 32 code bytes around the fault. |
| ReShade.log | `[MGPU][SO1] NOT STEPPED OVER ...` | Which check failed. |
| both | `Present evidence STRONG / partial / NONE (S_OK unverified) / CONTRARY` | Part of each SO1 line (SO4): the CALL's form and vtable slot, and whether the thread was presenting. |
| ReShade.log | `[MGPU][SO4] guard records not logged ...` | Only if records were dropped (slot busy), overwritten unread or skipped, or a step-over got no verdict because 8 were already waiting. crash.txt has every step-over regardless. |
| ReShade.log | `[MGPU][SO2] step-over #n: RESUMED / ... / DEGRADED / FAILED` | About 2 s after: presents since, rate before and after, whether the faulting thread presented again, any failed present (bridge), device removed (game's D3D12 device), new faults. |
| ReShade.log | `[MGPU][SO3] 60 s after step-over #n: ...` | Process memory, handle count and the render card's video memory compared with just after the step-over. |

"STEPPED OVER" means the handler changed the context and returned
EXCEPTION_CONTINUE_EXECUTION - an attempt. Only the SO2 verdict says whether
rendering actually came back. Each step-over has its own verdict (up to 8 waiting at
once); a second fault within the 2 s shows in the first one's verdict as a new fault.

### How the handler stays out of the way (SO4)

- Records go from the handler to the bridge thread through a 32-slot ring. Each slot
  holds its sequence number and a state (free, being written, ready, being read) in one
  atomic word: a writer only writes a slot nobody is reading, the reader only reads a
  slot nobody is writing, and a writer that finds its slot busy drops the record
  instead of waiting (counted, `[MGPU][SO4]`). Wraparound overwrites the oldest unread
  record (counted) and the reader skips it.
- The handler's crash.txt line is one `WriteFile` to a handle opened at start-up, with
  no `FlushFileBuffers`: the data is in Windows' file cache, which survives the game
  ending; a flush only adds a disk wait on the render thread. The CR1 line for a
  fault that is passed on still flushes, as before.
- Start-up work under the loader lock (DllMain and the DLL-load callback) is limited
  to opening crash.txt, reading mgpu.ini, `GetModuleHandleW` and reading the overlay's
  PE header from memory - nothing that loads a DLL or waits on another thread. The
  ring and assessment table are plain atomics initialised before DllMain runs.

ReShade.log lines are written by the bridge thread (a few times a second while the
bridge is running). If the bridge is not running, only crash.txt has the record.

## Validating in a RoboCop session

1. Check the build: `[MGPU][R141] ... source SO2` or later.
2. Check the guard line: `[MGPU][SO2] Steam overlay guard on, the known fault only |
   gameoverlayrenderer64.dll at 0x..., ... build stamp 0x...`. Note the build stamp:
   if the offset ever changes, this is what moved.
3. Play. When a step-over happens, expect in order: `SO1 STEPPED OVER #1` (crash.txt
   and ReShade.log), then `SO2 step-over #1: RESUMED`, then `SO3` a minute later.
4. Read the SO2 verdict:
   - **RESUMED** - what we want.
   - **frames are flowing, but the faulting thread has not presented since** - the
     game is fine but the thread that faulted did not draw again within 2 s.
     If a freeze or hitch followed, report it with the log.
   - **DEGRADED** - slower than before, or another fault followed. Report.
   - **FAILED** - no frames, failed present or device removed. Report.
5. A `NOT STEPPED OVER` line means a check refused. If it says "NOT the known one",
   the overlay faulted differently (the registers and code bytes in the line are what
   is needed to look at it). `SteamOverlayGuard=2` accepts any overlay fault if that
   keeps happening. If it names an unsafe return, send the line - that check is what
   kept the game from resuming somewhere wrong.

## 30-60 minute stress checklist

Run once with the settings you normally use (FG x4-x6, neural rendering on), started
from Steam.

- [ ] Log shows `source SO2` or later and the `[MGPU][SO2] Steam overlay guard on` line.
- [ ] 0-10 min: normal play, alt-tab out and back twice, open and close the MGPU panel.
- [ ] 10-20 min: Shift+Tab (Steam overlay) open/close five times; take a Steam
      screenshot (F12) twice.
- [ ] 20-30 min: load two saves, one heavy fight; change FG multiplier once in the panel.
- [ ] 30-60 min (optional): idle in a busy scene for 10 min, then play.
- [ ] Every `SO1 STEPPED OVER` is followed by an `SO2` verdict; count RESUMED vs other.
- [ ] No `LIMIT REACHED`, no `NOT STEPPED OVER`.
- [ ] `SO3` lines: memory/handles/video memory not climbing step-over after step-over.
- [ ] No freeze. If the game freezes after a step-over (the overlay holding a lock is
      the one failure no check can see), note the time and send ReShade.log and crash.txt.
- [ ] FPS HUD (ADVANCED) frame rates the same before and after each step-over.

## Rollback

- **Broad acceptance:** `SteamOverlayGuard=2` in mgpu.ini. Use it if overlay faults now
  show up as `NOT STEPPED OVER ... NOT the known one`.
- **The old guard's behaviour, with the new logging:** `SteamOverlayGuard=3`. Use it if a
  return check refuses (`NOT STEPPED OVER ... unsafe return`). Each step-over then says
  "the return checks would have passed / REFUSED - reason", which is what is needed to
  fix the check.
- **Guard off:** `SteamOverlayGuard=0`.
- **Previous build:** keep the previous `.addon64` (build SA4, commit e0aaf4c) next to
  the new one under another name and swap them back; or `git revert` the SO2 commit.
  mgpu.ini needs no change for that - SA4 reads `SteamOverlayGuard=0` the same way and
  treats 2 and 3 as on.

## What static checks can and cannot show

Checked here (Linux, no Windows):
- The decisions (signature, CALL detection, destination checks, rate limit under 16
  threads, health verdict), and since SO4 the CALL's vtable slot and the evidence
  rule, the ring (wraparound, 12 writers racing one reader with no torn records and
  every record accounted for, ordering with one writer) and the per-recovery
  assessments (back-to-back recoveries, a full table, 16 threads claiming at once,
  claims and presents racing): `tests/steam_guard_test.cpp`, 281 checks, clean under
  ThreadSanitizer and AddressSanitizer/UBSan over repeated runs.
- `crashlog.cpp`, `dllmain.cpp` and `gpu1_context.cpp` compile against the Windows
  headers (clang, x86_64-windows) with no warnings in the changed code.

Only a real session can show (Windows, ReShade, Steam, D3D12, NGX):
- That the real caller's CALL decodes as expected (the SO1 line shows it) and that
  the faulting thread is seen presenting. If the evidence comes out NONE on real
  step-overs, the rule is only reporting, not refusing.
- That `thread_local` and the static ring and table behave in the add-on (MSVC, a DLL
  loaded at run time) as they do in the tests (GCC, an executable).
- That the overlay's frames have unwind data and the caller passes the checks (if
  not, the log says which check refused - that is the first thing to look at).
- That returning S_OK is acceptable to the caller each time (the SO2 verdict).
- That the overlay holds no lock across the faulting read. Its own cleanup does not
  run when it is stepped over; a freeze would be the sign.
- Whether the overlay's own state is still sound after the skipped call (repeat
  faults, SO3 drift).

## Prevention on the mod's own chain: tried, not possible this way (OB1-OB4)

The first real step-over (2026-11-07 22:21:01) faulted on the mod's frame generation
present thread, at the overlay's first instruction (`mov [rsp+8],rbx`), reached through
`call [rax+0x40]` from dxgi.dll - a general-protection fault (Windows reports address -1
when it has none), most likely the hook's code being changed at that moment while that
thread calls through it up to ~400 times a second.

OB1-OB3 tried to give the mod's swap chain a private function table with Present
pointing at dxgi.dll's own code. The check on 2026-11-08 found the table untouched:
Steam patches DirectX's Present code itself, so a table cannot skip it. The option was
removed in OB4 (OB1/OB2 also crashed when turned on in that case - an unbuilt table was
installed). What remains is `[MGPU][SH1]`, a one-time read-only report of how Steam
hooks Present. Skipping an in-code hook would need a trampoline (Present's original
first instructions, read from dxgi.dll on disk, then a jump past Steam's patch); not
built - the guard handles the fault.

## Toward preventing it

Recovery is after the fact. To prevent the fault, the overlay's bad read must be
understood: which pointer it follows and why it holds -1 (often a "not found" index or
handle). The first SO1 line for each offset carries the registers and 32 code bytes
around the faulting instruction for that reason - with them the instruction and the
register holding -1 can be decoded and matched to what the overlay was looking up
(for example a swap chain it did not see created). Until that is known, there is no
safe hook-level fix to make.
