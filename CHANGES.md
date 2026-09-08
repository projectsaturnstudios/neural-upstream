# Changes in this fork

Everything below is on top of [matiasLombo/neural-upstream](https://github.com/matiasLombo/neural-upstream).
Upstream's own research notes are in [FINDINGS.md](FINDINGS.md) and still apply.

## Fixes

**Tearing down GPU state from inside the evaluate hook crashed the game.**
When the render resolution changed, the add-on released the neural feature, its
textures, the descriptor heap and its shaders immediately, from the game's render
thread, with frames still in flight reading them. Worse, the function then
carried on for another hundred lines using the freed pointers. Anything that
notices a change now only raises a flag; the teardown happens at frame present
after the command queue has drained, and the frame that noticed is handed to the
game untouched. Reproduced by changing Resolution Scaling in Silent Hill 2.

**Games that declare no render sub-rectangle were never started.** The add-on
required `DLSS.Render.Subrect.Dimensions` before it would create the network.
Control declares none at all, so the add-on loaded, hooked, and sat idle forever.
It now falls back to the dimensions of the colour texture the game hands DLSS,
which is already the render resolution.

**The high-dynamic-range check could never answer "no".** One variable served as
both the starting assumption and the proven-true latch, so nothing could clear
it and the branch for a display-referred buffer was unreachable. It is now
decided by the colour buffer's pixel format, which cannot lie about whether
values above one are representable. That path then re-applied a transfer curve
the game had already applied; it now genuinely passes through.

**The depth and highlight guides were only derived with automatic exposure on.**
They are needed for correctness, not just for exposure, and games differ on which
way depth runs. They are now derived either way.

**A second neural add-on in the same folder crashed the game.** Both lay
trampolines over the same NGX entry points, both initialise the neural runtime,
and both drive a feature on the same command list while each saves and restores
the compute state the other is changing. The add-on now detects a competing
add-on before hooking anything, installs nothing, explains itself in the log and
in the overlay, and leaves the game untouched. Reproduced in Alan Wake 2 against
`renodx-dlss5.addon64`.

## Features

**Network resolution.** The network runs on a copy of the frame scaled down by a
chosen fraction, and only its contribution is resampled back up; the
full-resolution proxy it lands on is recomputed per pixel, so the game's own
detail is never resampled. Cost falls with area. Upstream's `ScalingRatio` knob
was measured inert, so this is done here rather than asked of the runtime.

**Inference passes.** One to three runs of the network per frame, each reading
what the last one wrote. There was a hidden debug counter for this that called
evaluate repeatedly with identical inputs, which cost N times as much and changed
nothing; passes now ping-pong through a second output texture so the effect
actually compounds.

**Style.** `DLSSNR.Style` selects between two looks the network itself carries.
Upstream set it to 0 and never exposed it, so the add-on was permanently on one
of them. It is now a control. RenoDX's add-on labels the two **Natural** (0) and
**Cinematic** (1) and defaults to Cinematic, which accounts for part of the
difference between the two add-ons at matching slider values.

**Diagnostic views.** A View control hands the game the image the network was
given, or the network's answer, or a split with the game's own frame on the
left, instead of the result. This is how the colour reconstruction gets judged
by eye: if the network's input looks too dark the reference white is too high,
if its bright areas are flat it is too low.

**Reference white that reaches other games.** Automatic exposure is on by
default, prefers the exposure texture where the game supplies one, and the
texture is now interpreted against the pre-exposure and exposure-scale scalars
DLSS defines rather than assumed to be raw. The manual slider and every internal
clamp were bounded at 4.0, which Alan Wake 2 sits above on every frame; they now
reach 32 on a logarithmic scale.

**Defaults aligned with RenoDX's add-on.** Strengths at 1.0, skin following
structure, automatic mask on, Cinematic style, automatic exposure on. Upstream
shipped local tone at 0.15 and local structure at 0.70 while calling its own
"Reference" preset 1.0 across, so a fresh install did not match its own preset.
Network resolution is a dropdown of fixed steps rather than a slider, because
every distinct value rebuilds the network.

**A rebuilt overlay.** A configurable toggle key, the diagnostic hotkeys off by
default so they cannot collide with the game's own bindings, and a cost panel
that reports measured network time and projects what other render scales would
cost. The projection fits fixed and per-pixel terms from measurements at
different sizes rather than assuming cost scales with area, which understated the
small sizes by about half.

**Diagnostics separated from measurement.** The GPU profiler now feeds the
overlay whether or not logging is enabled.

## Tooling

- `scripts/fetch-deps.ps1` fetches the third-party headers, with ImGui pinned to
  the version ReShade's overlay header requires.
- `build.ps1` and `install.ps1` for Windows without a POSIX shell.
- `check-shaders.ps1` compiles every compute kernel with the same compiler the
  add-on uses at runtime, so a shader error is caught at build time instead of
  silently disabling the colour codec in-game.
- `tools-drs-dump.ps1` dumps the NVIDIA driver profile for an executable through
  NvAPI, which is how the "is a driver override forcing this?" question gets
  answered from evidence rather than from the NVIDIA App's own display.
