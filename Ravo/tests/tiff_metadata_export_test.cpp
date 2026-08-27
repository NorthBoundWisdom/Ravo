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
#include <vector>

#include <QByteArray>
#include <QColor>
#include <QColorSpace>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QImage>
#include <gtest/gtest.h>

#include "../adapters/src/tiff_encoder.h"
#include "ravo/adapters/filesystem_preview_cache.h"
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
inline constexpr std::uint16_t kTagIccProfile = 34675U;

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
parse_classic_little_endian_directory(const std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < 8U || bytes[0] != 'I' || bytes[1] != 'I' || read_u16_le(bytes, 2U) != 42U)
    {
        return std::nullopt;
    }
    const std::size_t ifd_offset = read_u32_le(bytes, 4U);
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
void expect_error(const Result<T> &result, const ErrorCode code, const std::string_view reason)
{
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, code);
    ASSERT_TRUE(result.error().context.contains("format"));
    EXPECT_EQ(result.error().context.at("format"), "tiff");
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
    expect_error(invalid_writable, ErrorCode::kValidation, "invalid_tiff_export_metadata");
    ASSERT_TRUE(invalid_writable.error().context.contains("field"));
    EXPECT_EQ(invalid_writable.error().context.at("field"), "title");
}

TEST(TiffMetadataAdapterTest, WritesExactBaselineDirectoryTagsAndOmitsExtendedOwners)
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
    EXPECT_EQ(unique_field(*directory, kTagXmp), nullptr);
    EXPECT_EQ(unique_field(*directory, kTagIptc), nullptr);
    EXPECT_EQ(unique_field(*directory, kTagExifIfd), nullptr);
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

TEST(TiffMetadataPortTest, LegacyDoubleFailsClosedForTiffAndIgnoresMetadataForOtherFormats)
{
    LegacyRasterDouble concrete;
    const RasterDecoder &decoder = concrete;
    ExportMetadataSnapshot metadata;
    metadata.destination_document_name = "/tmp/output.tif";
    const auto rejected = decoder.encode(
        1U, 1U, {1U, 2U, 3U}, ColorProfileState{}, ExportFormat::kTiff, JpegExportOptions{},
        CancellationToken{}, PngExportOptions{}, TiffExportOptions{}, metadata);
    expect_error(rejected, ErrorCode::kUnsupported, "unsupported_tiff_metadata_owner");
    EXPECT_EQ(concrete.calls, 0U);

    metadata.destination_document_name = std::string("bad\0ignored", 11U);
    const auto unrelated = decoder.encode(
        1U, 1U, {1U, 2U, 3U}, ColorProfileState{}, ExportFormat::kPng, JpegExportOptions{},
        CancellationToken{}, PngExportOptions{}, TiffExportOptions{}, metadata);
    ASSERT_TRUE(unrelated) << unrelated.error().message;
    EXPECT_EQ(unrelated.value(), std::vector<std::uint8_t>{0x42U});
    EXPECT_EQ(concrete.calls, 1U);
}

TEST(TiffMetadataCatalogTest, SnapshotsNormalizedDestinationAndWritableMetadataOnlyForTiff)
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
    auto raster = std::make_unique<CapturingMetadataDecoder>();
    CapturingMetadataDecoder *const capturing = raster.get();
    CatalogService service(std::move(engine).value(), std::move(repository).value(),
                           std::move(raster), std::move(cache).value());

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

    ExportRequest unrelated = request;
    unrelated.format = ExportFormat::kPng;
    unrelated.output_path = (temporary.path() / "unrelated.png").string();
    const auto png = service.export_asset(unrelated);
    ASSERT_TRUE(png) << png.error().message;
    EXPECT_EQ(capturing->last_format, ExportFormat::kPng);
    EXPECT_EQ(capturing->last_metadata, ExportMetadataSnapshot{});

    EXPECT_EQ(QCryptographicHash::hash(read_file(input_path), QCryptographicHash::Sha256),
              source_hash);
    EXPECT_EQ(QCryptographicHash::hash(read_file(sidecar_path), QCryptographicHash::Sha256),
              sidecar_hash);
    EXPECT_EQ(std::filesystem::file_size(input_path), source_size);
    EXPECT_EQ(std::filesystem::file_size(sidecar_path), sidecar_size);
    EXPECT_EQ(std::filesystem::last_write_time(input_path), source_mtime);
    EXPECT_EQ(std::filesystem::last_write_time(sidecar_path), sidecar_mtime);
    EXPECT_FALSE(std::filesystem::exists(normalized_output.value().path + ".xmp"));
    EXPECT_TRUE(service.close());
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
