#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace ares::Stereo {

// Off-axis stereo: x_eye = x + separation * (1 - convergence / camera_z).
// The caller supplies inverse camera depth in its projection's units.
struct Settings {
  bool enabled = false;
  bool swapEyes = false;
  bool fullWidth = false;
  unsigned depth = 20;
  unsigned convergence = 10;

  auto separation(unsigned width, unsigned eye) const -> double {
    return width * std::min(depth, 100u) * 0.0003 * ((eye ^ unsigned(swapEyes)) ? 1.0 : -1.0);
  }

  auto offset(double inverseDepth, unsigned width, unsigned eye) const -> int {
    if(!std::isfinite(inverseDepth) || inverseDepth <= 0.0) return 0;
    auto shift = separation(width, eye) * (1.0 - std::min(convergence, 100u) * inverseDepth);
    return int(std::lround(std::clamp(shift, -double(width) * 0.1, double(width) * 0.1)));
  }
};

// PS1 GPU packets retain only integer screen coordinates. Match them to recent
// GTE projections, rejecting hash collisions, old entries and ambiguous depths.
// This is deliberately conservative; software-transformed geometry stays flat.
struct ProjectionCache {
  struct Entry {
    uint32_t key = 0;
    uint64_t frame = 0;
    float inverseDepth = 0;
  };
  std::vector<Entry> entries;
  uint64_t frame = 1;

  auto reset(bool enabled) -> void {
    entries.clear();
    if(enabled) entries.resize(65536);
    frame = 1;
  }
  auto nextFrame() -> void { ++frame; }
  static auto key(int x, int y) -> uint32_t {
    return uint16_t(x) | uint32_t(uint16_t(y)) << 16;
  }
  static auto index(uint32_t key) -> uint32_t { return (key * 2654435761u) >> 16; }

  auto record(int x, int y, float inverseDepth) -> void {
    if(entries.empty() || !std::isfinite(inverseDepth) || inverseDepth <= 0) return;
    auto k = key(x, y);
    auto& entry = entries[index(k)];
    if(entry.frame == frame && entry.key == k &&
       std::abs(entry.inverseDepth - inverseDepth) > inverseDepth * 0.01f) {
      entry.inverseDepth = 0;
      return;
    }
    entry = {k, frame, inverseDepth};
  }

  auto find(int x, int y) const -> float {
    if(entries.empty()) return 0;
    auto k = key(x, y);
    auto& entry = entries[index(k)];
    return entry.key == k && entry.frame && frame - entry.frame <= 1 ? entry.inverseDepth : 0;
  }
};

}
