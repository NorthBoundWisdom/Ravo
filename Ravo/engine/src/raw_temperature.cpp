#include "raw_temperature.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <new>
#include <string>

namespace ravo
{
namespace
{

[[nodiscard]] Result<std::array<float, kTemperatureChannelCount>>
float_coefficients(const std::array<double, kTemperatureChannelCount> &source)
{
    std::array<float, kTemperatureChannelCount> result{};
    for (std::size_t index = 0; index < result.size(); ++index)
    {
        if (!std::isfinite(source[index]) || source[index] <= 0.0 || source[index] > 8.0)
        {
            return make_error(ErrorCode::kValidation, "Temperature coefficient is outside (0, 8]",
                              {{"index", std::to_string(index)}});
        }
        result[index] = static_cast<float>(source[index]);
    }
    return result;
}

[[nodiscard]] Result<void>
validate_coefficients(const std::array<float, kTemperatureChannelCount> &coefficients)
{
    for (std::size_t index = 0; index < coefficients.size(); ++index)
    {
        if (!std::isfinite(coefficients[index]) || coefficients[index] <= 0.0F ||
            coefficients[index] > 8.0F)
        {
            return make_error(ErrorCode::kValidation, "Temperature coefficient is outside (0, 8]",
                              {{"index", std::to_string(index)}});
        }
    }
    return {};
}

} // namespace

Result<ResolvedTemperature> resolve_raw_temperature(const DecodedRaw &raw, const Recipe &recipe)
{
    const OperationInstance *selected = nullptr;
    for (const auto &operation : recipe.operations)
    {
        if (!operation.enabled || operation.id != "ravo.color.temperature")
        {
            continue;
        }
        if (selected != nullptr)
        {
            return make_error(ErrorCode::kConflict,
                              "RAW recipe contains more than one enabled temperature operation");
        }
        selected = &operation;
    }

    TemperatureParams params;
    if (selected != nullptr)
    {
        auto parsed = temperature_from_parameters(selected->parameters);
        if (!parsed)
        {
            return parsed.error();
        }
        params = std::move(parsed).value();
    }

    ResolvedTemperature result;
    result.mode = params.mode;
    if (params.coefficients)
    {
        auto coefficients = float_coefficients(*params.coefficients);
        if (!coefficients)
        {
            return coefficients.error();
        }
        result.coefficients = coefficients.value();
        return result;
    }

    if (params.mode == kTemperatureModeAsShot || params.mode == kTemperatureModeAsShotToReference)
    {
        if (!raw.has_as_shot_white_balance)
        {
            return make_error(ErrorCode::kValidation,
                              "RAW input has no valid as-shot white balance metadata",
                              {{"mode", params.mode}});
        }
        result.coefficients = raw.as_shot_white_balance;
    }
    else if (params.mode == kTemperatureModeCameraReference)
    {
        if (!raw.has_camera_reference_white_balance)
        {
            return make_error(ErrorCode::kValidation,
                              "RAW input has no valid camera-reference white balance metadata",
                              {{"mode", params.mode}});
        }
        result.coefficients = raw.camera_reference_white_balance;
    }
    else
    {
        return make_error(ErrorCode::kValidation,
                          "Manual temperature mode requires explicit coefficients");
    }
    auto valid = validate_coefficients(result.coefficients);
    if (!valid)
    {
        return valid.error();
    }
    return result;
}

Result<void> apply_temperature_rgb(WorkingImage &image, const OperationInstance &operation,
                                   const CancellationToken &cancellation)
{
    auto parsed = temperature_from_parameters(operation.parameters);
    if (!parsed)
    {
        return parsed.error();
    }
    if (!parsed.value().coefficients)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Automatic temperature modes require RAW camera metadata",
                          {{"mode", parsed.value().mode}});
    }
    auto coefficients = float_coefficients(*parsed.value().coefficients);
    if (!coefficients)
    {
        return coefficients.error();
    }
    const std::size_t expected =
        static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 3U;
    if (image.width == 0 || image.height == 0 || image.rgb.size() != expected)
    {
        return make_error(ErrorCode::kValidation,
                          "Temperature RGB input buffer is empty or undersized");
    }
    std::vector<float> output;
    try
    {
        output.resize(image.rgb.size());
    }
    catch (const std::bad_alloc &)
    {
        return make_error(ErrorCode::kInternal, "Temperature RGB processing ran out of memory");
    }
    for (std::uint32_t row = 0; row < image.height; ++row)
    {
        auto active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        for (std::uint32_t column = 0; column < image.width; ++column)
        {
            const std::size_t index = (static_cast<std::size_t>(row) * image.width + column) * 3U;
            for (std::size_t channel = 0; channel < 3U; ++channel)
            {
                const float sample = image.rgb[index + channel];
                if (!std::isfinite(sample))
                {
                    return make_error(ErrorCode::kValidation,
                                      "Temperature RGB input contains a non-finite sample",
                                      {{"sample_index", std::to_string(index + channel)}});
                }
                output[index + channel] = sample * coefficients.value()[channel];
                if (!std::isfinite(output[index + channel]))
                {
                    return make_error(ErrorCode::kValidation,
                                      "Temperature RGB output contains a non-finite sample",
                                      {{"sample_index", std::to_string(index + channel)}});
                }
            }
        }
    }
    image.rgb.swap(output);
    return {};
}

Result<std::vector<float>>
scale_temperature_cfa(const std::vector<float> &input, const std::uint32_t width,
                      const std::uint32_t height, const std::uint32_t cfa_width,
                      const std::uint32_t cfa_height, const std::vector<std::uint8_t> &cfa_channels,
                      const std::array<float, kTemperatureChannelCount> &coefficients,
                      const CancellationToken &cancellation)
{
    const std::size_t expected = static_cast<std::size_t>(width) * height;
    if (width == 0 || height == 0 || input.size() != expected || cfa_width == 0 ||
        cfa_height == 0 || cfa_channels.size() != static_cast<std::size_t>(cfa_width) * cfa_height)
    {
        return make_error(ErrorCode::kValidation, "Temperature CFA input or pattern is invalid");
    }
    auto valid = validate_coefficients(coefficients);
    if (!valid)
    {
        return valid.error();
    }
    for (const auto channel : cfa_channels)
    {
        if (channel >= kTemperatureChannelCount)
        {
            return make_error(ErrorCode::kUnsupported,
                              "Temperature CFA pattern contains an unsupported channel",
                              {{"channel", std::to_string(channel)}});
        }
    }
    std::vector<float> output(input.size());
    for (std::uint32_t row = 0; row < height; ++row)
    {
        auto active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        for (std::uint32_t column = 0; column < width; ++column)
        {
            const std::size_t index = static_cast<std::size_t>(row) * width + column;
            const auto channel =
                cfa_channels[(row % cfa_height) * cfa_width + (column % cfa_width)];
            if (!std::isfinite(input[index]))
            {
                return make_error(ErrorCode::kValidation,
                                  "Temperature CFA input contains a non-finite sample",
                                  {{"sample_index", std::to_string(index)}});
            }
            output[index] = input[index] * coefficients[channel];
            if (!std::isfinite(output[index]))
            {
                return make_error(ErrorCode::kValidation,
                                  "Temperature CFA output contains a non-finite sample",
                                  {{"sample_index", std::to_string(index)}});
            }
        }
    }
    return output;
}

namespace
{

void invert_user_orientation(double &u, double &v, const WhiteBalancePickRequest &request)
{
    if (request.flip_horizontal)
    {
        u = 1.0 - u;
    }
    if (request.flip_vertical)
    {
        v = 1.0 - v;
    }
    const int turns = normalized_rotate_quarters(request.rotate_quarters);
    const double x = u;
    const double y = v;
    if (turns == 1)
    {
        u = y;
        v = 1.0 - x;
    }
    else if (turns == 2)
    {
        u = 1.0 - x;
        v = 1.0 - y;
    }
    else if (turns == 3)
    {
        u = 1.0 - y;
        v = x;
    }
}

void oriented_to_sensor(const double u, const double v, const std::uint32_t sensor_width,
                        const std::uint32_t sensor_height, const int turns, std::uint32_t &sx,
                        std::uint32_t &sy)
{
    std::uint32_t display_width = sensor_width;
    std::uint32_t display_height = sensor_height;
    apply_display_rotation_to_size(display_width, display_height, turns);
    const double dx = std::clamp(u, 0.0, 1.0) * static_cast<double>(display_width - 1U);
    const double dy = std::clamp(v, 0.0, 1.0) * static_cast<double>(display_height - 1U);
    const auto dx_i = static_cast<std::uint32_t>(std::llround(dx));
    const auto dy_i = static_cast<std::uint32_t>(std::llround(dy));
    if (turns == 0)
    {
        sx = std::min(dx_i, sensor_width - 1U);
        sy = std::min(dy_i, sensor_height - 1U);
        return;
    }
    if (turns == 1)
    {
        sx = std::min(dy_i, sensor_width - 1U);
        sy = std::min(sensor_height - 1U - std::min(dx_i, sensor_height - 1U), sensor_height - 1U);
        return;
    }
    if (turns == 2)
    {
        sx = sensor_width - 1U - std::min(dx_i, sensor_width - 1U);
        sy = sensor_height - 1U - std::min(dy_i, sensor_height - 1U);
        return;
    }
    sx = sensor_width - 1U - std::min(dy_i, sensor_width - 1U);
    sy = std::min(dx_i, sensor_height - 1U);
}

} // namespace

Result<std::array<double, kTemperatureChannelCount>>
sample_white_balance_coefficients(const DecodedRaw &raw, const WhiteBalancePickRequest &request)
{
    if (raw.width < 2U || raw.height < 2U || raw.pixels.size() !=
                                                 static_cast<std::size_t>(raw.width) * raw.height ||
        raw.cfa_width == 0 || raw.cfa_height == 0 ||
        raw.cfa_channels.size() != static_cast<std::size_t>(raw.cfa_width) * raw.cfa_height)
    {
        return make_error(ErrorCode::kValidation, "White-balance sample requires a Bayer CFA frame");
    }
    if (!std::isfinite(request.preview_x) || !std::isfinite(request.preview_y) ||
        request.preview_x < 0.0 || request.preview_x > 1.0 || request.preview_y < 0.0 ||
        request.preview_y > 1.0 || !std::isfinite(request.crop_x) || !std::isfinite(request.crop_y) ||
        !std::isfinite(request.crop_width) || !std::isfinite(request.crop_height) ||
        request.crop_width <= 0.0 || request.crop_height <= 0.0)
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "White-balance pick coordinates must be finite in [0, 1]");
    }
    double u = request.crop_x + request.preview_x * request.crop_width;
    double v = request.crop_y + request.preview_y * request.crop_height;
    invert_user_orientation(u, v, request);
    std::uint32_t sx = 0;
    std::uint32_t sy = 0;
    oriented_to_sensor(u, v, raw.width, raw.height, normalized_rotate_quarters(raw.rotate_quarters),
                       sx, sy);
    const int radius = static_cast<int>(
        std::max<std::uint32_t>(2U, std::min(raw.width, raw.height) / 64U));
    const float denominator = static_cast<float>(
        std::max<std::int64_t>(1, static_cast<std::int64_t>(raw.white_level) - raw.black_level));
    std::array<double, kTemperatureChannelCount> sums{};
    std::array<std::uint32_t, kTemperatureChannelCount> counts{};
    for (int offset_y = -radius; offset_y <= radius; ++offset_y)
    {
        const int y = std::clamp(static_cast<int>(sy) + offset_y, 0, static_cast<int>(raw.height) - 1);
        for (int offset_x = -radius; offset_x <= radius; ++offset_x)
        {
            const int x =
                std::clamp(static_cast<int>(sx) + offset_x, 0, static_cast<int>(raw.width) - 1);
            const auto channel =
                raw.cfa_channels[(static_cast<std::uint32_t>(y) % raw.cfa_height) * raw.cfa_width +
                                 (static_cast<std::uint32_t>(x) % raw.cfa_width)];
            if (channel >= kTemperatureChannelCount)
            {
                continue;
            }
            const float sample =
                (static_cast<float>(
                     raw.pixels[static_cast<std::size_t>(y) * raw.width + static_cast<std::size_t>(x)]) -
                 static_cast<float>(raw.black_level)) /
                denominator;
            if (!std::isfinite(sample) || sample <= 0.0F)
            {
                continue;
            }
            sums[channel] += static_cast<double>(sample);
            ++counts[channel];
        }
    }
    if (counts[0] == 0 || counts[1] == 0 || counts[2] == 0)
    {
        return make_error(ErrorCode::kValidation,
                          "White-balance pick did not sample red, green, and blue CFA values",
                          {{"blue", std::to_string(counts[2])},
                           {"green", std::to_string(counts[1])},
                           {"red", std::to_string(counts[0])}});
    }
    const double mean_r = sums[0] / static_cast<double>(counts[0]);
    const double mean_g = sums[1] / static_cast<double>(counts[1]);
    const double mean_b = sums[2] / static_cast<double>(counts[2]);
    const double mean_g2 = counts[3] == 0 ? mean_g : sums[3] / static_cast<double>(counts[3]);
    if (mean_r <= 1.0e-8 || mean_g <= 1.0e-8 || mean_b <= 1.0e-8 || mean_g2 <= 1.0e-8)
    {
        return make_error(ErrorCode::kValidation, "White-balance pick sampled a near-black patch");
    }
    std::array<double, kTemperatureChannelCount> coefficients{mean_g / mean_r, 1.0, mean_g / mean_b,
                                                              mean_g / mean_g2};
    for (double &coefficient : coefficients)
    {
        if (!std::isfinite(coefficient) || coefficient <= 0.0)
        {
            return make_error(ErrorCode::kValidation,
                              "White-balance pick produced a non-finite coefficient");
        }
        coefficient = std::clamp(coefficient, 0.000001, 8.0);
    }
    return coefficients;
}

} // namespace ravo
