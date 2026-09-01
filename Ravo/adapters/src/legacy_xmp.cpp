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

namespace ravo
{
using namespace legacy_xmp_internal;

namespace legacy_xmp_internal
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

[[nodiscard]] Result<std::vector<std::uint8_t>>
decode_legacy_parameter_blob_min(const std::string_view encoded, const std::size_t minimum_size,
                                 const std::string_view operation)
{
    if (!encoded.starts_with("gz"))
    {
        if (encoded.size() % 2U != 0U || encoded.size() / 2U < minimum_size)
        {
            return make_error(ErrorCode::kValidation,
                              "Legacy parameters have an unexpected hexadecimal length",
                              {{"legacy_operation", std::string(operation)}});
        }
        return decode_legacy_parameter_blob(encoded, encoded.size() / 2U, operation);
    }
    constexpr std::size_t kMaxDecoded = 64U * 1024U;
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
    std::vector<std::uint8_t> decoded(kMaxDecoded);
    uLongf decoded_size = static_cast<uLongf>(decoded.size());
    const int status = uncompress(decoded.data(), &decoded_size,
                                  reinterpret_cast<const Bytef *>(compressed.constData()),
                                  static_cast<uLong>(compressed.size()));
    if (status != Z_OK || decoded_size < minimum_size)
    {
        return make_error(ErrorCode::kValidation,
                          "Legacy compressed parameters have an unexpected payload",
                          {{"legacy_operation", std::string(operation)}});
    }
    decoded.resize(static_cast<std::size_t>(decoded_size));
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

[[nodiscard]] Result<LegacyColorReconstructionCandidate>
capture_color_reconstruction_candidate(const QXmlStreamAttributes &attributes)
{
    const auto position = required_attribute(attributes, u"num", "colorreconstruct");
    const auto priority = required_attribute(attributes, u"multi_priority", "colorreconstruct");
    const auto name = attribute_value(attributes, u"multi_name");
    if (!position || !priority || !name)
    {
        return !position ?
                   position.error() :
               !priority ?
                   priority.error() :
                   make_error(ErrorCode::kUnsupported,
                              "Legacy Color Reconstruction singleton name is missing",
                              {{"attribute", "multi_name"},
                               {"legacy_operation", "colorreconstruct"},
                               {"reason", "unsupported_legacy_colorreconstruct_multi_state"}});
    }
    auto parsed_position = legacy_history_position(position.value(), "num", "colorreconstruct",
                                                   "invalid_legacy_history_position");
    if (!parsed_position)
    {
        return parsed_position.error();
    }
    const auto hand_edited = attribute_value(attributes, u"multi_name_hand_edited");
    if (priority.value() != "0" || !name->empty() || (hand_edited && *hand_edited != "0"))
    {
        return make_error(
            ErrorCode::kUnsupported,
            "Legacy Color Reconstruction instance is not the frozen singleton priority",
            {{"legacy_operation", "colorreconstruct"},
             {"reason", "unsupported_legacy_colorreconstruct_multi_state"}});
    }
    return LegacyColorReconstructionCandidate{attributes, parsed_position.value()};
}

[[nodiscard]] Result<LegacySharpenCandidate>
capture_sharpen_candidate(const QXmlStreamAttributes &attributes)
{
    const auto position = required_attribute(attributes, u"num", "sharpen");
    const auto priority = required_attribute(attributes, u"multi_priority", "sharpen");
    const auto name = attribute_value(attributes, u"multi_name");
    if (!position || !priority || !name)
    {
        return !position ? position.error() :
               !priority ? priority.error() :
                           make_error(ErrorCode::kUnsupported,
                                      "Legacy Sharpen singleton name is missing",
                                      {{"attribute", "multi_name"},
                                       {"legacy_operation", "sharpen"},
                                       {"reason", "unsupported_legacy_sharpen_multi_state"}});
    }
    auto parsed_position = legacy_history_position(position.value(), "num", "sharpen",
                                                   "invalid_legacy_history_position");
    if (!parsed_position)
    {
        return parsed_position.error();
    }
    const auto hand_edited = attribute_value(attributes, u"multi_name_hand_edited");
    if (priority.value() != "0" || !name->empty() || (hand_edited && *hand_edited != "0"))
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Sharpen instance is not the frozen singleton priority",
                          {{"legacy_operation", "sharpen"},
                           {"reason", "unsupported_legacy_sharpen_multi_state"}});
    }
    return LegacySharpenCandidate{attributes, parsed_position.value()};
}

[[nodiscard]] Result<LegacyDehazeCandidate>
capture_dehaze_candidate(const QXmlStreamAttributes &attributes)
{
    const auto position = required_attribute(attributes, u"num", "hazeremoval");
    const auto priority = required_attribute(attributes, u"multi_priority", "hazeremoval");
    const auto name = attribute_value(attributes, u"multi_name");
    if (!position || !priority || !name)
    {
        return !position ? position.error() :
               !priority ? priority.error() :
                           make_error(ErrorCode::kUnsupported,
                                      "Legacy Haze Removal singleton name is missing",
                                      {{"attribute", "multi_name"},
                                       {"legacy_operation", "hazeremoval"},
                                       {"reason", "unsupported_legacy_dehaze_multi_state"}});
    }
    auto parsed_position = legacy_history_position(position.value(), "num", "hazeremoval",
                                                   "invalid_legacy_history_position");
    if (!parsed_position)
    {
        return parsed_position.error();
    }
    const auto hand_edited = attribute_value(attributes, u"multi_name_hand_edited");
    if (priority.value() != "0" || !name->empty() || (hand_edited && *hand_edited != "0"))
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy Haze Removal instance is not the frozen singleton priority",
                          {{"legacy_operation", "hazeremoval"},
                           {"reason", "unsupported_legacy_dehaze_multi_state"}});
    }
    return LegacyDehazeCandidate{attributes, parsed_position.value()};
}

[[nodiscard]] Result<LegacyOutputDitherCandidate>
capture_output_dither_candidate(const QXmlStreamAttributes &attributes)
{
    const auto position = required_attribute(attributes, u"num", "dither");
    const auto priority = required_attribute(attributes, u"multi_priority", "dither");
    const auto name = attribute_value(attributes, u"multi_name");
    if (!position || !priority || !name)
    {
        return !position ? position.error() :
               !priority ? priority.error() :
                           make_error(ErrorCode::kUnsupported,
                                      "Legacy Output Dither singleton name is missing",
                                      {{"attribute", "multi_name"},
                                       {"legacy_operation", "dither"},
                                       {"reason", "unsupported_legacy_dither_multi_state"}});
    }
    auto parsed_position = legacy_history_position(position.value(), "num", "dither",
                                                   "invalid_legacy_history_position");
    if (!parsed_position)
        return parsed_position.error();
    const auto hand_edited = attribute_value(attributes, u"multi_name_hand_edited");
    if (priority.value() != "0" || !name->empty() || (hand_edited && *hand_edited != "0"))
    {
        return make_error(
            ErrorCode::kUnsupported,
            "Legacy Output Dither instance is not the frozen singleton priority",
            {{"legacy_operation", "dither"}, {"reason", "unsupported_legacy_dither_multi_state"}});
    }
    return LegacyOutputDitherCandidate{attributes, parsed_position.value()};
}

template <typename Candidate>
[[nodiscard]] Result<Candidate> capture_geometry_singleton(const QXmlStreamAttributes &attributes,
                                                           const std::string_view operation)
{
    const auto position = required_attribute(attributes, u"num", operation);
    const auto priority = required_attribute(attributes, u"multi_priority", operation);
    const auto name = attribute_value(attributes, u"multi_name");
    if (!position || !priority || !name)
    {
        return !position ? position.error() :
               !priority ? priority.error() :
                           make_error(ErrorCode::kUnsupported,
                                      "Legacy geometry singleton name is missing",
                                      {{"legacy_operation", std::string(operation)},
                                       {"reason", "unsupported_legacy_geometry_multi_state"}});
    }
    auto parsed_position = legacy_history_position(position.value(), "num", operation,
                                                   "invalid_legacy_history_position");
    if (!parsed_position)
        return parsed_position.error();
    const auto hand_edited = attribute_value(attributes, u"multi_name_hand_edited");
    if (priority.value() != "0" || !name->empty() || (hand_edited && *hand_edited != "0"))
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy geometry instance is not the frozen singleton priority",
                          {{"legacy_operation", std::string(operation)},
                           {"reason", "unsupported_legacy_geometry_multi_state"}});
    }
    return Candidate{attributes, parsed_position.value()};
}

} // namespace legacy_xmp_internal

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
    if (is_crs_xmp_document(request.xmp_utf8))
    {
        return make_error(ErrorCode::kUnsupported,
                          "Camera Raw XMP is not leftover darktable history",
                          {{"reason", "crs_requires_crs_importer"}});
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
    std::vector<LegacyColorReconstructionCandidate> color_reconstruction_candidates;
    std::vector<LegacySharpenCandidate> sharpen_candidates;
    std::vector<LegacyDehazeCandidate> dehaze_candidates;
    std::vector<LegacyOutputDitherCandidate> output_dither_candidates;
    std::vector<LegacyCanvasCandidate> canvas_candidates;
    std::vector<LegacyFrameCandidate> frame_candidates;
    std::vector<LegacyColorZonesCandidate> color_zones_candidates;
    std::vector<LegacyMonochromeCandidate> monochrome_candidates;
    std::vector<LegacySplitToningCandidate> split_toning_candidates;
    std::vector<LegacyVelviaCandidate> velvia_candidates;
    std::vector<LegacyRetouchCandidate> retouch_candidates;
    std::vector<LegacyMaskRecord> legacy_masks;
    std::optional<OperationInstance> input_color;
    std::optional<OperationInstance> output_color;
    std::optional<OperationInstance> primaries;
    std::optional<LeftoverFlipGeometry> flip_geometry;
    std::optional<LeftoverCropBox> crop_box;
    std::optional<PerspectiveParams> ashift_perspective;
    std::optional<RgbLevelsParams> rgb_levels;
    std::optional<RgbCurveParams> rgb_curve;
    std::optional<LeftoverRawDenoise> raw_denoise;
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
                auto masks = parse_legacy_mask_history(reader);
                if (!masks)
                {
                    return masks.error();
                }
                legacy_masks = std::move(masks).value();
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
            if (operation.value() == "colorreconstruct")
            {
                auto captured = capture_color_reconstruction_candidate(reader.attributes());
                if (!captured)
                {
                    return captured.error();
                }
                if (!color_reconstruction_candidates.empty())
                {
                    return make_error(
                        ErrorCode::kConflict,
                        "Multiple legacy Color Reconstruction singleton entries have no canonical mapping",
                        {{"legacy_operation", "colorreconstruct"},
                         {"reason", "duplicate_legacy_colorreconstruct"}});
                }
                color_reconstruction_candidates.push_back(std::move(captured).value());
                ++history_index;
                continue;
            }
            if (operation.value() == "sharpen")
            {
                auto captured = capture_sharpen_candidate(reader.attributes());
                if (!captured)
                {
                    return captured.error();
                }
                if (!sharpen_candidates.empty())
                {
                    return make_error(
                        ErrorCode::kConflict,
                        "Multiple legacy Sharpen singleton entries have no canonical mapping",
                        {{"legacy_operation", "sharpen"}, {"reason", "duplicate_legacy_sharpen"}});
                }
                sharpen_candidates.push_back(std::move(captured).value());
                ++history_index;
                continue;
            }
            if (operation.value() == "hazeremoval")
            {
                auto captured = capture_dehaze_candidate(reader.attributes());
                if (!captured)
                {
                    return captured.error();
                }
                if (!dehaze_candidates.empty())
                {
                    return make_error(
                        ErrorCode::kConflict,
                        "Multiple legacy Haze Removal singleton entries have no canonical mapping",
                        {{"legacy_operation", "hazeremoval"},
                         {"reason", "duplicate_legacy_dehaze"}});
                }
                dehaze_candidates.push_back(std::move(captured).value());
                ++history_index;
                continue;
            }
            if (operation.value() == "dither")
            {
                auto captured = capture_output_dither_candidate(reader.attributes());
                if (!captured)
                    return captured.error();
                if (!output_dither_candidates.empty())
                {
                    return make_error(
                        ErrorCode::kConflict,
                        "Multiple legacy Output Dither singleton entries have no canonical mapping",
                        {{"legacy_operation", "dither"}, {"reason", "duplicate_legacy_dither"}});
                }
                output_dither_candidates.push_back(std::move(captured).value());
                ++history_index;
                continue;
            }
            if (operation.value() == "enlargecanvas")
            {
                auto captured = capture_geometry_singleton<LegacyCanvasCandidate>(
                    reader.attributes(), "enlargecanvas");
                if (!captured)
                    return captured.error();
                if (!canvas_candidates.empty())
                    return make_error(ErrorCode::kConflict, "Duplicate legacy Canvas",
                                      {{"reason", "duplicate_legacy_canvas"}});
                canvas_candidates.push_back(std::move(captured).value());
                ++history_index;
                continue;
            }
            if (operation.value() == "borders")
            {
                auto captured = capture_geometry_singleton<LegacyFrameCandidate>(
                    reader.attributes(), "borders");
                if (!captured)
                    return captured.error();
                if (!frame_candidates.empty())
                    return make_error(ErrorCode::kConflict, "Duplicate legacy Frame",
                                      {{"reason", "duplicate_legacy_frame"}});
                frame_candidates.push_back(std::move(captured).value());
                ++history_index;
                continue;
            }
            if (operation.value() == "colorzones")
            {
                auto captured = capture_geometry_singleton<LegacyColorZonesCandidate>(
                    reader.attributes(), "colorzones");
                if (!captured)
                    return captured.error();
                if (!color_zones_candidates.empty())
                    return make_error(ErrorCode::kConflict, "Duplicate legacy Color Zones",
                                      {{"reason", "duplicate_legacy_color_zones"}});
                color_zones_candidates.push_back(std::move(captured).value());
                ++history_index;
                continue;
            }
            if (operation.value() == "monochrome")
            {
                auto captured = capture_geometry_singleton<LegacyMonochromeCandidate>(
                    reader.attributes(), "monochrome");
                if (!captured)
                    return captured.error();
                if (!monochrome_candidates.empty())
                    return make_error(ErrorCode::kConflict, "Duplicate legacy Monochrome",
                                      {{"reason", "duplicate_legacy_monochrome"}});
                monochrome_candidates.push_back(std::move(captured).value());
                ++history_index;
                continue;
            }
            if (operation.value() == "splittoning")
            {
                auto captured = capture_geometry_singleton<LegacySplitToningCandidate>(
                    reader.attributes(), "splittoning");
                if (!captured)
                    return captured.error();
                if (!split_toning_candidates.empty())
                    return make_error(ErrorCode::kConflict, "Duplicate legacy Split Toning",
                                      {{"reason", "duplicate_legacy_split_toning"}});
                split_toning_candidates.push_back(std::move(captured).value());
                ++history_index;
                continue;
            }
            if (operation.value() == "velvia")
            {
                auto captured = capture_geometry_singleton<LegacyVelviaCandidate>(
                    reader.attributes(), "velvia");
                if (!captured)
                    return captured.error();
                if (!velvia_candidates.empty())
                    return make_error(ErrorCode::kConflict, "Duplicate legacy Velvia",
                                      {{"reason", "duplicate_legacy_velvia"}});
                velvia_candidates.push_back(std::move(captured).value());
                ++history_index;
                continue;
            }
            if (operation.value() == "retouch")
            {
                auto captured = capture_retouch_candidate(reader.attributes());
                if (!captured)
                {
                    return captured.error();
                }
                if (std::any_of(
                        retouch_candidates.begin(), retouch_candidates.end(),
                        [&](const LegacyRetouchCandidate &existing)
                        { return existing.history_position == captured.value().history_position; }))
                {
                    return make_error(ErrorCode::kConflict,
                                      "Legacy Retouch revisions reuse one history position",
                                      {{"legacy_operation", "retouch"},
                                       {"reason", "duplicate_legacy_retouch_revision"}});
                }
                retouch_candidates.push_back(std::move(captured).value());
                ++history_index;
                continue;
            }
            if (operation.value() == "mask_manager")
            {
                const auto version =
                    required_attribute(reader.attributes(), u"modversion", "mask_manager");
                const auto enabled =
                    required_attribute(reader.attributes(), u"enabled", "mask_manager");
                const auto parameters =
                    required_attribute(reader.attributes(), u"params", "mask_manager");
                const auto blend_version =
                    required_attribute(reader.attributes(), u"blendop_version", "mask_manager");
                const auto blend =
                    required_attribute(reader.attributes(), u"blendop_params", "mask_manager");
                const auto priority =
                    required_attribute(reader.attributes(), u"multi_priority", "mask_manager");
                const auto name = attribute_value(reader.attributes(), u"multi_name");
                if (!version || !enabled || !parameters || !blend_version || !blend || !priority ||
                    !name || version.value() != "2" || enabled.value() != "0" ||
                    parameters.value() != "00000000" || blend_version.value() != "10" ||
                    blend.value() != kLegacyFlipBlendGz14 || priority.value() != "0" ||
                    !name->empty())
                {
                    return make_error(ErrorCode::kUnsupported,
                                      "Legacy mask manager is outside the frozen dummy state",
                                      {{"legacy_operation", "mask_manager"},
                                       {"reason", "unsupported_legacy_mask_manager"}});
                }
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
            if (operation.value() == "ashift")
            {
                auto mapped = map_legacy_ashift(reader.attributes());
                if (!mapped)
                {
                    return mapped.error();
                }
                ashift_perspective = std::move(mapped).value();
                ++history_index;
                continue;
            }
            if (operation.value() == "rgblevels")
            {
                auto mapped = map_legacy_rgblevels(reader.attributes());
                if (!mapped)
                {
                    return mapped.error();
                }
                rgb_levels = std::move(mapped).value();
                ++history_index;
                continue;
            }
            if (operation.value() == "rgbcurve")
            {
                auto mapped = map_legacy_rgbcurve(reader.attributes());
                if (!mapped)
                {
                    return mapped.error();
                }
                rgb_curve = std::move(mapped).value();
                ++history_index;
                continue;
            }
            if (operation.value() == "rawdenoise")
            {
                auto mapped = map_legacy_rawdenoise(reader.attributes());
                if (!mapped)
                {
                    return mapped.error();
                }
                raw_denoise = std::move(mapped).value();
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
                if (operation.value() == "watermark")
                {
                    return make_error(
                        ErrorCode::kUnsupported,
                        "Legacy Watermark depends on an unversioned external resource",
                        {{"legacy_operation", "watermark"},
                         {"reason", "unsupported_legacy_watermark_resource"}});
                }
                if (operation.value() == "lut3d")
                {
                    return make_error(
                        ErrorCode::kUnsupported,
                        "Legacy 3D LUT state depends on an unversioned external resource",
                        {{"legacy_operation", "lut3d"},
                         {"reason", "unsupported_legacy_lut3d_resource"}});
                }
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
    if (!velvia_candidates.empty())
    {
        auto mapped = map_velvia_candidate(velvia_candidates.front());
        if (!mapped)
            return mapped.error();
        operations.push_back(std::move(mapped).value());
    }
    if (!color_zones_candidates.empty())
    {
        auto mapped = map_color_zones_candidate(color_zones_candidates.front());
        if (!mapped)
            return mapped.error();
        operations.push_back(std::move(mapped).value());
    }
    if (!monochrome_candidates.empty())
    {
        auto mapped = map_monochrome_candidate(monochrome_candidates.front());
        if (!mapped)
            return mapped.error();
        operations.push_back(std::move(mapped).value());
    }
    if (!split_toning_candidates.empty())
    {
        auto mapped = map_split_toning_candidate(split_toning_candidates.front());
        if (!mapped)
            return mapped.error();
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
    if (ashift_perspective && !ashift_perspective->is_identity())
    {
        auto parameters = perspective_to_parameters(*ashift_perspective);
        if (!parameters)
            return parameters.error();
        operations.push_back(OperationInstance{
            std::string(kPerspectiveOperationId), kPerspectiveOperationSchemaVersion,
            "legacy-ashift-perspective", true, std::move(parameters).value(), std::nullopt});
    }
    if (!canvas_candidates.empty())
    {
        auto mapped = map_canvas_candidate(canvas_candidates.front());
        if (!mapped)
            return mapped.error();
        operations.push_back(std::move(mapped).value());
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
    if (rgb_levels && !rgb_levels->is_identity())
    {
        operations.push_back(OperationInstance{"ravo.color.rgblevels", 1, "legacy-rgblevels", true,
                                               rgb_levels_to_parameters(*rgb_levels),
                                               std::nullopt});
    }
    if (rgb_curve && !rgb_curve->is_identity())
    {
        operations.push_back(OperationInstance{"ravo.color.rgbcurve", 1, "legacy-rgbcurve", true,
                                               rgb_curve_to_parameters(*rgb_curve), std::nullopt});
    }
    if (raw_denoise && !raw_denoise->is_identity())
    {
        operations.push_back(OperationInstance{
            "ravo.raw.denoise", 1, "legacy-rawdenoise", true,
            raw_denoise_to_parameters(raw_denoise->threshold, raw_denoise->bands), std::nullopt});
    }
    std::vector<Mask> canonical_masks;
    if (!retouch_candidates.empty())
    {
        const auto winner = std::max_element(
            retouch_candidates.begin(), retouch_candidates.end(),
            [](const LegacyRetouchCandidate &left, const LegacyRetouchCandidate &right)
            { return left.history_position < right.history_position; });
        auto mapped = map_retouch_candidate(*winner, legacy_masks);
        if (!mapped)
        {
            return mapped.error();
        }
        auto params = retouch_from_parameters(mapped.value().operation.parameters);
        if (!params)
        {
            return params.error();
        }
        if (!params.value().is_identity())
        {
            operations.push_back(std::move(mapped.value().operation));
            canonical_masks = std::move(mapped.value().masks);
        }
    }
    else if (!legacy_masks.empty())
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy XMP mask history has no canonical mask mapping",
                          {{"reason", "unsupported_legacy_mask"}});
    }
    if (!sharpen_candidates.empty())
    {
        auto mapped = map_sharpen_candidate(sharpen_candidates.front());
        if (!mapped)
        {
            return mapped.error();
        }
        operations.push_back(std::move(mapped).value());
    }
    if (!dehaze_candidates.empty())
    {
        auto mapped = map_dehaze_candidate(dehaze_candidates.front());
        if (!mapped)
        {
            return mapped.error();
        }
        operations.push_back(std::move(mapped).value());
    }
    std::optional<OperationInstance> output_dither;
    if (!output_dither_candidates.empty())
    {
        auto mapped = map_output_dither_candidate(output_dither_candidates.front());
        if (!mapped)
            return mapped.error();
        output_dither = std::move(mapped).value();
    }
    std::optional<OperationInstance> output_frame;
    if (!frame_candidates.empty())
    {
        auto mapped = map_frame_candidate(frame_candidates.front());
        if (!mapped)
            return mapped.error();
        output_frame = std::move(mapped).value();
    }
    if (!color_reconstruction_candidates.empty())
    {
        auto mapped = map_color_reconstruction_candidate(color_reconstruction_candidates.front());
        if (!mapped)
        {
            return mapped.error();
        }
        operations.push_back(std::move(mapped).value());
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
    if (output_dither)
        operations.push_back(std::move(*output_dither));
    if (output_frame)
        operations.push_back(std::move(*output_frame));
    return Recipe{3, request.asset, std::move(operations), std::move(canonical_masks)};
}

} // namespace ravo
