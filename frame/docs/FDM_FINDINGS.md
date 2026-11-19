# FDM on Adreno 750 / Turnip — probe findings

Measured on 2026-11-20 on the Steam Frame (Turnip Adreno 750, Mesa 26.3.0-devel,
`fragmentDensityMapDynamic = false`, `fragmentDensityMapNonSubsampledImages = true`,
density texel 32x32 to 1024x1024) with `frame/tools/fdm_probe.c`. The probe renders
a full-screen triangle that writes `gl_FragCoord`, attaches an `R8G8_UNORM` density
map through dynamic rendering, reads the target back and measures the footprint of
equal pixels. No validation layer was available, so only observable behaviour is
recorded here.

Build and run:

```sh
glslangValidator -V --target-env vulkan1.3 frame/tools/fdm_probe.vert -o /tmp/fdm_probe.vert.spv   # header is checked in
cc -std=c11 -O2 -o fdm_probe frame/tools/fdm_probe.c -lvulkan -lm
./fdm_probe --presubmit --radial        # see the file header for all options
./fdm_probe --presubmit --flat64 --oversized --fill255   # oversized image placement check
```

## Findings

1. **A density map written in the same submission as the render pass produces an
   all-zero image** (not even the clear colour), for every layout and map tried,
   including an all-255 map. The same map written in an earlier submission renders
   normally. Gaze maps are not affected (the front slot is always an upload the GPU
   has completed), but any map used in the submission that writes it must be avoided.
   `neutralDensity()` therefore waits for its upload fence.
2. **Pipelines with the FDM attachment flag render correctly in a pass without a
   density map.** Neutral maps on non-gaze passes are therefore optional on this
   driver and off by default (`DXVK_GAZE_FDM_NEUTRAL=1` enables them).
3. **Density is applied per GMEM bin, not per map texel.** The footprint is constant
   inside large blocks whose boundaries do not follow the map. For a 2880x1440 RGBA8
   target the image is split into roughly 1536x720 blocks; each block takes the density
   at its centre. A radial map (full density in the middle, 2x and 4x rings outside)
   collapsed to one footprint over the whole image because every block centre was
   outside the inner rings. Smaller blocks (more attachment bytes per pixel, so more
   bins) give finer control, but nothing near the 32 px texel size was observed.
4. **The density image is read by pixel position, not stretched over the whole image.**
   An exact-size map (90x45, texel 32) and a 512x512 image with only the top-left
   90x45 written gave identical footprints in every layout/command-buffer combination.
   The decisive check: a 512x512 image filled with 255 whose written corner is 64
   (`--flat64 --oversized --fill255`) behaved exactly like an exact-size all-64 map
   (coarse everywhere). A stretched mapping would have left most of the image at full
   rate. The patch's oversized slot images (only the top-left `tilesX x tilesY` written)
   are therefore fine. Texel 64 maps (`--texel 64`) and secondary command buffers gave
   the same footprints too.
5. Observed footprints were limited to 2 pixels horizontally in the band test and 2x4 in
   the radial test, never the 4x4 that a density of 64 asks for. In the radial test the
   point the map marks as full density (the "gaze point") was shaded at 2x4 as well,
   because the bin containing it takes its density from the bin centre.
6. The probe target is colour-only and single-sample. Real eye passes (SBS 2496x1248,
   one layer, 1x or 2x MSAA with depth, from the earlier VRS experiment logs) need more
   attachment memory per pixel, so the driver probably uses more, smaller bins than the
   ~1536x720 blocks seen here. That was not measured.
7. `DXVK_GAZE_*` variables reach the Windows process through the kit launcher without an
   allow-list (earlier experiment logs show `DXVK_GAZE_COARSE`, `DXVK_GAZE_DEBUG` and
   `DXVK_GAZE_FAKE` taking effect), so `DXVK_GAZE_FDM` does too.

## Consequence

On this GPU FDM can be enabled and driven from the existing gaze pipeline, but the
effective foveation is far coarser than the VRS path (8x8 texels, true per-draw VRS).
Treat the FDM preset as an experiment for comparing behaviour in a headset, not as a
replacement for the VRS preset. Layered eye passes still keep full density
(`VK_VALVE_fragment_density_map_layered` has not been examined).
