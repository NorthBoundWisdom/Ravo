#include "ravo/recipe/color_input.h"

#include <set>
#include <utility>

namespace ravo
{
namespace
{

[[nodiscard]] Result<std::string>
required_text(const std::map<std::string, ParameterValue, std::less<>> &parameters,
              const std::string_view name)
{
    const auto found = parameters.find(std::string(name));
    if (found == parameters.end())
    {
        return make_error(ErrorCode::kValidation, "Input colour parameter is required",
                          {{"parameter", std::string(name)}});
    }
    const auto *text = std::get_if<std::string>(&found->second.value);
    if (text == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Input colour parameter must be a string",
                          {{"parameter", std::string(name)}});
    }
    return *text;
}

[[nodiscard]] Result<bool>
required_boolean(const std::map<std::string, ParameterValue, std::less<>> &parameters,
                 const std::string_view name)
{
    const auto found = parameters.find(std::string(name));
    if (found == parameters.end())
    {
        return make_error(ErrorCode::kValidation, "Input colour parameter is required",
                          {{"parameter", std::string(name)}});
    }
    const auto *flag = std::get_if<bool>(&found->second.value);
    if (flag == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Input colour parameter must be a boolean",
                          {{"parameter", std::string(name)}});
    }
    return *flag;
}

[[nodiscard]] bool is_input_profile(const std::string_view value)
{
    static const std::set<std::string, std::less<>> values{
        std::string(kInputProfileSource),
        std::string(kInputProfileFileIcc),
        std::string(kInputProfileEmbeddedIcc),
        std::string(kInputProfileEmbeddedMatrix),
        std::string(kInputProfileStandardMatrix),
        std::string(kInputProfileEnhancedMatrix),
        std::string(kInputProfileVendorMatrix),
        std::string(kInputProfileAlternateMatrix),
        std::string(kInputProfileSrgb),
        std::string(kInputProfileAdobeRgb),
        std::string(kInputProfileLinearRec709),
        std::string(kInputProfileLinearRec2020),
        std::string(kInputProfileRec709),
        std::string(kInputProfileProPhotoRgb),
        std::string(kInputProfilePqRec2020),
        std::string(kInputProfileHlgRec2020),
        std::string(kInputProfilePqP3),
        std::string(kInputProfileHlgP3),
        std::string(kInputProfileDisplayP3),
        std::string(kInputProfileXyz),
        std::string(kInputProfileLab),
    };
    return values.contains(value);
}

[[nodiscard]] bool is_working_profile(const std::string_view value)
{
    static const std::set<std::string, std::less<>> values{
        std::string(kInputProfileFileIcc),       std::string(kInputProfileSrgb),
        std::string(kInputProfileAdobeRgb),      std::string(kInputProfileLinearRec709),
        std::string(kInputProfileLinearRec2020), std::string(kInputProfileProPhotoRgb),
        std::string(kInputProfileDisplayP3),
    };
    return values.contains(value);
}

} // namespace

bool InputColorParams::is_identity() const noexcept
{
    return input_profile == kInputProfileSource && input_profile_filename.empty() &&
           rendering_intent == kColorIntentPerceptual && gamut_normalize == kColorNormalizeOff &&
           !blue_mapping && working_profile == kInputProfileLinearRec709 &&
           working_profile_filename.empty();
}

Result<InputColorParams>
input_color_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    auto input = required_text(parameters, "input_profile");
    auto input_filename = required_text(parameters, "input_profile_filename");
    auto intent = required_text(parameters, "rendering_intent");
    auto normalize = required_text(parameters, "gamut_normalize");
    auto blue = required_boolean(parameters, "blue_mapping");
    auto working = required_text(parameters, "working_profile");
    auto working_filename = required_text(parameters, "working_profile_filename");
    if (!input || !input_filename || !intent || !normalize || !blue || !working ||
        !working_filename)
    {
        return !input          ? input.error() :
               !input_filename ? input_filename.error() :
               !intent         ? intent.error() :
               !normalize      ? normalize.error() :
               !blue           ? blue.error() :
               !working        ? working.error() :
                                 working_filename.error();
    }

    InputColorParams result;
    result.input_profile = std::move(input).value();
    result.input_profile_filename = std::move(input_filename).value();
    result.rendering_intent = std::move(intent).value();
    result.gamut_normalize = std::move(normalize).value();
    result.blue_mapping = blue.value();
    result.working_profile = std::move(working).value();
    result.working_profile_filename = std::move(working_filename).value();

    if (!is_input_profile(result.input_profile))
    {
        return make_error(ErrorCode::kUnsupported, "Input colour profile type is unsupported",
                          {{"profile", result.input_profile}});
    }
    if (!is_working_profile(result.working_profile))
    {
        return make_error(ErrorCode::kUnsupported, "Working colour profile type is unsupported",
                          {{"profile", result.working_profile}});
    }
    static const std::set<std::string, std::less<>> intents{
        std::string(kColorIntentPerceptual), std::string(kColorIntentRelative),
        std::string(kColorIntentSaturation), std::string(kColorIntentAbsolute)};
    if (!intents.contains(result.rendering_intent))
    {
        return make_error(ErrorCode::kUnsupported, "Rendering intent is unsupported",
                          {{"intent", result.rendering_intent}});
    }
    static const std::set<std::string, std::less<>> normalizations{
        std::string(kColorNormalizeOff), std::string(kColorNormalizeSrgb),
        std::string(kColorNormalizeAdobeRgb), std::string(kColorNormalizeLinearRec709),
        std::string(kColorNormalizeLinearRec2020)};
    if (!normalizations.contains(result.gamut_normalize))
    {
        return make_error(ErrorCode::kUnsupported, "Gamut normalization is unsupported",
                          {{"normalize", result.gamut_normalize}});
    }
    if ((result.input_profile == kInputProfileFileIcc) != !result.input_profile_filename.empty())
    {
        return make_error(ErrorCode::kValidation,
                          "Input profile filename is required only for file_icc",
                          {{"profile", result.input_profile}});
    }
    if ((result.working_profile == kInputProfileFileIcc) !=
        !result.working_profile_filename.empty())
    {
        return make_error(ErrorCode::kValidation,
                          "Working profile filename is required only for file_icc",
                          {{"profile", result.working_profile}});
    }
    return result;
}

Result<void> validate_input_color_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    auto parsed = input_color_from_parameters(parameters);
    if (!parsed)
    {
        return parsed.error();
    }
    return {};
}

std::map<std::string, ParameterValue, std::less<>>
input_color_to_parameters(const InputColorParams &params)
{
    return {{"input_profile", ParameterValue{params.input_profile}},
            {"input_profile_filename", ParameterValue{params.input_profile_filename}},
            {"rendering_intent", ParameterValue{params.rendering_intent}},
            {"gamut_normalize", ParameterValue{params.gamut_normalize}},
            {"blue_mapping", ParameterValue{params.blue_mapping}},
            {"working_profile", ParameterValue{params.working_profile}},
            {"working_profile_filename", ParameterValue{params.working_profile_filename}}};
}

} // namespace ravo
