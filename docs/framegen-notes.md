# Frame generation (FSR 3): what the game gives us

Research notes for a possible FSR 3 frame generation. Nothing here changes the game yet. Measured on 2026-11-11 with
the recorded `eksplorasi` scene (HQ helipad, test install): `bench.ps1 -Scene eksplorasi` with `LARECOMP_DEBUG_FRAME`,
`LARECOMP_DEBUG_FRAMES=2`, `LARECOMP_DEBUG_DATA=1` and `LARECOMP_SAVE_MSL=1` (translated GLSL in `build/game/build/glsl`).

## Passes of a gameplay frame (1920x1080)

| Order | Target | Draws | What it is |
|---|---|---|---|
| 1 | depth only, D24S8 1920x1080 | ~340 | depth prepass |
| 2 | R11G11B10F + depth | ~150 | first colour pass |
| 3 | R32 1920x1080 | 1 | linear depth (fs `72b22f7900`) |
| 4 | depth only | ~190 | (shadow/other depth work) |
| 5 | **2x RGBA8 (MRT)** | 1 | **camera velocity** into target 0, an occlusion-like term into target 1 (vs `72b202e200`, fs `72b202f100`) |
| 6 | R11G11B10F + depth | ~380 | main forward pass |
| 7 | **RGBA8 + depth** | ~100-150 | **object velocity** for moving meshes, depth tested over pass 5's result (vs `72b2032c00`, fs `72b2033a00`) |
| 8 | R11G11B10F, 960x540 ... 60x33 | ~30 | transparents, bloom chain |
| 9 | RGBA8 1920x1080 | 2 | tonemap / final composite |
| 10 | RGBA8 1920x1080 | 100-300 | **HUD**, drawn last |

Notes on reading the dump: `vk_end_pass` saves `st.rt.color[0]` when the render target changes. At that moment `st.rt`
is already the **next** pass's target. `pass_N.png` is therefore the next target's content before it is drawn. For
example, `pass_020` shows the velocity target after pass 5 and before pass 7.

## The velocity buffer: a motion blur buffer, not exact motion vectors

The format is RGBA8 UNORM. R is horizontal motion and G is vertical motion, both centred on 0.5. B and A are 0. The
target is cleared to (0.5, 0.5).

- **Camera (pass 5, fullscreen).** The shader reads linear depth, rebuilds the position, reprojects it with the
  previous frame's matrices, and writes:
  `0.5 + 2 * K * saturate(depth * 0.01)^2 * (ndc_now - ndc_prev)`.
  K is `cb6[0][0]`, a strength; its value could not be logged, because fcb6 does not appear in the
  `LARECOMP_DEBUG_DATA` output. The `saturate(depth/100)^2` factor weakens camera motion for everything nearer than
  100 units: near ground has almost none. This is a motion blur design choice.
- **Objects (pass 7, per mesh).** The vertex shader computes the current and previous clip positions. The previous ones
  use `cb6`, which holds the previous matrices and bones. It writes:
  `0.5 + 10 * clamp(ndc_now - ndc_prev - 0.001, -1, 1)`.
  The UNORM target then clips this to +-0.05 NDC, which is about +-48 px horizontally and +-27 px vertically at 1080p.
  The step size is 1/2550 NDC, about 0.38 px horizontally.
- **No jitter.** The game has no TAA.
- **Measured.** With the camera still and the player animating, the ground is at (128,127) and the character's limbs
  range from 90 to 152. With only the camera turning, the background shifts to R 111-118, and the silhouettes of
  railings and buildings are visible.

## What this means for FSR 3

FSR 3 frame interpolation needs exact screen-space motion vectors, depth, and the image without the HUD.

- **Depth.** Available: D24S8 1920x1080, plus linear depth in R32.
- **Image without the HUD.** Available: the HUD is the last pass, in its own run of draws on the RGBA8 target.
- **Motion vectors.** The game buffer as stored is **not usable directly**. Near camera motion is attenuated, the
  strength is scaled, the values are clipped at about 48 px, and the precision is 8 bit. Using it as-is would ghost
  near the player.
- **Usable route.** The game already computes everything that is needed: exact previous positions for skinned and moving
  meshes, and the previous camera matrices. The port could patch the two shaders by hash, as it already does for other
  effects, so that they write the exact, unattenuated, unclamped `ndc_now - ndc_prev` to an extra RG16F target. The game's
  own RGBA8 output would stay as it is, so motion blur keeps working.
  - The camera pass can drop the `saturate(depth/100)^2 * K` factor in the copy.
  - The object pass can drop the bias, the `x10` and the clamp in the copy.

## Implemented: exact motion vectors (AC_MOTION_VECTORS=1)

`las/nvn/vulkan/exec.c`, section "motion vectors". It is off by default, so normal play is unchanged.

- **Patched programs.** When it is on, the two velocity programs are recognised by stage hash: camera fs
  `331abe0da603979f`; object vs `193cf583896ed056` with fs `6185e39f4c367054`. Their translated GLSL is patched before
  it is compiled (`mv_patch_glsl`):
  - The camera fs writes `vec4(r2, r1, 0, 1)`, which is now minus previous, to `layout(location = 3)`. It is taken
    before the game's depth falloff and strength.
  - The object vs passes its `ndc_now - ndc_prev` in a new varying, taken before the bias, the x10 and the clamp. The
    object fs writes that varying to location 3.
  - The patch matches lines of the translation. If a line is not found, the program is left unpatched, and the log says
    `[mv] ... NOT patched`.
- **Colour target 3.** This slot is unused in both passes. Here it is `mv_img`, a port-owned RG16F image the size of
  the game's velocity texture.
  - The image is attached in `begin_pass` whenever the game's velocity texture is target 0. That texture is learned
    from the pass a velocity program begins.
  - Only the patched programs get a write mask on it: R and G. Other draws in those passes leave it alone.
  - The game's RGBA8 buffer, and so its motion blur, is unchanged.
- **Units.** `mv_img` holds `ndc_now - ndc_prev`, with NDC y pointing up. In pixels of the image this is
  `dx_px = mv.x * w/2`, and `dy_px = -mv.y * h/2` in image coordinates, where y points down. The previous position of a
  pixel is `p - (dx_px, dy_px)`.
- **Test dump.** `AC_MV_DUMP=<frame>,...` writes `frames/mv_<frame>.png` and `frames/mv_<frame>.bin` (u32 w, h, then
  f32 dx, dy pixels per pixel), and puts statistics in the log.

### Verification (2026-11-11, `eksplorasi` scene)

The test warps frame N-1 by the motion vectors and compares the result with frame N, using the mean absolute luma error
over the middle 3/4 of the screen. A correct vector field should bring the error well below the unwarped error.

| MV of present | Frames | No warp | Warped (scale 1, y flipped) |
|---|---|---|---|
| 4140 (camera turning) | 4140 -> 4141 | 13.4 | **4.8** |
| 4141 (camera turning) | 4141 -> 4142 | 22.0 | **9.2** |
| 5000 (walking) | 5000 -> 5001 | 11.3 | **5.2** |

Other scales (0.5, 1.5, 2) and an unflipped y are all worse. The motion vectors read back at the present of frame N
belong to the frame shown at N+1: the game has already sent the next frame's rendering when it presents. Frame
generation must pair them the same way, with the motion vectors and depth of the frame being shown.

Cost: none measurable. In the `eksplorasi` scene, alternating runs gave 63.8 and 66.0 fps with the vectors on, and 63.3
and 64.2 fps with them off.

Open points:
- the value of K;
- whether other velocity shaders exist (cutscenes, other areas);
- disocclusion and particles: transparent effects have no motion vectors;
- the effort of integrating FSR 3's Vulkan frame interpolation into the port's present path. It needs the image before
  the HUD, `mv_img` (negated and in FSR's units) and depth, from the same frame.
