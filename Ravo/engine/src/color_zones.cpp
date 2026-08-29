#include "color_zones.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <vector>

#include "d50_lab.h"
#include "ravo/recipe/color_input.h"

namespace ravo
{
namespace
{

constexpr std::size_t kLutSize = 0x10000U;
constexpr float kPi2 = 6.28318530717958647692F;

struct SplinePoint
{
    float x = 0.0F;
    float y = 0.0F;
    float derivative = 0.0F;
};

void checkpoint(const detail::ColorZonesControl &control, const detail::ColorZonesCheckpoint stage,
                const std::uint32_t progress) noexcept
{
    if (control.checkpoint_callback != nullptr)
        control.checkpoint_callback(control.context, stage, progress);
}

[[nodiscard]] Result<void> validate_input(const WorkingImage &input)
{
    const std::uint64_t pixels = static_cast<std::uint64_t>(input.width) * input.height;
    if (input.width == 0U || input.height == 0U || pixels > std::vector<float>{}.max_size() / 3U ||
        input.rgb.size() != static_cast<std::size_t>(pixels) * 3U)
        return make_error(ErrorCode::kValidation, "Color Zones input dimensions are invalid",
                          {{"reason", "invalid_color_zones_input"}});
    if (input.color_profile.model != ColorModel::kRgb ||
        input.color_profile.identifier != kInputProfileLinearRec709)
        return make_error(ErrorCode::kUnsupported,
                          "Color Zones requires declared linear Rec709 working pixels",
                          {{"profile", input.color_profile.identifier},
                           {"reason", "unsupported_color_zones_working_space"}});
    for (std::size_t index = 0U; index < input.rgb.size(); ++index)
    {
        if (!std::isfinite(input.rgb[index]))
            return make_error(ErrorCode::kValidation,
                              "Color Zones input contains a non-finite sample",
                              {{"sample_index", std::to_string(index)},
                               {"reason", "nonfinite_color_zones_input"}});
    }
    return {};
}

[[nodiscard]] float monotone_g(const float slope1, const float slope2, const float width1,
                               const float width2) noexcept
{
    if (slope1 * slope2 <= 0.0F)
        return 0.0F;
    const float alpha = (width1 + 2.0F * width2) / (3.0F * (width1 + width2));
    return slope1 * slope2 / (alpha * slope2 + (1.0F - alpha) * slope1);
}

[[nodiscard]] Result<void> catmull_derivatives(std::vector<SplinePoint> &points,
                                               const bool periodic)
{
    const std::size_t count = points.size();
    if (periodic)
    {
        points[0].derivative =
            (points[1].y - points[count - 1U].y) / (points[1].x - points[count - 1U].x + 1.0F);
        for (std::size_t index = 1U; index + 1U < count; ++index)
            points[index].derivative = (points[index + 1U].y - points[index - 1U].y) /
                                       (points[index + 1U].x - points[index - 1U].x);
        points[count - 1U].derivative =
            (points[0].y - points[count - 2U].y) / (points[0].x - points[count - 2U].x + 1.0F);
    }
    else
    {
        points[0].derivative = (points[1].y - points[0].y) / (points[1].x - points[0].x);
        for (std::size_t index = 1U; index + 1U < count; ++index)
            points[index].derivative = (points[index + 1U].y - points[index - 1U].y) /
                                       (points[index + 1U].x - points[index - 1U].x);
        points[count - 1U].derivative = (points[count - 1U].y - points[count - 2U].y) /
                                        (points[count - 1U].x - points[count - 2U].x);
    }
    return {};
}

[[nodiscard]] Result<void> monotone_derivatives(std::vector<SplinePoint> &points,
                                                const bool periodic)
{
    const std::size_t count = points.size();
    if (periodic)
    {
        std::vector<float> widths(count);
        std::vector<float> slopes(count);
        for (std::size_t index = 0U; index + 1U < count; ++index)
        {
            widths[index] = points[index + 1U].x - points[index].x;
            slopes[index] = (points[index + 1U].y - points[index].y) / widths[index];
        }
        widths[count - 1U] = points[0].x - points[count - 1U].x + 1.0F;
        slopes[count - 1U] = (points[0].y - points[count - 1U].y) / widths[count - 1U];
        points[0].derivative =
            monotone_g(slopes[count - 1U], slopes[0], widths[count - 1U], widths[0]);
        for (std::size_t index = 1U; index < count; ++index)
            points[index].derivative =
                monotone_g(slopes[index - 1U], slopes[index], widths[index - 1U], widths[index]);
        return {};
    }
    std::vector<float> slopes(count - 1U);
    for (std::size_t index = 0U; index + 1U < count; ++index)
        slopes[index] =
            (points[index + 1U].y - points[index].y) / (points[index + 1U].x - points[index].x);
    points[0].derivative = slopes[0];
    for (std::size_t index = 1U; index + 1U < count; ++index)
        points[index].derivative = slopes[index - 1U] * slopes[index] <= 0.0F ?
                                       0.0F :
                                       (slopes[index - 1U] + slopes[index]) * 0.5F;
    points[count - 1U].derivative = slopes[count - 2U];
    for (std::size_t index = 0U; index + 1U < count; ++index)
    {
        if (std::abs(slopes[index]) < std::numeric_limits<float>::epsilon())
        {
            points[index].derivative = 0.0F;
            points[index + 1U].derivative = 0.0F;
            continue;
        }
        const float alpha = points[index].derivative / slopes[index];
        const float beta = points[index + 1U].derivative / slopes[index];
        const float norm = alpha * alpha + beta * beta;
        if (norm > 9.0F)
        {
            points[index].derivative = 3.0F * alpha * slopes[index] / std::sqrt(norm);
            points[index + 1U].derivative = 3.0F * beta * slopes[index] / std::sqrt(norm);
        }
    }
    return {};
}

[[nodiscard]] bool solve_without_pivot(std::vector<float> &matrix, std::vector<float> &values,
                                       const std::size_t count) noexcept
{
    for (std::size_t pivot = 0U; pivot + 1U < count; ++pivot)
    {
        const float diagonal = matrix[pivot * count + pivot];
        if (diagonal == 0.0F)
            return false;
        for (std::size_t row = pivot + 1U; row < count; ++row)
        {
            matrix[row * count + pivot] /= diagonal;
            for (std::size_t column = pivot + 1U; column < count; ++column)
                matrix[row * count + column] -=
                    matrix[row * count + pivot] * matrix[pivot * count + column];
        }
    }
    for (std::size_t row = 0U; row < count; ++row)
        for (std::size_t column = 0U; column < row; ++column)
            values[row] -= matrix[row * count + column] * values[column];
    for (std::size_t row = count; row-- > 0U;)
    {
        for (std::size_t column = row + 1U; column < count; ++column)
            values[row] -= matrix[row * count + column] * values[column];
        const float diagonal = matrix[row * count + row];
        if (diagonal == 0.0F)
            return false;
        values[row] /= diagonal;
    }
    return true;
}

[[nodiscard]] Result<void> cubic_derivatives(std::vector<SplinePoint> &points, const bool periodic)
{
    const std::size_t count = points.size();
    std::vector<float> widths(periodic ? count : count - 1U);
    std::vector<float> deltas(periodic ? count : count - 1U);
    for (std::size_t index = 0U; index + 1U < count; ++index)
    {
        widths[index] = points[index + 1U].x - points[index].x;
        deltas[index] = points[index + 1U].y - points[index].y;
    }
    if (periodic)
    {
        widths[count - 1U] = points[0].x - points[count - 1U].x + 1.0F;
        deltas[count - 1U] = points[0].y - points[count - 1U].y;
    }
    std::vector<float> matrix(count * count, 0.0F);
    std::vector<float> second(count, 0.0F);
    for (std::size_t index = 1U; index + 1U < count; ++index)
    {
        matrix[index * count + index - 1U] = widths[index - 1U] / 6.0F;
        matrix[index * count + index] = (widths[index - 1U] + widths[index]) / 3.0F;
        matrix[index * count + index + 1U] = widths[index] / 6.0F;
        second[index] = deltas[index] / widths[index] - deltas[index - 1U] / widths[index - 1U];
    }
    if (periodic)
    {
        matrix[0] = (widths[count - 1U] + widths[0]) / 3.0F;
        matrix[(count - 1U) * count + count - 1U] =
            (widths[count - 2U] + widths[count - 1U]) / 3.0F;
        second[0] = deltas[0] / widths[0] - deltas[count - 1U] / widths[count - 1U];
        second[count - 1U] =
            deltas[count - 1U] / widths[count - 1U] - deltas[count - 2U] / widths[count - 2U];
        if (count > 2U)
        {
            matrix[1U] = widths[0] / 6.0F;
            matrix[(count - 1U) * count + count - 2U] = widths[count - 2U] / 6.0F;
            matrix[count - 1U] = widths[count - 1U] / 6.0F;
            matrix[(count - 1U) * count] = widths[count - 1U] / 6.0F;
        }
        else
        {
            matrix[1U] = (widths[0] + widths[1U]) / 6.0F;
            matrix[count] = matrix[1U];
        }
    }
    else
    {
        matrix[0] = 1.0F;
        matrix[(count - 1U) * count + count - 1U] = 1.0F;
    }
    if (!solve_without_pivot(matrix, second, count))
        return make_error(ErrorCode::kValidation, "Color Zones cubic curve is singular",
                          {{"reason", "singular_color_zones_curve"}});
    float last_slope = 0.0F;
    for (std::size_t index = 0U; index + 1U < count; ++index)
    {
        last_slope = deltas[index] / widths[index] -
                     widths[index] / 6.0F * (second[index + 1U] - second[index]);
        points[index].derivative = -widths[index] * second[index] * 0.5F + last_slope;
    }
    points[count - 1U].derivative =
        periodic ? widths[count - 2U] * second[count - 1U] * 0.5F + last_slope : last_slope;
    return {};
}

[[nodiscard]] Result<std::vector<SplinePoint>>
prepare_curve(const ColorZonesCurve &curve, const float strength, const bool periodic)
{
    std::vector<SplinePoint> points;
    points.reserve(curve.points.size());
    for (const auto &point : curve.points)
    {
        const float y = static_cast<float>(point.y) +
                        (static_cast<float>(point.y) - 0.5F) * (strength / 100.0F);
        if (!std::isfinite(y))
            return make_error(ErrorCode::kValidation, "Color Zones curve strength is non-finite",
                              {{"reason", "nonfinite_color_zones_curve"}});
        points.push_back({static_cast<float>(point.x), y, 0.0F});
    }
    switch (curve.interpolation)
    {
    case ColorZonesInterpolation::kCubicSpline:
    {
        auto prepared = cubic_derivatives(points, periodic);
        return prepared ? Result<std::vector<SplinePoint>>{std::move(points)} : prepared.error();
    }
    case ColorZonesInterpolation::kCatmullRom:
    {
        auto prepared = catmull_derivatives(points, periodic);
        return prepared ? Result<std::vector<SplinePoint>>{std::move(points)} : prepared.error();
    }
    case ColorZonesInterpolation::kMonotoneHermite:
    {
        auto prepared = monotone_derivatives(points, periodic);
        return prepared ? Result<std::vector<SplinePoint>>{std::move(points)} : prepared.error();
    }
    }
    return make_error(ErrorCode::kValidation, "Color Zones curve type is unsupported");
}

[[nodiscard]] float evaluate_curve(const std::vector<SplinePoint> &points, float x,
                                   const bool periodic) noexcept
{
    std::size_t first = 0U;
    std::size_t second = 1U;
    float width = 0.0F;
    if (periodic)
    {
        x = std::fmod(x, 1.0F);
        if (x < points.front().x)
            x += 1.0F;
        const auto upper = std::upper_bound(points.begin(), points.end(), x,
                                            [](const float value, const SplinePoint &point)
                                            { return value < point.x; });
        first = upper == points.begin() ? points.size() - 1U :
                upper == points.end()   ? points.size() - 1U :
                                          static_cast<std::size_t>(upper - points.begin() - 1);
        second = first + 1U < points.size() ? first + 1U : 0U;
        width = second > first ? points[second].x - points[first].x :
                                 points[second].x - points[first].x + 1.0F;
    }
    else
    {
        if (x <= points.front().x)
            return points.front().y;
        if (x >= points.back().x)
            return points.back().y;
        const auto upper = std::upper_bound(points.begin(), points.end(), x,
                                            [](const float value, const SplinePoint &point)
                                            { return value < point.x; });
        second = static_cast<std::size_t>(upper - points.begin());
        first = second - 1U;
        width = points[second].x - points[first].x;
    }
    const float normalized = (x - points[first].x) / width;
    const float squared = normalized * normalized;
    const float cubed = squared * normalized;
    return (2.0F * cubed - 3.0F * squared + 1.0F) * points[first].y +
           (cubed - 2.0F * squared + normalized) * width * points[first].derivative +
           (-2.0F * cubed + 3.0F * squared) * points[second].y +
           (cubed - squared) * width * points[second].derivative;
}

[[nodiscard]] Result<std::vector<float>> build_lut(const ColorZonesCurve &curve,
                                                   const float strength, const bool periodic,
                                                   const CancellationToken &cancellation)
{
    auto points = prepare_curve(curve, strength, periodic);
    if (!points)
        return points.error();
    std::vector<float> lut(kLutSize);
    for (std::size_t index = 0U; index < lut.size(); ++index)
    {
        if ((index & 0x0fffU) == 0U)
        {
            auto active = cancellation.check();
            if (!active)
                return active.error();
        }
        const float x = static_cast<float>(index) / static_cast<float>(kLutSize - 1U);
        const float value = evaluate_curve(points.value(), x, periodic);
        if (!std::isfinite(value) || value < 0.0F || value > 1.0F)
            return make_error(ErrorCode::kValidation,
                              "Color Zones curve produced an out-of-range sample",
                              {{"lut_index", std::to_string(index)},
                               {"reason", "invalid_color_zones_curve_output"}});
        const auto quantized = static_cast<std::uint16_t>(std::round(value * 65535.0F));
        lut[index] = static_cast<float>(quantized) / 65536.0F;
    }
    return lut;
}

[[nodiscard]] float lookup(const std::vector<float> &lut, const float value) noexcept
{
    const int bin0 = std::clamp(static_cast<int>(65536.0F * value), 0, 65535);
    const int bin1 = std::clamp(static_cast<int>(65536.0F * value) + 1, 0, 65535);
    const float fraction = 65536.0F * value - static_cast<float>(bin0);
    return lut[static_cast<std::size_t>(bin1)] * fraction +
           lut[static_cast<std::size_t>(bin0)] * (1.0F - fraction);
}

} // namespace

Result<WorkingImage> detail::apply_color_zones_controlled(WorkingImage input,
                                                          const ColorZonesParams &params,
                                                          const CancellationToken &cancellation,
                                                          const ColorZonesControl control)
try
{
    auto active = cancellation.check();
    if (!active)
        return active.error();
    auto canonical = color_zones_to_parameters(params);
    if (!canonical)
        return canonical.error();
    auto valid = validate_input(input);
    if (!valid)
        return valid.error();
    const bool periodic = params.select_by == ColorZonesChannel::kHue;
    std::array<std::vector<float>, 3> luts;
    for (std::size_t channel = 0U; channel < luts.size(); ++channel)
    {
        checkpoint(control, ColorZonesCheckpoint::kBuildLut, static_cast<std::uint32_t>(channel));
        auto built = build_lut(params.curves[channel], static_cast<float>(params.strength),
                               periodic, cancellation);
        if (!built)
            return built.error();
        luts[channel] = std::move(built).value();
    }
    WorkingImage output;
    output.width = input.width;
    output.height = input.height;
    output.color_profile = input.color_profile;
    output.exposure_analysis = input.exposure_analysis;
    output.canonical_roi_scale = input.canonical_roi_scale;
    output.mask_attached_frame = input.mask_attached_frame;
    output.rgb.resize(input.rgb.size());
    for (std::uint32_t row = 0U; row < input.height; ++row)
    {
        checkpoint(control, ColorZonesCheckpoint::kProcessRow, row);
        active = cancellation.check();
        if (!active)
            return active.error();
        for (std::uint32_t column = 0U; column < input.width; ++column)
        {
            const std::size_t pixel = (static_cast<std::size_t>(row) * input.width + column) * 3U;
            const auto lab = d50_lab::xyz_to_lab(d50_lab::linear_rec709_to_xyz(
                {input.rgb[pixel], input.rgb[pixel + 1U], input.rgb[pixel + 2U]}));
            const float a = lab[1];
            const float b = lab[2];
            const float hue = std::fmod(std::atan2(b, a) + kPi2, kPi2) / kPi2;
            const float chroma = std::sqrt(b * b + a * a);
            float selection = 0.0F;
            float blend = 0.0F;
            switch (params.select_by)
            {
            case ColorZonesChannel::kLightness:
                selection = std::min(1.0F, lab[0] / 100.0F);
                break;
            case ColorZonesChannel::kChroma:
                selection = std::min(1.0F, chroma / 128.0F);
                break;
            case ColorZonesChannel::kHue:
                selection = hue;
                blend = (1.0F - chroma / 128.0F) * (1.0F - chroma / 128.0F);
                break;
            }
            const float lightness_move =
                blend * 0.5F + (1.0F - blend) * lookup(luts[0], selection) - 0.5F;
            const float hue_move =
                blend * 0.5F + (1.0F - blend) * lookup(luts[2], selection) - 0.5F;
            const float chroma_move = 2.0F * lookup(luts[1], selection);
            const std::array<float, 3> adjusted_lab{
                lab[0] * std::pow(2.0F, 4.0F * lightness_move),
                std::cos(kPi2 * (hue + hue_move)) * chroma_move * chroma,
                std::sin(kPi2 * (hue + hue_move)) * chroma_move * chroma};
            const auto rgb = d50_lab::xyz_to_linear_rec709(d50_lab::lab_to_xyz(adjusted_lab));
            for (std::size_t channel = 0U; channel < 3U; ++channel)
            {
                if (!std::isfinite(rgb[channel]))
                    return make_error(ErrorCode::kValidation,
                                      "Color Zones produced a non-finite sample",
                                      {{"pixel", std::to_string(pixel / 3U)},
                                       {"reason", "nonfinite_color_zones_output"}});
                output.rgb[pixel + channel] = rgb[channel];
            }
        }
    }
    checkpoint(control, ColorZonesCheckpoint::kBeforePublication, 0U);
    active = cancellation.check();
    return active ? Result<WorkingImage>{std::move(output)} : active.error();
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Color Zones allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<WorkingImage> apply_color_zones(WorkingImage input, const ColorZonesParams &params,
                                       const CancellationToken &cancellation)
{
    return detail::apply_color_zones_controlled(std::move(input), params, cancellation);
}

Result<WorkingImage> apply_color_zones(WorkingImage input, const OperationInstance &operation,
                                       const CancellationToken &cancellation)
{
    if (operation.id != kColorZonesOperationId ||
        operation.schema_version != kColorZonesOperationSchemaVersion)
        return make_error(ErrorCode::kValidation, "Operation is not canonical Color Zones");
    auto params = color_zones_from_parameters(operation.parameters);
    return params ? apply_color_zones(std::move(input), params.value(), cancellation) :
                    params.error();
}

} // namespace ravo
