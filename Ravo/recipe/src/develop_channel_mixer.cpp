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

namespace develop_internal
{
[[nodiscard]] bool
channel_triplet_near(const std::array<double, kChannelMixerChannelCount> &left,
                     const std::array<double, kChannelMixerChannelCount> &right) noexcept
{
    for (std::size_t index = 0; index < left.size(); ++index)
    {
        if (!near(left[index], right[index]))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] ParameterValue
channel_triplet_parameter(const std::array<double, kChannelMixerChannelCount> &values)
{
    ParameterValue::Array array;
    array.reserve(values.size());
    for (const double value : values)
    {
        array.push_back(ParameterValue{value});
    }
    return ParameterValue{std::move(array)};
}

[[nodiscard]] Result<std::array<double, kChannelMixerChannelCount>>
parse_channel_triplet(const ParameterValue &value, const std::string_view name)
{
    const auto *array = std::get_if<ParameterValue::Array>(&value.value);
    if (array == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Color calibration parameter must be an array",
                          {{"parameter", std::string(name)}});
    }
    if (array->size() != kChannelMixerChannelCount)
    {
        return make_error(
            ErrorCode::kValidation, "Color calibration array must have exactly 3 values",
            {{"parameter", std::string(name)}, {"count", std::to_string(array->size())}});
    }
    std::array<double, kChannelMixerChannelCount> parsed{};
    for (std::size_t index = 0; index < parsed.size(); ++index)
    {
        const double sample = as_number((*array)[index], std::numeric_limits<double>::quiet_NaN());
        if (!std::isfinite(sample) || sample < -2.0 || sample > 2.0)
        {
            return make_error(ErrorCode::kValidation,
                              "Color calibration array value must be finite and within [-2, 2]",
                              {{"parameter", std::string(name)}, {"index", std::to_string(index)}});
        }
        parsed[index] = sample;
    }
    return parsed;
}

[[nodiscard]] Result<std::array<double, kColorEqualizerBandCount>>
parse_band_array(const ParameterValue &value, const std::string_view name)
{
    const auto *array = std::get_if<ParameterValue::Array>(&value.value);
    if (array == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Color equalizer parameter must be an array",
                          {{"parameter", std::string(name)}});
    }
    if (array->size() != kColorEqualizerBandCount)
    {
        return make_error(
            ErrorCode::kValidation, "Color equalizer array must have 8 values",
            {{"parameter", std::string(name)}, {"count", std::to_string(array->size())}});
    }
    std::array<double, kColorEqualizerBandCount> parsed{};
    for (std::size_t index = 0; index < parsed.size(); ++index)
    {
        const double sample = as_number((*array)[index], std::numeric_limits<double>::quiet_NaN());
        if (!std::isfinite(sample))
        {
            return make_error(ErrorCode::kValidation, "Color equalizer band must be finite",
                              {{"parameter", std::string(name)}, {"index", std::to_string(index)}});
        }
        parsed[index] = sample;
    }
    return parsed;
}

[[nodiscard]] bool parse_band_field(const std::string_view name, const std::string_view prefix,
                                    std::size_t &index) noexcept
{
    if (!name.starts_with(prefix) || name.size() != prefix.size() + 1U)
    {
        return false;
    }
    const char digit = name[prefix.size()];
    if (digit < '0' || digit > '7')
    {
        return false;
    }
    index = static_cast<std::size_t>(digit - '0');
    return true;
}

} // namespace develop_internal

bool ChannelMixerParams::is_identity() const noexcept
{
    const ChannelMixerParams identity;
    return channel_triplet_near(red, identity.red) && channel_triplet_near(green, identity.green) &&
           channel_triplet_near(blue, identity.blue) &&
           channel_triplet_near(saturation, identity.saturation) &&
           channel_triplet_near(lightness, identity.lightness) &&
           channel_triplet_near(grey, identity.grey) && normalize_red == identity.normalize_red &&
           normalize_green == identity.normalize_green &&
           normalize_blue == identity.normalize_blue &&
           normalize_saturation == identity.normalize_saturation &&
           normalize_lightness == identity.normalize_lightness &&
           normalize_grey == identity.normalize_grey && adaptation == identity.adaptation &&
           near(illuminant_x, identity.illuminant_x) && near(illuminant_y, identity.illuminant_y) &&
           near(gamut, identity.gamut) && clip == identity.clip;
}

Result<ChannelMixerParams>
channel_mixer_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    const auto required = [&](const std::string_view name) -> Result<const ParameterValue *>
    {
        const auto found = parameters.find(std::string(name));
        if (found == parameters.end())
        {
            return make_error(ErrorCode::kValidation, "Color calibration parameter is required",
                              {{"parameter", std::string(name)}});
        }
        return &found->second;
    };
    const auto text = [&](const std::string_view name) -> Result<std::string>
    {
        auto value = required(name);
        if (!value)
        {
            return value.error();
        }
        const auto *parsed = std::get_if<std::string>(&value.value()->value);
        if (parsed == nullptr)
        {
            return make_error(ErrorCode::kValidation,
                              "Color calibration parameter must be a string",
                              {{"parameter", std::string(name)}});
        }
        return *parsed;
    };
    const auto boolean = [&](const std::string_view name) -> Result<bool>
    {
        auto value = required(name);
        if (!value)
        {
            return value.error();
        }
        const auto *parsed = std::get_if<bool>(&value.value()->value);
        if (parsed == nullptr)
        {
            return make_error(ErrorCode::kValidation, "Color calibration parameter must be boolean",
                              {{"parameter", std::string(name)}});
        }
        return *parsed;
    };
    const auto number = [&](const std::string_view name) -> Result<double>
    {
        auto value = required(name);
        if (!value)
        {
            return value.error();
        }
        const double parsed = as_number(*value.value(), std::numeric_limits<double>::quiet_NaN());
        if (!std::isfinite(parsed))
        {
            return make_error(ErrorCode::kValidation, "Color calibration parameter must be finite",
                              {{"parameter", std::string(name)}});
        }
        return parsed;
    };
    const auto triplet =
        [&](const std::string_view name) -> Result<std::array<double, kChannelMixerChannelCount>>
    {
        auto value = required(name);
        if (!value)
        {
            return value.error();
        }
        return parse_channel_triplet(*value.value(), name);
    };

    auto working_space = text("working_space");
    auto algorithm = text("algorithm");
    auto adaptation = text("adaptation");
    if (!working_space || !algorithm || !adaptation)
    {
        return !working_space ? working_space.error() :
               !algorithm     ? algorithm.error() :
                                adaptation.error();
    }
    if (working_space.value() != kChannelMixerWorkingSpaceLinearSrgbD50)
    {
        return make_error(ErrorCode::kValidation, "Color calibration working space is unsupported",
                          {{"working_space", working_space.value()}});
    }
    if (algorithm.value() != kChannelMixerAlgorithmV3)
    {
        return make_error(ErrorCode::kValidation, "Color calibration algorithm is unsupported",
                          {{"algorithm", algorithm.value()}});
    }
    if (adaptation.value() != kChannelMixerAdaptationRgb &&
        adaptation.value() != kChannelMixerAdaptationCat16 &&
        adaptation.value() != kChannelMixerAdaptationLinearBradford &&
        adaptation.value() != kChannelMixerAdaptationFullBradford &&
        adaptation.value() != kChannelMixerAdaptationXyz)
    {
        return make_error(ErrorCode::kValidation, "Color calibration adaptation is unsupported",
                          {{"adaptation", adaptation.value()}});
    }

    auto red = triplet("red");
    auto green = triplet("green");
    auto blue = triplet("blue");
    auto saturation = triplet("saturation");
    auto lightness = triplet("lightness");
    auto grey = triplet("grey");
    auto normalize_red = boolean("normalize_red");
    auto normalize_green = boolean("normalize_green");
    auto normalize_blue = boolean("normalize_blue");
    auto normalize_saturation = boolean("normalize_saturation");
    auto normalize_lightness = boolean("normalize_lightness");
    auto normalize_grey = boolean("normalize_grey");
    auto illuminant_x = number("illuminant_x");
    auto illuminant_y = number("illuminant_y");
    auto gamut = number("gamut");
    auto clip = boolean("clip");
    if (!red || !green || !blue || !saturation || !lightness || !grey || !normalize_red ||
        !normalize_green || !normalize_blue || !normalize_saturation || !normalize_lightness ||
        !normalize_grey || !illuminant_x || !illuminant_y || !gamut || !clip)
    {
        return !red                  ? red.error() :
               !green                ? green.error() :
               !blue                 ? blue.error() :
               !saturation           ? saturation.error() :
               !lightness            ? lightness.error() :
               !grey                 ? grey.error() :
               !normalize_red        ? normalize_red.error() :
               !normalize_green      ? normalize_green.error() :
               !normalize_blue       ? normalize_blue.error() :
               !normalize_saturation ? normalize_saturation.error() :
               !normalize_lightness  ? normalize_lightness.error() :
               !normalize_grey       ? normalize_grey.error() :
               !illuminant_x         ? illuminant_x.error() :
               !illuminant_y         ? illuminant_y.error() :
               !gamut                ? gamut.error() :
                                       clip.error();
    }
    if (illuminant_x.value() <= 0.0 || illuminant_y.value() <= 0.0 ||
        illuminant_x.value() + illuminant_y.value() >= 1.0)
    {
        return make_error(ErrorCode::kValidation,
                          "Color calibration illuminant xy is outside the CIE chromaticity domain");
    }
    if (gamut.value() < 0.0 || gamut.value() > 12.0)
    {
        return make_error(ErrorCode::kValidation,
                          "Color calibration gamut compression is outside [0, 12]");
    }
    const auto reject_zero_normalized_row = [](const auto &row, const bool normalize,
                                               const std::string_view name) -> Result<void>
    {
        if (normalize && std::abs(row[0] + row[1] + row[2]) <= kEpsilon)
        {
            return make_error(ErrorCode::kValidation,
                              "Color calibration normalized row must have a non-zero sum",
                              {{"parameter", std::string(name)}});
        }
        return {};
    };
    auto valid_red = reject_zero_normalized_row(red.value(), normalize_red.value(), "red");
    auto valid_green = reject_zero_normalized_row(green.value(), normalize_green.value(), "green");
    auto valid_blue = reject_zero_normalized_row(blue.value(), normalize_blue.value(), "blue");
    if (!valid_red || !valid_green || !valid_blue)
    {
        return !valid_red   ? valid_red.error() :
               !valid_green ? valid_green.error() :
                              valid_blue.error();
    }

    ChannelMixerParams result;
    result.red = red.value();
    result.green = green.value();
    result.blue = blue.value();
    result.saturation = saturation.value();
    result.lightness = lightness.value();
    result.grey = grey.value();
    result.normalize_red = normalize_red.value();
    result.normalize_green = normalize_green.value();
    result.normalize_blue = normalize_blue.value();
    result.normalize_saturation = normalize_saturation.value();
    result.normalize_lightness = normalize_lightness.value();
    result.normalize_grey = normalize_grey.value();
    result.adaptation = adaptation.value();
    result.illuminant_x = illuminant_x.value();
    result.illuminant_y = illuminant_y.value();
    result.gamut = gamut.value();
    result.clip = clip.value();
    return result;
}

Result<void> validate_channel_mixer_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    auto parsed = channel_mixer_from_parameters(parameters);
    if (!parsed)
    {
        return parsed.error();
    }
    return {};
}

std::map<std::string, ParameterValue, std::less<>>
channel_mixer_to_parameters(const ChannelMixerParams &params)
{
    return {{"working_space", ParameterValue{std::string(kChannelMixerWorkingSpaceLinearSrgbD50)}},
            {"algorithm", ParameterValue{std::string(kChannelMixerAlgorithmV3)}},
            {"adaptation", ParameterValue{params.adaptation}},
            {"red", channel_triplet_parameter(params.red)},
            {"green", channel_triplet_parameter(params.green)},
            {"blue", channel_triplet_parameter(params.blue)},
            {"saturation", channel_triplet_parameter(params.saturation)},
            {"lightness", channel_triplet_parameter(params.lightness)},
            {"grey", channel_triplet_parameter(params.grey)},
            {"normalize_red", ParameterValue{params.normalize_red}},
            {"normalize_green", ParameterValue{params.normalize_green}},
            {"normalize_blue", ParameterValue{params.normalize_blue}},
            {"normalize_saturation", ParameterValue{params.normalize_saturation}},
            {"normalize_lightness", ParameterValue{params.normalize_lightness}},
            {"normalize_grey", ParameterValue{params.normalize_grey}},
            {"illuminant_x", ParameterValue{params.illuminant_x}},
            {"illuminant_y", ParameterValue{params.illuminant_y}},
            {"gamut", ParameterValue{params.gamut}},
            {"clip", ParameterValue{params.clip}}};
}


} // namespace ravo
