// SPDX-License-Identifier: Zlib
// Copyright (c) 2026 ji10me
#include "frame_foveation_map.h"
// Historical reference is used only as a regression oracle, never by the new
// module. Its provenance/license are recorded in frame/PROVENANCE.md.
#include "gaze_map_upstream.h"
#include <cassert>
#include <cstdio>
#include <limits>
#include <random>

static void compare(const frame_dfr::Layout &layout, const frame_dfr::Focus &focus) {
  dxvk::GazeMapRequest r;
  r.shape = {layout.width, layout.height, layout.layers, 97, 2};
  r.texel = {layout.tile_width, layout.tile_height};
  r.fx = focus.u; r.fy = focus.v;
  r.r1 = focus.inner_radius; r.r2 = focus.outer_radius;
  r.fullArea = focus.inner_percent; r.midArea = focus.outer_percent;
  r.coarse = focus.peripheral_rate;
  dxvk::GazeMap reference;
  dxvk::generateGazeMap(r, reference);
  std::vector<uint8_t> actual;
  assert(frame_dfr::build_map(layout, focus, actual));
  if (actual != reference.data) {
    std::fprintf(stderr, "Mismatch: %u %u %u tile %u %u, gaze %.9g %.9g %.9g %.9g, radii %.9g %.9g, area %.9g %.9g, rate %u\n",
        layout.width, layout.height, layout.layers, layout.tile_width, layout.tile_height,
        focus.u[0],focus.u[1],focus.v[0],focus.v[1],focus.inner_radius,focus.outer_radius,
        focus.inner_percent,focus.outer_percent,focus.peripheral_rate);
    for (size_t i=0;i<actual.size();++i) if (actual[i]!=reference.data[i]) {
      std::fprintf(stderr,"First mismatch at %zu: %u / %u\n",i,unsigned(actual[i]),unsigned(reference.data[i]));break;
    }
    assert(actual == reference.data);
  }
  auto address = actual.data();
  std::fill(actual.begin(), actual.end(), 255);
  assert(frame_dfr::build_map(layout, focus, actual));
  assert(actual == reference.data && actual.data() == address);
}

int main() {
  std::mt19937 rng(20261105);
  std::uniform_real_distribution<float> uv(-.3f, 1.3f);
  for (unsigned i = 0; i < 3500; ++i) {
    frame_dfr::Layout shape{uint32_t(1 + rng()%320), uint32_t(1 + rng()%240), uint32_t(1 + rng()%2),
                            uint32_t(1 + rng()%24), uint32_t(1 + rng()%24)};
    frame_dfr::Focus f;
    f.u = {uv(rng), uv(rng)}; f.v = {uv(rng), uv(rng)};
    f.inner_radius = uv(rng); f.outer_radius = uv(rng);
    f.peripheral_rate = rng()%12;
    if (i % 3) { f.inner_percent = float(rng()%101); f.outer_percent = float(rng()%101); }
    compare(shape, f);
  }
  for (auto layers : {1u, 2u}) for (auto tile : {4u, 8u, 16u})
    for (auto rate : {0u, 1u, 5u, 6u, 9u, 10u}) for (float u : {0.f, .5f, 1.f}) {
      frame_dfr::Focus f;
      f.u = {u, 1-u}; f.v = {1-u, u}; f.inner_percent = 70; f.outer_percent = 85;
      f.peripheral_rate = rate;
      compare({layers == 1 ? 2880u : 1440u, 1440, layers, tile, tile}, f);
    }
  // Exact circle boundaries, neighbouring representable radii, odd SBS seam,
  // tiny eyes, very wide/tall maps, partial tiles and four-byte layer padding.
  for (const auto shape : {frame_dfr::Layout{161, 81, 1, 8, 8},
                           {1, 1024, 1, 1, 1}, {65536, 1, 2, 1, 1},
                           {1, 65536, 2, 1, 1}, {347, 693, 2, 17, 9}}) {
    frame_dfr::Focus f;
    f.u = {.5f, .5f}; f.v = {.5f, .5f}; f.peripheral_rate = 10;
    const float ew = float(shape.width) / (shape.layers == 1 ? 2.f : 1.f);
    const float scale = std::min(ew, float(shape.height));
    for (unsigned col = 0; col < 12; ++col) {
      const float px = (float(col) + .5f) * float(shape.tile_width);
      const float py = .5f * float(shape.tile_height);
      const float edge = std::hypot(px - ew*.5f, py - float(shape.height)*.5f) / scale;
      for (float r : {edge, std::nextafter(edge, 0.f),
                     std::nextafter(edge, std::numeric_limits<float>::infinity())}) {
        f.inner_radius = r; f.outer_radius = r;
        compare(shape, f);
      }
    }
  }
  frame_dfr::Focus f; f.u = {.5f,.5f}; f.v = {.5f,.5f};
  std::vector<uint8_t> bytes{1,2,3};
  for (const auto invalid : {frame_dfr::Layout{0, 1440, 1, 8, 8},
                             {1440, 1440, 3, 8, 8}, {1440,1440,1,0,8},
                             {1048577,1,1,8,8}, {8192,8192,2,1,1}}) {
    assert(!frame_dfr::build_map(invalid, f, bytes) && bytes.empty());
  }
  f.u[0] = std::numeric_limits<float>::quiet_NaN();
  assert(!frame_dfr::build_map({2880,1440,1,8,8}, f, bytes) && bytes.empty());
  f.u[0] = .5f; f.inner_percent = std::numeric_limits<float>::infinity();
  assert(!frame_dfr::build_map({2880,1440,1,8,8}, f, bytes) && bytes.empty());

  // Density conversion: 2 bytes per tile, red = horizontal, green = vertical,
  // per-layer four-byte padding, exact LUT for the 1x1/2x2/4x4/2x4/4x2 codes.
  {
    frame_dfr::Focus df; df.u = {.5f, .5f}; df.v = {.5f, .5f};
    df.inner_percent = 30; df.outer_percent = 60; df.peripheral_rate = 10;
    for (const auto shape : {frame_dfr::Layout{2880, 1440, 1, 32, 32},
                             {161, 81, 1, 32, 32}, {347, 693, 2, 17, 9}}) {
      std::vector<uint8_t> rates, density;
      assert(frame_dfr::build_map(shape, df, rates));
      assert(frame_dfr::to_density(shape, rates, density));
      const size_t tiles = size_t((shape.width + shape.tile_width - 1) / shape.tile_width)
                         * ((shape.height + shape.tile_height - 1) / shape.tile_height);
      const size_t from = (tiles + 3) & ~size_t(3), to = (tiles * 2 + 3) & ~size_t(3);
      assert(density.size() == to * shape.layers);
      for (uint32_t layer = 0; layer < shape.layers; ++layer) {
        for (size_t t = 0; t < tiles; ++t) {
          const uint8_t code = rates[layer * from + t];
          const uint8_t dx = density[layer * to + t * 2], dy = density[layer * to + t * 2 + 1];
          if (code == 0) assert(dx == 255 && dy == 255);
          if (code == 5) assert(dx == 128 && dy == 128);
          if (code == 10) assert(dx == 64 && dy == 64);
          if (code == 6) assert(dx == 128 && dy == 64);
          if (code == 9) assert(dx == 64 && dy == 128);
        }
        for (size_t t = tiles * 2; t < to; ++t) assert(density[layer * to + t] == 0);
      }
    }
    // The centre must be full density and the periphery 4x4 (64) for a centred gaze.
    frame_dfr::Layout centred{640, 640, 2, 32, 32};
    std::vector<uint8_t> rates, density;
    assert(frame_dfr::build_map(centred, df, rates));
    assert(frame_dfr::to_density(centred, rates, density));
    const size_t stride = (400 * 2 + 3) & ~size_t(3);  // 20 x 20 tiles per layer
    assert(density[(10 * 20 + 10) * 2] == 255 && density[(10 * 20 + 10) * 2 + 1] == 255);
    assert(density[0] == 64 && density[stride - 1 - (stride - 800)] == 64);
    // Size mismatch and invalid layouts leave an empty result.
    std::vector<uint8_t> bad{1, 2, 3}, out{9};
    assert(!frame_dfr::to_density(centred, bad, out) && out.empty());
    assert(!frame_dfr::to_density({0, 1, 1, 8, 8}, bad, out) && out.empty());
    assert(!frame_dfr::to_density({64, 64, 3, 8, 8}, bad, out) && out.empty());
  }
}
