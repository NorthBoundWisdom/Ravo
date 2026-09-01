#include "ravo/adapters/qt_raster_decoder.h"

#include "jpeg_encoder.h"
#include "png_encoder.h"
#include "tiff_encoder.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <zlib.h>

#include <QtCore/QBuffer>
#include <QtCore/QByteArray>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QIODevice>
#include <QtGui/QColorSpace>
#include <QtGui/QColorTransform>
#include <QtGui/QImage>
#include <QtGui/QImageIOHandler>
#include <QtGui/QImageReader>
#include <QtGui/QTransform>

#include "qt_raster_internal.h"

namespace ravo::qt_raster_internal
{

[[nodiscard]] std::uint16_t read_u16(const std::span<const std::uint8_t> bytes,
                                     const bool little_endian) noexcept
{
    if (little_endian)
    {
        return static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[0]) |
                                          (static_cast<std::uint16_t>(bytes[1]) << 8U));
    }
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8U) |
                                      static_cast<std::uint16_t>(bytes[1]));
}

[[nodiscard]] std::uint32_t read_u32(const std::span<const std::uint8_t> bytes,
                                     const bool little_endian) noexcept
{
    if (little_endian)
    {
        return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
               (static_cast<std::uint32_t>(bytes[2]) << 16U) |
               (static_cast<std::uint32_t>(bytes[3]) << 24U);
    }
    return read_u32_be(bytes);
}

[[nodiscard]] std::uint64_t read_u64(const std::span<const std::uint8_t> bytes,
                                     const bool little_endian) noexcept
{
    std::uint64_t result = 0U;
    if (little_endian)
    {
        for (unsigned int index = 0U; index < 8U; ++index)
        {
            result |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
        }
    }
    else
    {
        for (unsigned int index = 0U; index < 8U; ++index)
        {
            result = (result << 8U) | bytes[index];
        }
    }
    return result;
}

[[nodiscard]] std::optional<std::uint64_t> tiff_type_size(const std::uint16_t type) noexcept
{
    switch (type)
    {
    case 1U: // BYTE
    case 2U: // ASCII
    case 6U: // SBYTE
    case 7U: // UNDEFINED
        return 1U;
    case 3U: // SHORT
    case 8U: // SSHORT
        return 2U;
    case 4U:  // LONG
    case 9U:  // SLONG
    case 11U: // FLOAT
    case 13U: // IFD
        return 4U;
    case 5U:  // RATIONAL
    case 10U: // SRATIONAL
    case 12U: // DOUBLE
    case 16U: // LONG8
    case 17U: // SLONG8
    case 18U: // IFD8
        return 8U;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] Result<std::vector<std::uint64_t>>
tiff_unsigned_values(const TiffField &field, const std::span<const std::uint8_t> bytes,
                     const bool little_endian, const std::string_view source,
                     const std::uint64_t maximum_count = 1024U)
{
    if (field.count == 0U || field.count > maximum_count)
    {
        return tiff_error(
            ErrorCode::kValidation, "TIFF field count is outside the safe bound", source,
            "invalid_tiff_field_count",
            {{"count", std::to_string(field.count)}, {"tag", std::to_string(field.tag)}});
    }
    std::size_t element_size = 0U;
    switch (field.type)
    {
    case 1U:
        element_size = 1U;
        break;
    case 3U:
        element_size = 2U;
        break;
    case 4U:
    case 13U:
        element_size = 4U;
        break;
    case 16U:
    case 18U:
        element_size = 8U;
        break;
    default:
        return tiff_error(
            ErrorCode::kValidation, "TIFF field has an invalid integer type", source,
            "invalid_tiff_integer_field_type",
            {{"tag", std::to_string(field.tag)}, {"type", std::to_string(field.type)}});
    }
    std::vector<std::uint64_t> result;
    result.reserve(static_cast<std::size_t>(field.count));
    for (std::uint64_t index = 0U; index < field.count; ++index)
    {
        const std::size_t offset =
            field.payload_offset + static_cast<std::size_t>(index) * element_size;
        const auto value = bytes.subspan(offset, element_size);
        if (element_size == 1U)
        {
            result.push_back(value[0]);
        }
        else if (element_size == 2U)
        {
            result.push_back(read_u16(value, little_endian));
        }
        else if (element_size == 4U)
        {
            result.push_back(read_u32(value, little_endian));
        }
        else
        {
            result.push_back(read_u64(value, little_endian));
        }
    }
    return result;
}

[[nodiscard]] Result<TiffContract> parse_tiff_contract(const std::span<const std::uint8_t> bytes,
                                                       const std::string_view source,
                                                       const CancellationToken *const cancellation)
{
    if (!is_tiff_payload(bytes))
    {
        return tiff_error(ErrorCode::kUnsupported, "Input is not a TIFF image", source,
                          "unrecognized_tiff_content");
    }
    const auto invalid =
        [&](std::string message, const std::string_view reason,
            std::map<std::string, std::string, std::less<>> context = {}) -> Result<TiffContract>
    {
        return tiff_error(ErrorCode::kValidation, std::move(message), source, reason,
                          std::move(context));
    };
    const auto unsupported =
        [&](std::string message, const std::string_view reason,
            std::map<std::string, std::string, std::less<>> context = {}) -> Result<TiffContract>
    {
        return tiff_error(ErrorCode::kUnsupported, std::move(message), source, reason,
                          std::move(context));
    };

    auto active = cancellation != nullptr ? cancellation->check() : Result<void>{};
    if (!active)
    {
        return active.error();
    }
    const bool little_endian = bytes[0] == 'I';
    const std::uint16_t magic = read_u16(bytes.subspan(2U, 2U), little_endian);
    const bool big_tiff = magic == 43U;
    const std::size_t header_size = big_tiff ? 16U : 8U;
    if (bytes.size() < header_size)
    {
        return invalid("TIFF header is truncated", "truncated_tiff_header");
    }
    if (big_tiff && (read_u16(bytes.subspan(4U, 2U), little_endian) != 8U ||
                     read_u16(bytes.subspan(6U, 2U), little_endian) != 0U))
    {
        return invalid("BigTIFF offset framing is unsupported or malformed",
                       "invalid_bigtiff_header");
    }
    const std::uint64_t ifd_offset = big_tiff ? read_u64(bytes.subspan(8U, 8U), little_endian) :
                                                read_u32(bytes.subspan(4U, 4U), little_endian);
    const std::size_t count_size = big_tiff ? 8U : 2U;
    const std::size_t entry_size = big_tiff ? 20U : 12U;
    const std::size_t inline_size = big_tiff ? 8U : 4U;
    if (ifd_offset > bytes.size() ||
        bytes.size() - static_cast<std::size_t>(ifd_offset) < count_size)
    {
        return invalid("TIFF primary IFD offset is outside the input", "invalid_tiff_ifd_offset");
    }
    const std::size_t ifd = static_cast<std::size_t>(ifd_offset);
    const std::uint64_t entry_count = big_tiff ?
                                          read_u64(bytes.subspan(ifd, count_size), little_endian) :
                                          read_u16(bytes.subspan(ifd, count_size), little_endian);
    if (entry_count > kTiffMaxIfdEntries)
    {
        return invalid("TIFF primary IFD has too many entries", "oversized_tiff_ifd",
                       {{"entries", std::to_string(entry_count)}});
    }
    const std::size_t entries_begin = ifd + count_size;
    if (entry_count > (bytes.size() - entries_begin) / entry_size)
    {
        return invalid("TIFF primary IFD entries are truncated", "truncated_tiff_ifd");
    }
    const std::size_t entries_bytes = static_cast<std::size_t>(entry_count) * entry_size;
    const std::size_t next_ifd_offset_position = entries_begin + entries_bytes;
    if (bytes.size() - next_ifd_offset_position < inline_size)
    {
        return invalid("TIFF primary IFD next-page pointer is truncated", "truncated_tiff_ifd");
    }

    static constexpr std::array<std::uint16_t, 15U> kRawOwnerTags{
        33421U, 33422U, 41730U, 50706U, 50707U, 50708U, 50721U, 50722U,
        50723U, 50724U, 50725U, 50726U, 50727U, 50728U, 50740U};
    // Catalog falls through to RAW inspection only on kUnsupported. Identify
    // DNG/CFA ownership from the bounded entry table before validating unrelated
    // proprietary payloads, so a camera container is never stolen by raster TIFF.
    for (std::uint64_t index = 0U; index < entry_count; ++index)
    {
        if ((index & 0xFFFU) == 0U && cancellation != nullptr)
        {
            active = cancellation->check();
            if (!active)
            {
                return active.error();
            }
        }
        const std::size_t offset = entries_begin + static_cast<std::size_t>(index) * entry_size;
        const std::uint16_t tag = read_u16(bytes.subspan(offset, 2U), little_endian);
        if (std::find(kRawOwnerTags.begin(), kRawOwnerTags.end(), tag) != kRawOwnerTags.end())
        {
            return unsupported("TIFF container belongs to the camera RAW decoder",
                               "unsupported_tiff_raw_container",
                               {{"raw_tag", std::to_string(tag)}});
        }
    }

    std::map<std::uint16_t, std::vector<TiffField>> fields;
    for (std::uint64_t index = 0U; index < entry_count; ++index)
    {
        if ((index & 0xFFFU) == 0U && cancellation != nullptr)
        {
            active = cancellation->check();
            if (!active)
            {
                return active.error();
            }
        }
        const std::size_t offset = entries_begin + static_cast<std::size_t>(index) * entry_size;
        const auto entry = bytes.subspan(offset, entry_size);
        TiffField field;
        field.tag = read_u16(entry.first(2U), little_endian);
        field.type = read_u16(entry.subspan(2U, 2U), little_endian);
        field.count = big_tiff ? read_u64(entry.subspan(4U, 8U), little_endian) :
                                 read_u32(entry.subspan(4U, 4U), little_endian);
        const auto element_size = tiff_type_size(field.type);
        if (!element_size)
        {
            return invalid(
                "TIFF IFD entry uses an unknown field type", "unknown_tiff_field_type",
                {{"tag", std::to_string(field.tag)}, {"type", std::to_string(field.type)}});
        }
        if (field.count > std::numeric_limits<std::uint64_t>::max() / *element_size)
        {
            return invalid("TIFF IFD field size overflows", "tiff_field_size_overflow",
                           {{"tag", std::to_string(field.tag)}});
        }
        field.payload_size = field.count * *element_size;
        const std::size_t value_offset = offset + (big_tiff ? 12U : 8U);
        std::uint64_t payload_offset = value_offset;
        if (field.payload_size > inline_size)
        {
            payload_offset = big_tiff ?
                                 read_u64(bytes.subspan(value_offset, inline_size), little_endian) :
                                 read_u32(bytes.subspan(value_offset, inline_size), little_endian);
        }
        if (payload_offset > bytes.size() || field.payload_size > bytes.size() - payload_offset)
        {
            return invalid("TIFF IFD field payload is truncated", "truncated_tiff_field_payload",
                           {{"tag", std::to_string(field.tag)}});
        }
        field.payload_offset = static_cast<std::size_t>(payload_offset);
        fields[field.tag].push_back(field);
    }

    if (const auto photometric = fields.find(262U);
        photometric != fields.end() && photometric->second.size() == 1U)
    {
        auto values =
            tiff_unsigned_values(photometric->second.front(), bytes, little_endian, source, 1U);
        if (values && values.value().front() == 32803U)
        {
            return unsupported("TIFF CFA pixels belong to the camera RAW decoder",
                               "unsupported_tiff_raw_container", {{"photometric", "32803"}});
        }
    }
    if (fields.contains(330U))
    {
        return unsupported("TIFF SubIFD/page selection is not supported",
                           "unsupported_tiff_subifd");
    }
    const std::uint64_t next_ifd =
        big_tiff ? read_u64(bytes.subspan(next_ifd_offset_position, inline_size), little_endian) :
                   read_u32(bytes.subspan(next_ifd_offset_position, inline_size), little_endian);
    if (next_ifd != 0U)
    {
        return unsupported("Multi-page TIFF input is not supported", "unsupported_tiff_multi_page");
    }
    static constexpr std::array<std::uint16_t, 4U> kTileTags{322U, 323U, 324U, 325U};
    for (const std::uint16_t tag : kTileTags)
    {
        if (fields.contains(tag))
        {
            return unsupported("Tiled TIFF input is not supported", "unsupported_tiff_tiled");
        }
    }

    static constexpr std::array<std::uint16_t, 15U> kUniqueContractTags{
        254U, 256U, 257U, 258U, 259U, 262U, 273U, 274U, 277U, 278U, 279U, 284U, 317U, 338U, 339U};
    for (const std::uint16_t tag : kUniqueContractTags)
    {
        if (const auto found = fields.find(tag);
            found != fields.end() && found->second.size() != 1U)
        {
            return invalid("TIFF contract field is duplicated", "duplicate_tiff_field",
                           {{"tag", std::to_string(tag)}});
        }
    }
    if (const auto icc = fields.find(34675U); icc != fields.end() && icc->second.size() != 1U)
    {
        return invalid("TIFF ICC profile field is duplicated", "duplicate_tiff_icc_profile");
    }
    struct TiffFieldTypeRule
    {
        std::uint16_t tag;
        std::uint32_t allowed_types;
    };
    constexpr auto type_mask = [](const std::uint16_t type) { return 1U << type; };
    static constexpr std::array<TiffFieldTypeRule, 13U> kFieldTypeRules{{
        {254U, type_mask(4U)},
        {256U, type_mask(3U) | type_mask(4U) | type_mask(16U)},
        {257U, type_mask(3U) | type_mask(4U) | type_mask(16U)},
        {258U, type_mask(3U)},
        {259U, type_mask(3U)},
        {262U, type_mask(3U)},
        {273U, type_mask(3U) | type_mask(4U) | type_mask(16U)},
        {274U, type_mask(3U)},
        {277U, type_mask(3U)},
        {278U, type_mask(3U) | type_mask(4U) | type_mask(16U)},
        {279U, type_mask(3U) | type_mask(4U) | type_mask(16U)},
        {284U, type_mask(3U)},
        {317U, type_mask(3U)},
    }};
    for (const TiffFieldTypeRule &rule : kFieldTypeRules)
    {
        const auto found = fields.find(rule.tag);
        if (found != fields.end() &&
            (rule.allowed_types & type_mask(found->second.front().type)) == 0U)
        {
            return invalid("TIFF contract field has an invalid type",
                           "invalid_tiff_contract_field_type",
                           {{"tag", std::to_string(rule.tag)},
                            {"type", std::to_string(found->second.front().type)}});
        }
    }
    if (const auto sample_format = fields.find(339U);
        sample_format != fields.end() && sample_format->second.front().type != 3U)
    {
        return invalid(
            "TIFF SampleFormat must use SHORT values", "invalid_tiff_contract_field_type",
            {{"tag", "339"}, {"type", std::to_string(sample_format->second.front().type)}});
    }
    if (const auto page_role = fields.find(254U); page_role != fields.end())
    {
        auto values =
            tiff_unsigned_values(page_role->second.front(), bytes, little_endian, source, 1U);
        if (!values)
        {
            return values.error();
        }
        if (values.value().front() != 0U)
        {
            return unsupported("TIFF reduced or mask page cannot be selected as the primary image",
                               "unsupported_tiff_page_role");
        }
    }

    const auto required_values = [&](const std::uint16_t tag,
                                     const std::uint64_t maximum_count =
                                         1024U) -> Result<std::vector<std::uint64_t>>
    {
        const auto found = fields.find(tag);
        if (found == fields.end())
        {
            return tiff_error(ErrorCode::kValidation, "TIFF is missing a required field", source,
                              "missing_tiff_field", {{"tag", std::to_string(tag)}});
        }
        return tiff_unsigned_values(found->second.front(), bytes, little_endian, source,
                                    maximum_count);
    };
    const auto required_scalar = [&](const std::uint16_t tag) -> Result<std::uint64_t>
    {
        auto values = required_values(tag, 1U);
        if (!values)
        {
            return values.error();
        }
        return values.value().front();
    };

    auto width_value = required_scalar(256U);
    auto height_value = required_scalar(257U);
    auto samples_value = required_scalar(277U);
    if (!width_value)
    {
        return width_value.error();
    }
    if (!height_value)
    {
        return height_value.error();
    }
    if (!samples_value)
    {
        return samples_value.error();
    }
    if (width_value.value() == 0U || height_value.value() == 0U ||
        width_value.value() > static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ||
        height_value.value() > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
    {
        return invalid("TIFF dimensions are invalid", "invalid_tiff_dimensions",
                       {{"height", std::to_string(height_value.value())},
                        {"width", std::to_string(width_value.value())}});
    }
    if (samples_value.value() == 0U || samples_value.value() > 4U)
    {
        return unsupported("TIFF sample layout is unsupported", "unsupported_tiff_samples",
                           {{"samples", std::to_string(samples_value.value())}});
    }
    const std::uint16_t samples = static_cast<std::uint16_t>(samples_value.value());
    auto bit_values = required_values(258U, 4U);
    if (!bit_values)
    {
        return bit_values.error();
    }
    if (bit_values.value().size() != 1U && bit_values.value().size() != samples)
    {
        return invalid("TIFF BitsPerSample does not describe every sample",
                       "invalid_tiff_bits_per_sample_count");
    }
    const std::uint64_t bits = bit_values.value().front();
    if (!std::all_of(bit_values.value().begin(), bit_values.value().end(),
                     [&](const auto value) { return value == bits; }))
    {
        return unsupported("Mixed TIFF sample depths are unsupported",
                           "unsupported_tiff_mixed_bit_depth");
    }

    std::vector<std::uint64_t> sample_formats{1U};
    if (const auto sample_format = fields.find(339U); sample_format != fields.end())
    {
        auto values =
            tiff_unsigned_values(sample_format->second.front(), bytes, little_endian, source, 4U);
        if (!values)
        {
            return values.error();
        }
        sample_formats = std::move(values).value();
    }
    if (sample_formats.size() != 1U && sample_formats.size() != samples)
    {
        return invalid("TIFF SampleFormat does not describe every sample",
                       "invalid_tiff_sample_format_count");
    }
    if (std::find(sample_formats.begin(), sample_formats.end(), 3U) != sample_formats.end())
    {
        return unsupported("Floating-point TIFF input is not supported",
                           "unsupported_tiff_float_samples");
    }
    if (!std::all_of(sample_formats.begin(), sample_formats.end(),
                     [](const auto value) { return value == 1U; }))
    {
        return unsupported("Signed or undefined TIFF samples are unsupported",
                           "unsupported_tiff_sample_format");
    }
    if (bits != 8U && bits != 16U)
    {
        return unsupported("TIFF integer bit depth is unsupported", "unsupported_tiff_bit_depth",
                           {{"bits", std::to_string(bits)}});
    }

    const std::uint64_t native_bytes_per_pixel = bits == 16U ? 8U : 4U;
    const std::uint64_t pixel_count = width_value.value() * height_value.value();
    if (pixel_count > kTiffMaxDecodedBytes / native_bytes_per_pixel)
    {
        const std::string worst_case_native_bytes =
            pixel_count > std::numeric_limits<std::uint64_t>::max() / native_bytes_per_pixel ?
                "uint64_overflow" :
                std::to_string(pixel_count * native_bytes_per_pixel);
        return invalid("TIFF native decoded dimensions exceed the allocation bound",
                       "tiff_dimensions_exceed_allocation_limit",
                       {{"limit_bytes", std::to_string(kTiffMaxDecodedBytes)},
                        {"native_bytes_per_pixel", std::to_string(native_bytes_per_pixel)},
                        {"worst_case_native_bytes", worst_case_native_bytes}});
    }

    auto photometric_value = required_scalar(262U);
    if (!photometric_value)
    {
        return photometric_value.error();
    }
    if (photometric_value.value() != 0U && photometric_value.value() != 1U &&
        photometric_value.value() != 2U)
    {
        return unsupported("TIFF photometric interpretation is unsupported",
                           "unsupported_tiff_photometric",
                           {{"photometric", std::to_string(photometric_value.value())}});
    }
    const std::uint16_t base_samples = photometric_value.value() == 2U ? 3U : 1U;
    if (samples != base_samples && samples != base_samples + 1U)
    {
        return unsupported("TIFF grayscale/RGB sample layout is unsupported",
                           "unsupported_tiff_samples", {{"samples", std::to_string(samples)}});
    }

    TiffContract result;
    result.width = static_cast<std::uint32_t>(width_value.value());
    result.height = static_cast<std::uint32_t>(height_value.value());
    result.bit_depth = static_cast<std::uint16_t>(bits);
    if (const auto orientation = fields.find(274U); orientation != fields.end())
    {
        auto values =
            tiff_unsigned_values(orientation->second.front(), bytes, little_endian, source, 1U);
        if (!values)
        {
            return values.error();
        }
        if (values.value().front() < 1U || values.value().front() > 8U)
        {
            return invalid("TIFF orientation is outside the supported range",
                           "invalid_tiff_orientation");
        }
        result.orientation = static_cast<std::uint16_t>(values.value().front());
    }

    const auto extra = fields.find(338U);
    if (samples == base_samples)
    {
        if (extra != fields.end())
        {
            return invalid("TIFF declares ExtraSamples without an extra plane",
                           "invalid_tiff_extra_samples");
        }
    }
    else
    {
        if (extra == fields.end())
        {
            return unsupported("TIFF extra sample semantics are unspecified",
                               "unsupported_tiff_extra_samples");
        }
        if (extra->second.front().type != 3U || extra->second.front().count != 1U)
        {
            return invalid("TIFF ExtraSamples must be one SHORT value",
                           "invalid_tiff_extra_samples_field",
                           {{"count", std::to_string(extra->second.front().count)},
                            {"type", std::to_string(extra->second.front().type)}});
        }
        auto values = tiff_unsigned_values(extra->second.front(), bytes, little_endian, source, 1U);
        if (!values)
        {
            return values.error();
        }
        if (values.value().front() != 1U && values.value().front() != 2U)
        {
            return unsupported("TIFF extra sample is not declared as alpha",
                               "unsupported_tiff_extra_samples");
        }
        result.alpha_value_offset = extra->second.front().payload_offset;
    }

    if (const auto planar = fields.find(284U); planar != fields.end())
    {
        auto values =
            tiff_unsigned_values(planar->second.front(), bytes, little_endian, source, 1U);
        if (!values)
        {
            return values.error();
        }
        if (values.value().front() != 1U)
        {
            return unsupported("Planar TIFF input is unsupported", "unsupported_tiff_planar");
        }
    }
    auto compression_value = required_scalar(259U);
    if (!compression_value)
    {
        return compression_value.error();
    }
    static constexpr std::array<std::uint64_t, 5U> kSupportedCompression{1U, 5U, 8U, 32946U,
                                                                         32773U};
    if (std::find(kSupportedCompression.begin(), kSupportedCompression.end(),
                  compression_value.value()) == kSupportedCompression.end())
    {
        return unsupported("TIFF compression is unsupported", "unsupported_tiff_compression",
                           {{"compression", std::to_string(compression_value.value())}});
    }
    if (const auto predictor = fields.find(317U); predictor != fields.end())
    {
        auto values =
            tiff_unsigned_values(predictor->second.front(), bytes, little_endian, source, 1U);
        if (!values)
        {
            return values.error();
        }
        if (values.value().front() != 1U &&
            !(values.value().front() == 2U &&
              (compression_value.value() == 5U || compression_value.value() == 8U ||
               compression_value.value() == 32946U)))
        {
            return unsupported("TIFF predictor is unsupported", "unsupported_tiff_predictor");
        }
    }

    auto rows_per_strip = required_scalar(278U);
    if (!rows_per_strip)
    {
        return rows_per_strip.error();
    }
    if (rows_per_strip.value() == 0U)
    {
        return invalid("TIFF RowsPerStrip must be nonzero", "invalid_tiff_rows_per_strip");
    }
    const std::uint64_t strip_count = 1U + (height_value.value() - 1U) / rows_per_strip.value();
    if (strip_count == 0U || strip_count > kTiffMaxStripCount)
    {
        return invalid("TIFF strip count exceeds the safe bound", "oversized_tiff_strip_count");
    }
    auto strip_offsets = required_values(273U, kTiffMaxStripCount);
    auto strip_byte_counts = required_values(279U, kTiffMaxStripCount);
    if (!strip_offsets)
    {
        return strip_offsets.error();
    }
    if (!strip_byte_counts)
    {
        return strip_byte_counts.error();
    }
    if (strip_offsets.value().size() != strip_count ||
        strip_byte_counts.value().size() != strip_count)
    {
        return invalid("TIFF strip tables do not cover the primary image",
                       "invalid_tiff_strip_count",
                       {{"expected", std::to_string(strip_count)},
                        {"offsets", std::to_string(strip_offsets.value().size())},
                        {"sizes", std::to_string(strip_byte_counts.value().size())}});
    }
    for (std::size_t index = 0U; index < strip_offsets.value().size(); ++index)
    {
        if ((index & 0xFFFU) == 0U && cancellation != nullptr)
        {
            active = cancellation->check();
            if (!active)
            {
                return active.error();
            }
        }
        const std::uint64_t offset = strip_offsets.value()[index];
        const std::uint64_t size = strip_byte_counts.value()[index];
        if (size == 0U || offset > bytes.size() || size > bytes.size() - offset)
        {
            return invalid("TIFF strip payload is truncated", "truncated_tiff_strip_data",
                           {{"strip", std::to_string(index)}});
        }
        if (compression_value.value() == 1U)
        {
            const std::uint64_t first_row =
                static_cast<std::uint64_t>(index) * rows_per_strip.value();
            const std::uint64_t rows =
                std::min(rows_per_strip.value(), height_value.value() - first_row);
            const std::uint64_t expected = rows * width_value.value() * samples * bits / 8U;
            if (size < expected)
            {
                return invalid("Uncompressed TIFF strip payload is truncated",
                               "truncated_tiff_strip_data",
                               {{"expected", std::to_string(expected)},
                                {"size", std::to_string(size)},
                                {"strip", std::to_string(index)}});
            }
        }
    }

    if (const auto icc = fields.find(34675U); icc != fields.end())
    {
        const TiffField &field = icc->second.front();
        if (field.type != 1U && field.type != 7U)
        {
            return invalid("TIFF ICC profile field has an invalid type",
                           "invalid_tiff_icc_field_type");
        }
        if (field.payload_size == 0U || field.payload_size > kTiffMaxIccBytes)
        {
            return invalid("TIFF ICC profile is empty or too large", "oversized_tiff_icc_profile",
                           {{"size", std::to_string(field.payload_size)}});
        }
        const auto profile =
            bytes.subspan(field.payload_offset, static_cast<std::size_t>(field.payload_size));
        const QByteArray profile_bytes(reinterpret_cast<const char *>(profile.data()),
                                       static_cast<qsizetype>(profile.size()));
        if (cancellation != nullptr)
        {
            active = cancellation->check();
            if (!active)
            {
                return active.error();
            }
        }
        const QColorSpace color_space = QColorSpace::fromIccProfile(profile_bytes);
        if (cancellation != nullptr)
        {
            active = cancellation->check();
            if (!active)
            {
                return active.error();
            }
        }
        if (!color_space.isValid())
        {
            return invalid("TIFF ICC profile is corrupt", "corrupt_tiff_icc_profile");
        }
        if (color_space.colorModel() != QColorSpace::ColorModel::Rgb)
        {
            return unsupported("TIFF ICC profile is not an RGB profile",
                               "unsupported_tiff_icc_color_model");
        }
        result.color_profile.kind = ColorProfileKind::kIcc;
        result.color_profile.model = ColorModel::kRgb;
        result.color_profile.identifier = "embedded_icc";
        result.color_profile.icc_bytes.assign(profile.begin(), profile.end());
    }
    return result;
}

[[nodiscard]] Result<std::uint16_t>
png_exif_orientation(const std::span<const std::uint8_t> payload, const std::string_view source,
                     const CancellationToken *const cancellation)
{
    if (payload.size() < 8U)
    {
        return png_error(ErrorCode::kValidation, "PNG eXIf TIFF header is truncated", source,
                         "truncated_png_exif_header");
    }
    const bool little_endian = payload[0] == 'I' && payload[1] == 'I';
    const bool big_endian = payload[0] == 'M' && payload[1] == 'M';
    if ((!little_endian && !big_endian) || read_u16(payload.subspan(2U, 2U), little_endian) != 42U)
    {
        return png_error(ErrorCode::kValidation, "PNG eXIf TIFF header is malformed", source,
                         "malformed_png_exif_header");
    }
    const std::uint32_t ifd_offset = read_u32(payload.subspan(4U, 4U), little_endian);
    if (ifd_offset > payload.size() || payload.size() - ifd_offset < 2U)
    {
        return png_error(ErrorCode::kValidation, "PNG eXIf IFD offset is invalid", source,
                         "invalid_png_exif_ifd_offset");
    }
    const std::size_t entry_count =
        read_u16(payload.subspan(static_cast<std::size_t>(ifd_offset), 2U), little_endian);
    const std::size_t entries_begin = static_cast<std::size_t>(ifd_offset) + 2U;
    if (entry_count > (payload.size() - entries_begin) / 12U)
    {
        return png_error(ErrorCode::kValidation, "PNG eXIf IFD entries are truncated", source,
                         "truncated_png_exif_ifd");
    }
    std::optional<std::uint16_t> orientation;
    for (std::size_t index = 0U; index < entry_count; ++index)
    {
        if ((index & 0xFFFU) == 0U && cancellation != nullptr)
        {
            auto active = cancellation->check();
            if (!active)
            {
                return active.error();
            }
        }
        const auto entry = payload.subspan(entries_begin + index * 12U, 12U);
        if (read_u16(entry.first(2U), little_endian) != 0x0112U)
        {
            continue;
        }
        if (orientation || read_u16(entry.subspan(2U, 2U), little_endian) != 3U ||
            read_u32(entry.subspan(4U, 4U), little_endian) != 1U)
        {
            return png_error(ErrorCode::kValidation,
                             "PNG eXIf orientation entry is duplicated or malformed", source,
                             "malformed_png_exif_orientation");
        }
        const std::uint16_t value = read_u16(entry.subspan(8U, 2U), little_endian);
        if (value < 1U || value > 8U)
        {
            return png_error(ErrorCode::kValidation, "PNG eXIf orientation is out of range", source,
                             "invalid_png_exif_orientation",
                             {{"orientation", std::to_string(value)}});
        }
        orientation = value;
    }
    return orientation.value_or(1U);
}

} // namespace ravo::qt_raster_internal
