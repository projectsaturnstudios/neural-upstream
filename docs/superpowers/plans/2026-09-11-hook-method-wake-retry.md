# Hook Method + Poke Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship RenoDX-aligned Hook method (`Off` / `Auto` / `Upscaled` / `FrameGen` / `Present`) plus always-on Poke, keeping Placement as a separate axis, so Streamline titles can move Waiting → ON by retargeting/re-arming NGX hooks; leave CHANGES ready for **v1.0.2**.

**Architecture:** Keep all logic in `src/addon.cpp`. Two axes: Placement (Before/After, no re-arm) and Hook method (attach/re-bind). Deferred `request_poke()` runs at present after in-flight evaluates quiesce. Owned MinHook target registry; successful enables counted separately from the `g_hooked` latch; per-arm evals counted at thunk entry. FrameGen uses a Streamline `*dlssg*` allowlist plus zero-jitter cadence guard. Present is a bounded incremental new-module scan.

**Tech Stack:** C++20 ReShade add-on, MinHook, ImGui (ReShade overlay), D3D12 NGX, Windows CI (`build.yml` → `check-shaders.ps1` + `build.ps1`).

**Spec:** `docs/superpowers/specs/2026-09-11-hook-method-wake-retry-design.md`

## Global Constraints

- Two axes: Placement stays; Hook method is Off/Auto/Upscaled/FrameGen/Present (no “Before upscale” method value).
- Default Hook method = **Auto**. Changing Placement alone does not re-arm hooks.
- Upscaled forces `g_placement = 1` and persists `Placement`.
- Never invent EvaluateFeature (no Feeder/synth).
- Do not tear down GPU state from inside evaluate; poke/Off/method change defer to present after `g_eval_inflight == 0`.
- Successful hooks ≠ `g_hooked`. Failed scans stay retryable. `hooked=<count>` is successful enables only.
- Per-arm evals at thunk entry; reset on completed re-arm; do not use `g_eval_count` for status.
- Reset `setup_nr` Init_Ext state only on a failed arm, never after a successful init (re-Init hangs).
- FrameGen: Auto set + Streamline-family `*dlssg*` only; never bare `nvngx_dlssg.dll`; zero-jitter extra slots do not `claim_frame`.
- Conflict status before Waiting; poke logs `conflict=`.
- Present: incremental new-module scan every 60 presents while arm evals == 0; stop when armed+seen eval or user leaves Present.
- Conflict list unchanged. Release asset name `nvngx.dll.nrpre.addon64` is documentation-only (Angel tags).
- No unit-test harness; verify with `.\check-shaders.ps1` and `.\build.ps1` on Windows CI.

## File map

| File | Role |
|---|---|
| `src/addon.cpp` | Hook method, registry, poke, status, settings, FrameGen/Present |
| `CHANGES.md` | v1.0.2 notes including FrameGen acceptance |
| `README.md` | Hook method / Poke / Waiting guidance |
| `docs/superpowers/specs/2026-09-11-hook-method-wake-retry-design.md` | Living contract |
| `.github/workflows/build.yml` | Unchanged; must stay green |

---

### Task 1: Hook method state, per-arm evals, freeze, resettable snippet init

**Files:**
- Modify: `src/addon.cpp` near `g_placement` (~482), `g_eval_count` (~1046), `eval_dispatch` (~1898), `setup_nr` (~2162), forward decls (~330)

**Interfaces:**
- Produces: `enum HookMethod : int { kHookOff=0, kHookAuto=1, kHookUpscaled=2, kHookFrameGen=3, kHookPresent=4 };`
- Produces: `g_hook_method`, `g_poke_pending`, `g_poke_why`, `g_hooks_frozen`, `g_eval_inflight`, `g_arm_evals`
- Produces: `g_snip_init_done`, `g_snip_init_ok`, `g_snip_init_res` (file-scope, replace function-statics)
- Produces: `hook_method_name(int)`, `request_poke(const char *why)`, `reset_failed_snip_init()`
- Consumes: existing `g_eval_count` (leave for profiler), `t_depth`

- [ ] **Step 1: Add enum + globals after `g_placement`**

```cpp
enum HookMethod : int {
    kHookOff = 0,
    kHookAuto = 1,
    kHookUpscaled = 2,
    kHookFrameGen = 3,
    kHookPresent = 4,
};
static int g_hook_method = kHookAuto;
static bool g_poke_pending = false;
static const char *g_poke_why = "poke";
static volatile LONG g_hooks_frozen = 0;
static volatile LONG g_eval_inflight = 0;
static volatile LONG g_arm_evals = 0;

static const char *hook_method_name(int m) {
    switch (m) {
    case kHookOff:      return "Off";
    case kHookAuto:     return "Auto";
    case kHookUpscaled: return "Upscaled";
    case kHookFrameGen: return "FrameGen";
    case kHookPresent:  return "Present";
    default:            return "?";
    }
}

static void request_poke(const char *why) {
    g_poke_why = why ? why : "poke";
    g_poke_pending = true;
    InterlockedExchange(&g_hooks_frozen, 1);
}
```

- [ ] **Step 2: Replace `setup_nr` function-statics**

File-scope next to other NR state:

```cpp
static bool g_snip_init_done = false;
static bool g_snip_init_ok = false;
static NVSDK_NGX_Result g_snip_init_res = NVSDK_NGX_Result_Fail;

static void reset_failed_snip_init() {
    if (g_nr_handle != nullptr) return;
    if (!g_snip_init_ok) {
        g_snip_init_done = false;
        g_snip_init_res = NVSDK_NGX_Result_Fail;
    }
}
```

In `setup_nr`, use these instead of `static bool s_init_done` / `s_init_res`. Set `g_snip_init_ok = (ri == NVSDK_NGX_Result_Success)` after a first attempt.

- [ ] **Step 3: Count arm evals + inflight in `eval_dispatch`**

At outermost entry (`t_depth == 0`): if frozen or method Off, forward `orig` only. Else increment `g_eval_inflight` and `g_arm_evals`, run body, decrement inflight. Nested re-entry still forwards without incrementing arm evals.

- [ ] **Step 4: Commit**

```bash
git add src/addon.cpp
git commit -m "feat: hook-method state, per-arm evals, resettable snippet init"
```

---

### Task 2: Owned target registry, install/disarm, FrameGen allowlist, Present scan

**Files:**
- Modify: `src/addon.cpp` — `is_framegen_snippet` (~3201), `install_hook` (~3240), `on_present` (~3369)

**Interfaces:**
- Produces: `g_eval_targets[kMaxNgx]`, `g_eval_fg_slot[kMaxNgx]`, `g_eval_hook_count`, `g_create_target`
- Produces: `g_seen_mods[]`, `g_seen_n`, `kPresentScanPeriod = 60`
- Produces: `disarm_hooks()`, `module_wanted()`, `gather_eval_candidates()`, `attach_eval_target()`, `install_hook()`, `tick_hooks()`, `apply_poke()`
- `g_hooked` true only after `g_eval_hook_count > 0` or conflict

- [ ] **Step 1: Path helpers + FrameGen allowlist**

Keep Auto skip of `dlssg`. FrameGen extra: `dlssg` **and** (`streamline` in path or `sl.dlss` / `sl.interposer` filename). Reject bare `nvngx_dlssg.dll`. Duplicate check is by Evaluate **address**. Extra slots set `g_eval_fg_slot[i]`; `eval_dispatch` sets `t_fg_guard`. In `eval_body` / claim site: if `t_fg_guard && cjx==0 && cjy==0`, forward without `claim_frame`.

- [ ] **Step 2: Registry attach/disarm**

`attach_eval_target(target, module, fg_extra)`: if target already owned, reuse; else first free slot, `MH_CreateHook` + `MH_EnableHook`, increment `g_eval_hook_count` only on enable success. `disarm_hooks()`: `MH_DisableHook` + `MH_RemoveHook` every owned Evaluate target and the Create target; clear slots; `g_hooked=false`; `g_eval_hook_count=0`.

- [ ] **Step 3: `install_hook` retryable; `tick_hooks` for Present**

`install_hook`: Off → return. Conflict → latch, no hooks. `MH_Initialize` failure → return without latch. Gather candidates; attach; set `g_hooked` only if `g_eval_hook_count > 0`. Mark enumerated modules seen. Present `tick_hooks`: if method Present and `g_arm_evals==0` and `(present_n % 60)==0`, attach **new** modules only. Stop when hooked and `g_arm_evals>0`, or method ≠ Present.

- [ ] **Step 4: `apply_poke` on present**

If `!g_poke_pending` return. If `g_eval_inflight != 0`, `queue->wait_idle()` then return (stay frozen/pending). Log pre-reset `g_arm_evals`. If Upscaled, `g_placement=1`. `reset_failed_snip_init()`. If handle non-null, `release_state("poke")`; always `g_setup_done=false`; `g_force_reset_frames=8`. Off → `disarm_hooks()`. Else: keep wanted targets, drop unwanted (FrameGen extras when leaving FrameGen), `install_hook` / incremental attach. `g_arm_evals=0`. Clear seen set. Log `poke: method= evals= hooked= conflict= why=`. Clear pending + frozen.

- [ ] **Step 5: Commit**

```bash
git add src/addon.cpp
git commit -m "feat: idempotent NGX hook registry, FrameGen allowlist, Present scan"
```

---

### Task 3: Settings, overlay status, Placement kept, Poke

**Files:**
- Modify: `src/addon.cpp` — `load_settings` (~2422), `save_settings` (~2494), `draw_overlay` (~2576), reset-defaults (~2934)

**Interfaces:**
- Config: `HookMethod` 0..4. Keep `Placement`. Upscaled save forces Placement 1.
- Overlay: conflict first; then Off / looking / waiting / seen / ON. Placement combo unchanged (no poke). Hook method combo requests poke. Poke while `g_nr_enabled`.

- [ ] **Step 1: load/save `HookMethod`**

Clamp 0..4. Missing key → Auto. After load, if Upscaled then `g_placement=1`. Save writes both keys; if Upscaled write Placement 1.

- [ ] **Step 2: Status + Hook method + Poke; keep Placement**

Status uses `g_conflict`, `g_hook_method`, `g_eval_hook_count`, `g_arm_evals`, `g_nr_handle` in spec order. Placement combo only sets `g_placement` + `changed`. Hook method combo sets method, Upscaled forces After, `request_poke("hook-method-changed")`. Poke button: `request_poke("ui")`.

- [ ] **Step 3: Commit**

```bash
git add src/addon.cpp
git commit -m "feat: Hook method overlay, status split, and Poke"
```

---

### Task 4: CHANGES + README for v1.0.2

**Files:**
- Modify: `CHANGES.md`, `README.md`

- [ ] **Step 1: CHANGES.md** — new `## v1.0.2` covering two axes, Poke, status, FrameGen allowlist (not all dlssg), Present incremental scan, release asset rename note.

- [ ] **Step 2: README.md** — Using-it bullets for Hook method, Poke, Waiting/menus, FrameGen cadence note.

- [ ] **Step 3: Commit**

```bash
git add CHANGES.md README.md
git commit -m "docs: v1.0.2 Hook method / Poke / Waiting guidance"
```

---

### Task 5: PR + CI

- [ ] Push `cursor/hook-method-poke-94c8`, open PR to `main` (do not merge/close PR #1).
- [ ] PR body lists each CodeRabbit item and how it was addressed.
- [ ] Wait for `build` / windows job green.
- [ ] Do **not** `gh release create` / tag v1.0.2.

## Spec coverage check

| Spec item | Task |
|---|---|
| Two axes; no Before-upscale method | Tasks 1, 3 |
| Placement change does not re-arm | Task 3 |
| Successful hooks ≠ latch | Task 2 |
| Per-arm evals | Task 1 |
| Quiescent idempotent registry / Off disarms | Task 2 |
| Resettable failed Init_Ext | Task 1 |
| FrameGen allowlist + cadence guard | Task 2 |
| Conflict before Waiting | Task 3 |
| Present incremental scan | Task 2 |
| Poke log fields | Task 2 |
| Config 0..4 + Placement sync | Task 3 |
| CHANGES / README / no Feeder / no tag | Tasks 4–5 |

## Plan self-review

- Names consistent: `request_poke` / `apply_poke` / `disarm_hooks` / `g_hook_method` / `g_eval_hook_count` / `g_arm_evals`.
- HookMethod values are 0..4 (Off, Auto, Upscaled, FrameGen, Present).
- Verification is Windows CI build, matching the repo (no unit harness).
