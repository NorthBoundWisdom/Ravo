#include "ravo/engine/mask_geometry.h"

#include <algorithm>
#include <cmath>
#include "canvas_frame.h"
#include "perspective_transform.h"

namespace ravo
{
namespace
{
using Matrix = std::array<double, 9>;
Matrix multiply(const Matrix &a, const Matrix &b)
{
    Matrix result{};
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t column = 0; column < 3; ++column)
            for (std::size_t index = 0; index < 3; ++index)
                result[row * 3 + column] += a[row * 3 + index] * b[index * 3 + column];
    return result;
}
Matrix translate(const double x, const double y)
{
    return {1, 0, x, 0, 1, y, 0, 0, 1};
}
Result<Matrix> inverse(const Matrix &m)
{
    Matrix result{m[4] * m[8] - m[5] * m[7], m[2] * m[7] - m[1] * m[8], m[1] * m[5] - m[2] * m[4],
                  m[5] * m[6] - m[3] * m[8], m[0] * m[8] - m[2] * m[6], m[2] * m[3] - m[0] * m[5],
                  m[3] * m[7] - m[4] * m[6], m[1] * m[6] - m[0] * m[7], m[0] * m[4] - m[1] * m[3]};
    const double determinant = m[0] * result[0] + m[1] * result[3] + m[2] * result[6];
    if (!std::isfinite(determinant) || std::abs(determinant) < 1e-15)
        return make_error(ErrorCode::kValidation, "Mask geometry is not invertible",
                          {{"reason", "mask_geometry_not_invertible"}});
    for (auto &value : result)
        value /= determinant;
    return result;
}
} // namespace

Result<MaskGeometryMapping> prepare_mask_geometry(const DevelopParams &params, std::uint32_t width,
                                                  std::uint32_t height)
{
    if (width == 0 || height == 0)
        return make_error(ErrorCode::kValidation, "Mask geometry requires image dimensions",
                          {{"reason", "mask_geometry_unavailable"}});
    Matrix mapping{
        static_cast<double>(width), 0, -0.5, 0, static_cast<double>(height), -0.5, 0, 0, 1};
    if (params.geometry_effect_enabled)
    {
        if (params.canvas_enabled)
        {
            auto layout = compute_canvas_layout(width, height, params.canvas);
            if (!layout)
                return layout.error();
            mapping = multiply(translate(layout.value().image_x, layout.value().image_y), mapping);
            width = layout.value().output_width;
            height = layout.value().output_height;
        }
        const auto turns = (params.rotate_quarters % 4 + 4) % 4;
        for (int turn = 0; turn < turns; ++turn)
        {
            mapping = multiply({0, -1, static_cast<double>(height - 1), 1, 0, 0, 0, 0, 1}, mapping);
            std::swap(width, height);
        }
        if (params.flip_horizontal)
            mapping = multiply({-1, 0, static_cast<double>(width - 1), 0, 1, 0, 0, 0, 1}, mapping);
        if (params.flip_vertical)
            mapping = multiply({1, 0, 0, 0, -1, static_cast<double>(height - 1), 0, 0, 1}, mapping);
        PerspectiveParams perspective;
        perspective.rotation_degrees = params.straighten_degrees;
        perspective.vertical_shift = params.perspective_vertical;
        perspective.horizontal_shift = params.perspective_horizontal;
        perspective.shear = params.perspective_shear;
        perspective.constrain_crop = params.perspective_constrain_crop;
        if (!perspective.is_identity())
        {
            auto layout = compute_perspective_layout(width, height, perspective);
            if (!layout)
                return layout.error();
            mapping = multiply(translate(-static_cast<double>(layout.value().output_left),
                                         -static_cast<double>(layout.value().output_top)),
                               multiply(layout.value().forward, mapping));
            width = layout.value().output_width;
            height = layout.value().output_height;
        }
        const auto left =
            std::clamp(std::llround(params.crop_x * width), 0LL, static_cast<long long>(width - 1));
        const auto top = std::clamp(std::llround(params.crop_y * height), 0LL,
                                    static_cast<long long>(height - 1));
        const auto crop_width = std::min(
            std::clamp(std::llround(params.crop_width * width), 1LL, static_cast<long long>(width)),
            static_cast<long long>(width) - left);
        const auto crop_height = std::min(std::clamp(std::llround(params.crop_height * height), 1LL,
                                                     static_cast<long long>(height)),
                                          static_cast<long long>(height) - top);
        mapping =
            multiply(translate(-static_cast<double>(left), -static_cast<double>(top)), mapping);
        width = static_cast<std::uint32_t>(crop_width);
        height = static_cast<std::uint32_t>(crop_height);
    }
    if (params.frame_enabled && params.effects_effect_enabled)
    {
        auto layout = compute_frame_layout(width, height, params.frame);
        if (!layout)
            return layout.error();
        mapping = multiply(translate(layout.value().image_x, layout.value().image_y), mapping);
        width = layout.value().output_width;
        height = layout.value().output_height;
    }
    mapping =
        multiply({1.0 / width, 0, 0.5 / width, 0, 1.0 / height, 0.5 / height, 0, 0, 1}, mapping);
    auto reversed = inverse(mapping);
    if (!reversed)
        return reversed.error();
    return MaskGeometryMapping{mapping, reversed.value()};
}

Result<MaskPoint> map_mask_point(const MaskGeometryMapping &mapping, const MaskPoint point,
                                 const bool to_mask_frame)
{
    if (!std::isfinite(point.x) || !std::isfinite(point.y))
        return make_error(ErrorCode::kInvalidArgument, "Non-finite mask coordinate",
                          {{"reason", "invalid_mask_coordinate"}});
    const auto &m = to_mask_frame ? mapping.inverse : mapping.forward;
    const double divisor = m[6] * point.x + m[7] * point.y + m[8];
    const MaskPoint result{(m[0] * point.x + m[1] * point.y + m[2]) / divisor,
                           (m[3] * point.x + m[4] * point.y + m[5]) / divisor};
    if (!std::isfinite(result.x) || !std::isfinite(result.y) || std::abs(divisor) < 1e-15 ||
        (to_mask_frame &&
         (result.x < -1e-9 || result.x > 1 + 1e-9 || result.y < -1e-9 || result.y > 1 + 1e-9)))
        return make_error(ErrorCode::kInvalidArgument, "Coordinate is outside the photo content",
                          {{"reason", "mask_coordinate_outside_photo"}});
    return result;
}
} // namespace ravo
