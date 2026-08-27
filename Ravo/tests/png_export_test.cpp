#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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

#include <zlib.h>

#include <QByteArray>
#include <QColor>
#include <QColorSpace>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QImage>
#include <gtest/gtest.h>

#include "../adapters/src/png_encoder.h"
#include "ravo/adapters/filesystem_preview_cache.h"
#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/domain/types.h"
#include "ravo/domain/uri.h"
#include "ravo/engine/engine.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/log.h"
#include "ravo/recipe/develop.h"
#include "ravo/services/catalog_service.h"

namespace ravo
{
namespace
{

inline constexpr std::array<std::uint8_t, 8U> kPngSignature{0x89U, 'P',   'N',   'G',
                                                            0x0DU, 0x0AU, 0x1AU, 0x0AU};

class PngExportTempDirectory
{
public:
    PngExportTempDirectory()
        : path_(std::filesystem::temp_directory_path() /
                ("ravo-png-export-" + generate_catalog_id()))
    {
        std::filesystem::create_directories(path_);
    }

    ~PngExportTempDirectory()
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

struct PngChunk
{
    std::array<char, 4U> type{};
    std::vector<std::uint8_t> payload;
};

struct PngIhdr
{
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint8_t bit_depth = 0U;
    std::uint8_t color_type = 0U;
    std::uint8_t compression = 0U;
    std::uint8_t filter = 0U;
    std::uint8_t interlace = 0U;
};

[[nodiscard]] std::uint32_t read_u32_be(const std::span<const std::uint8_t> bytes,
                                        const std::size_t offset)
{
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) |
           static_cast<std::uint32_t>(bytes[offset + 3U]);
}

[[nodiscard]] std::optional<std::vector<PngChunk>>
png_chunks(const std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < kPngSignature.size() ||
        !std::equal(kPngSignature.begin(), kPngSignature.end(), bytes.begin()))
    {
        return std::nullopt;
    }
    std::vector<PngChunk> result;
    std::size_t offset = kPngSignature.size();
    bool ended = false;
    while (offset < bytes.size())
    {
        if (bytes.size() - offset < 12U)
        {
            return std::nullopt;
        }
        const std::uint32_t length = read_u32_be(bytes, offset);
        offset += 4U;
        if (length > bytes.size() - offset - 8U)
        {
            return std::nullopt;
        }
        PngChunk chunk;
        std::copy_n(reinterpret_cast<const char *>(bytes.data() + offset), 4U, chunk.type.begin());
        offset += 4U;
        chunk.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                             bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
        offset += length + 4U;
        ended = chunk.type == std::array<char, 4U>{'I', 'E', 'N', 'D'};
        result.push_back(std::move(chunk));
        if (ended)
        {
            break;
        }
    }
    if (!ended || offset != bytes.size())
    {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] std::vector<const PngChunk *> chunks_named(const std::vector<PngChunk> &chunks,
                                                         const std::array<char, 4U> &type)
{
    std::vector<const PngChunk *> result;
    for (const PngChunk &chunk : chunks)
    {
        if (chunk.type == type)
        {
            result.push_back(&chunk);
        }
    }
    return result;
}

[[nodiscard]] std::optional<PngIhdr> png_ihdr(const std::vector<PngChunk> &chunks)
{
    const auto headers = chunks_named(chunks, {'I', 'H', 'D', 'R'});
    if (headers.size() != 1U || headers.front()->payload.size() != 13U)
    {
        return std::nullopt;
    }
    const auto bytes = std::span<const std::uint8_t>(headers.front()->payload);
    return PngIhdr{read_u32_be(bytes, 0U),
                   read_u32_be(bytes, 4U),
                   bytes[8U],
                   bytes[9U],
                   bytes[10U],
                   bytes[11U],
                   bytes[12U]};
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
    uLongf size = static_cast<uLongf>(result.size());
    const int status =
        uncompress(result.data(), &size, compressed.data(), static_cast<uLong>(compressed.size()));
    if (status != Z_OK || size != result.size())
    {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>>
inflate_bounded(const std::span<const std::uint8_t> compressed, const std::size_t maximum_size)
{
    if (compressed.size() > static_cast<std::size_t>(std::numeric_limits<uInt>::max()))
    {
        return std::nullopt;
    }
    z_stream stream{};
    if (inflateInit(&stream) != Z_OK)
    {
        return std::nullopt;
    }
    stream.next_in = const_cast<Bytef *>(compressed.data());
    stream.avail_in = static_cast<uInt>(compressed.size());
    std::vector<std::uint8_t> result;
    std::array<std::uint8_t, 4096U> output{};
    int status = Z_OK;
    while (status == Z_OK)
    {
        stream.next_out = output.data();
        stream.avail_out = static_cast<uInt>(output.size());
        status = inflate(&stream, Z_NO_FLUSH);
        const std::size_t produced = output.size() - stream.avail_out;
        if (produced > maximum_size - result.size())
        {
            inflateEnd(&stream);
            return std::nullopt;
        }
        result.insert(result.end(), output.begin(),
                      output.begin() + static_cast<std::ptrdiff_t>(produced));
    }
    const bool complete = status == Z_STREAM_END && stream.avail_in == 0U;
    inflateEnd(&stream);
    if (!complete)
    {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>> png_icc(const std::vector<PngChunk> &chunks)
{
    const auto profiles = chunks_named(chunks, {'i', 'C', 'C', 'P'});
    if (profiles.size() != 1U)
    {
        return std::nullopt;
    }
    const auto &payload = profiles.front()->payload;
    const auto name_end = std::find(payload.begin(), payload.end(), 0U);
    if (name_end == payload.end())
    {
        return std::nullopt;
    }
    const std::size_t compression_offset =
        static_cast<std::size_t>(name_end - payload.begin()) + 1U;
    if (compression_offset >= payload.size() || payload[compression_offset] != 0U)
    {
        return std::nullopt;
    }
    return inflate_bounded(std::span<const std::uint8_t>(payload).subspan(compression_offset + 1U),
                           detail::kPngMaxIccBytes);
}

[[nodiscard]] std::uint8_t paeth_predictor(const std::uint8_t left, const std::uint8_t above,
                                           const std::uint8_t upper_left)
{
    const int prediction = static_cast<int>(left) + static_cast<int>(above) - upper_left;
    const int left_distance = std::abs(prediction - left);
    const int above_distance = std::abs(prediction - above);
    const int upper_left_distance = std::abs(prediction - upper_left);
    if (left_distance <= above_distance && left_distance <= upper_left_distance)
    {
        return left;
    }
    if (above_distance <= upper_left_distance)
    {
        return above;
    }
    return upper_left;
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>>
png_rgb_sample_bytes(const std::vector<PngChunk> &chunks, const PngIhdr &header)
{
    if ((header.bit_depth != 8U && header.bit_depth != 16U) || header.color_type != 2U ||
        header.interlace != 0U)
    {
        return std::nullopt;
    }
    std::vector<std::uint8_t> compressed;
    for (const PngChunk *chunk : chunks_named(chunks, {'I', 'D', 'A', 'T'}))
    {
        compressed.insert(compressed.end(), chunk->payload.begin(), chunk->payload.end());
    }
    const std::size_t bytes_per_pixel = header.bit_depth == 16U ? 6U : 3U;
    const std::size_t stride = static_cast<std::size_t>(header.width) * bytes_per_pixel;
    const std::size_t packed_size = (stride + 1U) * header.height;
    auto packed = inflate_exact(compressed, packed_size);
    if (!packed)
    {
        return std::nullopt;
    }
    std::vector<std::uint8_t> result(stride * header.height);
    for (std::uint32_t row = 0U; row < header.height; ++row)
    {
        const std::size_t packed_offset = static_cast<std::size_t>(row) * (stride + 1U);
        const std::uint8_t filter = packed.value()[packed_offset];
        if (filter > 4U)
        {
            return std::nullopt;
        }
        for (std::size_t column = 0U; column < stride; ++column)
        {
            const std::uint8_t encoded = packed.value()[packed_offset + 1U + column];
            const std::uint8_t left =
                column >= bytes_per_pixel ?
                    result[static_cast<std::size_t>(row) * stride + column - bytes_per_pixel] :
                    0U;
            const std::uint8_t above =
                row > 0U ? result[(static_cast<std::size_t>(row) - 1U) * stride + column] : 0U;
            const std::uint8_t upper_left =
                row > 0U && column >= bytes_per_pixel ?
                    result[(static_cast<std::size_t>(row) - 1U) * stride + column -
                           bytes_per_pixel] :
                    0U;
            std::uint8_t value = encoded;
            switch (filter)
            {
            case 0U:
                break;
            case 1U:
                value = static_cast<std::uint8_t>(encoded + left);
                break;
            case 2U:
                value = static_cast<std::uint8_t>(encoded + above);
                break;
            case 3U:
                value = static_cast<std::uint8_t>(encoded +
                                                  ((static_cast<unsigned int>(left) + above) / 2U));
                break;
            case 4U:
                value =
                    static_cast<std::uint8_t>(encoded + paeth_predictor(left, above, upper_left));
                break;
            default:
                return std::nullopt;
            }
            result[static_cast<std::size_t>(row) * stride + column] = value;
        }
    }
    return result;
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>>
png_rgb8_pixels(const std::vector<PngChunk> &chunks, const PngIhdr &header)
{
    if (header.bit_depth != 8U)
    {
        return std::nullopt;
    }
    return png_rgb_sample_bytes(chunks, header);
}

[[nodiscard]] std::optional<std::vector<std::uint16_t>>
png_rgb16_pixels(const std::vector<PngChunk> &chunks, const PngIhdr &header)
{
    if (header.bit_depth != 16U)
    {
        return std::nullopt;
    }
    const auto bytes = png_rgb_sample_bytes(chunks, header);
    if (!bytes || bytes->size() % 2U != 0U)
    {
        return std::nullopt;
    }
    std::vector<std::uint16_t> result(bytes->size() / 2U);
    for (std::size_t index = 0U; index < result.size(); ++index)
    {
        result[index] =
            static_cast<std::uint16_t>((static_cast<std::uint16_t>((*bytes)[index * 2U]) << 8U) |
                                       static_cast<std::uint16_t>((*bytes)[index * 2U + 1U]));
    }
    return result;
}

[[nodiscard]] std::vector<std::uint8_t> png_test_pixels(const std::uint32_t width,
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

[[nodiscard]] std::vector<std::uint16_t> png_test_pixels16(const std::uint32_t width,
                                                           const std::uint32_t height)
{
    std::vector<std::uint16_t> pixels(static_cast<std::size_t>(width) * height * 3U);
    for (std::uint32_t row = 0U; row < height; ++row)
    {
        for (std::uint32_t column = 0U; column < width; ++column)
        {
            const std::size_t offset = (static_cast<std::size_t>(row) * width + column) * 3U;
            pixels[offset] =
                static_cast<std::uint16_t>((column * 257U + row * 19U + 0x0102U) & 0xFFFFU);
            pixels[offset + 1U] =
                static_cast<std::uint16_t>((column * 131U + row * 409U + 0x20F1U) & 0xFFFFU);
            pixels[offset + 2U] =
                static_cast<std::uint16_t>((column * 17U + row * 1021U + 0xF00DU) & 0xFFFFU);
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

[[nodiscard]] ColorProfileState builtin_profile(const std::string &identifier)
{
    ColorProfileState profile;
    profile.kind = ColorProfileKind::kBuiltin;
    profile.model = ColorModel::kRgb;
    profile.identifier = identifier;
    return profile;
}

template <typename T>
void expect_png_error(const Result<T> &result, const ErrorCode code, const std::string_view reason)
{
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, code);
    EXPECT_EQ(result.error().context.at("format"), "png");
    EXPECT_EQ(result.error().context.at("reason"), reason);
}

struct PngCheckpointState
{
    CancellationSource cancellation;
    std::uint32_t cancel_row = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t fail_row = std::numeric_limits<std::uint32_t>::max();
    int configured_compression = -1;
    bool configured = false;
    bool cancelled = false;
    bool failed = false;
    detail::PngEncodeInjectedFailure injected_failure =
        detail::PngEncodeInjectedFailure::kEncoderFailure;
};

[[nodiscard]] detail::PngEncodeInjectedFailure
png_checkpoint(void *const context, const detail::PngEncodeCheckpoint checkpoint,
               const std::uint32_t progress, const int configured_compression) noexcept
{
    auto *const state = static_cast<PngCheckpointState *>(context);
    if (checkpoint == detail::PngEncodeCheckpoint::kConfigured)
    {
        state->configured = true;
        state->configured_compression = configured_compression;
    }
    if (checkpoint == detail::PngEncodeCheckpoint::kScanline && progress == state->cancel_row)
    {
        state->cancelled = true;
        (void)state->cancellation.cancel("png-scanline-test");
    }
    if (checkpoint == detail::PngEncodeCheckpoint::kScanline && progress == state->fail_row)
    {
        state->failed = true;
        return state->injected_failure;
    }
    return detail::PngEncodeInjectedFailure::kNone;
}

class CapturingRasterDecoder final : public RasterDecoder
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
        ++encode_calls;
        last_format = format;
        last_png_options = png_options;
        return delegate_.encode(width, height, rgb, color_profile, format, jpeg_options,
                                cancellation, png_options);
    }

    [[nodiscard]] Result<std::vector<std::uint8_t>>
    encode(const std::uint32_t width, const std::uint32_t height,
           const std::vector<std::uint8_t> &rgb, const ColorProfileState &color_profile,
           const ExportFormat format, const JpegExportOptions &jpeg_options,
           const CancellationToken &cancellation, const PngExportOptions &png_options,
           const TiffExportOptions &tiff_options,
           const ExportMetadataSnapshot &metadata) const override
    {
        ++encode_calls;
        last_format = format;
        last_png_options = png_options;
        last_metadata = metadata;
        return delegate_.encode(width, height, rgb, color_profile, format, jpeg_options,
                                cancellation, png_options, tiff_options, metadata);
    }

    [[nodiscard]] Result<std::vector<std::uint8_t>>
    encode(const ExportPixelBuffer &source, const ExportFormat format,
           const JpegExportOptions &jpeg_options, const CancellationToken &cancellation,
           const PngExportOptions &png_options, const TiffExportOptions &tiff_options,
           const ExportMetadataSnapshot &metadata) const override
    {
        ++encode_calls;
        last_format = format;
        last_png_options = png_options;
        last_metadata = metadata;
        return delegate_.encode(source, format, jpeg_options, cancellation, png_options,
                                tiff_options, metadata);
    }

    mutable std::size_t encode_calls = 0U;
    mutable ExportFormat last_format = ExportFormat::kPng;
    mutable PngExportOptions last_png_options;
    mutable ExportMetadataSnapshot last_metadata;

private:
    QtRasterDecoder delegate_;
};

TEST(PngExportContractTest, PropagatesTypedConfiguration)
{
    const auto pixels = png_test_pixels(16U, 8U);
    const QByteArray icc = QColorSpace(QColorSpace::SRgb).iccProfile();
    ASSERT_FALSE(icc.isEmpty());
    const detail::PngEncodeColorMetadata metadata{
        {reinterpret_cast<const std::uint8_t *>(icc.constData()),
         static_cast<std::size_t>(icc.size())},
        true,
        {1U, 13U, 0U, 1U}};

    for (const int compression : {0, 5, 9})
    {
        SCOPED_TRACE(compression);
        const auto configuration = detail::png_encode_configuration({PngBitDepth::k8, compression});
        ASSERT_TRUE(configuration) << configuration.error().message;
        EXPECT_EQ(configuration.value().bit_depth, 8);
        EXPECT_EQ(configuration.value().color_type, 2);
        EXPECT_EQ(configuration.value().interlace_type, 0);
        EXPECT_EQ(configuration.value().compression_type, 0);
        EXPECT_EQ(configuration.value().filter_method, 0);
        EXPECT_EQ(configuration.value().compression_level, compression);
        EXPECT_EQ(configuration.value().compression_mem_level, 8);
        EXPECT_EQ(configuration.value().compression_strategy, Z_DEFAULT_STRATEGY);
        EXPECT_EQ(configuration.value().compression_window_bits, 15);
        EXPECT_EQ(configuration.value().compression_method, 8);
        EXPECT_EQ(configuration.value().compression_buffer_size, 8192U);
        EXPECT_EQ(configuration.value().enabled_filters, 0xF8);

        PngCheckpointState checkpoint;
        detail::PngEncodeControl control;
        control.checkpoint_observer = {&checkpoint, png_checkpoint};
        const auto encoded =
            detail::encode_png_rgb8(16U, 8U, pixels, metadata, {PngBitDepth::k8, compression},
                                    CancellationToken{}, control);
        ASSERT_TRUE(encoded) << encoded.error().message;
        EXPECT_TRUE(checkpoint.configured);
        EXPECT_EQ(checkpoint.configured_compression, compression);
    }

    for (const int compression : {0, 5, 9})
    {
        SCOPED_TRACE(compression);
        const auto configuration =
            detail::png_encode_configuration({PngBitDepth::k16, compression});
        ASSERT_TRUE(configuration) << configuration.error().message;
        EXPECT_EQ(configuration.value().bit_depth, 16);
        EXPECT_EQ(configuration.value().color_type, 2);
        EXPECT_EQ(configuration.value().interlace_type, 0);
        EXPECT_EQ(configuration.value().compression_level, compression);
        EXPECT_EQ(configuration.value().compression_mem_level, 8);
        EXPECT_EQ(configuration.value().compression_strategy, Z_DEFAULT_STRATEGY);
        EXPECT_EQ(configuration.value().enabled_filters, 0xF8);
    }
}

TEST(PngExportContractTest, WritesExactOpaqueRgb8WithIccAndKnownCicp)
{
    constexpr std::uint32_t kWidth = 19U;
    constexpr std::uint32_t kHeight = 11U;
    const auto pixels = png_test_pixels(kWidth, kHeight);
    const auto pixels_before = pixels;
    const ColorProfileState srgb = builtin_profile("srgb");
    const auto profile_before = srgb;
    QtRasterDecoder decoder;

    const auto encoded =
        decoder.encode(kWidth, kHeight, pixels, srgb, ExportFormat::kPng, JpegExportOptions{},
                       CancellationToken{}, PngExportOptions{});
    ASSERT_TRUE(encoded) << encoded.error().message;
    const auto chunks = png_chunks(encoded.value());
    ASSERT_TRUE(chunks);
    const auto header = png_ihdr(chunks.value());
    ASSERT_TRUE(header);
    EXPECT_EQ(header->width, kWidth);
    EXPECT_EQ(header->height, kHeight);
    EXPECT_EQ(header->bit_depth, 8U);
    EXPECT_EQ(header->color_type, 2U);
    EXPECT_EQ(header->compression, 0U);
    EXPECT_EQ(header->filter, 0U);
    EXPECT_EQ(header->interlace, 0U);
    EXPECT_TRUE(chunks_named(chunks.value(), {'t', 'R', 'N', 'S'}).empty());
    EXPECT_TRUE(chunks_named(chunks.value(), {'s', 'R', 'G', 'B'}).empty());
    EXPECT_TRUE(chunks_named(chunks.value(), {'e', 'X', 'I', 'f'}).empty());
    EXPECT_TRUE(chunks_named(chunks.value(), {'i', 'T', 'X', 't'}).empty());
    EXPECT_TRUE(chunks_named(chunks.value(), {'p', 'H', 'Y', 's'}).empty());
    const auto cicp = chunks_named(chunks.value(), {'c', 'I', 'C', 'P'});
    ASSERT_EQ(cicp.size(), 1U);
    EXPECT_EQ(cicp.front()->payload, (std::vector<std::uint8_t>{1U, 13U, 0U, 1U}));
    const auto embedded_icc = png_icc(chunks.value());
    ASSERT_TRUE(embedded_icc);
    EXPECT_EQ(embedded_icc.value(), qbyte_array_bytes(QColorSpace(QColorSpace::SRgb).iccProfile()));
    const auto decoded = png_rgb8_pixels(chunks.value(), header.value());
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value(), pixels);
    EXPECT_EQ(pixels, pixels_before);
    EXPECT_EQ(srgb, profile_before);
}

TEST(PngExportContractTest, EmbedsDisplayP3AndCustomFileProfilesWithoutInventedMetadata)
{
    const auto pixels = png_test_pixels(13U, 7U);
    QtRasterDecoder decoder;

    const ColorProfileState display_p3 = builtin_profile("display_p3");
    const auto display_encoded =
        decoder.encode(13U, 7U, pixels, display_p3, ExportFormat::kPng, JpegExportOptions{},
                       CancellationToken{}, PngExportOptions{});
    ASSERT_TRUE(display_encoded) << display_encoded.error().message;
    const auto display_chunks = png_chunks(display_encoded.value());
    ASSERT_TRUE(display_chunks);
    const auto display_cicp = chunks_named(display_chunks.value(), {'c', 'I', 'C', 'P'});
    ASSERT_EQ(display_cicp.size(), 1U);
    EXPECT_EQ(display_cicp.front()->payload, (std::vector<std::uint8_t>{12U, 13U, 0U, 1U}));
    const auto display_icc = png_icc(display_chunks.value());
    ASSERT_TRUE(display_icc);
    EXPECT_EQ(display_icc.value(),
              qbyte_array_bytes(QColorSpace(QColorSpace::DisplayP3).iccProfile()));

    ColorProfileState custom;
    custom.kind = ColorProfileKind::kIcc;
    custom.model = ColorModel::kRgb;
    custom.identifier = "srgb";
    custom.icc_bytes = qbyte_array_bytes(QColorSpace(QColorSpace::DisplayP3).iccProfile());
    const auto custom_before = custom;
    const auto custom_encoded =
        decoder.encode(13U, 7U, pixels, custom, ExportFormat::kPng, JpegExportOptions{},
                       CancellationToken{}, PngExportOptions{});
    ASSERT_TRUE(custom_encoded) << custom_encoded.error().message;
    const auto custom_chunks = png_chunks(custom_encoded.value());
    ASSERT_TRUE(custom_chunks);
    EXPECT_TRUE(chunks_named(custom_chunks.value(), {'c', 'I', 'C', 'P'}).empty());
    const auto custom_icc = png_icc(custom_chunks.value());
    ASSERT_TRUE(custom_icc);
    EXPECT_EQ(custom_icc.value(), custom.icc_bytes);
    EXPECT_EQ(custom, custom_before);

    const auto unknown =
        decoder.encode(13U, 7U, pixels, builtin_profile("unknown-profile"), ExportFormat::kPng,
                       JpegExportOptions{}, CancellationToken{}, PngExportOptions{});
    expect_png_error(unknown, ErrorCode::kUnsupported, "unsupported_png_output_profile");
}

TEST(PngExportContractTest, WritesExactOpaqueRgb16FromRealSixteenBitSource)
{
    constexpr std::uint32_t kWidth = 17U;
    constexpr std::uint32_t kHeight = 9U;
    const auto pixels = png_test_pixels16(kWidth, kHeight);
    const auto pixels_before = pixels;
    ASSERT_NE(pixels[0], static_cast<std::uint16_t>(pixels[0] >> 8U) * 257U);
    ASSERT_NE(pixels[1] & 0xFFU, pixels[1] >> 8U);
    const QByteArray icc = QColorSpace(QColorSpace::SRgb).iccProfile();
    ASSERT_FALSE(icc.isEmpty());
    const detail::PngEncodeColorMetadata metadata{
        {reinterpret_cast<const std::uint8_t *>(icc.constData()),
         static_cast<std::size_t>(icc.size())},
        true,
        {1U, 13U, 0U, 1U}};

    PngCheckpointState checkpoint;
    detail::PngEncodeControl control;
    control.checkpoint_observer = {&checkpoint, png_checkpoint};
    const auto encoded = detail::encode_png_rgb16(
        kWidth, kHeight, pixels, metadata, {PngBitDepth::k16, 5}, CancellationToken{}, control);
    ASSERT_TRUE(encoded) << encoded.error().message;
    EXPECT_TRUE(checkpoint.configured);
    EXPECT_EQ(checkpoint.configured_compression, 5);
    const auto chunks = png_chunks(encoded.value());
    ASSERT_TRUE(chunks);
    const auto header = png_ihdr(chunks.value());
    ASSERT_TRUE(header);
    EXPECT_EQ(header->width, kWidth);
    EXPECT_EQ(header->height, kHeight);
    EXPECT_EQ(header->bit_depth, 16U);
    EXPECT_EQ(header->color_type, 2U);
    EXPECT_EQ(header->compression, 0U);
    EXPECT_EQ(header->filter, 0U);
    EXPECT_EQ(header->interlace, 0U);
    EXPECT_TRUE(chunks_named(chunks.value(), {'t', 'R', 'N', 'S'}).empty());
    EXPECT_TRUE(chunks_named(chunks.value(), {'s', 'R', 'G', 'B'}).empty());
    EXPECT_TRUE(chunks_named(chunks.value(), {'e', 'X', 'I', 'f'}).empty());
    EXPECT_TRUE(chunks_named(chunks.value(), {'i', 'T', 'X', 't'}).empty());
    EXPECT_TRUE(chunks_named(chunks.value(), {'p', 'H', 'Y', 's'}).empty());
    const auto cicp = chunks_named(chunks.value(), {'c', 'I', 'C', 'P'});
    ASSERT_EQ(cicp.size(), 1U);
    EXPECT_EQ(cicp.front()->payload, (std::vector<std::uint8_t>{1U, 13U, 0U, 1U}));
    const auto embedded_icc = png_icc(chunks.value());
    ASSERT_TRUE(embedded_icc);
    EXPECT_EQ(embedded_icc.value(), qbyte_array_bytes(icc));
    const auto decoded = png_rgb16_pixels(chunks.value(), header.value());
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value(), pixels);
    EXPECT_EQ(pixels, pixels_before);
}

TEST(PngExportContractTest, RejectsEightBitRequestsAndMismatchedRgb16Sources)
{
    const auto pixels = png_test_pixels16(8U, 4U);
    const auto pixels_before = pixels;
    const QByteArray icc_bytes = QColorSpace(QColorSpace::SRgb).iccProfile();
    ASSERT_FALSE(icc_bytes.isEmpty());
    const detail::PngEncodeColorMetadata metadata{
        {reinterpret_cast<const std::uint8_t *>(icc_bytes.constData()),
         static_cast<std::size_t>(icc_bytes.size())},
        true,
        {1U, 13U, 0U, 1U}};

    expect_png_error(
        detail::encode_png_rgb16(8U, 4U, pixels, metadata, PngExportOptions{}, CancellationToken{}),
        ErrorCode::kValidation, "png_16bit_source_requires_16bit_depth");
    expect_png_error(detail::encode_png_rgb16(8U, 4U,
                                              std::span<const std::uint16_t>(pixels).first(5U),
                                              metadata, {PngBitDepth::k16, 5}, CancellationToken{}),
                     ErrorCode::kValidation, "png_source_size_mismatch");
    expect_png_error(detail::encode_png_rgb16(detail::kPngMaxDimension, detail::kPngMaxDimension,
                                              {}, metadata, {PngBitDepth::k16, 5},
                                              CancellationToken{}),
                     ErrorCode::kValidation, "png_source_too_large");

    CancellationSource entry;
    ASSERT_TRUE(entry.cancel("png16-entry-test"));
    expect_png_error(
        detail::encode_png_rgb16(8U, 4U, pixels, metadata, {PngBitDepth::k16, 5}, entry.token()),
        ErrorCode::kCancelled, "png_encode_cancelled");
    EXPECT_EQ(pixels, pixels_before);
}

TEST(PngExportContractTest, RejectsSixteenBitRgb8SourcesAndInvalidOrOversizedInputs)
{
    const auto pixels = png_test_pixels(16U, 8U);
    const QByteArray icc_bytes = QColorSpace(QColorSpace::SRgb).iccProfile();
    ASSERT_FALSE(icc_bytes.isEmpty());
    const detail::PngEncodeColorMetadata metadata{
        {reinterpret_cast<const std::uint8_t *>(icc_bytes.constData()),
         static_cast<std::size_t>(icc_bytes.size())},
        true,
        {1U, 13U, 0U, 1U}};

    expect_png_error(detail::encode_png_rgb8(16U, 8U, pixels, metadata, {PngBitDepth::k16, 5},
                                             CancellationToken{}),
                     ErrorCode::kUnsupported, "unsupported_png_16bit_source");
    expect_png_error(
        detail::encode_png_rgb8(0U, 8U, pixels, metadata, PngExportOptions{}, CancellationToken{}),
        ErrorCode::kValidation, "invalid_png_dimensions");
    expect_png_error(detail::encode_png_rgb8(detail::kPngMaxDimension + 1U, 1U, {}, metadata,
                                             PngExportOptions{}, CancellationToken{}),
                     ErrorCode::kValidation, "invalid_png_dimensions");
    expect_png_error(detail::encode_png_rgb8(20000U, 20000U, {}, metadata, PngExportOptions{},
                                             CancellationToken{}),
                     ErrorCode::kValidation, "png_source_too_large");
    expect_png_error(detail::encode_png_rgb8(16U, 8U,
                                             std::span<const std::uint8_t>(pixels).first(7U),
                                             metadata, PngExportOptions{}, CancellationToken{}),
                     ErrorCode::kValidation, "png_source_size_mismatch");

    detail::PngEncodeColorMetadata missing_icc;
    expect_png_error(detail::encode_png_rgb8(16U, 8U, pixels, missing_icc, PngExportOptions{},
                                             CancellationToken{}),
                     ErrorCode::kValidation, "missing_png_output_icc");
    std::vector<std::uint8_t> oversized_icc(detail::kPngMaxIccBytes + 1U);
    detail::PngEncodeColorMetadata oversized_metadata{oversized_icc, false, {}};
    expect_png_error(detail::encode_png_rgb8(16U, 8U, pixels, oversized_metadata,
                                             PngExportOptions{}, CancellationToken{}),
                     ErrorCode::kValidation, "oversized_png_output_icc");

    detail::PngEncodeControl tiny_output;
    tiny_output.max_output_bytes = 32U;
    expect_png_error(detail::encode_png_rgb8(16U, 8U, pixels, metadata, PngExportOptions{},
                                             CancellationToken{}, tiny_output),
                     ErrorCode::kValidation, "png_output_too_large");

    for (const std::size_t invalid_bound :
         std::array<std::size_t, 2U>{0U, detail::kPngMaxOutputBytes + 1U})
    {
        detail::PngEncodeControl invalid_output;
        invalid_output.max_output_bytes = invalid_bound;
        expect_png_error(detail::encode_png_rgb8(16U, 8U, pixels, metadata, PngExportOptions{},
                                                 CancellationToken{}, invalid_output),
                         ErrorCode::kValidation, "invalid_png_output_bound");
    }

    for (const std::array<std::uint8_t, 4U> invalid_cicp :
         {std::array<std::uint8_t, 4U>{0U, 13U, 0U, 1U},
          std::array<std::uint8_t, 4U>{1U, 0U, 0U, 1U},
          std::array<std::uint8_t, 4U>{1U, 13U, 1U, 1U},
          std::array<std::uint8_t, 4U>{1U, 13U, 0U, 0U}})
    {
        detail::PngEncodeColorMetadata invalid_metadata = metadata;
        invalid_metadata.cicp = invalid_cicp;
        expect_png_error(detail::encode_png_rgb8(16U, 8U, pixels, invalid_metadata,
                                                 PngExportOptions{}, CancellationToken{}),
                         ErrorCode::kValidation, "invalid_png_cicp");
    }

    ColorProfileState non_rgb = builtin_profile("srgb");
    non_rgb.model = ColorModel::kLab;
    QtRasterDecoder decoder;
    expect_png_error(decoder.encode(16U, 8U, pixels, non_rgb, ExportFormat::kPng,
                                    JpegExportOptions{}, CancellationToken{}, PngExportOptions{}),
                     ErrorCode::kUnsupported, "unsupported_png_output_icc_color_model");
}

TEST(PngExportContractTest, CancellationAndInjectedFailurePublishNoBytesOrMutations)
{
    const auto pixels = png_test_pixels(128U, 32U);
    const auto pixels_before = pixels;
    const auto profile = qbyte_array_bytes(QColorSpace(QColorSpace::SRgb).iccProfile());
    const auto profile_before = profile;
    const detail::PngEncodeColorMetadata metadata{profile, true, {1U, 13U, 0U, 1U}};

    CancellationSource entry;
    ASSERT_TRUE(entry.cancel("png-entry-test"));
    const auto entry_result =
        detail::encode_png_rgb8(128U, 32U, pixels, metadata, PngExportOptions{}, entry.token());
    expect_png_error(entry_result, ErrorCode::kCancelled, "png_encode_cancelled");

    PngCheckpointState row_cancel;
    row_cancel.cancel_row = 5U;
    detail::PngEncodeControl cancel_control;
    cancel_control.checkpoint_observer = {&row_cancel, png_checkpoint};
    const auto cancelled = detail::encode_png_rgb8(128U, 32U, pixels, metadata, PngExportOptions{},
                                                   row_cancel.cancellation.token(), cancel_control);
    expect_png_error(cancelled, ErrorCode::kCancelled, "png_encode_cancelled");
    EXPECT_TRUE(row_cancel.cancelled);

    PngCheckpointState injected;
    injected.fail_row = 7U;
    detail::PngEncodeControl failure_control;
    failure_control.checkpoint_observer = {&injected, png_checkpoint};
    const auto failed = detail::encode_png_rgb8(128U, 32U, pixels, metadata, PngExportOptions{},
                                                CancellationToken{}, failure_control);
    expect_png_error(failed, ErrorCode::kIo, "png_encoder_failure");
    EXPECT_TRUE(injected.failed);

    PngCheckpointState allocation;
    allocation.fail_row = 3U;
    allocation.injected_failure = detail::PngEncodeInjectedFailure::kAllocationFailure;
    detail::PngEncodeControl allocation_control;
    allocation_control.checkpoint_observer = {&allocation, png_checkpoint};
    const auto allocation_failed = detail::encode_png_rgb8(
        128U, 32U, pixels, metadata, PngExportOptions{}, CancellationToken{}, allocation_control);
    expect_png_error(allocation_failed, ErrorCode::kIo, "png_output_allocation_failed");
    EXPECT_TRUE(allocation.failed);
    EXPECT_EQ(pixels, pixels_before);
    EXPECT_EQ(profile, profile_before);
}

TEST(PngCatalogTest, ForwardsDefaultsAndExplicitOptionsWithFormatIsolation)
{
    PngExportTempDirectory temporary;
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
    const auto database_path = (temporary.path() / "library.sqlite").string();

    auto engine_result = EngineFacade::create_phase1();
    ASSERT_TRUE(engine_result) << engine_result.error().message;
    auto repository = SqliteCatalogRepository::create(database_path);
    ASSERT_TRUE(repository) << repository.error().message;
    auto cache = FilesystemPreviewCache::create(database_path + ".preview");
    ASSERT_TRUE(cache) << cache.error().message;
    auto raster = std::make_unique<CapturingRasterDecoder>();
    CapturingRasterDecoder *const capturing = raster.get();
    CatalogService service(std::move(engine_result).value(), std::move(repository).value(),
                           std::move(raster), std::move(cache).value());

    const auto imported = service.import_one(input_path.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);

    ExportRequest defaults;
    defaults.asset_id = imported.value().asset->id;
    defaults.output_path = (temporary.path() / "default.png").string();
    defaults.format = ExportFormat::kPng;
    const auto default_export = service.export_asset(defaults);
    ASSERT_TRUE(default_export) << default_export.error().message;
    EXPECT_EQ(capturing->last_format, ExportFormat::kPng);
    EXPECT_EQ(capturing->last_png_options, PngExportOptions());
    EXPECT_TRUE(std::filesystem::is_regular_file(defaults.output_path));

    ExportRequest explicit_options = defaults;
    explicit_options.output_path = (temporary.path() / "compression-9.png").string();
    explicit_options.png_options = {PngBitDepth::k8, 9};
    const auto explicit_export = service.export_asset(explicit_options);
    ASSERT_TRUE(explicit_export) << explicit_export.error().message;
    EXPECT_EQ(capturing->last_png_options, explicit_options.png_options);
    EXPECT_TRUE(std::filesystem::is_regular_file(explicit_options.output_path));

    DevelopParams develop;
    develop.exposure_ev = 0.37;
    const auto saved = service.save_develop(imported.value().asset->id, develop);
    ASSERT_TRUE(saved) << saved.error().message;

    ExportRequest eight_bit_after_edit = defaults;
    eight_bit_after_edit.output_path = (temporary.path() / "edited-8.png").string();
    const auto eight_bit_export = service.export_asset(eight_bit_after_edit);
    ASSERT_TRUE(eight_bit_export) << eight_bit_export.error().message;

    const std::size_t calls_before_sixteen = capturing->encode_calls;
    ExportRequest sixteen_bit = defaults;
    sixteen_bit.output_path = (temporary.path() / "edited-16.png").string();
    sixteen_bit.png_options = {PngBitDepth::k16, 5};
    const auto sixteen = service.export_asset(sixteen_bit);
    ASSERT_TRUE(sixteen) << sixteen.error().message;
    EXPECT_EQ(capturing->encode_calls, calls_before_sixteen + 1U);
    EXPECT_EQ(capturing->last_png_options, sixteen_bit.png_options);
    EXPECT_TRUE(std::filesystem::is_regular_file(sixteen_bit.output_path));

    const auto eight_chunks =
        png_chunks(qbyte_array_bytes(read_file(eight_bit_after_edit.output_path)));
    ASSERT_TRUE(eight_chunks);
    const auto eight_header = png_ihdr(eight_chunks.value());
    ASSERT_TRUE(eight_header);
    const auto eight_pixels = png_rgb8_pixels(eight_chunks.value(), eight_header.value());
    ASSERT_TRUE(eight_pixels);
    const auto sixteen_chunks = png_chunks(qbyte_array_bytes(read_file(sixteen_bit.output_path)));
    ASSERT_TRUE(sixteen_chunks);
    const auto sixteen_header = png_ihdr(sixteen_chunks.value());
    ASSERT_TRUE(sixteen_header);
    EXPECT_EQ(sixteen_header->bit_depth, 16U);
    EXPECT_EQ(sixteen_header->color_type, 2U);
    EXPECT_EQ(sixteen_header->interlace, 0U);
    EXPECT_TRUE(chunks_named(sixteen_chunks.value(), {'e', 'X', 'I', 'f'}).empty());
    EXPECT_TRUE(chunks_named(sixteen_chunks.value(), {'p', 'H', 'Y', 's'}).empty());
    const auto cicp = chunks_named(sixteen_chunks.value(), {'c', 'I', 'C', 'P'});
    ASSERT_EQ(cicp.size(), 1U);
    EXPECT_EQ(cicp.front()->payload, (std::vector<std::uint8_t>{1U, 13U, 0U, 1U}));
    ASSERT_TRUE(png_icc(sixteen_chunks.value()));
    const auto sixteen_pixels = png_rgb16_pixels(sixteen_chunks.value(), sixteen_header.value());
    ASSERT_TRUE(sixteen_pixels);
    ASSERT_EQ(sixteen_pixels->size(), eight_pixels->size());
    bool found_non_expansion = false;
    for (std::size_t index = 0U; index < sixteen_pixels->size(); ++index)
    {
        if ((*sixteen_pixels)[index] != static_cast<std::uint16_t>((*eight_pixels)[index]) * 257U)
        {
            found_non_expansion = true;
            break;
        }
    }
    EXPECT_TRUE(found_non_expansion);

    const std::size_t calls_before_invalid = capturing->encode_calls;

    ExportRequest invalid = defaults;
    invalid.output_path = (temporary.path() / "invalid.png").string();
    invalid.png_options.compression = 10;
    const auto invalid_result = service.export_asset(invalid);
    expect_png_error(invalid_result, ErrorCode::kValidation, "invalid_png_compression");
    EXPECT_EQ(capturing->encode_calls, calls_before_invalid);
    EXPECT_FALSE(std::filesystem::exists(invalid.output_path));

    ExportRequest invalid_depth = defaults;
    invalid_depth.output_path = (temporary.path() / "invalid-depth.png").string();
    invalid_depth.png_options.bit_depth = static_cast<PngBitDepth>(255U);
    const auto invalid_depth_result = service.export_asset(invalid_depth);
    expect_png_error(invalid_depth_result, ErrorCode::kValidation, "invalid_png_bit_depth");
    EXPECT_EQ(capturing->encode_calls, calls_before_invalid);
    EXPECT_FALSE(std::filesystem::exists(invalid_depth.output_path));

    const PngExportOptions deliberately_invalid{static_cast<PngBitDepth>(255U), -1};
    for (const auto [format, suffix] : std::array<std::pair<ExportFormat, std::string_view>, 2U>{
             {{ExportFormat::kJpeg, ".jpg"}, {ExportFormat::kTiff, ".tif"}}})
    {
        ExportRequest unrelated = defaults;
        unrelated.format = format;
        unrelated.output_path = (temporary.path() / ("unrelated" + std::string(suffix))).string();
        unrelated.png_options = deliberately_invalid;
        const auto exported = service.export_asset(unrelated);
        ASSERT_TRUE(exported) << exported.error().message;
        EXPECT_EQ(capturing->last_format, format);
        EXPECT_EQ(capturing->last_png_options, deliberately_invalid);
        if (format == ExportFormat::kTiff)
        {
            const auto normalized_output = normalize_local_input(unrelated.output_path);
            ASSERT_TRUE(normalized_output) << normalized_output.error().message;
            EXPECT_EQ(capturing->last_metadata.destination_document_name,
                      normalized_output.value().path);
            EXPECT_EQ(capturing->last_metadata.writable, WritableMetadata{});
        }
        else
        {
            EXPECT_EQ(capturing->last_metadata, ExportMetadataSnapshot{});
        }
        EXPECT_TRUE(std::filesystem::is_regular_file(unrelated.output_path));
    }

    EXPECT_EQ(QCryptographicHash::hash(read_file(input_path), QCryptographicHash::Sha256),
              source_hash);
    EXPECT_TRUE(service.close());
}

} // namespace
} // namespace ravo

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    ravo::init_logging("ravo-png-export-tests");
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    ravo::shutdown_logging();
    return result;
}
