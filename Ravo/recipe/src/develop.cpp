#include "ravo/recipe/develop.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <string>
#include <utility>

namespace ravo
{
namespace
{

constexpr double kEpsilon = 1e-6;

[[nodiscard]] bool near(const double left, const double right) noexcept
{
    return std::abs(left - right) <= kEpsilon;
}

[[nodiscard]] double clamp_value(const double value, const double lo, const double hi) noexcept
{
    return std::clamp(value, lo, hi);
}

[[nodiscard]] double as_number(const ParameterValue &value, const double fallback)
{
    if (std::holds_alternative<double>(value.value))
    {
        return std::get<double>(value.value);
    }
    if (std::holds_alternative<std::int64_t>(value.value))
    {
        return static_cast<double>(std::get<std::int64_t>(value.value));
    }
    if (std::holds_alternative<bool>(value.value))
    {
        return std::get<bool>(value.value) ? 1.0 : 0.0;
    }
    return fallback;
}

[[nodiscard]] std::int64_t as_integer(const ParameterValue &value, const std::int64_t fallback)
{
    if (std::holds_alternative<std::int64_t>(value.value))
    {
        return std::get<std::int64_t>(value.value);
    }
    if (std::holds_alternative<double>(value.value))
    {
        return static_cast<std::int64_t>(std::llround(std::get<double>(value.value)));
    }
    if (std::holds_alternative<bool>(value.value))
    {
        return std::get<bool>(value.value) ? 1 : 0;
    }
    return fallback;
}

void add_operation(Recipe &recipe, std::string id, std::string instance_id,
                   std::map<std::string, ParameterValue, std::less<>> parameters)
{
    recipe.operations.push_back(
        {std::move(id), 1, std::move(instance_id), true, std::move(parameters), std::nullopt});
}

[[nodiscard]] std::int64_t flag01(const std::int64_t value) noexcept
{
    return value != 0 ? 1 : 0;
}

struct ToneCurveSpline
{
    std::vector<ToneCurvePoint> points;
    std::vector<double> tangents;
};

[[nodiscard]] ToneCurveSpline make_tone_curve_spline(const std::vector<ToneCurvePoint> &points)
{
    ToneCurveSpline spline;
    spline.points = points;
    if (points.size() < 2)
    {
        return spline;
    }
    const auto count = points.size();
    std::vector<double> delta(count - 1U);
    for (std::size_t index = 0; index + 1 < count; ++index)
    {
        const double dx = points[index + 1U].x - points[index].x;
        delta[index] = dx > 1e-12 ? (points[index + 1U].y - points[index].y) / dx : 0.0;
    }
    spline.tangents.assign(count, 0.0);
    spline.tangents.front() = delta.front();
    spline.tangents.back() = delta.back();
    for (std::size_t index = 1; index + 1 < count; ++index)
    {
        if (delta[index - 1U] * delta[index] <= 0.0)
        {
            spline.tangents[index] = 0.0;
        }
        else
        {
            spline.tangents[index] = 0.5 * (delta[index - 1U] + delta[index]);
        }
    }
    for (std::size_t index = 0; index + 1 < count; ++index)
    {
        if (std::abs(delta[index]) <= 1e-12)
        {
            spline.tangents[index] = 0.0;
            spline.tangents[index + 1U] = 0.0;
            continue;
        }
        const double alpha = spline.tangents[index] / delta[index];
        const double beta = spline.tangents[index + 1U] / delta[index];
        const double sumsq = alpha * alpha + beta * beta;
        if (sumsq > 9.0)
        {
            const double tau = 3.0 / std::sqrt(sumsq);
            spline.tangents[index] = tau * alpha * delta[index];
            spline.tangents[index + 1U] = tau * beta * delta[index];
        }
    }
    return spline;
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
    std::size_t index = 0;
    while (index + 2 < spline.points.size() && x > spline.points[index + 1U].x)
    {
        ++index;
    }
    const auto &left = spline.points[index];
    const auto &right = spline.points[index + 1U];
    const double dx = right.x - left.x;
    if (dx <= 1e-12)
    {
        return left.y;
    }
    const double t = (x - left.x) / dx;
    const double t2 = t * t;
    const double t3 = t2 * t;
    const double h00 = 2.0 * t3 - 3.0 * t2 + 1.0;
    const double h10 = t3 - 2.0 * t2 + t;
    const double h01 = -2.0 * t3 + 3.0 * t2;
    const double h11 = t3 - t2;
    return h00 * left.y + h10 * dx * spline.tangents[index] + h01 * right.y +
           h11 * dx * spline.tangents[index + 1U];
}

[[nodiscard]] const std::string *as_string_if(const ParameterValue &value)
{
    return std::get_if<std::string>(&value.value);
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

double evaluate_tone_curve(const std::vector<ToneCurvePoint> &points, const double x) noexcept
{
    if (tone_curve_is_identity(points))
    {
        return std::clamp(x, 0.0, 1.0);
    }
    return evaluate_tone_curve_spline(make_tone_curve_spline(points), x);
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
    }
    return kToneCurveWorkingSpaceSrgb;
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
        return make_error(ErrorCode::kValidation, "Tone curve must have between 2 and 16 points",
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

Result<void> validate_tone_curve_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters)
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
        if (text == nullptr || *text != kToneCurveInterpolationMonotoneHermite)
        {
            return make_error(ErrorCode::kValidation, "Tone curve interpolation is unsupported",
                              {{"interpolation", text == nullptr ? std::string() : *text}});
        }
    }
    if (const auto found = parameters.find("channel_mode"); found != parameters.end())
    {
        const auto *text = as_string_if(found->second);
        if (text == nullptr || *text != kToneCurveChannelModeRgb)
        {
            return make_error(ErrorCode::kValidation, "Tone curve channel_mode is unsupported",
                              {{"channel_mode", text == nullptr ? std::string() : *text}});
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
    return {};
}

void clamp_develop(DevelopParams &params) noexcept
{
    params.temperature =
        clamp_value(params.temperature, kDevelopTemperatureMin, kDevelopTemperatureMax);
    params.tint = clamp_value(params.tint, -150.0, 150.0);
    params.exposure_ev = clamp_value(params.exposure_ev, -10.0, 10.0);
    params.contrast = clamp_value(params.contrast, -1.0, 1.0);
    params.highlights = clamp_value(params.highlights, -1.0, 1.0);
    params.shadows = clamp_value(params.shadows, -1.0, 1.0);
    params.whites = clamp_value(params.whites, -1.0, 1.0);
    params.blacks = clamp_value(params.blacks, -1.0, 1.0);
    params.vibrance = clamp_value(params.vibrance, -1.0, 1.0);
    params.saturation = clamp_value(params.saturation, -1.0, 1.0);
    params.rotate_quarters = ((params.rotate_quarters % 4) + 4) % 4;
    params.flip_horizontal = flag01(params.flip_horizontal);
    params.flip_vertical = flag01(params.flip_vertical);
    params.straighten_degrees =
        clamp_value(params.straighten_degrees, kDevelopStraightenMin, kDevelopStraightenMax);
    params.crop_width = clamp_value(params.crop_width, 0.01, 1.0);
    params.crop_height = clamp_value(params.crop_height, 0.01, 1.0);
    params.crop_x = clamp_value(params.crop_x, 0.0, 1.0 - params.crop_width);
    params.crop_y = clamp_value(params.crop_y, 0.0, 1.0 - params.crop_height);
    params.sharpen = clamp_value(params.sharpen, 0.0, 2.0);
    params.sharpen_radius = clamp_value(params.sharpen_radius, 0.0, 12.0);
    params.clarity = clamp_value(params.clarity, -1.0, 1.0);
    params.vignette = clamp_value(params.vignette, 0.0, 1.0);
    params.grain = clamp_value(params.grain, 0.0, 1.0);
    params.bloom = clamp_value(params.bloom, 0.0, 1.0);
    params.soften = clamp_value(params.soften, 0.0, 1.0);
    params.dehaze = clamp_value(params.dehaze, -1.0, 1.0);
    params.velvia = clamp_value(params.velvia, 0.0, 1.0);
    params.lift = clamp_value(params.lift, -1.0, 1.0);
    params.color_gamma = clamp_value(params.color_gamma, -1.0, 1.0);
    params.gain = clamp_value(params.gain, -1.0, 1.0);
    params.color_contrast = clamp_value(params.color_contrast, -1.0, 1.0);
    params.monochrome = clamp_value(params.monochrome, 0.0, 1.0);
    params.split_shadows_hue = clamp_value(params.split_shadows_hue, 0.0, 1.0);
    params.split_highlights_hue = clamp_value(params.split_highlights_hue, 0.0, 1.0);
    params.split_balance = clamp_value(params.split_balance, 0.0, 1.0);
    params.split_amount = clamp_value(params.split_amount, 0.0, 1.0);
    params.gamma = clamp_value(params.gamma, 0.2, 3.0);
    if (params.tone_curve_working_space != kToneCurveWorkingSpaceSrgb &&
        params.tone_curve_working_space != kToneCurveWorkingSpaceLinearRgb)
    {
        params.tone_curve_working_space = std::string(kToneCurveWorkingSpaceSrgb);
    }
    clamp_tone_curve(params.tone_curve);
}

bool DevelopParams::is_identity() const noexcept
{
    return near(temperature, kDevelopTemperatureDefault) && near(tint, 0.0) &&
           near(exposure_ev, 0.0) && near(contrast, 0.0) && near(highlights, 0.0) &&
           near(shadows, 0.0) && near(whites, 0.0) && near(blacks, 0.0) && near(vibrance, 0.0) &&
           near(saturation, 0.0) && rotate_quarters % 4 == 0 && flip_horizontal == 0 &&
           flip_vertical == 0 && near(straighten_degrees, 0.0) && near(crop_x, 0.0) &&
           near(crop_y, 0.0) && near(crop_width, 1.0) && near(crop_height, 1.0) &&
           near(sharpen, 0.0) && near(clarity, 0.0) && near(vignette, 0.0) && near(grain, 0.0) &&
           near(bloom, 0.0) && near(soften, 0.0) && near(dehaze, 0.0) && near(velvia, 0.0) &&
           near(lift, 0.0) && near(color_gamma, 0.0) && near(gain, 0.0) &&
           near(color_contrast, 0.0) && near(monochrome, 0.0) && near(split_amount, 0.0) &&
           near(gamma, kDevelopGammaDefault) && tone_curve_is_identity(tone_curve);
}

bool apply_develop_field(DevelopParams &params, const std::string_view name, const double value)
{
    if (name == "temperature")
    {
        params.temperature = value;
    }
    else if (name == "tint")
    {
        params.tint = value;
    }
    else if (name == "exposure")
    {
        params.exposure_ev = value;
    }
    else if (name == "contrast")
    {
        params.contrast = value;
    }
    else if (name == "highlights")
    {
        params.highlights = value;
    }
    else if (name == "shadows")
    {
        params.shadows = value;
    }
    else if (name == "whites")
    {
        params.whites = value;
    }
    else if (name == "blacks")
    {
        params.blacks = value;
    }
    else if (name == "vibrance")
    {
        params.vibrance = value;
    }
    else if (name == "saturation")
    {
        params.saturation = value;
    }
    else if (name == "straighten")
    {
        params.straighten_degrees = value;
    }
    else if (name == "cropX")
    {
        params.crop_x = value;
    }
    else if (name == "cropY")
    {
        params.crop_y = value;
    }
    else if (name == "cropWidth")
    {
        params.crop_width = value;
    }
    else if (name == "cropHeight")
    {
        params.crop_height = value;
    }
    else if (name == "sharpen")
    {
        params.sharpen = value;
    }
    else if (name == "sharpenRadius")
    {
        params.sharpen_radius = value;
    }
    else if (name == "clarity")
    {
        params.clarity = value;
    }
    else if (name == "vignette")
    {
        params.vignette = value;
    }
    else if (name == "grain")
    {
        params.grain = value;
    }
    else if (name == "bloom")
    {
        params.bloom = value;
    }
    else if (name == "soften")
    {
        params.soften = value;
    }
    else if (name == "dehaze")
    {
        params.dehaze = value;
    }
    else if (name == "velvia")
    {
        params.velvia = value;
    }
    else if (name == "lift")
    {
        params.lift = value;
    }
    else if (name == "colorGamma")
    {
        params.color_gamma = value;
    }
    else if (name == "gain")
    {
        params.gain = value;
    }
    else if (name == "colorContrast")
    {
        params.color_contrast = value;
    }
    else if (name == "monochrome")
    {
        params.monochrome = value;
    }
    else if (name == "splitShadowsHue")
    {
        params.split_shadows_hue = value;
    }
    else if (name == "splitHighlightsHue")
    {
        params.split_highlights_hue = value;
    }
    else if (name == "splitBalance")
    {
        params.split_balance = value;
    }
    else if (name == "splitAmount")
    {
        params.split_amount = value;
    }
    else if (name == "gamma")
    {
        params.gamma = value;
    }
    else
    {
        return false;
    }
    clamp_develop(params);
    return true;
}

bool reset_develop_field(DevelopParams &params, const std::string_view name)
{
    DevelopParams identity;
    if (name == "temperature")
    {
        params.temperature = identity.temperature;
    }
    else if (name == "tint")
    {
        params.tint = identity.tint;
    }
    else if (name == "exposure")
    {
        params.exposure_ev = identity.exposure_ev;
    }
    else if (name == "contrast")
    {
        params.contrast = identity.contrast;
    }
    else if (name == "highlights")
    {
        params.highlights = identity.highlights;
    }
    else if (name == "shadows")
    {
        params.shadows = identity.shadows;
    }
    else if (name == "whites")
    {
        params.whites = identity.whites;
    }
    else if (name == "blacks")
    {
        params.blacks = identity.blacks;
    }
    else if (name == "vibrance")
    {
        params.vibrance = identity.vibrance;
    }
    else if (name == "saturation")
    {
        params.saturation = identity.saturation;
    }
    else if (name == "rotate")
    {
        params.rotate_quarters = 0;
    }
    else if (name == "flip")
    {
        params.flip_horizontal = 0;
        params.flip_vertical = 0;
    }
    else if (name == "straighten")
    {
        params.straighten_degrees = identity.straighten_degrees;
    }
    else if (name == "crop" || name == "cropX" || name == "cropY" || name == "cropWidth" ||
             name == "cropHeight")
    {
        params.crop_x = 0.0;
        params.crop_y = 0.0;
        params.crop_width = 1.0;
        params.crop_height = 1.0;
    }
    else if (name == "sharpen" || name == "sharpenRadius")
    {
        params.sharpen = identity.sharpen;
        if (name == "sharpenRadius")
        {
            params.sharpen_radius = identity.sharpen_radius;
        }
    }
    else if (name == "clarity")
    {
        params.clarity = identity.clarity;
    }
    else if (name == "vignette")
    {
        params.vignette = identity.vignette;
    }
    else if (name == "grain")
    {
        params.grain = identity.grain;
    }
    else if (name == "bloom")
    {
        params.bloom = identity.bloom;
    }
    else if (name == "soften")
    {
        params.soften = identity.soften;
    }
    else if (name == "dehaze")
    {
        params.dehaze = identity.dehaze;
    }
    else if (name == "velvia")
    {
        params.velvia = identity.velvia;
    }
    else if (name == "lift")
    {
        params.lift = identity.lift;
    }
    else if (name == "colorGamma")
    {
        params.color_gamma = identity.color_gamma;
    }
    else if (name == "gain")
    {
        params.gain = identity.gain;
    }
    else if (name == "colorContrast")
    {
        params.color_contrast = identity.color_contrast;
    }
    else if (name == "monochrome")
    {
        params.monochrome = identity.monochrome;
    }
    else if (name == "splitShadowsHue")
    {
        params.split_shadows_hue = identity.split_shadows_hue;
    }
    else if (name == "splitHighlightsHue")
    {
        params.split_highlights_hue = identity.split_highlights_hue;
    }
    else if (name == "splitBalance")
    {
        params.split_balance = identity.split_balance;
    }
    else if (name == "splitAmount")
    {
        params.split_amount = identity.split_amount;
    }
    else if (name == "gamma")
    {
        params.gamma = identity.gamma;
    }
    else if (name == "toneCurve")
    {
        params.tone_curve.clear();
        params.tone_curve_working_space = std::string(kToneCurveWorkingSpaceSrgb);
    }
    else
    {
        return false;
    }
    clamp_develop(params);
    return true;
}

bool reset_develop_section(DevelopParams &params, const std::string_view section)
{
    DevelopParams identity;
    if (section == "geometry")
    {
        params.rotate_quarters = 0;
        params.flip_horizontal = 0;
        params.flip_vertical = 0;
        params.straighten_degrees = 0.0;
        params.crop_x = 0.0;
        params.crop_y = 0.0;
        params.crop_width = 1.0;
        params.crop_height = 1.0;
    }
    else if (section == "whiteBalance")
    {
        params.temperature = identity.temperature;
        params.tint = identity.tint;
    }
    else if (section == "light")
    {
        params.exposure_ev = identity.exposure_ev;
        params.contrast = identity.contrast;
        params.highlights = identity.highlights;
        params.shadows = identity.shadows;
        params.whites = identity.whites;
        params.blacks = identity.blacks;
        params.gamma = identity.gamma;
        params.tone_curve.clear();
        params.tone_curve_working_space = std::string(kToneCurveWorkingSpaceSrgb);
    }
    else if (section == "color")
    {
        params.vibrance = identity.vibrance;
        params.saturation = identity.saturation;
        params.velvia = identity.velvia;
        params.lift = identity.lift;
        params.color_gamma = identity.color_gamma;
        params.gain = identity.gain;
        params.color_contrast = identity.color_contrast;
        params.monochrome = identity.monochrome;
        params.split_shadows_hue = identity.split_shadows_hue;
        params.split_highlights_hue = identity.split_highlights_hue;
        params.split_balance = identity.split_balance;
        params.split_amount = identity.split_amount;
    }
    else if (section == "detail")
    {
        params.sharpen = identity.sharpen;
        params.sharpen_radius = identity.sharpen_radius;
        params.clarity = identity.clarity;
        params.grain = identity.grain;
    }
    else if (section == "effects")
    {
        params.vignette = identity.vignette;
        params.bloom = identity.bloom;
        params.soften = identity.soften;
        params.dehaze = identity.dehaze;
    }
    else
    {
        return false;
    }
    clamp_develop(params);
    return true;
}

bool apply_crop_aspect(DevelopParams &params, const std::string_view aspect)
{
    if (aspect == "free")
    {
        return true;
    }
    double ratio = 0.0;
    if (aspect == "1:1")
    {
        ratio = 1.0;
    }
    else if (aspect == "3:2")
    {
        ratio = 3.0 / 2.0;
    }
    else if (aspect == "4:3")
    {
        ratio = 4.0 / 3.0;
    }
    else if (aspect == "5:4")
    {
        ratio = 5.0 / 4.0;
    }
    else if (aspect == "16:9")
    {
        ratio = 16.0 / 9.0;
    }
    else
    {
        return false;
    }

    const double box_w = params.crop_width;
    const double box_h = params.crop_height;
    const double box_x = params.crop_x;
    const double box_y = params.crop_y;
    double new_w = box_w;
    double new_h = box_h;
    if (box_w / std::max(box_h, kEpsilon) > ratio)
    {
        new_w = box_h * ratio;
        new_h = box_h;
    }
    else
    {
        new_w = box_w;
        new_h = box_w / ratio;
    }
    params.crop_width = new_w;
    params.crop_height = new_h;
    params.crop_x = box_x + (box_w - new_w) * 0.5;
    params.crop_y = box_y + (box_h - new_h) * 0.5;
    clamp_develop(params);
    return true;
}

void transform_crop_for_quarter_turns(DevelopParams &params, int turns_cw) noexcept
{
    turns_cw = ((turns_cw % 4) + 4) % 4;
    for (int turn = 0; turn < turns_cw; ++turn)
    {
        const double x = params.crop_x;
        const double y = params.crop_y;
        const double width = params.crop_width;
        const double height = params.crop_height;
        params.crop_x = 1.0 - y - height;
        params.crop_y = x;
        params.crop_width = height;
        params.crop_height = width;
    }
    clamp_develop(params);
}

void transform_crop_for_flip(DevelopParams &params, const bool horizontal,
                             const bool vertical) noexcept
{
    if (horizontal)
    {
        params.crop_x = 1.0 - params.crop_x - params.crop_width;
    }
    if (vertical)
    {
        params.crop_y = 1.0 - params.crop_y - params.crop_height;
    }
    clamp_develop(params);
}

double working_image_aspect(const std::int64_t rotate_quarters, const double source_aspect) noexcept
{
    const double aspect = source_aspect > kEpsilon ? source_aspect : 1.5;
    if (((rotate_quarters % 4) + 4) % 4 % 2 != 0)
    {
        return 1.0 / aspect;
    }
    return aspect;
}

namespace
{

[[nodiscard]] double max_inscribed_normalized_height(const double degrees,
                                                     const double image_aspect,
                                                     const double crop_aspect) noexcept
{
    const double ratio = std::max(crop_aspect, kEpsilon);
    const double aspect = std::max(image_aspect, kEpsilon);
    double height = std::min(1.0, 1.0 / ratio);
    if (std::abs(degrees) < 1e-4)
    {
        return height;
    }
    const double rad = -degrees * std::numbers::pi / 180.0;
    const double inv_c = std::cos(rad);
    const double inv_s = std::sin(rad);
    const double terms[4] = {
        std::abs(inv_c * ratio - inv_s / aspect), std::abs(inv_c * ratio + inv_s / aspect),
        std::abs(inv_s * ratio * aspect + inv_c), std::abs(inv_s * ratio * aspect - inv_c)};
    for (const double term : terms)
    {
        if (term > kEpsilon)
        {
            height = std::min(height, 1.0 / term);
        }
    }
    return clamp_value(height, 0.01, 1.0);
}

} // namespace

void map_straighten_normalized(const double x, const double y, const double straighten_degrees,
                               const double working_aspect, const bool inverse, double &ox,
                               double &oy) noexcept
{
    const double rad =
        (inverse ? -straighten_degrees : straighten_degrees) * std::numbers::pi / 180.0;
    const double cosine = std::cos(rad);
    const double sine = std::sin(rad);
    const double aspect = std::max(working_aspect, kEpsilon);
    const double dx = (x - 0.5) * aspect;
    const double dy = y - 0.5;
    ox = (cosine * dx - sine * dy) / aspect + 0.5;
    oy = sine * dx + cosine * dy + 0.5;
}

void straightened_source_quad(const double straighten_degrees, const double working_aspect,
                              std::array<double, 8> &corners) noexcept
{
    constexpr double kSource[8] = {0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0};
    for (int index = 0; index < 4; ++index)
    {
        map_straighten_normalized(kSource[index * 2], kSource[index * 2 + 1], straighten_degrees,
                                  working_aspect, false,
                                  corners[static_cast<std::size_t>(index * 2)],
                                  corners[static_cast<std::size_t>(index * 2 + 1)]);
    }
}

void inscribed_crop_for_straighten(const double straighten_degrees, const double working_aspect,
                                   const double crop_aspect_norm, double &x, double &y,
                                   double &width, double &height) noexcept
{
    height = max_inscribed_normalized_height(straighten_degrees, working_aspect, crop_aspect_norm);
    width = crop_aspect_norm * height;
    if (width > 1.0)
    {
        width = 1.0;
        height = width / std::max(crop_aspect_norm, kEpsilon);
    }
    x = (1.0 - width) * 0.5;
    y = (1.0 - height) * 0.5;
}

void constrain_crop_to_straighten(DevelopParams &params, const double working_aspect) noexcept
{
    const double ratio = params.crop_width / std::max(params.crop_height, kEpsilon);
    double limit_x = 0.0;
    double limit_y = 0.0;
    double limit_w = 1.0;
    double limit_h = 1.0;
    inscribed_crop_for_straighten(params.straighten_degrees, working_aspect, ratio, limit_x,
                                  limit_y, limit_w, limit_h);
    double width = std::min(params.crop_width, limit_w);
    double height = std::min(params.crop_height, limit_h);
    if (width / std::max(height, kEpsilon) > ratio)
    {
        width = height * ratio;
    }
    else
    {
        height = width / std::max(ratio, kEpsilon);
    }
    const double center_x = params.crop_x + params.crop_width * 0.5;
    const double center_y = params.crop_y + params.crop_height * 0.5;
    params.crop_width = width;
    params.crop_height = height;
    params.crop_x = clamp_value(center_x - width * 0.5, limit_x, limit_x + limit_w - width);
    params.crop_y = clamp_value(center_y - height * 0.5, limit_y, limit_y + limit_h - height);
    clamp_develop(params);
}

void fit_crop_to_straighten(DevelopParams &params, const double working_aspect) noexcept
{
    const double ratio = params.crop_width / std::max(params.crop_height, kEpsilon);
    inscribed_crop_for_straighten(params.straighten_degrees, working_aspect, ratio, params.crop_x,
                                  params.crop_y, params.crop_width, params.crop_height);
    clamp_develop(params);
}

void strip_crop_operations(Recipe &recipe)
{
    recipe.operations.erase(std::remove_if(recipe.operations.begin(), recipe.operations.end(),
                                           [](const OperationInstance &operation)
                                           { return operation.id == "ravo.geometry.crop"; }),
                            recipe.operations.end());
}

void strip_straighten_operations(Recipe &recipe)
{
    recipe.operations.erase(std::remove_if(recipe.operations.begin(), recipe.operations.end(),
                                           [](const OperationInstance &operation)
                                           { return operation.id == "ravo.geometry.straighten"; }),
                            recipe.operations.end());
}

Result<Recipe> recipe_from_develop(AssetDescriptor asset, const DevelopParams &params)
{
    DevelopParams clamped = params;
    clamp_develop(clamped);
    Recipe recipe;
    recipe.asset = std::move(asset);
    if (!near(clamped.temperature, kDevelopTemperatureDefault) || !near(clamped.tint, 0.0))
    {
        add_operation(recipe, "ravo.color.white_balance", "white-balance-1",
                      {{"temperature", ParameterValue{clamped.temperature}},
                       {"tint", ParameterValue{clamped.tint}}});
    }
    if (!near(clamped.exposure_ev, 0.0))
    {
        add_operation(recipe, "ravo.core.exposure", "exposure-1",
                      {{"exposure_ev", ParameterValue{clamped.exposure_ev}}});
    }
    if (!near(clamped.highlights, 0.0))
    {
        add_operation(recipe, "ravo.core.highlights", "highlights-1",
                      {{"amount", ParameterValue{clamped.highlights}}});
    }
    if (!near(clamped.shadows, 0.0))
    {
        add_operation(recipe, "ravo.core.shadows", "shadows-1",
                      {{"amount", ParameterValue{clamped.shadows}}});
    }
    if (!near(clamped.whites, 0.0))
    {
        add_operation(recipe, "ravo.core.whites", "whites-1",
                      {{"amount", ParameterValue{clamped.whites}}});
    }
    if (!near(clamped.blacks, 0.0))
    {
        add_operation(recipe, "ravo.core.blacks", "blacks-1",
                      {{"amount", ParameterValue{clamped.blacks}}});
    }
    if (!near(clamped.contrast, 0.0))
    {
        add_operation(recipe, "ravo.core.contrast", "contrast-1",
                      {{"amount", ParameterValue{clamped.contrast}}});
    }
    if (!near(clamped.gamma, kDevelopGammaDefault))
    {
        add_operation(recipe, "ravo.core.gamma", "gamma-1",
                      {{"gamma", ParameterValue{clamped.gamma}}});
    }
    if (!tone_curve_is_identity(clamped.tone_curve))
    {
        add_operation(recipe, "ravo.core.tonecurve", "tonecurve-1",
                      {{"working_space", ParameterValue{clamped.tone_curve_working_space}},
                       {"interpolation",
                        ParameterValue{std::string(kToneCurveInterpolationMonotoneHermite)}},
                       {"channel_mode", ParameterValue{std::string(kToneCurveChannelModeRgb)}},
                       {"points", tone_curve_points_to_parameter(clamped.tone_curve)}});
    }
    if (!near(clamped.lift, 0.0) || !near(clamped.color_gamma, 0.0) || !near(clamped.gain, 0.0))
    {
        add_operation(recipe, "ravo.color.colorbalance", "colorbalance-1",
                      {{"lift", ParameterValue{clamped.lift}},
                       {"gamma", ParameterValue{clamped.color_gamma}},
                       {"gain", ParameterValue{clamped.gain}}});
    }
    if (!near(clamped.color_contrast, 0.0))
    {
        add_operation(recipe, "ravo.color.colorcontrast", "colorcontrast-1",
                      {{"amount", ParameterValue{clamped.color_contrast}}});
    }
    if (!near(clamped.velvia, 0.0))
    {
        add_operation(recipe, "ravo.color.velvia", "velvia-1",
                      {{"amount", ParameterValue{clamped.velvia}}, {"bias", ParameterValue{1.0}}});
    }
    if (!near(clamped.vibrance, 0.0))
    {
        add_operation(recipe, "ravo.color.vibrance", "vibrance-1",
                      {{"amount", ParameterValue{clamped.vibrance}}});
    }
    if (!near(clamped.saturation, 0.0))
    {
        add_operation(recipe, "ravo.color.saturation", "saturation-1",
                      {{"amount", ParameterValue{clamped.saturation}}});
    }
    if (!near(clamped.monochrome, 0.0))
    {
        add_operation(recipe, "ravo.color.monochrome", "monochrome-1",
                      {{"amount", ParameterValue{clamped.monochrome}}});
    }
    if (!near(clamped.split_amount, 0.0))
    {
        add_operation(recipe, "ravo.color.splittoning", "splittoning-1",
                      {{"shadows_hue", ParameterValue{clamped.split_shadows_hue}},
                       {"highlights_hue", ParameterValue{clamped.split_highlights_hue}},
                       {"balance", ParameterValue{clamped.split_balance}},
                       {"amount", ParameterValue{clamped.split_amount}}});
    }
    if (!near(clamped.sharpen, 0.0))
    {
        add_operation(recipe, "ravo.detail.sharpen", "sharpen-1",
                      {{"amount", ParameterValue{clamped.sharpen}},
                       {"radius", ParameterValue{clamped.sharpen_radius}},
                       {"threshold", ParameterValue{0.0}}});
    }
    if (!near(clamped.clarity, 0.0))
    {
        add_operation(recipe, "ravo.detail.clarity", "clarity-1",
                      {{"amount", ParameterValue{clamped.clarity}}});
    }
    if (!near(clamped.bloom, 0.0))
    {
        add_operation(recipe, "ravo.effect.bloom", "bloom-1",
                      {{"amount", ParameterValue{clamped.bloom}}});
    }
    if (!near(clamped.soften, 0.0))
    {
        add_operation(recipe, "ravo.effect.soften", "soften-1",
                      {{"amount", ParameterValue{clamped.soften}}});
    }
    if (!near(clamped.dehaze, 0.0))
    {
        add_operation(recipe, "ravo.effect.dehaze", "dehaze-1",
                      {{"amount", ParameterValue{clamped.dehaze}}});
    }
    if (!near(clamped.vignette, 0.0))
    {
        add_operation(recipe, "ravo.effect.vignette", "vignette-1",
                      {{"amount", ParameterValue{clamped.vignette}},
                       {"midpoint", ParameterValue{0.5}},
                       {"falloff", ParameterValue{0.5}}});
    }
    if (!near(clamped.grain, 0.0))
    {
        add_operation(recipe, "ravo.effect.grain", "grain-1",
                      {{"amount", ParameterValue{clamped.grain}}});
    }
    if (clamped.rotate_quarters % 4 != 0)
    {
        add_operation(recipe, "ravo.geometry.rotate", "rotate-1",
                      {{"quarters", ParameterValue{clamped.rotate_quarters % 4}}});
    }
    if (clamped.flip_horizontal != 0 || clamped.flip_vertical != 0)
    {
        add_operation(recipe, "ravo.geometry.flip", "flip-1",
                      {{"horizontal", ParameterValue{clamped.flip_horizontal}},
                       {"vertical", ParameterValue{clamped.flip_vertical}}});
    }
    if (!near(clamped.straighten_degrees, 0.0))
    {
        add_operation(recipe, "ravo.geometry.straighten", "straighten-1",
                      {{"degrees", ParameterValue{clamped.straighten_degrees}}});
    }
    if (!near(clamped.crop_x, 0.0) || !near(clamped.crop_y, 0.0) ||
        !near(clamped.crop_width, 1.0) || !near(clamped.crop_height, 1.0))
    {
        add_operation(recipe, "ravo.geometry.crop", "crop-1",
                      {{"x", ParameterValue{clamped.crop_x}},
                       {"y", ParameterValue{clamped.crop_y}},
                       {"width", ParameterValue{clamped.crop_width}},
                       {"height", ParameterValue{clamped.crop_height}}});
    }
    return recipe;
}

Result<DevelopParams> develop_from_recipe(const Recipe &recipe)
{
    DevelopParams params;
    for (const auto &operation : recipe.operations)
    {
        if (!operation.enabled)
        {
            continue;
        }
        const auto number = [&](const std::string_view name, const double fallback)
        {
            const auto found = operation.parameters.find(std::string(name));
            if (found == operation.parameters.end())
            {
                return fallback;
            }
            return as_number(found->second, fallback);
        };
        const auto integer = [&](const std::string_view name, const std::int64_t fallback)
        {
            const auto found = operation.parameters.find(std::string(name));
            if (found == operation.parameters.end())
            {
                return fallback;
            }
            return as_integer(found->second, fallback);
        };
        if (operation.id == "ravo.color.white_balance")
        {
            params.temperature = number("temperature", params.temperature);
            params.tint = number("tint", params.tint);
        }
        else if (operation.id == "ravo.core.exposure")
        {
            params.exposure_ev = number("exposure_ev", params.exposure_ev);
        }
        else if (operation.id == "ravo.core.contrast")
        {
            params.contrast = number("amount", params.contrast);
        }
        else if (operation.id == "ravo.core.highlights")
        {
            params.highlights = number("amount", params.highlights);
        }
        else if (operation.id == "ravo.core.shadows")
        {
            params.shadows = number("amount", params.shadows);
        }
        else if (operation.id == "ravo.core.whites")
        {
            params.whites = number("amount", params.whites);
        }
        else if (operation.id == "ravo.core.blacks")
        {
            params.blacks = number("amount", params.blacks);
        }
        else if (operation.id == "ravo.core.gamma")
        {
            params.gamma = number("gamma", params.gamma);
        }
        else if (operation.id == "ravo.core.tonecurve")
        {
            if (const auto found = operation.parameters.find("working_space");
                found != operation.parameters.end())
            {
                if (const auto *text = as_string_if(found->second); text != nullptr)
                {
                    params.tone_curve_working_space = *text;
                }
            }
            if (const auto found = operation.parameters.find("points");
                found != operation.parameters.end())
            {
                auto points = parse_tone_curve_points(found->second);
                if (!points)
                {
                    return points.error();
                }
                params.tone_curve = std::move(points).value();
            }
        }
        else if (operation.id == "ravo.color.vibrance")
        {
            params.vibrance = number("amount", params.vibrance);
        }
        else if (operation.id == "ravo.color.saturation")
        {
            params.saturation = number("amount", params.saturation);
        }
        else if (operation.id == "ravo.color.velvia")
        {
            params.velvia = number("amount", params.velvia);
        }
        else if (operation.id == "ravo.color.colorbalance")
        {
            params.lift = number("lift", params.lift);
            params.color_gamma = number("gamma", params.color_gamma);
            params.gain = number("gain", params.gain);
        }
        else if (operation.id == "ravo.color.colorcontrast")
        {
            params.color_contrast = number("amount", params.color_contrast);
        }
        else if (operation.id == "ravo.color.monochrome")
        {
            params.monochrome = number("amount", params.monochrome);
        }
        else if (operation.id == "ravo.color.splittoning")
        {
            params.split_shadows_hue = number("shadows_hue", params.split_shadows_hue);
            params.split_highlights_hue = number("highlights_hue", params.split_highlights_hue);
            params.split_balance = number("balance", params.split_balance);
            params.split_amount = number("amount", params.split_amount);
        }
        else if (operation.id == "ravo.detail.sharpen")
        {
            params.sharpen = number("amount", params.sharpen);
            params.sharpen_radius = number("radius", params.sharpen_radius);
        }
        else if (operation.id == "ravo.detail.clarity")
        {
            params.clarity = number("amount", params.clarity);
        }
        else if (operation.id == "ravo.effect.vignette")
        {
            params.vignette = number("amount", params.vignette);
        }
        else if (operation.id == "ravo.effect.grain")
        {
            params.grain = number("amount", params.grain);
        }
        else if (operation.id == "ravo.effect.bloom")
        {
            params.bloom = number("amount", params.bloom);
        }
        else if (operation.id == "ravo.effect.soften")
        {
            params.soften = number("amount", params.soften);
        }
        else if (operation.id == "ravo.effect.dehaze")
        {
            params.dehaze = number("amount", params.dehaze);
        }
        else if (operation.id == "ravo.geometry.rotate")
        {
            params.rotate_quarters = integer("quarters", 0) % 4;
            if (params.rotate_quarters < 0)
            {
                params.rotate_quarters += 4;
            }
        }
        else if (operation.id == "ravo.geometry.flip")
        {
            params.flip_horizontal = flag01(integer("horizontal", 0));
            params.flip_vertical = flag01(integer("vertical", 0));
        }
        else if (operation.id == "ravo.geometry.straighten")
        {
            params.straighten_degrees = number("degrees", params.straighten_degrees);
        }
        else if (operation.id == "ravo.geometry.crop")
        {
            params.crop_x = number("x", params.crop_x);
            params.crop_y = number("y", params.crop_y);
            params.crop_width = number("width", params.crop_width);
            params.crop_height = number("height", params.crop_height);
        }
    }
    clamp_develop(params);
    return params;
}

} // namespace ravo
