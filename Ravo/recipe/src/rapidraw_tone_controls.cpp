#include "ravo/recipe/rapidraw_tone_controls.h"

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
        return make_error(
            ErrorCode::kValidation, "RapidRAW tone parameter is required",
            {{"parameter", std::string(name)}, {"reason", "invalid_rapidraw_tone_controls"}});
    }
    double value = std::numeric_limits<double>::quiet_NaN();
    if (const auto *floating = std::get_if<double>(&found->second.value); floating != nullptr)
        value = *floating;
    else if (const auto *integer = std::get_if<std::int64_t>(&found->second.value);
             integer != nullptr)
        value = static_cast<double>(*integer);
    if (!std::isfinite(value) || value < minimum || value > maximum)
    {
        return make_error(
            ErrorCode::kValidation, "RapidRAW tone parameter is outside the permitted range",
            {{"parameter", std::string(name)}, {"reason", "invalid_rapidraw_tone_controls"}});
    }
    return value;
}

} // namespace

bool RapidRawToneControlsParams::is_identity() const noexcept
{
    return ev_shift == 0.0 && exposure == 0.0 && contrast == 0.0 && highlights == 0.0 &&
           shadows == 0.0 && whites == 0.0 && blacks == 0.0;
}

Result<RapidRawToneControlsParams> rapidraw_tone_controls_from_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    static const std::set<std::string, std::less<>> kKnown{
        "working_space", "ev_shift", "exposure", "contrast",
        "highlights",    "shadows",  "whites",   "blacks",
    };
    if (parameters.size() != kKnown.size())
    {
        return make_error(ErrorCode::kValidation,
                          "RapidRAW tone parameters are incomplete or unknown",
                          {{"reason", "invalid_rapidraw_tone_controls"}});
    }
    for (const auto &[name, ignored] : parameters)
    {
        static_cast<void>(ignored);
        if (!kKnown.contains(name))
        {
            return make_error(ErrorCode::kValidation, "RapidRAW tone parameter is unknown",
                              {{"parameter", name}, {"reason", "invalid_rapidraw_tone_controls"}});
        }
    }
    const auto working = parameters.find("working_space");
    const auto *working_space =
        working == parameters.end() ? nullptr : std::get_if<std::string>(&working->second.value);
    if (working_space == nullptr || *working_space != kRapidRawToneControlsWorkingSpace)
    {
        return make_error(ErrorCode::kValidation, "RapidRAW tone working space is unsupported",
                          {{"reason", "invalid_rapidraw_tone_controls"}});
    }
    auto ev_shift =
        required_number(parameters, "ev_shift", kRapidRawExposureMin, kRapidRawExposureMax);
    auto exposure =
        required_number(parameters, "exposure", kRapidRawExposureMin, kRapidRawExposureMax);
    auto contrast = required_number(parameters, "contrast", kRapidRawToneMin, kRapidRawToneMax);
    auto highlights = required_number(parameters, "highlights", kRapidRawToneMin, kRapidRawToneMax);
    auto shadows = required_number(parameters, "shadows", kRapidRawToneMin, kRapidRawToneMax);
    auto whites = required_number(parameters, "whites", kRapidRawToneMin, kRapidRawToneMax);
    auto blacks = required_number(parameters, "blacks", kRapidRawToneMin, kRapidRawToneMax);
    if (!ev_shift || !exposure || !contrast || !highlights || !shadows || !whites || !blacks)
    {
        return !ev_shift   ? ev_shift.error() :
               !exposure   ? exposure.error() :
               !contrast   ? contrast.error() :
               !highlights ? highlights.error() :
               !shadows    ? shadows.error() :
               !whites     ? whites.error() :
                             blacks.error();
    }
    return RapidRawToneControlsParams{ev_shift.value(),   exposure.value(), contrast.value(),
                                      highlights.value(), shadows.value(),  whites.value(),
                                      blacks.value()};
}

Result<std::map<std::string, ParameterValue, std::less<>>>
rapidraw_tone_controls_to_parameters(const RapidRawToneControlsParams &params)
{
    std::map<std::string, ParameterValue, std::less<>> parameters{
        {"working_space", ParameterValue{std::string(kRapidRawToneControlsWorkingSpace)}},
        {"ev_shift", ParameterValue{params.ev_shift}},
        {"exposure", ParameterValue{params.exposure}},
        {"contrast", ParameterValue{params.contrast}},
        {"highlights", ParameterValue{params.highlights}},
        {"shadows", ParameterValue{params.shadows}},
        {"whites", ParameterValue{params.whites}},
        {"blacks", ParameterValue{params.blacks}},
    };
    auto valid = validate_rapidraw_tone_controls_parameters(parameters);
    if (!valid)
        return valid.error();
    return parameters;
}

Result<void> validate_rapidraw_tone_controls_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    auto parsed = rapidraw_tone_controls_from_parameters(parameters);
    if (!parsed)
        return parsed.error();
    return {};
}

} // namespace ravo
