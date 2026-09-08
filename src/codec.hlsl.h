// Colour codec for DLSS-NR: develops the game's scene-linear HDR buffer into the
// bounded, display-referred image the network expects, and puts the network's
// contribution back onto the original afterwards.
// DLSSNR expects a bounded, display-referred, sRGB-encoded image. The game's
// DLSS colour buffer is scene-referred linear HDR, so it must be normalised to
// paper white and soft-clipped first; the decode then restores the HDR range
// using the original/proxy luminance ratio (the soft clip is not invertible).
//
// Network resolution. The network may run on a smaller image than the game
// renders (NetSize < FullSize). The proxy, the network output and the stored
// delta then live on the NetSize grid; the original and the final image stay on
// the FullSize grid. What crosses between the two is only the network's *change*
// (neural - proxy, in the encoded domain), which is low-frequency and survives a
// bilinear resample; the full-resolution proxy it is added to is recomputed per
// pixel from the original, so no detail is lost to the resample.
static const char *kCodecHLSL = R"HLSL(
Texture2D<float4>   Original : register(t0);
Texture2D<float4>   Proxy    : register(t1);
Texture2D<float4>   Neural   : register(t2);
Texture2D<float4>   MVec     : register(t3);
Texture2D<float>    DepthTex : register(t4);
RWTexture2D<float4> Output   : register(u0);
RWStructuredBuffer<uint> Hist : register(u1);   // 128-bin luminance histogram

cbuffer K : register(b0) {
  uint2 Size;          // the grid this dispatch covers
  float PaperWhiteScale;
  float TransferStrength;
  float ColorStrength;
  uint  HdrMode;
  float Knee;          // shoulder start
  float DeltaClamp;    // cap on the reused NR delta, in encoded units
  float2 MvScale;      // the game's own motion-vector scale
  float  Reproject;    // 0 = sample the delta in place, 1 = follow the motion
  float  DepthReject;  // relative depth mismatch that counts as a disocclusion
  float  StructGate;   // how much local structure must justify the reused effect
  float  EffectStrength;  // how far to carry the network's result, 1 = all of it
  float  ChromaTransfer;  // how much of the network's own colour to adopt
  float  ViewMode;        // 0 result, 1 split, 2 network input, 3 network output
  uint2 NetSize;       // grid the network, proxy and delta live on
  uint2 FullSize;      // grid the game renders on
};

float Luminance(float3 c) { return dot(c, float3(0.212639, 0.715169, 0.072192)); }

float3 SrgbEncode(float3 c) {
  c = saturate(c);
  return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}
float3 SrgbDecode(float3 c) {
  c = saturate(c);
  return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

// Undo whatever the encode applied. In passthrough mode it applied nothing, so
// neither does this: the proxy, the network's answer and the game's own image
// all stay in one domain, which is what keeps the gain below meaningful.
float3 CodecDecode(float3 c) {
  return (HdrMode == 0) ? saturate(c) : SrgbDecode(c);
}

// Diagnostic views, so the colour reconstruction can be judged by eye instead
// of inferred from the result. 2 shows the bounded image the network was
// actually given, as a picture: if it looks too dark the reference white is set
// too high, if the bright areas are flat and washed it is too low. 3 shows the
// network's answer the same way. 1 splits the screen, the game's own frame on
// the left. None of this touches what the network does; it only changes what
// is handed on.
float3 ApplyView(float3 result, float3 original, float3 proxy_enc, float3 neural_enc, uint2 px) {
  const int v = (int)(ViewMode + 0.5);
  if (v == 1 && px.x < Size.x / 2) return original;
  if (v == 2) return CodecDecode(proxy_enc);
  if (v == 3) return CodecDecode(neural_enc);
  return result;
}

// ---- grid helpers ----------------------------------------------------------
bool Scaled() { return any(NetSize != FullSize); }
// Continuous coordinates, texel centres at .5, so a resample lands between texels.
float2 FullToLo(float2 p) { return (p + 0.5) * float2(NetSize) / float2(FullSize) - 0.5; }
float2 LoToFull(float2 q) { return (q + 0.5) * float2(FullSize) / float2(NetSize) - 0.5; }
int2 NearestIn(float2 c, uint2 sz) { return clamp(int2(round(c)), int2(0, 0), int2(sz) - 1); }

float4 Bilinear(Texture2D<float4> t, float2 c, uint2 sz) {
  c = clamp(c, 0.0, float2(sz) - 1.0);
  const int2 b0 = int2(floor(c));
  const int2 b1 = min(b0 + 1, int2(sz) - 1);
  const float2 f = c - float2(b0);
  const float4 t00 = t.Load(int3(b0, 0));
  const float4 t10 = t.Load(int3(b1.x, b0.y, 0));
  const float4 t01 = t.Load(int3(b0.x, b1.y, 0));
  const float4 t11 = t.Load(int3(b1, 0));
  return lerp(lerp(t00, t10, f.x), lerp(t01, t11, f.x), f.y);
}


// ---------------------------------------------------------------------------
// Putting the network's work back onto the game's own image.
//
// The network never saw the scene's real range: it was handed the bounded proxy
// and returned something in that same bounded space. So its output is not the
// image we want -- it knows nothing about what lives above 1.0. What it does
// carry is the *change* it made, proxy -> neural, and that is what has to come
// back across.
//
// Reading that change as a per-pixel gain on luminance and applying it to the
// original keeps the original's range intact. It also keeps colour intact for
// free: scaling r, g and b by the same number does not move a pixel's
// chromaticity, so hue and saturation come through exactly as the game rendered
// them and no separate hue-repair step is needed.
//
// The cost of that is the network's own colour changes are dropped, since they
// are not a luminance gain. ChromaTransfer brings them back on demand, applied
// as a direction at the luminance we already restored, so the range still comes
// from the original and only the hue follows the network.
float3 RestoreRange(float3 original, float3 proxy, float3 neural) {
  const float oy = Luminance(original);
  const float py = Luminance(proxy);
  const float ny = Luminance(neural);
  // Where the proxy is essentially black the gain is meaningless -- a near-zero
  // denominator turns shadow noise into blown pixels -- so leave those alone.
  if (py <= 1e-5 || oy <= 1e-5) return original;

  const float kGainLimit = 8.0;
  const float gain = clamp(ny / py, 1.0 / kGainLimit, kGainLimit);
  float3 lit = original * gain;

  if (ChromaTransfer > 0.0 && ny > 1e-5) {
    const float3 net_dir = neural / ny;          // the network's colour, unit luminance
    lit = lerp(lit, net_dir * Luminance(lit), ChromaTransfer);
  }
  return lerp(original, lit, TransferStrength);
}


// scene-linear HDR -> bounded display-referred sRGB that DLSSNR can consume.
// One pixel's worth, so the full-resolution decode can recompute it in place.
float3 EncodePixel(float3 c) {
  c = max(c, 0.0);
  if (HdrMode == 2) c /= max(PaperWhiteScale, 1e-4);   // normalise to paper white
  if (HdrMode == 0) {
    // Already bounded and display-referred, which is exactly the domain the
    // network works in. Hand it over untouched. Applying a transfer curve the
    // game has already applied would wash the image out, and there is no range
    // above 1.0 here to develop in the first place.
    return saturate(c);
  }
  // Roll off LUMINANCE, not each channel: a per-channel shoulder compresses the
  // bright primaries of a saturated colour and leaves the dark ones alone, which
  // changes the ratio between channels -- that is a hue shift, and the network
  // then bakes it in. Scaling the colour by the luminance ratio keeps hue and
  // relative chroma exactly.
  const float k = clamp(Knee, 0.05, 0.99);
  const float span = 1.0 - k;
  const float y = Luminance(c);
  const float y_out = (y <= k) ? y : (k + span * (1.0 - exp(-(5.770780 * 0.25 / span) * (y - k))));
  c *= y_out / max(y, 1e-6);
  // A very saturated colour can still push a channel past 1.0. Pull it back
  // towards its luminance rather than letting saturate() clip one channel and
  // skew the hue -- but only for genuine overshoot, and only partially: the
  // full-strength version desaturated the whole frame whenever the exposure
  // estimate ran hot.
  const float peak = max(c.r, max(c.g, c.b));
  if (peak > 1.0) {
    const float t = saturate((peak - 1.0) / 3.0) * 0.5;   // gentle, capped at 50%
    c = lerp(c, y_out.xxx, t);
  }
  return SrgbEncode(c);
}

// The encoded proxy at a full-resolution pixel: read it when the proxy lives on
// the same grid, recompute it when the proxy is the network's smaller copy.
float3 ProxyEncAt(int2 q) {
  return Scaled() ? EncodePixel(Original.Load(int3(q, 0)).rgb)
                  : Proxy.Load(int3(q, 0)).rgb;
}

// Runs on the NetSize grid. At full resolution it reads the original 1:1; when
// scaled it resamples the original down to the network's grid.
[numthreads(16, 16, 1)]
void CSEncode(uint3 tid : SV_DispatchThreadID) {
  if (any(tid.xy >= Size)) return;
  const float4 src = Scaled() ? Bilinear(Original, LoToFull(float2(tid.xy)), FullSize)
                              : Original.Load(int3(tid.xy, 0));
  Output[tid.xy] = float4(EncodePixel(src.rgb), src.a);
}

// DLSSNR output -> back to the game's scene-linear HDR range. FullSize grid.
[numthreads(16, 16, 1)]
void CSDecode(uint3 tid : SV_DispatchThreadID) {
  if (any(tid.xy >= Size)) return;
  float4 src = Original.Load(int3(tid.xy, 0));
  const float pw = max(PaperWhiteScale, 1e-4);
  float3 original = max(src.rgb, 0.0);
  float3 proxy_enc, neural_enc;
  if (Scaled()) {
    // Carry the network's change, not its image: the change is low-frequency and
    // resamples cleanly; the full-resolution proxy it lands on is exact.
    const float2 c = FullToLo(float2(tid.xy));
    const float3 p_lo = Bilinear(Proxy,  c, NetSize).rgb;
    const float3 n_lo = saturate(Bilinear(Neural, c, NetSize).rgb);
    proxy_enc  = EncodePixel(original);
    neural_enc = saturate(proxy_enc + (n_lo - p_lo));
  } else {
    proxy_enc = Proxy.Load(int3(tid.xy, 0)).rgb;
    // saturate() is not cosmetic. CSApplyDelta clamps its reconstructed
    // neural_enc to [0,1]; without the same clamp here the two paths disagree
    // wherever the encoded value passes 1.0 -- the highlights, on HDR content.
    // At any cadence above 1 those two paths alternate frame by frame, so a
    // difference that exists on only one of them reads as flicker, on exactly
    // the pixels the eye is drawn to. Clip both or clip neither.
    neural_enc = saturate(Neural.Load(int3(tid.xy, 0)).rgb);
  }
  float3 proxy  = CodecDecode(proxy_enc);
  float3 neural = CodecDecode(neural_enc);
  if (HdrMode == 2) { proxy *= pw; neural *= pw; }
  float3 result = RestoreRange(original, proxy, neural);
  const float luma_only = Luminance(result);
  result = lerp(luma_only.xxx, result, ColorStrength == 0.0 ? 1.0 : ColorStrength);
  // Dial the whole thing back toward the image the game handed us. Done here, at
  // the end, it applies to the real result and the reconstructed one alike.
  result = lerp(original, result, EffectStrength);
  result = ApplyView(result, original, proxy_enc, neural_enc, tid.xy);
  Output[tid.xy] = float4(result, src.a);
}

// --- Cadence support: reuse the network's *effect*, not its output ------------
// On a skipped frame the cached neural image is one frame stale while the motion
// vectors are current, so feeding it produces ghosting. What survives a frame far
// better is the difference the network made: it is low-frequency next to the image
// itself. So store that difference on frames the network runs... (NetSize grid;
// the depth bound here is on the same grid as the proxy.)
[numthreads(16, 16, 1)]
void CSDelta(uint3 tid : SV_DispatchThreadID) {
  if (any(tid.xy >= Size)) return;
  const float3 proxy  = Proxy.Load(int3(tid.xy, 0)).rgb;
  const float3 neural = Neural.Load(int3(tid.xy, 0)).rgb;
  // The alpha channel was spare, so it carries the depth this delta belongs to.
  // Reapplying it later is only valid where that surface is still the one on
  // screen, and this is what lets the apply pass check.
  const float d = DepthTex.Load(int3(tid.xy, 0)).x;
  Output[tid.xy] = float4(neural - proxy, d);     // kept in the encoded domain
}

// ...and on a skipped frame apply it to the *current* proxy. The base is current,
// so geometry and motion stay correct and only the enhancement is a frame old.
// Clamping bounds how wrong it can be where the delta no longer fits the scene,
// such as at a disocclusion or a sudden lighting change.
// What to do on a skipped frame when this pixel's history cannot be reprojected.
// Falling back to the game's own colour costs the pixel the effect for one frame
// while its neighbours keep it, and the next frame hands it back: a
// full-amplitude blink, on exactly the pixels motion has just uncovered. That is
// what the flicker is made of, and it is why it tracks movement. A delta that is
// stale in *position* is far less visible than an effect that switches off, so
// reuse the one stored at this pixel and trust it only as far as the depth it was
// captured at still matches this frame's.
float3 DeltaInPlace(uint2 px) {
  const int2   ql   = Scaled() ? NearestIn(FullToLo(float2(px)), NetSize) : int2(px);
  const float4 here = Neural.Load(int3(ql, 0));
  const float  dcur = DepthTex.Load(int3(px, 0)).x;
  const float  tol  = max(DepthReject, 0.0);
  const float  ref  = max(abs(dcur), 1e-6);
  const float  keep = (tol <= 0.0 || abs(here.a - dcur) / ref < tol) ? 1.0 : 0.5;
  return here.rgb * keep;
}

// FullSize grid. Original, MVec and DepthTex are the game's; Neural holds the
// stored delta on the NetSize grid.
[numthreads(16, 16, 1)]
void CSApplyDelta(uint3 tid : SV_DispatchThreadID) {
  if (any(tid.xy >= Size)) return;
  float4 src = Original.Load(int3(tid.xy, 0));
  const float pw = max(PaperWhiteScale, 1e-4);
  float3 original = max(src.rgb, 0.0);
  const float3 proxy_enc = ProxyEncAt(int2(tid.xy));
  // The delta was measured at last frame's pixel positions, so on a moving camera
  // it sits where the edges *were*: reapplied in place that reads as ghosting.
  // Follow the motion vector back to where this pixel was and take the delta from
  // there. Outside the frame there is no history, so contribute nothing rather
  // than something wrong.
  float3 delta = float3(0.0, 0.0, 0.0);
  if (Reproject != 0.0) {
    const float2 mv = MVec.Load(int3(tid.xy, 0)).xy * MvScale;
    const float2 prev = float2(tid.xy) + mv * Reproject;
    const bool inside = !(any(prev < 0.0) || any(prev >= float2(Size) - 1.0));
    float wsum = 0.0;
    if (inside) {
      // Motion is rarely a whole number of pixels, and the enhancement is a
      // high-frequency signal, so rounding the tap to the nearest texel smears it
      // by up to half a pixel -- which is the very misalignment we are here to fix.
      // The delta lives on the network's grid, so the tap position is mapped there.
      const float2 pl = clamp(Scaled() ? FullToLo(prev) : prev, 0.0, float2(NetSize) - 1.0);
      const int2   b0 = int2(floor(pl));
      const int2   b1 = min(b0 + 1, int2(NetSize) - 1);
      const float2 f  = pl - float2(b0);
      const float4 t00 = Neural.Load(int3(b0, 0));
      const float4 t10 = Neural.Load(int3(b1.x, b0.y, 0));
      const float4 t01 = Neural.Load(int3(b0.x, b1.y, 0));
      const float4 t11 = Neural.Load(int3(b1, 0));

      // A pixel uncovered by something that moved is inside the frame and reprojects
      // to a perfectly valid coordinate -- the delta waiting there just belongs to
      // whatever used to occlude it. Depth is what separates the two: keep only the
      // taps still sitting on this pixel's surface, and reweight what is left.
      const float dcur = DepthTex.Load(int3(tid.xy, 0)).x;
      const float tol  = max(DepthReject, 0.0);
      float4 w = float4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y),
                        (1.0 - f.x) * f.y,         f.x * f.y);
      if (tol > 0.0) {
        const float ref = max(abs(dcur), 1e-6);
        w.x *= (abs(t00.a - dcur) / ref < tol) ? 1.0 : 0.0;
        w.y *= (abs(t10.a - dcur) / ref < tol) ? 1.0 : 0.0;
        w.z *= (abs(t01.a - dcur) / ref < tol) ? 1.0 : 0.0;
        w.w *= (abs(t11.a - dcur) / ref < tol) ? 1.0 : 0.0;
      }
      wsum = w.x + w.y + w.z + w.w;
      if (wsum >= 1e-4) {
      delta = (t00.rgb * w.x + t10.rgb * w.y + t01.rgb * w.z + t11.rgb * w.w) / wsum;

      // Second guard, for what depth alone misses. The effect is edge enhancement,
      // so it can only belong where this frame still has an edge. A strong delta
      // sitting on flat ground is the silhouette of something that has since moved
      // away -- the arc left behind a turning head. Hold the delta to what the
      // local structure of the current frame can account for.
      // Only the doubtful pixels are gated, so the whole neighbourhood scan can be
      // skipped wherever the history checked out -- which is nearly all of them.
      // Luminance is taken in the encoded domain: the delta is stored there too, so
      // the two are directly comparable, and it costs no sRGB decode per tap.
      if (StructGate > 0.0 && wsum < 0.999) {
        const float lc = Luminance(proxy_enc);
        float lo = lc, hi = lc;
        [unroll] for (int dy = -1; dy <= 1; ++dy)
          [unroll] for (int dx = -1; dx <= 1; ++dx) {
            const int2 q = clamp(int2(tid.xy) + int2(dx, dy), int2(0, 0), int2(Size) - 1);
            const float l = Luminance(ProxyEncAt(q));
            lo = min(lo, l); hi = max(hi, l);
          }
        const float allowed = (hi - lo) * StructGate;
        const float mag = max(abs(delta.r), max(abs(delta.g), abs(delta.b)));
        // Only bind where the history is actually suspect. wsum is the share of taps
        // whose depth still matches, so 1-wsum is how disoccluded this pixel is.
        // Gating everywhere would weaken the delta on skipped frames only, and that
        // difference alternates with the frames that run the network -- which reads
        // as flicker. Confining it to the doubtful pixels keeps the rest identical.
        if (mag > allowed) {
          const float gated = allowed / max(mag, 1e-6);
          delta *= lerp(1.0, gated, saturate(1.0 - wsum));
        }
      }
        }
    }
    if (!inside || wsum < 1e-4) delta = DeltaInPlace(tid.xy);
  } else {
    delta = Scaled() ? Bilinear(Neural, FullToLo(float2(tid.xy)), NetSize).rgb
                     : Neural.Load(int3(tid.xy, 0)).rgb;   // Neural slot holds the delta
  }
  const float cap = max(DeltaClamp, 0.0);
  if (cap > 0.0) delta = clamp(delta, -cap, cap);
  const float3 neural_enc = saturate(proxy_enc + delta);

  float3 proxy    = CodecDecode(proxy_enc);
  float3 neural   = CodecDecode(neural_enc);
  if (HdrMode == 2) { proxy *= pw; neural *= pw; }
  float3 result = RestoreRange(original, proxy, neural);
  const float luma_only = Luminance(result);
  result = lerp(luma_only.xxx, result, ColorStrength == 0.0 ? 1.0 : ColorStrength);
  // Dial the whole thing back toward the image the game handed us. Done here, at
  // the end, it applies to the real result and the reconstructed one alike.
  result = lerp(original, result, EffectStrength);
  result = ApplyView(result, original, proxy_enc, neural_enc, tid.xy);
  Output[tid.xy] = float4(result, src.a);
}

// Carry the stored effect forward by exactly one frame.
//
// Reusing a delta that is several frames old means either following one frame of
// motion (it lands short) or multiplying that frame's motion by its age (it lands
// wrong the moment the camera turns, because the multiplication assumes the
// velocity never changed). Both are guesses about frames we did measure and then
// threw away.
//
// So do not let the delta get old. Advect it every frame by the motion of that
// frame, and it is always where it belongs, with one measured step and nothing
// extrapolated. The apply pass then samples it in place. The cost is one more
// resample per frame, which softens the delta slightly -- acceptable for a signal
// this low-frequency, and cheap next to being in the wrong place.
// NetSize grid; MVec and DepthTex are the game's full-resolution textures.
[numthreads(16, 16, 1)]
void CSAdvect(uint3 tid : SV_DispatchThreadID) {
  if (any(tid.xy >= Size)) return;
  const int2   pf   = Scaled() ? NearestIn(LoToFull(float2(tid.xy)), FullSize) : int2(tid.xy);
  const float  dcur = DepthTex.Load(int3(pf, 0)).x;
  float2 mv = MVec.Load(int3(pf, 0)).xy * MvScale;
  if (Scaled()) mv *= float2(NetSize) / float2(FullSize);   // pixels of this grid
  const float2 prev = float2(tid.xy) + mv;          // one frame, never a multiple
  if (any(prev < 0.0) || any(prev >= float2(Size) - 1.0)) {
    Output[tid.xy] = float4(0.0, 0.0, 0.0, dcur);   // came from off-screen: no history
    return;
  }
  const float2 f = frac(prev);
  const int3   b = int3(int2(prev), 0);
  const float4 t00 = Neural.Load(b);
  const float4 t10 = Neural.Load(b + int3(1, 0, 0));
  const float4 t01 = Neural.Load(b + int3(0, 1, 0));
  const float4 t11 = Neural.Load(b + int3(1, 1, 0));
  float4 w = float4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y),
                    (1.0 - f.x) * f.y,         f.x * f.y);
  const float tol = max(DepthReject, 0.0);
  if (tol > 0.0) {
    const float ref = max(abs(dcur), 1e-6);
    w.x *= (abs(t00.a - dcur) / ref < tol) ? 1.0 : 0.0;
    w.y *= (abs(t10.a - dcur) / ref < tol) ? 1.0 : 0.0;
    w.z *= (abs(t01.a - dcur) / ref < tol) ? 1.0 : 0.0;
    w.w *= (abs(t11.a - dcur) / ref < tol) ? 1.0 : 0.0;
  }
  const float wsum = w.x + w.y + w.z + w.w;
  // Nothing here belongs to this surface any more: let the effect fade at a
  // disocclusion rather than drag the occluder's delta along with it.
  const float3 d = (wsum < 1e-4) ? float3(0.0, 0.0, 0.0)
      : (t00.rgb * w.x + t10.rgb * w.y + t01.rgb * w.z + t11.rgb * w.w) / wsum;
  Output[tid.xy] = float4(d, dcur);   // alpha carries the depth it now belongs to
}

// Take a private copy of one of the game's textures.
//
// The network reads Color, Depth and MVec straight from the game. That is fine
// while it runs on the same queue as the render, in order; it stops being fine the
// moment the network runs anywhere else, because the graphics queue is already
// drawing the next frame into those same textures. A copy is what makes the
// network's inputs hold still.
//
// It reads through an SRV rather than CopyResource on purpose: an SRV is the state
// the add-on already relies on these resources being in at this exact point, while
// a copy would need them transitioned to COPY_SOURCE and back -- a state change on
// a resource the game owns and whose real state we are only guessing at.
//
// One kernel serves all three. Writing a float4 into a one- or two-channel typed
// UAV drops the components that do not exist, and depth arrives as .x through the
// same format mapping the codec already does for its own depth reads.
[numthreads(16, 16, 1)]
void CSSnapshot(uint3 tid : SV_DispatchThreadID) {
  if (any(tid.xy >= Size)) return;
  Output[tid.xy] = Original.Load(int3(tid.xy, 0));
}

// The network's guide inputs on its own grid. Depth is point-sampled: a blend of
// two surfaces' depths is a depth that belongs to neither.
[numthreads(16, 16, 1)]
void CSDownsample(uint3 tid : SV_DispatchThreadID) {
  if (any(tid.xy >= Size)) return;
  const int2 pf = NearestIn(LoToFull(float2(tid.xy)), FullSize);
  Output[tid.xy] = Original.Load(int3(pf, 0));
}

// Motion vectors are in pixels of the grid they were rendered on, so a copy onto
// a smaller grid has to shrink them by the same ratio.
[numthreads(16, 16, 1)]
void CSDownsampleMV(uint3 tid : SV_DispatchThreadID) {
  if (any(tid.xy >= Size)) return;
  const int2 pf = NearestIn(LoToFull(float2(tid.xy)), FullSize);
  float4 v = Original.Load(int3(pf, 0));
  v.xy *= float2(NetSize) / float2(FullSize);
  Output[tid.xy] = v;
}

// Luminance histogram of the scene buffer, log2-spaced so it covers HDR range
// with useful resolution down in the shadows. One dispatch over a sparse grid:
// exposure is a global property, so sampling every 4th pixel is plenty.
[numthreads(16, 16, 1)]
void CSHistogram(uint3 tid : SV_DispatchThreadID) {
  const uint2 px = tid.xy * 4;
  if (any(px >= Size)) return;
  const float3 c = max(Original.Load(int3(px, 0)).rgb, 0.0);
  const float y = Luminance(c);
  // map [2^-10, 2^6] to [0, 127]
  const float lg = clamp((log2(max(y, 1e-4)) + 10.0) / 16.0, 0.0, 0.9999);
  InterlockedAdd(Hist[(uint)(lg * 128.0)], 1);

  // Bin 128 counts pixels above display range. If the buffer never exceeds 1.0
  // it is already display-referred and the soft clip would only crush it.
  if (y > 1.0) InterlockedAdd(Hist[128], 1);

  // Bins 129/130: depth convention. Sample the centre of the screen, where
  // geometry is almost always present, and see which end of the range it sits
  // at -- reversed-Z puts near geometry at ~1.0, classic depth at ~0.
  if (all(px == (Size / 2))) {
    const float d = Proxy.Load(int3(px, 0)).r;      // depth is bound as t1 here
    InterlockedAdd(Hist[129], (uint)(saturate(d) * 1000.0));
    InterlockedAdd(Hist[130], 1);
  }
}
)HLSL";
