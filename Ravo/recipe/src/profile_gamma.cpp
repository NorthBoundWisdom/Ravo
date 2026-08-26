#include "ravo/recipe/profile_gamma.h"

#include <cmath>
#include <limits>
#include <set>
#include <utility>

namespace ravo
{
namespace
{

[[nodiscard]] Result<std::string>
required_mode(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    const auto found = parameters.find("mode");
    if (found == parameters.end())
    {
        return make_error(ErrorCode::kValidation, "Profile gamma mode is required",
                          {{"parameter", "mode"}});
    }
    const auto *mode = std::get_if<std::string>(&found->second.value);
    if (mode == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Profile gamma mode must be a string",
                          {{"parameter", "mode"}});
    }
    if (*mode != kProfileGammaModeLogarithmic && *mode != kProfileGammaModeGamma)
    {
        return make_error(ErrorCode::kValidation, "Profile gamma mode is unsupported",
                          {{"parameter", "mode"}, {"mode", *mode}});
    }
    return *mode;
}

[[nodiscard]] Result<double>
required_number(const std::map<std::string, ParameterValue, std::less<>> &parameters,
                const std::string_view name, const double minimum, const double maximum)
{
    const auto found = parameters.find(std::string(name));
    if (found == parameters.end())
    {
        return make_error(ErrorCode::kValidation, "Profile gamma parameter is required",
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
        return make_error(ErrorCode::kValidation, "Profile gamma parameter must be finite",
                          {{"parameter", std::string(name)}});
    }
    if (value < minimum || value > maximum)
    {
        return make_error(ErrorCode::kValidation,
                          "Profile gamma parameter is outside the permitted range",
                          {{"parameter", std::string(name)}});
    }
    return value;
}

} // namespace

bool ProfileGammaParams::is_default() const noexcept
{
    return mode == kProfileGammaModeLogarithmic && linear == kProfileGammaLinearDefault &&
           gamma == kProfileGammaGammaDefault &&
           dynamic_range == kProfileGammaDynamicRangeDefault &&
           grey_point == kProfileGammaGreyPointDefault &&
           shadows_range == kProfileGammaShadowsRangeDefault &&
           security_factor == kProfileGammaSecurityFactorDefault;
}

Result<ProfileGammaParams>
profile_gamma_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    static const std::set<std::string, std::less<>> kKnownParameters{
        "mode",       "linear",        "gamma",           "dynamic_range",
        "grey_point", "shadows_range", "security_factor",
    };
    for (const auto &[name, ignored] : parameters)
    {
        static_cast<void>(ignored);
        if (!kKnownParameters.contains(name))
        {
            return make_error(ErrorCode::kValidation, "Profile gamma parameter is unknown",
                              {{"parameter", name}});
        }
    }

    auto mode = required_mode(parameters);
    auto linear =
        required_number(parameters, "linear", kProfileGammaLinearMin, kProfileGammaLinearMax);
    auto gamma = required_number(parameters, "gamma", kProfileGammaGammaMin, kProfileGammaGammaMax);
    auto dynamic_range = required_number(parameters, "dynamic_range", kProfileGammaDynamicRangeMin,
                                         kProfileGammaDynamicRangeMax);
    auto grey_point = required_number(parameters, "grey_point", kProfileGammaGreyPointMin,
                                      kProfileGammaGreyPointMax);
    auto shadows_range = required_number(parameters, "shadows_range", kProfileGammaShadowsRangeMin,
                                         kProfileGammaShadowsRangeMax);
    auto security_factor =
        required_number(parameters, "security_factor", kProfileGammaSecurityFactorMin,
                        kProfileGammaSecurityFactorMax);
    if (!mode || !linear || !gamma || !dynamic_range || !grey_point || !shadows_range ||
        !security_factor)
    {
        return !mode          ? mode.error() :
               !linear        ? linear.error() :
               !gamma         ? gamma.error() :
               !dynamic_range ? dynamic_range.error() :
               !grey_point    ? grey_point.error() :
               !shadows_range ? shadows_range.error() :
                                security_factor.error();
    }

    return ProfileGammaParams{std::move(mode).value(), linear.value(),     gamma.value(),
                              dynamic_range.value(),   grey_point.value(), shadows_range.value(),
                              security_factor.value()};
}

Result<void> validate_profile_gamma_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    auto parsed = profile_gamma_from_parameters(parameters);
    if (!parsed)
    {
        return parsed.error();
    }
    return {};
}

Result<std::map<std::string, ParameterValue, std::less<>>>
profile_gamma_to_parameters(const ProfileGammaParams &params)
{
    std::map<std::string, ParameterValue, std::less<>> result{
        {"mode", ParameterValue{params.mode}},
        {"linear", ParameterValue{params.linear}},
        {"gamma", ParameterValue{params.gamma}},
        {"dynamic_range", ParameterValue{params.dynamic_range}},
        {"grey_point", ParameterValue{params.grey_point}},
        {"shadows_range", ParameterValue{params.shadows_range}},
        {"security_factor", ParameterValue{params.security_factor}},
    };
    auto valid = validate_profile_gamma_parameters(result);
    if (!valid)
    {
        return valid.error();
    }
    return result;
}

} // namespace ravo
