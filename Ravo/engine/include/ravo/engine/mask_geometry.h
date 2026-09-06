#pragma once

#include <array>
#include "ravo/recipe/develop.h"

namespace ravo
{
struct MaskPoint
{
    double x = 0;
    double y = 0;
};

// Immutable normalized photo-content <-> displayed-photo mapping. Pixel-centre
// conventions and integer geometry layouts are identical to Engine rendering.
struct MaskGeometryMapping
{
    std::array<double, 9> forward{};
    std::array<double, 9> inverse{};
};

[[nodiscard]] Result<MaskGeometryMapping> prepare_mask_geometry(const DevelopParams &params,
                                                                std::uint32_t source_width,
                                                                std::uint32_t source_height);
[[nodiscard]] Result<MaskPoint> map_mask_point(const MaskGeometryMapping &mapping, MaskPoint point,
                                               bool to_mask_frame);
} // namespace ravo
