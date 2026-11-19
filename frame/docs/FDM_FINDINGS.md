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
4. Exact-size, oversized (512x512, top-left written), 32 px and 64 px texel maps, inline
   and secondary command buffers, `GENERAL` and `FRAGMENT_DENSITY_MAP_OPTIMAL_EXT`
   layouts all gave identical footprints, so the slot-image strategy of the patch is not
   a problem.
5. Observed footprints were limited to 2 pixels horizontally in the band test and 2x4 in
   the radial test, never the 4x4 that a density of 64 asks for.

## Consequence

On this GPU FDM can be enabled and driven from the existing gaze pipeline, but the
effective foveation is far coarser than the VRS path (8x8 texels, true per-draw VRS).
Treat the FDM preset as an experiment for comparing behaviour in a headset, not as a
replacement for the VRS preset. Layered eye passes still keep full density
(`VK_VALVE_fragment_density_map_layered` has not been examined).
