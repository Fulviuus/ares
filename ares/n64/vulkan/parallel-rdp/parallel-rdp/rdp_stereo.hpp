#pragma once

#include "rdp_data_structures.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace RDP
{
// RDP has already projected its vertices. With the usual perspective depth
// mapping, 1 - normalized_z is proportional to inverse camera depth. Applying
// an off-axis eye translation is therefore an affine shear of screen x. Transform
// all three edges AND the attribute gradients so texture/color/depth interpolation
// continues to describe the same surface. Rectangles and triangles without Z are
// deliberately left at screen depth by the command decoder.
inline bool stereo_transform(TriangleSetup &setup, AttributeSetup &attr,
                             double separation, double convergence)
{
  if(separation == 0.0) return true;
  constexpr double max_z = 32767.0 * 65536.0;
  constexpr double x_unit = 32768.0;
  constexpr double slope_unit = x_unit / 4.0;
  const double gain = separation * convergence / max_z;
  const double scale = 1.0 + gain * attr.dzdx;
  const double shear = gain * attr.dzdy;
  const double shift = separation * (1.0 - convergence) + gain * attr.z;
  if(!std::isfinite(scale) || scale < 0.125 || scale > 8.0) return false;

  auto triangle = setup;
  auto attributes = attr;
  const double x_origin = setup.xh / x_unit;
  const double y_origin = setup.yh >> 2;
  bool valid = true;
  auto convert = [&](double value) -> int32_t {
    if(!std::isfinite(value) || value < std::numeric_limits<int32_t>::min() ||
       value > std::numeric_limits<int32_t>::max()) {
      valid = false;
      return 0;
    }
    return int32_t(std::llround(value));
  };
  auto edge = [&](int32_t x, double y) {
    return convert(x_unit * (x / x_unit + shift +
      (scale - 1.0) * (x / x_unit - x_origin) + shear * (y - y_origin)));
  };
  triangle.xh = edge(setup.xh, y_origin);
  triangle.xm = edge(setup.xm, y_origin);
  triangle.xl = edge(setup.xl, setup.ym / 4.0);
  triangle.dxhdy = convert(scale * setup.dxhdy + shear * slope_unit);
  triangle.dxmdy = convert(scale * setup.dxmdy + shear * slope_unit);
  triangle.dxldy = convert(scale * setup.dxldy + shear * slope_unit);

  auto gradient = [&](int32_t &dx, int32_t &dy) {
    auto new_dx = dx / scale;
    dy = convert(dy - new_dx * shear);
    dx = convert(new_dx);
  };
  gradient(attributes.drdx, attributes.drdy);
  gradient(attributes.dgdx, attributes.dgdy);
  gradient(attributes.dbdx, attributes.dbdy);
  gradient(attributes.dadx, attributes.dady);
  gradient(attributes.dsdx, attributes.dsdy);
  gradient(attributes.dtdx, attributes.dtdy);
  gradient(attributes.dzdx, attributes.dzdy);
  gradient(attributes.dwdx, attributes.dwdy);
  if(!valid) return false;
  setup = triangle;
  attr = attributes;
  return true;
}
}
