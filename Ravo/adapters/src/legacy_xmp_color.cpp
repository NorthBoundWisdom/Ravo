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

[[nodiscard]] Result<ColorCheckerParams>
decode_legacy_color_checker_parameters(const std::string &version, const std::string_view encoded)
{
    if (version == "1")
    {
        constexpr std::size_t count = kColorCheckerDefaultPatchCount;
        auto decoded =
            decode_legacy_parameter_blob(encoded, count * 3U * sizeof(float), "colorchecker");
        if (!decoded)
        {
            auto error = decoded.error();
            error.context.emplace("legacy_version", version);
            error.context.emplace("reason", "invalid_legacy_colorchecker_parameters");
            return error;
        }
        std::vector<ColorCheckerPatch> patches;
        patches.reserve(count);
        for (std::size_t patch = 0U; patch < count; ++patch)
        {
            std::array<double, 3> target{};
            for (std::size_t channel = 0U; channel < 3U; ++channel)
            {
                const float value =
                    read_f32(decoded.value(), (channel * count + patch) * sizeof(float));
                if (!std::isfinite(value))
                {
                    return make_error(
                        ErrorCode::kValidation,
                        "Legacy color checker v1 target contains a non-finite component",
                        {{"legacy_operation", "colorchecker"},
                         {"legacy_version", version},
                         {"patch_index", std::to_string(patch)},
                         {"reason", "invalid_legacy_colorchecker_parameters"}});
                }
                target[channel] = value;
            }
            patches.push_back(
                {{{kLegacyColorCheckerV1Sources[patch][0], kLegacyColorCheckerV1Sources[patch][1],
                   kLegacyColorCheckerV1Sources[patch][2]}},
                 target});
        }
        return ColorCheckerParams{std::move(patches)};
    }
    if (version != "2")
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy color checker module version is not supported",
                          {{"legacy_operation", "colorchecker"},
                           {"legacy_version", version},
                           {"reason", "unsupported_legacy_colorchecker_version"}});
    }

    constexpr std::size_t plane_count = 6U;
    constexpr std::size_t stride = kColorCheckerMaxPatchCount;
    constexpr std::size_t payload_size =
        plane_count * stride * sizeof(float) + sizeof(std::int32_t);
    auto decoded = decode_legacy_parameter_blob(encoded, payload_size, "colorchecker");
    if (!decoded)
    {
        auto error = decoded.error();
        error.context.emplace("legacy_version", version);
        error.context.emplace("reason", "invalid_legacy_colorchecker_parameters");
        return error;
    }
    const std::int32_t signed_count =
        read_i32(decoded.value(), payload_size - sizeof(std::int32_t));
    if (signed_count < 0 || signed_count > static_cast<std::int32_t>(kColorCheckerMaxPatchCount))
    {
        return make_error(ErrorCode::kValidation,
                          "Legacy color checker patch count is outside 0..49",
                          {{"legacy_operation", "colorchecker"},
                           {"legacy_version", version},
                           {"reason", "invalid_legacy_colorchecker_parameters"}});
    }
    const std::size_t count = static_cast<std::size_t>(signed_count);
    std::vector<ColorCheckerPatch> patches;
    patches.reserve(count);
    for (std::size_t patch = 0U; patch < count; ++patch)
    {
        ColorCheckerPatch value;
        for (std::size_t channel = 0U; channel < 3U; ++channel)
        {
            const float source =
                read_f32(decoded.value(), (channel * stride + patch) * sizeof(float));
            const float target =
                read_f32(decoded.value(), ((channel + 3U) * stride + patch) * sizeof(float));
            if (!std::isfinite(source) || !std::isfinite(target))
            {
                return make_error(ErrorCode::kValidation,
                                  "Legacy color checker patch contains a non-finite component",
                                  {{"legacy_operation", "colorchecker"},
                                   {"legacy_version", version},
                                   {"patch_index", std::to_string(patch)},
                                   {"reason", "invalid_legacy_colorchecker_parameters"}});
            }
            value.source_lab[channel] = source;
            value.target_lab[channel] = target;
        }
        patches.push_back(value);
    }
    ColorCheckerParams params{std::move(patches)};
    auto canonical = color_checker_to_parameters(params);
    if (!canonical)
    {
        auto error = canonical.error();
        error.context.emplace("legacy_operation", "colorchecker");
        error.context.emplace("legacy_version", version);
        error.context.insert_or_assign("reason", "invalid_legacy_colorchecker_parameters");
        return error;
    }
    return params;
}

[[nodiscard]] Result<ColorCorrectionParams>
decode_legacy_color_correction_parameters(const std::string &version,
                                          const std::string_view encoded)
{
    if (version != "1")
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Color Correction module version is not supported",
                          {{"legacy_operation", "colorcorrection"},
                           {"legacy_version", version},
                           {"reason", "unsupported_legacy_colorcorrection_version"}});
    }
    auto decoded = decode_legacy_parameter_blob(encoded, 5U * sizeof(float), "colorcorrection");
    if (!decoded)
    {
        auto error = decoded.error();
        error.context.emplace("legacy_version", version);
        error.context.emplace("reason", "invalid_legacy_colorcorrection_parameters");
        return error;
    }
    ColorCorrectionParams params;
    params.highlight_a = read_f32(decoded.value(), 0U * sizeof(float));
    params.highlight_b = read_f32(decoded.value(), 1U * sizeof(float));
    params.shadow_a = read_f32(decoded.value(), 2U * sizeof(float));
    params.shadow_b = read_f32(decoded.value(), 3U * sizeof(float));
    params.saturation = read_f32(decoded.value(), 4U * sizeof(float));
    auto canonical = color_correction_to_parameters(params);
    if (!canonical)
    {
        auto error = canonical.error();
        error.context.emplace("legacy_operation", "colorcorrection");
        error.context.emplace("legacy_version", version);
        error.context.insert_or_assign("reason", "invalid_legacy_colorcorrection_parameters");
        return error;
    }
    return params;
}

[[nodiscard]] Result<ColorContrastParams>
decode_legacy_color_contrast_parameters(const std::string &version, const std::string_view encoded)
{
    std::size_t expected_size = 0U;
    if (version == "1")
    {
        expected_size = 4U * sizeof(float);
    }
    else if (version == "2")
    {
        expected_size = 4U * sizeof(float) + sizeof(std::int32_t);
    }
    else
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Color Contrast module version is not supported",
                          {{"legacy_operation", "colorcontrast"},
                           {"legacy_version", version},
                           {"reason", "unsupported_legacy_colorcontrast_version"}});
    }
    auto decoded = decode_legacy_parameter_blob(encoded, expected_size, "colorcontrast");
    if (!decoded)
    {
        auto error = decoded.error();
        error.context.emplace("legacy_version", version);
        error.context.emplace("reason", "invalid_legacy_colorcontrast_parameters");
        return error;
    }
    ColorContrastParams params;
    params.a_steepness = read_f32(decoded.value(), 0U * sizeof(float));
    params.a_offset = read_f32(decoded.value(), 1U * sizeof(float));
    params.b_steepness = read_f32(decoded.value(), 2U * sizeof(float));
    params.b_offset = read_f32(decoded.value(), 3U * sizeof(float));
    params.unbound = false;
    if (version == "2")
    {
        const std::int32_t unbound = read_i32(decoded.value(), 4U * sizeof(float));
        if (unbound != 0 && unbound != 1)
        {
            return make_error(ErrorCode::kValidation,
                              "Legacy Color Contrast unbound flag is invalid",
                              {{"legacy_operation", "colorcontrast"},
                               {"legacy_version", version},
                               {"reason", "invalid_legacy_colorcontrast_parameters"}});
        }
        params.unbound = unbound == 1;
    }
    auto canonical = color_contrast_to_parameters(params);
    if (!canonical)
    {
        auto error = canonical.error();
        error.context.emplace("legacy_operation", "colorcontrast");
        error.context.emplace("legacy_version", version);
        error.context.insert_or_assign("reason", "invalid_legacy_colorcontrast_parameters");
        return error;
    }
    return params;
}

[[nodiscard]] Result<ColorHarmonizerParams>
decode_legacy_color_harmonizer_parameters(const std::string &version,
                                          const std::string_view encoded)
{
    if (version != "1")
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Color Harmonizer module version is not supported",
                          {{"legacy_operation", "colorharmonizer"},
                           {"legacy_version", version},
                           {"reason", "unsupported_legacy_colorharmonizer_version"}});
    }
    auto decoded = decode_legacy_parameter_blob(encoded, 60U, "colorharmonizer");
    if (!decoded)
    {
        auto error = decoded.error();
        error.context.emplace("legacy_version", version);
        error.context.emplace("reason", "invalid_legacy_colorharmonizer_parameters");
        return error;
    }
    const auto &bytes = decoded.value();
    const std::int32_t rule_bits = read_i32(bytes, 0U);
    auto rule = color_harmonizer_rule_from_index(rule_bits);
    if (!rule)
    {
        return make_error(ErrorCode::kValidation,
                          "Legacy Color Harmonizer rule is outside the frozen enumeration",
                          {{"legacy_operation", "colorharmonizer"},
                           {"legacy_version", version},
                           {"reason", "invalid_legacy_colorharmonizer_parameters"}});
    }
    ColorHarmonizerParams params;
    params.rule = rule.value();
    std::size_t offset = sizeof(std::int32_t);
    const auto assign_finite = [&](double &target, const std::string_view field) -> Result<void>
    {
        const float value = read_f32(bytes, offset);
        offset += sizeof(float);
        if (!std::isfinite(value))
        {
            return make_error(ErrorCode::kValidation,
                              "Legacy Color Harmonizer parameter is not a finite float",
                              {{"legacy_operation", "colorharmonizer"},
                               {"legacy_version", version},
                               {"parameter", std::string(field)},
                               {"reason", "invalid_legacy_colorharmonizer_parameters"}});
        }
        target = value;
        return {};
    };
    if (auto assigned = assign_finite(params.anchor_hue, "anchor_hue"); !assigned)
    {
        return assigned.error();
    }
    if (auto assigned = assign_finite(params.pull_strength, "pull_strength"); !assigned)
    {
        return assigned.error();
    }
    if (auto assigned = assign_finite(params.neutral_protection, "neutral_protection"); !assigned)
    {
        return assigned.error();
    }
    if (auto assigned = assign_finite(params.pull_width, "pull_width"); !assigned)
    {
        return assigned.error();
    }
    for (std::size_t index = 0U; index < params.custom_hue.size(); ++index)
    {
        if (auto assigned = assign_finite(params.custom_hue[index], "custom_hue"); !assigned)
        {
            return assigned.error();
        }
    }
    const std::int32_t node_count = read_i32(bytes, offset);
    offset += sizeof(std::int32_t);
    if (node_count < kColorHarmonizerCustomNodesMin || node_count > kColorHarmonizerCustomNodesMax)
    {
        return make_error(ErrorCode::kValidation,
                          "Legacy Color Harmonizer custom-node count is outside 2..4",
                          {{"legacy_operation", "colorharmonizer"},
                           {"legacy_version", version},
                           {"reason", "invalid_legacy_colorharmonizer_parameters"}});
    }
    params.num_custom_nodes = node_count;
    for (std::size_t index = 0U; index < params.node_saturation.size(); ++index)
    {
        if (auto assigned = assign_finite(params.node_saturation[index], "node_saturation");
            !assigned)
        {
            return assigned.error();
        }
    }
    if (auto assigned = assign_finite(params.smoothing, "smoothing"); !assigned)
    {
        return assigned.error();
    }
    if (params.smoothing != 0.0)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Color Harmonizer smoothing is outside the frozen 0176 evidence",
                          {{"legacy_operation", "colorharmonizer"},
                           {"legacy_version", version},
                           {"parameter", "smoothing"},
                           {"reason", "unsupported_legacy_colorharmonizer_unevidenced_smoothing"}});
    }
    auto canonical = color_harmonizer_to_parameters(params);
    if (!canonical)
    {
        auto error = canonical.error();
        error.context.emplace("legacy_operation", "colorharmonizer");
        error.context.emplace("legacy_version", version);
        error.context.insert_or_assign("reason", "invalid_legacy_colorharmonizer_parameters");
        return error;
    }
    return params;
}

[[nodiscard]] Result<ColorReconstructionParams>
decode_legacy_color_reconstruction_parameters(const std::string &version,
                                              const std::string_view encoded)
{
    if (version != "3")
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Color Reconstruction module version is not supported",
                          {{"legacy_operation", "colorreconstruct"},
                           {"legacy_version", version},
                           {"reason", "unsupported_legacy_colorreconstruct_version"}});
    }
    auto decoded = decode_legacy_parameter_blob(encoded, 4U * sizeof(float) + sizeof(std::int32_t),
                                                "colorreconstruct");
    if (!decoded)
    {
        auto error = decoded.error();
        error.context.emplace("legacy_version", version);
        error.context.emplace("reason", "invalid_legacy_colorreconstruct_parameters");
        return error;
    }
    ColorReconstructionParams params;
    params.threshold = read_f32(decoded.value(), 0U * sizeof(float));
    params.spatial = read_f32(decoded.value(), 1U * sizeof(float));
    params.range = read_f32(decoded.value(), 2U * sizeof(float));
    params.hue = read_f32(decoded.value(), 3U * sizeof(float));
    const std::int32_t precedence = read_i32(decoded.value(), 4U * sizeof(float));
    if (precedence < 0 || precedence > 2)
    {
        return make_error(ErrorCode::kValidation,
                          "Legacy Color Reconstruction precedence is invalid",
                          {{"legacy_operation", "colorreconstruct"},
                           {"legacy_version", version},
                           {"reason", "invalid_legacy_colorreconstruct_parameters"}});
    }
    params.precedence = static_cast<ColorReconstructionPrecedence>(precedence);
    auto canonical = color_reconstruction_to_parameters(params);
    if (!canonical)
    {
        auto error = canonical.error();
        error.context.emplace("legacy_operation", "colorreconstruct");
        error.context.emplace("legacy_version", version);
        error.context.insert_or_assign("reason", "invalid_legacy_colorreconstruct_parameters");
        return error;
    }
    return params;
}

[[nodiscard]] bool is_allowed_color_checker_attribute(const QStringView name) noexcept
{
    return name == u"num" || name == u"operation" || name == u"enabled" || name == u"modversion" ||
           name == u"params" || name == u"multi_name" || name == u"multi_priority" ||
           name == u"multi_name_hand_edited" || name == u"blendop_version" ||
           name == u"blendop_params";
}

[[nodiscard]] Result<OperationInstance>
map_color_checker_candidate(const LegacyColorCheckerCandidate &candidate)
{
    for (const auto &attribute : candidate.attributes)
    {
        const auto name = attribute.name();
        if (name.contains(u"mask"))
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy color checker mask has no canonical graph mapping",
                              {{"attribute", utf8(name)},
                               {"legacy_operation", "colorchecker"},
                               {"reason", "unsupported_legacy_colorchecker_mask"}});
        }
        if (!is_allowed_color_checker_attribute(name) ||
            attribute.namespaceUri() != u"http://darktable.sf.net/")
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy color checker contains unproven history state",
                              {{"attribute", utf8(name)},
                               {"legacy_operation", "colorchecker"},
                               {"reason", "unsupported_legacy_colorchecker_attribute"}});
        }
    }
    const auto version = required_attribute(candidate.attributes, u"modversion", "colorchecker");
    const auto enabled = required_attribute(candidate.attributes, u"enabled", "colorchecker");
    const auto encoded = required_attribute(candidate.attributes, u"params", "colorchecker");
    const auto blend_version =
        required_attribute(candidate.attributes, u"blendop_version", "colorchecker");
    const auto blend_parameters =
        required_attribute(candidate.attributes, u"blendop_params", "colorchecker");
    if (!version || !enabled || !encoded || !blend_version || !blend_parameters)
    {
        return !version       ? version.error() :
               !enabled       ? enabled.error() :
               !encoded       ? encoded.error() :
               !blend_version ? blend_version.error() :
                                blend_parameters.error();
    }
    if (enabled.value() != "1")
    {
        return make_error(
            ErrorCode::kUnsupported,
            "Legacy color checker enabled state is outside the frozen fixture evidence",
            {{"legacy_operation", "colorchecker"},
             {"reason", "unsupported_legacy_colorchecker_enabled_state"}});
    }
    if (blend_version.value() != "11" || blend_parameters.value() != kFrozenColorCheckerBlendV11)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy color checker blend is not the frozen unmasked default",
                          {{"legacy_blend_version", blend_version.value()},
                           {"legacy_operation", "colorchecker"},
                           {"reason", "unsupported_legacy_colorchecker_blend"}});
    }
    auto decoded = decode_legacy_color_checker_parameters(version.value(), encoded.value());
    if (!decoded)
    {
        return decoded.error();
    }
    auto parameters = color_checker_to_parameters(decoded.value());
    if (!parameters)
    {
        return parameters.error();
    }
    return OperationInstance{std::string(kColorCheckerOperationId),
                             kColorCheckerOperationSchemaVersion,
                             "legacy-colorchecker-" + std::to_string(candidate.history_position),
                             true,
                             std::move(parameters).value(),
                             std::nullopt};
}

[[nodiscard]] bool is_allowed_color_correction_attribute(const QStringView name) noexcept
{
    return name == u"num" || name == u"operation" || name == u"enabled" || name == u"modversion" ||
           name == u"params" || name == u"multi_name" || name == u"multi_priority" ||
           name == u"multi_name_hand_edited" || name == u"blendop_version" ||
           name == u"blendop_params";
}

[[nodiscard]] Result<OperationInstance>
map_color_correction_candidate(const LegacyColorCorrectionCandidate &candidate)
{
    for (const auto &attribute : candidate.attributes)
    {
        const auto name = attribute.name();
        if (name.contains(u"mask"))
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy Color Correction mask has no canonical graph mapping",
                              {{"attribute", utf8(name)},
                               {"legacy_operation", "colorcorrection"},
                               {"reason", "unsupported_legacy_colorcorrection_mask"}});
        }
        if (!is_allowed_color_correction_attribute(name) ||
            attribute.namespaceUri() != u"http://darktable.sf.net/")
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy Color Correction contains unproven history state",
                              {{"attribute", utf8(name)},
                               {"legacy_operation", "colorcorrection"},
                               {"reason", "unsupported_legacy_colorcorrection_attribute"}});
        }
    }
    const auto version = required_attribute(candidate.attributes, u"modversion", "colorcorrection");
    const auto enabled = required_attribute(candidate.attributes, u"enabled", "colorcorrection");
    const auto encoded = required_attribute(candidate.attributes, u"params", "colorcorrection");
    const auto blend_version =
        required_attribute(candidate.attributes, u"blendop_version", "colorcorrection");
    const auto blend_parameters =
        required_attribute(candidate.attributes, u"blendop_params", "colorcorrection");
    if (!version || !enabled || !encoded || !blend_version || !blend_parameters)
    {
        return !version       ? version.error() :
               !enabled       ? enabled.error() :
               !encoded       ? encoded.error() :
               !blend_version ? blend_version.error() :
                                blend_parameters.error();
    }
    if (version.value() != "1")
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Color Correction version is outside the frozen evidence",
                          {{"legacy_operation", "colorcorrection"},
                           {"legacy_version", version.value()},
                           {"reason", "unsupported_legacy_colorcorrection_version"}});
    }
    if (enabled.value() != "1")
    {
        return make_error(
            ErrorCode::kUnsupported,
            "Legacy Color Correction enabled state is outside the frozen fixture evidence",
            {{"legacy_operation", "colorcorrection"},
             {"reason", "unsupported_legacy_colorcorrection_enabled_state"}});
    }
    const bool frozen_blend = std::any_of(kFrozenColorCorrectionBlendTuples.begin(),
                                          kFrozenColorCorrectionBlendTuples.end(),
                                          [&](const LegacyGammaBlendTuple &frozen)
                                          {
                                              return frozen.version == blend_version.value() &&
                                                     frozen.parameters == blend_parameters.value();
                                          });
    if (!frozen_blend)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Color Correction blend is not a frozen unmasked default",
                          {{"legacy_blend_version", blend_version.value()},
                           {"legacy_operation", "colorcorrection"},
                           {"reason", "unsupported_legacy_colorcorrection_blend"}});
    }
    auto decoded = decode_legacy_color_correction_parameters(version.value(), encoded.value());
    if (!decoded)
    {
        return decoded.error();
    }
    auto parameters = color_correction_to_parameters(decoded.value());
    if (!parameters)
    {
        return parameters.error();
    }
    return OperationInstance{std::string(kColorCorrectionOperationId),
                             kColorCorrectionOperationSchemaVersion,
                             "legacy-colorcorrection-" + std::to_string(candidate.history_position),
                             true,
                             std::move(parameters).value(),
                             std::nullopt};
}

[[nodiscard]] bool is_allowed_color_contrast_attribute(const QStringView name) noexcept
{
    return name == u"num" || name == u"operation" || name == u"enabled" || name == u"modversion" ||
           name == u"params" || name == u"multi_name" || name == u"multi_priority" ||
           name == u"multi_name_hand_edited" || name == u"blendop_version" ||
           name == u"blendop_params";
}

[[nodiscard]] Result<OperationInstance>
map_color_contrast_candidate(const LegacyColorContrastCandidate &candidate)
{
    for (const auto &attribute : candidate.attributes)
    {
        const auto name = attribute.name();
        if (name.contains(u"mask"))
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy Color Contrast mask has no canonical graph mapping",
                              {{"attribute", utf8(name)},
                               {"legacy_operation", "colorcontrast"},
                               {"reason", "unsupported_legacy_colorcontrast_mask"}});
        }
        if (!is_allowed_color_contrast_attribute(name) ||
            attribute.namespaceUri() != u"http://darktable.sf.net/")
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy Color Contrast contains unproven history state",
                              {{"attribute", utf8(name)},
                               {"legacy_operation", "colorcontrast"},
                               {"reason", "unsupported_legacy_colorcontrast_attribute"}});
        }
    }
    const auto version = required_attribute(candidate.attributes, u"modversion", "colorcontrast");
    const auto enabled = required_attribute(candidate.attributes, u"enabled", "colorcontrast");
    const auto encoded = required_attribute(candidate.attributes, u"params", "colorcontrast");
    const auto blend_version =
        required_attribute(candidate.attributes, u"blendop_version", "colorcontrast");
    const auto blend_parameters =
        required_attribute(candidate.attributes, u"blendop_params", "colorcontrast");
    if (!version || !enabled || !encoded || !blend_version || !blend_parameters)
    {
        return !version       ? version.error() :
               !enabled       ? enabled.error() :
               !encoded       ? encoded.error() :
               !blend_version ? blend_version.error() :
                                blend_parameters.error();
    }
    if (version.value() != "1" && version.value() != "2")
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Color Contrast version is outside the frozen evidence",
                          {{"legacy_operation", "colorcontrast"},
                           {"legacy_version", version.value()},
                           {"reason", "unsupported_legacy_colorcontrast_version"}});
    }
    if (enabled.value() != "1")
    {
        return make_error(
            ErrorCode::kUnsupported,
            "Legacy Color Contrast enabled state is outside the frozen fixture evidence",
            {{"legacy_operation", "colorcontrast"},
             {"reason", "unsupported_legacy_colorcontrast_enabled_state"}});
    }
    if (blend_version.value() != "10" || blend_parameters.value() != kFrozenColorContrastBlendV10)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Color Contrast blend is not the frozen unmasked default",
                          {{"legacy_blend_version", blend_version.value()},
                           {"legacy_operation", "colorcontrast"},
                           {"reason", "unsupported_legacy_colorcontrast_blend"}});
    }
    auto decoded = decode_legacy_color_contrast_parameters(version.value(), encoded.value());
    if (!decoded)
    {
        return decoded.error();
    }
    auto parameters = color_contrast_to_parameters(decoded.value());
    if (!parameters)
    {
        return parameters.error();
    }
    return OperationInstance{std::string(kColorContrastOperationId),
                             kColorContrastOperationSchemaVersion,
                             "legacy-colorcontrast-" + std::to_string(candidate.history_position),
                             true,
                             std::move(parameters).value(),
                             std::nullopt};
}

[[nodiscard]] bool is_allowed_color_harmonizer_attribute(const QStringView name) noexcept
{
    return name == u"num" || name == u"operation" || name == u"enabled" || name == u"modversion" ||
           name == u"params" || name == u"multi_name" || name == u"multi_priority" ||
           name == u"multi_name_hand_edited" || name == u"blendop_version" ||
           name == u"blendop_params";
}

[[nodiscard]] Result<OperationInstance>
map_color_harmonizer_candidate(const LegacyColorHarmonizerCandidate &candidate)
{
    for (const auto &attribute : candidate.attributes)
    {
        const auto name = attribute.name();
        if (name.contains(u"mask"))
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy Color Harmonizer mask has no canonical graph mapping",
                              {{"attribute", utf8(name)},
                               {"legacy_operation", "colorharmonizer"},
                               {"reason", "unsupported_legacy_colorharmonizer_mask"}});
        }
        if (!is_allowed_color_harmonizer_attribute(name) ||
            attribute.namespaceUri() != u"http://darktable.sf.net/")
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy Color Harmonizer contains unproven history state",
                              {{"attribute", utf8(name)},
                               {"legacy_operation", "colorharmonizer"},
                               {"reason", "unsupported_legacy_colorharmonizer_attribute"}});
        }
    }
    const auto version = required_attribute(candidate.attributes, u"modversion", "colorharmonizer");
    const auto enabled = required_attribute(candidate.attributes, u"enabled", "colorharmonizer");
    const auto encoded = required_attribute(candidate.attributes, u"params", "colorharmonizer");
    const auto blend_version =
        required_attribute(candidate.attributes, u"blendop_version", "colorharmonizer");
    const auto blend_parameters =
        required_attribute(candidate.attributes, u"blendop_params", "colorharmonizer");
    if (!version || !enabled || !encoded || !blend_version || !blend_parameters)
    {
        return !version       ? version.error() :
               !enabled       ? enabled.error() :
               !encoded       ? encoded.error() :
               !blend_version ? blend_version.error() :
                                blend_parameters.error();
    }
    if (version.value() != "1")
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Color Harmonizer version is outside the frozen evidence",
                          {{"legacy_operation", "colorharmonizer"},
                           {"legacy_version", version.value()},
                           {"reason", "unsupported_legacy_colorharmonizer_version"}});
    }
    if (enabled.value() != "1")
    {
        return make_error(
            ErrorCode::kUnsupported,
            "Legacy Color Harmonizer enabled state is outside the frozen fixture evidence",
            {{"legacy_operation", "colorharmonizer"},
             {"reason", "unsupported_legacy_colorharmonizer_enabled_state"}});
    }
    if (blend_version.value() != "14" || blend_parameters.value() != kFrozenColorHarmonizerBlendV14)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Color Harmonizer blend is not the frozen unmasked default",
                          {{"legacy_blend_version", blend_version.value()},
                           {"legacy_operation", "colorharmonizer"},
                           {"reason", "unsupported_legacy_colorharmonizer_blend"}});
    }
    auto decoded = decode_legacy_color_harmonizer_parameters(version.value(), encoded.value());
    if (!decoded)
    {
        return decoded.error();
    }
    auto parameters = color_harmonizer_to_parameters(decoded.value());
    if (!parameters)
    {
        return parameters.error();
    }
    return OperationInstance{std::string(kColorHarmonizerOperationId),
                             kColorHarmonizerOperationSchemaVersion,
                             "legacy-colorharmonizer-0",
                             true,
                             std::move(parameters).value(),
                             std::nullopt};
}

[[nodiscard]] bool is_allowed_color_reconstruction_attribute(const QStringView name) noexcept
{
    return name == u"num" || name == u"operation" || name == u"enabled" || name == u"modversion" ||
           name == u"params" || name == u"multi_name" || name == u"multi_priority" ||
           name == u"multi_name_hand_edited" || name == u"blendop_version" ||
           name == u"blendop_params";
}

[[nodiscard]] Result<OperationInstance>
map_color_reconstruction_candidate(const LegacyColorReconstructionCandidate &candidate)
{
    for (const auto &attribute : candidate.attributes)
    {
        const auto name = attribute.name();
        if (name.contains(u"mask"))
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy Color Reconstruction mask has no canonical graph mapping",
                              {{"attribute", utf8(name)},
                               {"legacy_operation", "colorreconstruct"},
                               {"reason", "unsupported_legacy_colorreconstruct_mask"}});
        }
        if (!is_allowed_color_reconstruction_attribute(name) ||
            attribute.namespaceUri() != u"http://darktable.sf.net/")
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy Color Reconstruction contains unproven history state",
                              {{"attribute", utf8(name)},
                               {"legacy_operation", "colorreconstruct"},
                               {"reason", "unsupported_legacy_colorreconstruct_attribute"}});
        }
    }
    const auto version =
        required_attribute(candidate.attributes, u"modversion", "colorreconstruct");
    const auto enabled = required_attribute(candidate.attributes, u"enabled", "colorreconstruct");
    const auto encoded = required_attribute(candidate.attributes, u"params", "colorreconstruct");
    const auto blend_version =
        required_attribute(candidate.attributes, u"blendop_version", "colorreconstruct");
    const auto blend_parameters =
        required_attribute(candidate.attributes, u"blendop_params", "colorreconstruct");
    if (!version || !enabled || !encoded || !blend_version || !blend_parameters)
    {
        return !version       ? version.error() :
               !enabled       ? enabled.error() :
               !encoded       ? encoded.error() :
               !blend_version ? blend_version.error() :
                                blend_parameters.error();
    }
    if (version.value() != "3")
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Color Reconstruction version is outside the frozen evidence",
                          {{"legacy_operation", "colorreconstruct"},
                           {"legacy_version", version.value()},
                           {"reason", "unsupported_legacy_colorreconstruct_version"}});
    }
    if (enabled.value() != "1")
    {
        return make_error(
            ErrorCode::kUnsupported,
            "Legacy Color Reconstruction enabled state is outside the frozen fixture evidence",
            {{"legacy_operation", "colorreconstruct"},
             {"reason", "unsupported_legacy_colorreconstruct_enabled_state"}});
    }
    if (blend_version.value() != "10" ||
        blend_parameters.value() != kFrozenColorReconstructionBlendV10)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Color Reconstruction blend is not the frozen unmasked default",
                          {{"legacy_blend_version", blend_version.value()},
                           {"legacy_operation", "colorreconstruct"},
                           {"reason", "unsupported_legacy_colorreconstruct_blend"}});
    }
    auto decoded = decode_legacy_color_reconstruction_parameters(version.value(), encoded.value());
    if (!decoded)
    {
        return decoded.error();
    }
    auto parameters = color_reconstruction_to_parameters(decoded.value());
    if (!parameters)
    {
        return parameters.error();
    }
    return OperationInstance{std::string(kColorReconstructionOperationId),
                             kColorReconstructionOperationSchemaVersion,
                             "legacy-colorreconstruct-" +
                                 std::to_string(candidate.history_position),
                             true,
                             std::move(parameters).value(),
                             std::nullopt};
}

} // namespace ravo::legacy_xmp_internal
