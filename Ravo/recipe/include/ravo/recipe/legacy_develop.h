#pragma once

#include "ravo/recipe/develop.h"

namespace ravo
{
// Historical input conversion only; normal editing does not depend on this header.
struct LeftoverFlipGeometry
{
    std::int64_t rotate_quarters = 0;
    std::int64_t flip_horizontal = 0;
    std::int64_t flip_vertical = 0;

    [[nodiscard]] bool is_identity() const noexcept
    {
        return rotate_quarters == 0 && flip_horizontal == 0 && flip_vertical == 0;
    }
};

// Maps leftover flip.c orientation bits (ORIENTATION_NULL=-1 through TRANSVERSE=7)
// onto canonical rotate-then-flip. NULL and NONE are identity because Ravo applies
// camera EXIF at decode.
[[nodiscard]] Result<LeftoverFlipGeometry>
leftover_flip_orientation_to_geometry(std::int32_t orientation);

struct LeftoverCropBox
{
    double x = 0.0;
    double y = 0.0;
    double width = 1.0;
    double height = 1.0;

    [[nodiscard]] bool is_identity() const noexcept;
};

// Maps leftover crop.c left/top/right/bottom (cx, cy, cw, ch) onto canonical
// x/y/width/height. Full-frame 0,0,1,1 is identity. ratio_n/ratio_d export
// snapping is outside the accepted import mapping.
[[nodiscard]] Result<LeftoverCropBox> leftover_crop_box_to_geometry(float left, float top,
                                                                    float right, float bottom);

// Maps the leftover ashift generic lens model onto canonical perspective. The
// adapter remains responsible for rejecting legacy-only lens/crop modes before
// calling this bounded head conversion.
[[nodiscard]] Result<PerspectiveParams>
leftover_ashift_to_perspective(float rotation, float lensshift_v, float lensshift_h, float shear,
                               bool constrain_crop);

[[nodiscard]] Result<RgbLevelsParams>
leftover_rgblevels_from_v1(std::int32_t autoscale, std::int32_t preserve_colors,
                           const std::array<float, 9> &levels);
[[nodiscard]] Result<RgbCurveParams>
leftover_rgbcurve_from_v1(const std::vector<std::uint8_t> &payload);
[[nodiscard]] Result<void> leftover_rawdenoise_from_v2(const std::vector<std::uint8_t> &payload,
                                                       double &threshold,
                                                       std::array<std::array<double, 5>, 4> &bands);

} // namespace ravo
