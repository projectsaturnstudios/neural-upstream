# Hook Method Wake/Retry Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship RenoDX-aligned Hook method (Off/Auto/Before upscale/Upscaled/FrameGen/Present) plus always-on Poke so Streamline titles can move Waiting → ON by retargeting/re-arming NGX hooks; release as **v1.0.2**.

**Architecture:** Keep all logic in `src/addon.cpp`. Add `g_hook_method`, a deferred `request_poke()` that runs on present (safe vs mid-evaluate), refactor `install_hook()` to support rescan + optional frame-gen modules, replace Placement combo with Hook method as source of truth (still sync `g_placement` / `Placement` ini), split overlay status strings, document in CHANGES.md, tag release `nvngx.dll.nrpre.addon64`.

**Tech Stack:** C++20 ReShade add-on, MinHook, ImGui (ReShade overlay), D3D12 NGX, Windows CI (`build.yml` → `build.ps1` + `check-shaders.ps1`).

**Spec:** `docs/superpowers/specs/2026-09-11-hook-method-wake-retry-design.md`

## Global Constraints

- Default Hook method = **Auto** (no behavior change until user changes it).
- Never invent EvaluateFeature in v1.0.2 (no Feeder/synth).
- Do not tear down GPU state from inside evaluate; poke defers to **present**.
- Conflict list unchanged (`renodx-dlss5`, etc.); Hook method is for Neural Upstream alone.
- Release asset must be named `nvngx.dll.nrpre.addon64` (filename contains `nvngx.dll`).
- This repo has **no unit-test harness**; verify with `.\check-shaders.ps1`, `.\build.ps1`, and log/manual checklist.

## File map

| File | Role |
|---|---|
| `src/addon.cpp` | Hook method state, poke, install_hook rescan, overlay, settings |
| `CHANGES.md` | v1.0.2 user-facing notes |
| `README.md` | Short Hook method / Waiting guidance |
| `docs/superpowers/specs/2026-09-11-hook-method-wake-retry-design.md` | Spec (already written) |
| `.github/workflows/build.yml` | Unchanged; CI must stay green |

---

### Task 1: Hook method enum + deferred poke core

**Files:**
- Modify: `src/addon.cpp` (near `g_placement` ~482, `g_hooked` ~3182, `install_hook` ~3240, `on_present` ~3369, `release_state` ~3141)

**Interfaces:**
- Produces: `enum HookMethod : int { kHookOff=0, kHookAuto=1, kHookBefore=2, kHookUpscaled=3, kHookFrameGen=4, kHookPresent=5 };`
- Produces: `static int g_hook_method = kHookAuto;`
- Produces: `static bool g_poke_pending = false;`
- Produces: `static const char *hook_method_name(int m);`
- Produces: `static void request_poke(const char *why);` — sets flags only
- Produces: `static void apply_poke();` — runs on present: clear NR latch safely, force-reset, re-arm hooks
- Consumes: existing `release_state`, `g_setup_done`, `g_force_reset_frames`, `g_nr_handle`, `install_hook`

- [ ] **Step 1: Add enum and globals after `g_placement`**

```cpp
// RenoDX-aligned DirectNeuralRenderingHookPoint mapping (see design spec).
enum HookMethod : int {
    kHookOff = 0,
    kHookAuto = 1,
    kHookBefore = 2,
    kHookUpscaled = 3,
    kHookFrameGen = 4,
    kHookPresent = 5,
};
static int  g_hook_method = kHookAuto;
static bool g_poke_pending = false;
static bool g_hooks_armed = false;   // true after at least one successful Evaluate hook

static const char *hook_method_name(int m) {
    switch (m) {
    case kHookOff:      return "Off";
    case kHookAuto:     return "Auto";
    case kHookBefore:   return "Before";
    case kHookUpscaled: return "Upscaled";
    case kHookFrameGen: return "FrameGen";
    case kHookPresent:  return "Present";
    default:            return "?";
    }
}
```

- [ ] **Step 2: Add `request_poke` / `apply_poke` (declarations near other statics; defs before `on_present`)**

```cpp
static void request_poke(const char *why) {
    g_poke_pending = true;
    logf("[NRPRE] poke requested (%s) method=%s evals=%u logged=%u",
         why ? why : "?", hook_method_name(g_hook_method), g_eval_count, g_logged);
}

// Runs only from on_present. Clears create latch and re-arms hooks for g_hook_method.
static void apply_poke();
```

Implement `apply_poke` to:
1. If `g_hook_method == kHookOff`: disable our Evaluate/Create hooks if any (see Task 2), set `g_hooks_armed = false`, leave game DLSS alone, clear pending, return after log.
2. Else: call a new `disarm_hooks()` then set `g_hooked = false` so `install_hook` can run again.
3. Sync placement: if Before → `g_placement = 0`; if Upscaled → `g_placement = 1`; Auto leaves `g_placement` as-is.
4. Clear create latch without mid-evaluate teardown of textures: set `g_setup_done = false`; if handles non-null, set `g_rebuild_pending = true` and rely on existing present teardown / or call `release_state("poke")` **only here on present** (present is the safe thread — OK to call `release_state` from `apply_poke`).
5. `g_force_reset_frames = 8;`
6. Call `install_hook();`
7. Log: `[NRPRE] poke: method=%s evals=%u hooked=%d armed=%d` with `g_hooks_armed`.
8. `g_poke_pending = false;`

- [ ] **Step 3: Call apply at start of `on_present` (after toggle key handling is fine)**

```cpp
if (g_poke_pending)
    apply_poke();
```

For **Present** method only, also:

```cpp
if (g_hook_method == kHookPresent && g_nr_enabled && !g_conflict && g_logged == 0)
    request_poke("present-rescan");
```

Throttle present-rescan: only if `!g_hooks_armed` OR every N presents (e.g. every 120 frames) to avoid spam — use `static unsigned present_n` and `if ((++present_n % 120) == 0)`.

- [ ] **Step 4: Build**

Run (on Windows CI or local MinGW): `.\scripts\fetch-deps.ps1` if needed; `.\check-shaders.ps1`; `.\build.ps1`  
Expected: `built: neural-upstream.addon64  ... bytes`

- [ ] **Step 5: Commit**

```bash
git add src/addon.cpp
git commit -m "$(cat <<'EOF'
feat: add Hook method poke core (deferred to present)

EOF
)"
```

---

### Task 2: Refactor `install_hook` for rescan + FrameGen

**Files:**
- Modify: `src/addon.cpp` — `is_framegen_snippet` (~3201), `install_hook` (~3240), MinHook orig arrays

**Interfaces:**
- Produces: `static void disarm_hooks();`
- Modifies: `install_hook()` to respect `g_hook_method`, optional include of dlssg modules when FrameGen
- Produces: sets `g_hooks_armed = (ok > 0)`

- [ ] **Step 1: Implement `disarm_hooks`**

Track targets you enabled. Simplest approach matching current code: keep `static LPVOID g_eval_targets[kMaxNgx] = {};` and `static LPVOID g_create_target = nullptr;` set when hooks succeed.

```cpp
static LPVOID g_eval_targets[kMaxNgx] = {};
static unsigned g_eval_target_n = 0;
static LPVOID g_create_target = nullptr;

static void disarm_hooks() {
    for (unsigned i = 0; i < g_eval_target_n; ++i) {
        if (g_eval_targets[i])
            MH_DisableHook(g_eval_targets[i]);
        g_eval_targets[i] = nullptr;
        g_orig_eval_n[i] = nullptr;
    }
    g_eval_target_n = 0;
    g_orig_eval = nullptr;
    if (g_create_target) {
        MH_DisableHook(g_create_target);
        g_create_target = nullptr;
        g_orig_create = nullptr;
    }
    g_hooked = false;
    g_hooks_armed = false;
}
```

- [ ] **Step 2: Change `install_hook` early exits**

```cpp
static void install_hook() {
    if (g_hook_method == kHookOff) return;
    if (g_hooked) return;
    if (conflicting_addon_loaded()) {
        g_hooked = true;
        g_conflict = true;
        // existing log...
        return;
    }
    // ... gather candidates ...
    const bool want_fg = (g_hook_method == kHookFrameGen);
    // inside module loop:
    if (!want_fg && is_framegen_snippet(mods[i])) continue;
    // when FrameGen: still skip pure duplicates; prefer including dlssg-named modules
```

When recording a successful Evaluate hook, store `g_eval_targets[g_eval_target_n++] = target;` and set `g_hooks_armed = true` if `ok > 0`.

Same for CreateFeature → `g_create_target = ctarget`.

Keep comment on `is_framegen_snippet` explaining Auto still skips FG; FrameGen includes them deliberately for Streamline wake.

- [ ] **Step 3: Build** (`.\check-shaders.ps1`; `.\build.ps1`) — must pass

- [ ] **Step 4: Commit**

```bash
git add src/addon.cpp
git commit -m "$(cat <<'EOF'
feat: rescanable NGX hooks with FrameGen module include

EOF
)"
```

---

### Task 3: Settings persistence + Placement sync

**Files:**
- Modify: `src/addon.cpp` — `load_settings` (~2422), `save_settings` (~2494)

**Interfaces:**
- Config key: `NRPreUpscale` / `HookMethod` int 0..5
- On load: clamp; if HookMethod is Before/Upscaled sync `g_placement`; if missing HookMethod, derive from Placement (0→Before not Auto — actually default Auto; only set placement from legacy Placement key as today, leave HookMethod Auto)
- On save: write HookMethod; when method is Before/Upscaled write Placement to match

- [ ] **Step 1: load**

```cpp
if (reshade::get_config_value(rt, "NRPreUpscale", "HookMethod", v)) {
    if (v < kHookOff) v = kHookOff;
    if (v > kHookPresent) v = kHookPresent;
    g_hook_method = v;
}
// existing Placement load stays
if (g_hook_method == kHookBefore) g_placement = 0;
else if (g_hook_method == kHookUpscaled) g_placement = 1;
```

- [ ] **Step 2: save**

```cpp
reshade::set_config_value(rt, "NRPreUpscale", "HookMethod", g_hook_method);
if (g_hook_method == kHookBefore) g_placement = 0;
else if (g_hook_method == kHookUpscaled) g_placement = 1;
reshade::set_config_value(rt, "NRPreUpscale", "Placement", g_placement);
```

- [ ] **Step 3: Build + commit**

```bash
git add src/addon.cpp
git commit -m "$(cat <<'EOF'
feat: persist HookMethod and sync Placement

EOF
)"
```

---

### Task 4: Overlay — status strings, Hook method combo, Poke

**Files:**
- Modify: `src/addon.cpp` — `draw_overlay` (~2576)

**Interfaces:**
- Consumes: `g_hook_method`, `g_hooks_armed`, `g_logged`, `g_eval_count`, `g_nr_handle`, `request_poke`
- Removes Placement combo as primary control (spec: Hook method owns before/after). Keep Placement only if Hook method is Auto (optional advanced) — **YAGNI:** remove Placement combo; Hook method Before/Upscaled set placement.

- [ ] **Step 1: Replace status block**

```cpp
ImGui::SameLine();
if (g_hook_method == kHookOff) {
    ImGui::TextColored(kOff, "OFF (hook method)");
} else if (g_nr_enabled && g_nr_handle != nullptr) {
    ImGui::TextColored(kOn, "ON");
} else if (g_nr_enabled && !g_hooks_armed) {
    ImGui::TextColored(kWarn, "ON (looking for NGX / DLSS modules...)");
} else if (g_nr_enabled && g_logged == 0) {
    ImGui::TextColored(kWarn, "ON (waiting for the game to call DLSS — menus often don't; try in-world or change Hook method / Poke)");
} else if (g_nr_enabled) {
    ImGui::TextColored(kWarn, "ON (DLSS seen; neural feature not created — Poke to retry)");
} else {
    ImGui::TextColored(kOff, "OFF");
}
```

Use `g_logged == 0` for “no evaluate diagnostics yet” (existing counter). Optionally also show `g_eval_count` in Advanced later — not required.

- [ ] **Step 2: Replace Placement combo with Hook method + Poke**

```cpp
{
    const char *methods[] = {
        "Off", "Auto", "Before upscale", "Upscaled", "FrameGen", "Present"
    };
    int hm = g_hook_method;
    if (hm < 0 || hm > 5) hm = kHookAuto;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    if (ImGui::Combo("Hook method", &hm, methods, 6)) {
        g_hook_method = hm;
        if (hm == kHookBefore) g_placement = 0;
        else if (hm == kHookUpscaled) g_placement = 1;
        changed = true;
        request_poke("hook-method-changed");
    }
    ImGui::SetItemTooltip(
        "Where to attach to NGX, aligned with RenoDX Hook method. "
        "Changing this re-arms hooks (Poke). Auto = current behavior. "
        "Upscaled = after upscale. FrameGen includes DLSS-G modules. "
        "Present keeps rescanning for late Streamline loads.");
}
if (g_nr_enabled && !g_conflict) {
    if (ImGui::Button("Poke"))
        request_poke("ui");
    ImGui::SetItemTooltip(
        "Re-arm NGX hooks for the selected Hook method and clear the neural create latch. "
        "Does not invent DLSS if the game never evaluates.");
    ImGui::TextDisabled("Poke re-arms hooks; it does not invent DLSS calls.");
}
```

Place Hook method + Poke near the master switch (where Placement was), bottom-of-panel is also fine — spec said bottom; put **after Cost section / before Look** or at end before footer. Prefer **right after master switch** so Waiting users see it immediately.

- [ ] **Step 3: Build + commit**

```bash
git add src/addon.cpp
git commit -m "$(cat <<'EOF'
feat: Hook method overlay, status split, and Poke button

EOF
)"
```

---

### Task 5: Docs + version notes for v1.0.2

**Files:**
- Modify: `CHANGES.md` (top section)
- Modify: `README.md` (Using it / troubleshooting Waiting)

- [ ] **Step 1: CHANGES.md** — add at top:

```markdown
## v1.0.2

**Hook method.** Overlay control aligned with RenoDX's Hook method
(`Off` / `Auto` / `Before upscale` / `Upscaled` / `FrameGen` / `Present`).
Changing it (or pressing **Poke**) re-arms NGX Evaluate/Create hooks and
clears the neural create latch on the next present. Status text now
distinguishes "looking for modules", "waiting for the game to call DLSS",
and "DLSS seen but neural feature not created".

FrameGen includes modules Auto skips (`dlssg` in the path) for Streamline
titles that only evaluate on that path. Present periodically re-scans while
no evaluate has been seen yet. Neither invents DLSS; games that never call
it still need Feeder/bridge (not in this release).
```

- [ ] **Step 2: README** — under Using it, short bullet:

```markdown
- **Hook method** and **Poke** (overlay): if status stays on waiting after boot,
  try Upscaled / FrameGen / Present or press Poke. Some Streamline games only
  start DLSS evaluates after a path change (same idea as RenoDX's Hook method).
  Menus often never call DLSS — enter gameplay first (title-dependent).
```

- [ ] **Step 3: Commit**

```bash
git add CHANGES.md README.md
git commit -m "$(cat <<'EOF'
docs: Hook method / Poke notes for v1.0.2

EOF
)"
```

---

### Task 6: CI green + GitHub release v1.0.2

**Files:** none (process)

- [ ] **Step 1:** Open/push PR for the feature branch; wait for `build` workflow **windows** job SUCCESS (shaders + `build.ps1`).

- [ ] **Step 2:** Merge to `main`.

- [ ] **Step 3:** Download CI artifact `neural-upstream.addon64` from the main build run (or build locally).

- [ ] **Step 4:** Rename to `nvngx.dll.nrpre.addon64` (do not leave the CI name on the release).

- [ ] **Step 5:** Publish release:

```bash
gh release create v1.0.2 nvngx.dll.nrpre.addon64 \
  --repo projectsaturnstudios/neural-upstream \
  --title "v1.0.2" \
  --notes "$(cat <<'EOF'
Hook method (Off/Auto/Before upscale/Upscaled/FrameGen/Present) and Poke re-arm NGX hooks / clear the neural create latch — for Streamline titles stuck on Waiting (same idea as RenoDX Hook method).

Copy `nvngx.dll.nrpre.addon64` into the game folder next to the executable. Do not rename it.

Requires ReShade with add-on support and `nvngx_dlssnr.dll` in the game folder (not included).
EOF
)"
```

- [ ] **Step 6:** Manual smoke checklist (document results in PR or Discord):

1. AW2 or Control, Hook method Auto — still works as v1.0.1.  
2. Force Waiting (menu): status shows waiting-for-call text; Poke stays enabled; log has `poke:`.  
3. On a Streamline title if available: cycle Hook method; confirm `poke:` lines and eventually ON when evaluates start.  

---

## Spec coverage check

| Spec item | Task |
|---|---|
| Hook method UI values | Task 4 |
| Poke always on while waiting | Task 4 |
| Status string split | Task 4 |
| Re-arm + clear latch + force-reset + log | Tasks 1–2 |
| Before/Upscaled → placement | Tasks 1, 3, 4 |
| FrameGen includes dlssg modules | Task 2 |
| Present rescan while no evals | Task 1 |
| Config HookMethod + Placement sync | Task 3 |
| Defer work to present | Task 1 |
| No synth / Feeder | Global + Task 6 notes |
| v1.0.2 release asset naming | Task 6 |

## Plan self-review

- No TBD/TODO placeholders in steps.  
- Names consistent: `request_poke` / `apply_poke` / `disarm_hooks` / `g_hook_method` / `g_hooks_armed`.  
- Verification uses real project commands (`check-shaders.ps1`, `build.ps1`, `gh release create`).  
