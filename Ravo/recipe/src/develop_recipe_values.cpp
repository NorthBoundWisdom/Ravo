#include "ravo/recipe/develop.h"
#include "develop_internal.h"

namespace ravo
{
using namespace develop_internal;

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
