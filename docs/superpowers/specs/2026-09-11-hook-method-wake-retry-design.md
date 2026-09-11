# Hook method wake/retry for Neural Upstream

**Date:** 2026-09-11  
**Repo:** [projectsaturnstudios/neural-upstream](https://github.com/projectsaturnstudios/neural-upstream)  
**Status:** Design locked (implementation contract)  
**Release target:** v1.0.2  
**Related Discord:** J.E.R. [MATH] — NBA 2K27, RTX 40-series, patched `nvngx_dlssnr.dll`, stuck on “waiting for the game to create DLSS” until cycling RenoDX Hook method  
**Supersedes:** docs-only draft on `docs/hook-method-wake-retry` / PR #1 (CodeRabbit: not clean). This document is the living contract.

## Problem

Neural Upstream hooks `NVSDK_NGX_D3D12_EvaluateFeature` and creates the neural feature lazily from the game’s DLSS evaluate path. Overlay status while enabled and `g_nr_handle == nullptr` is a single yellow string:

> ON (waiting for the game to create DLSS)

That string conflates several states:

1. NGX modules not hooked yet (or a failed scan latched as “done”)
2. Hooks OK, but the game has not called Evaluate yet (common on menus)
3. Evaluates seen, but neural feature create latched failed / never completed
4. A conflicting add-on forced a stand-down, still shown as Waiting

RenoDX exposes **`DirectNeuralRenderingHookPoint`** (“Hook method”) with:

`Off` · `Auto` · `Upscaled` · `FrameGen` · `Present`

Users report that **cycling that dropdown** makes DLSS/NR start on titles where Auto alone stays Waiting (NBA 2K27 + Streamline). Changing the hook point retargets where NR attaches and forces a re-bind; it does not invent EvaluateFeature from nothing.

### What the current code actually does (verified in `src/addon.cpp`)

- `install_hook()` sets `g_hooked = true` after `MH_Initialize` **before** any `MH_CreateHook` / `MH_EnableHook` succeeds. A scan that finds candidates but fails every enable never retries.
- `g_eval_count` increments only after the neural path runs with a valid handle. It is cumulative. It cannot distinguish “DLSS seen” from “Waiting”.
- `g_hooked` is a latch only. `release_state()` does not `MH_DisableHook` / `MH_RemoveHook`. Resetting the latch leaves existing detours enabled; a second `MH_CreateHook` returns `MH_ERROR_ALREADY_CREATED`. `Off` cannot actually disarm.
- `setup_nr()` keeps `s_init_done` / `s_init_res` as function-static state. Clearing `g_setup_done` does not allow `Init_Ext` retry after a failed init.
- `is_framegen_snippet` skips any module whose path contains `dlssg`, because hooking FG evaluate into the NR thunk steals the jitter claim and wrecks cadence.
- `draw_overlay()` shows the conflict banner, then still paints Waiting on the master-switch line when `g_nr_handle` is null.
- Present currently calls `install_hook()` every frame; the latch makes that a no-op after the first attempt, including failed attempts.

## Goal

Add a RenoDX-aligned **Hook method** control (attach / re-bind) **alongside** the existing **Placement** control (Before / After). Always re-arm hooks and clear the NR create latch when the method changes or the user presses **Poke** — enough to move Waiting → ON on Streamline / late-eval titles the same way RenoDX Hook method does.

## Two axes (do not collapse)

| Axis | Control | What it changes | Re-arms NGX hooks? |
|---|---|---|---|
| **Placement** | Before / After (existing combo) | Where NR sits relative to the game’s DLSS evaluate | **No.** Changing Placement alone rebuilds the network (existing `g_rebuild_pending`) and does not rescan modules. |
| **Hook method** | Off / Auto / Upscaled / FrameGen / Present | Which modules we attach to, and whether we keep scanning | **Yes**, on change and on Poke. |

- **Before** = NR on colour into evaluate (this add-on’s reason to exist).
- **After** = NR on evaluate Output (RenoDX-like consumer).
- There is **no** “Before upscale” Hook method value. Placement Before already covers that.
- **Upscaled** is the one Hook method that also writes Placement: it forces `g_placement = 1` (After) and saves `Placement` in sync.
- **Auto** keeps the user’s current Placement.

## Non-goals (phase 2+)

- Inventing EvaluateFeature when the game never calls DLSS (Feeder / dlss5-bridge synth / private D3D12 contract)
- Full earlier-DX (D3D11 present) support
- Cloning RenoDX preset banks, NeuralUplift, or other RenoDX-global keys
- Matching RenoDX binary behavior byte-for-byte if undocumented
- Creating the GitHub Release / tagging v1.0.2 (Angel releases). CHANGES must be ready; the release asset must be renamed to `nvngx.dll.nrpre.addon64`.

## UI

**NR Pre-Upscale** overlay, next to the existing Placement combo:

1. **Placement** combo — unchanged: `Before upscale` · `After upscale`. Does not poke / re-arm.
2. **Hook method** combo (persisted): `Off` · `Auto` · `Upscaled` · `FrameGen` · `Present`
3. **Poke** button — always enabled while Neural Rendering is on (including Waiting and conflict stand-down). Same action as changing Hook method (re-arm + clear latch + log) without changing the selected method.
4. **Status line** on the master switch — **order is mandatory**:

| Priority | Condition | Status |
|---|---|---|
| 1 | `g_conflict` | Existing standing-down message (do **not** fall through to Waiting) |
| 2 | Hook method Off | `OFF (hook method)` |
| 3 | No successfully enabled Evaluate hooks | `ON (looking for NGX / DLSS modules…)` |
| 4 | Hooked, current-arm evals == 0 | `ON (waiting for the game to call DLSS — menus often don’t; try in-world or change Hook method / Poke)` |
| 5 | Current-arm evals > 0, handle null | `ON (DLSS seen; neural feature not created — Poke to retry)` |
| 6 | Handle non-null | green `ON` |

Short disabled/help text under Poke: re-arms hooks for the selected method; does not invent DLSS if the game never evaluates on any path.

## Behavior

### Successful hooks ≠ `g_hooked` latch

Track an **owned target registry**: each successfully `MH_CreateHook` + `MH_EnableHook` Evaluate target (address, not just `HMODULE`), its thunk slot, original trampoline, and whether the slot is a FrameGen extra attach.

- `g_eval_hook_count` / overlay “hooked” / poke `hooked=<count>` = number of **successfully enabled** Evaluate hooks.
- `g_hooked` is only a latch meaning “this arm currently owns at least one enabled Evaluate hook, **or** we have stood down for a conflict.”
- A scan that finds zero candidates, or finds candidates but enables none, **must remain retryable** (`g_hooked` stays false).
- Conflict still latches (`g_conflict` + `g_hooked`) and installs nothing.

### Per-arm evaluation count

Do **not** use `g_eval_count` for Waiting vs “DLSS seen”. That counter is cumulative and only increments after the neural path with a valid handle.

- Increment `g_arm_evals` at the hooked EvaluateFeature thunk entry for the **outermost** interception (`t_depth == 0`). Nested proxy re-entry does not increment.
- Failed neural-handle creation still counts: the game called Evaluate.
- Reset / advance the baseline to 0 when a Hook method change or Poke re-arm **completes**.
- Status and poke `evals=<n>` use this current-arm value. Log the **pre-reset** arm count on poke so two consecutive pokes show the 0 → N transition.

### Idempotent quiescent re-arm

`release_state()` does not own MinHook. The registry does.

On method change / Poke / Off:

1. Overlay / hot path only sets flags (`request_poke`). Freeze new NR work immediately (`g_hooks_frozen`) so no evaluate observes released state.
2. Queue the work until a **present** boundary with `g_eval_inflight == 0` (wait the command queue idle, then apply). Do not tear GPU resources mid-evaluate.
3. **Reuse** registry entries whose targets are still wanted. **Disable + Remove** entries that are no longer wanted, then create+enable new targets. Do not `MH_CreateHook` a target we already own.
4. **Off** must actually disarm every owned detour (Evaluate and our Create hook). Repeated Off is a no-op. Off → Auto re-creates as needed and is idempotent.
5. Repeated Poke on the same method reuses hooks, still clears the NR latch, resets the arm eval baseline, and logs.

### Reset `setup_nr` init state on poke (safely)

Move `s_init_done` / `s_init_res` to resettable per-arm file-scope state.

- Re-Init of a snippet that is **already up** can hang (existing comment). Never reset init state while `g_nr_handle != nullptr`, and never reset after a **successful** `Init_Ext`.
- Reset init state only when tearing down / abandoning a **failed** arm (`Init_Ext` failed, handle never obtained) so Poke can attempt `Init_Ext` again.
- If `Init_Ext` succeeded but `CreateFeature` failed: keep init, clear `g_setup_done` so Create retries. That is the existing “do not retry a dead create every frame” latch, deliberately lifted by Poke.

### FrameGen attachment set (correctness over “unskip all dlssg”)

Auto **keeps** skipping modules whose path contains `dlssg`. Blindly feeding every DLSS-G exporter into the NR thunk corrupts cadence: FG evaluates carry no jitter, `claim_frame` remaps key 0 → 1, and a false claim scrambles which frames run the network (FINDINGS.md §6).

**FrameGen allowed set:**

1. Everything Auto would hook (non-self modules exporting `NVSDK_NGX_D3D12_EvaluateFeature`, excluding `*dlssg*`).
2. **Plus** modules Auto would skip **only when** the path contains `dlssg` **and** the module is a Streamline-family exporter (`streamline` in the path, or a `sl.dlss*` / `sl.interposer` filename). Bare driver `nvngx_dlssg.dll` is **not** attached.

**Guards (all required):**

- Duplicate **target address** check (not only `HMODULE`), so a proxy and its forwarded export are not hooked twice into two slots.
- Existing `t_depth` re-entry guard: a Streamline wrapper that forwards to `_nvngx` must not run NR twice.
- FrameGen-extra slots (`*dlssg*` Streamline) set a per-call FG guard: if jitter X and Y are both 0, **do not** `claim_frame` and **do not** run NR; forward only. Real late DLSS evaluates on that module (non-zero jitter) still enter the normal path.
- Cap remains `kMaxNgx` (8) Evaluate hooks.

Acceptance: FrameGen is “rescan including Streamline modules Auto would skip only when path matches the allowlist, with cadence re-entry / zero-jitter guard.” It is **not** “unskip all dlssg.” Document this in CHANGES / README. Prefer correctness if a title still needs Feeder (phase 2).

### Conflict before Waiting

When `g_conflict`, overlay shows the existing standing-down copy and **must not** show Waiting. Poke log reports `conflict=1`. `g_hooked` on a conflict is not evidence that hooks were installed (`hooked=0`).

### Present = bounded incremental scan

Present does **not** mean “run NR at Present.” It is late adoption of exporters.

- Track already-seen `HMODULE`s from `K32EnumProcessModules`.
- While current-arm evals == 0 (or until at least one new exporter is successfully armed on this method), every **N = 60** presents inspect **new** modules only. Do not reset `g_hooked` and re-enum/re-create everything blindly.
- Stop incremental scanning when `(g_eval_hook_count > 0 && g_arm_evals > 0)` **or** the user leaves Present.
- Poke / method change clears the seen set so a deliberate rescan can re-adopt.
- Present still does not synthesize an Evaluate.

### On Hook method change or Poke (at present after quiesce)

1. Rescan / re-arm per method (idempotent registry).
2. Clear NR create latch safely + resettable `setup_nr` init state as above. If a handle exists, `release_state` runs here (present is the safe thread) and `g_force_reset_frames` is set.
3. Force-reset (`g_force_reset_frames = 8`).
4. Log one structured line:

```
[NRPRE] poke: method=<name> evals=<n> hooked=<count> conflict=<0|1> why=<reason>
```

`evals` is the current-arm count **before** reset. `hooked` is the successfully enabled Evaluate count **after** re-arm.

## Method mapping

| Method | Attachment | Placement |
|---|---|---|
| **Off** | Disarm owned detours. Game DLSS untouched. | Unchanged |
| **Auto** | Current module scan: every non-self, non-`dlssg` exporter of `EvaluateFeature`. | Unchanged (user’s Before/After) |
| **Upscaled** | Same scan as Auto | Force After (`g_placement = 1`), persist Placement |
| **FrameGen** | Auto set + Streamline-family `*dlssg*` allowlist, with guards above | Unchanged |
| **Present** | Auto set, plus bounded incremental scan for **new** modules | Unchanged |

## Config

`[NRPreUpscale]` in `ReShade.ini`:

| Key | Values | Default |
|---|---|---|
| `HookMethod` | `0` Off, `1` Auto, `2` Upscaled, `3` FrameGen, `4` Present | `1` Auto |
| `Placement` | `0` Before, `1` After (existing) | `0` Before |

Keep `Placement`. When method is Upscaled, write `Placement=1` on save. Missing `HookMethod` → Auto; do not silently rewrite older installs to Upscaled just because Placement was After.

## Threading / safety

- MH work and `release_state` run on the threads already used for `install_hook` / present.
- Overlay Poke / combo only set flags + freeze.
- No evaluate may observe released GPU state or a half-removed detour.
- Conflict list unchanged (`renodx-dlss5.addon64`, etc.). Hook method is for Neural Upstream alone.

## Success criteria

1. Fresh install, Hook method Auto, Placement Before: existing titles (AW2, Control, SH2) behave as v1.0.1.
2. Changing Placement alone does not re-arm hooks and does not reset `g_arm_evals`.
3. On a Streamline title that stays Waiting on Auto: Upscaled / FrameGen / Present or Poke re-arms, logs `poke:`, and when an evaluate appears on that path, status becomes green ON and NR runs.
4. Failed first scans retry; `hooked=` never counts a failed enable.
5. Two consecutive Pokes: first can log `evals=0`; after the game evaluates, second logs `evals>0` even if create failed.
6. Off disarms detours. Off → Auto is idempotent. Repeated Poke is idempotent.
7. Poke after a failed `Init_Ext` retries `Init_Ext`. Poke after a successful snippet init does not re-Init.
8. Conflict shows standing-down, never Waiting; poke logs `conflict=1`.
9. FrameGen does not attach bare `nvngx_dlssg.dll`; zero-jitter FG evaluates do not claim the cadence.
10. Present does not re-create every hook every frame.
11. CI `build` workflow green. No Feeder. No v1.0.2 git tag from this PR.

## Out of scope follow-ups

1. Feeder / bridge synthetic evaluate when all methods still show 0 evals.
2. Deeper Streamline API poking if FrameGen / Present prove insufficient on NBA 2K after implementation.

## References

- `src/addon.cpp` — `install_hook`, `g_placement`, `g_eval_count`, `setup_nr` statics, overlay status, `is_framegen_snippet`, `claim_frame`, conflict list
- FINDINGS.md §1 (hook every exporter, re-entry) and §6 (FG cadence)
- RenoDX `DirectNeuralRenderingHookPoint`: Off / Auto / Upscaled / FrameGen / Present
- CodeRabbit review on PR #1 (six functional findings + Present nit)
