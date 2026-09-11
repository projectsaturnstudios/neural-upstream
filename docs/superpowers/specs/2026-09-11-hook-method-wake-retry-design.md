# Hook method wake/retry for Neural Upstream

**Date:** 2026-09-11  
**Repo:** [projectsaturnstudios/neural-upstream](https://github.com/projectsaturnstudios/neural-upstream)  
**Status:** Design locked (pending implementation plan)  
**Related Discord:** J.E.R. [MATH] — NBA 2K27, RTX 40-series, patched `nvngx_dlssnr.dll`, stuck on “waiting for the game to create DLSS” until cycling RenoDX Hook method

## Problem

Neural Upstream hooks `NVSDK_NGX_D3D12_EvaluateFeature` / `CreateFeature` and creates the neural feature lazily from the game’s DLSS evaluate path. Overlay status while enabled and `g_nr_handle == nullptr` is a single yellow string:

> ON (waiting for the game to create DLSS)

That string conflates several states:

1. NGX modules not hooked yet  
2. Hooks OK, but the game has not called Evaluate yet (common on menus; title-dependent — e.g. Silent Hill 2 often evaluates at the start screen, Control / Alan Wake 2 often only in-world, Streamline titles may stay quiet until a graphics path is poked)  
3. Evaluates seen, but neural feature create latched failed / never completed  

RenoDX exposes **`DirectNeuralRenderingHookPoint`** (“Hook method”) with:

`Off` · `Auto` · `Upscaled` · `FrameGen` · `Present`

Users report that **cycling that dropdown** makes DLSS/NR start on titles where Auto alone stays Waiting (NBA 2K27 + Streamline). Changing the hook point retargets where NR attaches and forces a re-bind; it does not invent EvaluateFeature from nothing.

Neural Upstream today only has Placement **Before / After upscale** (roughly a subset of Upscaled vs pre-upscale). Changing Placement does **not** re-arm hooks. `install_hook()` sets `g_hooked` once after the first successful candidate scan and never rescans; frame-gen snippet modules are explicitly skipped.

## Goal

Add a RenoDX-aligned **Hook method** control so users can retarget attachment and **always** re-arm hooks / clear the NR create latch when the method changes (or when they press Poke) — enough to move Waiting → ON on Streamline/late-eval titles the same way J.E.R. does with RenoDX.

## Non-goals (phase 2+)

- Inventing EvaluateFeature when the game never calls DLSS (Feeder / dlss5-bridge synth / private D3D12 contract)  
- Full earlier-DX (D3D11 present) support beyond what an existing D3D12 evaluate path already provides  
- Cloning RenoDX preset banks, NeuralUplift, or other RenoDX-global keys  
- Matching RenoDX binary behavior byte-for-byte if undocumented  

## UI

Bottom of the **NR Pre-Upscale** overlay panel:

1. **Hook method** combo (persisted):  
   `Off` · `Auto` · `Before upscale` · `Upscaled` · `FrameGen` · `Present`  

   Label “Before upscale” is clearer than RenoDX’s omission of an explicit pre-upscale name; map **Upscaled** to after-upscale (RenoDX naming). Do **not** use a separate Placement combo once Hook method owns before/after — keep one source of truth. If Placement remains briefly for compatibility, Hook method overrides it when not Auto.

2. **Poke** button — always enabled while Neural Rendering is on (including Waiting). Same action as changing Hook method (re-arm + clear latch + log) without changing the selected method.

3. **Status line** — replace the single waiting string:

| Condition | Status |
|---|---|
| Hook method Off | `OFF (hook method)` |
| Looking for NGX modules / not hooked yet | `ON (looking for NGX / DLSS modules…)` |
| Hooked, eval count == 0 | `ON (waiting for the game to call DLSS — menus often don’t; try in-world or change Hook method / Poke)` |
| Eval count > 0, handle null | `ON (DLSS seen; neural feature not created — Poke to retry)` |
| Handle non-null | green `ON` (unchanged) |

Short disabled/help text under Poke: re-arms hooks for the selected method; does not invent DLSS if the game never evaluates on any path.

## Behavior

### On Hook method change or Poke

Always (even while Waiting):

1. **Rescan / re-arm** Evaluate + Create hooks according to the selected method (see mapping below). Allow re-entry even if `g_hooked` was already true (reset the “hooked once” latch for this action).  
2. **Clear NR create latch:** release or null `g_nr_handle` / hi handle as safe; clear `g_setup_done` (and any sibling “create abandoned” flags) so the next evaluate runs `setup_nr` again.  
3. **Force-reset:** set `g_force_reset_frames` (existing mechanism) so temporal history does not carry across the re-arm.  
4. **Log** a single structured line, e.g.  
   `[NRPRE] poke: method=<name> evals=<n> hooked=<count> modules=...`  
   so Discord support can see whether the poke helped.

### Method mapping

| Method | Attachment behavior |
|---|---|
| **Off** | Do not install or enable NGX evaluate hooks for NR; passthrough game DLSS; status Off. |
| **Auto** | Current behavior: enum process modules exporting `NVSDK_NGX_D3D12_EvaluateFeature`; hook all non-self, non-excluded candidates; Placement/Before-vs-After follows existing `g_placement` default (or last Before/Upscaled choice). |
| **Before upscale** | Auto-style module scan + force `g_placement = 0` (NR on colour into evaluate). |
| **Upscaled** | Auto-style module scan + force `g_placement = 1` (NR on evaluate Output — RenoDX-like consumer role). |
| **FrameGen** | Like Auto, but **do not skip** frame-gen / DLSSG-related modules that `is_framegen_snippet` currently excludes; prefer Streamline FG-related exporters when present. Still D3D12 EvaluateFeature only. Intent: NBA 2K / Streamline paths where evaluates appear on FG-related modules. |
| **Present** | Like Auto re-arm every present (or until first eval) so late-loaded Streamline modules are adopted; if still zero evals after arming, status text stresses in-world / graphics toggle. Does **not** synthesize an evaluate in v1. Optional follow-up: light present-path diagnostics only. |

### Config

Persist under existing `[NRPreUpscale]` in `ReShade.ini`, e.g.:

- `HookMethod` = 0..5 (Off, Auto, Before, Upscaled, FrameGen, Present)  
- Keep `Placement` in sync when method is Before or Upscaled so older builds/tools reading Placement stay consistent  

Default: **Auto** (no behavior change for existing users until they change the dropdown).

### Threading / safety

- Perform MH re-hook work on the same threads already used for `install_hook` / present (no new random threads).  
- Do not tear down GPU resources from inside evaluate; keep using existing present-deferred teardown patterns (`g_rebuild_pending` / release_state at present).  
- Poke from overlay may only set flags if re-hooking mid-evaluate is unsafe; document that the actual re-arm runs at next present (preferred) if required for stability.

## Success criteria

1. Fresh install, Hook method Auto: existing titles (AW2, Control, SH2) behave as today.  
2. On a Streamline title that stays Waiting on Auto: changing Hook method among Upscaled / FrameGen / Present (or pressing Poke) re-arms hooks and logs `poke:`; when the game’s evaluate appears on that path, status becomes green ON and NR runs.  
3. Poke is never greyed out while Neural Rendering is on and waiting.  
4. Support log after a user poke is enough to tell “0 evals after poke” vs “evals > 0 but create failed.”  

## Risks / open implementation notes

- **FrameGen:** Today frame-gen snippets are skipped on purpose (FINDINGS / comments). Enabling them may double-enter hooks or attach to the wrong evaluate. Implement behind the FrameGen method only; guard re-entry (already partially present). Validate on one Streamline title before declaring support.  
- **Present:** Without synth, Present cannot create evaluates; it only improves late module adoption and messaging. Do not over-promise “Present = always works with no DLSS.”  
- **RenoDX coexistence:** Conflict list still stands down when `renodx-dlss5.addon64` is loaded. Hook method is for Neural Upstream alone, not a second neural consumer beside RenoDX.  

## Out of scope follow-ups

1. Feeder / bridge synthetic evaluate when all methods still show 0 evals (earlier-DX / no-DLSS phase).  
2. Deeper Streamline API poking if FrameGen/Present prove insufficient on NBA 2K after implementation.  

## References

- Neural Upstream: `src/addon.cpp` — `install_hook`, `g_placement`, overlay status, `is_framegen_snippet`, conflict list  
- RenoDX hook points (external UI mirror): `DirectNeuralRenderingHookPoint` values Off/Auto/Upscaled/FrameGen/Present (documented via DLSS-5-MANAGER overlay mapping to RenoDX config)  
- Discord: J.E.R. [MATH] NBA 2K27 waiting until Hook method cycled; patched ShortFuse NR model already in use  
