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

} // namespace ravo
