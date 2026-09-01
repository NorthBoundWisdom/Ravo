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

[[nodiscard]] bool is_legacy_unmasked_geometry_blend(const std::string_view blend) noexcept
{
    return blend == kDefaultBlendParameters || blend == kLegacyFlipBlendGz14 ||
           blend == kLegacyGeometryBlendGz14GuideFive;
}

[[nodiscard]] bool is_legacy_unmasked_tone_blend(const std::string_view blend) noexcept
{
    return is_legacy_unmasked_geometry_blend(blend) || blend == kLegacyToneBlendGz13 ||
           blend == kLegacyRawDenoiseBlendGz13;
}

[[nodiscard]] bool is_allowed_geometry_attribute(const QStringView name) noexcept
{
    return name == u"num" || name == u"operation" || name == u"enabled" || name == u"modversion" ||
           name == u"params" || name == u"multi_name" || name == u"multi_priority" ||
           name == u"multi_name_hand_edited" || name == u"blendop_version" ||
           name == u"blendop_params";
}

[[nodiscard]] Result<LeftoverFlipGeometry> map_legacy_flip(const QXmlStreamAttributes &attributes)
{
    for (const auto &attribute : attributes)
    {
        const auto name = attribute.name();
        if (name.contains(u"mask"))
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy flip mask has no canonical graph mapping",
                              {{"attribute", utf8(name)},
                               {"legacy_operation", "flip"},
                               {"reason", "unsupported_legacy_flip_mask"}});
        }
        if (!is_allowed_geometry_attribute(name) ||
            attribute.namespaceUri() != u"http://darktable.sf.net/")
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy flip contains unproven history state",
                              {{"attribute", utf8(name)},
                               {"legacy_operation", "flip"},
                               {"reason", "unsupported_legacy_flip_attribute"}});
        }
    }
    const auto version = required_attribute(attributes, u"modversion", "flip");
    const auto enabled = required_attribute(attributes, u"enabled", "flip");
    const auto encoded = required_attribute(attributes, u"params", "flip");
    const auto blend = required_attribute(attributes, u"blendop_params", "flip");
    const auto priority = required_attribute(attributes, u"multi_priority", "flip");
    const auto name = attribute_value(attributes, u"multi_name");
    if (!version || !enabled || !encoded || !blend || !priority || !name)
    {
        return !version ? version.error() :
               !enabled ? enabled.error() :
               !encoded ? encoded.error() :
               !blend   ? blend.error() :
               !priority ?
                        priority.error() :
                        make_error(ErrorCode::kUnsupported, "Legacy flip singleton name is missing",
                                   {{"attribute", "multi_name"},
                                    {"legacy_operation", "flip"},
                                    {"reason", "unsupported_legacy_flip_multi_state"}});
    }
    const auto hand_edited = attribute_value(attributes, u"multi_name_hand_edited");
    if (priority.value() != "0" || !name->empty() || (hand_edited && *hand_edited != "0"))
    {
        return make_error(
            ErrorCode::kUnsupported, "Legacy flip instance is not the frozen singleton priority",
            {{"legacy_operation", "flip"}, {"reason", "unsupported_legacy_flip_multi_state"}});
    }
    if (version.value() != "2")
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy flip version is outside the frozen evidence",
                          {{"legacy_operation", "flip"},
                           {"legacy_version", version.value()},
                           {"reason", "unsupported_legacy_flip_version"}});
    }
    if (enabled.value() != "1")
    {
        return make_error(
            ErrorCode::kUnsupported,
            "Legacy flip enabled state is outside the frozen fixture evidence",
            {{"legacy_operation", "flip"}, {"reason", "unsupported_legacy_flip_enabled_state"}});
    }
    if (!is_legacy_unmasked_geometry_blend(blend.value()))
    {
        return make_error(
            ErrorCode::kUnsupported, "Legacy flip blend is not a frozen unmasked default",
            {{"legacy_operation", "flip"}, {"reason", "unsupported_legacy_flip_blend"}});
    }
    auto decoded = decode_legacy_parameter_blob(encoded.value(), sizeof(std::int32_t), "flip");
    if (!decoded)
    {
        return decoded.error();
    }
    return leftover_flip_orientation_to_geometry(read_i32(decoded.value(), 0U));
}

[[nodiscard]] Result<LeftoverCropBox> map_legacy_crop(const QXmlStreamAttributes &attributes)
{
    for (const auto &attribute : attributes)
    {
        const auto name = attribute.name();
        if (name.contains(u"mask"))
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy crop mask has no canonical graph mapping",
                              {{"attribute", utf8(name)},
                               {"legacy_operation", "crop"},
                               {"reason", "unsupported_legacy_crop_mask"}});
        }
        if (!is_allowed_geometry_attribute(name) ||
            attribute.namespaceUri() != u"http://darktable.sf.net/")
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy crop contains unproven history state",
                              {{"attribute", utf8(name)},
                               {"legacy_operation", "crop"},
                               {"reason", "unsupported_legacy_crop_attribute"}});
        }
    }
    const auto version = required_attribute(attributes, u"modversion", "crop");
    const auto enabled = required_attribute(attributes, u"enabled", "crop");
    const auto encoded = required_attribute(attributes, u"params", "crop");
    const auto blend = required_attribute(attributes, u"blendop_params", "crop");
    const auto priority = required_attribute(attributes, u"multi_priority", "crop");
    const auto name = attribute_value(attributes, u"multi_name");
    if (!version || !enabled || !encoded || !blend || !priority || !name)
    {
        return !version ? version.error() :
               !enabled ? enabled.error() :
               !encoded ? encoded.error() :
               !blend   ? blend.error() :
               !priority ?
                        priority.error() :
                        make_error(ErrorCode::kUnsupported, "Legacy crop singleton name is missing",
                                   {{"attribute", "multi_name"},
                                    {"legacy_operation", "crop"},
                                    {"reason", "unsupported_legacy_crop_multi_state"}});
    }
    const auto hand_edited = attribute_value(attributes, u"multi_name_hand_edited");
    if (priority.value() != "0" || !name->empty() || (hand_edited && *hand_edited != "0"))
    {
        return make_error(
            ErrorCode::kUnsupported, "Legacy crop instance is not the frozen singleton priority",
            {{"legacy_operation", "crop"}, {"reason", "unsupported_legacy_crop_multi_state"}});
    }
    if (version.value() != "1" && version.value() != "2" && version.value() != "3")
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy crop version is outside the frozen evidence",
                          {{"legacy_operation", "crop"},
                           {"legacy_version", version.value()},
                           {"reason", "unsupported_legacy_crop_version"}});
    }
    if (enabled.value() != "1")
    {
        return make_error(
            ErrorCode::kUnsupported,
            "Legacy crop enabled state is outside the frozen fixture evidence",
            {{"legacy_operation", "crop"}, {"reason", "unsupported_legacy_crop_enabled_state"}});
    }
    if (!is_legacy_unmasked_geometry_blend(blend.value()))
    {
        return make_error(
            ErrorCode::kUnsupported, "Legacy crop blend is not a frozen unmasked default",
            {{"legacy_operation", "crop"}, {"reason", "unsupported_legacy_crop_blend"}});
    }
    auto decoded = decode_legacy_parameter_blob(encoded.value(), 24U, "crop");
    if (!decoded)
    {
        return decoded.error();
    }
    return leftover_crop_box_to_geometry(
        read_f32(decoded.value(), 0U), read_f32(decoded.value(), 4U), read_f32(decoded.value(), 8U),
        read_f32(decoded.value(), 12U));
}

[[nodiscard]] Result<PerspectiveParams> map_legacy_ashift(const QXmlStreamAttributes &attributes)
{
    for (const auto &attribute : attributes)
    {
        const auto name = attribute.name();
        if (name.contains(u"mask"))
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy ashift mask has no canonical graph mapping",
                              {{"attribute", utf8(name)},
                               {"legacy_operation", "ashift"},
                               {"reason", "unsupported_legacy_ashift_mask"}});
        }
        if (!is_allowed_geometry_attribute(name) ||
            attribute.namespaceUri() != u"http://darktable.sf.net/")
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy ashift contains unproven history state",
                              {{"attribute", utf8(name)},
                               {"legacy_operation", "ashift"},
                               {"reason", "unsupported_legacy_ashift_attribute"}});
        }
    }
    const auto version = required_attribute(attributes, u"modversion", "ashift");
    const auto enabled = required_attribute(attributes, u"enabled", "ashift");
    const auto encoded = required_attribute(attributes, u"params", "ashift");
    const auto blend = required_attribute(attributes, u"blendop_params", "ashift");
    const auto priority = required_attribute(attributes, u"multi_priority", "ashift");
    const auto name = attribute_value(attributes, u"multi_name");
    if (!version || !enabled || !encoded || !blend || !priority || !name)
    {
        return !version  ? version.error() :
               !enabled  ? enabled.error() :
               !encoded  ? encoded.error() :
               !blend    ? blend.error() :
               !priority ? priority.error() :
                           make_error(ErrorCode::kUnsupported,
                                      "Legacy ashift singleton name is missing",
                                      {{"attribute", "multi_name"},
                                       {"legacy_operation", "ashift"},
                                       {"reason", "unsupported_legacy_ashift_multi_state"}});
    }
    const auto hand_edited = attribute_value(attributes, u"multi_name_hand_edited");
    if (priority.value() != "0" || !name->empty() || (hand_edited && *hand_edited != "0"))
    {
        return make_error(
            ErrorCode::kUnsupported, "Legacy ashift instance is not the frozen singleton priority",
            {{"legacy_operation", "ashift"}, {"reason", "unsupported_legacy_ashift_multi_state"}});
    }
    if (version.value() != "4" && version.value() != "5")
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy ashift version is outside the frozen evidence",
                          {{"legacy_operation", "ashift"},
                           {"legacy_version", version.value()},
                           {"reason", "unsupported_legacy_ashift_version"}});
    }
    if (enabled.value() != "1")
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy ashift enabled state is outside the frozen fixture evidence",
                          {{"legacy_operation", "ashift"},
                           {"reason", "unsupported_legacy_ashift_enabled_state"}});
    }
    if (!is_legacy_unmasked_geometry_blend(blend.value()))
    {
        return make_error(
            ErrorCode::kUnsupported, "Legacy ashift blend is not a frozen unmasked default",
            {{"legacy_operation", "ashift"}, {"reason", "unsupported_legacy_ashift_blend"}});
    }
    // v4 retains an obsolete UI-only toggle between mode and cropmode. v5
    // removed it before appending saved guide-line state.
    const bool version_four = version.value() == "4";
    const std::size_t crop_mode_offset = version_four ? 40U : 36U;
    const std::size_t crop_box_offset = version_four ? 44U : 40U;
    const std::size_t minimum_size = crop_box_offset + 4U * sizeof(float);
    auto decoded = decode_legacy_parameter_blob_min(encoded.value(), minimum_size, "ashift");
    if (!decoded)
    {
        return decoded.error();
    }
    const float focal_length = read_f32(decoded.value(), 16U);
    const float crop_factor = read_f32(decoded.value(), 20U);
    const float orthographic_correction = read_f32(decoded.value(), 24U);
    const float aspect = read_f32(decoded.value(), 28U);
    const std::int32_t mode = read_i32(decoded.value(), 32U);
    if (version_four)
    {
        const std::int32_t obsolete_toggle = read_i32(decoded.value(), 36U);
        if (obsolete_toggle < 0 || obsolete_toggle > 1)
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy ashift UI toggle is outside frozen evidence",
                              {{"legacy_operation", "ashift"},
                               {"reason", "unsupported_legacy_ashift_geometry_state"}});
        }
    }
    const std::int32_t crop_mode = read_i32(decoded.value(), crop_mode_offset);
    const std::array<float, 4> crop_box{
        read_f32(decoded.value(), crop_box_offset),
        read_f32(decoded.value(), crop_box_offset + sizeof(float)),
        read_f32(decoded.value(), crop_box_offset + 2U * sizeof(float)),
        read_f32(decoded.value(), crop_box_offset + 3U * sizeof(float))};
    if (!std::isfinite(focal_length) || !std::isfinite(crop_factor) ||
        !std::isfinite(orthographic_correction) || !std::isfinite(aspect) ||
        !std::all_of(crop_box.begin(), crop_box.end(),
                     [](const float value) { return std::isfinite(value); }) ||
        focal_length <= 0.0F || crop_factor <= 0.0F || orthographic_correction < 0.0F ||
        orthographic_correction > 100.0F || aspect < 0.5F || aspect > 2.0F || crop_box[0] < 0.0F ||
        crop_box[1] > 1.0F || crop_box[2] < 0.0F || crop_box[3] > 1.0F ||
        crop_box[1] <= crop_box[0] || crop_box[3] <= crop_box[2])
    {
        return make_error(ErrorCode::kUnsupported, "Legacy ashift geometry state is invalid",
                          {{"legacy_operation", "ashift"},
                           {"reason", "unsupported_legacy_ashift_geometry_state"}});
    }
    if (mode != 0)
    {
        return make_error(
            ErrorCode::kUnsupported, "Legacy ashift specific-lens mode has no canonical mapping",
            {{"legacy_operation", "ashift"}, {"reason", "unsupported_legacy_ashift_lens_mode"}});
    }
    if (crop_mode < 0 || crop_mode > 2 || crop_mode == 2)
    {
        return make_error(
            ErrorCode::kUnsupported, "Legacy ashift crop mode has no canonical mapping",
            {{"legacy_operation", "ashift"}, {"reason", "unsupported_legacy_ashift_crop_mode"}});
    }
    constexpr float kNearIdentity = 1.0e-4F;
    const bool full_crop =
        std::abs(crop_box[0]) <= kNearIdentity && std::abs(crop_box[1] - 1.0F) <= kNearIdentity &&
        std::abs(crop_box[2]) <= kNearIdentity && std::abs(crop_box[3] - 1.0F) <= kNearIdentity;
    if (crop_mode == 0 && !full_crop)
    {
        return make_error(
            ErrorCode::kUnsupported, "Legacy ashift manual crop box has no canonical mapping",
            {{"legacy_operation", "ashift"}, {"reason", "unsupported_legacy_ashift_crop_box"}});
    }
    return leftover_ashift_to_perspective(
        read_f32(decoded.value(), 0U), read_f32(decoded.value(), 4U), read_f32(decoded.value(), 8U),
        read_f32(decoded.value(), 12U), crop_mode == 1);
}

[[nodiscard]] Result<RgbLevelsParams> map_legacy_rgblevels(const QXmlStreamAttributes &attributes)
{
    for (const auto &attribute : attributes)
    {
        const auto name = attribute.name();
        if (name.contains(u"mask"))
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy RGB levels mask has no canonical graph mapping",
                              {{"attribute", utf8(name)},
                               {"legacy_operation", "rgblevels"},
                               {"reason", "unsupported_legacy_rgblevels_mask"}});
        }
        if (!is_allowed_geometry_attribute(name) ||
            attribute.namespaceUri() != u"http://darktable.sf.net/")
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy RGB levels contains unproven history state",
                              {{"attribute", utf8(name)},
                               {"legacy_operation", "rgblevels"},
                               {"reason", "unsupported_legacy_rgblevels_attribute"}});
        }
    }
    const auto version = required_attribute(attributes, u"modversion", "rgblevels");
    const auto enabled = required_attribute(attributes, u"enabled", "rgblevels");
    const auto encoded = required_attribute(attributes, u"params", "rgblevels");
    const auto blend = required_attribute(attributes, u"blendop_params", "rgblevels");
    const auto priority = required_attribute(attributes, u"multi_priority", "rgblevels");
    const auto name = attribute_value(attributes, u"multi_name");
    if (!version || !enabled || !encoded || !blend || !priority || !name)
    {
        return !version  ? version.error() :
               !enabled  ? enabled.error() :
               !encoded  ? encoded.error() :
               !blend    ? blend.error() :
               !priority ? priority.error() :
                           make_error(ErrorCode::kUnsupported,
                                      "Legacy RGB levels singleton name is missing",
                                      {{"attribute", "multi_name"},
                                       {"legacy_operation", "rgblevels"},
                                       {"reason", "unsupported_legacy_rgblevels_multi_state"}});
    }
    const auto hand_edited = attribute_value(attributes, u"multi_name_hand_edited");
    if (priority.value() != "0" || !name->empty() || (hand_edited && *hand_edited != "0"))
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy RGB levels instance is not the frozen singleton priority",
                          {{"legacy_operation", "rgblevels"},
                           {"reason", "unsupported_legacy_rgblevels_multi_state"}});
    }
    if (version.value() != "1")
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy RGB levels version is outside the frozen evidence",
                          {{"legacy_operation", "rgblevels"},
                           {"legacy_version", version.value()},
                           {"reason", "unsupported_legacy_rgblevels_version"}});
    }
    if (enabled.value() != "1")
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy RGB levels enabled state is outside the frozen fixture evidence",
                          {{"legacy_operation", "rgblevels"},
                           {"reason", "unsupported_legacy_rgblevels_enabled_state"}});
    }
    if (!is_legacy_unmasked_tone_blend(blend.value()))
    {
        return make_error(
            ErrorCode::kUnsupported, "Legacy RGB levels blend is not a frozen unmasked default",
            {{"legacy_operation", "rgblevels"}, {"reason", "unsupported_legacy_rgblevels_blend"}});
    }
    auto decoded = decode_legacy_parameter_blob(encoded.value(), 44U, "rgblevels");
    if (!decoded)
    {
        return decoded.error();
    }
    std::array<float, 9> stops{};
    for (std::size_t index = 0; index < stops.size(); ++index)
    {
        stops[index] = read_f32(decoded.value(), 8U + index * 4U);
    }
    return leftover_rgblevels_from_v1(read_i32(decoded.value(), 0U), read_i32(decoded.value(), 4U),
                                      stops);
}

[[nodiscard]] Result<RgbCurveParams> map_legacy_rgbcurve(const QXmlStreamAttributes &attributes)
{
    for (const auto &attribute : attributes)
    {
        const auto name = attribute.name();
        if (name.contains(u"mask"))
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy RGB curve mask has no canonical graph mapping",
                              {{"attribute", utf8(name)},
                               {"legacy_operation", "rgbcurve"},
                               {"reason", "unsupported_legacy_rgbcurve_mask"}});
        }
        if (!is_allowed_geometry_attribute(name) ||
            attribute.namespaceUri() != u"http://darktable.sf.net/")
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy RGB curve contains unproven history state",
                              {{"attribute", utf8(name)},
                               {"legacy_operation", "rgbcurve"},
                               {"reason", "unsupported_legacy_rgbcurve_attribute"}});
        }
    }
    const auto version = required_attribute(attributes, u"modversion", "rgbcurve");
    const auto enabled = required_attribute(attributes, u"enabled", "rgbcurve");
    const auto encoded = required_attribute(attributes, u"params", "rgbcurve");
    const auto blend = required_attribute(attributes, u"blendop_params", "rgbcurve");
    const auto priority = required_attribute(attributes, u"multi_priority", "rgbcurve");
    const auto name = attribute_value(attributes, u"multi_name");
    if (!version || !enabled || !encoded || !blend || !priority || !name)
    {
        return !version  ? version.error() :
               !enabled  ? enabled.error() :
               !encoded  ? encoded.error() :
               !blend    ? blend.error() :
               !priority ? priority.error() :
                           make_error(ErrorCode::kUnsupported,
                                      "Legacy RGB curve singleton name is missing",
                                      {{"attribute", "multi_name"},
                                       {"legacy_operation", "rgbcurve"},
                                       {"reason", "unsupported_legacy_rgbcurve_multi_state"}});
    }
    const auto hand_edited = attribute_value(attributes, u"multi_name_hand_edited");
    if (priority.value() != "0" || !name->empty() || (hand_edited && *hand_edited != "0"))
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy RGB curve instance is not the frozen singleton priority",
                          {{"legacy_operation", "rgbcurve"},
                           {"reason", "unsupported_legacy_rgbcurve_multi_state"}});
    }
    if (version.value() != "1")
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy RGB curve version is outside the frozen evidence",
                          {{"legacy_operation", "rgbcurve"},
                           {"legacy_version", version.value()},
                           {"reason", "unsupported_legacy_rgbcurve_version"}});
    }
    if (enabled.value() != "1")
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy RGB curve enabled state is outside the frozen fixture evidence",
                          {{"legacy_operation", "rgbcurve"},
                           {"reason", "unsupported_legacy_rgbcurve_enabled_state"}});
    }
    if (!is_legacy_unmasked_tone_blend(blend.value()))
    {
        return make_error(
            ErrorCode::kUnsupported, "Legacy RGB curve blend is not a frozen unmasked default",
            {{"legacy_operation", "rgbcurve"}, {"reason", "unsupported_legacy_rgbcurve_blend"}});
    }
    auto decoded = decode_legacy_parameter_blob(encoded.value(), 516U, "rgbcurve");
    if (!decoded)
    {
        return decoded.error();
    }
    return leftover_rgbcurve_from_v1(decoded.value());
}

[[nodiscard]] Result<LeftoverRawDenoise>
map_legacy_rawdenoise(const QXmlStreamAttributes &attributes)
{
    for (const auto &attribute : attributes)
    {
        const auto name = attribute.name();
        if (name.contains(u"mask"))
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy RAW denoise mask has no canonical graph mapping",
                              {{"attribute", utf8(name)},
                               {"legacy_operation", "rawdenoise"},
                               {"reason", "unsupported_legacy_rawdenoise_mask"}});
        }
        if (!is_allowed_geometry_attribute(name) ||
            attribute.namespaceUri() != u"http://darktable.sf.net/")
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy RAW denoise contains unproven history state",
                              {{"attribute", utf8(name)},
                               {"legacy_operation", "rawdenoise"},
                               {"reason", "unsupported_legacy_rawdenoise_attribute"}});
        }
    }
    const auto version = required_attribute(attributes, u"modversion", "rawdenoise");
    const auto enabled = required_attribute(attributes, u"enabled", "rawdenoise");
    const auto encoded = required_attribute(attributes, u"params", "rawdenoise");
    const auto blend = required_attribute(attributes, u"blendop_params", "rawdenoise");
    const auto priority = required_attribute(attributes, u"multi_priority", "rawdenoise");
    const auto name = attribute_value(attributes, u"multi_name");
    if (!version || !enabled || !encoded || !blend || !priority || !name)
    {
        return !version  ? version.error() :
               !enabled  ? enabled.error() :
               !encoded  ? encoded.error() :
               !blend    ? blend.error() :
               !priority ? priority.error() :
                           make_error(ErrorCode::kUnsupported,
                                      "Legacy RAW denoise singleton name is missing",
                                      {{"attribute", "multi_name"},
                                       {"legacy_operation", "rawdenoise"},
                                       {"reason", "unsupported_legacy_rawdenoise_multi_state"}});
    }
    const auto hand_edited = attribute_value(attributes, u"multi_name_hand_edited");
    if (priority.value() != "0" || !name->empty() || (hand_edited && *hand_edited != "0"))
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy RAW denoise instance is not the frozen singleton priority",
                          {{"legacy_operation", "rawdenoise"},
                           {"reason", "unsupported_legacy_rawdenoise_multi_state"}});
    }
    if (version.value() != "2")
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy RAW denoise version is outside the frozen evidence",
                          {{"legacy_operation", "rawdenoise"},
                           {"legacy_version", version.value()},
                           {"reason", "unsupported_legacy_rawdenoise_version"}});
    }
    if (enabled.value() != "1")
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy RAW denoise enabled state is outside the frozen fixture evidence",
                          {{"legacy_operation", "rawdenoise"},
                           {"reason", "unsupported_legacy_rawdenoise_enabled_state"}});
    }
    if (!is_legacy_unmasked_tone_blend(blend.value()))
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy RAW denoise blend is not a frozen unmasked default",
                          {{"legacy_operation", "rawdenoise"},
                           {"reason", "unsupported_legacy_rawdenoise_blend"}});
    }
    auto decoded = decode_legacy_parameter_blob(encoded.value(), 164U, "rawdenoise");
    if (!decoded)
    {
        return decoded.error();
    }
    LeftoverRawDenoise mapped;
    auto parsed = leftover_rawdenoise_from_v2(decoded.value(), mapped.threshold, mapped.bands);
    if (!parsed)
    {
        return parsed.error();
    }
    return mapped;
}

constexpr std::array<std::string_view, 5> kFrozenRetouchBlendParameters{
    "gz10eJxjYGBgkGAAgRNOIPJN67k4BihgZCAWNNhD8EjlYwcAZ+wboQ==",
    "gz09eJxjYGBgYAFiCQYYOOEEIk8zn46HiTAyEAsa7CF4pPKxg4pZVw6AMC4+IQAAgt0lmA==",
    "gz09eJxjYGBgYAFiCQYYOOEEIptZTsfDRBgZiAUN9hA8UvnYQcWsKwdAGBefEAAAFQglUQ==",
    "gz10eJxjYGBgYAFiCQYYOOHEgAYY0QVwggZ7CB6pfOygYtaVAyCMi08IAAB5xiOg",
    "gz09eJxjYGBgYAFiCQYYOOEEIk1YT8fDRBgZiAUN9hA8UvnYQcWsKwdAGBefEAAAnFAlAw==",
};

constexpr std::array<std::string_view, 5> kFrozenRetouchParameters{
    "gz99eJzt2rENQGAYBNCvQGsFkxCLGMEEWkYwxz+BEdQqAxhALaJR6DXvdXfJbXDbuHZ5RGQBAAAAAAAAAAAAAAD/uX/txSuf5bykfq+PYWqepmq/dhczfgm8",
    "gz99eJzt2jENgDAQhtE2NYKUoqRWuzGwIAAVKIAENhpyO+8t//Kdg1vK2tIlp29bsNuD3RHsAAAAAAAAAAAAAAD4h/d/ee3P1nuneXR3ApqZCgA=",
    "gz99eJzt2jENgDAQhtGmqRCkFCW1UgFIQAw7O3oggY2G3M57y7985+B62Vu65PRtCXZrsDuCHQAAAAAAAAAAAAAA//D+L6/bs/XeaR7dnbw+CSI=",
    "gz99eJzty7EJACAMRNGAizhK3NzRLLRLepv3mg8HFwEAAAAAAAAAAAAAAP+NsuR+zdu5ut8Ba9UB5Q==",
    "gz93eJzt2s0NAWEQBuBPcFCHCtw3rO1AAaIKNWxD7twVoAmHzQpnP/tJhOHs8DyXSWbezDQwk+F+lW76qXNcjKoUmL/keo9OE+aWb/u+5eqcG0TDp8O42AS5+/3LeT07te00t7a/1gAAAAAAAAAAAAAA8P8+/8vLXa5lV5sq+ie/Ao3zHMg=",
};

[[nodiscard]] Result<LegacyRetouchCandidate>
capture_retouch_candidate(const QXmlStreamAttributes &attributes)
{
    for (const auto &attribute : attributes)
    {
        if (!is_allowed_geometry_attribute(attribute.name()) ||
            attribute.namespaceUri() != u"http://darktable.sf.net/")
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy Retouch contains unproven history state",
                              {{"attribute", utf8(attribute.name())},
                               {"legacy_operation", "retouch"},
                               {"reason", "unsupported_legacy_retouch_attribute"}});
        }
    }
    const auto position = required_attribute(attributes, u"num", "retouch");
    const auto version = required_attribute(attributes, u"modversion", "retouch");
    const auto enabled = required_attribute(attributes, u"enabled", "retouch");
    const auto parameters = required_attribute(attributes, u"params", "retouch");
    const auto priority = required_attribute(attributes, u"multi_priority", "retouch");
    const auto name = attribute_value(attributes, u"multi_name");
    const auto blend_version = required_attribute(attributes, u"blendop_version", "retouch");
    const auto blend = required_attribute(attributes, u"blendop_params", "retouch");
    if (!position || !version || !enabled || !parameters || !priority || !name || !blend_version ||
        !blend)
    {
        return !position      ? position.error() :
               !version       ? version.error() :
               !enabled       ? enabled.error() :
               !parameters    ? parameters.error() :
               !priority      ? priority.error() :
               !name          ? make_error(ErrorCode::kUnsupported,
                                           "Legacy Retouch singleton name is missing",
                                           {{"legacy_operation", "retouch"},
                                            {"reason", "unsupported_legacy_retouch_multi_state"}}) :
               !blend_version ? blend_version.error() :
                                blend.error();
    }
    auto parsed_position = legacy_history_position(position.value(), "num", "retouch",
                                                   "invalid_legacy_retouch_revision");
    if (!parsed_position)
        return parsed_position.error();
    const auto hand_edited = attribute_value(attributes, u"multi_name_hand_edited");
    if (priority.value() != "0" || !name->empty() || (hand_edited && *hand_edited != "0"))
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Retouch instance is outside the frozen singleton state",
                          {{"legacy_operation", "retouch"},
                           {"reason", "unsupported_legacy_retouch_multi_state"}});
    }
    if (version.value() != "1" || enabled.value() != "1")
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Retouch version or enabled state is outside the frozen evidence",
                          {{"legacy_operation", "retouch"},
                           {"legacy_version", version.value()},
                           {"reason", "unsupported_legacy_retouch_version"}});
    }
    const bool frozen_blend =
        (blend_version.value() == "9" && blend.value() == kFrozenRetouchBlendParameters[0]) ||
        (blend_version.value() == "10" &&
         std::find(kFrozenRetouchBlendParameters.begin() + 1, kFrozenRetouchBlendParameters.end(),
                   blend.value()) != kFrozenRetouchBlendParameters.end());
    if (!frozen_blend)
    {
        return make_error(
            ErrorCode::kUnsupported,
            "Legacy Retouch blend state is outside the frozen fixture census",
            {{"legacy_operation", "retouch"}, {"reason", "unsupported_legacy_retouch_blend"}});
    }
    if (std::find(kFrozenRetouchParameters.begin(), kFrozenRetouchParameters.end(),
                  parameters.value()) == kFrozenRetouchParameters.end())
    {
        return make_error(
            ErrorCode::kUnsupported,
            "Legacy Retouch parameters are outside the frozen fixture census",
            {{"legacy_operation", "retouch"}, {"reason", "unsupported_legacy_retouch_parameters"}});
    }
    auto decoded = decode_legacy_parameter_blob(parameters.value(), 12056U, "retouch");
    if (!decoded)
    {
        auto error = decoded.error();
        error.context["reason"] = "invalid_legacy_retouch_parameters";
        return error;
    }
    return LegacyRetouchCandidate{attributes, parsed_position.value()};
}

[[nodiscard]] Result<const LegacyMaskRecord *>
latest_mask_record(const std::vector<LegacyMaskRecord> &records, const std::int32_t id,
                   const std::uint64_t history_position)
{
    const LegacyMaskRecord *winner = nullptr;
    for (const auto &record : records)
    {
        if (record.id != id || record.history_position > history_position)
            continue;
        if (winner == nullptr || record.history_position > winner->history_position)
        {
            winner = &record;
            continue;
        }
        if (record.history_position == winner->history_position)
        {
            return make_error(
                ErrorCode::kConflict,
                "Legacy Retouch mask ID is duplicated at one history position",
                {{"mask_id", std::to_string(id)}, {"reason", "duplicate_legacy_retouch_mask"}});
        }
    }
    if (winner == nullptr)
    {
        return make_error(
            ErrorCode::kUnsupported, "Legacy Retouch parameters reference a missing mask",
            {{"mask_id", std::to_string(id)}, {"reason", "missing_legacy_retouch_mask"}});
    }
    return winner;
}

[[nodiscard]] Result<std::array<double, 2>>
decode_legacy_mask_source(const LegacyMaskRecord &record)
{
    auto decoded = decode_legacy_parameter_blob(record.source, 8U, "retouch-mask-source");
    if (!decoded)
        return decoded.error();
    const float x = read_f32(decoded.value(), 0U);
    const float y = read_f32(decoded.value(), 4U);
    if (!std::isfinite(x) || !std::isfinite(y) || x < 0.0F || x > 1.0F || y < 0.0F || y > 1.0F)
    {
        return make_error(
            ErrorCode::kValidation, "Legacy Retouch source point is invalid",
            {{"mask_id", std::to_string(record.id)}, {"reason", "invalid_legacy_retouch_source"}});
    }
    return std::array<double, 2>{static_cast<double>(x), static_cast<double>(y)};
}

[[nodiscard]] Result<double>
legacy_retouch_group_opacity(const std::vector<LegacyMaskRecord> &records,
                             const std::int32_t form_id, const std::uint64_t history_position)
{
    std::optional<std::uint64_t> winning_position;
    std::optional<double> opacity;
    for (const auto &record : records)
    {
        constexpr std::int32_t kGroup = 1 << 2;
        if ((record.type & kGroup) == 0 || record.history_position > history_position)
            continue;
        auto decoded = decode_legacy_parameter_blob(
            record.points, record.point_count * 4U * sizeof(std::int32_t), "retouch-mask-group");
        if (!decoded)
            return decoded.error();
        for (std::size_t index = 0U; index < record.point_count; ++index)
        {
            const std::size_t offset = index * 16U;
            if (read_i32(decoded.value(), offset) != form_id)
                continue;
            const std::int32_t parent = read_i32(decoded.value(), offset + 4U);
            const std::int32_t state = read_i32(decoded.value(), offset + 8U);
            const float value = read_f32(decoded.value(), offset + 12U);
            if (parent != record.id || (state & 1) == 0 || (state & 4) != 0 ||
                !std::isfinite(value) || value < 0.0F || value > 1.0F)
            {
                return make_error(ErrorCode::kUnsupported,
                                  "Legacy Retouch group edge is outside the frozen evidence",
                                  {{"mask_id", std::to_string(form_id)},
                                   {"reason", "unsupported_legacy_retouch_group"}});
            }
            if (!winning_position || record.history_position > *winning_position)
            {
                winning_position = record.history_position;
                opacity = static_cast<double>(value);
            }
            else if (record.history_position == *winning_position)
            {
                return make_error(ErrorCode::kConflict,
                                  "Legacy Retouch mask belongs to multiple groups",
                                  {{"mask_id", std::to_string(form_id)},
                                   {"reason", "duplicate_legacy_retouch_group"}});
            }
        }
    }
    if (!opacity)
    {
        return make_error(
            ErrorCode::kUnsupported, "Legacy Retouch mask has no frozen group edge",
            {{"mask_id", std::to_string(form_id)}, {"reason", "missing_legacy_retouch_group"}});
    }
    return *opacity;
}

[[nodiscard]] Result<Mask> map_legacy_retouch_mask(const LegacyMaskRecord &record)
{
    constexpr std::int32_t kCircle = 1 << 0;
    constexpr std::int32_t kPath = 1 << 1;
    constexpr std::int32_t kGroup = 1 << 2;
    constexpr std::int32_t kClone = 1 << 3;
    constexpr std::int32_t kEllipse = 1 << 5;
    constexpr std::int32_t kBrush = 1 << 6;
    constexpr std::int32_t kNonClone = 1 << 7;
    constexpr std::int32_t kShapeBits = kCircle | kPath | kEllipse | kBrush;
    const std::int32_t shape = record.type & kShapeBits;
    if (record.version != 6 || (record.type & kGroup) != 0 ||
        (record.type & ~(kShapeBits | kClone | kNonClone)) != 0 ||
        (shape != kCircle && shape != kPath && shape != kEllipse && shape != kBrush) ||
        ((record.type & kClone) != 0) == ((record.type & kNonClone) != 0))
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Retouch mask type is outside the frozen shape domain",
                          {{"mask_id", std::to_string(record.id)},
                           {"reason", "unsupported_legacy_retouch_mask_type"}});
    }
    const std::string canonical_id = "legacy-retouch-mask-" + std::to_string(record.id);
    if (shape == kCircle)
    {
        if (record.point_count != 1U)
            return make_error(ErrorCode::kValidation, "Legacy Retouch circle count is invalid",
                              {{"reason", "invalid_legacy_retouch_mask"}});
        auto decoded = decode_legacy_parameter_blob(record.points, 16U, "retouch-circle");
        if (!decoded)
            return decoded.error();
        Mask mask{canonical_id, kCanonicalMaskSchemaVersion, MaskKind::kCircle};
        mask.payload = CircleMask{read_f32(decoded.value(), 0U), read_f32(decoded.value(), 4U),
                                  read_f32(decoded.value(), 8U), read_f32(decoded.value(), 12U)};
        return mask;
    }
    if (shape == kEllipse)
    {
        if (record.point_count != 1U)
            return make_error(ErrorCode::kValidation, "Legacy Retouch ellipse count is invalid",
                              {{"reason", "invalid_legacy_retouch_mask"}});
        auto decoded = decode_legacy_parameter_blob(record.points, 28U, "retouch-ellipse");
        if (!decoded)
            return decoded.error();
        if (read_i32(decoded.value(), 24U) != 0)
        {
            return make_error(ErrorCode::kUnsupported,
                              "Proportional legacy Retouch ellipse feather is unsupported",
                              {{"reason", "unsupported_legacy_retouch_ellipse_feather"}});
        }
        Mask mask{canonical_id, kCanonicalMaskSchemaVersion, MaskKind::kEllipse};
        mask.payload = EllipseMask{read_f32(decoded.value(), 0U),  read_f32(decoded.value(), 4U),
                                   read_f32(decoded.value(), 8U),  read_f32(decoded.value(), 12U),
                                   read_f32(decoded.value(), 16U), read_f32(decoded.value(), 20U)};
        return mask;
    }
    if (shape == kPath)
    {
        auto decoded =
            decode_legacy_parameter_blob(record.points, record.point_count * 36U, "retouch-path");
        if (!decoded)
            return decoded.error();
        PathMask path;
        path.points.reserve(record.point_count);
        std::optional<float> feather;
        for (std::size_t index = 0U; index < record.point_count; ++index)
        {
            const std::size_t offset = index * 36U;
            const float left = read_f32(decoded.value(), offset + 24U);
            const float right = read_f32(decoded.value(), offset + 28U);
            const std::int32_t state = read_i32(decoded.value(), offset + 32U);
            if (state != 1 && state != 2)
                return make_error(ErrorCode::kUnsupported,
                                  "Legacy Retouch path point state is unsupported",
                                  {{"reason", "unsupported_legacy_retouch_path_state"}});
            if (!std::isfinite(left) || left != right || (feather && *feather != left))
                return make_error(ErrorCode::kUnsupported,
                                  "Variable legacy Retouch path feather is unsupported",
                                  {{"reason", "unsupported_legacy_retouch_path_feather"}});
            feather = left;
            path.points.push_back(PathMaskPoint{
                read_f32(decoded.value(), offset), read_f32(decoded.value(), offset + 4U),
                read_f32(decoded.value(), offset + 8U), read_f32(decoded.value(), offset + 12U),
                read_f32(decoded.value(), offset + 16U), read_f32(decoded.value(), offset + 20U)});
        }
        path.feather = feather.value_or(0.0F);
        Mask mask{canonical_id, kCanonicalMaskSchemaVersion, MaskKind::kPath};
        mask.payload = std::move(path);
        return mask;
    }
    auto decoded =
        decode_legacy_parameter_blob(record.points, record.point_count * 44U, "retouch-brush");
    if (!decoded)
        return decoded.error();
    BrushMask brush;
    brush.points.reserve(record.point_count);
    for (std::size_t index = 0U; index < record.point_count; ++index)
    {
        const std::size_t offset = index * 44U;
        const float radius0 = read_f32(decoded.value(), offset + 24U);
        const float radius1 = read_f32(decoded.value(), offset + 28U);
        const std::int32_t state = read_i32(decoded.value(), offset + 40U);
        if (radius0 != radius1 || (state != 1 && state != 2))
            return make_error(ErrorCode::kUnsupported,
                              "Legacy Retouch brush has asymmetric radius or unknown point state",
                              {{"reason", "unsupported_legacy_retouch_brush_point"}});
        brush.points.push_back(BrushMaskPoint{
            read_f32(decoded.value(), offset), read_f32(decoded.value(), offset + 4U),
            read_f32(decoded.value(), offset + 8U), read_f32(decoded.value(), offset + 12U),
            read_f32(decoded.value(), offset + 16U), read_f32(decoded.value(), offset + 20U),
            radius0, read_f32(decoded.value(), offset + 36U),
            read_f32(decoded.value(), offset + 32U)});
    }
    Mask mask{canonical_id, kCanonicalMaskSchemaVersion, MaskKind::kBrush};
    mask.payload = std::move(brush);
    return mask;
}

[[nodiscard]] Result<LegacyRetouchMapping>
map_retouch_candidate(const LegacyRetouchCandidate &candidate,
                      const std::vector<LegacyMaskRecord> &mask_records)
{
    const auto encoded = required_attribute(candidate.attributes, u"params", "retouch");
    if (!encoded)
        return encoded.error();
    auto decoded = decode_legacy_parameter_blob(encoded.value(), 12056U, "retouch");
    if (!decoded)
        return decoded.error();
    RetouchParams params;
    params.num_scales = read_i32(decoded.value(), 12004U);
    params.merge_from_scale = read_i32(decoded.value(), 12012U);
    params.max_heal_iterations = kRetouchDefaultHealIterations;
    if (params.num_scales < 0 || params.num_scales > kRetouchMaxScales ||
        params.merge_from_scale < 0 || params.merge_from_scale > params.num_scales)
    {
        return make_error(ErrorCode::kValidation, "Legacy Retouch wavelet state is invalid",
                          {{"reason", "invalid_legacy_retouch_wavelet_state"}});
    }
    LegacyRetouchMapping mapping;
    for (std::size_t index = 0U; index < 300U; ++index)
    {
        const std::size_t offset = index * 40U;
        const std::int32_t form_id = read_i32(decoded.value(), offset);
        if (form_id == 0)
            continue;
        const std::int32_t scale = read_i32(decoded.value(), offset + 4U);
        const std::int32_t algorithm = read_i32(decoded.value(), offset + 8U);
        const std::int32_t blur_type = read_i32(decoded.value(), offset + 12U);
        const float blur_radius = read_f32(decoded.value(), offset + 16U);
        const std::int32_t fill_mode = read_i32(decoded.value(), offset + 20U);
        if (scale < 0 || scale > params.num_scales + 1 || algorithm < 1 || algorithm > 4 ||
            (blur_type != 0 && blur_type != 1) || (fill_mode != 0 && fill_mode != 1))
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy Retouch form state is outside the frozen algorithm domain",
                              {{"mask_id", std::to_string(form_id)},
                               {"reason", "unsupported_legacy_retouch_form"}});
        }
        auto record = latest_mask_record(mask_records, form_id, candidate.history_position);
        if (!record)
            return record.error();
        auto mask = map_legacy_retouch_mask(*record.value());
        if (!mask)
            return mask.error();
        const bool expects_clone = algorithm == 1 || algorithm == 2;
        constexpr std::int32_t kClone = 1 << 3;
        if (((record.value()->type & kClone) != 0) != expects_clone)
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy Retouch algorithm and mask source kind disagree",
                              {{"mask_id", std::to_string(form_id)},
                               {"reason", "unsupported_legacy_retouch_source_kind"}});
        }
        auto source = decode_legacy_mask_source(*record.value());
        auto opacity =
            legacy_retouch_group_opacity(mask_records, form_id, candidate.history_position);
        if (!source || !opacity)
            return !source ? source.error() : opacity.error();
        RetouchMode mode = RetouchMode::kClone;
        if (algorithm == 2)
            mode = RetouchMode::kHeal;
        else if (algorithm == 3)
            mode = RetouchMode::kBlur;
        else if (algorithm == 4)
            mode = RetouchMode::kFill;
        RetouchRegion region;
        region.mask_id = mask.value().id;
        region.mode = mode;
        region.scale = scale;
        region.opacity = opacity.value();
        region.source_x = source.value()[0];
        region.source_y = source.value()[1];
        region.blur_type =
            blur_type == 0 ? RetouchBlurType::kGaussian : RetouchBlurType::kBilateral;
        region.blur_radius =
            algorithm == 3 ? static_cast<double>(blur_radius) : RetouchRegion{}.blur_radius;
        region.fill_mode = fill_mode == 0 ? RetouchFillMode::kErase : RetouchFillMode::kColor;
        region.fill_color = {read_f32(decoded.value(), offset + 24U),
                             read_f32(decoded.value(), offset + 28U),
                             read_f32(decoded.value(), offset + 32U)};
        region.fill_brightness = read_f32(decoded.value(), offset + 36U);
        params.regions.push_back(std::move(region));
        mapping.masks.push_back(std::move(mask).value());
    }
    mapping.operation =
        OperationInstance{std::string(kRetouchOperationId),
                          kRetouchOperationSchemaVersion,
                          "legacy-retouch-" + std::to_string(candidate.history_position),
                          true,
                          retouch_to_parameters(params),
                          std::nullopt};
    auto valid = validate_retouch_operation(mapping.operation, mapping.masks);
    if (!valid)
        return valid.error();
    return mapping;
}

[[nodiscard]] Result<std::int32_t> legacy_mask_integer(const std::string_view value,
                                                       const std::string_view attribute)
{
    std::int32_t parsed = 0;
    const auto [position, error] =
        std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (value.empty() || error != std::errc{} || position != value.data() + value.size())
    {
        return make_error(
            ErrorCode::kValidation, "Legacy mask integer is invalid",
            {{"attribute", std::string(attribute)}, {"reason", "invalid_legacy_retouch_mask"}});
    }
    return parsed;
}

[[nodiscard]] Result<std::vector<LegacyMaskRecord>>
parse_legacy_mask_history(QXmlStreamReader &reader)
{
    std::vector<LegacyMaskRecord> records;
    std::size_t depth = 1;
    while (depth > 0 && !reader.atEnd())
    {
        reader.readNext();
        if (reader.isStartElement())
        {
            ++depth;
            if (reader.name() == u"li")
            {
                for (const auto &attribute : reader.attributes())
                {
                    const auto name = attribute.name();
                    const bool allowed = name == u"mask_num" || name == u"mask_id" ||
                                         name == u"mask_type" || name == u"mask_name" ||
                                         name == u"mask_version" || name == u"mask_points" ||
                                         name == u"mask_nb" || name == u"mask_src";
                    if (!allowed || attribute.namespaceUri() != u"http://darktable.sf.net/")
                    {
                        return make_error(
                            ErrorCode::kUnsupported, "Legacy mask contains unproven history state",
                            {{"attribute", utf8(name)},
                             {"reason", "unsupported_legacy_retouch_mask_attribute"}});
                    }
                }
                const auto number = required_attribute(reader.attributes(), u"mask_num", "mask");
                const auto id = required_attribute(reader.attributes(), u"mask_id", "mask");
                const auto type = required_attribute(reader.attributes(), u"mask_type", "mask");
                const auto version =
                    required_attribute(reader.attributes(), u"mask_version", "mask");
                const auto points = required_attribute(reader.attributes(), u"mask_points", "mask");
                const auto count = required_attribute(reader.attributes(), u"mask_nb", "mask");
                const auto source = required_attribute(reader.attributes(), u"mask_src", "mask");
                if (!number || !id || !type || !version || !points || !count || !source)
                {
                    return !number  ? number.error() :
                           !id      ? id.error() :
                           !type    ? type.error() :
                           !version ? version.error() :
                           !points  ? points.error() :
                           !count   ? count.error() :
                                      source.error();
                }
                auto parsed_number = legacy_history_position(number.value(), "mask_num", "retouch",
                                                             "invalid_legacy_retouch_mask");
                auto parsed_id = legacy_mask_integer(id.value(), "mask_id");
                auto parsed_type = legacy_mask_integer(type.value(), "mask_type");
                auto parsed_version = legacy_mask_integer(version.value(), "mask_version");
                auto parsed_count = legacy_history_position(count.value(), "mask_nb", "retouch",
                                                            "invalid_legacy_retouch_mask");
                if (!parsed_number || !parsed_id || !parsed_type || !parsed_version ||
                    !parsed_count)
                {
                    return !parsed_number  ? parsed_number.error() :
                           !parsed_id      ? parsed_id.error() :
                           !parsed_type    ? parsed_type.error() :
                           !parsed_version ? parsed_version.error() :
                                             parsed_count.error();
                }
                if (parsed_id.value() <= 0 || parsed_type.value() <= 0 ||
                    parsed_version.value() != 6 || parsed_count.value() == 0U ||
                    parsed_count.value() > kCanonicalMaskMaxTessellatedSamples)
                {
                    return make_error(
                        ErrorCode::kUnsupported,
                        "Legacy mask is outside the frozen Retouch evidence",
                        {{"mask_id", id.value()}, {"reason", "unsupported_legacy_retouch_mask"}});
                }
                records.push_back(LegacyMaskRecord{parsed_number.value(), parsed_id.value(),
                                                   parsed_type.value(), parsed_version.value(),
                                                   static_cast<std::size_t>(parsed_count.value()),
                                                   points.value(), source.value()});
            }
        }
        else if (reader.isEndElement())
        {
            --depth;
        }
    }
    if (reader.hasError())
    {
        return make_error(ErrorCode::kValidation, "Legacy mask history is not well-formed XML",
                          {{"reason", "invalid_legacy_retouch_mask"}});
    }
    return records;
}

} // namespace ravo::legacy_xmp_internal
