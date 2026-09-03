#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ravo/domain/types.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"

namespace ravo::detail
{

inline constexpr std::string_view kJpegExifApp1Identifier = std::string_view("Exif\0\0", 6U);
inline constexpr std::string_view kJpegXmpApp1Identifier =
    std::string_view("http://ns.adobe.com/xap/1.0/", 29U);
inline constexpr std::string_view kJpegPhotoshopApp13Identifier =
    std::string_view("Photoshop 3.0\0", 14U);
inline constexpr std::string_view kPngXmpItxtKeyword = "XML:com.adobe.xmp";
inline constexpr std::uint16_t kPhotoshopIptcResourceId = 0x0404U;

struct PreparedExportMetadata
{
    std::uint32_t pixel_width = 0U;
    std::uint32_t pixel_height = 0U;
    std::uint16_t orientation = 1U;
    std::uint16_t color_space = 0xFFFFU;
    std::optional<std::string> make;
    std::optional<std::string> model;
    std::optional<std::uint16_t> iso;
    std::optional<ExportUnsignedRational> aperture;
    std::optional<ExportUnsignedRational> focal_length;
    std::optional<ExportUnsignedRational> shutter;
    std::optional<std::string> title;
    std::optional<std::string> description;
    std::optional<std::string> creator;
    std::optional<std::string> copyright;
    std::optional<std::string> country;
    std::optional<std::string> province_state;
    std::optional<std::string> city;
    std::optional<std::string> sublocation;
    std::vector<std::string> tags;
    std::optional<std::string> datetime_original;
    std::optional<std::string> offset_time_original;
    std::optional<std::string> subsec_time_original;
    bool has_gps = false;
    char gps_latitude_ref = 'N';
    char gps_longitude_ref = 'E';
    std::array<ExportUnsignedRational, 3> gps_latitude{};
    std::array<ExportUnsignedRational, 3> gps_longitude{};
    std::optional<std::uint8_t> gps_altitude_ref;
    std::optional<ExportUnsignedRational> gps_altitude;
    std::optional<std::string> xmp_datetime_original;
    std::optional<std::string> xmp_gps_latitude;
    std::optional<std::string> xmp_gps_longitude;
    std::optional<std::string> xmp_gps_altitude_ref;
    std::optional<std::string> xmp_gps_altitude;
    std::vector<std::uint8_t> exif_tiff_profile;
    std::vector<std::uint8_t> xmp_packet;
    std::optional<std::vector<std::uint8_t>> iptc_iim;
};

[[nodiscard]] std::string xml_escape_utf8(std::string_view text);
[[nodiscard]] Result<std::vector<std::uint8_t>>
build_export_exif_tiff_profile(const PreparedExportMetadata &prepared);
[[nodiscard]] Result<std::vector<std::uint8_t>>
build_export_xmp_packet(const PreparedExportMetadata &prepared);
[[nodiscard]] Result<std::vector<std::uint8_t>>
build_export_iptc_iim(const PreparedExportMetadata &prepared);
[[nodiscard]] Result<std::vector<std::uint8_t>>
build_jpeg_exif_app1_payload(const std::vector<std::uint8_t> &exif_tiff_profile);
[[nodiscard]] Result<std::vector<std::uint8_t>>
build_jpeg_xmp_app1_payload(const std::vector<std::uint8_t> &xmp_packet);
[[nodiscard]] Result<std::vector<std::uint8_t>>
build_jpeg_iptc_app13_payload(const std::vector<std::uint8_t> &iptc_iim);
[[nodiscard]] Result<PreparedExportMetadata>
prepare_export_metadata(const ExportMetadataSnapshot &snapshot, std::uint32_t width,
                        std::uint32_t height, bool builtin_srgb,
                        const CancellationToken &cancellation);

} // namespace ravo::detail
