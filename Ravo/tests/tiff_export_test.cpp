#include <algorithm>
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

#include <zlib.h>

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
#include "ravo/domain/types.h"
#include "ravo/engine/engine.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/log.h"
#include "ravo/services/catalog_service.h"

namespace ravo
{
namespace
{

inline constexpr std::uint16_t kTagImageWidth = 256U;
inline constexpr std::uint16_t kTagImageLength = 257U;
inline constexpr std::uint16_t kTagBitsPerSample = 258U;
inline constexpr std::uint16_t kTagCompression = 259U;
inline constexpr std::uint16_t kTagPhotometric = 262U;
inline constexpr std::uint16_t kTagDocumentName = 269U;
inline constexpr std::uint16_t kTagStripOffsets = 273U;
inline constexpr std::uint16_t kTagOrientation = 274U;
inline constexpr std::uint16_t kTagSamplesPerPixel = 277U;
inline constexpr std::uint16_t kTagRowsPerStrip = 278U;
inline constexpr std::uint16_t kTagStripByteCounts = 279U;
inline constexpr std::uint16_t kTagXResolution = 282U;
inline constexpr std::uint16_t kTagYResolution = 283U;
inline constexpr std::uint16_t kTagPlanarConfiguration = 284U;
inline constexpr std::uint16_t kTagPageName = 285U;
inline constexpr std::uint16_t kTagResolutionUnit = 296U;
inline constexpr std::uint16_t kTagPageNumber = 297U;
inline constexpr std::uint16_t kTagPredictor = 317U;
inline constexpr std::uint16_t kTagTileWidth = 322U;
inline constexpr std::uint16_t kTagSubIfd = 330U;
inline constexpr std::uint16_t kTagExtraSamples = 338U;
inline constexpr std::uint16_t kTagSampleFormat = 339U;
inline constexpr std::uint16_t kTagXmp = 700U;
inline constexpr std::uint16_t kTagIptc = 33723U;
inline constexpr std::uint16_t kTagExifIfd = 34665U;
inline constexpr std::uint16_t kTagIccProfile = 34675U;

class TiffExportTempDirectory
{
public:
    TiffExportTempDirectory()
        : path_(std::filesystem::temp_directory_path() /
                ("ravo-tiff-export-" + generate_catalog_id()))
    {
        std::filesystem::create_directories(path_);
    }

    ~TiffExportTempDirectory()
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

struct TiffField
{
    std::uint16_t tag = 0U;
    std::uint16_t type = 0U;
    std::uint32_t count = 0U;
    std::vector<std::uint8_t> payload;
};

struct TiffDocument
{
    std::vector<TiffField> fields;
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

[[nodiscard]] std::optional<std::size_t> tiff_type_size(const std::uint16_t type)
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

[[nodiscard]] std::optional<TiffDocument>
parse_classic_little_endian_tiff(const std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < 8U || bytes[0] != 'I' || bytes[1] != 'I' || read_u16_le(bytes, 2U) != 42U)
    {
        return std::nullopt;
    }
    const std::uint32_t ifd_offset = read_u32_le(bytes, 4U);
    if (ifd_offset > bytes.size() || bytes.size() - ifd_offset < 2U)
    {
        return std::nullopt;
    }
    const std::uint16_t field_count = read_u16_le(bytes, ifd_offset);
    const std::uint64_t ifd_size = 2U + static_cast<std::uint64_t>(field_count) * 12U + 4U;
    if (ifd_size > bytes.size() - ifd_offset)
    {
        return std::nullopt;
    }

    TiffDocument document;
    document.fields.reserve(field_count);
    for (std::uint16_t index = 0U; index < field_count; ++index)
    {
        const std::size_t offset = static_cast<std::size_t>(ifd_offset) + 2U + index * 12U;
        TiffField field;
        field.tag = read_u16_le(bytes, offset);
        field.type = read_u16_le(bytes, offset + 2U);
        field.count = read_u32_le(bytes, offset + 4U);
        const auto element_size = tiff_type_size(field.type);
        if (!element_size ||
            field.count > std::numeric_limits<std::size_t>::max() / element_size.value())
        {
            return std::nullopt;
        }
        const std::size_t payload_size =
            static_cast<std::size_t>(field.count) * element_size.value();
        std::size_t payload_offset = offset + 8U;
        if (payload_size > 4U)
        {
            payload_offset = read_u32_le(bytes, offset + 8U);
        }
        if (payload_offset > bytes.size() || payload_size > bytes.size() - payload_offset)
        {
            return std::nullopt;
        }
        field.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset),
                             bytes.begin() +
                                 static_cast<std::ptrdiff_t>(payload_offset + payload_size));
        document.fields.push_back(std::move(field));
    }
    document.next_ifd = read_u32_le(bytes, static_cast<std::size_t>(ifd_offset) + 2U +
                                               static_cast<std::size_t>(field_count) * 12U);
    return document;
}

[[nodiscard]] const TiffField *unique_field(const TiffDocument &document, const std::uint16_t tag)
{
    const TiffField *result = nullptr;
    for (const TiffField &field : document.fields)
    {
        if (field.tag != tag)
        {
            continue;
        }
        if (result != nullptr)
        {
            return nullptr;
        }
        result = &field;
    }
    return result;
}

[[nodiscard]] std::optional<std::vector<std::uint32_t>>
unsigned_values(const TiffField *const field)
{
    if (field == nullptr)
    {
        return std::nullopt;
    }
    std::vector<std::uint32_t> values;
    values.reserve(field->count);
    if (field->type == 1U)
    {
        for (const std::uint8_t value : field->payload)
        {
            values.push_back(value);
        }
        return values;
    }
    if (field->type == 3U && field->payload.size() == static_cast<std::size_t>(field->count) * 2U)
    {
        for (std::size_t offset = 0U; offset < field->payload.size(); offset += 2U)
        {
            values.push_back(read_u16_le(field->payload, offset));
        }
        return values;
    }
    if (field->type == 4U && field->payload.size() == static_cast<std::size_t>(field->count) * 4U)
    {
        for (std::size_t offset = 0U; offset < field->payload.size(); offset += 4U)
        {
            values.push_back(read_u32_le(field->payload, offset));
        }
        return values;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::uint32_t> unsigned_scalar(const TiffDocument &document,
                                                           const std::uint16_t tag)
{
    const auto values = unsigned_values(unique_field(document, tag));
    if (!values || values->size() != 1U)
    {
        return std::nullopt;
    }
    return values->front();
}

[[nodiscard]] std::optional<double> rational_scalar(const TiffDocument &document,
                                                    const std::uint16_t tag)
{
    const TiffField *const field = unique_field(document, tag);
    if (field == nullptr || field->type != 5U || field->count != 1U || field->payload.size() != 8U)
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

[[nodiscard]] std::optional<std::vector<std::uint8_t>>
inflate_exact(const std::span<const std::uint8_t> compressed, const std::size_t expected_size)
{
    if (compressed.size() > static_cast<std::size_t>(std::numeric_limits<uLong>::max()) ||
        expected_size > static_cast<std::size_t>(std::numeric_limits<uLongf>::max()))
    {
        return std::nullopt;
    }
    std::vector<std::uint8_t> result(expected_size);
    uLongf result_size = static_cast<uLongf>(result.size());
    const int status = uncompress(result.data(), &result_size, compressed.data(),
                                  static_cast<uLong>(compressed.size()));
    if (status != Z_OK || result_size != result.size())
    {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>>
tiff_pixels(const std::span<const std::uint8_t> bytes, const TiffDocument &document)
{
    const auto width = unsigned_scalar(document, kTagImageWidth);
    const auto height = unsigned_scalar(document, kTagImageLength);
    const auto samples = unsigned_scalar(document, kTagSamplesPerPixel);
    const auto rows_per_strip = unsigned_scalar(document, kTagRowsPerStrip);
    const auto compression = unsigned_scalar(document, kTagCompression);
    const auto predictor = unsigned_scalar(document, kTagPredictor);
    const auto bits = unsigned_values(unique_field(document, kTagBitsPerSample));
    const auto offsets = unsigned_values(unique_field(document, kTagStripOffsets));
    const auto byte_counts = unsigned_values(unique_field(document, kTagStripByteCounts));
    if (!width || !height || !samples || !rows_per_strip || !compression || !bits || !offsets ||
        !byte_counts || width.value() == 0U || height.value() == 0U || samples.value() == 0U ||
        rows_per_strip.value() == 0U || offsets->size() != byte_counts->size() ||
        !std::all_of(bits->begin(), bits->end(),
                     [](const std::uint32_t value) { return value == 8U; }))
    {
        return std::nullopt;
    }
    const std::uint64_t row_bytes64 = static_cast<std::uint64_t>(width.value()) * samples.value();
    const std::uint64_t total_bytes64 = row_bytes64 * height.value();
    if (row_bytes64 > std::numeric_limits<std::size_t>::max() ||
        total_bytes64 > std::numeric_limits<std::size_t>::max())
    {
        return std::nullopt;
    }
    const std::size_t row_bytes = static_cast<std::size_t>(row_bytes64);
    std::vector<std::uint8_t> result;
    result.reserve(static_cast<std::size_t>(total_bytes64));
    for (std::size_t index = 0U; index < offsets->size(); ++index)
    {
        const std::size_t offset = offsets->at(index);
        const std::size_t byte_count = byte_counts->at(index);
        if (offset > bytes.size() || byte_count > bytes.size() - offset)
        {
            return std::nullopt;
        }
        const std::uint32_t first_row = static_cast<std::uint32_t>(index) * rows_per_strip.value();
        if (first_row >= height.value())
        {
            return std::nullopt;
        }
        const std::uint32_t strip_rows =
            std::min(rows_per_strip.value(), height.value() - first_row);
        const std::size_t expected = row_bytes * strip_rows;
        const auto payload = bytes.subspan(offset, byte_count);
        if (compression.value() == 1U)
        {
            if (payload.size() != expected)
            {
                return std::nullopt;
            }
            result.insert(result.end(), payload.begin(), payload.end());
        }
        else if (compression.value() == 8U)
        {
            auto inflated = inflate_exact(payload, expected);
            if (!inflated)
            {
                return std::nullopt;
            }
            result.insert(result.end(), inflated->begin(), inflated->end());
        }
        else
        {
            return std::nullopt;
        }
    }
    if (result.size() != total_bytes64)
    {
        return std::nullopt;
    }
    const std::uint32_t predictor_value = predictor.value_or(1U);
    if (predictor_value == 2U)
    {
        for (std::uint32_t row = 0U; row < height.value(); ++row)
        {
            const std::size_t row_offset = static_cast<std::size_t>(row) * row_bytes;
            for (std::size_t byte = samples.value(); byte < row_bytes; ++byte)
            {
                result[row_offset + byte] = static_cast<std::uint8_t>(
                    result[row_offset + byte] + result[row_offset + byte - samples.value()]);
            }
        }
    }
    else if (predictor_value != 1U)
    {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] std::vector<std::uint8_t> tiff_test_pixels(const std::uint32_t width,
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
                static_cast<std::uint8_t>((column * 5U + row * 29U + 41U) & 0xFFU);
            pixels[offset + 2U] =
                static_cast<std::uint8_t>((column * 31U + row * 7U + 113U) & 0xFFU);
        }
    }
    return pixels;
}

[[nodiscard]] std::vector<std::uint8_t> qbyte_array_bytes(const QByteArray &bytes)
{
    return {reinterpret_cast<const std::uint8_t *>(bytes.constData()),
            reinterpret_cast<const std::uint8_t *>(bytes.constData()) + bytes.size()};
}

[[nodiscard]] QByteArray read_file(const std::filesystem::path &path)
{
    QFile file(QString::fromStdString(path.string()));
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    return file.readAll();
}

template <typename T>
void expect_tiff_error(const Result<T> &result, const ErrorCode code, const std::string_view reason)
{
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, code);
    ASSERT_TRUE(result.error().context.contains("format"));
    EXPECT_EQ(result.error().context.at("format"), "tiff");
    ASSERT_TRUE(result.error().context.contains("reason"));
    EXPECT_EQ(result.error().context.at("reason"), reason);
}

struct TiffCheckpointState
{
    CancellationSource cancellation;
    detail::TiffEncodeCheckpoint cancel_checkpoint = detail::TiffEncodeCheckpoint::kConfigured;
    std::uint32_t cancel_progress = std::numeric_limits<std::uint32_t>::max();
    detail::TiffEncodeCheckpoint fail_checkpoint = detail::TiffEncodeCheckpoint::kConfigured;
    std::uint32_t fail_progress = std::numeric_limits<std::uint32_t>::max();
    detail::TiffEncodeInjectedFailure injected_failure =
        detail::TiffEncodeInjectedFailure::kEncoderFailure;
    detail::TiffEncodeConfiguration configured;
    bool saw_configuration = false;
    bool cancelled = false;
    bool failed = false;
};

[[nodiscard]] detail::TiffEncodeInjectedFailure
tiff_checkpoint(void *const context, const detail::TiffEncodeCheckpoint checkpoint,
                const std::uint32_t progress,
                const detail::TiffEncodeConfiguration &configuration) noexcept
{
    auto *const state = static_cast<TiffCheckpointState *>(context);
    if (checkpoint == detail::TiffEncodeCheckpoint::kConfigured)
    {
        state->configured = configuration;
        state->saw_configuration = true;
    }
    if (checkpoint == state->cancel_checkpoint && progress == state->cancel_progress)
    {
        state->cancelled = true;
        (void)state->cancellation.cancel("tiff-encode-test");
    }
    if (checkpoint == state->fail_checkpoint && progress == state->fail_progress)
    {
        state->failed = true;
        return state->injected_failure;
    }
    return detail::TiffEncodeInjectedFailure::kNone;
}

class CapturingTiffRasterDecoder final : public RasterDecoder
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
                                                      const int rotate_quarters = 0) const override
    {
        return delegate_.decode_memory(encoded, max_edge, cancellation, rotate_quarters);
    }

    [[nodiscard]] Result<std::vector<std::uint8_t>>
    encode(const std::uint32_t width, const std::uint32_t height,
           const std::vector<std::uint8_t> &rgb, const ColorProfileState &color_profile,
           const ExportFormat format, const JpegExportOptions &jpeg_options,
           const CancellationToken &cancellation,
           const PngExportOptions &png_options = {}) const override
    {
        return delegate_.encode(width, height, rgb, color_profile, format, jpeg_options,
                                cancellation, png_options);
    }

    [[nodiscard]] Result<std::vector<std::uint8_t>>
    encode(const std::uint32_t width, const std::uint32_t height,
           const std::vector<std::uint8_t> &rgb, const ColorProfileState &color_profile,
           const ExportFormat format, const JpegExportOptions &jpeg_options,
           const CancellationToken &cancellation, const PngExportOptions &png_options,
           const TiffExportOptions &tiff_options) const override
    {
        ++encode_calls;
        last_format = format;
        last_tiff_options = tiff_options;
        return delegate_.encode(width, height, rgb, color_profile, format, jpeg_options,
                                cancellation, png_options, tiff_options);
    }

    mutable std::size_t encode_calls = 0U;
    mutable ExportFormat last_format = ExportFormat::kPng;
    mutable TiffExportOptions last_tiff_options;

private:
    QtRasterDecoder delegate_;
};

TEST(TiffExportContractTest, PropagatesFrozenCodecConfiguration)
{
    const auto pixels = tiff_test_pixels(16U, 8U);
    const auto icc = qbyte_array_bytes(QColorSpace(QColorSpace::SRgb).iccProfile());
    ASSERT_FALSE(icc.empty());

    struct CompressionExpectation
    {
        TiffCompression mode = TiffCompression::kNone;
        std::uint16_t compression_tag = 1U;
        std::uint16_t predictor_tag = 1U;
    };
    constexpr std::array<CompressionExpectation, 3U> kExpectations{{
        {TiffCompression::kNone, 1U, 1U},
        {TiffCompression::kDeflate, 8U, 1U},
        {TiffCompression::kDeflatePredictor, 8U, 2U},
    }};
    for (const CompressionExpectation &expectation : kExpectations)
    {
        for (const int level : {1, 6, 9})
        {
            SCOPED_TRACE(level);
            TiffExportOptions options;
            options.compression = expectation.mode;
            options.compression_level = level;
            const auto configuration = detail::tiff_encode_configuration(options);
            ASSERT_TRUE(configuration) << configuration.error().message;
            EXPECT_EQ(configuration.value().bits_per_sample, 8U);
            EXPECT_EQ(configuration.value().sample_format, 1U);
            EXPECT_EQ(configuration.value().samples_per_pixel, 3U);
            EXPECT_EQ(configuration.value().photometric, 2U);
            EXPECT_EQ(configuration.value().planar_configuration, 1U);
            EXPECT_EQ(configuration.value().orientation, 1U);
            EXPECT_EQ(configuration.value().compression, expectation.compression_tag);
            EXPECT_EQ(configuration.value().predictor, expectation.predictor_tag);
            EXPECT_EQ(configuration.value().compression_level, level);
            EXPECT_FLOAT_EQ(configuration.value().resolution_dpi, 300.0F);
            EXPECT_EQ(configuration.value().resolution_unit, 2U);
            EXPECT_TRUE(configuration.value().little_endian);
            EXPECT_FALSE(configuration.value().tiled);

            TiffCheckpointState checkpoint;
            detail::TiffEncodeControl control;
            control.checkpoint_observer = {&checkpoint, tiff_checkpoint};
            const auto encoded = detail::encode_tiff_rgb8(16U, 8U, pixels, icc, options,
                                                          CancellationToken{}, control);
            ASSERT_TRUE(encoded) << encoded.error().message;
            EXPECT_TRUE(checkpoint.saw_configuration);
            EXPECT_EQ(checkpoint.configured.compression, expectation.compression_tag);
            EXPECT_EQ(checkpoint.configured.predictor, expectation.predictor_tag);
            EXPECT_EQ(checkpoint.configured.compression_level, level);
        }
    }
}

TEST(TiffExportContractTest, WritesClassicLittleEndianOpaqueRgb8TagsPixelsAndExactIcc)
{
    constexpr std::uint32_t kWidth = 19U;
    constexpr std::uint32_t kHeight = 11U;
    const auto pixels = tiff_test_pixels(kWidth, kHeight);
    const auto pixels_before = pixels;
    ColorProfileState profile;
    profile.kind = ColorProfileKind::kIcc;
    profile.model = ColorModel::kRgb;
    profile.identifier = "display-p3-test";
    profile.icc_bytes = qbyte_array_bytes(QColorSpace(QColorSpace::DisplayP3).iccProfile());
    ASSERT_FALSE(profile.icc_bytes.empty());
    const auto profile_before = profile;
    QtRasterDecoder decoder;

    const auto encoded =
        decoder.encode(kWidth, kHeight, pixels, profile, ExportFormat::kTiff, JpegExportOptions{},
                       CancellationToken{}, PngExportOptions{}, TiffExportOptions{});
    ASSERT_TRUE(encoded) << encoded.error().message;
    const auto document = parse_classic_little_endian_tiff(encoded.value());
    ASSERT_TRUE(document);
    EXPECT_EQ(document->next_ifd, 0U);
    EXPECT_EQ(unsigned_scalar(*document, kTagImageWidth), kWidth);
    EXPECT_EQ(unsigned_scalar(*document, kTagImageLength), kHeight);
    EXPECT_EQ(unsigned_values(unique_field(*document, kTagBitsPerSample)),
              (std::optional<std::vector<std::uint32_t>>{{8U, 8U, 8U}}));
    EXPECT_EQ(unsigned_values(unique_field(*document, kTagSampleFormat)),
              (std::optional<std::vector<std::uint32_t>>{{1U, 1U, 1U}}));
    EXPECT_EQ(unsigned_scalar(*document, kTagSamplesPerPixel), 3U);
    EXPECT_EQ(unsigned_scalar(*document, kTagPhotometric), 2U);
    EXPECT_EQ(unsigned_scalar(*document, kTagPlanarConfiguration), 1U);
    EXPECT_EQ(unsigned_scalar(*document, kTagOrientation), 1U);
    EXPECT_EQ(unsigned_scalar(*document, kTagCompression), 8U);
    EXPECT_EQ(unsigned_scalar(*document, kTagPredictor), 2U);
    EXPECT_GT(unsigned_scalar(*document, kTagRowsPerStrip).value_or(0U), 0U);
    EXPECT_DOUBLE_EQ(rational_scalar(*document, kTagXResolution).value_or(0.0), 300.0);
    EXPECT_DOUBLE_EQ(rational_scalar(*document, kTagYResolution).value_or(0.0), 300.0);
    EXPECT_EQ(unsigned_scalar(*document, kTagResolutionUnit), 2U);

    const TiffField *const embedded_icc = unique_field(*document, kTagIccProfile);
    ASSERT_NE(embedded_icc, nullptr);
    EXPECT_EQ(embedded_icc->type, 7U);
    EXPECT_EQ(embedded_icc->payload, profile.icc_bytes);
    for (const std::uint16_t absent_tag :
         {kTagDocumentName, kTagPageName, kTagPageNumber, kTagTileWidth, kTagSubIfd,
          kTagExtraSamples, kTagXmp, kTagIptc, kTagExifIfd})
    {
        EXPECT_EQ(unique_field(*document, absent_tag), nullptr);
    }
    const auto decoded = tiff_pixels(encoded.value(), *document);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value(), pixels);
    EXPECT_EQ(pixels, pixels_before);
    EXPECT_EQ(profile, profile_before);
}

TEST(TiffExportContractTest, ConditionalGrayscaleMatchesFrozenRgb8ThresholdAndSmallImageBoundary)
{
    const auto icc = qbyte_array_bytes(QColorSpace(QColorSpace::SRgb).iccProfile());
    ASSERT_FALSE(icc.empty());
    TiffExportOptions options;
    options.grayscale_if_neutral = true;

    std::vector<std::uint8_t> neutral(7U * 7U * 3U, 80U);
    neutral[(3U * 7U + 3U) * 3U + 1U] = 82U;
    neutral[(0U * 7U + 0U) * 3U + 2U] = 200U;
    const auto grayscale =
        detail::encode_tiff_rgb8(7U, 7U, neutral, icc, options, CancellationToken{});
    ASSERT_TRUE(grayscale) << grayscale.error().message;
    const auto grayscale_document = parse_classic_little_endian_tiff(grayscale.value());
    ASSERT_TRUE(grayscale_document);
    EXPECT_EQ(unsigned_scalar(*grayscale_document, kTagSamplesPerPixel), 1U);
    EXPECT_EQ(unsigned_scalar(*grayscale_document, kTagPhotometric), 1U);
    EXPECT_EQ(unsigned_values(unique_field(*grayscale_document, kTagBitsPerSample)),
              (std::optional<std::vector<std::uint32_t>>{{8U}}));
    EXPECT_EQ(unsigned_values(unique_field(*grayscale_document, kTagSampleFormat)),
              (std::optional<std::vector<std::uint32_t>>{{1U}}));
    const auto grayscale_pixels = tiff_pixels(grayscale.value(), *grayscale_document);
    ASSERT_TRUE(grayscale_pixels);
    std::vector<std::uint8_t> expected_gray(49U);
    for (std::size_t pixel = 0U; pixel < expected_gray.size(); ++pixel)
    {
        expected_gray[pixel] = neutral[pixel * 3U];
    }
    EXPECT_EQ(grayscale_pixels.value(), expected_gray);

    neutral[(3U * 7U + 3U) * 3U + 2U] = 84U;
    const auto non_neutral =
        detail::encode_tiff_rgb8(7U, 7U, neutral, icc, options, CancellationToken{});
    ASSERT_TRUE(non_neutral) << non_neutral.error().message;
    const auto non_neutral_document = parse_classic_little_endian_tiff(non_neutral.value());
    ASSERT_TRUE(non_neutral_document);
    EXPECT_EQ(unsigned_scalar(*non_neutral_document, kTagSamplesPerPixel), 3U);
    EXPECT_EQ(unsigned_scalar(*non_neutral_document, kTagPhotometric), 2U);

    std::vector<std::uint8_t> small_neutral(4U * 7U * 3U, 30U);
    const auto small =
        detail::encode_tiff_rgb8(4U, 7U, small_neutral, icc, options, CancellationToken{});
    ASSERT_TRUE(small) << small.error().message;
    const auto small_document = parse_classic_little_endian_tiff(small.value());
    ASSERT_TRUE(small_document);
    EXPECT_EQ(unsigned_scalar(*small_document, kTagSamplesPerPixel), 3U);
}

TEST(TiffExportContractTest, RejectsHighPrecisionInvalidAndOversizedInputsWithoutBytes)
{
    const auto pixels = tiff_test_pixels(16U, 8U);
    const auto icc = qbyte_array_bytes(QColorSpace(QColorSpace::SRgb).iccProfile());
    ASSERT_FALSE(icc.empty());
    for (const TiffSampleType sample_type :
         {TiffSampleType::kUint16, TiffSampleType::kFloat16, TiffSampleType::kFloat32})
    {
        TiffExportOptions options;
        options.sample_type = sample_type;
        expect_tiff_error(
            detail::encode_tiff_rgb8(16U, 8U, pixels, icc, options, CancellationToken{}),
            ErrorCode::kUnsupported, "unsupported_tiff_high_precision_source");
    }

    expect_tiff_error(
        detail::encode_tiff_rgb8(0U, 8U, pixels, icc, TiffExportOptions{}, CancellationToken{}),
        ErrorCode::kValidation, "invalid_tiff_dimensions");
    expect_tiff_error(
        detail::encode_tiff_rgb8(20000U, 20000U, {}, icc, TiffExportOptions{}, CancellationToken{}),
        ErrorCode::kValidation, "tiff_source_too_large");
    expect_tiff_error(detail::encode_tiff_rgb8(16U, 8U,
                                               std::span<const std::uint8_t>(pixels).first(7U), icc,
                                               TiffExportOptions{}, CancellationToken{}),
                      ErrorCode::kValidation, "tiff_source_size_mismatch");
    expect_tiff_error(
        detail::encode_tiff_rgb8(16U, 8U, pixels, {}, TiffExportOptions{}, CancellationToken{}),
        ErrorCode::kValidation, "missing_tiff_output_icc");

    std::vector<std::uint8_t> oversized_icc(detail::kTiffMaxIccBytes + 1U);
    expect_tiff_error(detail::encode_tiff_rgb8(16U, 8U, pixels, oversized_icc, TiffExportOptions{},
                                               CancellationToken{}),
                      ErrorCode::kValidation, "oversized_tiff_output_icc");

    detail::TiffEncodeControl tiny_output;
    tiny_output.max_output_bytes = 32U;
    expect_tiff_error(detail::encode_tiff_rgb8(16U, 8U, pixels, icc, TiffExportOptions{},
                                               CancellationToken{}, tiny_output),
                      ErrorCode::kValidation, "tiff_output_too_large");
    for (const std::size_t invalid_bound :
         std::array<std::size_t, 2U>{0U, detail::kTiffMaxOutputBytes + 1U})
    {
        detail::TiffEncodeControl invalid_output;
        invalid_output.max_output_bytes = invalid_bound;
        expect_tiff_error(detail::encode_tiff_rgb8(16U, 8U, pixels, icc, TiffExportOptions{},
                                                   CancellationToken{}, invalid_output),
                          ErrorCode::kValidation, "invalid_tiff_output_bound");
    }

    TiffExportOptions invalid_options;
    invalid_options.compression = static_cast<TiffCompression>(255U);
    expect_tiff_error(
        detail::encode_tiff_rgb8(16U, 8U, pixels, icc, invalid_options, CancellationToken{}),
        ErrorCode::kValidation, "invalid_tiff_compression");

    ColorProfileState non_rgb;
    non_rgb.kind = ColorProfileKind::kBuiltin;
    non_rgb.model = ColorModel::kLab;
    non_rgb.identifier = "srgb";
    QtRasterDecoder decoder;
    expect_tiff_error(decoder.encode(16U, 8U, pixels, non_rgb, ExportFormat::kTiff,
                                     JpegExportOptions{}, CancellationToken{}, PngExportOptions{},
                                     TiffExportOptions{}),
                      ErrorCode::kUnsupported, "unsupported_tiff_output_icc_color_model");
}

TEST(TiffExportContractTest, CancellationAndInjectedFailurePublishNoBytesOrMutations)
{
    const auto pixels = tiff_test_pixels(128U, 32U);
    const auto pixels_before = pixels;
    const auto icc = qbyte_array_bytes(QColorSpace(QColorSpace::SRgb).iccProfile());
    const auto icc_before = icc;
    ASSERT_FALSE(icc.empty());

    CancellationSource entry;
    ASSERT_TRUE(entry.cancel("tiff-entry-test"));
    expect_tiff_error(
        detail::encode_tiff_rgb8(128U, 32U, pixels, icc, TiffExportOptions{}, entry.token()),
        ErrorCode::kCancelled, "tiff_encode_cancelled");

    for (const auto [checkpoint, progress] :
         std::array<std::pair<detail::TiffEncodeCheckpoint, std::uint32_t>, 2U>{{
             {detail::TiffEncodeCheckpoint::kScanline, 5U},
             {detail::TiffEncodeCheckpoint::kBeforeFinish, 32U},
         }})
    {
        TiffCheckpointState state;
        state.cancel_checkpoint = checkpoint;
        state.cancel_progress = progress;
        detail::TiffEncodeControl control;
        control.checkpoint_observer = {&state, tiff_checkpoint};
        const auto cancelled = detail::encode_tiff_rgb8(128U, 32U, pixels, icc, TiffExportOptions{},
                                                        state.cancellation.token(), control);
        expect_tiff_error(cancelled, ErrorCode::kCancelled, "tiff_encode_cancelled");
        EXPECT_TRUE(state.cancelled);
    }

    struct FailureExpectation
    {
        detail::TiffEncodeInjectedFailure failure = detail::TiffEncodeInjectedFailure::kNone;
        std::string_view reason;
    };
    const std::array<FailureExpectation, 6U> failures{{
        {detail::TiffEncodeInjectedFailure::kEncoderFailure, "tiff_encoder_failure"},
        {detail::TiffEncodeInjectedFailure::kClientWriteFailure, "tiff_client_write_failed"},
        {detail::TiffEncodeInjectedFailure::kClientSeekFailure, "tiff_client_seek_failed"},
        {detail::TiffEncodeInjectedFailure::kAllocationFailure, "tiff_output_allocation_failed"},
        {detail::TiffEncodeInjectedFailure::kClientCloseFailure, "tiff_client_close_failed"},
        {detail::TiffEncodeInjectedFailure::kFinalizeFailure, "tiff_encoder_failure"},
    }};
    for (const FailureExpectation &expectation : failures)
    {
        TiffCheckpointState state;
        state.fail_checkpoint = detail::TiffEncodeCheckpoint::kScanline;
        state.fail_progress = 7U;
        if (expectation.failure == detail::TiffEncodeInjectedFailure::kClientCloseFailure ||
            expectation.failure == detail::TiffEncodeInjectedFailure::kFinalizeFailure)
        {
            state.fail_checkpoint = detail::TiffEncodeCheckpoint::kBeforeFinish;
            state.fail_progress = 32U;
        }
        state.injected_failure = expectation.failure;
        detail::TiffEncodeControl control;
        control.checkpoint_observer = {&state, tiff_checkpoint};
        const auto failed = detail::encode_tiff_rgb8(128U, 32U, pixels, icc, TiffExportOptions{},
                                                     CancellationToken{}, control);
        expect_tiff_error(failed, ErrorCode::kIo, expectation.reason);
        EXPECT_TRUE(state.failed);
    }
    EXPECT_EQ(pixels, pixels_before);
    EXPECT_EQ(icc, icc_before);
}

TEST(TiffCatalogTest, ForwardsDefaultsAndExplicitOptionsWithFormatIsolation)
{
    TiffExportTempDirectory temporary;
    const auto input_path = temporary.path() / "source.png";
    QImage source(32, 24, QImage::Format_RGB888);
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
    const QByteArray source_hash =
        QCryptographicHash::hash(read_file(input_path), QCryptographicHash::Sha256);

    auto engine_result = EngineFacade::create_phase1();
    ASSERT_TRUE(engine_result) << engine_result.error().message;
    auto repository =
        SqliteCatalogRepository::create((temporary.path() / "library.sqlite").string());
    ASSERT_TRUE(repository) << repository.error().message;
    auto cache = FilesystemPreviewCache::create((temporary.path() / "preview").string());
    ASSERT_TRUE(cache) << cache.error().message;
    auto raster = std::make_unique<CapturingTiffRasterDecoder>();
    CapturingTiffRasterDecoder *const capturing = raster.get();
    CatalogService service(std::move(engine_result).value(), std::move(repository).value(),
                           std::move(raster), std::move(cache).value());

    const auto imported = service.import_one(input_path.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);

    ExportRequest defaults;
    defaults.asset_id = imported.value().asset->id;
    defaults.output_path = (temporary.path() / "default.tif").string();
    defaults.format = ExportFormat::kTiff;
    const auto default_export = service.export_asset(defaults);
    ASSERT_TRUE(default_export) << default_export.error().message;
    EXPECT_EQ(capturing->last_format, ExportFormat::kTiff);
    EXPECT_EQ(capturing->last_tiff_options, TiffExportOptions());
    EXPECT_TRUE(std::filesystem::is_regular_file(defaults.output_path));

    ExportRequest explicit_options = defaults;
    explicit_options.output_path = (temporary.path() / "explicit.tif").string();
    explicit_options.tiff_options = {TiffSampleType::kUint8, TiffCompression::kNone, 1, true};
    const auto explicit_export = service.export_asset(explicit_options);
    ASSERT_TRUE(explicit_export) << explicit_export.error().message;
    EXPECT_EQ(capturing->last_tiff_options, explicit_options.tiff_options);
    EXPECT_TRUE(std::filesystem::is_regular_file(explicit_options.output_path));

    const std::size_t calls_before_rejection = capturing->encode_calls;
    ExportRequest high_precision = defaults;
    high_precision.output_path = (temporary.path() / "unsupported-16.tif").string();
    high_precision.tiff_options.sample_type = TiffSampleType::kUint16;
    expect_tiff_error(service.export_asset(high_precision), ErrorCode::kUnsupported,
                      "unsupported_tiff_high_precision_source");
    EXPECT_EQ(capturing->encode_calls, calls_before_rejection + 1U);
    EXPECT_FALSE(std::filesystem::exists(high_precision.output_path));

    ExportRequest invalid = defaults;
    invalid.output_path = (temporary.path() / "invalid.tif").string();
    invalid.tiff_options.compression_level = 10;
    expect_tiff_error(service.export_asset(invalid), ErrorCode::kValidation,
                      "invalid_tiff_compression_level");
    EXPECT_EQ(capturing->encode_calls, calls_before_rejection + 1U);
    EXPECT_FALSE(std::filesystem::exists(invalid.output_path));

    TiffExportOptions deliberately_invalid;
    deliberately_invalid.sample_type = static_cast<TiffSampleType>(255U);
    deliberately_invalid.compression_level = 0;
    for (const auto [format, suffix] : std::array<std::pair<ExportFormat, std::string_view>, 2U>{{
             {ExportFormat::kJpeg, ".jpg"},
             {ExportFormat::kPng, ".png"},
         }})
    {
        ExportRequest unrelated = defaults;
        unrelated.format = format;
        unrelated.output_path = (temporary.path() / ("unrelated" + std::string(suffix))).string();
        unrelated.tiff_options = deliberately_invalid;
        const auto exported = service.export_asset(unrelated);
        ASSERT_TRUE(exported) << exported.error().message;
        EXPECT_EQ(capturing->last_format, format);
        EXPECT_EQ(capturing->last_tiff_options, deliberately_invalid);
        EXPECT_TRUE(std::filesystem::is_regular_file(unrelated.output_path));
    }

    const auto conflict = service.export_asset(defaults);
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().code, ErrorCode::kConflict);
    EXPECT_TRUE(std::filesystem::is_regular_file(defaults.output_path));

    CancellationSource cancelled;
    ASSERT_TRUE(cancelled.cancel("tiff-catalog-test"));
    ExportRequest cancelled_request = defaults;
    cancelled_request.output_path = (temporary.path() / "cancelled.tif").string();
    cancelled_request.cancellation = cancelled.token();
    const auto cancelled_result = service.export_asset(cancelled_request);
    ASSERT_FALSE(cancelled_result);
    EXPECT_EQ(cancelled_result.error().code, ErrorCode::kCancelled);
    EXPECT_FALSE(std::filesystem::exists(cancelled_request.output_path));

    EXPECT_EQ(QCryptographicHash::hash(read_file(input_path), QCryptographicHash::Sha256),
              source_hash);
    EXPECT_TRUE(service.close());
}

} // namespace
} // namespace ravo

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    ravo::init_logging("ravo-tiff-export-tests");
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    ravo::shutdown_logging();
    return result;
}
