#include "ravo/recipe/color_output.h"

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
        return make_error(ErrorCode::kValidation, "Output colour parameter is required",
                          {{"parameter", std::string(name)}});
    }
    const auto *text = std::get_if<std::string>(&found->second.value);
    if (text == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Output colour parameter must be a string",
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
        return make_error(ErrorCode::kValidation, "Output colour parameter is required",
                          {{"parameter", std::string(name)}});
    }
    const auto *flag = std::get_if<bool>(&found->second.value);
    if (flag == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Output colour parameter must be a boolean",
                          {{"parameter", std::string(name)}});
    }
    return *flag;
}

[[nodiscard]] bool supported_output_profile(const std::string_view value)
{
    static const std::set<std::string, std::less<>> values{
        std::string(kInputProfileFileIcc),       std::string(kInputProfileSrgb),
        std::string(kInputProfileAdobeRgb),      std::string(kInputProfileLinearRec709),
        std::string(kInputProfileLinearRec2020), std::string(kInputProfileRec709),
        std::string(kInputProfileProPhotoRgb),   std::string(kInputProfilePqRec2020),
        std::string(kInputProfileHlgRec2020),    std::string(kInputProfilePqP3),
        std::string(kInputProfileHlgP3),         std::string(kInputProfileDisplayP3),
        std::string(kInputProfileXyz),           std::string(kInputProfileLab),
    };
    return values.contains(value);
}

[[nodiscard]] bool supported_proof_profile(const std::string_view value)
{
    return supported_output_profile(value) && value != kInputProfileXyz &&
           value != kInputProfileLab;
}

[[nodiscard]] bool supported_intent(const std::string_view value)
{
    return value == kColorIntentPerceptual || value == kColorIntentRelative ||
           value == kColorIntentSaturation || value == kColorIntentAbsolute;
}

} // namespace

bool OutputColorParams::is_identity() const noexcept
{
    return output_profile == kInputProfileSrgb && output_profile_filename.empty() &&
           rendering_intent == kColorIntentPerceptual && proof_mode == kProofModeOff &&
           proof_profile == kInputProfileSrgb && proof_profile_filename.empty() &&
           proof_intent == kColorIntentRelative && black_point_compensation;
}

Result<OutputColorParams>
output_color_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    auto output_profile = required_text(parameters, "output_profile");
    auto output_filename = required_text(parameters, "output_profile_filename");
    auto intent = required_text(parameters, "rendering_intent");
    auto proof_mode = required_text(parameters, "proof_mode");
    auto proof_profile = required_text(parameters, "proof_profile");
    auto proof_filename = required_text(parameters, "proof_profile_filename");
    auto proof_intent = required_text(parameters, "proof_intent");
    auto black_point_compensation = required_boolean(parameters, "black_point_compensation");
    if (!output_profile || !output_filename || !intent || !proof_mode || !proof_profile ||
        !proof_filename || !proof_intent || !black_point_compensation)
    {
        return !output_profile  ? output_profile.error() :
               !output_filename ? output_filename.error() :
               !intent          ? intent.error() :
               !proof_mode      ? proof_mode.error() :
               !proof_profile   ? proof_profile.error() :
               !proof_filename  ? proof_filename.error() :
               !proof_intent    ? proof_intent.error() :
                                  black_point_compensation.error();
    }

    OutputColorParams result;
    result.output_profile = std::move(output_profile).value();
    result.output_profile_filename = std::move(output_filename).value();
    result.rendering_intent = std::move(intent).value();
    result.proof_mode = std::move(proof_mode).value();
    result.proof_profile = std::move(proof_profile).value();
    result.proof_profile_filename = std::move(proof_filename).value();
    result.proof_intent = std::move(proof_intent).value();
    result.black_point_compensation = black_point_compensation.value();

    if (!supported_output_profile(result.output_profile))
    {
        return make_error(ErrorCode::kUnsupported, "Output colour profile type is unsupported",
                          {{"profile", result.output_profile}});
    }
    if (!supported_proof_profile(result.proof_profile))
    {
        return make_error(ErrorCode::kUnsupported, "Proof colour profile type is unsupported",
                          {{"profile", result.proof_profile}});
    }
    if (!supported_intent(result.rendering_intent) || !supported_intent(result.proof_intent))
    {
        return make_error(ErrorCode::kUnsupported, "Output rendering intent is unsupported");
    }
    if (result.proof_mode != kProofModeOff && result.proof_mode != kProofModeSoftproof &&
        result.proof_mode != kProofModeGamutCheck)
    {
        return make_error(ErrorCode::kUnsupported, "Output proof mode is unsupported",
                          {{"proof_mode", result.proof_mode}});
    }
    if (result.proof_mode == kProofModeGamutCheck &&
        (result.output_profile == kInputProfileXyz || result.output_profile == kInputProfileLab))
    {
        return make_error(ErrorCode::kUnsupported, "Gamut warning requires an RGB output profile",
                          {{"profile", result.output_profile}});
    }
    if ((result.output_profile == kInputProfileFileIcc) != !result.output_profile_filename.empty())
    {
        return make_error(ErrorCode::kValidation,
                          "Output profile filename is required only for file_icc",
                          {{"profile", result.output_profile}});
    }
    if ((result.proof_profile == kInputProfileFileIcc) != !result.proof_profile_filename.empty())
    {
        return make_error(ErrorCode::kValidation,
                          "Proof profile filename is required only for file_icc",
                          {{"profile", result.proof_profile}});
    }
    return result;
}

Result<void> validate_output_color_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    auto parsed = output_color_from_parameters(parameters);
    if (!parsed)
    {
        return parsed.error();
    }
    return {};
}

std::map<std::string, ParameterValue, std::less<>>
output_color_to_parameters(const OutputColorParams &params)
{
    return {{"output_profile", ParameterValue{params.output_profile}},
            {"output_profile_filename", ParameterValue{params.output_profile_filename}},
            {"rendering_intent", ParameterValue{params.rendering_intent}},
            {"proof_mode", ParameterValue{params.proof_mode}},
            {"proof_profile", ParameterValue{params.proof_profile}},
            {"proof_profile_filename", ParameterValue{params.proof_profile_filename}},
            {"proof_intent", ParameterValue{params.proof_intent}},
            {"black_point_compensation", ParameterValue{params.black_point_compensation}}};
}

} // namespace ravo
