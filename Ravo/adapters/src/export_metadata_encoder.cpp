#include "export_metadata_encoder.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <new>
#include <string>
#include <string_view>
#include <utility>

namespace ravo::detail
{
namespace
{

[[nodiscard]] TaskError
export_metadata_error(const ErrorCode code, std::string message, const std::string_view reason,
                      std::map<std::string, std::string, std::less<>> context = {})
{
    context.emplace("reason", reason);
    return make_error(code, std::move(message), std::move(context));
}

void append_bytes(std::vector<std::uint8_t> &out, const std::string_view text)
{
    out.insert(out.end(), text.begin(), text.end());
}

void append_u16_le(std::vector<std::uint8_t> &out, const std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void append_u32_le(std::vector<std::uint8_t> &out, const std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

void write_u16_le(std::vector<std::uint8_t> &out, const std::size_t offset,
                  const std::uint16_t value)
{
    out[offset] = static_cast<std::uint8_t>(value & 0xFFU);
    out[offset + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
}

void write_u32_le(std::vector<std::uint8_t> &out, const std::size_t offset,
                  const std::uint32_t value)
{
    out[offset] = static_cast<std::uint8_t>(value & 0xFFU);
    out[offset + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    out[offset + 2U] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
    out[offset + 3U] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
}

void append_u16_be(std::vector<std::uint8_t> &out, const std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

void append_u32_be(std::vector<std::uint8_t> &out, const std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

struct TiffIfdEntry
{
    std::uint16_t tag = 0U;
    std::uint16_t type = 0U;
    std::uint32_t count = 0U;
    std::vector<std::uint8_t> value;
};

constexpr std::uint16_t kTypeAscii = 2U;
constexpr std::uint16_t kTypeShort = 3U;
constexpr std::uint16_t kTypeLong = 4U;
constexpr std::uint16_t kTypeRational = 5U;

[[nodiscard]] std::vector<std::uint8_t> ascii_bytes(const std::string &text)
{
    std::vector<std::uint8_t> bytes(text.begin(), text.end());
    bytes.push_back(0U);
    return bytes;
}

[[nodiscard]] std::vector<std::uint8_t> short_bytes(const std::uint16_t value)
{
    return {static_cast<std::uint8_t>(value & 0xFFU),
            static_cast<std::uint8_t>((value >> 8U) & 0xFFU), 0U, 0U};
}

[[nodiscard]] std::vector<std::uint8_t> long_bytes(const std::uint32_t value)
{
    return {static_cast<std::uint8_t>(value & 0xFFU),
            static_cast<std::uint8_t>((value >> 8U) & 0xFFU),
            static_cast<std::uint8_t>((value >> 16U) & 0xFFU),
            static_cast<std::uint8_t>((value >> 24U) & 0xFFU)};
}

[[nodiscard]] std::vector<std::uint8_t> rational_bytes(const ExportUnsignedRational value)
{
    std::vector<std::uint8_t> bytes;
    bytes.reserve(8U);
    append_u32_le(bytes, value.numerator);
    append_u32_le(bytes, value.denominator);
    return bytes;
}

[[nodiscard]] TiffIfdEntry make_ascii_entry(const std::uint16_t tag, const std::string &text)
{
    auto bytes = ascii_bytes(text);
    return {tag, kTypeAscii, static_cast<std::uint32_t>(bytes.size()), std::move(bytes)};
}

[[nodiscard]] TiffIfdEntry make_short_entry(const std::uint16_t tag, const std::uint16_t value)
{
    return {tag, kTypeShort, 1U, short_bytes(value)};
}

[[nodiscard]] TiffIfdEntry make_long_entry(const std::uint16_t tag, const std::uint32_t value)
{
    return {tag, kTypeLong, 1U, long_bytes(value)};
}

[[nodiscard]] TiffIfdEntry make_rational_entry(const std::uint16_t tag,
                                               const ExportUnsignedRational value)
{
    return {tag, kTypeRational, 1U, rational_bytes(value)};
}

void append_ifd(std::vector<std::uint8_t> &out, const std::vector<TiffIfdEntry> &entries)
{
    append_u16_le(out, static_cast<std::uint16_t>(entries.size()));
    const std::size_t entries_offset = out.size();
    out.resize(out.size() + (entries.size() * 12U) + 4U, 0U);
    write_u32_le(out, entries_offset + (entries.size() * 12U), 0U);
    std::size_t overflow = out.size();
    for (std::size_t index = 0U; index < entries.size(); ++index)
    {
        const auto &entry = entries[index];
        const std::size_t field = entries_offset + (index * 12U);
        write_u16_le(out, field, entry.tag);
        write_u16_le(out, field + 2U, entry.type);
        write_u32_le(out, field + 4U, entry.count);
        if (entry.value.size() <= 4U)
        {
            for (std::size_t byte = 0U; byte < entry.value.size(); ++byte)
            {
                out[field + 8U + byte] = entry.value[byte];
            }
        }
        else
        {
            write_u32_le(out, field + 8U, static_cast<std::uint32_t>(overflow));
            out.insert(out.end(), entry.value.begin(), entry.value.end());
            if ((out.size() % 2U) != 0U)
            {
                out.push_back(0U);
            }
            overflow = out.size();
        }
    }
}

[[nodiscard]] std::vector<TiffIfdEntry> ifd0_entries(const PreparedExportMetadata &prepared,
                                                     const std::uint32_t exif_ifd_offset)
{
    std::vector<TiffIfdEntry> entries;
    if (prepared.description)
    {
        entries.push_back(make_ascii_entry(270U, *prepared.description));
    }
    if (prepared.make)
    {
        entries.push_back(make_ascii_entry(271U, *prepared.make));
    }
    if (prepared.model)
    {
        entries.push_back(make_ascii_entry(272U, *prepared.model));
    }
    entries.push_back(make_short_entry(274U, prepared.orientation));
    if (prepared.creator)
    {
        entries.push_back(make_ascii_entry(315U, *prepared.creator));
    }
    if (prepared.copyright)
    {
        entries.push_back(make_ascii_entry(33432U, *prepared.copyright));
    }
    entries.push_back(make_long_entry(34665U, exif_ifd_offset));
    return entries;
}

[[nodiscard]] std::vector<TiffIfdEntry> exif_ifd_entries(const PreparedExportMetadata &prepared)
{
    std::vector<TiffIfdEntry> entries;
    if (prepared.shutter)
    {
        entries.push_back(make_rational_entry(33434U, *prepared.shutter));
    }
    if (prepared.aperture)
    {
        entries.push_back(make_rational_entry(33437U, *prepared.aperture));
    }
    if (prepared.iso)
    {
        entries.push_back(make_short_entry(34855U, *prepared.iso));
    }
    if (prepared.focal_length)
    {
        entries.push_back(make_rational_entry(37386U, *prepared.focal_length));
    }
    entries.push_back(make_short_entry(40961U, prepared.color_space));
    entries.push_back(make_long_entry(40962U, prepared.pixel_width));
    entries.push_back(make_long_entry(40963U, prepared.pixel_height));
    return entries;
}

[[nodiscard]] std::size_t ifd_size(const std::vector<TiffIfdEntry> &entries)
{
    std::size_t size = 2U + (entries.size() * 12U) + 4U;
    for (const auto &entry : entries)
    {
        if (entry.value.size() > 4U)
        {
            size += entry.value.size();
            size += entry.value.size() % 2U;
        }
    }
    return size;
}

void append_iptc_dataset(std::vector<std::uint8_t> &out, const std::uint8_t record,
                         const std::uint8_t dataset, const std::string_view value)
{
    out.push_back(0x1CU);
    out.push_back(record);
    out.push_back(dataset);
    append_u16_be(out, static_cast<std::uint16_t>(value.size()));
    append_bytes(out, value);
}

} // namespace

std::string xml_escape_utf8(const std::string_view text)
{
    std::string escaped;
    escaped.reserve(xml_escaped_utf8_size(text));
    for (const char byte : text)
    {
        switch (byte)
        {
        case '&':
            escaped += "&amp;";
            break;
        case '<':
            escaped += "&lt;";
            break;
        case '>':
            escaped += "&gt;";
            break;
        case '"':
            escaped += "&quot;";
            break;
        case '\'':
            escaped += "&apos;";
            break;
        case '\r':
            escaped += "&#xD;";
            break;
        default:
            escaped.push_back(byte);
            break;
        }
    }
    return escaped;
}

Result<std::vector<std::uint8_t>>
build_export_exif_tiff_profile(const PreparedExportMetadata &prepared)
{
    try
    {
        const auto exif_entries = exif_ifd_entries(prepared);
        const std::size_t header_bytes = 8U;
        const auto ifd0_without_exif = ifd0_entries(prepared, 0U);
        // ExifIFD offset is written after IFD0; compute the final offset first.
        const std::uint32_t exif_offset =
            static_cast<std::uint32_t>(header_bytes + ifd_size(ifd0_without_exif));
        const auto ifd0_entries_final = ifd0_entries(prepared, exif_offset);

        std::vector<std::uint8_t> profile;
        profile.reserve(header_bytes + ifd_size(ifd0_entries_final) + ifd_size(exif_entries));
        profile.push_back('I');
        profile.push_back('I');
        append_u16_le(profile, 42U);
        append_u32_le(profile, static_cast<std::uint32_t>(header_bytes));
        append_ifd(profile, ifd0_entries_final);
        if (profile.size() != exif_offset)
        {
            return export_metadata_error(ErrorCode::kInternal,
                                         "Exif IFD0 size does not match the computed offset",
                                         "export_exif_offset_mismatch");
        }
        append_ifd(profile, exif_entries);
        if (profile.size() > kExportExifTiffProfileMaxBytes)
        {
            return export_metadata_error(
                ErrorCode::kValidation, "Export Exif packet exceeds the JPEG APP1 bound",
                "export_exif_packet_too_large",
                {{"maximum_bytes", std::to_string(kExportExifTiffProfileMaxBytes)},
                 {"size_bytes", std::to_string(profile.size())}});
        }
        return profile;
    }
    catch (const std::bad_alloc &)
    {
        return export_metadata_error(ErrorCode::kIo, "Unable to allocate export Exif packet",
                                     "export_metadata_allocation_failed");
    }
}

Result<std::vector<std::uint8_t>> build_export_xmp_packet(const PreparedExportMetadata &prepared)
{
    try
    {
        std::string xml;
        xml += "<?xpacket begin=\"\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n";
        xml += "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n";
        xml += " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n";
        xml += "  <rdf:Description rdf:about=\"\"\n";
        xml += "    xmlns:dc=\"http://purl.org/dc/elements/1.1/\"\n";
        xml += "    xmlns:exif=\"http://ns.adobe.com/exif/1.0/\"\n";
        xml += "    xmlns:tiff=\"http://ns.adobe.com/tiff/1.0/\"\n";
        xml += "    xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\">\n";
        xml += "   <xmp:CreatorTool>";
        xml += xml_escape_utf8(kExportXmpCreatorTool);
        xml += "</xmp:CreatorTool>\n";
        xml += "   <tiff:Orientation>1</tiff:Orientation>\n";
        if (prepared.make)
        {
            xml += "   <tiff:Make>";
            xml += xml_escape_utf8(*prepared.make);
            xml += "</tiff:Make>\n";
        }
        if (prepared.model)
        {
            xml += "   <tiff:Model>";
            xml += xml_escape_utf8(*prepared.model);
            xml += "</tiff:Model>\n";
        }
        xml += "   <exif:ColorSpace>";
        xml += std::to_string(prepared.color_space);
        xml += "</exif:ColorSpace>\n";
        xml += "   <exif:PixelXDimension>";
        xml += std::to_string(prepared.pixel_width);
        xml += "</exif:PixelXDimension>\n";
        xml += "   <exif:PixelYDimension>";
        xml += std::to_string(prepared.pixel_height);
        xml += "</exif:PixelYDimension>\n";
        if (prepared.iso)
        {
            xml += "   <exif:ISOSpeedRatings>\n    <rdf:Seq>\n     <rdf:li>";
            xml += std::to_string(*prepared.iso);
            xml += "</rdf:li>\n    </rdf:Seq>\n   </exif:ISOSpeedRatings>\n";
        }
        if (prepared.aperture)
        {
            xml += "   <exif:FNumber>";
            xml += export_rational_xmp_text(*prepared.aperture);
            xml += "</exif:FNumber>\n";
        }
        if (prepared.focal_length)
        {
            xml += "   <exif:FocalLength>";
            xml += export_rational_xmp_text(*prepared.focal_length);
            xml += "</exif:FocalLength>\n";
        }
        if (prepared.shutter)
        {
            xml += "   <exif:ExposureTime>";
            xml += export_rational_xmp_text(*prepared.shutter);
            xml += "</exif:ExposureTime>\n";
        }
        const auto append_lang_alt =
            [&](const std::string_view element, const std::optional<std::string> &value)
        {
            if (!value)
            {
                return;
            }
            xml += "   <";
            xml += element;
            xml += ">\n    <rdf:Alt>\n     <rdf:li xml:lang=\"x-default\">";
            xml += xml_escape_utf8(*value);
            xml += "</rdf:li>\n    </rdf:Alt>\n   </";
            xml += element;
            xml += ">\n";
        };
        append_lang_alt("dc:title", prepared.title);
        append_lang_alt("dc:description", prepared.description);
        if (prepared.creator)
        {
            xml += "   <dc:creator>\n    <rdf:Seq>\n     <rdf:li>";
            xml += xml_escape_utf8(*prepared.creator);
            xml += "</rdf:li>\n    </rdf:Seq>\n   </dc:creator>\n";
        }
        append_lang_alt("dc:rights", prepared.copyright);
        if (!prepared.tags.empty())
        {
            xml += "   <dc:subject>\n    <rdf:Bag>\n";
            for (const auto &tag : prepared.tags)
            {
                xml += "     <rdf:li>";
                xml += xml_escape_utf8(tag);
                xml += "</rdf:li>\n";
            }
            xml += "    </rdf:Bag>\n   </dc:subject>\n";
        }
        xml += "  </rdf:Description>\n </rdf:RDF>\n</x:xmpmeta>\n";
        xml += "<?xpacket end=\"w\"?>";

        std::vector<std::uint8_t> packet(xml.begin(), xml.end());
        if (packet.size() > kExportXmpPacketMaxBytes)
        {
            return export_metadata_error(
                ErrorCode::kValidation, "Export XMP packet exceeds the JPEG APP1 bound",
                "export_xmp_packet_too_large",
                {{"maximum_bytes", std::to_string(kExportXmpPacketMaxBytes)},
                 {"size_bytes", std::to_string(packet.size())}});
        }
        return packet;
    }
    catch (const std::bad_alloc &)
    {
        return export_metadata_error(ErrorCode::kIo, "Unable to allocate export XMP packet",
                                     "export_metadata_allocation_failed");
    }
}

Result<std::vector<std::uint8_t>> build_export_iptc_iim(const PreparedExportMetadata &prepared)
{
    if (!prepared.title && !prepared.description && !prepared.creator && !prepared.copyright &&
        prepared.tags.empty())
    {
        return export_metadata_error(ErrorCode::kInternal,
                                     "IPTC packet requested when every writable value is absent",
                                     "export_iptc_unexpected_empty");
    }
    try
    {
        std::vector<std::uint8_t> iim;
        constexpr std::array<char, 3> kUtf8Escape{0x1B, '%', 'G'};
        append_iptc_dataset(iim, 1U, 90U, std::string_view(kUtf8Escape.data(), kUtf8Escape.size()));
        constexpr std::array<char, 2> kRecordVersion{0x00, 0x04};
        append_iptc_dataset(iim, 2U, 0U,
                            std::string_view(kRecordVersion.data(), kRecordVersion.size()));
        if (prepared.title)
        {
            append_iptc_dataset(iim, 2U, 5U, *prepared.title);
        }
        for (const auto &tag : prepared.tags)
        {
            append_iptc_dataset(iim, 2U, 25U, tag);
        }
        if (prepared.creator)
        {
            append_iptc_dataset(iim, 2U, 80U, *prepared.creator);
        }
        if (prepared.copyright)
        {
            append_iptc_dataset(iim, 2U, 116U, *prepared.copyright);
        }
        if (prepared.description)
        {
            append_iptc_dataset(iim, 2U, 120U, *prepared.description);
        }
        if (iim.size() > kExportIptcIimMaxBytes)
        {
            return export_metadata_error(ErrorCode::kValidation,
                                         "Export IPTC packet exceeds the JPEG APP13 bound",
                                         "export_iptc_packet_too_large",
                                         {{"maximum_bytes", std::to_string(kExportIptcIimMaxBytes)},
                                          {"size_bytes", std::to_string(iim.size())}});
        }
        return iim;
    }
    catch (const std::bad_alloc &)
    {
        return export_metadata_error(ErrorCode::kIo, "Unable to allocate export IPTC packet",
                                     "export_metadata_allocation_failed");
    }
}

Result<std::vector<std::uint8_t>>
build_jpeg_exif_app1_payload(const std::vector<std::uint8_t> &exif_tiff_profile)
{
    if (kJpegExifApp1Identifier.size() + exif_tiff_profile.size() > kJpegAppMarkerMaxPayloadBytes)
    {
        return export_metadata_error(
            ErrorCode::kValidation, "JPEG Exif APP1 exceeds the marker length",
            "export_exif_packet_too_large",
            {{"maximum_bytes", std::to_string(kExportExifTiffProfileMaxBytes)},
             {"size_bytes", std::to_string(exif_tiff_profile.size())}});
    }
    try
    {
        std::vector<std::uint8_t> payload;
        payload.reserve(kJpegExifApp1Identifier.size() + exif_tiff_profile.size());
        append_bytes(payload, kJpegExifApp1Identifier);
        payload.insert(payload.end(), exif_tiff_profile.begin(), exif_tiff_profile.end());
        return payload;
    }
    catch (const std::bad_alloc &)
    {
        return export_metadata_error(ErrorCode::kIo, "Unable to allocate JPEG Exif APP1",
                                     "export_metadata_allocation_failed");
    }
}

Result<std::vector<std::uint8_t>>
build_jpeg_xmp_app1_payload(const std::vector<std::uint8_t> &xmp_packet)
{
    if (kJpegXmpApp1Identifier.size() + xmp_packet.size() > kJpegAppMarkerMaxPayloadBytes)
    {
        return export_metadata_error(ErrorCode::kValidation,
                                     "JPEG XMP APP1 exceeds the marker length",
                                     "export_xmp_packet_too_large",
                                     {{"maximum_bytes", std::to_string(kExportXmpPacketMaxBytes)},
                                      {"size_bytes", std::to_string(xmp_packet.size())}});
    }
    try
    {
        std::vector<std::uint8_t> payload;
        payload.reserve(kJpegXmpApp1Identifier.size() + xmp_packet.size());
        append_bytes(payload, kJpegXmpApp1Identifier);
        payload.insert(payload.end(), xmp_packet.begin(), xmp_packet.end());
        return payload;
    }
    catch (const std::bad_alloc &)
    {
        return export_metadata_error(ErrorCode::kIo, "Unable to allocate JPEG XMP APP1",
                                     "export_metadata_allocation_failed");
    }
}

Result<std::vector<std::uint8_t>>
build_jpeg_iptc_app13_payload(const std::vector<std::uint8_t> &iptc_iim)
{
    const std::size_t irb = kJpegPhotoshopIrbHeaderBytes + iptc_iim.size() + (iptc_iim.size() % 2U);
    if (kJpegPhotoshopApp13Identifier.size() + irb > kJpegAppMarkerMaxPayloadBytes)
    {
        return export_metadata_error(ErrorCode::kValidation,
                                     "JPEG IPTC APP13 exceeds the marker length",
                                     "export_iptc_packet_too_large",
                                     {{"maximum_bytes", std::to_string(kExportIptcIimMaxBytes)},
                                      {"size_bytes", std::to_string(iptc_iim.size())}});
    }
    try
    {
        std::vector<std::uint8_t> payload;
        append_bytes(payload, kJpegPhotoshopApp13Identifier);
        payload.insert(payload.end(), {'8', 'B', 'I', 'M'});
        append_u16_be(payload, kPhotoshopIptcResourceId);
        payload.push_back(0U);
        payload.push_back(0U);
        append_u32_be(payload, static_cast<std::uint32_t>(iptc_iim.size()));
        payload.insert(payload.end(), iptc_iim.begin(), iptc_iim.end());
        if ((iptc_iim.size() % 2U) != 0U)
        {
            payload.push_back(0U);
        }
        return payload;
    }
    catch (const std::bad_alloc &)
    {
        return export_metadata_error(ErrorCode::kIo, "Unable to allocate JPEG IPTC APP13",
                                     "export_metadata_allocation_failed");
    }
}

Result<PreparedExportMetadata> prepare_export_metadata(const ExportMetadataSnapshot &snapshot,
                                                       const std::uint32_t width,
                                                       const std::uint32_t height,
                                                       const bool builtin_srgb,
                                                       const CancellationToken &cancellation)
{
    auto valid = validate_export_metadata(snapshot, cancellation);
    if (!valid)
    {
        return valid.error();
    }
    try
    {
        PreparedExportMetadata prepared;
        prepared.pixel_width = width;
        prepared.pixel_height = height;
        prepared.orientation = 1U;
        prepared.color_space = builtin_srgb ? 1U : 0xFFFFU;
        prepared.make = snapshot.capture.camera_make;
        prepared.model = snapshot.capture.camera_model;
        prepared.title = snapshot.writable.title;
        prepared.description = snapshot.writable.description;
        prepared.creator = snapshot.writable.creator;
        prepared.copyright = snapshot.writable.copyright;
        prepared.tags = snapshot.tags;
        if (snapshot.capture.iso)
        {
            auto iso = export_photographic_sensitivity(*snapshot.capture.iso);
            if (!iso)
            {
                return iso.error();
            }
            prepared.iso = iso.value();
        }
        auto active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        if (snapshot.capture.aperture)
        {
            auto value = export_positive_rational(*snapshot.capture.aperture);
            if (!value)
            {
                return value.error();
            }
            prepared.aperture = value.value();
        }
        if (snapshot.capture.focal_length_mm)
        {
            auto value = export_positive_rational(*snapshot.capture.focal_length_mm);
            if (!value)
            {
                return value.error();
            }
            prepared.focal_length = value.value();
        }
        if (snapshot.capture.shutter_s)
        {
            auto value = export_positive_rational(*snapshot.capture.shutter_s);
            if (!value)
            {
                return value.error();
            }
            prepared.shutter = value.value();
        }

        active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        auto exif = build_export_exif_tiff_profile(prepared);
        if (!exif)
        {
            return exif.error();
        }
        prepared.exif_tiff_profile = std::move(exif).value();

        active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        auto xmp = build_export_xmp_packet(prepared);
        if (!xmp)
        {
            return xmp.error();
        }
        prepared.xmp_packet = std::move(xmp).value();

        if (!export_iptc_should_omit(snapshot))
        {
            active = cancellation.check();
            if (!active)
            {
                return active.error();
            }
            auto iptc = build_export_iptc_iim(prepared);
            if (!iptc)
            {
                return iptc.error();
            }
            prepared.iptc_iim = std::move(iptc).value();
        }

        auto jpeg_exif = build_jpeg_exif_app1_payload(prepared.exif_tiff_profile);
        if (!jpeg_exif)
        {
            return jpeg_exif.error();
        }
        auto jpeg_xmp = build_jpeg_xmp_app1_payload(prepared.xmp_packet);
        if (!jpeg_xmp)
        {
            return jpeg_xmp.error();
        }
        if (prepared.iptc_iim)
        {
            auto jpeg_iptc = build_jpeg_iptc_app13_payload(*prepared.iptc_iim);
            if (!jpeg_iptc)
            {
                return jpeg_iptc.error();
            }
        }
        return prepared;
    }
    catch (const std::bad_alloc &)
    {
        return export_metadata_error(ErrorCode::kIo, "Unable to allocate prepared export metadata",
                                     "export_metadata_allocation_failed");
    }
}

} // namespace ravo::detail
