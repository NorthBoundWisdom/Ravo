#include "ravo/recipe/develop.h"
#include "ravo/recipe/develop_mask.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <iomanip>
#include <map>
#include <new>
#include <numbers>
#include <set>
#include <sstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>


#include "develop_internal.h"

namespace ravo
{
using namespace develop_internal;

namespace
{
struct ToneCurveSpline
{
    enum class Kind : std::uint8_t
    {
        kHermite = 0,
        kCubic = 1,
    };
    std::vector<ToneCurvePoint> points;
    std::vector<double> coeffs;
    Kind kind = Kind::kHermite;
};

[[nodiscard]] std::size_t tone_curve_interval(const std::vector<ToneCurvePoint> &points,
                                              const double x) noexcept
{
    if (points.size() < 2)
    {
        return 0;
    }
    std::size_t index = points.size() - 2U;
    for (std::size_t cursor = 0; cursor + 1 < points.size(); ++cursor)
    {
        if (x < points[cursor + 1U].x)
        {
            return cursor;
        }
    }
    return index;
}

[[nodiscard]] ToneCurveSpline make_hermite_spline(const std::vector<ToneCurvePoint> &points,
                                                  const bool monotone)
{
    ToneCurveSpline spline;
    spline.points = points;
    spline.kind = ToneCurveSpline::Kind::kHermite;
    if (points.size() < 2)
    {
        return spline;
    }
    const auto count = points.size();
    spline.coeffs.assign(count, 0.0);
    spline.coeffs.front() = (points[1].y - points[0].y) / (points[1].x - points[0].x);
    spline.coeffs.back() = (points[count - 1U].y - points[count - 2U].y) /
                           (points[count - 1U].x - points[count - 2U].x);
    for (std::size_t index = 1; index + 1 < count; ++index)
    {
        if (monotone)
        {
            const double left =
                (points[index].y - points[index - 1U].y) / (points[index].x - points[index - 1U].x);
            const double right =
                (points[index + 1U].y - points[index].y) / (points[index + 1U].x - points[index].x);
            spline.coeffs[index] = left * right <= 0.0 ? 0.0 : 0.5 * (left + right);
        }
        else
        {
            spline.coeffs[index] = (points[index + 1U].y - points[index - 1U].y) /
                                   (points[index + 1U].x - points[index - 1U].x);
        }
    }
    if (!monotone)
    {
        return spline;
    }
    std::vector<double> delta(count - 1U);
    for (std::size_t index = 0; index + 1 < count; ++index)
    {
        const double dx = points[index + 1U].x - points[index].x;
        delta[index] = dx > 1e-12 ? (points[index + 1U].y - points[index].y) / dx : 0.0;
    }
    for (std::size_t index = 0; index + 1 < count; ++index)
    {
        if (std::abs(delta[index]) <= 1e-12)
        {
            spline.coeffs[index] = 0.0;
            spline.coeffs[index + 1U] = 0.0;
            continue;
        }
        const double alpha = spline.coeffs[index] / delta[index];
        const double beta = spline.coeffs[index + 1U] / delta[index];
        const double sumsq = alpha * alpha + beta * beta;
        if (sumsq > 9.0)
        {
            const double tau = 3.0 / std::sqrt(sumsq);
            spline.coeffs[index] = tau * alpha * delta[index];
            spline.coeffs[index + 1U] = tau * beta * delta[index];
        }
    }
    return spline;
}

[[nodiscard]] ToneCurveSpline make_cubic_spline(const std::vector<ToneCurvePoint> &points)
{
    ToneCurveSpline spline;
    spline.points = points;
    spline.kind = ToneCurveSpline::Kind::kCubic;
    if (points.size() < 2)
    {
        return spline;
    }
    const auto count = points.size();
    spline.coeffs.assign(count, 0.0);
    if (count < 3)
    {
        return spline;
    }
    const auto interior = count - 2U;
    std::vector<double> lower(interior, 0.0);
    std::vector<double> diagonal(interior, 0.0);
    std::vector<double> upper(interior, 0.0);
    std::vector<double> rhs(interior, 0.0);
    for (std::size_t index = 1; index + 1 < count; ++index)
    {
        const auto slot = index - 1U;
        const double h0 = points[index].x - points[index - 1U].x;
        const double h1 = points[index + 1U].x - points[index].x;
        if (h0 <= 1e-12 || h1 <= 1e-12)
        {
            return make_hermite_spline(points, true);
        }
        lower[slot] = h0 / 6.0;
        diagonal[slot] = (h0 + h1) / 3.0;
        upper[slot] = h1 / 6.0;
        rhs[slot] = (points[index + 1U].y - points[index].y) / h1 -
                    (points[index].y - points[index - 1U].y) / h0;
    }
    std::vector<double> c_prime(interior, 0.0);
    std::vector<double> d_prime(interior, 0.0);
    if (std::abs(diagonal[0]) <= 1e-12)
    {
        return make_hermite_spline(points, true);
    }
    c_prime[0] = upper[0] / diagonal[0];
    d_prime[0] = rhs[0] / diagonal[0];
    for (std::size_t index = 1; index < interior; ++index)
    {
        const double denom = diagonal[index] - lower[index] * c_prime[index - 1U];
        if (std::abs(denom) <= 1e-12)
        {
            return make_hermite_spline(points, true);
        }
        c_prime[index] = index + 1 < interior ? upper[index] / denom : 0.0;
        d_prime[index] = (rhs[index] - lower[index] * d_prime[index - 1U]) / denom;
    }
    spline.coeffs[count - 2U] = d_prime[interior - 1U];
    for (std::size_t index = interior - 1U; index-- > 0U;)
    {
        spline.coeffs[index + 1U] = d_prime[index] - c_prime[index] * spline.coeffs[index + 2U];
    }
    return spline;
}

[[nodiscard]] ToneCurveSpline make_tone_curve_spline(const std::vector<ToneCurvePoint> &points,
                                                     const std::string_view interpolation)
{
    if (interpolation == kToneCurveInterpolationCubicSpline)
    {
        return make_cubic_spline(points);
    }
    return make_hermite_spline(points, interpolation != kToneCurveInterpolationCatmullRom);
}

[[nodiscard]] double evaluate_tone_curve_spline(const ToneCurveSpline &spline, const double x)
{
    if (spline.points.size() < 2)
    {
        return std::clamp(x, 0.0, 1.0);
    }
    if (x <= spline.points.front().x)
    {
        return spline.points.front().y;
    }
    if (x >= spline.points.back().x)
    {
        return spline.points.back().y;
    }
    const auto index = tone_curve_interval(spline.points, x);
    const auto &left = spline.points[index];
    const auto &right = spline.points[index + 1U];
    const double dx = right.x - left.x;
    if (dx <= 1e-12)
    {
        return left.y;
    }
    if (spline.kind == ToneCurveSpline::Kind::kCubic)
    {
        const double dt = x - left.x;
        const double ypp0 = index < spline.coeffs.size() ? spline.coeffs[index] : 0.0;
        const double ypp1 = index + 1U < spline.coeffs.size() ? spline.coeffs[index + 1U] : 0.0;
        const double b = (right.y - left.y) / dx - (ypp1 + 2.0 * ypp0) * dx / 6.0;
        const double c = ypp0 / 2.0;
        const double d = (ypp1 - ypp0) / (6.0 * dx);
        return left.y + b * dt + c * dt * dt + d * dt * dt * dt;
    }
    const double t = (x - left.x) / dx;
    const double t2 = t * t;
    const double t3 = t2 * t;
    const double h00 = 2.0 * t3 - 3.0 * t2 + 1.0;
    const double h10 = t3 - 2.0 * t2 + t;
    const double h01 = -2.0 * t3 + 3.0 * t2;
    const double h11 = t3 - t2;
    const double m0 = index < spline.coeffs.size() ? spline.coeffs[index] : 0.0;
    const double m1 = index + 1U < spline.coeffs.size() ? spline.coeffs[index + 1U] : 0.0;
    return h00 * left.y + h10 * dx * m0 + h01 * right.y + h11 * dx * m1;
}

} // namespace

bool tone_curve_is_identity(const std::vector<ToneCurvePoint> &points) noexcept
{
    if (points.empty())
    {
        return true;
    }
    if (points.size() < kToneCurveMinPoints || !near(points.front().x, 0.0) ||
        !near(points.back().x, 1.0))
    {
        return false;
    }
    for (const auto &point : points)
    {
        if (!near(point.x, point.y))
        {
            return false;
        }
    }
    return true;
}

void clamp_tone_curve(std::vector<ToneCurvePoint> &points) noexcept
{
    if (points.empty())
    {
        return;
    }
    for (auto &point : points)
    {
        point.x = clamp_value(point.x, 0.0, 1.0);
        point.y = clamp_value(point.y, 0.0, 1.0);
    }
    std::sort(points.begin(), points.end(),
              [](const ToneCurvePoint &left, const ToneCurvePoint &right)
              { return left.x < right.x; });
    std::vector<ToneCurvePoint> merged;
    merged.reserve(points.size());
    for (const auto &point : points)
    {
        if (!merged.empty() && point.x - merged.back().x < 1e-4)
        {
            merged.back() = point;
        }
        else
        {
            merged.push_back(point);
        }
    }
    if (merged.size() < kToneCurveMinPoints)
    {
        points.clear();
        return;
    }
    merged.front().x = 0.0;
    merged.back().x = 1.0;
    if (merged.size() >= 3 && merged[1].x < 1e-4)
    {
        merged.erase(merged.begin() + 1);
    }
    if (merged.size() >= 3 && merged[merged.size() - 2U].x > 1.0 - 1e-4)
    {
        merged.erase(merged.end() - 2);
    }
    while (merged.size() > kToneCurveMaxPoints && merged.size() > kToneCurveMinPoints)
    {
        merged.erase(merged.begin() + static_cast<std::ptrdiff_t>(merged.size() / 2));
    }
    if (tone_curve_is_identity(merged))
    {
        points.clear();
        return;
    }
    points = std::move(merged);
}

bool curve_interpolation_is_supported(const std::string_view interpolation) noexcept
{
    return interpolation.empty() || interpolation == kToneCurveInterpolationMonotoneHermite ||
           interpolation == kToneCurveInterpolationCatmullRom ||
           interpolation == kToneCurveInterpolationCubicSpline;
}

int curve_interpolation_index(const std::string_view interpolation) noexcept
{
    if (interpolation == kToneCurveInterpolationCatmullRom)
    {
        return 1;
    }
    if (interpolation == kToneCurveInterpolationCubicSpline)
    {
        return 2;
    }
    return 0;
}

std::string_view curve_interpolation_from_index(const int index) noexcept
{
    if (index == 1)
    {
        return kToneCurveInterpolationCatmullRom;
    }
    if (index == 2)
    {
        return kToneCurveInterpolationCubicSpline;
    }
    return kToneCurveInterpolationMonotoneHermite;
}


Result<std::string_view> parse_curve_interpolation(const std::string_view interpolation)
{
    if (curve_interpolation_is_supported(interpolation))
    {
        return interpolation.empty() ? kToneCurveInterpolationMonotoneHermite : interpolation;
    }
    return make_error(ErrorCode::kValidation, "Curve interpolation is unsupported",
                      {{"interpolation", std::string(interpolation)}});
}

double evaluate_tone_curve(const std::vector<ToneCurvePoint> &points, const double x) noexcept
{
    return evaluate_tone_curve(points, x, kToneCurveInterpolationMonotoneHermite);
}

double evaluate_tone_curve(const std::vector<ToneCurvePoint> &points, const double x,
                           const std::string_view interpolation) noexcept
{
    if (tone_curve_is_identity(points))
    {
        return std::clamp(x, 0.0, 1.0);
    }
    const auto kind = curve_interpolation_is_supported(interpolation) ?
                          interpolation :
                          kToneCurveInterpolationMonotoneHermite;
    return evaluate_tone_curve_spline(make_tone_curve_spline(points, kind), x);
}

Result<std::vector<float>> build_tone_curve_lut(const std::vector<ToneCurvePoint> &points,
                                                const std::string_view interpolation,
                                                const std::size_t sample_count)
try
{
    if (sample_count == 0U)
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Tone curve LUT sample count must be non-zero");
    }
    if (sample_count > std::vector<float>{}.max_size())
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Tone curve LUT sample count exceeds the supported size",
                          {{"reason", "sample_count_too_large"}});
    }
    if (!curve_interpolation_is_supported(interpolation))
    {
        return make_error(ErrorCode::kValidation, "Tone curve interpolation is unsupported",
                          {{"interpolation", std::string(interpolation)}});
    }
    std::vector<float> lut(sample_count, 0.0F);
    if (tone_curve_is_identity(points))
    {
        for (std::size_t index = 0; index < sample_count; ++index)
        {
            lut[index] =
                static_cast<float>(static_cast<double>(index) / static_cast<double>(sample_count));
        }
        return lut;
    }
    const ToneCurveSpline spline = make_tone_curve_spline(points, interpolation);
    for (std::size_t index = 0; index < sample_count; ++index)
    {
        const double x = static_cast<double>(index) / static_cast<double>(sample_count);
        lut[index] = static_cast<float>(evaluate_tone_curve_spline(spline, x));
    }
    return lut;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Tone curve LUT allocation failed",
                      {{"reason", "allocation_failed"}});
}

[[nodiscard]] double parametric_hermite01(const double t) noexcept
{
    const double clamped = std::clamp(t, 0.0, 1.0);
    return clamped * clamped * (3.0 - 2.0 * clamped);
}

[[nodiscard]] double parametric_fade_out(const double x, const double start,
                                         const double end) noexcept
{
    if (x <= start)
    {
        return 1.0;
    }
    if (x >= end || end <= start)
    {
        return 0.0;
    }
    return 1.0 - parametric_hermite01((x - start) / (end - start));
}

[[nodiscard]] double parametric_fade_in(const double x, const double start,
                                        const double end) noexcept
{
    if (x <= start || end <= start)
    {
        return 0.0;
    }
    if (x >= end)
    {
        return 1.0;
    }
    return parametric_hermite01((x - start) / (end - start));
}

[[nodiscard]] double parametric_bump(const double x, const double left, const double peak,
                                     const double right) noexcept
{
    if (x <= left || x >= right)
    {
        return 0.0;
    }
    if (x <= peak)
    {
        return parametric_fade_in(x, left, peak);
    }
    return parametric_fade_out(x, peak, right);
}

bool rgb_curve_parametric_is_identity(const RgbCurveParams &params) noexcept
{
    return near(params.parametric_shadows, 0.0) && near(params.parametric_darks, 0.0) &&
           near(params.parametric_lights, 0.0) && near(params.parametric_highlights, 0.0);
}

double evaluate_rgb_curve_parametric(const RgbCurveParams &params, const double x) noexcept
{
    const double input = std::clamp(x, 0.0, 1.0);
    if (rgb_curve_parametric_is_identity(params))
    {
        return input;
    }
    const double split0 = params.parametric_split_shadows;
    const double split1 = params.parametric_split_mid;
    const double split2 = params.parametric_split_highlights;
    const double delta = params.parametric_shadows * parametric_fade_out(input, 0.0, split0) +
                         params.parametric_darks * parametric_bump(input, 0.0, split0, split1) +
                         params.parametric_lights * parametric_bump(input, split0, split1, split2) +
                         params.parametric_highlights * parametric_fade_in(input, split2, 1.0);
    return std::clamp(input + 0.35 * delta, 0.0, 1.0);
}

void clamp_rgb_curve(RgbCurveParams &params) noexcept
{
    for (auto &channel : params.channels)
    {
        for (auto &point : channel)
        {
            point.x = clamp_value(point.x, 0.0, 1.0);
            point.y = clamp_value(point.y, 0.0, 1.0);
        }
        std::sort(channel.begin(), channel.end(),
                  [](const ToneCurvePoint &left, const ToneCurvePoint &right)
                  { return left.x < right.x; });
        if (channel.size() < kToneCurveMinPoints)
        {
            channel = {{0.0, 0.0}, {1.0, 1.0}};
        }
        while (channel.size() > kToneCurveMaxPoints)
        {
            channel.erase(channel.begin() + static_cast<std::ptrdiff_t>(channel.size() / 2));
        }
    }
    if (params.mode != kRgbLevelsModeIndependent)
    {
        params.mode = std::string(kRgbLevelsModeLinked);
    }
    if (!curve_interpolation_is_supported(params.interpolation))
    {
        params.interpolation = std::string(kToneCurveInterpolationMonotoneHermite);
    }
    if (params.application_space != kRgbCurveApplicationSpaceSceneLinear &&
        params.application_space != kRgbCurveApplicationSpaceDisplaySrgb)
    {
        params.application_space = std::string(kRgbCurveApplicationSpaceSceneLinear);
    }
    params.parametric_shadows = clamp_value(params.parametric_shadows, -1.0, 1.0);
    params.parametric_darks = clamp_value(params.parametric_darks, -1.0, 1.0);
    params.parametric_lights = clamp_value(params.parametric_lights, -1.0, 1.0);
    params.parametric_highlights = clamp_value(params.parametric_highlights, -1.0, 1.0);
    params.parametric_split_shadows = clamp_value(params.parametric_split_shadows, 0.05, 0.90);
    params.parametric_split_mid = clamp_value(params.parametric_split_mid, 0.10, 0.95);
    params.parametric_split_highlights =
        clamp_value(params.parametric_split_highlights, 0.15, 0.98);
    if (params.parametric_split_mid < params.parametric_split_shadows + 0.05)
    {
        params.parametric_split_mid = params.parametric_split_shadows + 0.05;
    }
    if (params.parametric_split_highlights < params.parametric_split_mid + 0.05)
    {
        params.parametric_split_highlights = std::min(0.98, params.parametric_split_mid + 0.05);
    }
    if (params.parametric_split_mid > params.parametric_split_highlights - 0.05)
    {
        params.parametric_split_mid = params.parametric_split_highlights - 0.05;
    }
    if (params.parametric_split_shadows > params.parametric_split_mid - 0.05)
    {
        params.parametric_split_shadows = params.parametric_split_mid - 0.05;
    }
}

Result<ToneCurveWorkingSpace> parse_tone_curve_working_space(const std::string_view text)
{
    if (text == kToneCurveWorkingSpaceSrgb)
    {
        return ToneCurveWorkingSpace::kSrgb;
    }
    if (text == kToneCurveWorkingSpaceLinearRgb)
    {
        return ToneCurveWorkingSpace::kLinearRgb;
    }
    if (text == kToneCurveWorkingSpaceRgb)
    {
        return ToneCurveWorkingSpace::kRgb;
    }
    if (text == kToneCurveWorkingSpaceLab)
    {
        return ToneCurveWorkingSpace::kLab;
    }
    if (text == kToneCurveWorkingSpaceXyz)
    {
        return ToneCurveWorkingSpace::kXyz;
    }
    if (text == kToneCurveWorkingSpaceLabIndependent)
    {
        return ToneCurveWorkingSpace::kLabIndependent;
    }
    return make_error(ErrorCode::kValidation, "Tone curve working space is unsupported",
                      {{"working_space", std::string(text)}});
}

std::string_view tone_curve_working_space_name(const ToneCurveWorkingSpace space) noexcept
{
    switch (space)
    {
    case ToneCurveWorkingSpace::kSrgb:
        return kToneCurveWorkingSpaceSrgb;
    case ToneCurveWorkingSpace::kLinearRgb:
        return kToneCurveWorkingSpaceLinearRgb;
    case ToneCurveWorkingSpace::kRgb:
        return kToneCurveWorkingSpaceRgb;
    case ToneCurveWorkingSpace::kLab:
        return kToneCurveWorkingSpaceLab;
    case ToneCurveWorkingSpace::kXyz:
        return kToneCurveWorkingSpaceXyz;
    case ToneCurveWorkingSpace::kLabIndependent:
        return kToneCurveWorkingSpaceLabIndependent;
    }
    return kToneCurveWorkingSpaceRgb;
}

Result<std::vector<ToneCurvePoint>> parse_tone_curve_points(const ParameterValue &value)
{
    const auto *array = std::get_if<ParameterValue::Array>(&value.value);
    if (array == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Tone curve points must be an array");
    }
    if (array->size() < kToneCurveMinPoints || array->size() > kToneCurveMaxPoints)
    {
        return make_error(ErrorCode::kValidation, "Tone curve must have between 2 and 20 points",
                          {{"point_count", std::to_string(array->size())}});
    }
    std::vector<ToneCurvePoint> points;
    points.reserve(array->size());
    for (std::size_t index = 0; index < array->size(); ++index)
    {
        const auto *object = std::get_if<ParameterValue::Object>(&(*array)[index].value);
        if (object == nullptr || object->size() != 2)
        {
            return make_error(ErrorCode::kValidation,
                              "Tone curve point must be an object with only x and y",
                              {{"point_index", std::to_string(index)}});
        }
        const auto x_found = object->find("x");
        const auto y_found = object->find("y");
        if (x_found == object->end() || y_found == object->end())
        {
            return make_error(ErrorCode::kValidation, "Tone curve point is missing x or y",
                              {{"point_index", std::to_string(index)}});
        }
        const double x = as_number(x_found->second, std::numeric_limits<double>::quiet_NaN());
        const double y = as_number(y_found->second, std::numeric_limits<double>::quiet_NaN());
        if (!std::isfinite(x) || !std::isfinite(y) || x < 0.0 || x > 1.0 || y < 0.0 || y > 1.0)
        {
            return make_error(ErrorCode::kValidation, "Tone curve point is outside 0..1",
                              {{"point_index", std::to_string(index)}});
        }
        if (!points.empty() && x < points.back().x + 1e-4)
        {
            return make_error(ErrorCode::kValidation, "Tone curve point x values must increase",
                              {{"point_index", std::to_string(index)}});
        }
        points.push_back({x, y});
    }
    if (!near(points.front().x, 0.0) || !near(points.back().x, 1.0))
    {
        return make_error(ErrorCode::kValidation, "Tone curve must start at x=0 and end at x=1");
    }
    return points;
}

Result<std::vector<ToneCurvePoint>> parse_rgb_curve_points(const ParameterValue &value)
{
    const auto *array = std::get_if<ParameterValue::Array>(&value.value);
    if (array == nullptr)
    {
        return make_error(ErrorCode::kValidation, "RGB curve points must be an array");
    }
    if (array->size() < kToneCurveMinPoints || array->size() > kToneCurveMaxPoints)
    {
        return make_error(ErrorCode::kValidation, "RGB curve must have between 2 and 20 points",
                          {{"point_count", std::to_string(array->size())}});
    }
    std::vector<ToneCurvePoint> points;
    points.reserve(array->size());
    for (std::size_t index = 0; index < array->size(); ++index)
    {
        const auto *object = std::get_if<ParameterValue::Object>(&(*array)[index].value);
        if (object == nullptr || object->size() != 2)
        {
            return make_error(ErrorCode::kValidation,
                              "RGB curve point must be an object with only x and y",
                              {{"point_index", std::to_string(index)}});
        }
        const auto x_found = object->find("x");
        const auto y_found = object->find("y");
        if (x_found == object->end() || y_found == object->end())
        {
            return make_error(ErrorCode::kValidation, "RGB curve point is missing x or y",
                              {{"point_index", std::to_string(index)}});
        }
        const double x = as_number(x_found->second, std::numeric_limits<double>::quiet_NaN());
        const double y = as_number(y_found->second, std::numeric_limits<double>::quiet_NaN());
        if (!std::isfinite(x) || !std::isfinite(y) || x < 0.0 || x > 1.0 || y < 0.0 || y > 1.0)
        {
            return make_error(ErrorCode::kValidation, "RGB curve point is outside 0..1",
                              {{"point_index", std::to_string(index)}});
        }
        if (!points.empty() && !(x > points.back().x))
        {
            return make_error(ErrorCode::kValidation, "RGB curve point x values must increase",
                              {{"point_index", std::to_string(index)}});
        }
        points.push_back({x, y});
    }
    return points;
}

ParameterValue tone_curve_points_to_parameter(const std::vector<ToneCurvePoint> &points)
{
    ParameterValue::Array array;
    const auto &source = tone_curve_is_identity(points) ?
                             std::vector<ToneCurvePoint>{{0.0, 0.0}, {1.0, 1.0}} :
                             points;
    array.reserve(source.size());
    for (const auto &point : source)
    {
        array.push_back(ParameterValue{ParameterValue::Object{
            {"x", ParameterValue{point.x}},
            {"y", ParameterValue{point.y}},
        }});
    }
    return ParameterValue{std::move(array)};
}

Result<void>
validate_tone_curve_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    if (const auto found = parameters.find("working_space"); found != parameters.end())
    {
        const auto *text = as_string_if(found->second);
        if (text == nullptr)
        {
            return make_error(ErrorCode::kValidation, "Tone curve working_space must be a string");
        }
        auto space = parse_tone_curve_working_space(*text);
        if (!space)
        {
            return space.error();
        }
    }
    if (const auto found = parameters.find("interpolation"); found != parameters.end())
    {
        const auto *text = as_string_if(found->second);
        if (text == nullptr || !curve_interpolation_is_supported(*text))
        {
            return make_error(ErrorCode::kValidation, "Tone curve interpolation is unsupported",
                              {{"interpolation", text == nullptr ? std::string() : *text}});
        }
    }
    if (const auto found = parameters.find("channel_mode"); found != parameters.end())
    {
        const auto *text = as_string_if(found->second);
        if (text == nullptr ||
            (*text != kToneCurveChannelModeRgb && *text != kToneCurveChannelModeIndependent))
        {
            return make_error(ErrorCode::kValidation, "Tone curve channel_mode is unsupported",
                              {{"channel_mode", text == nullptr ? std::string() : *text}});
        }
    }
    if (const auto found = parameters.find("preserve_colors"); found != parameters.end())
    {
        const auto *text = as_string_if(found->second);
        if (text == nullptr ||
            (*text != kToneCurvePreserveColorsAverage && *text != kToneCurvePreserveColorsNone &&
             *text != kToneCurvePreserveColorsLuminance && *text != kToneCurvePreserveColorsMax &&
             *text != kToneCurvePreserveColorsSum && *text != kToneCurvePreserveColorsNorm &&
             *text != kToneCurvePreserveColorsPower))
        {
            return make_error(ErrorCode::kValidation, "Tone curve preserve_colors is unsupported",
                              {{"preserve_colors", text == nullptr ? std::string() : *text}});
        }
    }
    if (const auto found = parameters.find("points"); found != parameters.end())
    {
        auto points = parse_tone_curve_points(found->second);
        if (!points)
        {
            return points.error();
        }
    }
    if (const auto found = parameters.find("points_a"); found != parameters.end())
    {
        auto points = parse_tone_curve_points(found->second);
        if (!points)
        {
            return points.error();
        }
    }
    if (const auto found = parameters.find("points_b"); found != parameters.end())
    {
        auto points = parse_tone_curve_points(found->second);
        if (!points)
        {
            return points.error();
        }
    }
    return {};
}

Result<void>
validate_rgb_curve_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    std::string_view mode = kRgbLevelsModeLinked;
    if (const auto found = parameters.find("mode"); found != parameters.end())
    {
        const auto *text = as_string_if(found->second);
        if (text == nullptr ||
            (*text != kRgbLevelsModeLinked && *text != kRgbLevelsModeIndependent))
        {
            return make_error(ErrorCode::kValidation, "RGB curve mode is unsupported",
                              {{"mode", text == nullptr ? std::string() : *text}});
        }
        mode = *text;
    }
    if (const auto found = parameters.find("interpolation"); found != parameters.end())
    {
        const auto *text = as_string_if(found->second);
        if (text == nullptr || !curve_interpolation_is_supported(*text))
        {
            return make_error(ErrorCode::kValidation, "RGB curve interpolation is unsupported",
                              {{"interpolation", text == nullptr ? std::string() : *text}});
        }
    }
    std::string_view application_space = kRgbCurveApplicationSpaceSceneLinear;
    if (const auto found = parameters.find("application_space"); found != parameters.end())
    {
        const auto *text = as_string_if(found->second);
        if (text == nullptr || (*text != kRgbCurveApplicationSpaceSceneLinear &&
                                *text != kRgbCurveApplicationSpaceDisplaySrgb))
        {
            return make_error(ErrorCode::kValidation, "RGB curve application space is unsupported",
                              {{"application_space", text == nullptr ? std::string() : *text}});
        }
        application_space = *text;
    }
    std::string_view preserve_colors = kToneCurvePreserveColorsLuminance;
    if (const auto found = parameters.find("preserve_colors"); found != parameters.end())
    {
        const auto *text = as_string_if(found->second);
        if (text == nullptr ||
            (*text != kToneCurvePreserveColorsNone && *text != kToneCurvePreserveColorsLuminance &&
             *text != kToneCurvePreserveColorsMax && *text != kToneCurvePreserveColorsAverage &&
             *text != kToneCurvePreserveColorsSum && *text != kToneCurvePreserveColorsNorm &&
             *text != kToneCurvePreserveColorsPower))
        {
            return make_error(ErrorCode::kValidation, "RGB curve preserve_colors is unsupported",
                              {{"preserve_colors", text == nullptr ? std::string() : *text}});
        }
        preserve_colors = *text;
    }
    const auto parse_points = [&](const char *name) -> Result<void>
    {
        const auto found = parameters.find(name);
        if (found == parameters.end())
        {
            return {};
        }
        auto points = parse_rgb_curve_points(found->second);
        if (!points)
        {
            return points.error();
        }
        return {};
    };
    if (auto red = parse_points("points"); !red)
    {
        return red.error();
    }
    if (auto green = parse_points("points_g"); !green)
    {
        return green.error();
    }
    if (auto blue = parse_points("points_b"); !blue)
    {
        return blue.error();
    }
    bool compensate = false;
    if (const auto found = parameters.find("compensate_middle_grey"); found != parameters.end())
    {
        if (const auto *flag = std::get_if<bool>(&found->second.value); flag != nullptr)
            compensate = *flag;
    }
    bool parametric_active = false;
    for (const auto name :
         {"parametric_shadows", "parametric_darks", "parametric_lights", "parametric_highlights"})
    {
        if (const auto found = parameters.find(name); found != parameters.end())
        {
            const double value = as_number(found->second, 0.0);
            parametric_active = parametric_active || !near(value, 0.0);
        }
    }
    if (application_space == kRgbCurveApplicationSpaceDisplaySrgb &&
        (mode != kRgbLevelsModeIndependent || preserve_colors != kToneCurvePreserveColorsNone ||
         compensate || parametric_active))
    {
        return make_error(
            ErrorCode::kValidation,
            "Display-sRGB RGB curves require independent channels without scene compensation",
            {{"reason", "unsupported_display_srgb_curve_policy"}});
    }
    return {};
}

Result<void>
validate_sigmoid_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    const auto validate_one_of =
        [&](const std::string_view name,
            const std::initializer_list<std::string_view> allowed) -> Result<void>
    {
        const auto found = parameters.find(std::string(name));
        if (found == parameters.end())
        {
            return {};
        }
        const auto *text = as_string_if(found->second);
        if (text == nullptr)
        {
            return make_error(ErrorCode::kValidation, "Sigmoid parameter value is unsupported",
                              {{"parameter", std::string(name)}});
        }
        for (const auto expected : allowed)
        {
            if (*text == expected)
            {
                return {};
            }
        }
        return make_error(ErrorCode::kValidation, "Sigmoid parameter value is unsupported",
                          {{"parameter", std::string(name)}, {"value", *text}});
    };
    auto working_space = validate_one_of("working_space", {kSigmoidWorkingSpaceLinearSrgb});
    if (!working_space)
    {
        return working_space.error();
    }
    auto color_processing = validate_one_of(
        "color_processing", {kSigmoidColorProcessingPerChannel, kSigmoidColorProcessingRgbRatio});
    if (!color_processing)
    {
        return color_processing.error();
    }
    return {};
}


} // namespace ravo
