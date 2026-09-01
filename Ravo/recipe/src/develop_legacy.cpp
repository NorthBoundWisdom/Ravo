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

Result<LeftoverFlipGeometry> leftover_flip_orientation_to_geometry(const std::int32_t orientation)
{
    LeftoverFlipGeometry geometry;
    switch (orientation)
    {
    case -1:
    case 0:
        return geometry;
    case 1:
        geometry.flip_vertical = 1;
        return geometry;
    case 2:
        geometry.flip_horizontal = 1;
        return geometry;
    case 3:
        geometry.rotate_quarters = 2;
        return geometry;
    case 4:
        geometry.rotate_quarters = 1;
        geometry.flip_horizontal = 1;
        return geometry;
    case 5:
        geometry.rotate_quarters = 1;
        return geometry;
    case 6:
        geometry.rotate_quarters = 3;
        return geometry;
    case 7:
        geometry.rotate_quarters = 1;
        geometry.flip_vertical = 1;
        return geometry;
    default:
        return make_error(ErrorCode::kUnsupported,
                          "Legacy flip orientation is outside the frozen bit contract",
                          {{"legacy_operation", "flip"},
                           {"orientation", std::to_string(orientation)},
                           {"reason", "unsupported_legacy_flip_orientation"}});
    }
}

bool LeftoverCropBox::is_identity() const noexcept
{
    return near(x, 0.0) && near(y, 0.0) && near(width, 1.0) && near(height, 1.0);
}

Result<LeftoverCropBox> leftover_crop_box_to_geometry(const float left, const float top,
                                                      const float right, const float bottom)
{
    if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) ||
        !std::isfinite(bottom))
    {
        return make_error(
            ErrorCode::kUnsupported, "Legacy crop box contains a non-finite edge",
            {{"legacy_operation", "crop"}, {"reason", "unsupported_legacy_crop_box"}});
    }
    constexpr float kMin = 0.01F;
    const float cx = std::clamp(left, 0.0F, 1.0F - kMin);
    const float cy = std::clamp(top, 0.0F, 1.0F - kMin);
    const float cw = std::clamp(right, kMin, 1.0F);
    const float ch = std::clamp(bottom, kMin, 1.0F);
    const float width = cw - cx;
    const float height = ch - cy;
    if (width < kMin || height < kMin)
    {
        return make_error(
            ErrorCode::kUnsupported, "Legacy crop box is empty after leftover clamp",
            {{"legacy_operation", "crop"}, {"reason", "unsupported_legacy_crop_box"}});
    }
    LeftoverCropBox box;
    box.x = cx;
    box.y = cy;
    box.width = width;
    box.height = height;
    return box;
}

Result<PerspectiveParams> leftover_ashift_to_perspective(const float rotation,
                                                         const float lensshift_v,
                                                         const float lensshift_h, const float shear,
                                                         const bool constrain_crop)
{
    if (!std::isfinite(rotation) || !std::isfinite(lensshift_v) || !std::isfinite(lensshift_h) ||
        !std::isfinite(shear))
    {
        return make_error(
            ErrorCode::kUnsupported, "Legacy ashift head contains a non-finite value",
            {{"legacy_operation", "ashift"}, {"reason", "unsupported_legacy_ashift_head"}});
    }
    if (rotation < static_cast<float>(kPerspectiveRotationMin) ||
        rotation > static_cast<float>(kPerspectiveRotationMax) ||
        lensshift_v < static_cast<float>(kPerspectiveShiftMin) ||
        lensshift_v > static_cast<float>(kPerspectiveShiftMax) ||
        lensshift_h < static_cast<float>(kPerspectiveShiftMin) ||
        lensshift_h > static_cast<float>(kPerspectiveShiftMax) ||
        shear < static_cast<float>(kPerspectiveShearMin) ||
        shear > static_cast<float>(kPerspectiveShearMax))
    {
        return make_error(
            ErrorCode::kUnsupported, "Legacy ashift exceeds canonical perspective range",
            {{"legacy_operation", "ashift"}, {"reason", "unsupported_legacy_ashift_range"}});
    }
    return PerspectiveParams{static_cast<double>(rotation),
                             static_cast<double>(lensshift_v),
                             static_cast<double>(lensshift_h),
                             static_cast<double>(shear),
                             constrain_crop,
                             std::string(kPerspectiveInterpolationLanczos3)};
}

bool RgbLevelsParams::is_identity() const noexcept
{
    const auto identity_channel = [](const std::array<double, 3> &channel)
    { return near(channel[0], 0.0) && near(channel[1], 0.5) && near(channel[2], 1.0); };
    if (mode == kRgbLevelsModeIndependent)
    {
        return identity_channel(levels[0]) && identity_channel(levels[1]) &&
               identity_channel(levels[2]);
    }
    return identity_channel(levels[0]);
}

Result<RgbLevelsParams> leftover_rgblevels_from_v1(const std::int32_t autoscale,
                                                   const std::int32_t preserve_colors,
                                                   const std::array<float, 9> &levels)
{
    RgbLevelsParams result;
    if (autoscale == 0)
    {
        result.mode = std::string(kRgbLevelsModeLinked);
    }
    else if (autoscale == 1)
    {
        result.mode = std::string(kRgbLevelsModeIndependent);
    }
    else
    {
        return make_error(
            ErrorCode::kUnsupported, "Legacy RGB levels mode is unsupported",
            {{"legacy_operation", "rgblevels"}, {"reason", "unsupported_legacy_rgblevels_mode"}});
    }
    switch (preserve_colors)
    {
    case 0:
        result.preserve_colors = std::string(kToneCurvePreserveColorsNone);
        break;
    case 1:
        result.preserve_colors = std::string(kToneCurvePreserveColorsLuminance);
        break;
    case 2:
        result.preserve_colors = std::string(kToneCurvePreserveColorsMax);
        break;
    case 3:
        result.preserve_colors = std::string(kToneCurvePreserveColorsAverage);
        break;
    case 4:
        result.preserve_colors = std::string(kToneCurvePreserveColorsSum);
        break;
    case 5:
        result.preserve_colors = std::string(kToneCurvePreserveColorsNorm);
        break;
    case 6:
        result.preserve_colors = std::string(kToneCurvePreserveColorsPower);
        break;
    default:
        return make_error(ErrorCode::kUnsupported,
                          "Legacy RGB levels preserve-colors is unsupported",
                          {{"legacy_operation", "rgblevels"},
                           {"reason", "unsupported_legacy_rgblevels_preserve"}});
    }
    for (std::size_t channel = 0; channel < 3; ++channel)
    {
        for (std::size_t stop = 0; stop < 3; ++stop)
        {
            const float value = levels[channel * 3U + stop];
            if (!std::isfinite(value))
            {
                return make_error(ErrorCode::kUnsupported,
                                  "Legacy RGB levels contain a non-finite stop",
                                  {{"legacy_operation", "rgblevels"},
                                   {"reason", "unsupported_legacy_rgblevels_levels"}});
            }
            result.levels[channel][stop] = value;
        }
        if (!(result.levels[channel][2] > result.levels[channel][0]))
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy RGB levels white must be greater than black",
                              {{"legacy_operation", "rgblevels"},
                               {"reason", "unsupported_legacy_rgblevels_levels"}});
        }
    }
    return result;
}

std::map<std::string, ParameterValue, std::less<>>
rgb_levels_to_parameters(const RgbLevelsParams &params)
{
    return {{"mode", ParameterValue{params.mode}},
            {"preserve_colors", ParameterValue{params.preserve_colors}},
            {"black", ParameterValue{params.levels[0][0]}},
            {"grey", ParameterValue{params.levels[0][1]}},
            {"white", ParameterValue{params.levels[0][2]}},
            {"black_g", ParameterValue{params.levels[1][0]}},
            {"grey_g", ParameterValue{params.levels[1][1]}},
            {"white_g", ParameterValue{params.levels[1][2]}},
            {"black_b", ParameterValue{params.levels[2][0]}},
            {"grey_b", ParameterValue{params.levels[2][1]}},
            {"white_b", ParameterValue{params.levels[2][2]}}};
}

bool RgbCurveParams::is_identity() const noexcept
{
    if (!rgb_curve_parametric_is_identity(*this))
    {
        return false;
    }
    if (mode == kRgbLevelsModeIndependent)
    {
        return tone_curve_is_identity(channels[0]) && tone_curve_is_identity(channels[1]) &&
               tone_curve_is_identity(channels[2]);
    }
    return tone_curve_is_identity(channels[0]);
}

namespace
{

[[nodiscard]] std::int32_t rgb_curve_read_i32(const std::vector<std::uint8_t> &payload,
                                              const std::size_t offset) noexcept
{
    std::int32_t value = 0;
    std::memcpy(&value, payload.data() + offset, sizeof(value));
    return value;
}

[[nodiscard]] float rgb_curve_read_f32(const std::vector<std::uint8_t> &payload,
                                       const std::size_t offset) noexcept
{
    float value = 0.0F;
    std::memcpy(&value, payload.data() + offset, sizeof(value));
    return value;
}

} // namespace

Result<RgbCurveParams> leftover_rgbcurve_from_v1(const std::vector<std::uint8_t> &payload)
{
    constexpr std::size_t kPayloadSize = 516;
    constexpr std::size_t kMaxNodes = 20;
    constexpr std::int32_t kMonotoneHermite = 2;
    if (payload.size() != kPayloadSize)
    {
        return make_error(
            ErrorCode::kUnsupported, "Legacy RGB curve payload size is unsupported",
            {{"legacy_operation", "rgbcurve"}, {"reason", "unsupported_legacy_rgbcurve_payload"}});
    }
    RgbCurveParams result;
    const auto autoscale = rgb_curve_read_i32(payload, 504);
    const auto compensate = rgb_curve_read_i32(payload, 508);
    const auto preserve = rgb_curve_read_i32(payload, 512);
    if (autoscale == 0)
    {
        result.mode = std::string(kRgbLevelsModeLinked);
    }
    else if (autoscale == 1)
    {
        result.mode = std::string(kRgbLevelsModeIndependent);
    }
    else
    {
        return make_error(
            ErrorCode::kUnsupported, "Legacy RGB curve mode is unsupported",
            {{"legacy_operation", "rgbcurve"}, {"reason", "unsupported_legacy_rgbcurve_mode"}});
    }
    if (compensate != 0 && compensate != 1)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy RGB curve middle-grey flag is unsupported",
                          {{"legacy_operation", "rgbcurve"},
                           {"reason", "unsupported_legacy_rgbcurve_middle_grey"}});
    }
    result.compensate_middle_grey = compensate == 1;
    switch (preserve)
    {
    case 0:
        result.preserve_colors = std::string(kToneCurvePreserveColorsNone);
        break;
    case 1:
        result.preserve_colors = std::string(kToneCurvePreserveColorsLuminance);
        break;
    case 2:
        result.preserve_colors = std::string(kToneCurvePreserveColorsMax);
        break;
    case 3:
        result.preserve_colors = std::string(kToneCurvePreserveColorsAverage);
        break;
    case 4:
        result.preserve_colors = std::string(kToneCurvePreserveColorsSum);
        break;
    case 5:
        result.preserve_colors = std::string(kToneCurvePreserveColorsNorm);
        break;
    case 6:
        result.preserve_colors = std::string(kToneCurvePreserveColorsPower);
        break;
    default:
        return make_error(
            ErrorCode::kUnsupported, "Legacy RGB curve preserve-colors is unsupported",
            {{"legacy_operation", "rgbcurve"}, {"reason", "unsupported_legacy_rgbcurve_preserve"}});
    }
    for (std::size_t channel = 0; channel < 3; ++channel)
    {
        const auto count = rgb_curve_read_i32(payload, 480 + channel * 4U);
        const auto type = rgb_curve_read_i32(payload, 492 + channel * 4U);
        if (count < 2 || static_cast<std::size_t>(count) > kMaxNodes)
        {
            return make_error(ErrorCode::kUnsupported, "Legacy RGB curve node count is unsupported",
                              {{"legacy_operation", "rgbcurve"},
                               {"reason", "unsupported_legacy_rgbcurve_nodes"}});
        }
        if (type != 0 && type != 1 && type != kMonotoneHermite)
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy RGB curve interpolation is unsupported",
                              {{"legacy_operation", "rgbcurve"},
                               {"reason", "unsupported_legacy_rgbcurve_interpolation"}});
        }
        const auto interpolation = curve_interpolation_from_index(type == kMonotoneHermite ? 0 :
                                                                  type == 1                ? 1 :
                                                                                             2);
        if (channel == 0)
        {
            result.interpolation = std::string(interpolation);
        }
        else if (result.mode == kRgbLevelsModeIndependent && result.interpolation != interpolation)
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy RGB curve mixed interpolators are unsupported",
                              {{"legacy_operation", "rgbcurve"},
                               {"reason", "unsupported_legacy_rgbcurve_interpolation"}});
        }
        std::vector<ToneCurvePoint> points;
        points.reserve(static_cast<std::size_t>(count));
        for (std::int32_t index = 0; index < count; ++index)
        {
            const std::size_t offset = (channel * kMaxNodes + static_cast<std::size_t>(index)) * 8U;
            const float x = rgb_curve_read_f32(payload, offset);
            const float y = rgb_curve_read_f32(payload, offset + 4U);
            if (!std::isfinite(x) || !std::isfinite(y) || x < 0.0F || x > 1.0F || y < 0.0F ||
                y > 1.0F)
            {
                return make_error(ErrorCode::kUnsupported,
                                  "Legacy RGB curve nodes are outside the unit interval",
                                  {{"legacy_operation", "rgbcurve"},
                                   {"reason", "unsupported_legacy_rgbcurve_nodes"}});
            }
            if (!points.empty() && !(x > points.back().x))
            {
                return make_error(ErrorCode::kUnsupported,
                                  "Legacy RGB curve nodes must be strictly increasing",
                                  {{"legacy_operation", "rgbcurve"},
                                   {"reason", "unsupported_legacy_rgbcurve_nodes"}});
            }
            points.push_back({x, y});
        }
        result.channels[channel] = std::move(points);
    }
    return result;
}

std::map<std::string, ParameterValue, std::less<>>
rgb_curve_to_parameters(const RgbCurveParams &params)
{
    auto parameters = std::map<std::string, ParameterValue, std::less<>>{
        {"mode", ParameterValue{params.mode}},
        {"preserve_colors", ParameterValue{params.preserve_colors}},
        {"interpolation", ParameterValue{params.interpolation}},
        {"application_space", ParameterValue{params.application_space}},
        {"compensate_middle_grey", ParameterValue{params.compensate_middle_grey}},
        {"points", tone_curve_points_to_parameter(params.channels[0])},
        {"points_g", tone_curve_points_to_parameter(params.channels[1])},
        {"points_b", tone_curve_points_to_parameter(params.channels[2])}};
    if (!rgb_curve_parametric_is_identity(params))
    {
        parameters.emplace("parametric_shadows", ParameterValue{params.parametric_shadows});
        parameters.emplace("parametric_darks", ParameterValue{params.parametric_darks});
        parameters.emplace("parametric_lights", ParameterValue{params.parametric_lights});
        parameters.emplace("parametric_highlights", ParameterValue{params.parametric_highlights});
        parameters.emplace("parametric_split_shadows",
                           ParameterValue{params.parametric_split_shadows});
        parameters.emplace("parametric_split_mid", ParameterValue{params.parametric_split_mid});
        parameters.emplace("parametric_split_highlights",
                           ParameterValue{params.parametric_split_highlights});
    }
    return parameters;
}

Result<void> leftover_rawdenoise_from_v2(const std::vector<std::uint8_t> &payload,
                                         double &threshold,
                                         std::array<std::array<double, 5>, 4> &bands)
{
    constexpr std::size_t kPayloadSize = 164;
    if (payload.size() != kPayloadSize)
    {
        return make_error(ErrorCode::kUnsupported, "Legacy RAW denoise payload size is unsupported",
                          {{"legacy_operation", "rawdenoise"},
                           {"reason", "unsupported_legacy_rawdenoise_payload"}});
    }
    float threshold_f = 0.0F;
    std::memcpy(&threshold_f, payload.data(), sizeof(threshold_f));
    if (!std::isfinite(threshold_f) || threshold_f < 0.0F || threshold_f > 1.0F)
    {
        return make_error(ErrorCode::kUnsupported, "Legacy RAW denoise threshold is unsupported",
                          {{"legacy_operation", "rawdenoise"},
                           {"reason", "unsupported_legacy_rawdenoise_threshold"}});
    }
    threshold = threshold_f;
    constexpr float kExpectedX[5] = {0.0F, 0.25F, 0.5F, 0.75F, 1.0F};
    for (std::size_t channel = 0; channel < 4; ++channel)
    {
        for (std::size_t band = 0; band < 5; ++band)
        {
            float x = 0.0F;
            float y = 0.0F;
            std::memcpy(&x, payload.data() + 4U + (channel * 5U + band) * 4U, sizeof(x));
            std::memcpy(&y, payload.data() + 84U + (channel * 5U + band) * 4U, sizeof(y));
            if (!std::isfinite(x) || std::abs(x - kExpectedX[band]) > 1.0e-5F)
            {
                return make_error(ErrorCode::kUnsupported,
                                  "Legacy RAW denoise band positions are unsupported",
                                  {{"legacy_operation", "rawdenoise"},
                                   {"reason", "unsupported_legacy_rawdenoise_bands"}});
            }
            if (!std::isfinite(y) || y < 0.0F || y > 16.0F)
            {
                return make_error(ErrorCode::kUnsupported,
                                  "Legacy RAW denoise band values are unsupported",
                                  {{"legacy_operation", "rawdenoise"},
                                   {"reason", "unsupported_legacy_rawdenoise_bands"}});
            }
            bands[channel][band] = y;
        }
    }
    return {};
}

std::map<std::string, ParameterValue, std::less<>>
raw_denoise_to_parameters(const double threshold, const std::array<std::array<double, 5>, 4> &bands)
{
    std::map<std::string, ParameterValue, std::less<>> parameters{
        {"threshold", ParameterValue{threshold}}};
    const char *names[4] = {"all", "red", "green", "blue"};
    for (int channel = 0; channel < 4; ++channel)
    {
        for (int band = 0; band < 5; ++band)
        {
            parameters.emplace(
                std::string("y_") + names[channel] + std::to_string(band),
                ParameterValue{
                    bands[static_cast<std::size_t>(channel)][static_cast<std::size_t>(band)]});
        }
    }
    return parameters;
}


} // namespace ravo
