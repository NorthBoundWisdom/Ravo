#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <QByteArray>
#include <QColor>
#include <QColorSpace>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QImage>
#include <QXmlStreamReader>
#include <gtest/gtest.h>

#include "../adapters/src/tiff_encoder.h"
#include "ravo/adapters/filesystem_preview_cache.h"
#include "ravo/adapters/filesystem_recovery_store.h"
#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/domain/raster_decoder.h"
#include "ravo/domain/types.h"
#include "ravo/domain/uri.h"
#include "ravo/engine/engine.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/log.h"
#include "ravo/services/catalog_service.h"

namespace ravo
{
namespace
{

inline constexpr std::uint16_t kTypeAscii = 2U;
inline constexpr std::uint16_t kTypeRational = 5U;
inline constexpr std::uint16_t kTypeUndefined = 7U;
inline constexpr std::uint16_t kTagDocumentName = 269U;
inline constexpr std::uint16_t kTagImageDescription = 270U;
inline constexpr std::uint16_t kTagXResolution = 282U;
inline constexpr std::uint16_t kTagYResolution = 283U;
inline constexpr std::uint16_t kTagResolutionUnit = 296U;
inline constexpr std::uint16_t kTagArtist = 315U;
inline constexpr std::uint16_t kTagCopyright = 33432U;
inline constexpr std::uint16_t kTagXmp = 700U;
inline constexpr std::uint16_t kTagIptc = 33723U;
inline constexpr std::uint16_t kTagExifIfd = 34665U;
inline constexpr std::uint16_t kTagGpsIfd = 34853U;
inline constexpr std::uint16_t kExifDateTimeOriginal = 36867U;
inline constexpr std::uint16_t kExifOffsetTimeOriginal = 36881U;
inline constexpr std::uint16_t kExifSubSecTimeOriginal = 37521U;
inline constexpr std::uint16_t kTagIccProfile = 34675U;
inline constexpr std::uint16_t kExifExposureTime = 33434U;
inline constexpr std::uint16_t kExifFNumber = 33437U;
inline constexpr std::uint16_t kExifIso = 34855U;
inline constexpr std::uint16_t kExifFocalLength = 37386U;
inline constexpr std::uint16_t kExifColorSpace = 40961U;
inline constexpr std::uint16_t kExifPixelX = 40962U;
inline constexpr std::uint16_t kExifPixelY = 40963U;

class MetadataTempDirectory
{
public:
    MetadataTempDirectory()
        : path_(std::filesystem::temp_directory_path() /
                ("ravo-tiff-metadata-" + generate_catalog_id()))
    {
        std::filesystem::create_directories(path_);
    }

    ~MetadataTempDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path &path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

struct DirectoryField
{
    std::uint16_t tag = 0U;
    std::uint16_t type = 0U;
    std::uint32_t count = 0U;
    std::vector<std::uint8_t> payload;
};

struct ClassicTiffDirectory
{
    std::vector<DirectoryField> fields;
    std::uint32_t next_ifd = 0U;
};

[[nodiscard]] std::uint16_t read_u16_le(const std::span<const std::uint8_t> bytes,
                                        const std::size_t offset)
{
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset]) |
                                      (static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U));
}

[[nodiscard]] std::uint32_t read_u32_le(const std::span<const std::uint8_t> bytes,
                                        const std::size_t offset)
{
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

[[nodiscard]] std::optional<std::size_t> field_type_size(const std::uint16_t type)
{
    switch (type)
    {
    case 1U:
    case 2U:
    case 6U:
    case 7U:
        return 1U;
    case 3U:
    case 8U:
        return 2U;
    case 4U:
    case 9U:
    case 11U:
        return 4U;
    case 5U:
    case 10U:
    case 12U:
        return 8U;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<ClassicTiffDirectory>
parse_classic_little_endian_ifd(const std::span<const std::uint8_t> bytes,
                                const std::size_t ifd_offset)
{
    if (ifd_offset > bytes.size() || bytes.size() - ifd_offset < 2U)
    {
        return std::nullopt;
    }
    const std::uint16_t field_count = read_u16_le(bytes, ifd_offset);
    const std::uint64_t directory_bytes = 2U + static_cast<std::uint64_t>(field_count) * 12U + 4U;
    if (directory_bytes > bytes.size() - ifd_offset)
    {
        return std::nullopt;
    }

    ClassicTiffDirectory result;
    result.fields.reserve(field_count);
    for (std::uint16_t index = 0U; index < field_count; ++index)
    {
        const std::size_t entry = ifd_offset + 2U + static_cast<std::size_t>(index) * 12U;
        DirectoryField field;
        field.tag = read_u16_le(bytes, entry);
        field.type = read_u16_le(bytes, entry + 2U);
        field.count = read_u32_le(bytes, entry + 4U);
        const auto item_size = field_type_size(field.type);
        if (!item_size || field.count > std::numeric_limits<std::size_t>::max() / *item_size)
        {
            return std::nullopt;
        }
        const std::size_t payload_size = static_cast<std::size_t>(field.count) * *item_size;
        std::size_t payload_offset = entry + 8U;
        if (payload_size > 4U)
        {
            payload_offset = read_u32_le(bytes, entry + 8U);
        }
        if (payload_offset > bytes.size() || payload_size > bytes.size() - payload_offset)
        {
            return std::nullopt;
        }
        field.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset),
                             bytes.begin() +
                                 static_cast<std::ptrdiff_t>(payload_offset + payload_size));
        result.fields.push_back(std::move(field));
    }
    result.next_ifd = read_u32_le(bytes, ifd_offset + directory_bytes - 4U);
    return result;
}

[[nodiscard]] std::optional<ClassicTiffDirectory>
parse_classic_little_endian_directory(const std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < 8U || bytes[0] != 'I' || bytes[1] != 'I' || read_u16_le(bytes, 2U) != 42U)
    {
        return std::nullopt;
    }
    return parse_classic_little_endian_ifd(bytes, read_u32_le(bytes, 4U));
}

[[nodiscard]] std::optional<std::uint32_t> directory_offset(const DirectoryField *const field)
{
    if (field == nullptr || field->count != 1U)
    {
        return std::nullopt;
    }
    if (field->type == 4U && field->payload.size() == 4U)
    {
        return read_u32_le(field->payload, 0U);
    }
    if (field->type == 16U && field->payload.size() == 8U)
    {
        if (read_u32_le(field->payload, 4U) != 0U)
        {
            return std::nullopt;
        }
        return read_u32_le(field->payload, 0U);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::uint32_t> unsigned_long(const DirectoryField *const field)
{
    if (field == nullptr || field->count != 1U)
    {
        return std::nullopt;
    }
    if (field->type == 4U && field->payload.size() == 4U)
    {
        return read_u32_le(field->payload, 0U);
    }
    if (field->type == 3U && field->payload.size() == 2U)
    {
        return read_u16_le(field->payload, 0U);
    }
    return std::nullopt;
}

[[nodiscard]] bool contains_text(const QByteArray &bytes, const std::string_view needle)
{
    return bytes.indexOf(QByteArray(needle.data(), static_cast<int>(needle.size()))) >= 0;
}

[[nodiscard]] const DirectoryField *unique_field(const ClassicTiffDirectory &directory,
                                                 const std::uint16_t tag)
{
    const DirectoryField *found = nullptr;
    for (const DirectoryField &field : directory.fields)
    {
        if (field.tag != tag)
        {
            continue;
        }
        if (found != nullptr)
        {
            return nullptr;
        }
        found = &field;
    }
    return found;
}

[[nodiscard]] std::vector<std::uint8_t> nul_terminated_bytes(const std::string_view value)
{
    std::vector<std::uint8_t> result(value.begin(), value.end());
    result.push_back(0U);
    return result;
}

[[nodiscard]] std::optional<double> rational_value(const DirectoryField *const field)
{
    if (field == nullptr || field->type != kTypeRational || field->count != 1U ||
        field->payload.size() != 8U)
    {
        return std::nullopt;
    }
    const std::uint32_t numerator = read_u32_le(field->payload, 0U);
    const std::uint32_t denominator = read_u32_le(field->payload, 4U);
    if (denominator == 0U)
    {
        return std::nullopt;
    }
    return static_cast<double>(numerator) / denominator;
}

[[nodiscard]] std::optional<std::string> ascii_value(const DirectoryField *const field)
{
    if (field == nullptr || field->type != kTypeAscii || field->count == 0U ||
        field->payload.size() != field->count || field->payload.back() != 0U ||
        std::find(field->payload.begin(), field->payload.end() - 1, 0U) != field->payload.end() - 1)
    {
        return std::nullopt;
    }
    return std::string(field->payload.begin(), field->payload.end() - 1);
}

[[nodiscard]] std::optional<std::vector<std::pair<std::uint32_t, std::uint32_t>>>
rational_values(const DirectoryField *const field, const std::uint32_t count)
{
    if (field == nullptr || field->type != kTypeRational || field->count != count ||
        field->payload.size() != static_cast<std::size_t>(count) * 8U)
    {
        return std::nullopt;
    }
    std::vector<std::pair<std::uint32_t, std::uint32_t>> values;
    values.reserve(count);
    for (std::uint32_t index = 0U; index < count; ++index)
    {
        values.emplace_back(read_u32_le(field->payload, static_cast<std::size_t>(index) * 8U),
                            read_u32_le(field->payload, static_cast<std::size_t>(index) * 8U + 4U));
    }
    return values;
}

[[nodiscard]] std::optional<std::uint16_t> short_value(const DirectoryField *const field)
{
    if (field == nullptr || field->type != 3U || field->count != 1U || field->payload.size() != 2U)
    {
        return std::nullopt;
    }
    return read_u16_le(field->payload, 0U);
}

[[nodiscard]] std::vector<std::uint8_t> byte_vector(const QByteArray &bytes)
{
    return {reinterpret_cast<const std::uint8_t *>(bytes.constData()),
            reinterpret_cast<const std::uint8_t *>(bytes.constData()) + bytes.size()};
}

[[nodiscard]] std::vector<std::uint8_t> test_pixels(const std::uint32_t width,
                                                    const std::uint32_t height)
{
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 3U);
    for (std::uint32_t row = 0U; row < height; ++row)
    {
        for (std::uint32_t column = 0U; column < width; ++column)
        {
            const std::size_t offset = (static_cast<std::size_t>(row) * width + column) * 3U;
            pixels[offset] = static_cast<std::uint8_t>((column * 17U + row * 3U) & 0xFFU);
            pixels[offset + 1U] =
                static_cast<std::uint8_t>((column * 7U + row * 19U + 31U) & 0xFFU);
            pixels[offset + 2U] =
                static_cast<std::uint8_t>((column * 29U + row * 5U + 73U) & 0xFFU);
        }
    }
    return pixels;
}

[[nodiscard]] QByteArray read_file(const std::filesystem::path &path)
{
    QFile file(QString::fromStdString(path.string()));
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    return file.readAll();
}

template <typename T>
void expect_error(const Result<T> &result, const ErrorCode code, const std::string_view reason,
                  const std::optional<std::string_view> format = "tiff")
{
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, code);
    if (format)
    {
        ASSERT_TRUE(result.error().context.contains("format"));
        EXPECT_EQ(result.error().context.at("format"), *format);
    }
    ASSERT_TRUE(result.error().context.contains("reason"));
    EXPECT_EQ(result.error().context.at("reason"), reason);
}

struct MetadataCheckpointState
{
    CancellationSource cancellation;
    std::uint32_t target_tag = kTagDocumentName;
    bool cancel = false;
    bool fail = false;
    bool observed = false;
};

[[nodiscard]] detail::TiffEncodeInjectedFailure
metadata_checkpoint(void *const context, const detail::TiffEncodeCheckpoint checkpoint,
                    const std::uint32_t progress, const detail::TiffEncodeConfiguration &) noexcept
{
    auto *const state = static_cast<MetadataCheckpointState *>(context);
    if (checkpoint != detail::TiffEncodeCheckpoint::kMetadata || progress != state->target_tag)
    {
        return detail::TiffEncodeInjectedFailure::kNone;
    }
    state->observed = true;
    if (state->cancel)
    {
        (void)state->cancellation.cancel("tiff-metadata-test");
    }
    if (state->fail)
    {
        return detail::TiffEncodeInjectedFailure::kMetadataTagFailure;
    }
    return detail::TiffEncodeInjectedFailure::kNone;
}

class LegacyRasterDouble final : public RasterDecoder
{
public:
    [[nodiscard]] Result<RasterInfo> probe(std::string_view) const override
    {
        return RasterInfo{};
    }

    [[nodiscard]] Result<DecodedRaster> decode(std::string_view, std::uint32_t,
                                               const CancellationToken &) const override
    {
        return DecodedRaster{};
    }

    [[nodiscard]] Result<DecodedRaster> decode_memory(const std::vector<std::uint8_t> &,
                                                      std::uint32_t, const CancellationToken &,
                                                      int) const override
    {
        return DecodedRaster{};
    }

    [[nodiscard]] Result<std::vector<std::uint8_t>>
    encode(std::uint32_t, std::uint32_t, const std::vector<std::uint8_t> &,
           const ColorProfileState &, ExportFormat, const JpegExportOptions &,
           const CancellationToken &, const PngExportOptions &) const override
    {
        ++calls;
        return std::vector<std::uint8_t>{0x42U};
    }

    [[nodiscard]] Result<std::vector<std::uint8_t>>
    encode(std::uint32_t, std::uint32_t, const std::vector<std::uint8_t> &,
           const ColorProfileState &, const ExportFormat format, const JpegExportOptions &,
           const CancellationToken &, const PngExportOptions &, const TiffExportOptions &,
           const ExportMetadataSnapshot &) const override
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy raster double does not own export metadata",
                          {{"format", std::string(export_format_name(format))},
                           {"reason", "unsupported_export_metadata_owner"}});
    }

    [[nodiscard]] Result<std::vector<std::uint8_t>>
    encode(const ExportPixelBuffer &source, const ExportFormat format,
           const JpegExportOptions &jpeg_options, const CancellationToken &cancellation,
           const PngExportOptions &png_options, const TiffExportOptions &tiff_options,
           const ExportMetadataSnapshot &metadata) const override
    {
        const auto *const rgb8 = std::get_if<std::vector<std::uint8_t>>(&source.samples);
        if (rgb8 == nullptr)
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy raster double does not own high-precision sources",
                              {{"reason", "unsupported_tiff_high_precision_source"}});
        }
        return encode(source.width, source.height, *rgb8, source.color_profile, format,
                      jpeg_options, cancellation, png_options, tiff_options, metadata);
    }

    mutable std::size_t calls = 0U;
};

class CapturingMetadataDecoder final : public RasterDecoder
{
public:
    [[nodiscard]] Result<RasterInfo> probe(const std::string_view path) const override
    {
        return delegate_.probe(path);
    }

    [[nodiscard]] Result<DecodedRaster> decode(const std::string_view path,
                                               const std::uint32_t max_edge,
                                               const CancellationToken &cancellation) const override
    {
        return delegate_.decode(path, max_edge, cancellation);
    }

    [[nodiscard]] Result<DecodedRaster> decode_memory(const std::vector<std::uint8_t> &encoded,
                                                      const std::uint32_t max_edge,
                                                      const CancellationToken &cancellation,
                                                      const int rotate_quarters) const override
    {
        return delegate_.decode_memory(encoded, max_edge, cancellation, rotate_quarters);
    }

    [[nodiscard]] Result<std::vector<std::uint8_t>>
    encode(const std::uint32_t width, const std::uint32_t height,
           const std::vector<std::uint8_t> &rgb, const ColorProfileState &profile,
           const ExportFormat format, const JpegExportOptions &jpeg_options,
           const CancellationToken &cancellation,
           const PngExportOptions &png_options) const override
    {
        return delegate_.encode(width, height, rgb, profile, format, jpeg_options, cancellation,
                                png_options);
    }

    [[nodiscard]] Result<std::vector<std::uint8_t>>
    encode(const std::uint32_t width, const std::uint32_t height,
           const std::vector<std::uint8_t> &rgb, const ColorProfileState &profile,
           const ExportFormat format, const JpegExportOptions &jpeg_options,
           const CancellationToken &cancellation, const PngExportOptions &png_options,
           const TiffExportOptions &tiff_options) const override
    {
        return delegate_.encode(width, height, rgb, profile, format, jpeg_options, cancellation,
                                png_options, tiff_options);
    }

    [[nodiscard]] Result<std::vector<std::uint8_t>>
    encode(const std::uint32_t width, const std::uint32_t height,
           const std::vector<std::uint8_t> &rgb, const ColorProfileState &profile,
           const ExportFormat format, const JpegExportOptions &jpeg_options,
           const CancellationToken &cancellation, const PngExportOptions &png_options,
           const TiffExportOptions &tiff_options,
           const ExportMetadataSnapshot &metadata) const override
    {
        ++calls;
        last_format = format;
        last_metadata = metadata;
        return delegate_.encode(width, height, rgb, profile, format, jpeg_options, cancellation,
                                png_options, tiff_options, metadata);
    }

    [[nodiscard]] Result<std::vector<std::uint8_t>>
    encode(const ExportPixelBuffer &source, const ExportFormat format,
           const JpegExportOptions &jpeg_options, const CancellationToken &cancellation,
           const PngExportOptions &png_options, const TiffExportOptions &tiff_options,
           const ExportMetadataSnapshot &metadata) const override
    {
        ++calls;
        last_format = format;
        last_metadata = metadata;
        return delegate_.encode(source, format, jpeg_options, cancellation, png_options,
                                tiff_options, metadata);
    }

    mutable std::size_t calls = 0U;
    mutable ExportFormat last_format = ExportFormat::kPng;
    mutable ExportMetadataSnapshot last_metadata;

private:
    QtRasterDecoder delegate_;
};

TEST(TiffMetadataDomainTest, ValidatesResolutionDocumentNameAndWritableUtf8)
{
    TiffExportOptions defaults;
    EXPECT_EQ(defaults.resolution_dpi, 300);
    EXPECT_TRUE(validate_tiff_export_options(defaults));
    for (const int resolution : {72, 300, 9600})
    {
        TiffExportOptions options;
        options.resolution_dpi = resolution;
        EXPECT_TRUE(validate_tiff_export_options(options));
    }
    for (const int resolution : {71, 9601})
    {
        TiffExportOptions options;
        options.resolution_dpi = resolution;
        expect_error(validate_tiff_export_options(options), ErrorCode::kValidation,
                     "invalid_tiff_resolution");
    }

    ExportMetadataSnapshot metadata;
    metadata.destination_document_name = "/tmp/目录/输出.tif";
    metadata.writable.title = "标题";
    metadata.writable.description = "";
    metadata.writable.creator = "Photographer";
    metadata.writable.copyright = "Copyright ©";
    EXPECT_TRUE(validate_tiff_export_metadata(metadata));

    metadata.destination_document_name.assign(kExportDocumentNameMaxBytes, 'p');
    EXPECT_TRUE(validate_tiff_export_metadata(metadata));
    metadata.destination_document_name.push_back('x');
    expect_error(validate_tiff_export_metadata(metadata), ErrorCode::kValidation,
                 "invalid_tiff_document_name");

    metadata = {};
    metadata.destination_document_name = std::string("bad\0path", 8U);
    expect_error(validate_tiff_export_metadata(metadata), ErrorCode::kValidation,
                 "invalid_tiff_document_name");
    metadata.destination_document_name = std::string("\xC3\x28", 2U);
    expect_error(validate_tiff_export_metadata(metadata), ErrorCode::kValidation,
                 "invalid_tiff_document_name");
    metadata = {};
    metadata.writable.title = std::string("\xE2\x28\xA1", 3U);
    const auto invalid_writable = validate_tiff_export_metadata(metadata);
    expect_error(invalid_writable, ErrorCode::kValidation, "invalid_export_metadata", std::nullopt);
    ASSERT_TRUE(invalid_writable.error().context.contains("field"));
    EXPECT_EQ(invalid_writable.error().context.at("field"), "title");
}

TEST(TiffMetadataAdapterTest, WritesBaselineAndExtendedMetadataDirectories)
{
    const auto pixels = test_pixels(17U, 9U);
    const auto pixels_before = pixels;
    const auto icc = byte_vector(QColorSpace(QColorSpace::SRgb).iccProfile());
    const auto icc_before = icc;
    ASSERT_FALSE(icc.empty());

    TiffExportOptions options;
    options.resolution_dpi = 450;
    ExportMetadataSnapshot metadata;
    metadata.destination_document_name = "/tmp/目录/output.tif";
    metadata.writable.title = "Title stays out of baseline TIFF tags";
    metadata.writable.description = "Edited description";
    metadata.writable.creator = "Creator Ω";
    metadata.writable.copyright = "Copyright © 2026";
    metadata.capture.iso = 200.0;
    metadata.capture.aperture = 2.8;
    metadata.capture.focal_length_mm = 50.0;
    metadata.capture.shutter_s = 0.008;

    const auto encoded =
        detail::encode_tiff_rgb8(17U, 9U, pixels, icc, options, metadata, CancellationToken{});
    ASSERT_TRUE(encoded) << encoded.error().message;
    const auto directory = parse_classic_little_endian_directory(encoded.value());
    ASSERT_TRUE(directory);
    EXPECT_EQ(directory->next_ifd, 0U);

    const auto expect_ascii = [&](const std::uint16_t tag, const std::string_view value)
    {
        const DirectoryField *const field = unique_field(*directory, tag);
        ASSERT_NE(field, nullptr) << tag;
        EXPECT_EQ(field->type, kTypeAscii) << tag;
        EXPECT_EQ(field->count, value.size() + 1U) << tag;
        EXPECT_EQ(field->payload, nul_terminated_bytes(value)) << tag;
    };
    expect_ascii(kTagDocumentName, metadata.destination_document_name);
    expect_ascii(kTagImageDescription, *metadata.writable.description);
    expect_ascii(kTagArtist, *metadata.writable.creator);
    expect_ascii(kTagCopyright, *metadata.writable.copyright);
    EXPECT_DOUBLE_EQ(rational_value(unique_field(*directory, kTagXResolution)).value_or(0.0),
                     450.0);
    EXPECT_DOUBLE_EQ(rational_value(unique_field(*directory, kTagYResolution)).value_or(0.0),
                     450.0);
    EXPECT_EQ(short_value(unique_field(*directory, kTagResolutionUnit)), 2U);

    const DirectoryField *const icc_field = unique_field(*directory, kTagIccProfile);
    ASSERT_NE(icc_field, nullptr);
    EXPECT_EQ(icc_field->type, kTypeUndefined);
    EXPECT_EQ(icc_field->payload, icc);
    EXPECT_NE(unique_field(*directory, kTagXmp), nullptr);
    EXPECT_NE(unique_field(*directory, kTagIptc), nullptr);
    const auto exif_offset = directory_offset(unique_field(*directory, kTagExifIfd));
    ASSERT_TRUE(exif_offset);
    const auto exif = parse_classic_little_endian_ifd(encoded.value(), *exif_offset);
    ASSERT_TRUE(exif);
    EXPECT_EQ(exif->next_ifd, 0U);
    EXPECT_EQ(short_value(unique_field(*exif, kExifColorSpace)), 0xFFFFU);
    EXPECT_EQ(unsigned_long(unique_field(*exif, kExifPixelX)), 17U);
    EXPECT_EQ(unsigned_long(unique_field(*exif, kExifPixelY)), 9U);
    EXPECT_EQ(short_value(unique_field(*exif, kExifIso)), 200U);
    ASSERT_TRUE(rational_value(unique_field(*exif, kExifExposureTime)));
    EXPECT_NEAR(*rational_value(unique_field(*exif, kExifExposureTime)), 0.008, 1e-6);
    ASSERT_TRUE(rational_value(unique_field(*exif, kExifFNumber)));
    EXPECT_NEAR(*rational_value(unique_field(*exif, kExifFNumber)), 2.8, 1e-5);
    ASSERT_TRUE(rational_value(unique_field(*exif, kExifFocalLength)));
    EXPECT_NEAR(*rational_value(unique_field(*exif, kExifFocalLength)), 50.0, 1e-5);
    const auto xmp = std::string(unique_field(*directory, kTagXmp)->payload.begin(),
                                 unique_field(*directory, kTagXmp)->payload.end());
    EXPECT_NE(xmp.find("<xmp:CreatorTool>Ravo</xmp:CreatorTool>"), std::string::npos);
    EXPECT_NE(xmp.find("<dc:title>"), std::string::npos);
    EXPECT_EQ(xmp.find("DateTimeOriginal"), std::string::npos);
    EXPECT_EQ(pixels, pixels_before);
    EXPECT_EQ(icc, icc_before);
    EXPECT_EQ(metadata.writable.title, "Title stays out of baseline TIFF tags");
}

TEST(TiffMetadataAdapterTest, DistinguishesAbsentFromPresentEmptyAndPreservesOldOverload)
{
    const auto pixels = test_pixels(8U, 8U);
    const auto icc = byte_vector(QColorSpace(QColorSpace::SRgb).iccProfile());
    ASSERT_FALSE(icc.empty());

    ExportMetadataSnapshot metadata;
    metadata.writable.description = "";
    const auto encoded = detail::encode_tiff_rgb8(8U, 8U, pixels, icc, TiffExportOptions{},
                                                  metadata, CancellationToken{});
    ASSERT_TRUE(encoded) << encoded.error().message;
    const auto directory = parse_classic_little_endian_directory(encoded.value());
    ASSERT_TRUE(directory);
    EXPECT_EQ(unique_field(*directory, kTagDocumentName), nullptr);
    const DirectoryField *const description = unique_field(*directory, kTagImageDescription);
    ASSERT_NE(description, nullptr);
    EXPECT_EQ(description->type, kTypeAscii);
    EXPECT_EQ(description->count, 1U);
    EXPECT_EQ(description->payload, std::vector<std::uint8_t>{0U});
    EXPECT_EQ(unique_field(*directory, kTagArtist), nullptr);
    EXPECT_EQ(unique_field(*directory, kTagCopyright), nullptr);

    const auto old_overload =
        detail::encode_tiff_rgb8(8U, 8U, pixels, icc, TiffExportOptions{}, CancellationToken{});
    ASSERT_TRUE(old_overload) << old_overload.error().message;
    const auto old_directory = parse_classic_little_endian_directory(old_overload.value());
    ASSERT_TRUE(old_directory);
    EXPECT_EQ(unique_field(*old_directory, kTagDocumentName), nullptr);
    EXPECT_DOUBLE_EQ(rational_value(unique_field(*old_directory, kTagXResolution)).value_or(0.0),
                     300.0);
}

TEST(TiffMetadataAdapterTest, CancelsAndFailsMetadataTagsWithoutReturningBytesOrMutatingInputs)
{
    const auto pixels = test_pixels(64U, 16U);
    const auto pixels_before = pixels;
    const auto icc = byte_vector(QColorSpace(QColorSpace::SRgb).iccProfile());
    const auto icc_before = icc;
    ExportMetadataSnapshot metadata;
    metadata.destination_document_name = "/tmp/cancelled-metadata.tif";
    metadata.writable.description = "description";
    const auto metadata_before = metadata;

    MetadataCheckpointState cancel_state;
    cancel_state.cancel = true;
    detail::TiffEncodeControl cancel_control;
    cancel_control.checkpoint_observer = {&cancel_state, metadata_checkpoint};
    const auto cancelled =
        detail::encode_tiff_rgb8(64U, 16U, pixels, icc, TiffExportOptions{}, metadata,
                                 cancel_state.cancellation.token(), cancel_control);
    expect_error(cancelled, ErrorCode::kCancelled, "tiff_encode_cancelled");
    EXPECT_TRUE(cancel_state.observed);

    MetadataCheckpointState failure_state;
    failure_state.fail = true;
    detail::TiffEncodeControl failure_control;
    failure_control.checkpoint_observer = {&failure_state, metadata_checkpoint};
    const auto failed = detail::encode_tiff_rgb8(64U, 16U, pixels, icc, TiffExportOptions{},
                                                 metadata, CancellationToken{}, failure_control);
    expect_error(failed, ErrorCode::kIo, "tiff_metadata_tag_failed");
    ASSERT_TRUE(failed.error().context.contains("tag"));
    EXPECT_EQ(failed.error().context.at("tag"), "269");
    EXPECT_TRUE(failure_state.observed);

    EXPECT_EQ(pixels, pixels_before);
    EXPECT_EQ(icc, icc_before);
    EXPECT_EQ(metadata, metadata_before);
}

TEST(TiffMetadataPortTest, LegacyDoubleExplicitlyRejectsEveryMetadataSnapshot)
{
    LegacyRasterDouble concrete;
    const RasterDecoder &decoder = concrete;
    ExportMetadataSnapshot metadata;
    metadata.destination_document_name = "/tmp/output.tif";
    const auto rejected = decoder.encode(
        1U, 1U, {1U, 2U, 3U}, ColorProfileState{}, ExportFormat::kTiff, JpegExportOptions{},
        CancellationToken{}, PngExportOptions{}, TiffExportOptions{}, metadata);
    expect_error(rejected, ErrorCode::kUnsupported, "unsupported_export_metadata_owner");
    EXPECT_EQ(concrete.calls, 0U);

    metadata.destination_document_name = std::string("bad\0ignored", 11U);
    const auto unrelated = decoder.encode(
        1U, 1U, {1U, 2U, 3U}, ColorProfileState{}, ExportFormat::kPng, JpegExportOptions{},
        CancellationToken{}, PngExportOptions{}, TiffExportOptions{}, metadata);
    expect_error(unrelated, ErrorCode::kUnsupported, "unsupported_export_metadata_owner", "png");
    EXPECT_EQ(concrete.calls, 0U);

    const auto empty = decoder.encode(1U, 1U, {1U, 2U, 3U}, ColorProfileState{}, ExportFormat::kPng,
                                      JpegExportOptions{}, CancellationToken{}, PngExportOptions{},
                                      TiffExportOptions{}, ExportMetadataSnapshot{});
    expect_error(empty, ErrorCode::kUnsupported, "unsupported_export_metadata_owner", "png");
    EXPECT_EQ(concrete.calls, 0U);

    const auto metadata_free =
        decoder.encode(1U, 1U, {1U, 2U, 3U}, ColorProfileState{}, ExportFormat::kPng,
                       JpegExportOptions{}, CancellationToken{}, PngExportOptions{});
    ASSERT_TRUE(metadata_free) << metadata_free.error().message;
    EXPECT_EQ(metadata_free.value(), std::vector<std::uint8_t>{0x42U});
    EXPECT_EQ(concrete.calls, 1U);
}

TEST(TiffMetadataCatalogTest, SnapshotsPublicMetadataForEveryRenderedFormat)
{
    MetadataTempDirectory temporary;
    const auto input_path = temporary.path() / "source.png";
    QImage source(24, 18, QImage::Format_RGB888);
    for (int row = 0; row < source.height(); ++row)
    {
        for (int column = 0; column < source.width(); ++column)
        {
            source.setPixelColor(column, row,
                                 QColor((column * 7 + row * 3) % 256,
                                        (column * 13 + row * 11) % 256,
                                        (column * 19 + row * 5) % 256));
        }
    }
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    ASSERT_TRUE(source.save(QString::fromStdString(input_path.string()), "PNG"));
    const auto sidecar_path = temporary.path() / "source.png.xmp";
    {
        QFile sidecar(QString::fromStdString(sidecar_path.string()));
        ASSERT_TRUE(sidecar.open(QIODevice::WriteOnly));
        ASSERT_EQ(sidecar.write("frozen-sidecar", 14), 14);
    }
    const QByteArray source_hash =
        QCryptographicHash::hash(read_file(input_path), QCryptographicHash::Sha256);
    const QByteArray sidecar_hash =
        QCryptographicHash::hash(read_file(sidecar_path), QCryptographicHash::Sha256);
    const std::uintmax_t source_size = std::filesystem::file_size(input_path);
    const std::uintmax_t sidecar_size = std::filesystem::file_size(sidecar_path);
    const auto source_mtime = std::filesystem::last_write_time(input_path);
    const auto sidecar_mtime = std::filesystem::last_write_time(sidecar_path);

    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    auto repository =
        SqliteCatalogRepository::create((temporary.path() / "library.sqlite").string());
    ASSERT_TRUE(repository) << repository.error().message;
    auto cache = FilesystemPreviewCache::create((temporary.path() / "preview").string());
    ASSERT_TRUE(cache) << cache.error().message;
    auto recovery = FilesystemRecoveryStore::create((temporary.path() / "recovery").string());
    ASSERT_TRUE(recovery) << recovery.error().message;
    auto raster = std::make_unique<CapturingMetadataDecoder>();
    CapturingMetadataDecoder *const capturing = raster.get();
    CatalogService service(std::move(engine).value(), std::move(repository).value(),
                           std::move(raster), std::move(cache).value(),
                           std::move(recovery).value());

    const auto imported = service.import_one(input_path.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    WritableMetadata writable;
    writable.title = "Catalog title";
    writable.description = "Catalog description";
    writable.creator = "Catalog creator";
    writable.copyright = "Catalog copyright";
    const auto updated = service.set_writable_metadata(imported.value().asset->id, writable);
    ASSERT_TRUE(updated) << updated.error().message;
    const auto tagged = service.set_tags(imported.value().asset->id, {"zeta", "alpha"});
    ASSERT_TRUE(tagged) << tagged.error().message;

    ExportRequest request;
    request.asset_id = imported.value().asset->id;
    request.output_path = (temporary.path() / "unused" / ".." / "metadata.tif").string();
    request.format = ExportFormat::kTiff;
    request.tiff_options.resolution_dpi = 600;
    const auto normalized_output = normalize_local_input(request.output_path);
    ASSERT_TRUE(normalized_output) << normalized_output.error().message;
    const auto exported = service.export_asset(request);
    ASSERT_TRUE(exported) << exported.error().message;
    ASSERT_EQ(capturing->calls, 1U);
    EXPECT_EQ(capturing->last_format, ExportFormat::kTiff);
    EXPECT_EQ(capturing->last_metadata.destination_document_name, normalized_output.value().path);
    EXPECT_EQ(capturing->last_metadata.writable, writable);
    EXPECT_EQ(capturing->last_metadata.tags, (std::vector<std::string>{"alpha", "zeta"}));
    EXPECT_TRUE(std::filesystem::is_regular_file(normalized_output.value().path));

    const auto directory = parse_classic_little_endian_directory(
        byte_vector(read_file(normalized_output.value().path)));
    ASSERT_TRUE(directory);
    const DirectoryField *const document_name = unique_field(*directory, kTagDocumentName);
    ASSERT_NE(document_name, nullptr);
    EXPECT_EQ(document_name->payload, nul_terminated_bytes(normalized_output.value().path));
    const DirectoryField *const description = unique_field(*directory, kTagImageDescription);
    ASSERT_NE(description, nullptr);
    EXPECT_EQ(description->payload, nul_terminated_bytes(*writable.description));
    EXPECT_DOUBLE_EQ(rational_value(unique_field(*directory, kTagXResolution)).value_or(0.0),
                     600.0);
    const auto catalog_exif_offset = directory_offset(unique_field(*directory, kTagExifIfd));
    ASSERT_TRUE(catalog_exif_offset);
    const auto catalog_exif = parse_classic_little_endian_ifd(
        byte_vector(read_file(normalized_output.value().path)), *catalog_exif_offset);
    ASSERT_TRUE(catalog_exif);
    EXPECT_EQ(short_value(unique_field(*catalog_exif, kExifColorSpace)), 1U);
    EXPECT_NE(unique_field(*directory, kTagXmp), nullptr);
    const auto catalog_xmp = std::string(unique_field(*directory, kTagXmp)->payload.begin(),
                                         unique_field(*directory, kTagXmp)->payload.end());
    EXPECT_NE(catalog_xmp.find("Catalog title"), std::string::npos);
    EXPECT_NE(catalog_xmp.find("<rdf:li>alpha</rdf:li>"), std::string::npos);
    EXPECT_LT(catalog_xmp.find("<rdf:li>alpha</rdf:li>"),
              catalog_xmp.find("<rdf:li>zeta</rdf:li>"));
    EXPECT_NE(unique_field(*directory, kTagIptc), nullptr);

    ExportRequest unrelated = request;
    unrelated.format = ExportFormat::kPng;
    unrelated.output_path = (temporary.path() / "unrelated.png").string();
    const auto png = service.export_asset(unrelated);
    ASSERT_TRUE(png) << png.error().message;
    EXPECT_EQ(capturing->last_format, ExportFormat::kPng);
    EXPECT_TRUE(capturing->last_metadata.destination_document_name.empty());
    EXPECT_EQ(capturing->last_metadata.writable, writable);
    EXPECT_EQ(capturing->last_metadata.tags, (std::vector<std::string>{"alpha", "zeta"}));

    const auto png_bytes = read_file(unrelated.output_path);
    EXPECT_TRUE(contains_text(png_bytes, "eXIf"));
    EXPECT_TRUE(contains_text(png_bytes, "XML:com.adobe.xmp"));
    EXPECT_TRUE(contains_text(png_bytes, "<xmp:CreatorTool>Ravo</xmp:CreatorTool>"));
    EXPECT_TRUE(contains_text(png_bytes, "Catalog title"));
    EXPECT_TRUE(contains_text(png_bytes, "<rdf:li>alpha</rdf:li>"));
    EXPECT_FALSE(contains_text(png_bytes, "pHYs"));
    EXPECT_FALSE(contains_text(png_bytes, "DateTimeOriginal"));

    ExportRequest png_repeat = unrelated;
    png_repeat.output_path = (temporary.path() / "unrelated-b.png").string();
    const auto png_again = service.export_asset(png_repeat);
    ASSERT_TRUE(png_again) << png_again.error().message;
    EXPECT_EQ(read_file(unrelated.output_path), read_file(png_repeat.output_path));

    ExportRequest jpeg = request;
    jpeg.format = ExportFormat::kJpeg;
    jpeg.output_path = (temporary.path() / "metadata.jpg").string();
    const auto jpeg_export = service.export_asset(jpeg);
    ASSERT_TRUE(jpeg_export) << jpeg_export.error().message;
    EXPECT_EQ(capturing->last_format, ExportFormat::kJpeg);
    EXPECT_TRUE(capturing->last_metadata.destination_document_name.empty());
    EXPECT_EQ(capturing->last_metadata.writable, writable);
    EXPECT_EQ(capturing->last_metadata.tags, (std::vector<std::string>{"alpha", "zeta"}));
    const auto jpeg_bytes = read_file(jpeg.output_path);
    EXPECT_TRUE(contains_text(jpeg_bytes, std::string("Exif\0\0", 6)));
    EXPECT_TRUE(contains_text(jpeg_bytes, "http://ns.adobe.com/xap/1.0/"));
    EXPECT_TRUE(contains_text(jpeg_bytes, "Photoshop 3.0"));
    EXPECT_TRUE(contains_text(jpeg_bytes, "Catalog title"));
    EXPECT_FALSE(contains_text(jpeg_bytes, "DateTimeOriginal"));
    ExportRequest jpeg_repeat = jpeg;
    jpeg_repeat.output_path = (temporary.path() / "metadata-b.jpg").string();
    const auto jpeg_again = service.export_asset(jpeg_repeat);
    ASSERT_TRUE(jpeg_again) << jpeg_again.error().message;
    EXPECT_EQ(read_file(jpeg.output_path), read_file(jpeg_repeat.output_path));

    ExportRequest tiff_repeat = request;
    tiff_repeat.output_path = (temporary.path() / "metadata-b.tif").string();
    const auto normalized_repeat = normalize_local_input(tiff_repeat.output_path);
    ASSERT_TRUE(normalized_repeat) << normalized_repeat.error().message;
    const auto tiff_again = service.export_asset(tiff_repeat);
    ASSERT_TRUE(tiff_again) << tiff_again.error().message;
    const auto repeat_directory = parse_classic_little_endian_directory(
        byte_vector(read_file(normalized_repeat.value().path)));
    ASSERT_TRUE(repeat_directory);
    const DirectoryField *const repeat_name = unique_field(*repeat_directory, kTagDocumentName);
    ASSERT_NE(repeat_name, nullptr);
    EXPECT_EQ(repeat_name->payload, nul_terminated_bytes(normalized_repeat.value().path));
    EXPECT_NE(read_file(normalized_output.value().path), read_file(normalized_repeat.value().path));

    const auto conflict = service.export_asset(request);
    EXPECT_FALSE(conflict);
    EXPECT_EQ(conflict.error().code, ErrorCode::kConflict);

    EXPECT_EQ(QCryptographicHash::hash(read_file(input_path), QCryptographicHash::Sha256),
              source_hash);
    EXPECT_EQ(QCryptographicHash::hash(read_file(sidecar_path), QCryptographicHash::Sha256),
              sidecar_hash);
    EXPECT_EQ(std::filesystem::file_size(input_path), source_size);
    EXPECT_EQ(std::filesystem::file_size(sidecar_path), sidecar_size);
    EXPECT_EQ(std::filesystem::last_write_time(input_path), source_mtime);
    EXPECT_EQ(std::filesystem::last_write_time(sidecar_path), sidecar_mtime);
    EXPECT_FALSE(std::filesystem::exists(normalized_output.value().path + ".xmp"));
    EXPECT_FALSE(std::filesystem::exists(unrelated.output_path + ".xmp"));
    EXPECT_FALSE(std::filesystem::exists(jpeg.output_path + ".xmp"));
    EXPECT_TRUE(service.close());
}

TEST(TiffMetadataAdapterTest, WritesGpsDirectoryAndInjectsLifecycleFailures)
{
    const auto pixels = test_pixels(8U, 4U);
    const auto icc = byte_vector(QColorSpace(QColorSpace::SRgb).iccProfile());
    ExportMetadataSnapshot metadata;
    metadata.destination_document_name = "/tmp/gps.tif";
    metadata.capture.captured_datetime = CaptureDateTime{"2007:09:11 13:53:33", "18", 120};
    metadata.capture.location = CaptureLocation{
        49253239, 3050766, CaptureAltitude{123456U, CaptureAltitudeReference::kAboveSeaLevel}};

    const auto encoded = detail::encode_tiff_rgb8(8U, 4U, pixels, icc, TiffExportOptions{},
                                                  metadata, CancellationToken{});
    ASSERT_TRUE(encoded) << encoded.error().message;
    const auto directory = parse_classic_little_endian_directory(encoded.value());
    ASSERT_TRUE(directory);
    EXPECT_EQ(directory->next_ifd, 0U);
    const auto gps_offset = directory_offset(unique_field(*directory, kTagGpsIfd));
    ASSERT_TRUE(gps_offset);
    const auto gps = parse_classic_little_endian_ifd(encoded.value(), *gps_offset);
    ASSERT_TRUE(gps);
    EXPECT_EQ(gps->next_ifd, 0U);
    const DirectoryField *const lat_ref = unique_field(*gps, 1U);
    const DirectoryField *const version = unique_field(*gps, 0U);
    const DirectoryField *const latitude = unique_field(*gps, 2U);
    const DirectoryField *const lon_ref = unique_field(*gps, 3U);
    const DirectoryField *const longitude = unique_field(*gps, 4U);
    const DirectoryField *const alt_ref = unique_field(*gps, 5U);
    const DirectoryField *const altitude = unique_field(*gps, 6U);
    ASSERT_NE(lat_ref, nullptr);
    ASSERT_NE(version, nullptr);
    EXPECT_EQ(version->type, 1U);
    EXPECT_EQ(version->count, 4U);
    EXPECT_EQ(version->payload, (std::vector<std::uint8_t>{2U, 3U, 0U, 0U}));
    EXPECT_EQ(lat_ref->type, kTypeAscii);
    EXPECT_EQ(lat_ref->count, 2U);
    ASSERT_GE(lat_ref->payload.size(), 1U);
    EXPECT_EQ(lat_ref->payload[0], static_cast<std::uint8_t>('N'));
    ASSERT_NE(latitude, nullptr);
    EXPECT_EQ(latitude->type, kTypeRational);
    EXPECT_EQ(latitude->count, 3U);
    EXPECT_EQ(rational_values(latitude, 3U), (std::vector<std::pair<std::uint32_t, std::uint32_t>>{
                                                 {49U, 1U}, {15U, 1U}, {29151U, 2500U}}));
    ASSERT_NE(lon_ref, nullptr);
    EXPECT_EQ(lon_ref->type, kTypeAscii);
    EXPECT_EQ(lon_ref->count, 2U);
    ASSERT_GE(lon_ref->payload.size(), 1U);
    EXPECT_EQ(lon_ref->payload[0], static_cast<std::uint8_t>('E'));
    ASSERT_NE(longitude, nullptr);
    EXPECT_EQ(longitude->type, kTypeRational);
    EXPECT_EQ(longitude->count, 3U);
    EXPECT_EQ(rational_values(longitude, 3U), (std::vector<std::pair<std::uint32_t, std::uint32_t>>{
                                                  {3U, 1U}, {3U, 1U}, {3447U, 1250U}}));
    ASSERT_NE(alt_ref, nullptr);
    EXPECT_EQ(alt_ref->type, 1U);
    EXPECT_EQ(alt_ref->count, 1U);
    ASSERT_FALSE(alt_ref->payload.empty());
    EXPECT_EQ(alt_ref->payload[0], 0U);
    ASSERT_NE(altitude, nullptr);
    EXPECT_EQ(altitude->type, kTypeRational);
    EXPECT_EQ(altitude->count, 1U);
    EXPECT_EQ(rational_values(altitude, 1U),
              (std::vector<std::pair<std::uint32_t, std::uint32_t>>{{15432U, 125U}}));
    const auto exif_offset = directory_offset(unique_field(*directory, kTagExifIfd));
    ASSERT_TRUE(exif_offset);
    const auto exif = parse_classic_little_endian_ifd(encoded.value(), *exif_offset);
    ASSERT_TRUE(exif);
    EXPECT_EQ(ascii_value(unique_field(*exif, kExifDateTimeOriginal)), "2007:09:11 13:53:33");
    EXPECT_EQ(ascii_value(unique_field(*exif, kExifOffsetTimeOriginal)), "+02:00");
    EXPECT_EQ(ascii_value(unique_field(*exif, kExifSubSecTimeOriginal)), "18");
    const auto xmp = std::string(unique_field(*directory, kTagXmp)->payload.begin(),
                                 unique_field(*directory, kTagXmp)->payload.end());
    EXPECT_NE(xmp.find("2007-09-11T13:53:33.18+02:00"), std::string::npos);
    EXPECT_NE(xmp.find("49,15.19434N"), std::string::npos);
    QXmlStreamReader xmp_reader(QByteArray(xmp.data(), static_cast<qsizetype>(xmp.size())));
    while (!xmp_reader.atEnd())
    {
        xmp_reader.readNext();
    }
    EXPECT_FALSE(xmp_reader.hasError()) << xmp_reader.errorString().toStdString();

    ExportMetadataSnapshot below_zero = metadata;
    below_zero.capture.location->altitude =
        CaptureAltitude{0U, CaptureAltitudeReference::kBelowSeaLevel};
    const auto encoded_below = detail::encode_tiff_rgb8(8U, 4U, pixels, icc, TiffExportOptions{},
                                                        below_zero, CancellationToken{});
    ASSERT_TRUE(encoded_below) << encoded_below.error().message;
    const auto below_directory = parse_classic_little_endian_directory(encoded_below.value());
    ASSERT_TRUE(below_directory);
    const auto below_gps_offset = directory_offset(unique_field(*below_directory, kTagGpsIfd));
    ASSERT_TRUE(below_gps_offset);
    const auto below_gps =
        parse_classic_little_endian_ifd(encoded_below.value(), *below_gps_offset);
    ASSERT_TRUE(below_gps);
    ASSERT_NE(unique_field(*below_gps, 5U), nullptr);
    EXPECT_EQ(unique_field(*below_gps, 5U)->payload, (std::vector<std::uint8_t>{1U}));
    EXPECT_EQ(rational_values(unique_field(*below_gps, 6U), 1U),
              (std::vector<std::pair<std::uint32_t, std::uint32_t>>{{0U, 1U}}));

    const std::array<std::pair<detail::TiffEncodeCheckpoint,
                               std::pair<std::uint32_t, detail::TiffEncodeInjectedFailure>>,
                     7>
        failures{{
            {detail::TiffEncodeCheckpoint::kExifDirectory,
             {0U, detail::TiffEncodeInjectedFailure::kExifCreateDirectoryFailure}},
            {detail::TiffEncodeCheckpoint::kExifDirectory,
             {1U, detail::TiffEncodeInjectedFailure::kExifWriteDirectoryFailure}},
            {detail::TiffEncodeCheckpoint::kGpsDirectory,
             {0U, detail::TiffEncodeInjectedFailure::kGpsCreateDirectoryFailure}},
            {detail::TiffEncodeCheckpoint::kGpsDirectory,
             {1U, detail::TiffEncodeInjectedFailure::kGpsWriteDirectoryFailure}},
            {detail::TiffEncodeCheckpoint::kRestoreMainDirectory,
             {0U, detail::TiffEncodeInjectedFailure::kRestoreDirectoryFailure}},
            {detail::TiffEncodeCheckpoint::kLinkDirectories,
             {0U, detail::TiffEncodeInjectedFailure::kLinkExifIfdFailure}},
            {detail::TiffEncodeCheckpoint::kLinkDirectories,
             {1U, detail::TiffEncodeInjectedFailure::kGpsLinkIfdFailure}},
        }};
    for (std::uint32_t stage = 0U; stage < failures.size(); ++stage)
    {
        detail::TiffEncodeCheckpointCallback callback =
            [](void *context, const detail::TiffEncodeCheckpoint checkpoint,
               const std::uint32_t progress, const detail::TiffEncodeConfiguration &) noexcept
            -> detail::TiffEncodeInjectedFailure
        {
            const auto *const wanted = static_cast<
                const std::pair<detail::TiffEncodeCheckpoint,
                                std::pair<std::uint32_t, detail::TiffEncodeInjectedFailure>> *>(
                context);
            if (checkpoint == wanted->first && progress == wanted->second.first)
            {
                return wanted->second.second;
            }
            return detail::TiffEncodeInjectedFailure::kNone;
        };
        auto want = failures[stage];
        detail::TiffEncodeControl control;
        control.checkpoint_observer = {&want, callback};
        const auto failed = detail::encode_tiff_rgb8(8U, 4U, pixels, icc, TiffExportOptions{},
                                                     metadata, CancellationToken{}, control);
        ASSERT_FALSE(failed) << static_cast<int>(stage);
        EXPECT_EQ(failed.error().code, ErrorCode::kIo);
    }
}

} // namespace
} // namespace ravo

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    ravo::init_logging("ravo-tiff-metadata-export-tests");
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    ravo::shutdown_logging();
    return result;
}
