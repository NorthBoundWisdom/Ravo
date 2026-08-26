#include "ravo/recipe/operation.h"

#include <cmath>
#include <utility>

#include "ravo/recipe/color_input.h"
#include "ravo/recipe/color_output.h"
#include "ravo/recipe/develop.h"

namespace ravo
{

OperationRegistry::OperationRegistry(std::vector<OperationDescriptor> descriptors)
    : descriptors_(std::move(descriptors))
{
    for (std::size_t index = 0; index < descriptors_.size(); ++index)
    {
        indexes_.emplace(descriptors_[index].id, index);
    }
}

Result<OperationRegistry> OperationRegistry::create(std::vector<OperationDescriptor> descriptors)
{
    std::map<std::string, std::size_t, std::less<>> indexes;
    for (std::size_t index = 0; index < descriptors.size(); ++index)
    {
        const auto &descriptor = descriptors[index];
        if (descriptor.id.empty())
        {
            return make_error(ErrorCode::kValidation, "Operation descriptor ID must not be empty",
                              {{"descriptor_index", std::to_string(index)}});
        }
        if (descriptor.parameter_schema_version < 1)
        {
            return make_error(ErrorCode::kValidation,
                              "Operation descriptor schema version must be positive",
                              {{"operation_id", descriptor.id}});
        }
        if (!indexes.emplace(descriptor.id, index).second)
        {
            return make_error(ErrorCode::kConflict, "Duplicate operation descriptor ID",
                              {{"operation_id", descriptor.id}});
        }

        std::map<std::string, bool, std::less<>> parameter_names;
        for (const auto &parameter : descriptor.parameters)
        {
            if (parameter.name.empty() || !parameter_names.emplace(parameter.name, true).second)
            {
                return make_error(ErrorCode::kValidation,
                                  "Operation descriptor has an invalid parameter name",
                                  {{"operation_id", descriptor.id}, {"parameter", parameter.name}});
            }
            if ((parameter.minimum.has_value() && !std::isfinite(*parameter.minimum)) ||
                (parameter.maximum.has_value() && !std::isfinite(*parameter.maximum)) ||
                (parameter.minimum.has_value() && parameter.maximum.has_value() &&
                 *parameter.minimum > *parameter.maximum))
            {
                return make_error(ErrorCode::kValidation,
                                  "Operation descriptor has an invalid numeric range",
                                  {{"operation_id", descriptor.id}, {"parameter", parameter.name}});
            }
        }
    }

    return OperationRegistry{std::move(descriptors)};
}

const OperationDescriptor *OperationRegistry::find(const std::string_view id) const noexcept
{
    const auto iterator = indexes_.find(std::string(id));
    if (iterator == indexes_.end())
    {
        return nullptr;
    }
    return &descriptors_[iterator->second];
}

const std::vector<OperationDescriptor> &OperationRegistry::descriptors() const noexcept
{
    return descriptors_;
}

Result<OperationRegistry> make_phase1_registry()
{
    return OperationRegistry::create({
        {"ravo.core.identity", "Identity", 1, {}, false, false},
        {"ravo.raw.prepare", "RAW prepare", 1, {}, false, false},
        {"ravo.raw.demosaic", "RAW demosaic", 1, {}, false, false},
        {"ravo.color.input",
         "Input colour",
         1,
         {{"input_profile", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"input_profile_filename", ParameterType::kString, true, std::nullopt, std::nullopt,
           std::nullopt},
          {"rendering_intent", ParameterType::kString, true, std::nullopt, std::nullopt,
           std::nullopt},
          {"gamut_normalize", ParameterType::kString, true, std::nullopt, std::nullopt,
           std::nullopt},
          {"blue_mapping", ParameterType::kBoolean, true, std::nullopt, std::nullopt, std::nullopt},
          {"working_profile", ParameterType::kString, true, std::nullopt, std::nullopt,
           std::nullopt},
          {"working_profile_filename", ParameterType::kString, true, std::nullopt, std::nullopt,
           std::nullopt}},
         false,
         true},
        {"ravo.core.exposure",
         "Exposure",
         1,
         {{"exposure_ev", ParameterType::kNumber, false, ParameterValue{0.0}, -10.0, 10.0}},
         false,
         true},
        {"ravo.color.temperature",
         "White balance",
         1,
         {{"working_space", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"algorithm", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"mode", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"coefficients", ParameterType::kArray, false, std::nullopt, std::nullopt, std::nullopt}},
         false,
         true},
        {"ravo.color.channelmixerrgb",
         "Color calibration",
         1,
         {{"working_space", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"algorithm", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"adaptation", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"red", ParameterType::kArray, true, std::nullopt, std::nullopt, std::nullopt},
          {"green", ParameterType::kArray, true, std::nullopt, std::nullopt, std::nullopt},
          {"blue", ParameterType::kArray, true, std::nullopt, std::nullopt, std::nullopt},
          {"saturation", ParameterType::kArray, true, std::nullopt, std::nullopt, std::nullopt},
          {"lightness", ParameterType::kArray, true, std::nullopt, std::nullopt, std::nullopt},
          {"grey", ParameterType::kArray, true, std::nullopt, std::nullopt, std::nullopt},
          {"normalize_red", ParameterType::kBoolean, true, std::nullopt, std::nullopt,
           std::nullopt},
          {"normalize_green", ParameterType::kBoolean, true, std::nullopt, std::nullopt,
           std::nullopt},
          {"normalize_blue", ParameterType::kBoolean, true, std::nullopt, std::nullopt,
           std::nullopt},
          {"normalize_saturation", ParameterType::kBoolean, true, std::nullopt, std::nullopt,
           std::nullopt},
          {"normalize_lightness", ParameterType::kBoolean, true, std::nullopt, std::nullopt,
           std::nullopt},
          {"normalize_grey", ParameterType::kBoolean, true, std::nullopt, std::nullopt,
           std::nullopt},
          {"illuminant_x", ParameterType::kNumber, true, std::nullopt, 0.000001, 0.999999},
          {"illuminant_y", ParameterType::kNumber, true, std::nullopt, 0.000001, 0.999999},
          {"gamut", ParameterType::kNumber, true, std::nullopt, 0.0, 12.0},
          {"clip", ParameterType::kBoolean, true, std::nullopt, std::nullopt, std::nullopt}},
         false,
         true},
        {"ravo.core.contrast",
         "Contrast",
         1,
         {{"amount", ParameterType::kNumber, false, ParameterValue{0.0}, -1.0, 1.0}},
         false,
         true},
        {"ravo.core.highlights",
         "Highlights",
         1,
         {{"amount", ParameterType::kNumber, false, ParameterValue{0.0}, -1.0, 1.0}},
         false,
         true},
        {"ravo.core.shadows",
         "Shadows",
         1,
         {{"amount", ParameterType::kNumber, false, ParameterValue{0.0}, -1.0, 1.0}},
         false,
         true},
        {"ravo.core.whites",
         "Whites",
         1,
         {{"amount", ParameterType::kNumber, false, ParameterValue{0.0}, -1.0, 1.0}},
         false,
         true},
        {"ravo.core.blacks",
         "Blacks",
         1,
         {{"amount", ParameterType::kNumber, false, ParameterValue{0.0}, -1.0, 1.0}},
         false,
         true},
        {"ravo.color.vibrance",
         "Vibrance",
         1,
         {{"amount", ParameterType::kNumber, false, ParameterValue{0.0}, -1.0, 1.0}},
         false,
         true},
        {"ravo.color.saturation",
         "Saturation",
         1,
         {{"amount", ParameterType::kNumber, false, ParameterValue{0.0}, -1.0, 1.0}},
         false,
         true},
        {"ravo.geometry.rotate",
         "Rotate",
         1,
         {{"quarters", ParameterType::kInteger, false, ParameterValue{std::int64_t{0}}, 0.0, 3.0}},
         false,
         true},
        {"ravo.geometry.crop",
         "Crop",
         1,
         {{"x", ParameterType::kNumber, false, ParameterValue{0.0}, 0.0, 1.0},
          {"y", ParameterType::kNumber, false, ParameterValue{0.0}, 0.0, 1.0},
          {"width", ParameterType::kNumber, false, ParameterValue{1.0}, 0.01, 1.0},
          {"height", ParameterType::kNumber, false, ParameterValue{1.0}, 0.01, 1.0}},
         false,
         true},
        {"ravo.geometry.flip",
         "Flip",
         1,
         {{"horizontal", ParameterType::kInteger, false, ParameterValue{std::int64_t{0}}, 0.0, 1.0},
          {"vertical", ParameterType::kInteger, false, ParameterValue{std::int64_t{0}}, 0.0, 1.0}},
         false,
         true},
        {"ravo.geometry.straighten",
         "Straighten",
         1,
         {{"degrees", ParameterType::kNumber, false, ParameterValue{0.0}, -45.0, 45.0}},
         false,
         true},
        {"ravo.core.gamma",
         "Gamma",
         1,
         {{"gamma", ParameterType::kNumber, false, ParameterValue{1.0}, 0.2, 3.0}},
         false,
         true},
        {"ravo.core.tonecurve",
         "Tone curve",
         1,
         {{"working_space", ParameterType::kString, false, ParameterValue{"rgb"}, std::nullopt,
           std::nullopt},
          {"interpolation", ParameterType::kString, false, ParameterValue{"monotone_hermite"},
           std::nullopt, std::nullopt},
          {"channel_mode", ParameterType::kString, false, ParameterValue{"rgb"}, std::nullopt,
           std::nullopt},
          {"preserve_colors", ParameterType::kString, false, ParameterValue{"average"},
           std::nullopt, std::nullopt},
          {"points", ParameterType::kArray, false,
           ParameterValue{ParameterValue::Array{
               ParameterValue{
                   ParameterValue::Object{{"x", ParameterValue{0.0}}, {"y", ParameterValue{0.0}}}},
               ParameterValue{
                   ParameterValue::Object{{"x", ParameterValue{1.0}}, {"y", ParameterValue{1.0}}}},
           }},
           std::nullopt, std::nullopt}},
         false,
         true},
        {"ravo.color.colorbalancergb",
         "Color Balance RGB",
         1,
         {{"working_space", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"algorithm", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"shadows_y", ParameterType::kNumber, true, std::nullopt, -1.0, 1.0},
          {"shadows_chroma", ParameterType::kNumber, true, std::nullopt, 0.0, 1.0},
          {"shadows_hue", ParameterType::kNumber, true, std::nullopt, 0.0, 360.0},
          {"midtones_y", ParameterType::kNumber, true, std::nullopt, -1.0, 1.0},
          {"midtones_chroma", ParameterType::kNumber, true, std::nullopt, 0.0, 1.0},
          {"midtones_hue", ParameterType::kNumber, true, std::nullopt, 0.0, 360.0},
          {"highlights_y", ParameterType::kNumber, true, std::nullopt, -1.0, 1.0},
          {"highlights_chroma", ParameterType::kNumber, true, std::nullopt, 0.0, 1.0},
          {"highlights_hue", ParameterType::kNumber, true, std::nullopt, 0.0, 360.0},
          {"global_y", ParameterType::kNumber, true, std::nullopt, -1.0, 1.0},
          {"global_chroma", ParameterType::kNumber, true, std::nullopt, 0.0, 1.0},
          {"global_hue", ParameterType::kNumber, true, std::nullopt, 0.0, 360.0},
          {"shadows_falloff", ParameterType::kNumber, true, std::nullopt, 0.0, 3.0},
          {"white_fulcrum_ev", ParameterType::kNumber, true, std::nullopt, -16.0, 16.0},
          {"highlights_falloff", ParameterType::kNumber, true, std::nullopt, 0.0, 3.0},
          {"chroma_shadows", ParameterType::kNumber, true, std::nullopt, -1.0, 1.0},
          {"chroma_highlights", ParameterType::kNumber, true, std::nullopt, -1.0, 1.0},
          {"chroma_global", ParameterType::kNumber, true, std::nullopt, -1.0, 1.0},
          {"chroma_midtones", ParameterType::kNumber, true, std::nullopt, -1.0, 1.0},
          {"saturation_global", ParameterType::kNumber, true, std::nullopt, -1.0, 1.0},
          {"saturation_highlights", ParameterType::kNumber, true, std::nullopt, -1.0, 1.0},
          {"saturation_midtones", ParameterType::kNumber, true, std::nullopt, -1.0, 1.0},
          {"saturation_shadows", ParameterType::kNumber, true, std::nullopt, -1.0, 1.0},
          {"hue_rotation", ParameterType::kNumber, true, std::nullopt, -180.0, 180.0},
          {"brilliance_global", ParameterType::kNumber, true, std::nullopt, -1.0, 1.0},
          {"brilliance_highlights", ParameterType::kNumber, true, std::nullopt, -1.0, 1.0},
          {"brilliance_midtones", ParameterType::kNumber, true, std::nullopt, -1.0, 1.0},
          {"brilliance_shadows", ParameterType::kNumber, true, std::nullopt, -1.0, 1.0},
          {"mask_grey_fulcrum", ParameterType::kNumber, true, std::nullopt, 0.0, 1.0},
          {"vibrance", ParameterType::kNumber, true, std::nullopt, -1.0, 1.0},
          {"grey_fulcrum", ParameterType::kNumber, true, std::nullopt, 0.0, 1.0},
          {"contrast", ParameterType::kNumber, true, std::nullopt, -1.0, 1.0},
          {"saturation_formula", ParameterType::kString, true, std::nullopt, std::nullopt,
           std::nullopt}},
         false,
         true},
        {"ravo.color.colorcontrast",
         "Color contrast",
         1,
         {{"amount", ParameterType::kNumber, false, ParameterValue{0.0}, -1.0, 1.0}},
         false,
         true},
        {"ravo.color.velvia",
         "Velvia",
         1,
         {{"amount", ParameterType::kNumber, false, ParameterValue{0.0}, 0.0, 1.0},
          {"bias", ParameterType::kNumber, false, ParameterValue{1.0}, 0.0, 1.0}},
         false,
         true},
        {"ravo.color.monochrome",
         "Monochrome",
         1,
         {{"amount", ParameterType::kNumber, false, ParameterValue{0.0}, 0.0, 1.0}},
         false,
         true},
        {"ravo.color.splittoning",
         "Split toning",
         1,
         {{"shadows_hue", ParameterType::kNumber, false, ParameterValue{0.55}, 0.0, 1.0},
          {"highlights_hue", ParameterType::kNumber, false, ParameterValue{0.08}, 0.0, 1.0},
          {"balance", ParameterType::kNumber, false, ParameterValue{0.5}, 0.0, 1.0},
          {"amount", ParameterType::kNumber, false, ParameterValue{0.0}, 0.0, 1.0}},
         false,
         true},
        {"ravo.detail.sharpen",
         "Sharpen",
         1,
         {{"amount", ParameterType::kNumber, false, ParameterValue{0.0}, 0.0, 2.0},
          {"radius", ParameterType::kNumber, false, ParameterValue{1.0}, 0.0, 12.0},
          {"threshold", ParameterType::kNumber, false, ParameterValue{0.0}, 0.0, 1.0}},
         false,
         true},
        {"ravo.detail.clarity",
         "Clarity",
         1,
         {{"amount", ParameterType::kNumber, false, ParameterValue{0.0}, -1.0, 1.0}},
         false,
         true},
        {"ravo.effect.vignette",
         "Vignette",
         1,
         {{"amount", ParameterType::kNumber, false, ParameterValue{0.0}, 0.0, 1.0},
          {"midpoint", ParameterType::kNumber, false, ParameterValue{0.5}, 0.0, 1.0},
          {"falloff", ParameterType::kNumber, false, ParameterValue{0.5}, 0.05, 1.0}},
         false,
         true},
        {"ravo.effect.grain",
         "Grain",
         1,
         {{"amount", ParameterType::kNumber, false, ParameterValue{0.0}, 0.0, 1.0}},
         false,
         true},
        {"ravo.effect.bloom",
         "Bloom",
         1,
         {{"amount", ParameterType::kNumber, false, ParameterValue{0.0}, 0.0, 1.0}},
         false,
         true},
        {"ravo.effect.soften",
         "Soften",
         1,
         {{"amount", ParameterType::kNumber, false, ParameterValue{0.0}, 0.0, 1.0}},
         false,
         true},
        {"ravo.effect.dehaze",
         "Dehaze",
         1,
         {{"amount", ParameterType::kNumber, false, ParameterValue{0.0}, -1.0, 1.0}},
         false,
         true},
        {"ravo.display.sigmoid",
         "Sigmoid display transform",
         1,
         {{"working_space", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"color_processing", ParameterType::kString, true, std::nullopt, std::nullopt,
           std::nullopt},
          {"middle_grey_contrast", ParameterType::kNumber, true, std::nullopt, kSigmoidContrastMin,
           kSigmoidContrastMax},
          {"contrast_skewness", ParameterType::kNumber, true, std::nullopt, kSigmoidSkewMin,
           kSigmoidSkewMax},
          {"display_white_target", ParameterType::kNumber, true, std::nullopt,
           kSigmoidDisplayWhiteMin, kSigmoidDisplayWhiteMax},
          {"display_black_target", ParameterType::kNumber, true, std::nullopt,
           kSigmoidDisplayBlackMin, kSigmoidDisplayBlackMax},
          {"hue_preservation", ParameterType::kNumber, true, std::nullopt, 0.0, 1.0}},
         false,
         true},
        {"ravo.raw.highlights",
         "RAW highlight reconstruction",
         1,
         {{"mode", ParameterType::kString, false, ParameterValue{"opposed"}, std::nullopt,
           std::nullopt},
          {"amount", ParameterType::kNumber, false, ParameterValue{0.0}, 0.0, 1.0},
          {"clip", ParameterType::kNumber, false, ParameterValue{0.987}, 0.5, 1.0}},
         false,
         true},
        {"ravo.raw.hotpixels",
         "Hot pixel correction",
         1,
         {{"strength", ParameterType::kNumber, false, ParameterValue{0.25}, 0.0, 1.0},
          {"threshold", ParameterType::kNumber, false, ParameterValue{0.05}, 0.0, 1.0},
          {"permissive", ParameterType::kBoolean, false, ParameterValue{false}, std::nullopt,
           std::nullopt}},
         false,
         true},
        {"ravo.raw.cacorrect",
         "RAW chromatic aberration correction",
         1,
         {{"iterations", ParameterType::kInteger, true, std::nullopt, 1.0, 5.0},
          {"avoid_color_shift", ParameterType::kBoolean, true, std::nullopt, std::nullopt,
           std::nullopt}},
         false,
         true},
        {"ravo.detail.denoiseprofile",
         "Profile denoise",
         1,
         {{"strength", ParameterType::kNumber, false, ParameterValue{0.0}, 0.0, 1.0},
          {"chroma", ParameterType::kNumber, false, ParameterValue{1.0}, 0.0, 1.0},
          {"radius", ParameterType::kNumber, false, ParameterValue{1.0}, 0.5, 8.0}},
         false,
         true},
        {"ravo.geometry.lens",
         "Lens correction",
         1,
         {{"mode", ParameterType::kString, false, ParameterValue{"manual"}, std::nullopt,
           std::nullopt},
          {"k1", ParameterType::kNumber, false, ParameterValue{0.0}, -2.0, 2.0},
          {"k2", ParameterType::kNumber, false, ParameterValue{0.0}, -2.0, 2.0},
          {"tca_r", ParameterType::kNumber, false, ParameterValue{1.0}, 0.9, 1.1},
          {"tca_b", ParameterType::kNumber, false, ParameterValue{1.0}, 0.9, 1.1},
          {"vignetting", ParameterType::kNumber, false, ParameterValue{0.0}, 0.0, 1.0},
          {"camera_make", ParameterType::kString, false, ParameterValue{""}, std::nullopt,
           std::nullopt},
          {"camera_model", ParameterType::kString, false, ParameterValue{""}, std::nullopt,
           std::nullopt},
          {"lens", ParameterType::kString, false, ParameterValue{""}, std::nullopt, std::nullopt},
          {"focal_mm", ParameterType::kNumber, false, ParameterValue{50.0}, 1.0, 2000.0}},
         false,
         true},
        {"ravo.color.colorequal",
         "Color equalizer",
         1,
         {{"hue_shift", ParameterType::kArray, false, std::nullopt, std::nullopt, std::nullopt},
          {"saturation", ParameterType::kArray, false, std::nullopt, std::nullopt, std::nullopt},
          {"lightness", ParameterType::kArray, false, std::nullopt, std::nullopt, std::nullopt}},
         false,
         true},
        {"ravo.effect.graduatednd",
         "Graduated ND",
         1,
         {{"density_ev", ParameterType::kNumber, false, ParameterValue{0.0}, -4.0, 4.0},
          {"hardness", ParameterType::kNumber, false, ParameterValue{0.5}, 0.0, 1.0},
          {"rotation_deg", ParameterType::kNumber, false, ParameterValue{0.0}, -180.0, 180.0},
          {"offset", ParameterType::kNumber, false, ParameterValue{0.0}, -1.0, 1.0}},
         false,
         true},
        {"ravo.core.toneequal",
         "Tone equalizer",
         1,
         {{"blacks", ParameterType::kNumber, false, ParameterValue{0.0}, -4.0, 4.0},
          {"shadows", ParameterType::kNumber, false, ParameterValue{0.0}, -4.0, 4.0},
          {"midtones", ParameterType::kNumber, false, ParameterValue{0.0}, -4.0, 4.0},
          {"highlights", ParameterType::kNumber, false, ParameterValue{0.0}, -4.0, 4.0},
          {"whites", ParameterType::kNumber, false, ParameterValue{0.0}, -4.0, 4.0}},
         false,
         true},
        {"ravo.color.output",
         "Output colour",
         1,
         {{"output_profile", ParameterType::kString, true, std::nullopt, std::nullopt,
           std::nullopt},
          {"output_profile_filename", ParameterType::kString, true, std::nullopt, std::nullopt,
           std::nullopt},
          {"rendering_intent", ParameterType::kString, true, std::nullopt, std::nullopt,
           std::nullopt},
          {"proof_mode", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"proof_profile", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"proof_profile_filename", ParameterType::kString, true, std::nullopt, std::nullopt,
           std::nullopt},
          {"proof_intent", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"black_point_compensation", ParameterType::kBoolean, true, std::nullopt, std::nullopt,
           std::nullopt}},
         false,
         true},
        {"ravo.output.scale",
         "Output scale",
         1,
         {{"max_dimension", ParameterType::kInteger, false, ParameterValue{std::int64_t{0}}, 0.0,
           100000.0}},
         false,
         false},
    });
}

std::string_view parameter_type_name(const ParameterType type) noexcept
{
    switch (type)
    {
    case ParameterType::kBoolean:
        return "boolean";
    case ParameterType::kInteger:
        return "integer";
    case ParameterType::kNumber:
        return "number";
    case ParameterType::kString:
        return "string";
    case ParameterType::kArray:
        return "array";
    case ParameterType::kObject:
        return "object";
    }
    return "string";
}

Result<JsonValue> operation_descriptor_to_json(const OperationDescriptor &descriptor)
{
    JsonValue::Array parameters;
    for (const auto &rule : descriptor.parameters)
    {
        JsonValue::Object parameter{
            {"name", rule.name},
            {"required", rule.required},
            {"type", std::string(parameter_type_name(rule.type))},
        };
        if (rule.default_value.has_value())
        {
            auto default_value = parameter_value_to_json(*rule.default_value);
            if (!default_value)
            {
                return default_value.error();
            }
            parameter.emplace("default", std::move(default_value).value());
        }
        if (rule.minimum.has_value())
        {
            parameter.emplace("minimum", JsonValue::number(std::to_string(*rule.minimum)));
        }
        if (rule.maximum.has_value())
        {
            parameter.emplace("maximum", JsonValue::number(std::to_string(*rule.maximum)));
        }
        parameters.emplace_back(std::move(parameter));
    }

    return JsonValue{JsonValue::Object{
        {"capabilities",
         JsonValue::Object{{"cpu_reference_available", descriptor.cpu_reference_available},
                           {"supports_mask", descriptor.supports_mask}}},
        {"display_name", descriptor.display_name},
        {"id", descriptor.id},
        {"parameter_schema_version",
         JsonValue::number(std::to_string(descriptor.parameter_schema_version))},
        {"parameters", std::move(parameters)},
    }};
}

} // namespace ravo
