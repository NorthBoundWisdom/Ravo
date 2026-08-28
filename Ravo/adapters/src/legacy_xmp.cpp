#include "ravo/adapters/legacy_xmp.h"

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
#include "ravo/recipe/color_contrast.h"
#include "ravo/recipe/color_correction.h"
#include "ravo/recipe/color_harmonizer.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/operation.h"

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

struct LegacyExposureParams
{
    ExposureParams params;
};

struct LegacyExposureCandidate
{
    QXmlStreamAttributes attributes;
    std::uint64_t history_position = 0;
};

struct LegacyColorBalanceCandidate
{
    QXmlStreamAttributes attributes;
    std::uint64_t history_position = 0;
};

struct LegacyColorCheckerCandidate
{
    QXmlStreamAttributes attributes;
    std::uint64_t history_position = 0;
};

struct LegacyColorCorrectionCandidate
{
    QXmlStreamAttributes attributes;
    std::uint64_t history_position = 0;
};

struct LegacyColorContrastCandidate
{
    QXmlStreamAttributes attributes;
    std::uint64_t history_position = 0;
};

struct LegacyColorHarmonizerCandidate
{
    QXmlStreamAttributes attributes;
    std::uint64_t history_position = 0;
};

// Repository-history evidence used only for the synthetic legacy v1 upgrade.
constexpr std::array<std::array<float, 3>, kColorCheckerDefaultPatchCount>
    kLegacyColorCheckerV1Sources{{
        {39.19F, 13.76F, 14.29F},  {65.18F, 19.00F, 17.32F},  {49.46F, -4.23F, -22.95F},
        {42.85F, -13.33F, 22.12F}, {55.18F, 9.44F, -24.94F},  {70.36F, -32.77F, -0.04F},
        {62.92F, 35.49F, 57.10F},  {40.75F, 11.41F, -46.03F}, {52.10F, 48.11F, 16.89F},
        {30.67F, 21.19F, -20.81F}, {73.08F, -23.55F, 56.97F}, {72.43F, 17.48F, 68.20F},
        {30.97F, 12.67F, -46.30F}, {56.43F, -40.66F, 31.94F}, {43.40F, 50.68F, 28.84F},
        {82.45F, 2.41F, 80.25F},   {51.98F, 50.68F, -14.84F}, {51.02F, -27.63F, -28.03F},
        {95.97F, -0.40F, 1.24F},   {81.10F, -0.83F, -0.43F},  {66.81F, -1.08F, -0.70F},
        {50.98F, -0.19F, -0.30F},  {35.72F, -0.69F, -1.11F},  {21.46F, 0.06F, -0.95F},
    }};

[[nodiscard]] Result<std::uint64_t> legacy_history_position(const std::string_view value,
                                                            const std::string_view attribute,
                                                            const std::string_view operation,
                                                            const std::string_view reason)
{
    std::uint64_t parsed = 0;
    const auto [position, error] =
        std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (value.empty() || error != std::errc{} || position != value.data() + value.size())
    {
        return make_error(ErrorCode::kValidation, "Legacy history position is invalid",
                          {{"attribute", std::string(attribute)},
                           {"legacy_operation", std::string(operation)},
                           {"reason", std::string(reason)}});
    }
    return parsed;
}

[[nodiscard]] Result<LegacyExposureCandidate>
capture_exposure_candidate(const QXmlStreamAttributes &attributes)
{
    const auto position = required_attribute(attributes, u"num", "exposure");
    const auto priority = required_attribute(attributes, u"multi_priority", "exposure");
    const auto name = attribute_value(attributes, u"multi_name");
    if (!position || !priority || !name)
    {
        return !position ? position.error() :
               !priority ? priority.error() :
                           make_error(ErrorCode::kUnsupported,
                                      "Legacy exposure singleton name is missing",
                                      {{"attribute", "multi_name"},
                                       {"legacy_operation", "exposure"},
                                       {"reason", "unsupported_legacy_exposure_multi_state"}});
    }
    auto parsed_position = legacy_history_position(position.value(), "num", "exposure",
                                                   "invalid_legacy_exposure_revision");
    if (!parsed_position)
    {
        return parsed_position.error();
    }
    const auto hand_edited = attribute_value(attributes, u"multi_name_hand_edited");
    if (priority.value() != "0" || !name->empty() || (hand_edited && *hand_edited != "0"))
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy exposure instance state is not the frozen singleton priority",
                          {{"legacy_operation", "exposure"},
                           {"reason", "unsupported_legacy_exposure_multi_state"}});
    }
    return LegacyExposureCandidate{attributes, parsed_position.value()};
}

[[nodiscard]] Result<LegacyColorBalanceCandidate>
capture_color_balance_candidate(const QXmlStreamAttributes &attributes)
{
    const auto position = required_attribute(attributes, u"num", "colorbalance");
    const auto priority = required_attribute(attributes, u"multi_priority", "colorbalance");
    const auto name = attribute_value(attributes, u"multi_name");
    if (!position || !priority || !name)
    {
        return !position ? position.error() :
               !priority ? priority.error() :
                           make_error(ErrorCode::kUnsupported,
                                      "Legacy Color Balance singleton name is missing",
                                      {{"attribute", "multi_name"},
                                       {"legacy_operation", "colorbalance"},
                                       {"reason", "unsupported_legacy_colorbalance_multi_state"}});
    }
    std::uint64_t parsed_position = 0U;
    const auto [end, error] =
        std::from_chars(position.value().data(), position.value().data() + position.value().size(),
                        parsed_position);
    if (position.value().empty() || error != std::errc{} ||
        end != position.value().data() + position.value().size() ||
        std::to_string(parsed_position) != position.value())
    {
        return make_error(ErrorCode::kValidation, "Legacy Color Balance revision state is invalid",
                          {{"attribute", "num"},
                           {"legacy_operation", "colorbalance"},
                           {"reason", "invalid_legacy_colorbalance_revision"}});
    }
    const auto hand_edited = attribute_value(attributes, u"multi_name_hand_edited");
    if (priority.value() != "0" || !name->empty() || (hand_edited && *hand_edited != "0"))
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Color Balance instance is not the frozen singleton priority",
                          {{"legacy_operation", "colorbalance"},
                           {"reason", "unsupported_legacy_colorbalance_multi_state"}});
    }
    return LegacyColorBalanceCandidate{attributes, parsed_position};
}

[[nodiscard]] Result<LegacyColorCheckerCandidate>
capture_color_checker_candidate(const QXmlStreamAttributes &attributes)
{
    const auto position = required_attribute(attributes, u"num", "colorchecker");
    const auto priority = required_attribute(attributes, u"multi_priority", "colorchecker");
    const auto name = attribute_value(attributes, u"multi_name");
    if (!position || !priority || !name)
    {
        return !position ? position.error() :
               !priority ? priority.error() :
                           make_error(ErrorCode::kUnsupported,
                                      "Legacy color checker singleton name is missing",
                                      {{"attribute", "multi_name"},
                                       {"legacy_operation", "colorchecker"},
                                       {"reason", "unsupported_legacy_colorchecker_multi_state"}});
    }
    std::uint64_t parsed_position = 0U;
    const auto [end, error] =
        std::from_chars(position.value().data(), position.value().data() + position.value().size(),
                        parsed_position);
    if (position.value().empty() || error != std::errc{} ||
        end != position.value().data() + position.value().size() ||
        std::to_string(parsed_position) != position.value())
    {
        return make_error(ErrorCode::kValidation, "Legacy color checker revision state is invalid",
                          {{"attribute", "num"},
                           {"legacy_operation", "colorchecker"},
                           {"reason", "invalid_legacy_colorchecker_revision"}});
    }
    const auto hand_edited = attribute_value(attributes, u"multi_name_hand_edited");
    if (priority.value() != "0" || !name->empty() || (hand_edited && *hand_edited != "0"))
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy color checker instance is not the frozen singleton priority",
                          {{"legacy_operation", "colorchecker"},
                           {"reason", "unsupported_legacy_colorchecker_multi_state"}});
    }
    return LegacyColorCheckerCandidate{attributes, parsed_position};
}

[[nodiscard]] Result<LegacyColorCorrectionCandidate>
capture_color_correction_candidate(const QXmlStreamAttributes &attributes)
{
    const auto position = required_attribute(attributes, u"num", "colorcorrection");
    const auto priority = required_attribute(attributes, u"multi_priority", "colorcorrection");
    const auto name = attribute_value(attributes, u"multi_name");
    if (!position || !priority || !name)
    {
        return !position ?
                   position.error() :
               !priority ?
                   priority.error() :
                   make_error(ErrorCode::kUnsupported,
                              "Legacy Color Correction singleton name is missing",
                              {{"attribute", "multi_name"},
                               {"legacy_operation", "colorcorrection"},
                               {"reason", "unsupported_legacy_colorcorrection_multi_state"}});
    }
    auto parsed_position = legacy_history_position(position.value(), "num", "colorcorrection",
                                                   "invalid_legacy_history_position");
    if (!parsed_position)
    {
        return parsed_position.error();
    }
    const auto hand_edited = attribute_value(attributes, u"multi_name_hand_edited");
    if (priority.value() != "0" || !name->empty() || (hand_edited && *hand_edited != "0"))
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Color Correction instance is not the frozen singleton priority",
                          {{"legacy_operation", "colorcorrection"},
                           {"reason", "unsupported_legacy_colorcorrection_multi_state"}});
    }
    return LegacyColorCorrectionCandidate{attributes, parsed_position.value()};
}

[[nodiscard]] Result<LegacyColorContrastCandidate>
capture_color_contrast_candidate(const QXmlStreamAttributes &attributes)
{
    const auto position = required_attribute(attributes, u"num", "colorcontrast");
    const auto priority = required_attribute(attributes, u"multi_priority", "colorcontrast");
    const auto name = attribute_value(attributes, u"multi_name");
    if (!position || !priority || !name)
    {
        return !position ? position.error() :
               !priority ? priority.error() :
                           make_error(ErrorCode::kUnsupported,
                                      "Legacy Color Contrast singleton name is missing",
                                      {{"attribute", "multi_name"},
                                       {"legacy_operation", "colorcontrast"},
                                       {"reason", "unsupported_legacy_colorcontrast_multi_state"}});
    }
    auto parsed_position = legacy_history_position(position.value(), "num", "colorcontrast",
                                                   "invalid_legacy_history_position");
    if (!parsed_position)
    {
        return parsed_position.error();
    }
    const auto hand_edited = attribute_value(attributes, u"multi_name_hand_edited");
    if (priority.value() != "0" || !name->empty() || (hand_edited && *hand_edited != "0"))
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Color Contrast instance is not the frozen singleton priority",
                          {{"legacy_operation", "colorcontrast"},
                           {"reason", "unsupported_legacy_colorcontrast_multi_state"}});
    }
    return LegacyColorContrastCandidate{attributes, parsed_position.value()};
}

[[nodiscard]] Result<LegacyColorHarmonizerCandidate>
capture_color_harmonizer_candidate(const QXmlStreamAttributes &attributes)
{
    const auto position = required_attribute(attributes, u"num", "colorharmonizer");
    const auto priority = required_attribute(attributes, u"multi_priority", "colorharmonizer");
    const auto name = attribute_value(attributes, u"multi_name");
    if (!position || !priority || !name)
    {
        return !position ?
                   position.error() :
               !priority ?
                   priority.error() :
                   make_error(ErrorCode::kUnsupported,
                              "Legacy Color Harmonizer singleton name is missing",
                              {{"attribute", "multi_name"},
                               {"legacy_operation", "colorharmonizer"},
                               {"reason", "unsupported_legacy_colorharmonizer_multi_state"}});
    }
    auto parsed_position = legacy_history_position(position.value(), "num", "colorharmonizer",
                                                   "invalid_legacy_history_position");
    if (!parsed_position)
    {
        return parsed_position.error();
    }
    const auto hand_edited = attribute_value(attributes, u"multi_name_hand_edited");
    if (priority.value() != "0" || !name->empty() || (hand_edited && *hand_edited != "0"))
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Color Harmonizer instance is not the frozen singleton priority",
                          {{"legacy_operation", "colorharmonizer"},
                           {"reason", "unsupported_legacy_colorharmonizer_multi_state"}});
    }
    return LegacyColorHarmonizerCandidate{attributes, parsed_position.value()};
}

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

constexpr std::string_view kExposureBlendV11UncompressedScene =
    "000000000300000018000000000000000000c84200000000000000000000000000000000050000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000803f0000803f00000000000000000000803f"
    "0000803f00000000000000000000803f0000803f00000000000000000000803f0000803f00000000000000000000803f"
    "0000803f00000000000000000000803f0000803f00000000000000000000803f0000803f00000000000000000000803f"
    "0000803f00000000000000000000803f0000803f00000000000000000000803f0000803f00000000000000000000803f"
    "0000803f00000000000000000000803f0000803f00000000000000000000803f0000803f00000000000000000000803f"
    "0000803f00000000000000000000803f0000803f00000000000000000000803f0000803f000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000";

constexpr std::array kLegacyExposureBlendTuples{
    LegacyGammaBlendTuple{"9", kDefaultBlendParameters},
    LegacyGammaBlendTuple{"10", "gz13eJxjYGBgYAZiCQYYOOHEgAYY0QVwggZ7CB6pfNoAAE4AGQc="},
    LegacyGammaBlendTuple{"11", kExposureBlendV11UncompressedScene},
    LegacyGammaBlendTuple{"11", "gz10eJxjYGBgYAFiCQYYOOHEgAZY0QVwggZ7CB6pfOygYtaVAyCMi08IAAB/xiOk"},
    LegacyGammaBlendTuple{"11", "gz13eJxjYGBgYAZiCQYYOOHEgAYY0QVwggZ7CB6pfNoAAE4AGQc="},
    LegacyGammaBlendTuple{"11", "gz13eJxjYGBgYAZiCQYYOOHEgAZY0QVwggZ7CB6pfNoAAFQAGQs="},
    LegacyGammaBlendTuple{"12", kPrimariesDefaultBlendParameters},
    LegacyGammaBlendTuple{"12", "gz10eJxjYGBgYAFiCQYYOOHEgAZY0QVwggZ7CB6pfOygYtaVAyCMi08IAAB/xiOk"},
    LegacyGammaBlendTuple{
        "13", "gz08eJxjYGBgYAFiCQYYOOHEgAZY0QWAgBGLGANDgz0Ej1Q+dlAx68oBEMbFxwX+AwGIBgCbGCeh"},
    LegacyGammaBlendTuple{"13", kPrimariesDefaultBlendParameters},
    LegacyGammaBlendTuple{
        "14", "gz08eJxjYGBgYAFiCQYYOOHEgAZY0QWAgBGLGANDgz0Ej1Q+dlAx68oBEMbFxwX+AwGIBgCbGCeh"},
};

constexpr std::array<std::string_view, 2> kFrozenColorBalanceParametricBlends{
    "gz06eJxjZWBgYGYAgRNODFDAyASlGfADjrXTbE8t2m/rW1Brz8DQYI+QaRgQvqYrl/3DZH77p/8r7OfK1dHRfuwAALvfIn8=",
    "gz05eJxjZWBgYGEAgRNODFDAyASlGfADjrXTbE8t2m/78Lik/cxncvYImQZ7CKYvf4M0l32GFb991c4ie6Mb5XS0HzsAACQpI3A=",
};

constexpr std::string_view kFrozenColorCheckerBlendV11 =
    "gz13eJxjYGBgYAJiCQYYOOHEgAZY0QVwggZ7CB6pfNoAAFJgGQo=";

constexpr std::array kFrozenColorCorrectionBlendTuples{
    LegacyGammaBlendTuple{"9", kDefaultBlendParameters},
    LegacyGammaBlendTuple{"11", "gz13eJxjYGBgYAJiCQYYOOHEgAZY0QVwggZ7CB6pfNoAAFJgGQo="},
};

constexpr std::string_view kFrozenColorContrastBlendV10 =
    "gz13eJxjYGBgYAJiCQYYOOHEgAYY0QVwggZ7CB6pfNoAAExgGQY=";

constexpr std::string_view kFrozenColorHarmonizerBlendV14 =
    "gz08eJxjYGBgYAFiCQYYOOHEgAZY0QWAgBGLGANDgz0Ej1Q+dlAx68oBEMbFxwX+AwGIBgCbGCeh";

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

constexpr std::string_view kLegacyFlipBlendGz14 =
    "gz14eJxjYIAACQYYOOHEgAYY0QVwggZ7CB6pfNoAAEkgGQQ=";
constexpr std::string_view kLegacyGeometryBlendGz14GuideFive =
    "gz14eJxjYIAACQYYOOHEgAZY0QVwggZ7CB6pfNoAAE8gGQg=";

[[nodiscard]] bool is_legacy_unmasked_geometry_blend(const std::string_view blend) noexcept
{
    return blend == kDefaultBlendParameters || blend == kLegacyFlipBlendGz14 ||
           blend == kLegacyGeometryBlendGz14GuideFive;
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
    std::vector<LegacyExposureCandidate> exposure_candidates;
    std::vector<LegacyColorBalanceCandidate> color_balance_candidates;
    std::vector<LegacyColorCheckerCandidate> color_checker_candidates;
    std::vector<LegacyColorCorrectionCandidate> color_correction_candidates;
    std::vector<LegacyColorContrastCandidate> color_contrast_candidates;
    std::vector<LegacyColorHarmonizerCandidate> color_harmonizer_candidates;
    std::optional<OperationInstance> input_color;
    std::optional<OperationInstance> output_color;
    std::optional<OperationInstance> primaries;
    std::optional<LeftoverFlipGeometry> flip_geometry;
    std::optional<LeftoverCropBox> crop_box;
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
            if (operation.value() == "colorbalance")
            {
                auto captured = capture_color_balance_candidate(reader.attributes());
                if (!captured)
                {
                    return captured.error();
                }
                if (!color_balance_candidates.empty())
                {
                    return make_error(
                        ErrorCode::kConflict,
                        "Multiple legacy Color Balance singleton entries have no canonical mapping",
                        {{"legacy_operation", "colorbalance"},
                         {"reason", "duplicate_legacy_colorbalance"}});
                }
                color_balance_candidates.push_back(std::move(captured).value());
                ++history_index;
                continue;
            }
            if (operation.value() == "colorchecker")
            {
                auto captured = capture_color_checker_candidate(reader.attributes());
                if (!captured)
                {
                    return captured.error();
                }
                if (!color_checker_candidates.empty())
                {
                    return make_error(
                        ErrorCode::kConflict,
                        "Multiple legacy color checker singleton entries have no canonical mapping",
                        {{"legacy_operation", "colorchecker"},
                         {"reason", "duplicate_legacy_colorchecker"}});
                }
                color_checker_candidates.push_back(std::move(captured).value());
                ++history_index;
                continue;
            }
            if (operation.value() == "colorcorrection")
            {
                auto captured = capture_color_correction_candidate(reader.attributes());
                if (!captured)
                {
                    return captured.error();
                }
                if (!color_correction_candidates.empty())
                {
                    return make_error(
                        ErrorCode::kConflict,
                        "Multiple legacy Color Correction singleton entries have no canonical mapping",
                        {{"legacy_operation", "colorcorrection"},
                         {"reason", "duplicate_legacy_colorcorrection"}});
                }
                color_correction_candidates.push_back(std::move(captured).value());
                ++history_index;
                continue;
            }
            if (operation.value() == "colorcontrast")
            {
                auto captured = capture_color_contrast_candidate(reader.attributes());
                if (!captured)
                {
                    return captured.error();
                }
                if (!color_contrast_candidates.empty())
                {
                    return make_error(
                        ErrorCode::kConflict,
                        "Multiple legacy Color Contrast singleton entries have no canonical mapping",
                        {{"legacy_operation", "colorcontrast"},
                         {"reason", "duplicate_legacy_colorcontrast"}});
                }
                color_contrast_candidates.push_back(std::move(captured).value());
                ++history_index;
                continue;
            }
            if (operation.value() == "colorharmonizer")
            {
                auto captured = capture_color_harmonizer_candidate(reader.attributes());
                if (!captured)
                {
                    return captured.error();
                }
                if (std::any_of(
                        color_harmonizer_candidates.begin(), color_harmonizer_candidates.end(),
                        [&](const LegacyColorHarmonizerCandidate &existing)
                        { return existing.history_position == captured.value().history_position; }))
                {
                    return make_error(
                        ErrorCode::kConflict,
                        "Legacy Color Harmonizer revisions reuse one history position",
                        {{"legacy_operation", "colorharmonizer"},
                         {"reason", "duplicate_legacy_colorharmonizer_revision"}});
                }
                color_harmonizer_candidates.push_back(std::move(captured).value());
                ++history_index;
                continue;
            }
            if (operation.value() == "flip")
            {
                auto mapped = map_legacy_flip(reader.attributes());
                if (!mapped)
                {
                    return mapped.error();
                }
                flip_geometry = std::move(mapped).value();
                ++history_index;
                continue;
            }
            if (operation.value() == "crop")
            {
                auto mapped = map_legacy_crop(reader.attributes());
                if (!mapped)
                {
                    return mapped.error();
                }
                crop_box = std::move(mapped).value();
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
            auto captured = capture_exposure_candidate(reader.attributes());
            if (!captured)
            {
                return captured.error();
            }
            if (std::any_of(
                    exposure_candidates.begin(), exposure_candidates.end(),
                    [&](const LegacyExposureCandidate &existing)
                    { return existing.history_position == captured.value().history_position; }))
            {
                return make_error(ErrorCode::kConflict,
                                  "Legacy exposure revisions reuse one history position",
                                  {{"legacy_operation", "exposure"},
                                   {"reason", "duplicate_legacy_exposure_revision"}});
            }
            exposure_candidates.push_back(std::move(captured).value());
            ++history_index;
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
    if (!exposure_candidates.empty())
    {
        const auto final_revision = std::max_element(
            exposure_candidates.begin(), exposure_candidates.end(),
            [](const LegacyExposureCandidate &left, const LegacyExposureCandidate &right)
            { return left.history_position < right.history_position; });
        auto mapped = map_exposure_candidate(*final_revision);
        if (!mapped)
        {
            return mapped.error();
        }
        operations.push_back(std::move(mapped).value());
    }
    if (!color_balance_candidates.empty())
    {
        auto mapped = map_color_balance_candidate(color_balance_candidates.front());
        if (!mapped)
        {
            return mapped.error();
        }
        operations.push_back(std::move(mapped).value());
    }
    if (!color_checker_candidates.empty())
    {
        auto mapped = map_color_checker_candidate(color_checker_candidates.front());
        if (!mapped)
        {
            return mapped.error();
        }
        const auto color_balance = std::find_if(
            operations.begin(), operations.end(), [](const OperationInstance &operation)
            { return operation.id == kColorBalanceOperationId; });
        operations.insert(color_balance, std::move(mapped).value());
    }
    if (!color_harmonizer_candidates.empty())
    {
        std::optional<OperationInstance> winner;
        std::uint64_t winner_position = 0;
        for (const auto &candidate : color_harmonizer_candidates)
        {
            auto mapped = map_color_harmonizer_candidate(candidate);
            if (!mapped)
            {
                return mapped.error();
            }
            if (!winner || candidate.history_position > winner_position)
            {
                winner_position = candidate.history_position;
                winner = std::move(mapped).value();
            }
        }
        const auto color_balance = std::find_if(
            operations.begin(), operations.end(), [](const OperationInstance &operation)
            { return operation.id == kColorBalanceOperationId; });
        operations.insert(color_balance, std::move(*winner));
    }
    if (!color_correction_candidates.empty())
    {
        auto mapped = map_color_correction_candidate(color_correction_candidates.front());
        if (!mapped)
        {
            return mapped.error();
        }
        operations.push_back(std::move(mapped).value());
    }
    if (!color_contrast_candidates.empty())
    {
        auto mapped = map_color_contrast_candidate(color_contrast_candidates.front());
        if (!mapped)
        {
            return mapped.error();
        }
        operations.push_back(std::move(mapped).value());
    }
    if (flip_geometry && !flip_geometry->is_identity())
    {
        if (flip_geometry->rotate_quarters != 0)
        {
            operations.push_back(
                OperationInstance{"ravo.geometry.rotate",
                                  1,
                                  "legacy-flip-rotate",
                                  true,
                                  {{"quarters", ParameterValue{flip_geometry->rotate_quarters}}},
                                  std::nullopt});
        }
        if (flip_geometry->flip_horizontal != 0 || flip_geometry->flip_vertical != 0)
        {
            operations.push_back(
                OperationInstance{"ravo.geometry.flip",
                                  1,
                                  "legacy-flip",
                                  true,
                                  {{"horizontal", ParameterValue{flip_geometry->flip_horizontal}},
                                   {"vertical", ParameterValue{flip_geometry->flip_vertical}}},
                                  std::nullopt});
        }
    }
    if (crop_box && !crop_box->is_identity())
    {
        operations.push_back(OperationInstance{"ravo.geometry.crop",
                                               1,
                                               "legacy-crop",
                                               true,
                                               {{"x", ParameterValue{crop_box->x}},
                                                {"y", ParameterValue{crop_box->y}},
                                                {"width", ParameterValue{crop_box->width}},
                                                {"height", ParameterValue{crop_box->height}}},
                                               std::nullopt});
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
