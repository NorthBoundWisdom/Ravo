#include "ravo/adapters/legacy_xmp.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QXmlStreamReader>

#include <zlib.h>

namespace ravo
{

namespace
{

[[nodiscard]] std::string utf8(const QStringView value)
{
    return value.toString().toUtf8().toStdString();
}

[[nodiscard]] Result<void> validate_asset(const AssetDescriptor &asset)
{
    if (asset.id.empty() || asset.input_uri.empty())
    {
        return make_error(ErrorCode::kValidation,
                          "Legacy XMP import requires an explicit asset ID and input URI");
    }
    return {};
}

[[nodiscard]] Result<std::string> required_attribute(const QXmlStreamAttributes &attributes,
                                                     const QStringView name,
                                                     const std::string_view operation)
{
    for (const auto &attribute : attributes)
    {
        if (attribute.name() == name && !attribute.value().isEmpty())
        {
            return utf8(attribute.value());
        }
    }
    return make_error(ErrorCode::kUnsupported,
                      "Legacy XMP operation is missing a required attribute",
                      {{"attribute", utf8(name)},
                       {"legacy_operation", std::string(operation)},
                       {"reason", "unsupported_legacy_operation"}});
}

[[nodiscard]] std::optional<std::string> attribute_value(const QXmlStreamAttributes &attributes,
                                                         const QStringView name)
{
    for (const auto &attribute : attributes)
    {
        if (attribute.name() == name)
        {
            return utf8(attribute.value());
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool has_attribute(const QXmlStreamAttributes &attributes, const QStringView name)
{
    for (const auto &attribute : attributes)
    {
        if (attribute.name() == name)
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool has_supported_xmp_schema(const QXmlStreamAttributes &attributes)
{
    for (const auto &attribute : attributes)
    {
        if (attribute.name() == u"xmp_version")
        {
            return attribute.value() == u"6";
        }
    }
    return false;
}

[[nodiscard]] int hex_value(const char value) noexcept
{
    if (value >= '0' && value <= '9')
    {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f')
    {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F')
    {
        return value - 'A' + 10;
    }
    return -1;
}

[[nodiscard]] std::int32_t read_i32(const std::vector<std::uint8_t> &data,
                                    const std::size_t offset) noexcept
{
    const std::uint32_t value = static_cast<std::uint32_t>(data[offset]) |
                                (static_cast<std::uint32_t>(data[offset + 1U]) << 8U) |
                                (static_cast<std::uint32_t>(data[offset + 2U]) << 16U) |
                                (static_cast<std::uint32_t>(data[offset + 3U]) << 24U);
    return std::bit_cast<std::int32_t>(value);
}

[[nodiscard]] float read_f32(const std::vector<std::uint8_t> &data,
                             const std::size_t offset) noexcept
{
    const std::uint32_t value = static_cast<std::uint32_t>(data[offset]) |
                                (static_cast<std::uint32_t>(data[offset + 1U]) << 8U) |
                                (static_cast<std::uint32_t>(data[offset + 2U]) << 16U) |
                                (static_cast<std::uint32_t>(data[offset + 3U]) << 24U);
    return std::bit_cast<float>(value);
}

[[nodiscard]] Result<std::vector<std::uint8_t>>
decode_legacy_parameter_blob(const std::string_view encoded, const std::size_t expected_size,
                             const std::string_view operation)
{
    if (!encoded.starts_with("gz"))
    {
        if (encoded.size() != expected_size * 2U)
        {
            return make_error(ErrorCode::kValidation,
                              "Legacy parameters have an unexpected hexadecimal length",
                              {{"legacy_operation", std::string(operation)}});
        }
        std::vector<std::uint8_t> decoded(expected_size);
        for (std::size_t index = 0; index < decoded.size(); ++index)
        {
            const int high = hex_value(encoded[index * 2U]);
            const int low = hex_value(encoded[index * 2U + 1U]);
            if (high < 0 || low < 0)
            {
                return make_error(ErrorCode::kValidation,
                                  "Legacy parameters contain invalid hexadecimal data",
                                  {{"legacy_operation", std::string(operation)}});
            }
            decoded[index] = static_cast<std::uint8_t>((high << 4) | low);
        }
        return decoded;
    }
    if (encoded.size() < 5U)
    {
        return make_error(ErrorCode::kValidation, "Legacy compressed parameters are truncated",
                          {{"legacy_operation", std::string(operation)}});
    }
    const QByteArray base64(encoded.data() + 4, static_cast<qsizetype>(encoded.size() - 4U));
    const QByteArray compressed =
        QByteArray::fromBase64(base64, QByteArray::AbortOnBase64DecodingErrors);
    if (compressed.isEmpty())
    {
        return make_error(ErrorCode::kValidation, "Legacy parameters contain invalid base64",
                          {{"legacy_operation", std::string(operation)}});
    }
    std::vector<std::uint8_t> decoded(expected_size);
    uLongf decoded_size = static_cast<uLongf>(decoded.size());
    const int status = uncompress(decoded.data(), &decoded_size,
                                  reinterpret_cast<const Bytef *>(compressed.constData()),
                                  static_cast<uLong>(compressed.size()));
    if (status != Z_OK || decoded_size != decoded.size())
    {
        return make_error(ErrorCode::kValidation,
                          "Legacy compressed parameters have an unexpected payload",
                          {{"legacy_operation", std::string(operation)}});
    }
    return decoded;
}

[[nodiscard]] Result<std::string> color_profile_name(const std::int32_t type)
{
    switch (type)
    {
    case 0:
        return std::string(kInputProfileFileIcc);
    case 1:
        return std::string(kInputProfileSrgb);
    case 2:
        return std::string(kInputProfileAdobeRgb);
    case 3:
        return std::string(kInputProfileLinearRec709);
    case 4:
        return std::string(kInputProfileLinearRec2020);
    case 5:
        return std::string(kInputProfileXyz);
    case 6:
        return std::string(kInputProfileLab);
    case 9:
        return std::string(kInputProfileEmbeddedIcc);
    case 10:
        return std::string(kInputProfileEmbeddedMatrix);
    case 11:
        return std::string(kInputProfileStandardMatrix);
    case 12:
        return std::string(kInputProfileEnhancedMatrix);
    case 13:
        return std::string(kInputProfileVendorMatrix);
    case 14:
        return std::string(kInputProfileAlternateMatrix);
    case 20:
        return std::string(kInputProfileRec709);
    case 21:
        return std::string(kInputProfileProPhotoRgb);
    case 22:
        return std::string(kInputProfilePqRec2020);
    case 23:
        return std::string(kInputProfileHlgRec2020);
    case 24:
        return std::string(kInputProfilePqP3);
    case 25:
        return std::string(kInputProfileHlgP3);
    case 26:
        return std::string(kInputProfileDisplayP3);
    default:
        return make_error(ErrorCode::kUnsupported, "Legacy colour profile type is unsupported",
                          {{"legacy_profile_type", std::to_string(type)}});
    }
}

[[nodiscard]] Result<std::string> fixed_string(const std::vector<std::uint8_t> &data,
                                               const std::size_t offset, const std::size_t capacity)
{
    const auto begin = data.begin() + static_cast<std::ptrdiff_t>(offset);
    const auto end = begin + static_cast<std::ptrdiff_t>(capacity);
    const auto terminator = std::find(begin, end, std::uint8_t{0});
    if (terminator == end)
    {
        return make_error(ErrorCode::kValidation,
                          "Legacy colour profile filename is not terminated");
    }
    const QByteArray bytes(reinterpret_cast<const char *>(&*begin),
                           static_cast<qsizetype>(std::distance(begin, terminator)));
    const QString text = QString::fromUtf8(bytes);
    if (text.toUtf8() != bytes)
    {
        return make_error(ErrorCode::kValidation,
                          "Legacy colour profile filename is not valid UTF-8");
    }
    return text.toUtf8().toStdString();
}

[[nodiscard]] Result<std::array<std::uint8_t, 20>>
decode_exposure_v5_parameters(const std::string_view encoded)
{
    if (encoded.size() != 40U)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy exposure v5 parameters have an unexpected length",
                          {{"expected_hex_bytes", "40"},
                           {"legacy_operation", "exposure"},
                           {"reason", "unsupported_legacy_exposure_parameters"}});
    }

    std::array<std::uint8_t, 20> decoded{};
    for (std::size_t index = 0; index < decoded.size(); ++index)
    {
        const auto high = hex_value(encoded[index * 2U]);
        const auto low = hex_value(encoded[index * 2U + 1U]);
        if (high < 0 || low < 0)
        {
            return make_error(
                ErrorCode::kValidation, "Legacy exposure parameters are not hexadecimal",
                {{"legacy_operation", "exposure"}, {"reason", "invalid_legacy_parameters"}});
        }
        decoded[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return decoded;
}

[[nodiscard]] std::uint32_t read_little_endian_word(const std::array<std::uint8_t, 20> &data,
                                                    const std::size_t word_index) noexcept
{
    const auto offset = word_index * 4U;
    return static_cast<std::uint32_t>(data[offset]) |
           (static_cast<std::uint32_t>(data[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(data[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(data[offset + 3U]) << 24U);
}

[[nodiscard]] Result<OperationInstance> map_exposure_v5(const QXmlStreamAttributes &attributes,
                                                        const std::size_t history_index)
{
    const auto version = required_attribute(attributes, u"modversion", "exposure");
    if (!version)
    {
        return version.error();
    }
    if (version.value() != "5")
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy exposure module version is not supported",
                          {{"legacy_operation", "exposure"},
                           {"legacy_version", version.value()},
                           {"reason", "unsupported_legacy_exposure_version"}});
    }

    const auto enabled = required_attribute(attributes, u"enabled", "exposure");
    if (!enabled)
    {
        return enabled.error();
    }
    if (enabled.value() != "0" && enabled.value() != "1")
    {
        return make_error(
            ErrorCode::kValidation, "Legacy exposure enabled flag is invalid",
            {{"legacy_operation", "exposure"}, {"reason", "invalid_legacy_parameters"}});
    }
    if (has_attribute(attributes, u"blendop_params"))
    {
        return make_error(
            ErrorCode::kUnsupported, "Legacy exposure blend data has no canonical mask mapping",
            {{"legacy_operation", "exposure"}, {"reason", "unsupported_legacy_blend"}});
    }

    const auto encoded = required_attribute(attributes, u"params", "exposure");
    if (!encoded)
    {
        return encoded.error();
    }
    const auto decoded = decode_exposure_v5_parameters(encoded.value());
    if (!decoded)
    {
        return decoded.error();
    }

    const auto mode = read_little_endian_word(decoded.value(), 0U);
    const auto black_bits = read_little_endian_word(decoded.value(), 1U);
    const auto exposure = std::bit_cast<float>(read_little_endian_word(decoded.value(), 2U));
    if (mode != 0U)
    {
        return make_error(
            ErrorCode::kUnsupported, "Legacy automatic exposure mode requires a histogram contract",
            {{"legacy_operation", "exposure"}, {"reason", "unsupported_legacy_exposure_mode"}});
    }
    if (black_bits != 0U)
    {
        return make_error(
            ErrorCode::kUnsupported,
            "Legacy exposure black-level correction has no canonical mapping",
            {{"legacy_operation", "exposure"}, {"reason", "unsupported_legacy_exposure_black"}});
    }
    if (!std::isfinite(exposure) || exposure < -10.0F || exposure > 10.0F)
    {
        return make_error(
            ErrorCode::kUnsupported, "Legacy exposure value is outside the Ravo schema range",
            {{"legacy_operation", "exposure"}, {"reason", "unsupported_legacy_exposure_value"}});
    }

    return OperationInstance{"ravo.core.exposure",
                             1,
                             "legacy-exposure-" + std::to_string(history_index),
                             enabled.value() == "1",
                             {{"exposure_ev", ParameterValue{static_cast<double>(exposure)}}},
                             std::nullopt};
}

struct BuiltinRawOperation
{
    std::string_view id;
    std::string_view version;
    std::string_view parameters;
};

constexpr std::array kBuiltinRawOperations{
    BuiltinRawOperation{"rawprepare", "1",
                        "1e000000120000000600000002000000060406040204020420350000"},
    BuiltinRawOperation{"temperature", "3", "006007400000803f0000b33f0000c07f"},
    BuiltinRawOperation{"highlights", "2", "000000000000803f00000000000000000000803f"},
    BuiltinRawOperation{"demosaic", "3", "0000000000000000000000000000000000000000"},
    BuiltinRawOperation{"flip", "2", "ffffffff"},
};

constexpr std::string_view kDefaultBlendParameters =
    "gz11eJxjYGBgkGAAgRNODGiAEV0AJ2iwh+CRyscOAAdeGQQ=";
constexpr std::string_view kPrimariesDefaultBlendParameters =
    "gz09eJxjYGBgYAFiCQYYOOHEgAZY0QVwggZ7CB6pfOygYtaVAyCMi48L/AcCEA0AmawnoA==";

struct LegacyGammaBlendTuple
{
    std::string_view version;
    std::string_view parameters;
};

constexpr std::string_view kGammaBlendGz14GuideOne =
    "gz14eJxjYIAACQYYOOHEgAYY0QVwggZ7CB6pfNoAAEkgGQQ=";
constexpr std::string_view kGammaBlendGz14GuideFive =
    "gz14eJxjYIAACQYYOOHEgAZY0QVwggZ7CB6pfNoAAE8gGQg=";
constexpr std::string_view kGammaBlendGz12GuideOne =
    "gz12eJxjYIAACQYYOOHEgAYY0QVwggZ7CB6pfOqC/0AAogFjBh0A";
constexpr std::string_view kGammaBlendGz12GuideFive =
    "gz12eJxjYIAACQYYOOHEgAZY0QVwggZ7CB6pfOqC/0AAogFpBh0E";
constexpr std::string_view kGammaBlendGz11FeatherV1 =
    "gz11eJxjYIAACQYYOOHEgAZY0QWAgBGLGANDgz0Ej1Q+dcF/IADRAGpyHQU=";
constexpr std::string_view kGammaBlendV11UncompressedGuideFive =
    "000000000000000018000000000000000000c84200000000000000000000000000000000050000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000803f0000803f00000000000000000000803f"
    "0000803f00000000000000000000803f0000803f00000000000000000000803f0000803f00000000000000000000803f"
    "0000803f00000000000000000000803f0000803f00000000000000000000803f0000803f00000000000000000000803f"
    "0000803f00000000000000000000803f0000803f00000000000000000000803f0000803f00000000000000000000803f"
    "0000803f00000000000000000000803f0000803f00000000000000000000803f0000803f00000000000000000000803f"
    "0000803f00000000000000000000803f0000803f00000000000000000000803f0000803f000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000";

constexpr std::array kLegacyGammaBlendTuples{
    LegacyGammaBlendTuple{"9", kDefaultBlendParameters},
    LegacyGammaBlendTuple{"10", kGammaBlendGz14GuideOne},
    LegacyGammaBlendTuple{"11", kGammaBlendV11UncompressedGuideFive},
    LegacyGammaBlendTuple{"11", kGammaBlendGz14GuideOne},
    LegacyGammaBlendTuple{"11", kGammaBlendGz14GuideFive},
    LegacyGammaBlendTuple{"12", kGammaBlendGz12GuideFive},
    LegacyGammaBlendTuple{"12", kGammaBlendGz14GuideOne},
    LegacyGammaBlendTuple{"12", kGammaBlendGz14GuideFive},
    LegacyGammaBlendTuple{"13", kGammaBlendGz11FeatherV1},
    LegacyGammaBlendTuple{"13", kGammaBlendGz12GuideOne},
    LegacyGammaBlendTuple{"13", kGammaBlendGz12GuideFive},
    LegacyGammaBlendTuple{"14", kGammaBlendGz11FeatherV1},
};

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

    const auto version = required_attribute(attributes, u"modversion", operation);
    const auto enabled = required_attribute(attributes, u"enabled", operation);
    const auto parameters = required_attribute(attributes, u"params", operation);
    const auto blend = required_attribute(attributes, u"blendop_params", operation);
    if (!version || !enabled || !parameters || !blend)
    {
        return !version    ? version.error() :
               !enabled    ? enabled.error() :
               !parameters ? parameters.error() :
                             blend.error();
    }
    if (version.value() != contract->version || enabled.value() != "1" ||
        parameters.value() != contract->parameters || blend.value() != kDefaultBlendParameters)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy built-in RAW operation differs from the frozen nop contract",
                          {{"legacy_operation", std::string(operation)},
                           {"reason", "unsupported_legacy_builtin_parameters"}});
    }
    return true;
}

[[nodiscard]] Result<void> consume_empty_mask_history(QXmlStreamReader &reader)
{
    std::size_t depth = 1;
    while (depth > 0 && !reader.atEnd())
    {
        reader.readNext();
        if (reader.isStartElement())
        {
            ++depth;
            if (reader.name() == u"li")
            {
                return make_error(ErrorCode::kUnsupported,
                                  "Legacy XMP mask history has no canonical mask mapping",
                                  {{"reason", "unsupported_legacy_mask"}});
            }
        }
        else if (reader.isEndElement())
        {
            --depth;
        }
    }
    return {};
}

} // namespace

Result<InputColorParams> decode_legacy_colorin_parameters(const std::string_view encoded_parameters)
{
    auto decoded = decode_legacy_parameter_blob(encoded_parameters, 1044U, "colorin");
    if (!decoded)
    {
        return decoded.error();
    }
    auto input_profile = color_profile_name(read_i32(decoded.value(), 0U));
    auto input_filename = fixed_string(decoded.value(), 4U, 512U);
    auto working_profile = color_profile_name(read_i32(decoded.value(), 528U));
    auto working_filename = fixed_string(decoded.value(), 532U, 512U);
    if (!input_profile || !input_filename || !working_profile || !working_filename)
    {
        return !input_profile   ? input_profile.error() :
               !input_filename  ? input_filename.error() :
               !working_profile ? working_profile.error() :
                                  working_filename.error();
    }

    InputColorParams result;
    result.input_profile = std::move(input_profile).value();
    result.input_profile_filename = std::move(input_filename).value();
    result.working_profile = std::move(working_profile).value();
    result.working_profile_filename = std::move(working_filename).value();
    switch (read_i32(decoded.value(), 516U))
    {
    case 0:
        result.rendering_intent = std::string(kColorIntentPerceptual);
        break;
    case 1:
        result.rendering_intent = std::string(kColorIntentRelative);
        break;
    case 2:
        result.rendering_intent = std::string(kColorIntentSaturation);
        break;
    case 3:
        result.rendering_intent = std::string(kColorIntentAbsolute);
        break;
    default:
        return make_error(ErrorCode::kUnsupported,
                          "Legacy colorin rendering intent is unsupported");
    }
    switch (read_i32(decoded.value(), 520U))
    {
    case 0:
        result.gamut_normalize = std::string(kColorNormalizeOff);
        break;
    case 1:
        result.gamut_normalize = std::string(kColorNormalizeSrgb);
        break;
    case 2:
        result.gamut_normalize = std::string(kColorNormalizeAdobeRgb);
        break;
    case 3:
        result.gamut_normalize = std::string(kColorNormalizeLinearRec709);
        break;
    case 4:
        result.gamut_normalize = std::string(kColorNormalizeLinearRec2020);
        break;
    default:
        return make_error(ErrorCode::kUnsupported,
                          "Legacy colorin gamut normalization is unsupported");
    }
    const auto blue_mapping = read_i32(decoded.value(), 524U);
    if (blue_mapping != 0 && blue_mapping != 1)
    {
        return make_error(ErrorCode::kValidation, "Legacy colorin blue-mapping flag is invalid");
    }
    result.blue_mapping = blue_mapping == 1;
    auto valid = validate_input_color_parameters(input_color_to_parameters(result));
    if (!valid)
    {
        return valid.error();
    }
    return result;
}

Result<OutputColorParams>
decode_legacy_colorout_parameters(const std::string_view encoded_parameters)
{
    auto decoded = decode_legacy_parameter_blob(encoded_parameters, 520U, "colorout");
    if (!decoded)
    {
        return decoded.error();
    }
    auto output_profile = color_profile_name(read_i32(decoded.value(), 0U));
    auto output_filename = fixed_string(decoded.value(), 4U, 512U);
    if (!output_profile || !output_filename)
    {
        return !output_profile ? output_profile.error() : output_filename.error();
    }
    OutputColorParams result;
    result.output_profile = std::move(output_profile).value();
    result.output_profile_filename = std::move(output_filename).value();
    switch (read_i32(decoded.value(), 516U))
    {
    case 0:
        result.rendering_intent = std::string(kColorIntentPerceptual);
        break;
    case 1:
        result.rendering_intent = std::string(kColorIntentRelative);
        break;
    case 2:
        result.rendering_intent = std::string(kColorIntentSaturation);
        break;
    case 3:
        result.rendering_intent = std::string(kColorIntentAbsolute);
        break;
    default:
        return make_error(ErrorCode::kUnsupported,
                          "Legacy colorout rendering intent is unsupported");
    }
    auto valid = validate_output_color_parameters(output_color_to_parameters(result));
    if (!valid)
    {
        return valid.error();
    }
    return result;
}

Result<PrimariesParams>
decode_legacy_primaries_v1_parameters(const std::string_view encoded_parameters)
{
    auto decoded = decode_legacy_parameter_blob(encoded_parameters, 32U, "primaries");
    if (!decoded)
    {
        return decoded.error();
    }

    PrimariesParams result{
        static_cast<double>(read_f32(decoded.value(), 0U)),
        static_cast<double>(read_f32(decoded.value(), 4U)),
        static_cast<double>(read_f32(decoded.value(), 8U)),
        static_cast<double>(read_f32(decoded.value(), 12U)),
        static_cast<double>(read_f32(decoded.value(), 16U)),
        static_cast<double>(read_f32(decoded.value(), 20U)),
        static_cast<double>(read_f32(decoded.value(), 24U)),
        static_cast<double>(read_f32(decoded.value(), 28U)),
    };
    auto valid = validate_primaries_parameters(primaries_to_parameters(result));
    if (!valid)
    {
        return valid.error();
    }
    return result;
}

Result<Recipe> import_legacy_xmp(const LegacyXmpImportRequest &request)
{
    auto valid_asset = validate_asset(request.asset);
    if (!valid_asset)
    {
        return valid_asset.error();
    }
    if (request.xmp_utf8.size() > static_cast<std::size_t>(std::numeric_limits<qsizetype>::max()))
    {
        return make_error(ErrorCode::kValidation, "Legacy XMP document is too large");
    }

    const QByteArray source(request.xmp_utf8.data(),
                            static_cast<qsizetype>(request.xmp_utf8.size()));
    QXmlStreamReader reader(source);
    bool found_description = false;
    bool in_history = false;
    bool has_supported_schema = false;
    std::vector<OperationInstance> operations;
    std::optional<OperationInstance> input_color;
    std::optional<OperationInstance> output_color;
    std::optional<OperationInstance> primaries;
    bool absorbed_gamma = false;
    std::size_t history_index = 0;
    while (!reader.atEnd())
    {
        reader.readNext();
        if (reader.isStartElement())
        {
            if (reader.name() == u"Description")
            {
                found_description = true;
                has_supported_schema = has_supported_xmp_schema(reader.attributes());
            }
            if (reader.name() == u"masks_history")
            {
                auto masks = consume_empty_mask_history(reader);
                if (!masks)
                {
                    return masks.error();
                }
                continue;
            }
            if (reader.name() == u"history")
            {
                in_history = true;
                continue;
            }
            if (!in_history || reader.name() != u"li")
            {
                continue;
            }
            const auto operation = required_attribute(reader.attributes(), u"operation", "unknown");
            if (!operation)
            {
                return operation.error();
            }
            if (!has_supported_schema)
            {
                return make_error(ErrorCode::kUnsupported,
                                  "Legacy XMP schema has no proven canonical recipe mapping",
                                  {{"legacy_operation", operation.value()},
                                   {"reason", "unsupported_legacy_xmp_schema"}});
            }
            if (operation.value() == "colorin")
            {
                const auto version =
                    required_attribute(reader.attributes(), u"modversion", "colorin");
                const auto enabled = required_attribute(reader.attributes(), u"enabled", "colorin");
                const auto parameters =
                    required_attribute(reader.attributes(), u"params", "colorin");
                const auto blend =
                    required_attribute(reader.attributes(), u"blendop_params", "colorin");
                if (!version || !enabled || !parameters || !blend)
                {
                    return !version    ? version.error() :
                           !enabled    ? enabled.error() :
                           !parameters ? parameters.error() :
                                         blend.error();
                }
                if ((version.value() != "6" && version.value() != "7") ||
                    (enabled.value() != "0" && enabled.value() != "1"))
                {
                    return make_error(ErrorCode::kUnsupported,
                                      "Legacy colorin version or enabled state is unsupported",
                                      {{"legacy_version", version.value()}});
                }
                if (blend.value() != kDefaultBlendParameters &&
                    blend.value() != "gz14eJxjYIAACQYYOOHEgAYY0QVwggZ7CB6pfNoAAEkgGQQ=")
                {
                    return make_error(ErrorCode::kUnsupported,
                                      "Legacy colorin blend data is unsupported",
                                      {{"reason", "unsupported_legacy_blend"}});
                }
                if (enabled.value() == "1")
                {
                    auto decoded = decode_legacy_colorin_parameters(parameters.value());
                    if (!decoded)
                    {
                        return decoded.error();
                    }
                    input_color =
                        OperationInstance{"ravo.color.input",
                                          1,
                                          "legacy-colorin-" + std::to_string(history_index),
                                          true,
                                          input_color_to_parameters(decoded.value()),
                                          std::nullopt};
                }
                ++history_index;
                continue;
            }
            if (operation.value() == "colorout")
            {
                const auto version =
                    required_attribute(reader.attributes(), u"modversion", "colorout");
                const auto enabled =
                    required_attribute(reader.attributes(), u"enabled", "colorout");
                const auto parameters =
                    required_attribute(reader.attributes(), u"params", "colorout");
                const auto blend =
                    required_attribute(reader.attributes(), u"blendop_params", "colorout");
                if (!version || !enabled || !parameters || !blend)
                {
                    return !version    ? version.error() :
                           !enabled    ? enabled.error() :
                           !parameters ? parameters.error() :
                                         blend.error();
                }
                if (version.value() != "5" || (enabled.value() != "0" && enabled.value() != "1"))
                {
                    return make_error(ErrorCode::kUnsupported,
                                      "Legacy colorout version or enabled state is unsupported",
                                      {{"legacy_version", version.value()}});
                }
                if (blend.value() != kDefaultBlendParameters &&
                    blend.value() != "gz14eJxjYIAACQYYOOHEgAYY0QVwggZ7CB6pfNoAAEkgGQQ=")
                {
                    return make_error(ErrorCode::kUnsupported,
                                      "Legacy colorout blend data is unsupported",
                                      {{"reason", "unsupported_legacy_blend"}});
                }
                if (enabled.value() == "1")
                {
                    auto decoded = decode_legacy_colorout_parameters(parameters.value());
                    if (!decoded)
                    {
                        return decoded.error();
                    }
                    output_color =
                        OperationInstance{"ravo.color.output",
                                          1,
                                          "legacy-colorout-" + std::to_string(history_index),
                                          true,
                                          output_color_to_parameters(decoded.value()),
                                          std::nullopt};
                }
                ++history_index;
                continue;
            }
            if (operation.value() == "profile_gamma")
            {
                return make_error(
                    ErrorCode::kUnsupported,
                    "Legacy profile gamma has no frozen parameter fixture for canonical import",
                    {{"legacy_operation", "profile_gamma"},
                     {"reason", "unsupported_legacy_profile_gamma_no_fixture"}});
            }
            if (operation.value() == "gamma")
            {
                if (absorbed_gamma)
                {
                    return make_error(
                        ErrorCode::kConflict,
                        "Multiple legacy display encoding boundaries have no singleton mapping",
                        {{"legacy_operation", "gamma"}, {"reason", "duplicate_legacy_gamma"}});
                }
                auto absorbed = absorb_legacy_gamma(reader.attributes());
                if (!absorbed)
                {
                    return absorbed.error();
                }
                absorbed_gamma = true;
                ++history_index;
                continue;
            }
            if (operation.value() == "primaries")
            {
                const auto version =
                    required_attribute(reader.attributes(), u"modversion", "primaries");
                const auto enabled =
                    required_attribute(reader.attributes(), u"enabled", "primaries");
                const auto parameters =
                    required_attribute(reader.attributes(), u"params", "primaries");
                const auto blend =
                    required_attribute(reader.attributes(), u"blendop_params", "primaries");
                const auto blend_version =
                    required_attribute(reader.attributes(), u"blendop_version", "primaries");
                if (!version || !enabled || !parameters || !blend || !blend_version)
                {
                    return !version    ? version.error() :
                           !enabled    ? enabled.error() :
                           !parameters ? parameters.error() :
                           !blend      ? blend.error() :
                                         blend_version.error();
                }
                if (version.value() != "1")
                {
                    return make_error(ErrorCode::kUnsupported,
                                      "Legacy RGB primaries module version is unsupported",
                                      {{"legacy_operation", "primaries"},
                                       {"legacy_version", version.value()},
                                       {"reason", "unsupported_legacy_primaries_version"}});
                }
                if (enabled.value() != "1")
                {
                    return make_error(
                        ErrorCode::kUnsupported,
                        "Disabled legacy RGB primaries has no canonical enabled-operation mapping",
                        {{"legacy_operation", "primaries"},
                         {"reason", "unsupported_legacy_primaries_disabled"}});
                }
                if (has_attribute(reader.attributes(), u"mask_id") ||
                    has_attribute(reader.attributes(), u"blendop_mask_id"))
                {
                    return make_error(
                        ErrorCode::kUnsupported,
                        "Legacy RGB primaries mask state has no canonical mapping",
                        {{"legacy_operation", "primaries"}, {"reason", "unsupported_legacy_mask"}});
                }
                if (blend_version.value() != "13" ||
                    blend.value() != kPrimariesDefaultBlendParameters)
                {
                    return make_error(
                        ErrorCode::kUnsupported,
                        "Legacy RGB primaries blend or mask state has no canonical mapping",
                        {{"legacy_operation", "primaries"},
                         {"reason", "unsupported_legacy_blend"}});
                }
                if (primaries)
                {
                    return make_error(
                        ErrorCode::kConflict,
                        "Multiple enabled legacy RGB primaries instances have no canonical mapping",
                        {{"legacy_operation", "primaries"},
                         {"reason", "duplicate_legacy_primaries"}});
                }
                auto decoded = decode_legacy_primaries_v1_parameters(parameters.value());
                if (!decoded)
                {
                    return decoded.error();
                }
                primaries = OperationInstance{std::string(kPrimariesOperationId),
                                              1,
                                              "legacy-primaries-" + std::to_string(history_index),
                                              true,
                                              primaries_to_parameters(decoded.value()),
                                              std::nullopt};
                ++history_index;
                continue;
            }
            auto absorbed = absorb_builtin_raw_operation(operation.value(), reader.attributes());
            if (!absorbed)
            {
                return absorbed.error();
            }
            if (absorbed.value())
            {
                ++history_index;
                continue;
            }
            if (operation.value() != "exposure")
            {
                return make_error(ErrorCode::kUnsupported,
                                  "Legacy XMP operation has no proven canonical recipe mapping",
                                  {{"legacy_operation", operation.value()},
                                   {"reason", "unsupported_legacy_operation"}});
            }
            if (!operations.empty())
            {
                return make_error(
                    ErrorCode::kUnsupported,
                    "Multiple legacy history entries have no canonical singleton mapping",
                    {{"legacy_operation", "exposure"}, {"reason", "unsupported_legacy_history"}});
            }
            auto mapped = map_exposure_v5(reader.attributes(), history_index++);
            if (!mapped)
            {
                return mapped.error();
            }
            operations.push_back(std::move(mapped).value());
        }
        else if (reader.isEndElement() && reader.name() == u"history")
        {
            in_history = false;
        }
    }
    if (reader.hasError())
    {
        return make_error(ErrorCode::kValidation, "Legacy XMP is not well-formed XML",
                          {{"column", std::to_string(reader.columnNumber())},
                           {"line", std::to_string(reader.lineNumber())},
                           {"xml_error", reader.errorString().toUtf8().toStdString()}});
    }
    if (!found_description)
    {
        return make_error(ErrorCode::kValidation, "Legacy XMP does not contain an RDF description");
    }
    operations.insert(operations.begin(),
                      input_color.value_or(OperationInstance{
                          "ravo.color.input", 1, "legacy-colorin-default", true,
                          input_color_to_parameters(InputColorParams{}), std::nullopt}));
    if (primaries)
    {
        operations.insert(operations.begin() + 1, std::move(*primaries));
    }
    operations.push_back(output_color.value_or(
        OperationInstance{"ravo.color.output", 1, "legacy-colorout-default", true,
                          output_color_to_parameters(OutputColorParams{}), std::nullopt}));
    return Recipe{3, request.asset, std::move(operations), {}};
}

} // namespace ravo
