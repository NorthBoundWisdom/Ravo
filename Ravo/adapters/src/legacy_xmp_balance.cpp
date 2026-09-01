#include "ravo/adapters/legacy_xmp.h"
#include "ravo/adapters/crs_xmp.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QXmlStreamReader>

#include <zlib.h>

#include "ravo/recipe/color_checker.h"
#include "ravo/recipe/canvas_frame.h"
#include "ravo/recipe/color_contrast.h"
#include "ravo/recipe/color_correction.h"
#include "ravo/recipe/color_harmonizer.h"
#include "ravo/recipe/color_reconstruction.h"
#include "ravo/recipe/color_zones.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/dehaze.h"
#include "ravo/recipe/monochrome.h"
#include "ravo/recipe/operation.h"
#include "ravo/recipe/output_dither.h"
#include "ravo/recipe/perspective.h"
#include "ravo/recipe/retouch.h"
#include "ravo/recipe/sharpen.h"
#include "ravo/recipe/split_toning.h"
#include "ravo/recipe/velvia.h"

#include "legacy_xmp_internal.h"

namespace ravo::legacy_xmp_internal
{

[[nodiscard]] Result<LegacyExposureParams>
decode_legacy_exposure_parameters(const std::string &version, const std::string_view encoded)
{
    std::size_t expected_size = 0;
    if (version == "5")
    {
        expected_size = 20U;
    }
    else if (version == "6")
    {
        expected_size = 24U;
    }
    else if (version == "7")
    {
        expected_size = 28U;
    }
    else
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy exposure module version is not supported",
                          {{"legacy_operation", "exposure"},
                           {"legacy_version", version},
                           {"reason", "unsupported_legacy_exposure_version"}});
    }

    auto decoded = decode_legacy_parameter_blob(encoded, expected_size, "exposure");
    if (!decoded)
    {
        auto error = decoded.error();
        error.context.emplace("legacy_version", version);
        error.context.emplace("reason", "invalid_legacy_exposure_parameters");
        return error;
    }
    const auto mode = read_i32(decoded.value(), 0U);
    const double black = read_f32(decoded.value(), 4U);
    const double exposure = read_f32(decoded.value(), 8U);
    const double percentile = read_f32(decoded.value(), 12U);
    const double target = read_f32(decoded.value(), 16U);
    const auto exposure_bias = version == "5" ? 0 : read_i32(decoded.value(), 20U);
    const auto highlight = version == "7" ? read_i32(decoded.value(), 24U) : 0;
    if ((mode != 0 && mode != 1) || (exposure_bias != 0 && exposure_bias != 1) ||
        (highlight != 0 && highlight != 1))
    {
        return make_error(ErrorCode::kValidation, "Legacy exposure enum or flag is invalid",
                          {{"legacy_operation", "exposure"},
                           {"legacy_version", version},
                           {"reason", "invalid_legacy_exposure_parameters"}});
    }
    struct BoundedField
    {
        double value;
        double minimum;
        double maximum;
        std::string_view parameter;
    };
    const std::array bounded{BoundedField{black, kExposureBlackMin, kExposureBlackMax, "black"},
                             BoundedField{exposure, kExposureEvMin, kExposureEvMax, "exposure_ev"},
                             BoundedField{percentile, kExposureDeflickerPercentileMin,
                                          kExposureDeflickerPercentileMax, "deflicker_percentile"},
                             BoundedField{target, kExposureDeflickerTargetEvMin,
                                          kExposureDeflickerTargetEvMax, "deflicker_target_ev"}};
    for (const auto &[value, minimum, maximum, parameter] : bounded)
    {
        if (!std::isfinite(value) || value < minimum || value > maximum)
        {
            return make_error(ErrorCode::kValidation,
                              "Legacy exposure parameter is outside its frozen range",
                              {{"legacy_operation", "exposure"},
                               {"legacy_version", version},
                               {"parameter", std::string(parameter)},
                               {"reason", "invalid_legacy_exposure_parameters"}});
        }
    }

    LegacyExposureParams result;
    result.params.mode =
        mode == 0 ? std::string(kExposureModeManual) : std::string(kExposureModeDeflicker);
    result.params.black = black;
    result.params.exposure_ev = exposure;
    result.params.deflicker_percentile = percentile;
    result.params.deflicker_target_ev = target;
    result.params.compensate_exposure_bias = exposure_bias != 0;
    result.params.compensate_highlight_preservation = highlight != 0;
    auto valid = validate_exposure_parameters(exposure_to_parameters(result.params));
    if (!valid)
    {
        auto error = valid.error();
        error.context.emplace("legacy_operation", "exposure");
        error.context.emplace("legacy_version", version);
        error.context.emplace("reason", "invalid_legacy_exposure_parameters");
        return error;
    }
    return result;
}

[[nodiscard]] Result<ColorBalanceParams>
decode_legacy_color_balance_parameters(const std::string &version, const std::string_view encoded)
{
    if (version != "3" && version != "4")
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Color Balance module version is not supported",
                          {{"legacy_operation", "colorbalance"},
                           {"legacy_version", version},
                           {"reason", "unsupported_legacy_colorbalance_version"}});
    }
    auto decoded = decode_legacy_parameter_blob(encoded, 68U, "colorbalance");
    if (!decoded)
    {
        auto error = decoded.error();
        error.context.emplace("legacy_version", version);
        error.context.emplace("reason", "invalid_legacy_colorbalance_parameters");
        return error;
    }
    const std::int32_t mode = read_i32(decoded.value(), 0U);
    if (mode != 0 && mode != 1)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Color Balance mode is not part of the frozen v4 owner",
                          {{"legacy_operation", "colorbalance"},
                           {"legacy_version", version},
                           {"legacy_mode", std::to_string(mode)},
                           {"reason", "unsupported_legacy_colorbalance_mode"}});
    }
    ColorBalanceParams params;
    params.mode = mode == 0 ? std::string(kColorBalanceModeLiftGammaGain) :
                              std::string(kColorBalanceModeSlopeOffsetPower);
    std::size_t offset = 4U;
    for (auto *values : {&params.lift, &params.gamma, &params.gain})
    {
        for (double &value : *values)
        {
            value = read_f32(decoded.value(), offset);
            offset += sizeof(float);
        }
    }
    params.input_saturation = read_f32(decoded.value(), offset);
    offset += sizeof(float);
    params.contrast = read_f32(decoded.value(), offset);
    offset += sizeof(float);
    params.grey_fulcrum_percent = read_f32(decoded.value(), offset);
    offset += sizeof(float);
    params.output_saturation = read_f32(decoded.value(), offset);

    auto valid = validate_color_balance_parameters(color_balance_to_parameters(params));
    if (!valid)
    {
        auto error = valid.error();
        error.context.emplace("legacy_operation", "colorbalance");
        error.context.emplace("legacy_version", version);
        error.context.emplace("reason", "invalid_legacy_colorbalance_parameters");
        return error;
    }
    return params;
}

[[nodiscard]] bool is_allowed_color_balance_attribute(const QStringView name) noexcept
{
    return name == u"num" || name == u"operation" || name == u"enabled" || name == u"modversion" ||
           name == u"params" || name == u"multi_name" || name == u"multi_priority" ||
           name == u"multi_name_hand_edited" || name == u"blendop_version" ||
           name == u"blendop_params";
}

[[nodiscard]] Result<OperationInstance>
map_color_balance_candidate(const LegacyColorBalanceCandidate &candidate)
{
    for (const auto &attribute : candidate.attributes)
    {
        const auto name = attribute.name();
        if (name.contains(u"mask"))
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy Color Balance mask has no canonical graph mapping",
                              {{"attribute", utf8(name)},
                               {"legacy_operation", "colorbalance"},
                               {"reason", "unsupported_legacy_colorbalance_mask"}});
        }
        if (!is_allowed_color_balance_attribute(name) ||
            attribute.namespaceUri() != u"http://darktable.sf.net/")
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy Color Balance contains unproven history state",
                              {{"attribute", utf8(name)},
                               {"legacy_operation", "colorbalance"},
                               {"reason", "unsupported_legacy_colorbalance_attribute"}});
        }
    }
    const auto version = required_attribute(candidate.attributes, u"modversion", "colorbalance");
    const auto enabled = required_attribute(candidate.attributes, u"enabled", "colorbalance");
    const auto encoded = required_attribute(candidate.attributes, u"params", "colorbalance");
    const auto blend_version =
        required_attribute(candidate.attributes, u"blendop_version", "colorbalance");
    const auto blend_parameters =
        required_attribute(candidate.attributes, u"blendop_params", "colorbalance");
    if (!version || !enabled || !encoded || !blend_version || !blend_parameters)
    {
        return !version       ? version.error() :
               !enabled       ? enabled.error() :
               !encoded       ? encoded.error() :
               !blend_version ? blend_version.error() :
                                blend_parameters.error();
    }
    if (enabled.value() != "0" && enabled.value() != "1")
    {
        return make_error(ErrorCode::kValidation, "Legacy Color Balance enabled flag is invalid",
                          {{"legacy_operation", "colorbalance"},
                           {"reason", "invalid_legacy_colorbalance_parameters"}});
    }
    if (std::find(kFrozenColorBalanceParametricBlends.begin(),
                  kFrozenColorBalanceParametricBlends.end(),
                  blend_parameters.value()) != kFrozenColorBalanceParametricBlends.end())
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Color Balance parametric mask has no canonical graph mapping",
                          {{"legacy_operation", "colorbalance"},
                           {"reason", "unsupported_legacy_colorbalance_mask"}});
    }
    if (blend_version.value() != "9" || blend_parameters.value() != kDefaultBlendParameters)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Color Balance blend is not the frozen unmasked default",
                          {{"legacy_blend_version", blend_version.value()},
                           {"legacy_operation", "colorbalance"},
                           {"reason", "unsupported_legacy_colorbalance_blend"}});
    }
    auto decoded = decode_legacy_color_balance_parameters(version.value(), encoded.value());
    if (!decoded)
    {
        return decoded.error();
    }
    return OperationInstance{std::string(kColorBalanceOperationId),
                             kColorBalanceOperationSchemaVersion,
                             "legacy-colorbalance-" + std::to_string(candidate.history_position),
                             enabled.value() == "1",
                             color_balance_to_parameters(decoded.value()),
                             std::nullopt};
}

[[nodiscard]] bool is_allowed_exposure_attribute(const QStringView name) noexcept
{
    return name == u"num" || name == u"operation" || name == u"enabled" || name == u"modversion" ||
           name == u"params" || name == u"multi_name" || name == u"multi_priority" ||
           name == u"multi_name_hand_edited" || name == u"blendop_version" ||
           name == u"blendop_params";
}

[[nodiscard]] Result<OperationInstance>
map_exposure_candidate(const LegacyExposureCandidate &candidate)
{
    for (const auto &attribute : candidate.attributes)
    {
        const auto name = attribute.name();
        if (name.contains(u"mask"))
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy exposure mask state has no canonical graph mapping",
                              {{"attribute", utf8(name)},
                               {"legacy_operation", "exposure"},
                               {"reason", "unsupported_legacy_exposure_mask"}});
        }
        if (!is_allowed_exposure_attribute(name) ||
            attribute.namespaceUri() != u"http://darktable.sf.net/")
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy exposure contains unproven history state",
                              {{"attribute", utf8(name)},
                               {"legacy_operation", "exposure"},
                               {"reason", "unsupported_legacy_exposure_attribute"}});
        }
    }

    const auto version = required_attribute(candidate.attributes, u"modversion", "exposure");
    const auto enabled = required_attribute(candidate.attributes, u"enabled", "exposure");
    const auto encoded = required_attribute(candidate.attributes, u"params", "exposure");
    const auto blend_version =
        required_attribute(candidate.attributes, u"blendop_version", "exposure");
    const auto blend_parameters =
        required_attribute(candidate.attributes, u"blendop_params", "exposure");
    if (!version || !enabled || !encoded || !blend_version || !blend_parameters)
    {
        return !version       ? version.error() :
               !enabled       ? enabled.error() :
               !encoded       ? encoded.error() :
               !blend_version ? blend_version.error() :
                                blend_parameters.error();
    }
    if (enabled.value() != "0" && enabled.value() != "1")
    {
        return make_error(
            ErrorCode::kValidation, "Legacy exposure enabled flag is invalid",
            {{"legacy_operation", "exposure"}, {"reason", "invalid_legacy_exposure_parameters"}});
    }
    const bool frozen_blend =
        std::any_of(kLegacyExposureBlendTuples.begin(), kLegacyExposureBlendTuples.end(),
                    [&](const LegacyGammaBlendTuple &frozen)
                    {
                        return frozen.version == blend_version.value() &&
                               frozen.parameters == blend_parameters.value();
                    });
    if (!frozen_blend)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy exposure blend state is not a frozen unmasked default",
                          {{"legacy_blend_version", blend_version.value()},
                           {"legacy_operation", "exposure"},
                           {"reason", "unsupported_legacy_exposure_blend"}});
    }
    auto decoded = decode_legacy_exposure_parameters(version.value(), encoded.value());
    if (!decoded)
    {
        return decoded.error();
    }
    return OperationInstance{std::string(kExposureOperationId),
                             kExposureOperationSchemaVersion,
                             "legacy-exposure-" + std::to_string(candidate.history_position),
                             enabled.value() == "1",
                             exposure_to_parameters(decoded.value().params),
                             std::nullopt};
}

[[nodiscard]] bool is_allowed_gamma_attribute(const QStringView name) noexcept
{
    return name == u"num" || name == u"operation" || name == u"enabled" || name == u"modversion" ||
           name == u"params" || name == u"multi_name" || name == u"multi_priority" ||
           name == u"multi_name_hand_edited" || name == u"blendop_version" ||
           name == u"blendop_params";
}

[[nodiscard]] Result<void> absorb_legacy_gamma(const QXmlStreamAttributes &attributes)
{
    for (const auto &attribute : attributes)
    {
        const auto name = attribute.name();
        if (name.contains(u"mask"))
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy display encoding mask state is unsupported",
                              {{"attribute", utf8(name)},
                               {"legacy_operation", "gamma"},
                               {"reason", "unsupported_legacy_gamma_mask"}});
        }
        if (!is_allowed_gamma_attribute(name) ||
            attribute.namespaceUri() != u"http://darktable.sf.net/")
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy display encoding contains unproven history state",
                              {{"attribute", utf8(name)},
                               {"legacy_operation", "gamma"},
                               {"reason", "unsupported_legacy_gamma_attribute"}});
        }
    }

    const auto version = attribute_value(attributes, u"modversion");
    if (!version || *version != "1")
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy display encoding module version is unsupported",
                          {{"legacy_operation", "gamma"},
                           {"legacy_version", version.value_or("<missing>")},
                           {"reason", "unsupported_legacy_gamma_version"}});
    }
    const auto enabled = attribute_value(attributes, u"enabled");
    if (!enabled || *enabled != "1")
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy display encoding boundary must be enabled",
                          {{"legacy_enabled", enabled.value_or("<missing>")},
                           {"legacy_operation", "gamma"},
                           {"reason", "unsupported_legacy_gamma_disabled"}});
    }
    const auto parameters = attribute_value(attributes, u"params");
    if (!parameters || *parameters != "0000000000000000")
    {
        return make_error(
            ErrorCode::kUnsupported,
            "Legacy display encoding parameters differ from the frozen boundary",
            {{"legacy_operation", "gamma"}, {"reason", "unsupported_legacy_gamma_parameters"}});
    }

    const auto blend_version = attribute_value(attributes, u"blendop_version");
    const auto blend_parameters = attribute_value(attributes, u"blendop_params");
    const bool frozen_blend =
        blend_version && blend_parameters &&
        std::any_of(kLegacyGammaBlendTuples.begin(), kLegacyGammaBlendTuples.end(),
                    [&](const LegacyGammaBlendTuple &candidate)
                    {
                        return candidate.version == *blend_version &&
                               candidate.parameters == *blend_parameters;
                    });
    if (!frozen_blend)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy display encoding blend state differs from every frozen default",
                          {{"legacy_blend_version", blend_version.value_or("<missing>")},
                           {"legacy_operation", "gamma"},
                           {"reason", "unsupported_legacy_gamma_blend"}});
    }

    const auto multi_priority = attribute_value(attributes, u"multi_priority");
    const auto multi_name = attribute_value(attributes, u"multi_name");
    const auto multi_name_hand_edited = attribute_value(attributes, u"multi_name_hand_edited");
    if (!multi_priority || *multi_priority != "0" || !multi_name || !multi_name->empty() ||
        (multi_name_hand_edited && *multi_name_hand_edited != "0"))
    {
        return make_error(
            ErrorCode::kUnsupported,
            "Legacy display encoding instance state is not the frozen singleton",
            {{"legacy_operation", "gamma"}, {"reason", "unsupported_legacy_gamma_multi_state"}});
    }
    return {};
}

[[nodiscard]] bool is_frozen_builtin_raw_blend(const std::string_view operation,
                                               const std::string_view blend_version,
                                               const std::string_view blend) noexcept
{
    if (blend_version == "9" && blend == kDefaultBlendParameters)
    {
        return true;
    }
    if (blend_version != "10")
    {
        return false;
    }
    if ((operation == "rawprepare" || operation == "temperature" || operation == "demosaic") &&
        blend == kLegacyFlipBlendGz14)
    {
        return true;
    }
    return operation == "highlights" && blend == kLegacyRawDenoiseBlendGz13;
}

[[nodiscard]] bool is_allowed_builtin_raw_attribute(const QStringView name) noexcept
{
    return name == u"num" || name == u"operation" || name == u"enabled" || name == u"modversion" ||
           name == u"params" || name == u"multi_name" || name == u"multi_priority" ||
           name == u"multi_name_hand_edited" || name == u"blendop_version" ||
           name == u"blendop_params";
}

[[nodiscard]] Result<bool> absorb_builtin_raw_operation(const std::string_view operation,
                                                        const QXmlStreamAttributes &attributes)
{
    const auto contract = std::find_if(kBuiltinRawOperations.begin(), kBuiltinRawOperations.end(),
                                       [operation](const BuiltinRawOperation &candidate)
                                       { return candidate.id == operation; });
    if (contract == kBuiltinRawOperations.end())
    {
        return false;
    }

    for (const auto &attribute : attributes)
    {
        const auto name = attribute.name();
        if (name.contains(u"mask") || !is_allowed_builtin_raw_attribute(name) ||
            attribute.namespaceUri() != u"http://darktable.sf.net/")
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy built-in RAW operation has unproven presentation state",
                              {{"attribute", utf8(name)},
                               {"legacy_operation", std::string(operation)},
                               {"reason", "unsupported_legacy_builtin_presentation"}});
        }
    }

    const auto version = required_attribute(attributes, u"modversion", operation);
    const auto enabled = required_attribute(attributes, u"enabled", operation);
    const auto parameters = required_attribute(attributes, u"params", operation);
    const auto blend = required_attribute(attributes, u"blendop_params", operation);
    const auto blend_version = required_attribute(attributes, u"blendop_version", operation);
    const auto priority = required_attribute(attributes, u"multi_priority", operation);
    const auto name = attribute_value(attributes, u"multi_name");
    if (!version || !enabled || !parameters || !blend || !blend_version || !priority || !name)
    {
        return !version       ? version.error() :
               !enabled       ? enabled.error() :
               !parameters    ? parameters.error() :
               !blend         ? blend.error() :
               !blend_version ? blend_version.error() :
               !priority      ? priority.error() :
                                make_error(ErrorCode::kUnsupported,
                                           "Legacy built-in RAW singleton name is missing",
                                           {{"attribute", "multi_name"},
                                            {"legacy_operation", std::string(operation)},
                                            {"reason", "unsupported_legacy_builtin_presentation"}});
    }
    const auto hand_edited = attribute_value(attributes, u"multi_name_hand_edited");
    if (version.value() != contract->version || enabled.value() != "1" ||
        parameters.value() != contract->parameters || priority.value() != "0" || !name->empty() ||
        (hand_edited && *hand_edited != "0") ||
        !is_frozen_builtin_raw_blend(operation, blend_version.value(), blend.value()))
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy built-in RAW operation differs from the frozen nop contract",
                          {{"legacy_operation", std::string(operation)},
                           {"reason", "unsupported_legacy_builtin_parameters"}});
    }
    return true;
}

} // namespace ravo::legacy_xmp_internal
