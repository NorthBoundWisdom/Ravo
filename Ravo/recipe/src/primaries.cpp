#include "ravo/recipe/primaries.h"

#include <cmath>
#include <limits>
#include <set>

namespace ravo
{
namespace
{

[[nodiscard]] Result<double>
required_number(const std::map<std::string, ParameterValue, std::less<>> &parameters,
                const std::string_view name, const double minimum, const double maximum)
{
    const auto found = parameters.find(std::string(name));
    if (found == parameters.end())
    {
        return make_error(ErrorCode::kValidation, "RGB primaries parameter is required",
                          {{"parameter", std::string(name)}});
    }

    double value = std::numeric_limits<double>::quiet_NaN();
    if (const auto *floating = std::get_if<double>(&found->second.value); floating != nullptr)
    {
        value = *floating;
    }
    else if (const auto *integer = std::get_if<std::int64_t>(&found->second.value);
             integer != nullptr)
    {
        value = static_cast<double>(*integer);
    }
    if (!std::isfinite(value))
    {
        return make_error(ErrorCode::kValidation, "RGB primaries parameter must be finite",
                          {{"parameter", std::string(name)}});
    }
    if (value < minimum || value > maximum)
    {
        return make_error(ErrorCode::kValidation,
                          "RGB primaries parameter is outside the permitted range",
                          {{"parameter", std::string(name)}});
    }
    return value;
}

} // namespace

bool PrimariesParams::is_identity() const noexcept
{
    return achromatic_tint_hue == 0.0 && achromatic_tint_purity == 0.0 && red_hue == 0.0 &&
           red_purity == 1.0 && green_hue == 0.0 && green_purity == 1.0 && blue_hue == 0.0 &&
           blue_purity == 1.0;
}

Result<PrimariesParams>
primaries_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    static const std::set<std::string, std::less<>> kKnownParameters{
        "achromatic_tint_hue", "achromatic_tint_purity", "red_hue",  "red_purity",
        "green_hue",           "green_purity",           "blue_hue", "blue_purity",
    };
    for (const auto &[name, ignored] : parameters)
    {
        static_cast<void>(ignored);
        if (!kKnownParameters.contains(name))
        {
            return make_error(ErrorCode::kValidation, "RGB primaries parameter is unknown",
                              {{"parameter", name}});
        }
    }

    auto achromatic_tint_hue =
        required_number(parameters, "achromatic_tint_hue", kPrimariesHueMin, kPrimariesHueMax);
    auto achromatic_tint_purity =
        required_number(parameters, "achromatic_tint_purity", kPrimariesAchromaticTintPurityMin,
                        kPrimariesAchromaticTintPurityMax);
    auto red_hue = required_number(parameters, "red_hue", kPrimariesHueMin, kPrimariesHueMax);
    auto red_purity = required_number(parameters, "red_purity", kPrimariesPrimaryPurityMin,
                                      kPrimariesPrimaryPurityMax);
    auto green_hue = required_number(parameters, "green_hue", kPrimariesHueMin, kPrimariesHueMax);
    auto green_purity = required_number(parameters, "green_purity", kPrimariesPrimaryPurityMin,
                                        kPrimariesPrimaryPurityMax);
    auto blue_hue = required_number(parameters, "blue_hue", kPrimariesHueMin, kPrimariesHueMax);
    auto blue_purity = required_number(parameters, "blue_purity", kPrimariesPrimaryPurityMin,
                                       kPrimariesPrimaryPurityMax);
    if (!achromatic_tint_hue || !achromatic_tint_purity || !red_hue || !red_purity || !green_hue ||
        !green_purity || !blue_hue || !blue_purity)
    {
        return !achromatic_tint_hue    ? achromatic_tint_hue.error() :
               !achromatic_tint_purity ? achromatic_tint_purity.error() :
               !red_hue                ? red_hue.error() :
               !red_purity             ? red_purity.error() :
               !green_hue              ? green_hue.error() :
               !green_purity           ? green_purity.error() :
               !blue_hue               ? blue_hue.error() :
                                         blue_purity.error();
    }

    return PrimariesParams{achromatic_tint_hue.value(),
                           achromatic_tint_purity.value(),
                           red_hue.value(),
                           red_purity.value(),
                           green_hue.value(),
                           green_purity.value(),
                           blue_hue.value(),
                           blue_purity.value()};
}

Result<void>
validate_primaries_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    auto parsed = primaries_from_parameters(parameters);
    if (!parsed)
    {
        return parsed.error();
    }
    return {};
}

std::map<std::string, ParameterValue, std::less<>>
primaries_to_parameters(const PrimariesParams &params)
{
    return {{"achromatic_tint_hue", ParameterValue{params.achromatic_tint_hue}},
            {"achromatic_tint_purity", ParameterValue{params.achromatic_tint_purity}},
            {"red_hue", ParameterValue{params.red_hue}},
            {"red_purity", ParameterValue{params.red_purity}},
            {"green_hue", ParameterValue{params.green_hue}},
            {"green_purity", ParameterValue{params.green_purity}},
            {"blue_hue", ParameterValue{params.blue_hue}},
            {"blue_purity", ParameterValue{params.blue_purity}}};
}

} // namespace ravo
