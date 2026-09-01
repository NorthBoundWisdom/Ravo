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

[[nodiscard]] Result<SharpenParams> decode_legacy_sharpen_parameters(const std::string &version,
                                                                     const std::string_view encoded)
{
    if (version != "1")
    {
        return make_error(ErrorCode::kUnsupported, "Legacy Sharpen module version is not supported",
                          {{"legacy_operation", "sharpen"},
                           {"legacy_version", version},
                           {"reason", "unsupported_legacy_sharpen_version"}});
    }
    auto decoded = decode_legacy_parameter_blob(encoded, 3U * sizeof(float), "sharpen");
    if (!decoded)
    {
        auto error = decoded.error();
        error.context.emplace("legacy_version", version);
        error.context.emplace("reason", "invalid_legacy_sharpen_parameters");
        return error;
    }
    SharpenParams params;
    params.radius = read_f32(decoded.value(), 0U * sizeof(float));
    params.amount = read_f32(decoded.value(), 1U * sizeof(float));
    params.threshold = read_f32(decoded.value(), 2U * sizeof(float));
    auto canonical = sharpen_to_parameters(params);
    if (!canonical)
    {
        auto error = canonical.error();
        error.context.emplace("legacy_operation", "sharpen");
        error.context.emplace("legacy_version", version);
        error.context.insert_or_assign("reason", "invalid_legacy_sharpen_parameters");
        return error;
    }
    return params;
}

[[nodiscard]] Result<DehazeParams> decode_legacy_dehaze_parameters(const std::string &version,
                                                                   const std::string_view encoded)
{
    std::size_t expected_size = 0U;
    if (version == "1")
    {
        expected_size = 2U * sizeof(float);
    }
    else if (version == "2")
    {
        expected_size = 2U * sizeof(float) + sizeof(std::int32_t);
    }
    else
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Haze Removal module version is not supported",
                          {{"legacy_operation", "hazeremoval"},
                           {"legacy_version", version},
                           {"reason", "unsupported_legacy_dehaze_version"}});
    }
    auto decoded = decode_legacy_parameter_blob(encoded, expected_size, "hazeremoval");
    if (!decoded)
    {
        auto error = decoded.error();
        error.context.emplace("legacy_version", version);
        error.context.emplace("reason", "invalid_legacy_dehaze_parameters");
        return error;
    }
    DehazeParams params;
    params.strength = read_f32(decoded.value(), 0U);
    params.distance = read_f32(decoded.value(), sizeof(float));
    params.adaptive = true;
    if (version == "2")
    {
        const std::int32_t adaptive = read_i32(decoded.value(), 2U * sizeof(float));
        if (adaptive != 0 && adaptive != 1)
        {
            return make_error(ErrorCode::kValidation,
                              "Legacy Haze Removal adaptive flag is invalid",
                              {{"legacy_operation", "hazeremoval"},
                               {"legacy_version", version},
                               {"reason", "invalid_legacy_dehaze_parameters"}});
        }
        params.adaptive = adaptive == 1;
    }
    auto canonical = dehaze_to_parameters(params);
    if (!canonical)
    {
        auto error = canonical.error();
        error.context.emplace("legacy_operation", "hazeremoval");
        error.context.emplace("legacy_version", version);
        error.context.insert_or_assign("reason", "invalid_legacy_dehaze_parameters");
        return error;
    }
    return params;
}

[[nodiscard]] Result<OutputDitherParams>
decode_legacy_output_dither_parameters(const std::string &version, const std::string_view encoded)
{
    if (version != "1")
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Output Dither module version is not supported",
                          {{"legacy_operation", "dither"},
                           {"legacy_version", version},
                           {"reason", "unsupported_legacy_dither_version"}});
    }
    auto decoded = decode_legacy_parameter_blob(encoded, 32U, "dither");
    if (!decoded)
    {
        auto error = decoded.error();
        error.context.emplace("legacy_version", version);
        error.context.emplace("reason", "invalid_legacy_dither_parameters");
        return error;
    }
    const std::int32_t method = read_i32(decoded.value(), 0U);
    const std::int32_t palette = read_i32(decoded.value(), 4U);
    const float radius = read_f32(decoded.value(), 8U);
    const std::array<float, 4> range{read_f32(decoded.value(), 12U), read_f32(decoded.value(), 16U),
                                     read_f32(decoded.value(), 20U),
                                     read_f32(decoded.value(), 24U)};
    const float damping = read_f32(decoded.value(), 28U);
    const bool common = palette == 0 && radius == 0.0F;
    const bool frozen_fs =
        method == 1 && common && range == std::array<float, 4>{} && damping == -200.0F;
    const bool frozen_random =
        method == 0 && common && range == std::array<float, 4>{} && damping == -6.91400146484375F;
    const bool frozen_poster =
        method == 0x103 && common && range == std::array<float, 4>{} && damping == -100.0F;
    if (!frozen_fs && !frozen_random && !frozen_poster)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Output Dither parameters are outside the frozen evidence",
                          {{"legacy_method", std::to_string(method)},
                           {"legacy_operation", "dither"},
                           {"reason", "unsupported_legacy_dither_parameters"}});
    }
    OutputDitherParams params;
    params.method = frozen_fs     ? OutputDitherMethod::kFloydSteinberg1BitGray :
                    frozen_random ? OutputDitherMethod::kRandom :
                                    OutputDitherMethod::kPosterize4;
    params.random_damping_db = damping;
    auto canonical = output_dither_to_parameters(params);
    if (!canonical)
    {
        auto error = canonical.error();
        error.context.emplace("legacy_operation", "dither");
        error.context.insert_or_assign("reason", "invalid_legacy_dither_parameters");
        return error;
    }
    return params;
}

[[nodiscard]] Result<CanvasParams> decode_legacy_canvas_parameters(const std::string &version,
                                                                   const std::string_view encoded)
{
    constexpr std::string_view kFrozenCanvas = "0000a04000002041010070410000a04102000000";
    if (version != "1" || encoded != kFrozenCanvas)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Canvas state is outside the frozen evidence",
                          {{"legacy_operation", "enlargecanvas"},
                           {"legacy_version", version},
                           {"reason", "unsupported_legacy_canvas_parameters"}});
    }
    auto decoded = decode_legacy_parameter_blob(encoded, 20U, "enlargecanvas");
    if (!decoded)
        return decoded.error();
    const std::int32_t color = read_i32(decoded.value(), 16U);
    if (color < 0 || color > 4)
        return make_error(ErrorCode::kValidation, "Legacy Canvas color is invalid",
                          {{"reason", "invalid_legacy_canvas_parameters"}});
    CanvasParams params{read_f32(decoded.value(), 0U), read_f32(decoded.value(), 4U),
                        read_f32(decoded.value(), 8U), read_f32(decoded.value(), 12U),
                        static_cast<CanvasColor>(color)};
    auto canonical = canvas_to_parameters(params);
    return canonical ? Result<CanvasParams>{params} : canonical.error();
}

[[nodiscard]] Result<ColorZonesParams>
decode_legacy_color_zones_parameters(const std::string &version, const std::string_view encoded)
{
    constexpr std::string_view frozen =
        "gz08eJxjYgCBBjsgYf9stZ596FsdewYGB3sQn2GQAK3z/2z1FVLtYO4cbO6DuSts/Sk7p8wFdoPNfcxImBEJwwCIDQAtfA+o";
    if (version != "5" || encoded != frozen)
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Color Zones state is outside the frozen evidence",
                          {{"legacy_operation", "colorzones"},
                           {"legacy_version", version},
                           {"reason", "unsupported_legacy_color_zones_parameters"}});
    auto decoded = decode_legacy_parameter_blob(encoded, 520U, "colorzones");
    if (!decoded)
        return decoded.error();
    const std::int32_t select_by = read_i32(decoded.value(), 0U);
    if (select_by < 0 || select_by > 2 || read_i32(decoded.value(), 512U) != 0 ||
        read_i32(decoded.value(), 516U) != 1)
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Color Zones reserved state is unsupported",
                          {{"reason", "unsupported_legacy_color_zones_parameters"}});
    ColorZonesParams params;
    params.select_by = static_cast<ColorZonesChannel>(static_cast<std::uint8_t>(select_by));
    for (std::size_t channel = 0U; channel < params.curves.size(); ++channel)
    {
        const std::int32_t count = read_i32(decoded.value(), 484U + channel * 4U);
        const std::int32_t interpolation = read_i32(decoded.value(), 496U + channel * 4U);
        if (count < 2 || count > static_cast<std::int32_t>(kColorZonesMaximumNodes) ||
            interpolation < 0 || interpolation > 2)
            return make_error(ErrorCode::kUnsupported,
                              "Legacy Color Zones curve layout is unsupported",
                              {{"reason", "unsupported_legacy_color_zones_parameters"}});
        auto &curve = params.curves[channel];
        curve.points.clear();
        curve.points.reserve(static_cast<std::size_t>(count));
        curve.interpolation =
            static_cast<ColorZonesInterpolation>(static_cast<std::uint8_t>(interpolation));
        const std::size_t base = 4U + channel * 160U;
        for (std::int32_t index = 0; index < count; ++index)
        {
            curve.points.push_back(
                {static_cast<double>(
                     read_f32(decoded.value(), base + static_cast<std::size_t>(index) * 8U)),
                 static_cast<double>(
                     read_f32(decoded.value(), base + static_cast<std::size_t>(index) * 8U + 4U))});
        }
    }
    params.strength = static_cast<double>(read_f32(decoded.value(), 508U));
    auto canonical = color_zones_to_parameters(params);
    return canonical ? Result<ColorZonesParams>{std::move(params)} : canonical.error();
}

[[nodiscard]] Result<MonochromeParams>
decode_legacy_monochrome_parameters(const std::string &version, const std::string_view encoded)
{
    constexpr std::string_view frozen = "5acafa4259234ec1000000409a99193f";
    if (version != "2" || encoded != frozen)
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Monochrome state is outside the frozen evidence",
                          {{"legacy_operation", "monochrome"},
                           {"legacy_version", version},
                           {"reason", "unsupported_legacy_monochrome_parameters"}});
    auto decoded = decode_legacy_parameter_blob(encoded, 16U, "monochrome");
    if (!decoded)
        return decoded.error();
    MonochromeParams params;
    params.filter_a = static_cast<double>(read_f32(decoded.value(), 0U));
    params.filter_b = static_cast<double>(read_f32(decoded.value(), 4U));
    params.size = static_cast<double>(read_f32(decoded.value(), 8U));
    params.highlights = static_cast<double>(read_f32(decoded.value(), 12U));
    params.mix = 1.0;
    auto canonical = monochrome_to_parameters(params);
    return canonical ? Result<MonochromeParams>{params} : canonical.error();
}

[[nodiscard]] Result<SplitToningParams>
decode_legacy_split_toning_parameters(const std::string &version, const std::string_view encoded)
{
    constexpr std::string_view frozen = "7b14ae3e6666663f7b146e3f6666663f3433b33e01007041";
    if (version != "1" || encoded != frozen)
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Split Toning state is outside the frozen evidence",
                          {{"legacy_operation", "splittoning"},
                           {"legacy_version", version},
                           {"reason", "unsupported_legacy_split_toning_parameters"}});
    auto decoded = decode_legacy_parameter_blob(encoded, 24U, "splittoning");
    if (!decoded)
        return decoded.error();
    SplitToningParams params;
    params.shadow_hue = read_f32(decoded.value(), 0U);
    params.shadow_saturation = read_f32(decoded.value(), 4U);
    params.highlight_hue = read_f32(decoded.value(), 8U);
    params.highlight_saturation = read_f32(decoded.value(), 12U);
    params.balance = read_f32(decoded.value(), 16U);
    params.compress = read_f32(decoded.value(), 20U);
    params.mix = 1.0;
    auto canonical = split_toning_to_parameters(params);
    return canonical ? Result<SplitToningParams>{params} : canonical.error();
}

[[nodiscard]] Result<VelviaParams> decode_legacy_velvia_parameters(const std::string &version,
                                                                   const std::string_view encoded)
{
    constexpr std::string_view frozen = "0000c8429a99193e";
    if (version != "2" || encoded != frozen)
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Velvia state is outside the frozen evidence",
                          {{"legacy_operation", "velvia"},
                           {"legacy_version", version},
                           {"reason", "unsupported_legacy_velvia_parameters"}});
    auto decoded = decode_legacy_parameter_blob(encoded, 8U, "velvia");
    if (!decoded)
        return decoded.error();
    VelviaParams params;
    params.strength = read_f32(decoded.value(), 0U);
    params.bias = read_f32(decoded.value(), 4U);
    auto canonical = velvia_to_parameters(params);
    return canonical ? Result<VelviaParams>{params} : canonical.error();
}

[[nodiscard]] Result<FrameParams> decode_legacy_frame_parameters(const std::string &version,
                                                                 const std::string_view encoded)
{
    constexpr std::string_view kFrozenV3 =
        "gz02eJxjYGiwZ0Dg/cn5ecUliXklCkn5RSmpRQwwwHV9sQ2QsjfUN2JYzcLAEMXGwMDNChSZwsQAE2/wvS8GUz9z5k77s2fO2ILkGJAAIxADAKSCFWo=";
    constexpr std::array<std::string_view, 2> kFrozenV4{
        "gz03eJzbucPC7vAhJ/u/f/7YMTAssGfAAhiB+OyZM7YMDA1Y5XGLgwGKHCOUBgC7mgxh",
        "gz02eJx7+OCB7eNHj+xu3oiwt+x7bM+ABTAB8dkzZ2y5ritjlWdgUMAqfvaMjy0DQ4P9wQMO9g+B9ixcoGDHCJUDAFchF5o="};
    const bool frozen =
        version == "3" ? encoded == kFrozenV3 :
        version == "4" ? std::find(kFrozenV4.begin(), kFrozenV4.end(), encoded) != kFrozenV4.end() :
                         false;
    if (!frozen)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Frame state is outside the frozen evidence",
                          {{"legacy_operation", "borders"},
                           {"legacy_version", version},
                           {"reason", "unsupported_legacy_frame_parameters"}});
    }
    const std::size_t size = version == "3" ? 116U : 120U;
    auto decoded = decode_legacy_parameter_blob(encoded, size, "borders");
    if (!decoded)
        return decoded.error();
    auto aspect_text = fixed_string(decoded.value(), 16U, 20U);
    auto position_h_text = fixed_string(decoded.value(), 48U, 20U);
    auto position_v_text = fixed_string(decoded.value(), 72U, 20U);
    if (!aspect_text || !position_h_text || !position_v_text)
        return !aspect_text     ? aspect_text.error() :
               !position_h_text ? position_h_text.error() :
                                  position_v_text.error();
    const std::int32_t orientation = read_i32(decoded.value(), 36U);
    const std::int32_t max_border_size = read_i32(decoded.value(), 112U);
    const std::int32_t basis = version == "3" ? 0 : read_i32(decoded.value(), 116U);
    if (orientation < 0 || orientation > 2 || max_border_size != 1 || basis < 0 || basis > 4)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Frame enum/reserved state is unsupported",
                          {{"reason", "unsupported_legacy_frame_parameters"}});
    }
    FrameParams params;
    params.border_color = {read_f32(decoded.value(), 0U), read_f32(decoded.value(), 4U),
                           read_f32(decoded.value(), 8U)};
    params.aspect = read_f32(decoded.value(), 12U);
    params.orientation = static_cast<FrameOrientation>(orientation);
    params.size = read_f32(decoded.value(), 40U);
    params.position_h = read_f32(decoded.value(), 44U);
    params.position_v = read_f32(decoded.value(), 68U);
    params.frame_size = read_f32(decoded.value(), 92U);
    params.frame_offset = read_f32(decoded.value(), 96U);
    params.frame_color = {read_f32(decoded.value(), 100U), read_f32(decoded.value(), 104U),
                          read_f32(decoded.value(), 108U)};
    params.basis = static_cast<FrameBasis>(basis);
    auto canonical = frame_to_parameters(params);
    return canonical ? Result<FrameParams>{params} : canonical.error();
}

[[nodiscard]] bool is_allowed_sharpen_attribute(const QStringView name) noexcept
{
    return name == u"num" || name == u"operation" || name == u"enabled" || name == u"modversion" ||
           name == u"params" || name == u"multi_name" || name == u"multi_priority" ||
           name == u"multi_name_hand_edited" || name == u"blendop_version" ||
           name == u"blendop_params";
}

[[nodiscard]] Result<OperationInstance>
map_sharpen_candidate(const LegacySharpenCandidate &candidate)
{
    for (const auto &attribute : candidate.attributes)
    {
        const auto name = attribute.name();
        if (name.contains(u"mask"))
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy Sharpen mask has no canonical graph mapping",
                              {{"attribute", utf8(name)},
                               {"legacy_operation", "sharpen"},
                               {"reason", "unsupported_legacy_sharpen_mask"}});
        }
        if (!is_allowed_sharpen_attribute(name) ||
            attribute.namespaceUri() != u"http://darktable.sf.net/")
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy Sharpen contains unproven history state",
                              {{"attribute", utf8(name)},
                               {"legacy_operation", "sharpen"},
                               {"reason", "unsupported_legacy_sharpen_attribute"}});
        }
    }
    const auto version = required_attribute(candidate.attributes, u"modversion", "sharpen");
    const auto enabled = required_attribute(candidate.attributes, u"enabled", "sharpen");
    const auto encoded = required_attribute(candidate.attributes, u"params", "sharpen");
    const auto blend_version =
        required_attribute(candidate.attributes, u"blendop_version", "sharpen");
    const auto blend_parameters =
        required_attribute(candidate.attributes, u"blendop_params", "sharpen");
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
                          "Legacy Sharpen version is outside the frozen evidence",
                          {{"legacy_operation", "sharpen"},
                           {"legacy_version", version.value()},
                           {"reason", "unsupported_legacy_sharpen_version"}});
    }
    if (enabled.value() != "1")
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Sharpen enabled state is outside the frozen fixture evidence",
                          {{"legacy_operation", "sharpen"},
                           {"reason", "unsupported_legacy_sharpen_enabled_state"}});
    }
    const bool frozen_blend =
        (blend_version.value() == "9" && blend_parameters.value() == kDefaultBlendParameters) ||
        (blend_version.value() == "11" && blend_parameters.value() == kFrozenSharpenBlendV11);
    if (!frozen_blend)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Sharpen blend is not a frozen unmasked default",
                          {{"legacy_blend_version", blend_version.value()},
                           {"legacy_operation", "sharpen"},
                           {"reason", "unsupported_legacy_sharpen_blend"}});
    }
    auto decoded = decode_legacy_sharpen_parameters(version.value(), encoded.value());
    if (!decoded)
    {
        return decoded.error();
    }
    auto parameters = sharpen_to_parameters(decoded.value());
    if (!parameters)
    {
        return parameters.error();
    }
    return OperationInstance{std::string(kSharpenOperationId),
                             kSharpenOperationSchemaVersion,
                             "legacy-sharpen-" + std::to_string(candidate.history_position),
                             true,
                             std::move(parameters).value(),
                             std::nullopt};
}

[[nodiscard]] bool is_allowed_dehaze_attribute(const QStringView name) noexcept
{
    return name == u"num" || name == u"operation" || name == u"enabled" || name == u"modversion" ||
           name == u"params" || name == u"multi_name" || name == u"multi_priority" ||
           name == u"multi_name_hand_edited" || name == u"blendop_version" ||
           name == u"blendop_params";
}

[[nodiscard]] Result<OperationInstance> map_dehaze_candidate(const LegacyDehazeCandidate &candidate)
{
    for (const auto &attribute : candidate.attributes)
    {
        const auto name = attribute.name();
        if (name.contains(u"mask"))
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy Haze Removal mask has no canonical graph mapping",
                              {{"attribute", utf8(name)},
                               {"legacy_operation", "hazeremoval"},
                               {"reason", "unsupported_legacy_dehaze_mask"}});
        }
        if (!is_allowed_dehaze_attribute(name) ||
            attribute.namespaceUri() != u"http://darktable.sf.net/")
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy Haze Removal contains unproven history state",
                              {{"attribute", utf8(name)},
                               {"legacy_operation", "hazeremoval"},
                               {"reason", "unsupported_legacy_dehaze_attribute"}});
        }
    }
    const auto version = required_attribute(candidate.attributes, u"modversion", "hazeremoval");
    const auto enabled = required_attribute(candidate.attributes, u"enabled", "hazeremoval");
    const auto encoded = required_attribute(candidate.attributes, u"params", "hazeremoval");
    const auto blend_version =
        required_attribute(candidate.attributes, u"blendop_version", "hazeremoval");
    const auto blend_parameters =
        required_attribute(candidate.attributes, u"blendop_params", "hazeremoval");
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
                          "Legacy Haze Removal version is outside the frozen evidence",
                          {{"legacy_operation", "hazeremoval"},
                           {"legacy_version", version.value()},
                           {"reason", "unsupported_legacy_dehaze_version"}});
    }
    if (enabled.value() != "1")
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Haze Removal enabled state is outside the frozen evidence",
                          {{"legacy_operation", "hazeremoval"},
                           {"reason", "unsupported_legacy_dehaze_enabled_state"}});
    }
    const bool frozen_blend =
        (blend_version.value() == "9" && blend_parameters.value() == kDefaultBlendParameters) ||
        (blend_version.value() == "13" && blend_parameters.value() == kFrozenDehazeBlendV13);
    if (!frozen_blend)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Haze Removal blend is not a frozen unmasked default",
                          {{"legacy_blend_version", blend_version.value()},
                           {"legacy_operation", "hazeremoval"},
                           {"reason", "unsupported_legacy_dehaze_blend"}});
    }
    auto decoded = decode_legacy_dehaze_parameters(version.value(), encoded.value());
    if (!decoded)
    {
        return decoded.error();
    }
    auto parameters = dehaze_to_parameters(decoded.value());
    if (!parameters)
    {
        return parameters.error();
    }
    return OperationInstance{std::string(kDehazeOperationId),
                             kDehazeOperationSchemaVersion,
                             "legacy-hazeremoval-" + std::to_string(candidate.history_position),
                             true,
                             std::move(parameters).value(),
                             std::nullopt};
}

[[nodiscard]] bool is_allowed_output_dither_attribute(const QStringView name) noexcept
{
    return name == u"num" || name == u"operation" || name == u"enabled" || name == u"modversion" ||
           name == u"params" || name == u"multi_name" || name == u"multi_priority" ||
           name == u"multi_name_hand_edited" || name == u"blendop_version" ||
           name == u"blendop_params";
}

[[nodiscard]] Result<OperationInstance>
map_output_dither_candidate(const LegacyOutputDitherCandidate &candidate)
{
    for (const auto &attribute : candidate.attributes)
    {
        const auto name = attribute.name();
        if (name.contains(u"mask"))
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy Output Dither mask has no canonical graph mapping",
                              {{"attribute", utf8(name)},
                               {"legacy_operation", "dither"},
                               {"reason", "unsupported_legacy_dither_mask"}});
        }
        if (!is_allowed_output_dither_attribute(name) ||
            attribute.namespaceUri() != u"http://darktable.sf.net/")
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy Output Dither contains unproven history state",
                              {{"attribute", utf8(name)},
                               {"legacy_operation", "dither"},
                               {"reason", "unsupported_legacy_dither_attribute"}});
        }
    }
    const auto version = required_attribute(candidate.attributes, u"modversion", "dither");
    const auto enabled = required_attribute(candidate.attributes, u"enabled", "dither");
    const auto encoded = required_attribute(candidate.attributes, u"params", "dither");
    const auto blend_version =
        required_attribute(candidate.attributes, u"blendop_version", "dither");
    const auto blend_parameters =
        required_attribute(candidate.attributes, u"blendop_params", "dither");
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
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Output Dither enabled state is outside the frozen evidence",
                          {{"legacy_operation", "dither"},
                           {"reason", "unsupported_legacy_dither_enabled_state"}});
    }
    auto decoded = decode_legacy_output_dither_parameters(version.value(), encoded.value());
    if (!decoded)
        return decoded.error();
    constexpr std::string_view kDitherBlendV10 = "gz14eJxjYIAACQYYOOHEgAYY0QVwggZ7CB6pfNoAAEkgGQQ=";
    constexpr std::string_view kPosterBlendV12 =
        "gz10eJxjYGBgYAFiCQYYOOHEgAZY0QVwggZ7CB6pfOygYtaVAyCMi08IAAB/xiOk";
    const bool poster = decoded.value().method == OutputDitherMethod::kPosterize4;
    const bool frozen_blend =
        poster ? blend_version.value() == "12" && blend_parameters.value() == kPosterBlendV12 :
                 blend_version.value() == "10" && blend_parameters.value() == kDitherBlendV10;
    if (!frozen_blend)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Output Dither blend is not the frozen unmasked default",
                          {{"legacy_blend_version", blend_version.value()},
                           {"legacy_operation", "dither"},
                           {"reason", "unsupported_legacy_dither_blend"}});
    }
    auto parameters = output_dither_to_parameters(decoded.value());
    if (!parameters)
        return parameters.error();
    return OperationInstance{std::string(kOutputDitherOperationId),
                             kOutputDitherOperationSchemaVersion,
                             "legacy-dither-" + std::to_string(candidate.history_position),
                             true,
                             std::move(parameters).value(),
                             std::nullopt};
}

[[nodiscard]] bool is_allowed_canvas_frame_attribute(const QStringView name) noexcept
{
    return name == u"num" || name == u"operation" || name == u"enabled" || name == u"modversion" ||
           name == u"params" || name == u"multi_name" || name == u"multi_priority" ||
           name == u"multi_name_hand_edited" || name == u"blendop_version" ||
           name == u"blendop_params";
}

template <typename Params, typename Decode, typename Serialize>
[[nodiscard]] Result<OperationInstance> map_canvas_frame_candidate(
    const QXmlStreamAttributes &attributes, const std::uint64_t history_position,
    const std::string_view legacy_operation, const std::string_view operation_id,
    const std::int64_t schema_version, Decode &&decode, Serialize &&serialize)
{
    for (const auto &attribute : attributes)
    {
        const auto name = attribute.name();
        if (name.contains(u"mask") || !is_allowed_canvas_frame_attribute(name) ||
            attribute.namespaceUri() != u"http://darktable.sf.net/")
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy canvas/frame contains unproven history state",
                              {{"attribute", utf8(name)},
                               {"legacy_operation", std::string(legacy_operation)},
                               {"reason", "unsupported_legacy_canvas_frame_attribute"}});
        }
    }
    const auto version = required_attribute(attributes, u"modversion", legacy_operation);
    const auto enabled = required_attribute(attributes, u"enabled", legacy_operation);
    const auto encoded = required_attribute(attributes, u"params", legacy_operation);
    const auto blend_version = required_attribute(attributes, u"blendop_version", legacy_operation);
    const auto blend = required_attribute(attributes, u"blendop_params", legacy_operation);
    if (!version || !enabled || !encoded || !blend_version || !blend)
        return !version       ? version.error() :
               !enabled       ? enabled.error() :
               !encoded       ? encoded.error() :
               !blend_version ? blend_version.error() :
                                blend.error();
    if (enabled.value() != "1")
        return make_error(ErrorCode::kUnsupported,
                          "Legacy canvas/frame disabled state is outside the frozen evidence",
                          {{"legacy_operation", std::string(legacy_operation)},
                           {"reason", "unsupported_legacy_canvas_frame_enabled_state"}});
    auto params = decode(version.value(), encoded.value());
    if (!params)
        return params.error();
    const bool canvas = legacy_operation == "enlargecanvas";
    const bool frozen_blend =
        canvas ? blend_version.value() == "13" && blend.value() == kGammaBlendGz11FeatherV1 :
        version.value() == "3" ?
                 blend_version.value() == "9" && blend.value() == kDefaultBlendParameters :
                 blend_version.value() == "13" && blend.value() == kGammaBlendGz12GuideFive;
    if (!frozen_blend)
        return make_error(ErrorCode::kUnsupported,
                          "Legacy canvas/frame blend is not the frozen unmasked default",
                          {{"legacy_operation", std::string(legacy_operation)},
                           {"reason", "unsupported_legacy_canvas_frame_blend"}});
    auto parameters = serialize(params.value());
    if (!parameters)
        return parameters.error();
    return OperationInstance{std::string(operation_id),
                             schema_version,
                             "legacy-" + std::string(legacy_operation) + "-" +
                                 std::to_string(history_position),
                             true,
                             std::move(parameters).value(),
                             std::nullopt};
}

[[nodiscard]] Result<OperationInstance> map_canvas_candidate(const LegacyCanvasCandidate &candidate)
{
    return map_canvas_frame_candidate<CanvasParams>(
        candidate.attributes, candidate.history_position, "enlargecanvas", kCanvasOperationId,
        kCanvasOperationSchemaVersion, decode_legacy_canvas_parameters, canvas_to_parameters);
}

[[nodiscard]] Result<OperationInstance> map_frame_candidate(const LegacyFrameCandidate &candidate)
{
    return map_canvas_frame_candidate<FrameParams>(
        candidate.attributes, candidate.history_position, "borders", kFrameOperationId,
        kFrameOperationSchemaVersion, decode_legacy_frame_parameters, frame_to_parameters);
}

[[nodiscard]] Result<OperationInstance>
map_color_zones_candidate(const LegacyColorZonesCandidate &candidate)
{
    for (const auto &attribute : candidate.attributes)
    {
        const auto name = attribute.name();
        if (name.contains(u"mask") || !is_allowed_canvas_frame_attribute(name) ||
            attribute.namespaceUri() != u"http://darktable.sf.net/")
            return make_error(ErrorCode::kUnsupported,
                              "Legacy Color Zones contains unproven history state",
                              {{"attribute", utf8(name)},
                               {"reason", "unsupported_legacy_color_zones_attribute"}});
    }
    const auto version = required_attribute(candidate.attributes, u"modversion", "colorzones");
    const auto enabled = required_attribute(candidate.attributes, u"enabled", "colorzones");
    const auto encoded = required_attribute(candidate.attributes, u"params", "colorzones");
    const auto blend_version =
        required_attribute(candidate.attributes, u"blendop_version", "colorzones");
    const auto blend = required_attribute(candidate.attributes, u"blendop_params", "colorzones");
    if (!version || !enabled || !encoded || !blend_version || !blend)
        return !version       ? version.error() :
               !enabled       ? enabled.error() :
               !encoded       ? encoded.error() :
               !blend_version ? blend_version.error() :
                                blend.error();
    if (enabled.value() != "1")
        return make_error(ErrorCode::kUnsupported, "Legacy Color Zones is not enabled",
                          {{"reason", "unsupported_legacy_color_zones_enabled_state"}});
    if (blend_version.value() != "9" || blend.value() != kDefaultBlendParameters)
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Color Zones blend is not the frozen unmasked default",
                          {{"reason", "unsupported_legacy_color_zones_blend"}});
    auto params = decode_legacy_color_zones_parameters(version.value(), encoded.value());
    if (!params)
        return params.error();
    auto parameters = color_zones_to_parameters(params.value());
    if (!parameters)
        return parameters.error();
    return OperationInstance{std::string(kColorZonesOperationId),
                             kColorZonesOperationSchemaVersion,
                             "legacy-colorzones-" + std::to_string(candidate.history_position),
                             true,
                             std::move(parameters).value(),
                             std::nullopt};
}

[[nodiscard]] Result<OperationInstance>
map_monochrome_candidate(const LegacyMonochromeCandidate &candidate)
{
    for (const auto &attribute : candidate.attributes)
    {
        const auto name = attribute.name();
        if (name.contains(u"mask") || !is_allowed_canvas_frame_attribute(name) ||
            attribute.namespaceUri() != u"http://darktable.sf.net/")
            return make_error(
                ErrorCode::kUnsupported, "Legacy Monochrome contains unproven history state",
                {{"attribute", utf8(name)}, {"reason", "unsupported_legacy_monochrome_attribute"}});
    }
    const auto version = required_attribute(candidate.attributes, u"modversion", "monochrome");
    const auto enabled = required_attribute(candidate.attributes, u"enabled", "monochrome");
    const auto encoded = required_attribute(candidate.attributes, u"params", "monochrome");
    const auto blend_version =
        required_attribute(candidate.attributes, u"blendop_version", "monochrome");
    const auto blend = required_attribute(candidate.attributes, u"blendop_params", "monochrome");
    if (!version || !enabled || !encoded || !blend_version || !blend)
        return !version       ? version.error() :
               !enabled       ? enabled.error() :
               !encoded       ? encoded.error() :
               !blend_version ? blend_version.error() :
                                blend.error();
    if (enabled.value() != "1")
        return make_error(ErrorCode::kUnsupported, "Legacy Monochrome is not enabled",
                          {{"reason", "unsupported_legacy_monochrome_enabled_state"}});
    if (blend_version.value() != "9" || blend.value() != kDefaultBlendParameters)
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Monochrome blend is not the frozen unmasked default",
                          {{"reason", "unsupported_legacy_monochrome_blend"}});
    auto params = decode_legacy_monochrome_parameters(version.value(), encoded.value());
    if (!params)
        return params.error();
    auto parameters = monochrome_to_parameters(params.value());
    if (!parameters)
        return parameters.error();
    return OperationInstance{std::string(kMonochromeOperationId),
                             kMonochromeOperationSchemaVersion,
                             "legacy-monochrome-" + std::to_string(candidate.history_position),
                             true,
                             std::move(parameters).value(),
                             std::nullopt};
}

[[nodiscard]] Result<OperationInstance>
map_split_toning_candidate(const LegacySplitToningCandidate &candidate)
{
    for (const auto &attribute : candidate.attributes)
    {
        const auto name = attribute.name();
        if (name.contains(u"mask") || !is_allowed_canvas_frame_attribute(name) ||
            attribute.namespaceUri() != u"http://darktable.sf.net/")
            return make_error(ErrorCode::kUnsupported,
                              "Legacy Split Toning contains unproven history state",
                              {{"attribute", utf8(name)},
                               {"reason", "unsupported_legacy_split_toning_attribute"}});
    }
    const auto version = required_attribute(candidate.attributes, u"modversion", "splittoning");
    const auto enabled = required_attribute(candidate.attributes, u"enabled", "splittoning");
    const auto encoded = required_attribute(candidate.attributes, u"params", "splittoning");
    const auto blend_version =
        required_attribute(candidate.attributes, u"blendop_version", "splittoning");
    const auto blend = required_attribute(candidate.attributes, u"blendop_params", "splittoning");
    if (!version || !enabled || !encoded || !blend_version || !blend)
        return !version       ? version.error() :
               !enabled       ? enabled.error() :
               !encoded       ? encoded.error() :
               !blend_version ? blend_version.error() :
                                blend.error();
    if (enabled.value() != "1")
        return make_error(ErrorCode::kUnsupported, "Legacy Split Toning is not enabled",
                          {{"reason", "unsupported_legacy_split_toning_enabled_state"}});
    if (blend_version.value() != "9" || blend.value() != kDefaultBlendParameters)
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Split Toning blend is not the frozen unmasked default",
                          {{"reason", "unsupported_legacy_split_toning_blend"}});
    auto params = decode_legacy_split_toning_parameters(version.value(), encoded.value());
    if (!params)
        return params.error();
    auto parameters = split_toning_to_parameters(params.value());
    if (!parameters)
        return parameters.error();
    return OperationInstance{std::string(kSplitToningOperationId),
                             kSplitToningOperationSchemaVersion,
                             "legacy-splittoning-" + std::to_string(candidate.history_position),
                             true,
                             std::move(parameters).value(),
                             std::nullopt};
}

[[nodiscard]] Result<OperationInstance> map_velvia_candidate(const LegacyVelviaCandidate &candidate)
{
    for (const auto &attribute : candidate.attributes)
    {
        const auto name = attribute.name();
        if (name.contains(u"mask") || !is_allowed_canvas_frame_attribute(name) ||
            attribute.namespaceUri() != u"http://darktable.sf.net/")
            return make_error(
                ErrorCode::kUnsupported, "Legacy Velvia contains unproven history state",
                {{"attribute", utf8(name)}, {"reason", "unsupported_legacy_velvia_attribute"}});
    }
    const auto version = required_attribute(candidate.attributes, u"modversion", "velvia");
    const auto enabled = required_attribute(candidate.attributes, u"enabled", "velvia");
    const auto encoded = required_attribute(candidate.attributes, u"params", "velvia");
    const auto blend_version =
        required_attribute(candidate.attributes, u"blendop_version", "velvia");
    const auto blend = required_attribute(candidate.attributes, u"blendop_params", "velvia");
    if (!version || !enabled || !encoded || !blend_version || !blend)
        return !version       ? version.error() :
               !enabled       ? enabled.error() :
               !encoded       ? encoded.error() :
               !blend_version ? blend_version.error() :
                                blend.error();
    if (enabled.value() != "1")
        return make_error(ErrorCode::kUnsupported, "Legacy Velvia is not enabled",
                          {{"reason", "unsupported_legacy_velvia_enabled_state"}});
    constexpr std::string_view frozen_blend =
        "gz13eJxjYGBgYAZiCQYYOOHEgAYY0QVwggZ7CB6pfNoAAE4AGQc=";
    if (blend_version.value() != "10" || blend.value() != frozen_blend)
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Velvia blend is not the frozen unmasked default",
                          {{"reason", "unsupported_legacy_velvia_blend"}});
    auto params = decode_legacy_velvia_parameters(version.value(), encoded.value());
    if (!params)
        return params.error();
    auto parameters = velvia_to_parameters(params.value());
    if (!parameters)
        return parameters.error();
    return OperationInstance{std::string(kVelviaOperationId),
                             kVelviaOperationSchemaVersion,
                             "legacy-velvia-" + std::to_string(candidate.history_position),
                             true,
                             std::move(parameters).value(),
                             std::nullopt};
}

} // namespace ravo::legacy_xmp_internal
