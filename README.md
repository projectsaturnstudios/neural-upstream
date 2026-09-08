# neural-upstream

A ReShade add-on that runs NVIDIA's DLSS 5 Neural Rendering network **before** the
game's upscaler instead of after it.

The usual placement is at the end of the frame, on the finished image, which
means the network sees every output pixel and costs the same no matter what
upscaling the game is doing. This one hooks the game's own DLSS call, runs the
network on the render-resolution colour buffer it was about to hand over, and
feeds the result back in as the colour input. DLSS then upscales an image the
network has already worked on.

The network enhances, it does not upscale, so running it on the smaller image
costs proportionally less and the upscaler still does the job it was going to do
anyway. Measured on a 3440x1440 output:

| render resolution | network cost per run |
| --- | --- |
| 3440x1440 (no upscaling) | 9.09 ms |
| 1146x480 (Ultra Performance) | 2.26 ms |

Cost does not fall with area alone. There is a fixed cost per invocation of
roughly a millisecond and a half, so the very low settings buy less than the
pixel counts suggest. The overlay measures both terms rather than assuming.

## Credit

This is a fork of **[matiasLombo/neural-upstream](https://github.com/matiasLombo/neural-upstream)**,
which is the original work: the placement, the colour codec, the cadence and
reprojection scheme, and the research in [FINDINGS.md](FINDINGS.md) are all
theirs. MIT licensed, and this fork keeps that licence and copyright unchanged.
What this fork adds is listed in [CHANGES.md](CHANGES.md).

Not affiliated with or endorsed by NVIDIA.

## Requirements

- A game that renders in **DirectX 12** and **already calls DLSS**. The whole
  add-on is a hook on that call, so neither is optional.
- **ReShade with add-on support**, installed as the game's graphics DLL.
- **`nvngx_dlssnr.dll`** in the game folder. Not distributed here.
- A recent RTX card.

## Build

Third-party headers are fetched, not vendored:

```
.\scripts\fetch-deps.ps1
.\build.ps1
```

Needs a MinGW-w64 g++ with C++20 support, either on `PATH` or unpacked into
`.\tools\mingw64`. `build.sh` is upstream's POSIX equivalent.

`.\check-shaders.ps1` compiles every compute kernel with the same compiler the
add-on uses at runtime. Worth running after touching `src/codec.hlsl.h`, because
a shader that fails to compile disables the colour codec silently at runtime
rather than failing the build.

## Install

```
.\install.ps1 -GameFolder "D:\EpicGames\AlanWake2"
```

**The filename is load-bearing.** It has to contain `nvngx.dll` and end in
`.addon64`. The neural runtime gates feature creation on a substring search for
`nvngx.dll` in the calling module's path, and returns `0xBAD00002` for anything
else. `nvngx.dll.nrpre.addon64` satisfies both.

**One neural add-on per game.** This cannot share a game folder with RenoDX's
DLSS 5 add-on or anything else that hooks the same entry points. Both would lay
trampolines over the same bytes, both would initialise the neural runtime, and
both would drive their own feature on the same command list. The game dies at the
second feature creation. This add-on detects the situation, installs nothing and
says so in the log and the overlay, but the fix is to keep one of the two.

## Using it

Everything is in the ReShade overlay, under **NR Pre-Upscale**. Settings are
saved per game in `ReShade.ini` under `[NRPreUpscale]`.

- **Neural Rendering** with a configurable toggle key, F7 by default.
- **Cost** reports measured network time and projects what other render scales
  would cost on this machine.
- **How often the network runs**, from every frame down to every third. Skipped
  frames reuse the stored effect, moved along the game's motion vectors and
  rejected where depth says the surface has changed.
- **Network resolution**, 50% to 100% of the game's render resolution. Only the
  network's contribution is resampled back up, so the game's own detail is not
  softened. Works regardless of what the game's own settings do.
- **Preset** and **Effect strength**, from restrained to deliberately overcooked.
- **Inference passes**, one to three, each reading what the last one wrote.
- **Exposure**, automatic from the game's own exposure buffer or the scene, or by
  hand.
- **Advanced** holds the colour codec, the skipped-frame reconstruction controls,
  logging, and the diagnostic hotkeys, which are off by default.

## What this is not

It is not a preview of shipping DLSS 5, though the gap is narrower than it first
looks. NVIDIA describes the model as working from colour, albedo, lighting and
surface normals, which reads as though the engine hands those over. This runtime
has no input for any of them: there is no such parameter, and no mention of
albedo, normals, roughness or materials anywhere in the binary. It infers them
from the picture, which is also all it gets here.

What a real integration supplies and this add-on does not is the **control mask**,
a per-pixel map of where and how strongly to act, and the **UI layer**, so the
network does not treat the interface as scenery worth embellishing. Both are
parameters the runtime accepts. Neither is set here; the automatic mask flag is
used instead and the interface is left to fend for itself.

The colour space is the other gap. The network wants a bounded, display-referred
image, and the game hands DLSS unbounded scene-linear light. The add-on
manufactures one: normalise to an estimated reference white, roll off the
highlights, apply the display curve. That roll-off is not invertible, so getting
back out is done by measuring the network's change as a per-pixel luminance ratio
and applying it to the original. Three things follow. The network never sees the
real highlights, so bright areas benefit least. Its own colour decisions are
discarded unless chroma transfer is raised, because only the brightness ratio
survives the round trip. And the whole thing rests on the reference white
estimate, which is why exposure has an automatic mode.

The runtime is also a pre-release build. Two of the parameters it advertises, the
scaling ratio and the render preset, already do nothing.

## Licence

MIT. See [LICENSE](LICENSE), which is upstream's and unchanged.

This software contains source code provided by NVIDIA Corporation.
