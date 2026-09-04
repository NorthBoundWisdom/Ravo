#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ravo::test_support
{

struct UnsignedRational
{
    std::uint32_t numerator = 0U;
    std::uint32_t denominator = 1U;
};

struct CaptureExifProfile
{
    std::string datetime = "2007:09:11 13:53:33";
    std::optional<std::string> image_datetime;
    std::optional<std::string> duplicate_photo_datetime;
    std::string offset = "+02:00";
    std::string subsecond = "18";
    std::optional<std::string> lens_make;
    std::optional<std::string> lens_model;
    char latitude_ref = 'N';
    std::array<UnsignedRational, 3> latitude{{{49U, 1U}, {15U, 1U}, {116604U, 10000U}}};
    char longitude_ref = 'E';
    std::array<UnsignedRational, 3> longitude{{{3U, 1U}, {3U, 1U}, {27576U, 10000U}}};
    std::uint8_t altitude_ref = 0U;
    UnsignedRational altitude{15432U, 125U};
};

namespace detail
{

inline void append_u16(std::vector<std::uint8_t> &bytes, const std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}

inline void append_u32(std::vector<std::uint8_t> &bytes, const std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 16U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 24U));
}

inline void patch_u32(std::vector<std::uint8_t> &bytes, const std::size_t offset,
                      const std::uint32_t value)
{
    bytes.at(offset) = static_cast<std::uint8_t>(value);
    bytes.at(offset + 1U) = static_cast<std::uint8_t>(value >> 8U);
    bytes.at(offset + 2U) = static_cast<std::uint8_t>(value >> 16U);
    bytes.at(offset + 3U) = static_cast<std::uint8_t>(value >> 24U);
}

inline std::uint16_t read_u16(const std::vector<std::uint8_t> &bytes, const std::size_t offset)
{
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes.at(offset)) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes.at(offset + 1U)) << 8U));
}

inline std::uint32_t read_u32(const std::vector<std::uint8_t> &bytes, const std::size_t offset)
{
    return static_cast<std::uint32_t>(bytes.at(offset)) |
           (static_cast<std::uint32_t>(bytes.at(offset + 1U)) << 8U) |
           (static_cast<std::uint32_t>(bytes.at(offset + 2U)) << 16U) |
           (static_cast<std::uint32_t>(bytes.at(offset + 3U)) << 24U);
}

inline std::optional<std::size_t> find_entry(const std::vector<std::uint8_t> &bytes,
                                             const std::size_t ifd_offset, const std::uint16_t tag)
{
    const std::uint16_t count = read_u16(bytes, ifd_offset);
    for (std::uint16_t index = 0U; index < count; ++index)
    {
        const std::size_t entry = ifd_offset + 2U + static_cast<std::size_t>(index) * 12U;
        if (read_u16(bytes, entry) == tag)
        {
            return entry;
        }
    }
    return std::nullopt;
}

inline std::size_t append_entry(std::vector<std::uint8_t> &bytes, const std::uint16_t tag,
                                const std::uint16_t type, const std::uint32_t count,
                                const std::uint32_t value)
{
    append_u16(bytes, tag);
    append_u16(bytes, type);
    append_u32(bytes, count);
    const std::size_t value_offset = bytes.size();
    append_u32(bytes, value);
    return value_offset;
}

inline std::uint32_t inline_ascii(const char first, const char second = '\0') noexcept
{
    return static_cast<std::uint32_t>(static_cast<unsigned char>(first)) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(second)) << 8U);
}

inline void append_c_string(std::vector<std::uint8_t> &bytes, const std::string &text)
{
    bytes.insert(bytes.end(), text.begin(), text.end());
    bytes.push_back(0U);
}

inline void append_rational(std::vector<std::uint8_t> &bytes, const UnsignedRational value)
{
    append_u32(bytes, value.numerator);
    append_u32(bytes, value.denominator);
}

} // namespace detail

// A hand-built, one-pixel, little-endian baseline TIFF. It is intentionally
// independent of Ravo's TIFF/Exif serializers and is valid both as an Exif TIFF
// profile and as a standalone raster input.
inline std::vector<std::uint8_t> make_capture_exif_tiff(const CaptureExifProfile &profile = {})
{
    using namespace detail;
    std::vector<std::uint8_t> bytes;
    bytes.reserve(512U);
    bytes.insert(bytes.end(), {'I', 'I'});
    append_u16(bytes, 42U);
    append_u32(bytes, 8U);

    append_u16(bytes, static_cast<std::uint16_t>(12U + (profile.image_datetime ? 1U : 0U)));
    append_entry(bytes, 256U, 4U, 1U, 1U); // ImageWidth
    append_entry(bytes, 257U, 4U, 1U, 1U); // ImageLength
    const auto bits_offset_field = append_entry(bytes, 258U, 3U, 3U, 0U);
    append_entry(bytes, 259U, 3U, 1U, 1U); // Compression=None
    append_entry(bytes, 262U, 3U, 1U, 2U); // RGB
    const auto strip_offset_field = append_entry(bytes, 273U, 4U, 1U, 0U);
    append_entry(bytes, 277U, 3U, 1U, 3U); // SamplesPerPixel
    append_entry(bytes, 278U, 4U, 1U, 1U); // RowsPerStrip
    append_entry(bytes, 279U, 4U, 1U, 3U); // StripByteCounts
    append_entry(bytes, 284U, 3U, 1U, 1U); // PlanarConfiguration
    const auto exif_offset_field = append_entry(bytes, 34665U, 4U, 1U, 0U);
    const auto gps_offset_field = append_entry(bytes, 34853U, 4U, 1U, 0U);
    std::size_t image_datetime_offset_field = 0U;
    if (profile.image_datetime)
    {
        image_datetime_offset_field =
            append_entry(bytes, 0x9003U, 2U,
                         static_cast<std::uint32_t>(profile.image_datetime->size() + 1U), 0U);
    }
    append_u32(bytes, 0U);

    patch_u32(bytes, bits_offset_field, static_cast<std::uint32_t>(bytes.size()));
    append_u16(bytes, 8U);
    append_u16(bytes, 8U);
    append_u16(bytes, 8U);

    if (profile.image_datetime)
    {
        patch_u32(bytes, image_datetime_offset_field, static_cast<std::uint32_t>(bytes.size()));
        append_c_string(bytes, *profile.image_datetime);
        if ((bytes.size() & 1U) != 0U)
        {
            bytes.push_back(0U);
        }
    }

    patch_u32(bytes, exif_offset_field, static_cast<std::uint32_t>(bytes.size()));
    const std::uint16_t exif_count =
        static_cast<std::uint16_t>(3U + (profile.duplicate_photo_datetime ? 1U : 0U) +
                                   (profile.lens_make ? 1U : 0U) + (profile.lens_model ? 1U : 0U));
    append_u16(bytes, exif_count);
    const auto datetime_offset_field = append_entry(
        bytes, 0x9003U, 2U, static_cast<std::uint32_t>(profile.datetime.size() + 1U), 0U);
    std::size_t duplicate_datetime_offset_field = 0U;
    if (profile.duplicate_photo_datetime)
    {
        duplicate_datetime_offset_field = append_entry(
            bytes, 0x9003U, 2U,
            static_cast<std::uint32_t>(profile.duplicate_photo_datetime->size() + 1U), 0U);
    }
    const auto offset_offset_field = append_entry(
        bytes, 0x9011U, 2U, static_cast<std::uint32_t>(profile.offset.size() + 1U), 0U);
    std::size_t subsecond_offset_field = 0U;
    if (profile.subsecond.size() <= 3U)
    {
        std::uint32_t inline_value = 0U;
        for (std::size_t index = 0; index < profile.subsecond.size(); ++index)
        {
            inline_value |=
                static_cast<std::uint32_t>(static_cast<unsigned char>(profile.subsecond[index]))
                << static_cast<unsigned int>(index * 8U);
        }
        append_entry(bytes, 0x9291U, 2U, static_cast<std::uint32_t>(profile.subsecond.size() + 1U),
                     inline_value);
    }
    else
    {
        subsecond_offset_field = append_entry(
            bytes, 0x9291U, 2U, static_cast<std::uint32_t>(profile.subsecond.size() + 1U), 0U);
    }
    std::size_t lens_make_offset_field = 0U;
    if (profile.lens_make)
    {
        if (profile.lens_make->size() <= 3U)
        {
            std::uint32_t inline_value = 0U;
            for (std::size_t index = 0; index < profile.lens_make->size(); ++index)
            {
                inline_value |= static_cast<std::uint32_t>(
                                    static_cast<unsigned char>((*profile.lens_make)[index]))
                                << static_cast<unsigned int>(index * 8U);
            }
            append_entry(bytes, 0xA433U, 2U,
                         static_cast<std::uint32_t>(profile.lens_make->size() + 1U), inline_value);
        }
        else
        {
            lens_make_offset_field = append_entry(
                bytes, 0xA433U, 2U, static_cast<std::uint32_t>(profile.lens_make->size() + 1U), 0U);
        }
    }
    std::size_t lens_model_offset_field = 0U;
    if (profile.lens_model)
    {
        if (profile.lens_model->size() <= 3U)
        {
            std::uint32_t inline_value = 0U;
            for (std::size_t index = 0; index < profile.lens_model->size(); ++index)
            {
                inline_value |= static_cast<std::uint32_t>(
                                    static_cast<unsigned char>((*profile.lens_model)[index]))
                                << static_cast<unsigned int>(index * 8U);
            }
            append_entry(bytes, 0xA434U, 2U,
                         static_cast<std::uint32_t>(profile.lens_model->size() + 1U), inline_value);
        }
        else
        {
            lens_model_offset_field =
                append_entry(bytes, 0xA434U, 2U,
                             static_cast<std::uint32_t>(profile.lens_model->size() + 1U), 0U);
        }
    }
    append_u32(bytes, 0U);
    patch_u32(bytes, datetime_offset_field, static_cast<std::uint32_t>(bytes.size()));
    append_c_string(bytes, profile.datetime);
    if (profile.duplicate_photo_datetime)
    {
        patch_u32(bytes, duplicate_datetime_offset_field, static_cast<std::uint32_t>(bytes.size()));
        append_c_string(bytes, *profile.duplicate_photo_datetime);
    }
    patch_u32(bytes, offset_offset_field, static_cast<std::uint32_t>(bytes.size()));
    append_c_string(bytes, profile.offset);
    if (subsecond_offset_field != 0U)
    {
        patch_u32(bytes, subsecond_offset_field, static_cast<std::uint32_t>(bytes.size()));
        append_c_string(bytes, profile.subsecond);
    }
    if (lens_make_offset_field != 0U)
    {
        patch_u32(bytes, lens_make_offset_field, static_cast<std::uint32_t>(bytes.size()));
        append_c_string(bytes, *profile.lens_make);
    }
    if (lens_model_offset_field != 0U)
    {
        patch_u32(bytes, lens_model_offset_field, static_cast<std::uint32_t>(bytes.size()));
        append_c_string(bytes, *profile.lens_model);
    }
    if ((bytes.size() & 1U) != 0U)
    {
        bytes.push_back(0U);
    }

    patch_u32(bytes, gps_offset_field, static_cast<std::uint32_t>(bytes.size()));
    append_u16(bytes, 7U);
    append_entry(bytes, 0U, 1U, 4U, 0x00000302U); // GPSVersionID 2.3.0.0
    append_entry(bytes, 1U, 2U, 2U, inline_ascii(profile.latitude_ref));
    const auto latitude_offset_field = append_entry(bytes, 2U, 5U, 3U, 0U);
    append_entry(bytes, 3U, 2U, 2U, inline_ascii(profile.longitude_ref));
    const auto longitude_offset_field = append_entry(bytes, 4U, 5U, 3U, 0U);
    append_entry(bytes, 5U, 1U, 1U, profile.altitude_ref);
    const auto altitude_offset_field = append_entry(bytes, 6U, 5U, 1U, 0U);
    append_u32(bytes, 0U);

    patch_u32(bytes, latitude_offset_field, static_cast<std::uint32_t>(bytes.size()));
    for (const auto value : profile.latitude)
    {
        append_rational(bytes, value);
    }
    patch_u32(bytes, longitude_offset_field, static_cast<std::uint32_t>(bytes.size()));
    for (const auto value : profile.longitude)
    {
        append_rational(bytes, value);
    }
    patch_u32(bytes, altitude_offset_field, static_cast<std::uint32_t>(bytes.size()));
    append_rational(bytes, profile.altitude);

    patch_u32(bytes, strip_offset_field, static_cast<std::uint32_t>(bytes.size()));
    bytes.insert(bytes.end(), {20U, 40U, 60U});
    return bytes;
}

inline bool rewrite_linked_ifd_entry(std::vector<std::uint8_t> &bytes,
                                     const std::uint16_t pointer_tag,
                                     const std::uint16_t target_tag,
                                     const std::optional<std::uint16_t> replacement_tag,
                                     const std::optional<std::uint16_t> replacement_type = {},
                                     const std::optional<std::uint32_t> replacement_count = {})
{
    const auto pointer = detail::find_entry(bytes, detail::read_u32(bytes, 4U), pointer_tag);
    if (!pointer)
    {
        return false;
    }
    const std::size_t ifd = detail::read_u32(bytes, *pointer + 8U);
    const auto entry = detail::find_entry(bytes, ifd, target_tag);
    if (!entry)
    {
        return false;
    }
    if (replacement_tag)
    {
        bytes.at(*entry) = static_cast<std::uint8_t>(*replacement_tag);
        bytes.at(*entry + 1U) = static_cast<std::uint8_t>(*replacement_tag >> 8U);
    }
    if (replacement_type)
    {
        bytes.at(*entry + 2U) = static_cast<std::uint8_t>(*replacement_type);
        bytes.at(*entry + 3U) = static_cast<std::uint8_t>(*replacement_type >> 8U);
    }
    if (replacement_count)
    {
        detail::patch_u32(bytes, *entry + 4U, *replacement_count);
    }
    return true;
}

} // namespace ravo::test_support
