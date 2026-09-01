#include "ravo/recipe/operation.h"
#include "ravo/recipe/output_dither.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include "ravo/recipe/color_contrast.h"
#include "ravo/recipe/canvas_frame.h"
#include "ravo/recipe/color_correction.h"
#include "ravo/recipe/color_harmonizer.h"
#include "ravo/recipe/color_input.h"
#include "ravo/recipe/color_output.h"
#include "ravo/recipe/color_reconstruction.h"
#include "ravo/recipe/color_zones.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/dehaze.h"
#include "ravo/recipe/lut3d.h"
#include "ravo/recipe/monochrome.h"
#include "ravo/recipe/perspective.h"
#include "ravo/recipe/profile_gamma.h"
#include "ravo/recipe/primaries.h"
#include "ravo/recipe/retouch.h"
#include "ravo/recipe/sharpen.h"
#include "ravo/recipe/split_toning.h"
#include "ravo/recipe/texture.h"
#include "ravo/recipe/watermark.h"
#include "ravo/recipe/velvia.h"

namespace ravo
{

namespace
{

[[nodiscard]] Result<double>
exposure_number(const std::map<std::string, ParameterValue, std::less<>> &parameters,
                const std::string_view name, const double minimum, const double maximum)
{
    const auto found = parameters.find(std::string(name));
    if (found == parameters.end())
    {
        return make_error(ErrorCode::kValidation, "Exposure parameter is required",
                          {{"parameter", std::string(name)}});
    }
    const auto *floating = std::get_if<double>(&found->second.value);
    const auto *integer = std::get_if<std::int64_t>(&found->second.value);
    if (floating == nullptr && integer == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Exposure parameter must be numeric",
                          {{"parameter", std::string(name)}});
    }
    const double value = floating != nullptr ? *floating : static_cast<double>(*integer);
    if (!std::isfinite(value))
    {
        return make_error(ErrorCode::kValidation, "Exposure parameter must be finite",
                          {{"parameter", std::string(name)}});
    }
    if (value < minimum || value > maximum)
    {
        return make_error(ErrorCode::kValidation,
                          "Exposure parameter is outside the permitted range",
                          {{"parameter", std::string(name)}});
    }
    return value;
}

[[nodiscard]] Result<bool>
exposure_boolean(const std::map<std::string, ParameterValue, std::less<>> &parameters,
                 const std::string_view name)
{
    const auto found = parameters.find(std::string(name));
    if (found == parameters.end())
    {
        return make_error(ErrorCode::kValidation, "Exposure parameter is required",
                          {{"parameter", std::string(name)}});
    }
    const auto *value = std::get_if<bool>(&found->second.value);
    if (value == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Exposure parameter must be boolean",
                          {{"parameter", std::string(name)}});
    }
    return *value;
}

} // namespace

Result<std::string>
demosaic_mode_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    const auto found = parameters.find("mode");
    if (found == parameters.end())
    {
        return std::string(kDemosaicModeRcd);
    }
    const auto *mode = std::get_if<std::string>(&found->second.value);
    if (mode == nullptr ||
        (*mode != kDemosaicModeRcd && *mode != kDemosaicModePpg &&
         *mode != kDemosaicModeMarkesteijn1 && *mode != kDemosaicModeMarkesteijn3))
    {
        return make_error(ErrorCode::kUnsupported, "RAW demosaic mode is unsupported",
                          {{"reason", "unsupported_demosaic_mode"}});
    }
    return *mode;
}

Result<void>
validate_demosaic_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    auto mode = demosaic_mode_from_parameters(parameters);
    if (!mode)
    {
        return mode.error();
    }
    return {};
}

bool ExposureParams::is_identity() const noexcept
{
    return mode == kExposureModeManual && black == 0.0 && exposure_ev == 0.0 &&
           deflicker_percentile == kExposureDeflickerPercentileDefault &&
           deflicker_target_ev == kExposureDeflickerTargetEvDefault && !compensate_exposure_bias &&
           !compensate_highlight_preservation;
}

Result<ExposureParams>
exposure_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    constexpr std::array<std::string_view, 7> kNames{"mode",
                                                     "black",
                                                     "exposure_ev",
                                                     "deflicker_percentile",
                                                     "deflicker_target_ev",
                                                     "compensate_exposure_bias",
                                                     "compensate_highlight_preservation"};
    for (const auto &[name, value] : parameters)
    {
        (void)value;
        if (std::find(kNames.begin(), kNames.end(), name) == kNames.end())
        {
            return make_error(ErrorCode::kValidation, "Exposure parameter is unknown",
                              {{"parameter", name}});
        }
    }

    const auto mode_entry = parameters.find("mode");
    if (mode_entry == parameters.end())
    {
        return make_error(ErrorCode::kValidation, "Exposure parameter is required",
                          {{"parameter", "mode"}});
    }
    const auto *mode = std::get_if<std::string>(&mode_entry->second.value);
    if (mode == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Exposure mode must be a string",
                          {{"parameter", "mode"}});
    }
    if (*mode != kExposureModeManual && *mode != kExposureModeDeflicker)
    {
        return make_error(ErrorCode::kValidation, "Exposure mode is unsupported",
                          {{"parameter", "mode"}, {"mode", *mode}});
    }

    auto black = exposure_number(parameters, "black", kExposureBlackMin, kExposureBlackMax);
    auto exposure = exposure_number(parameters, "exposure_ev", kExposureEvMin, kExposureEvMax);
    auto percentile =
        exposure_number(parameters, "deflicker_percentile", kExposureDeflickerPercentileMin,
                        kExposureDeflickerPercentileMax);
    auto target = exposure_number(parameters, "deflicker_target_ev", kExposureDeflickerTargetEvMin,
                                  kExposureDeflickerTargetEvMax);
    auto compensate_bias = exposure_boolean(parameters, "compensate_exposure_bias");
    auto compensate_highlight = exposure_boolean(parameters, "compensate_highlight_preservation");
    if (!black || !exposure || !percentile || !target || !compensate_bias || !compensate_highlight)
    {
        return !black           ? black.error() :
               !exposure        ? exposure.error() :
               !percentile      ? percentile.error() :
               !target          ? target.error() :
               !compensate_bias ? compensate_bias.error() :
                                  compensate_highlight.error();
    }

    if (*mode == kExposureModeManual && black.value() >= std::exp2(-exposure.value()))
    {
        return make_error(ErrorCode::kValidation,
                          "Exposure black level must remain below the derived white level",
                          {{"parameter", "black"}, {"reason", "invalid_exposure_denominator"}});
    }
    return ExposureParams{*mode,
                          black.value(),
                          exposure.value(),
                          percentile.value(),
                          target.value(),
                          compensate_bias.value(),
                          compensate_highlight.value()};
}

std::map<std::string, ParameterValue, std::less<>>
exposure_to_parameters(const ExposureParams &params)
{
    return {{"black", ParameterValue{params.black}},
            {"compensate_exposure_bias", ParameterValue{params.compensate_exposure_bias}},
            {"compensate_highlight_preservation",
             ParameterValue{params.compensate_highlight_preservation}},
            {"deflicker_percentile", ParameterValue{params.deflicker_percentile}},
            {"deflicker_target_ev", ParameterValue{params.deflicker_target_ev}},
            {"exposure_ev", ParameterValue{params.exposure_ev}},
            {"mode", ParameterValue{params.mode}}};
}

Result<void>
validate_exposure_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    auto parsed = exposure_from_parameters(parameters);
    if (!parsed)
    {
        return parsed.error();
    }
    return {};
}

Result<void> upgrade_exposure_operation(OperationInstance &operation)
{
    if (operation.id != kExposureOperationId)
    {
        return make_error(ErrorCode::kValidation, "Operation is not exposure",
                          {{"operation_id", operation.id}});
    }
    if (operation.schema_version == kExposureOperationSchemaVersion)
    {
        return {};
    }
    if (operation.schema_version != 1)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Exposure operation schema version is unsupported",
                          {{"operation_id", operation.id},
                           {"schema_version", std::to_string(operation.schema_version)}});
    }
    if (operation.parameters.size() > 1U ||
        (operation.parameters.size() == 1U && !operation.parameters.contains("exposure_ev")))
    {
        return make_error(ErrorCode::kValidation, "Exposure v1 contains an unknown parameter",
                          {{"operation_id", operation.id}});
    }
    double exposure_ev = 0.0;
    if (operation.parameters.contains("exposure_ev"))
    {
        auto exposure = exposure_number(operation.parameters, "exposure_ev", -10.0, 10.0);
        if (!exposure)
        {
            auto error = exposure.error();
            error.context.emplace("operation_id", operation.id);
            return error;
        }
        exposure_ev = exposure.value();
    }
    ExposureParams params;
    params.exposure_ev = exposure_ev;
    operation.schema_version = kExposureOperationSchemaVersion;
    operation.parameters = exposure_to_parameters(params);
    return {};
}

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
        {std::string(kDemosaicOperationId),
         "RAW demosaic",
         1,
         {{"mode", ParameterType::kString, false, ParameterValue{std::string(kDemosaicModeRcd)},
           std::nullopt, std::nullopt}},
         false,
         true},
        {std::string(kProfileGammaOperationId),
         "Unbreak input profile",
         kProfileGammaOperationSchemaVersion,
         {{"mode", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"linear", ParameterType::kNumber, true, std::nullopt, kProfileGammaLinearMin,
           kProfileGammaLinearMax},
          {"gamma", ParameterType::kNumber, true, std::nullopt, kProfileGammaGammaMin,
           kProfileGammaGammaMax},
          {"dynamic_range", ParameterType::kNumber, true, std::nullopt,
           kProfileGammaDynamicRangeMin, kProfileGammaDynamicRangeMax},
          {"grey_point", ParameterType::kNumber, true, std::nullopt, kProfileGammaGreyPointMin,
           kProfileGammaGreyPointMax},
          {"shadows_range", ParameterType::kNumber, true, std::nullopt,
           kProfileGammaShadowsRangeMin, kProfileGammaShadowsRangeMax},
          {"security_factor", ParameterType::kNumber, true, std::nullopt,
           kProfileGammaSecurityFactorMin, kProfileGammaSecurityFactorMax}},
         false,
         true},
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
        {std::string(kPrimariesOperationId),
         "RGB primaries",
         1,
         {{"achromatic_tint_hue", ParameterType::kNumber, true, std::nullopt, kPrimariesHueMin,
           kPrimariesHueMax},
          {"achromatic_tint_purity", ParameterType::kNumber, true, std::nullopt,
           kPrimariesAchromaticTintPurityMin, kPrimariesAchromaticTintPurityMax},
          {"red_hue", ParameterType::kNumber, true, std::nullopt, kPrimariesHueMin,
           kPrimariesHueMax},
          {"red_purity", ParameterType::kNumber, true, std::nullopt, kPrimariesPrimaryPurityMin,
           kPrimariesPrimaryPurityMax},
          {"green_hue", ParameterType::kNumber, true, std::nullopt, kPrimariesHueMin,
           kPrimariesHueMax},
          {"green_purity", ParameterType::kNumber, true, std::nullopt, kPrimariesPrimaryPurityMin,
           kPrimariesPrimaryPurityMax},
          {"blue_hue", ParameterType::kNumber, true, std::nullopt, kPrimariesHueMin,
           kPrimariesHueMax},
          {"blue_purity", ParameterType::kNumber, true, std::nullopt, kPrimariesPrimaryPurityMin,
           kPrimariesPrimaryPurityMax}},
         false,
         true},
        {std::string(kExposureOperationId),
         "Exposure",
         kExposureOperationSchemaVersion,
         {{"mode", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"black", ParameterType::kNumber, true, std::nullopt, kExposureBlackMin,
           kExposureBlackMax},
          {"exposure_ev", ParameterType::kNumber, true, std::nullopt, kExposureEvMin,
           kExposureEvMax},
          {"deflicker_percentile", ParameterType::kNumber, true, std::nullopt,
           kExposureDeflickerPercentileMin, kExposureDeflickerPercentileMax},
          {"deflicker_target_ev", ParameterType::kNumber, true, std::nullopt,
           kExposureDeflickerTargetEvMin, kExposureDeflickerTargetEvMax},
          {"compensate_exposure_bias", ParameterType::kBoolean, true, std::nullopt, std::nullopt,
           std::nullopt},
          {"compensate_highlight_preservation", ParameterType::kBoolean, true, std::nullopt,
           std::nullopt, std::nullopt}},
         true,
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
        {std::string(kPerspectiveOperationId),
         "Perspective",
         kPerspectiveOperationSchemaVersion,
         {{"working_space", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"algorithm", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"rotation_degrees", ParameterType::kNumber, true, std::nullopt, kPerspectiveRotationMin,
           kPerspectiveRotationMax},
          {"vertical_shift", ParameterType::kNumber, true, std::nullopt, kPerspectiveShiftMin,
           kPerspectiveShiftMax},
          {"horizontal_shift", ParameterType::kNumber, true, std::nullopt, kPerspectiveShiftMin,
           kPerspectiveShiftMax},
          {"shear", ParameterType::kNumber, true, std::nullopt, kPerspectiveShearMin,
           kPerspectiveShearMax},
          {"constrain_crop", ParameterType::kBoolean, true, std::nullopt, std::nullopt,
           std::nullopt},
          {"interpolation", ParameterType::kString, true, std::nullopt, std::nullopt,
           std::nullopt}},
         false,
         true},
        {"ravo.core.gamma",
         "Gamma",
         1,
         {{"gamma", ParameterType::kNumber, false, ParameterValue{1.0}, 0.2, 3.0}},
         false,
         true},
        {"ravo.color.rgblevels",
         "RGB levels",
         1,
         {{"mode", ParameterType::kString, false, ParameterValue{"linked"}, std::nullopt,
           std::nullopt},
          {"preserve_colors", ParameterType::kString, false, ParameterValue{"luminance"},
           std::nullopt, std::nullopt},
          {"black", ParameterType::kNumber, false, ParameterValue{0.0}, 0.0, 1.0},
          {"grey", ParameterType::kNumber, false, ParameterValue{0.5}, 0.0, 1.0},
          {"white", ParameterType::kNumber, false, ParameterValue{1.0}, 0.0, 1.0},
          {"black_g", ParameterType::kNumber, false, ParameterValue{0.0}, 0.0, 1.0},
          {"grey_g", ParameterType::kNumber, false, ParameterValue{0.5}, 0.0, 1.0},
          {"white_g", ParameterType::kNumber, false, ParameterValue{1.0}, 0.0, 1.0},
          {"black_b", ParameterType::kNumber, false, ParameterValue{0.0}, 0.0, 1.0},
          {"grey_b", ParameterType::kNumber, false, ParameterValue{0.5}, 0.0, 1.0},
          {"white_b", ParameterType::kNumber, false, ParameterValue{1.0}, 0.0, 1.0}},
         false,
         true},
        {"ravo.color.rgbcurve",
         "RGB curve",
         1,
         {{"mode", ParameterType::kString, false, ParameterValue{"linked"}, std::nullopt,
           std::nullopt},
          {"preserve_colors", ParameterType::kString, false, ParameterValue{"luminance"},
           std::nullopt, std::nullopt},
          {"interpolation", ParameterType::kString, false, ParameterValue{"monotone_hermite"},
           std::nullopt, std::nullopt},
          {"application_space", ParameterType::kString, false, ParameterValue{"scene_linear"},
           std::nullopt, std::nullopt},
          {"compensate_middle_grey", ParameterType::kBoolean, false, ParameterValue{false},
           std::nullopt, std::nullopt},
          {"points", ParameterType::kArray, false,
           ParameterValue{ParameterValue::Array{
               ParameterValue{
                   ParameterValue::Object{{"x", ParameterValue{0.0}}, {"y", ParameterValue{0.0}}}},
               ParameterValue{
                   ParameterValue::Object{{"x", ParameterValue{1.0}}, {"y", ParameterValue{1.0}}}},
           }},
           std::nullopt, std::nullopt},
          {"points_g", ParameterType::kArray, false,
           ParameterValue{ParameterValue::Array{
               ParameterValue{
                   ParameterValue::Object{{"x", ParameterValue{0.0}}, {"y", ParameterValue{0.0}}}},
               ParameterValue{
                   ParameterValue::Object{{"x", ParameterValue{1.0}}, {"y", ParameterValue{1.0}}}},
           }},
           std::nullopt, std::nullopt},
          {"points_b", ParameterType::kArray, false,
           ParameterValue{ParameterValue::Array{
               ParameterValue{
                   ParameterValue::Object{{"x", ParameterValue{0.0}}, {"y", ParameterValue{0.0}}}},
               ParameterValue{
                   ParameterValue::Object{{"x", ParameterValue{1.0}}, {"y", ParameterValue{1.0}}}},
           }},
           std::nullopt, std::nullopt},
          {"parametric_shadows", ParameterType::kNumber, false, ParameterValue{0.0}, -1.0, 1.0},
          {"parametric_darks", ParameterType::kNumber, false, ParameterValue{0.0}, -1.0, 1.0},
          {"parametric_lights", ParameterType::kNumber, false, ParameterValue{0.0}, -1.0, 1.0},
          {"parametric_highlights", ParameterType::kNumber, false, ParameterValue{0.0}, -1.0, 1.0},
          {"parametric_split_shadows", ParameterType::kNumber, false, ParameterValue{0.25}, 0.05,
           0.90},
          {"parametric_split_mid", ParameterType::kNumber, false, ParameterValue{0.50}, 0.10, 0.95},
          {"parametric_split_highlights", ParameterType::kNumber, false, ParameterValue{0.75}, 0.15,
           0.98}},
         true,
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
           std::nullopt, std::nullopt},
          {"points_a", ParameterType::kArray, false,
           ParameterValue{ParameterValue::Array{
               ParameterValue{
                   ParameterValue::Object{{"x", ParameterValue{0.0}}, {"y", ParameterValue{0.0}}}},
               ParameterValue{
                   ParameterValue::Object{{"x", ParameterValue{1.0}}, {"y", ParameterValue{1.0}}}},
           }},
           std::nullopt, std::nullopt},
          {"points_b", ParameterType::kArray, false,
           ParameterValue{ParameterValue::Array{
               ParameterValue{
                   ParameterValue::Object{{"x", ParameterValue{0.0}}, {"y", ParameterValue{0.0}}}},
               ParameterValue{
                   ParameterValue::Object{{"x", ParameterValue{1.0}}, {"y", ParameterValue{1.0}}}},
           }},
           std::nullopt, std::nullopt}},
         false,
         true},
        {std::string(kColorBalanceOperationId),
         "Color Balance",
         kColorBalanceOperationSchemaVersion,
         {{"working_space", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"algorithm", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"mode", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"lift_factor", ParameterType::kNumber, true, std::nullopt, 0.0, 2.0},
          {"lift_red", ParameterType::kNumber, true, std::nullopt, 0.0, 2.0},
          {"lift_green", ParameterType::kNumber, true, std::nullopt, 0.0, 2.0},
          {"lift_blue", ParameterType::kNumber, true, std::nullopt, 0.0, 2.0},
          {"gamma_factor", ParameterType::kNumber, true, std::nullopt, 0.0, 2.0},
          {"gamma_red", ParameterType::kNumber, true, std::nullopt, 0.0, 2.0},
          {"gamma_green", ParameterType::kNumber, true, std::nullopt, 0.0, 2.0},
          {"gamma_blue", ParameterType::kNumber, true, std::nullopt, 0.0, 2.0},
          {"gain_factor", ParameterType::kNumber, true, std::nullopt, 0.0, 2.0},
          {"gain_red", ParameterType::kNumber, true, std::nullopt, 0.0, 2.0},
          {"gain_green", ParameterType::kNumber, true, std::nullopt, 0.0, 2.0},
          {"gain_blue", ParameterType::kNumber, true, std::nullopt, 0.0, 2.0},
          {"input_saturation", ParameterType::kNumber, true, std::nullopt, 0.0, 2.0},
          {"contrast", ParameterType::kNumber, true, std::nullopt, 0.01, 1.99},
          {"grey_fulcrum_percent", ParameterType::kNumber, true, std::nullopt, 0.1, 100.0},
          {"output_saturation", ParameterType::kNumber, true, std::nullopt, 0.0, 2.0}},
         false,
         true},
        {std::string(kColorCheckerOperationId),
         "Color look-up table",
         kColorCheckerOperationSchemaVersion,
         {{"working_space", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"algorithm", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"patches", ParameterType::kArray, true, std::nullopt, std::nullopt, std::nullopt}},
         false,
         true},
        {std::string(kColorHarmonizerOperationId),
         "Color Harmonizer",
         kColorHarmonizerOperationSchemaVersion,
         {{"working_space", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"algorithm", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"rule", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"anchor_hue", ParameterType::kNumber, true, std::nullopt, kColorHarmonizerHueMin,
           kColorHarmonizerHueMax},
          {"pull_strength", ParameterType::kNumber, true, std::nullopt,
           kColorHarmonizerPullStrengthMin, kColorHarmonizerPullStrengthMax},
          {"neutral_protection", ParameterType::kNumber, true, std::nullopt,
           kColorHarmonizerNeutralProtectionMin, kColorHarmonizerNeutralProtectionMax},
          {"pull_width", ParameterType::kNumber, true, std::nullopt, kColorHarmonizerPullWidthMin,
           kColorHarmonizerPullWidthMax},
          {"custom_hue_0", ParameterType::kNumber, true, std::nullopt, kColorHarmonizerHueMin,
           kColorHarmonizerHueMax},
          {"custom_hue_1", ParameterType::kNumber, true, std::nullopt, kColorHarmonizerHueMin,
           kColorHarmonizerHueMax},
          {"custom_hue_2", ParameterType::kNumber, true, std::nullopt, kColorHarmonizerHueMin,
           kColorHarmonizerHueMax},
          {"custom_hue_3", ParameterType::kNumber, true, std::nullopt, kColorHarmonizerHueMin,
           kColorHarmonizerHueMax},
          {"num_custom_nodes", ParameterType::kInteger, true, std::nullopt,
           static_cast<double>(kColorHarmonizerCustomNodesMin),
           static_cast<double>(kColorHarmonizerCustomNodesMax)},
          {"node_saturation_0", ParameterType::kNumber, true, std::nullopt,
           kColorHarmonizerNodeSaturationMin, kColorHarmonizerNodeSaturationMax},
          {"node_saturation_1", ParameterType::kNumber, true, std::nullopt,
           kColorHarmonizerNodeSaturationMin, kColorHarmonizerNodeSaturationMax},
          {"node_saturation_2", ParameterType::kNumber, true, std::nullopt,
           kColorHarmonizerNodeSaturationMin, kColorHarmonizerNodeSaturationMax},
          {"node_saturation_3", ParameterType::kNumber, true, std::nullopt,
           kColorHarmonizerNodeSaturationMin, kColorHarmonizerNodeSaturationMax},
          {"smoothing", ParameterType::kNumber, true, std::nullopt, kColorHarmonizerSmoothingMin,
           kColorHarmonizerSmoothingMax}},
         true,
         true},
        {std::string(kColorCorrectionOperationId),
         "Color Correction",
         kColorCorrectionOperationSchemaVersion,
         {{"working_space", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"algorithm", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"highlight_a", ParameterType::kNumber, true, std::nullopt, kColorCorrectionEndpointMin,
           kColorCorrectionEndpointMax},
          {"highlight_b", ParameterType::kNumber, true, std::nullopt, kColorCorrectionEndpointMin,
           kColorCorrectionEndpointMax},
          {"shadow_a", ParameterType::kNumber, true, std::nullopt, kColorCorrectionEndpointMin,
           kColorCorrectionEndpointMax},
          {"shadow_b", ParameterType::kNumber, true, std::nullopt, kColorCorrectionEndpointMin,
           kColorCorrectionEndpointMax},
          {"saturation", ParameterType::kNumber, true, std::nullopt, kColorCorrectionSaturationMin,
           kColorCorrectionSaturationMax}},
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
         true,
         true},
        {std::string(kColorContrastOperationId),
         "Color Contrast",
         kColorContrastOperationSchemaVersion,
         {{"working_space", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"algorithm", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"a_steepness", ParameterType::kNumber, true, std::nullopt, kColorContrastSteepnessMin,
           kColorContrastSteepnessMax},
          {"a_offset", ParameterType::kNumber, true, std::nullopt, std::nullopt, std::nullopt},
          {"b_steepness", ParameterType::kNumber, true, std::nullopt, kColorContrastSteepnessMin,
           kColorContrastSteepnessMax},
          {"b_offset", ParameterType::kNumber, true, std::nullopt, std::nullopt, std::nullopt},
          {"unbound", ParameterType::kBoolean, true, std::nullopt, std::nullopt, std::nullopt}},
         false,
         true},
        {std::string(kColorReconstructionOperationId),
         "Color Reconstruction",
         kColorReconstructionOperationSchemaVersion,
         {{"working_space", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"algorithm", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"threshold", ParameterType::kNumber, true, std::nullopt,
           kColorReconstructionThresholdMin, kColorReconstructionThresholdMax},
          {"spatial", ParameterType::kNumber, true, std::nullopt, kColorReconstructionSpatialMin,
           kColorReconstructionSpatialMax},
          {"range", ParameterType::kNumber, true, std::nullopt, kColorReconstructionRangeMin,
           kColorReconstructionRangeMax},
          {"hue", ParameterType::kNumber, true, std::nullopt, kColorReconstructionHueMin,
           kColorReconstructionHueMax},
          {"precedence", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt}},
         false,
         true},
        {std::string(kVelviaOperationId),
         "Velvia",
         kVelviaOperationSchemaVersion,
         {{"working_space", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"algorithm", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"strength", ParameterType::kNumber, true, std::nullopt, 0.0, 100.0},
          {"bias", ParameterType::kNumber, true, std::nullopt, 0.0, 1.0}},
         true,
         true},
        {std::string(kLut3dOperationId),
         "3D LUT",
         kLut3dOperationSchemaVersion,
         {{"file_path", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"input_space", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"output_space", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"interpolation", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"strength", ParameterType::kNumber, true, std::nullopt, 0.0, 1.0}},
         false,
         true},
        {std::string(kMonochromeOperationId),
         "Monochrome",
         kMonochromeOperationSchemaVersion,
         {{"working_space", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"algorithm", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"filter_a", ParameterType::kNumber, true, std::nullopt, -128.0, 128.0},
          {"filter_b", ParameterType::kNumber, true, std::nullopt, -128.0, 128.0},
          {"size", ParameterType::kNumber, true, std::nullopt, 0.5, 3.0},
          {"highlights", ParameterType::kNumber, true, std::nullopt, 0.0, 1.0},
          {"mix", ParameterType::kNumber, true, std::nullopt, 0.0, 1.0}},
         true,
         true},
        {std::string(kSplitToningOperationId),
         "Split toning",
         kSplitToningOperationSchemaVersion,
         {{"working_space", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"algorithm", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"shadow_hue", ParameterType::kNumber, true, std::nullopt, 0.0, 1.0},
          {"shadow_saturation", ParameterType::kNumber, true, std::nullopt, 0.0, 1.0},
          {"highlight_hue", ParameterType::kNumber, true, std::nullopt, 0.0, 1.0},
          {"highlight_saturation", ParameterType::kNumber, true, std::nullopt, 0.0, 1.0},
          {"balance", ParameterType::kNumber, true, std::nullopt, 0.0, 1.0},
          {"compress", ParameterType::kNumber, true, std::nullopt, 0.0, 100.0},
          {"mix", ParameterType::kNumber, true, std::nullopt, 0.0, 1.0}},
         true,
         true},
        {std::string(kSharpenOperationId),
         "Sharpen",
         kSharpenOperationSchemaVersion,
         {{"working_space", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"algorithm", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"radius", ParameterType::kNumber, true, std::nullopt, kSharpenRadiusMin,
           kSharpenRadiusMax},
          {"amount", ParameterType::kNumber, true, std::nullopt, kSharpenAmountMin,
           kSharpenAmountMax},
          {"threshold", ParameterType::kNumber, true, std::nullopt, kSharpenThresholdMin,
           kSharpenThresholdMax}},
         false,
         true},
        {std::string(kTextureOperationId),
         "Texture",
         kTextureOperationSchemaVersion,
         {{"working_space", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"algorithm", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"strength", ParameterType::kNumber, true, std::nullopt, kTextureStrengthMin,
           kTextureStrengthMax},
          {"detail_threshold", ParameterType::kNumber, true, std::nullopt,
           kTextureDetailThresholdMin, kTextureDetailThresholdMax},
          {"iterations", ParameterType::kInteger, true, std::nullopt,
           static_cast<double>(kTextureIterationsMin), static_cast<double>(kTextureIterationsMax)}},
         false,
         true},
        {std::string(kRetouchOperationId),
         "Retouch",
         kRetouchOperationSchemaVersion,
         {{"working_space", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"algorithm", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"num_scales", ParameterType::kInteger, true, std::nullopt, 0.0,
           static_cast<double>(kRetouchMaxScales)},
          {"merge_from_scale", ParameterType::kInteger, true, std::nullopt, 0.0,
           static_cast<double>(kRetouchMaxScales)},
          {"max_heal_iterations", ParameterType::kInteger, true, std::nullopt, 1.0,
           static_cast<double>(kRetouchMaxHealIterations)},
          {"regions", ParameterType::kArray, true, std::nullopt, std::nullopt, std::nullopt}},
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
         {{"amount", ParameterType::kNumber, false, ParameterValue{0.0}, -1.0, 1.0},
          {"midpoint", ParameterType::kNumber, false, ParameterValue{0.8}, 0.0, 1.0},
          {"falloff", ParameterType::kNumber, false, ParameterValue{0.5}, 0.05, 1.0},
          {"shape", ParameterType::kNumber, false, ParameterValue{1.0}, 0.5, 5.0},
          {"center_x", ParameterType::kNumber, false, ParameterValue{0.0}, -1.0, 1.0},
          {"center_y", ParameterType::kNumber, false, ParameterValue{0.0}, -1.0, 1.0}},
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
        {std::string(kDehazeOperationId),
         "Dehaze",
         kDehazeOperationSchemaVersion,
         {{"working_space", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"algorithm", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"strength", ParameterType::kNumber, true, std::nullopt, kDehazeStrengthMin,
           kDehazeStrengthMax},
          {"distance", ParameterType::kNumber, true, std::nullopt, kDehazeDistanceMin,
           kDehazeDistanceMax},
          {"adaptive", ParameterType::kBoolean, true, std::nullopt, std::nullopt, std::nullopt}},
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
        {"ravo.raw.denoise",
         "RAW denoise",
         1,
         {{"threshold", ParameterType::kNumber, false, ParameterValue{0.01}, 0.0, 1.0},
          {"y_all0", ParameterType::kNumber, false, ParameterValue{0.5}, 0.0, 16.0},
          {"y_all1", ParameterType::kNumber, false, ParameterValue{0.5}, 0.0, 16.0},
          {"y_all2", ParameterType::kNumber, false, ParameterValue{0.5}, 0.0, 16.0},
          {"y_all3", ParameterType::kNumber, false, ParameterValue{0.5}, 0.0, 16.0},
          {"y_all4", ParameterType::kNumber, false, ParameterValue{0.5}, 0.0, 16.0},
          {"y_red0", ParameterType::kNumber, false, ParameterValue{0.5}, 0.0, 16.0},
          {"y_red1", ParameterType::kNumber, false, ParameterValue{0.5}, 0.0, 16.0},
          {"y_red2", ParameterType::kNumber, false, ParameterValue{0.5}, 0.0, 16.0},
          {"y_red3", ParameterType::kNumber, false, ParameterValue{0.5}, 0.0, 16.0},
          {"y_red4", ParameterType::kNumber, false, ParameterValue{0.5}, 0.0, 16.0},
          {"y_green0", ParameterType::kNumber, false, ParameterValue{0.5}, 0.0, 16.0},
          {"y_green1", ParameterType::kNumber, false, ParameterValue{0.5}, 0.0, 16.0},
          {"y_green2", ParameterType::kNumber, false, ParameterValue{0.5}, 0.0, 16.0},
          {"y_green3", ParameterType::kNumber, false, ParameterValue{0.5}, 0.0, 16.0},
          {"y_green4", ParameterType::kNumber, false, ParameterValue{0.5}, 0.0, 16.0},
          {"y_blue0", ParameterType::kNumber, false, ParameterValue{0.5}, 0.0, 16.0},
          {"y_blue1", ParameterType::kNumber, false, ParameterValue{0.5}, 0.0, 16.0},
          {"y_blue2", ParameterType::kNumber, false, ParameterValue{0.5}, 0.0, 16.0},
          {"y_blue3", ParameterType::kNumber, false, ParameterValue{0.5}, 0.0, 16.0},
          {"y_blue4", ParameterType::kNumber, false, ParameterValue{0.5}, 0.0, 16.0}},
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
        {std::string(kCanvasOperationId),
         "Enlarge canvas",
         kCanvasOperationSchemaVersion,
         {{"working_space", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"algorithm", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"percent_left", ParameterType::kNumber, true, std::nullopt, kCanvasPercentMin,
           kCanvasPercentMax},
          {"percent_right", ParameterType::kNumber, true, std::nullopt, kCanvasPercentMin,
           kCanvasPercentMax},
          {"percent_top", ParameterType::kNumber, true, std::nullopt, kCanvasPercentMin,
           kCanvasPercentMax},
          {"percent_bottom", ParameterType::kNumber, true, std::nullopt, kCanvasPercentMin,
           kCanvasPercentMax},
          {"color", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt}},
         false,
         true},
        {std::string(kColorZonesOperationId),
         "Color Zones",
         kColorZonesOperationSchemaVersion,
         {{"working_space", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"algorithm", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"select_by", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"lightness_curve", ParameterType::kArray, true, std::nullopt, std::nullopt,
           std::nullopt},
          {"lightness_interpolation", ParameterType::kString, true, std::nullopt, std::nullopt,
           std::nullopt},
          {"chroma_curve", ParameterType::kArray, true, std::nullopt, std::nullopt, std::nullopt},
          {"chroma_interpolation", ParameterType::kString, true, std::nullopt, std::nullopt,
           std::nullopt},
          {"hue_curve", ParameterType::kArray, true, std::nullopt, std::nullopt, std::nullopt},
          {"hue_interpolation", ParameterType::kString, true, std::nullopt, std::nullopt,
           std::nullopt},
          {"strength", ParameterType::kNumber, true, std::nullopt, -200.0, 200.0}},
         true,
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
         true,
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
        {std::string(kOutputDitherOperationId),
         "Output dither / posterize",
         kOutputDitherOperationSchemaVersion,
         {{"working_space", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"algorithm", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"method", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"random_damping_db", ParameterType::kNumber, true, std::nullopt, kOutputDitherDampingMin,
           kOutputDitherDampingMax}},
         false,
         true},
        {std::string(kFrameOperationId),
         "Frame / border",
         kFrameOperationSchemaVersion,
         {{"working_space", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"algorithm", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"border_color", ParameterType::kArray, true, std::nullopt, std::nullopt, std::nullopt},
          {"aspect", ParameterType::kNumber, true, std::nullopt, -1.0, 3.0},
          {"orientation", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"size", ParameterType::kNumber, true, std::nullopt, 0.0, 0.5},
          {"position_h", ParameterType::kNumber, true, std::nullopt, 0.0, 1.0},
          {"position_v", ParameterType::kNumber, true, std::nullopt, 0.0, 1.0},
          {"frame_size", ParameterType::kNumber, true, std::nullopt, 0.0, 1.0},
          {"frame_offset", ParameterType::kNumber, true, std::nullopt, 0.0, 1.0},
          {"frame_color", ParameterType::kArray, true, std::nullopt, std::nullopt, std::nullopt},
          {"basis", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt}},
         false,
         true},
        {std::string(kWatermarkOperationId),
         "Text watermark",
         kWatermarkOperationSchemaVersion,
         {{"working_space", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"algorithm", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"text", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"color", ParameterType::kArray, true, std::nullopt, std::nullopt, std::nullopt},
          {"opacity", ParameterType::kNumber, true, std::nullopt, 0.0, 1.0},
          {"scale_percent", ParameterType::kNumber, true, std::nullopt, kWatermarkScaleMin,
           kWatermarkScaleMax},
          {"x_offset", ParameterType::kNumber, true, std::nullopt, -1.0, 1.0},
          {"y_offset", ParameterType::kNumber, true, std::nullopt, -1.0, 1.0},
          {"alignment", ParameterType::kString, true, std::nullopt, std::nullopt, std::nullopt},
          {"rotation_degrees", ParameterType::kNumber, true, std::nullopt, -180.0, 180.0}},
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
