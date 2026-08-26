#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QIODevice>
#include <gtest/gtest.h>

#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/adapters/radiance_rgbe_decoder.h"
#include "ravo/domain/hdr_decoder.h"
#include "ravo/domain/types.h"
#include "ravo/foundation/cancellation.h"

namespace ravo
{
namespace
{

using RgbePixel = std::array<std::uint8_t, 4>;
using RgbeChannels = std::array<std::vector<std::uint8_t>, 4>;

class RgbeTempDirectory
{
public:
    RgbeTempDirectory()
        : path_(std::filesystem::temp_directory_path() /
                ("ravo-rgbe-adapter-" + generate_catalog_id()))
    {
        std::filesystem::create_directories(path_);
    }

    ~RgbeTempDirectory()
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

[[nodiscard]] std::vector<std::uint8_t> vector_bytes(const QByteArray &bytes)
{
    return {reinterpret_cast<const std::uint8_t *>(bytes.constData()),
            reinterpret_cast<const std::uint8_t *>(bytes.constData()) + bytes.size()};
}

void append_byte(QByteArray &bytes, const std::uint8_t value)
{
    bytes.append(static_cast<char>(value));
}

void append_pixel(QByteArray &bytes, const RgbePixel &pixel)
{
    for (const std::uint8_t value : pixel)
    {
        append_byte(bytes, value);
    }
}

[[nodiscard]] QByteArray rgbe_header(const std::string_view magic, const std::uint32_t width,
                                     const std::uint32_t height,
                                     const std::vector<QByteArray> &metadata = {},
                                     const std::string_view format = "32-bit_rle_rgbe",
                                     const std::string_view resolution_prefix = "-Y",
                                     const std::string_view x_axis = "+X")
{
    QByteArray bytes(magic.data(), static_cast<qsizetype>(magic.size()));
    bytes.append('\n');
    for (const QByteArray &line : metadata)
    {
        bytes.append(line);
        bytes.append('\n');
    }
    bytes.append("FORMAT=");
    bytes.append(format.data(), static_cast<qsizetype>(format.size()));
    bytes.append("\n\n");
    bytes.append(resolution_prefix.data(), static_cast<qsizetype>(resolution_prefix.size()));
    bytes.append(' ');
    bytes.append(QByteArray::number(height));
    bytes.append(' ');
    bytes.append(x_axis.data(), static_cast<qsizetype>(x_axis.size()));
    bytes.append(' ');
    bytes.append(QByteArray::number(width));
    bytes.append('\n');
    return bytes;
}

[[nodiscard]] QByteArray flat_rgbe(const std::uint32_t width, const std::uint32_t height,
                                   const std::vector<RgbePixel> &pixels,
                                   const std::string_view magic = "#?RADIANCE",
                                   const std::vector<QByteArray> &metadata = {})
{
    QByteArray bytes = rgbe_header(magic, width, height, metadata);
    for (const RgbePixel &pixel : pixels)
    {
        append_pixel(bytes, pixel);
    }
    return bytes;
}

void append_rle_channel(QByteArray &bytes, const std::vector<std::uint8_t> &values)
{
    ASSERT_FALSE(values.empty());
    ASSERT_LE(values.size(), 127U);
    bool constant = true;
    for (const std::uint8_t value : values)
    {
        constant = constant && value == values.front();
    }
    if (constant)
    {
        append_byte(bytes, static_cast<std::uint8_t>(128U + values.size()));
        append_byte(bytes, values.front());
        return;
    }
    append_byte(bytes, static_cast<std::uint8_t>(values.size()));
    for (const std::uint8_t value : values)
    {
        append_byte(bytes, value);
    }
}

[[nodiscard]] QByteArray rle_rgbe(const std::uint32_t width,
                                  const std::vector<RgbeChannels> &scanlines,
                                  const std::string_view magic = "#?RADIANCE")
{
    QByteArray bytes = rgbe_header(magic, width, static_cast<std::uint32_t>(scanlines.size()));
    for (const RgbeChannels &scanline : scanlines)
    {
        append_byte(bytes, 2U);
        append_byte(bytes, 2U);
        append_byte(bytes, static_cast<std::uint8_t>((width >> 8U) & 0xFFU));
        append_byte(bytes, static_cast<std::uint8_t>(width & 0xFFU));
        for (const auto &channel : scanline)
        {
            append_rle_channel(bytes, channel);
        }
    }
    return bytes;
}

void write_file(const std::filesystem::path &path, const QByteArray &bytes)
{
    QFile file(QString::fromStdString(path.string()));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(file.write(bytes), bytes.size());
    file.close();
}

[[nodiscard]] QByteArray read_file(const std::filesystem::path &path)
{
    QFile file(QString::fromStdString(path.string()));
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    return file.readAll();
}

template <typename Value>
void expect_rgbe_error(const Result<Value> &result, const ErrorCode code,
                       const std::string_view reason, const std::string_view source = "memory")
{
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, code);
    ASSERT_TRUE(result.error().context.contains("format"));
    EXPECT_EQ(result.error().context.at("format"), "rgbe");
    ASSERT_TRUE(result.error().context.contains("reason"));
    EXPECT_EQ(result.error().context.at("reason"), reason);
    ASSERT_TRUE(result.error().context.contains("source"));
    EXPECT_EQ(result.error().context.at("source"), source);
}

void expect_float_bits(const float actual, const float expected)
{
    EXPECT_EQ(std::bit_cast<std::uint32_t>(actual), std::bit_cast<std::uint32_t>(expected));
}

[[nodiscard]] float frozen_rgbe_channel(const std::uint8_t mantissa, const std::uint8_t exponent)
{
    if (exponent == 0U)
    {
        return 0.0F;
    }
    const float value =
        static_cast<float>(mantissa) * std::ldexp(1.0F, static_cast<int>(exponent) - 136);
    return std::fmax(0.0F, std::fmin(10000.0F, value));
}

TEST(RgbeAdapterTest, DecodesFrozenFlatPixelsForBothMagicTokens)
{
    RadianceRgbeDecoder decoder;
    for (const std::string_view magic : {"#?RADIANCE", "#?RGBE"})
    {
        const QByteArray bytes =
            flat_rgbe(2U, 1U, {{{128U, 64U, 32U, 129U}, {1U, 2U, 3U, 128U}}}, magic);
        const auto decoded = decoder.decode_memory(vector_bytes(bytes), CancellationToken{});
        ASSERT_TRUE(decoded) << decoded.error().message;
        EXPECT_EQ(decoded.value().width, 2U);
        EXPECT_EQ(decoded.value().height, 1U);
        ASSERT_EQ(decoded.value().rgb.size(), 6U);
        expect_float_bits(decoded.value().rgb[0], 1.0F);
        expect_float_bits(decoded.value().rgb[1], 0.5F);
        expect_float_bits(decoded.value().rgb[2], 0.25F);
        expect_float_bits(decoded.value().rgb[3], 1.0F / 256.0F);
        expect_float_bits(decoded.value().rgb[4], 2.0F / 256.0F);
        expect_float_bits(decoded.value().rgb[5], 3.0F / 256.0F);
        EXPECT_EQ(decoded.value().radiance.program_type, magic.substr(2U));
        EXPECT_EQ(decoded.value().pixel_format, HdrPixelFormat::kLinearRgbF32);
        EXPECT_EQ(decoded.value().alpha_mode, HdrAlphaMode::kOpaque);
    }
}

TEST(RgbeAdapterTest, DecodesNewPerChannelRleAndClampsLegacyFloatOutput)
{
    RgbeChannels first;
    first[0] = {128U, 128U, 128U, 128U, 255U, 255U, 255U, 255U};
    first[1] = {64U, 48U, 32U, 16U, 255U, 128U, 64U, 32U};
    first[2] = {32U, 32U, 32U, 32U, 1U, 2U, 3U, 4U};
    first[3] = {129U, 129U, 129U, 129U, 255U, 255U, 255U, 255U};
    RgbeChannels second;
    second[0] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
    second[1] = {8U, 7U, 6U, 5U, 4U, 3U, 2U, 1U};
    second[2] = {9U, 9U, 9U, 9U, 9U, 9U, 9U, 9U};
    second[3] = {128U, 128U, 128U, 128U, 129U, 129U, 129U, 129U};
    const std::vector<RgbeChannels> scanlines{first, second};
    RadianceRgbeDecoder decoder;
    const auto decoded =
        decoder.decode_memory(vector_bytes(rle_rgbe(8U, scanlines, "#?RGBE")), CancellationToken{});
    ASSERT_TRUE(decoded) << decoded.error().message;
    ASSERT_EQ(decoded.value().height, 2U);
    ASSERT_EQ(decoded.value().rgb.size(), 48U);
    std::size_t output = 0U;
    for (const RgbeChannels &scanline : scanlines)
    {
        for (std::size_t column = 0U; column < 8U; ++column)
        {
            for (std::size_t channel = 0U; channel < 3U; ++channel)
            {
                expect_float_bits(
                    decoded.value().rgb[output++],
                    frozen_rgbe_channel(scanline[channel][column], scanline[3][column]));
            }
        }
    }
}

TEST(RgbeAdapterTest, ZeroExponentOverridesMantissasAndTrailingBytesAreIgnored)
{
    const QByteArray canonical = flat_rgbe(1U, 1U, {{{255U, 254U, 253U, 0U}}});
    QByteArray with_trailing = canonical;
    with_trailing.append("ignored trailing bytes");
    QByteArray extra_blank("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n\n-Y 1 +X 1\n");
    append_pixel(extra_blank, {255U, 254U, 253U, 0U});
    RadianceRgbeDecoder decoder;
    const auto plain = decoder.decode_memory(vector_bytes(canonical), CancellationToken{});
    const auto trailing = decoder.decode_memory(vector_bytes(with_trailing), CancellationToken{});
    const auto blank = decoder.decode_memory(vector_bytes(extra_blank), CancellationToken{});
    ASSERT_TRUE(plain) << plain.error().message;
    ASSERT_TRUE(trailing) << trailing.error().message;
    ASSERT_TRUE(blank) << blank.error().message;
    ASSERT_EQ(plain.value().rgb.size(), 3U);
    for (const float channel : plain.value().rgb)
    {
        expect_float_bits(channel, 0.0F);
    }
    EXPECT_EQ(trailing.value().rgb, plain.value().rgb);
    EXPECT_EQ(trailing.value().radiance, plain.value().radiance);
    EXPECT_EQ(blank.value().rgb, plain.value().rgb);
}

TEST(RgbeAdapterTest, PreservesWideFlatFallbackAndNarrowMarkerLikePixels)
{
    std::vector<RgbePixel> wide_pixels;
    for (std::uint8_t index = 0U; index < 8U; ++index)
    {
        wide_pixels.push_back({static_cast<std::uint8_t>(16U + index),
                               static_cast<std::uint8_t>(8U + index),
                               static_cast<std::uint8_t>(4U + index), 129U});
    }
    RadianceRgbeDecoder decoder;
    const auto wide =
        decoder.decode_memory(vector_bytes(flat_rgbe(8U, 1U, wide_pixels)), CancellationToken{});
    ASSERT_TRUE(wide) << wide.error().message;
    ASSERT_EQ(wide.value().rgb.size(), 24U);
    for (std::size_t pixel = 0U; pixel < wide_pixels.size(); ++pixel)
    {
        for (std::size_t channel = 0U; channel < 3U; ++channel)
        {
            expect_float_bits(
                wide.value().rgb[pixel * 3U + channel],
                frozen_rgbe_channel(wide_pixels[pixel][channel], wide_pixels[pixel][3]));
        }
    }

    std::vector<RgbePixel> narrow_pixels(7U, {0U, 0U, 0U, 0U});
    // Width seven is outside the new-RLE domain, so an exact 2,2,0,7
    // scanline-like prefix remains an ordinary frozen flat pixel.
    narrow_pixels.front() = {2U, 2U, 0U, 7U};
    const auto narrow =
        decoder.decode_memory(vector_bytes(flat_rgbe(7U, 1U, narrow_pixels)), CancellationToken{});
    ASSERT_TRUE(narrow) << narrow.error().message;
    ASSERT_EQ(narrow.value().rgb.size(), 21U);
    expect_float_bits(narrow.value().rgb[0], frozen_rgbe_channel(2U, 7U));
    expect_float_bits(narrow.value().rgb[1], frozen_rgbe_channel(2U, 7U));
    expect_float_bits(narrow.value().rgb[2], 0.0F);
}

TEST(RgbeAdapterTest, RecordsGammaExposureWithoutChangingPixels)
{
    const std::vector<RgbePixel> pixels{{64U, 32U, 16U, 129U}};
    RadianceRgbeDecoder decoder;
    const auto plain =
        decoder.decode_memory(vector_bytes(flat_rgbe(1U, 1U, pixels)), CancellationToken{});
    const auto annotated =
        decoder.decode_memory(vector_bytes(flat_rgbe(1U, 1U, pixels, "#?RADIANCE",
                                                     {"GAMMA=2.2trailing", "GAMMA=not-a-number",
                                                      "EXPOSURE=4.0", "EXPOSURE=invalid"})),
                              CancellationToken{});
    ASSERT_TRUE(plain) << plain.error().message;
    ASSERT_TRUE(annotated) << annotated.error().message;
    EXPECT_EQ(annotated.value().rgb, plain.value().rgb);
    EXPECT_TRUE(annotated.value().radiance.has_gamma);
    EXPECT_FLOAT_EQ(annotated.value().radiance.gamma, 2.2F);
    EXPECT_TRUE(annotated.value().radiance.has_exposure);
    EXPECT_FLOAT_EQ(annotated.value().radiance.exposure, 4.0F);
    EXPECT_FALSE(plain.value().radiance.has_gamma);
    EXPECT_FALSE(plain.value().radiance.has_exposure);
    EXPECT_FLOAT_EQ(plain.value().radiance.gamma, 1.0F);
    EXPECT_FLOAT_EQ(plain.value().radiance.exposure, 1.0F);
}

TEST(RgbeAdapterTest, ReproducesDefaultAndCustomPrimariesMatrices)
{
    RadianceRgbeDecoder decoder;
    const std::vector<RgbePixel> pixel{{0U, 0U, 0U, 0U}};
    const auto defaults =
        decoder.decode_memory(vector_bytes(flat_rgbe(1U, 1U, pixel)), CancellationToken{});
    ASSERT_TRUE(defaults) << defaults.error().message;
    EXPECT_FALSE(defaults.value().radiance.has_custom_primaries);
    const std::array<float, 8> expected_defaults{0.640F, 0.330F, 0.290F, 0.600F,
                                                 0.150F, 0.060F, 0.333F, 0.333F};
    EXPECT_EQ(defaults.value().radiance.primaries_xy, expected_defaults);
    const std::array<std::uint32_t, 9> default_rgb_to_xyz_bits{
        0x3F0376AAU, 0x3EA5D9C5U, 0x3E2671CEU, 0x3E879260U, 0x3F2B91D6U,
        0x3D8527D7U, 0x3CC53200U, 0x3DFBA2B2U, 0x3F5B26E8U};
    const std::array<std::uint32_t, 9> default_xyz_to_rgb_bits{
        0x4024606FU, 0xBF958920U, 0xBECC41F2U, 0xBF82D009U, 0x3FFD2FF6U,
        0x3D337839U, 0x3D987F17U, 0xBE8089E5U, 0x3F962719U};
    for (std::size_t index = 0U; index < 9U; ++index)
    {
        EXPECT_EQ(std::bit_cast<std::uint32_t>(defaults.value().radiance.rgb_to_xyz[index]),
                  default_rgb_to_xyz_bits[index]);
        EXPECT_EQ(std::bit_cast<std::uint32_t>(defaults.value().radiance.xyz_to_rgb[index]),
                  default_xyz_to_rgb_bits[index]);
    }

    const auto custom = decoder.decode_memory(
        vector_bytes(flat_rgbe(1U, 1U, pixel, "#?RADIANCE",
                               {"PRIMARIES=0.64 0.33 0.30 0.60 0.15 0.06 0.3127 0.3290"})),
        CancellationToken{});
    ASSERT_TRUE(custom) << custom.error().message;
    EXPECT_TRUE(custom.value().radiance.has_custom_primaries);
    const std::array<float, 9> expected_rgb_to_xyz{0.412391F, 0.357584F, 0.180481F,
                                                   0.212639F, 0.715169F, 0.072192F,
                                                   0.019331F, 0.119195F, 0.950532F};
    const std::array<float, 9> expected_xyz_to_rgb{3.240970F,  -1.537383F, -0.498611F,
                                                   -0.969244F, 1.875968F,  0.041555F,
                                                   0.055630F,  -0.203977F, 1.056972F};
    for (std::size_t index = 0U; index < 9U; ++index)
    {
        EXPECT_NEAR(custom.value().radiance.rgb_to_xyz[index], expected_rgb_to_xyz[index], 1.0e-5F);
        EXPECT_NEAR(custom.value().radiance.xyz_to_rgb[index], expected_xyz_to_rgb[index], 1.0e-5F);
    }
}

TEST(RgbeAdapterTest, RejectsInvalidPrimariesBeforePixelDecode)
{
    RadianceRgbeDecoder decoder;
    for (const QByteArray &primaries :
         {QByteArray("PRIMARIES=0.64 0 0.30 0.60 0.15 0.06 0.3127 0.3290"),
          QByteArray("PRIMARIES=0.64 0.33 0.64 0.33 0.15 0.06 0.3127 0.3290"),
          QByteArray("PRIMARIES=nan 0.33 0.30 0.60 0.15 0.06 0.3127 0.3290")})
    {
        const QByteArray bytes = flat_rgbe(1U, 1U, {}, "#?RADIANCE", {primaries});
        expect_rgbe_error(decoder.decode_memory(vector_bytes(bytes), CancellationToken{}),
                          ErrorCode::kValidation, "invalid_rgbe_primaries");
    }
}

TEST(RgbeAdapterTest, RejectsFormatsOrientationsAndOldRleOutsideFrozenDomain)
{
    RadianceRgbeDecoder decoder;

    QByteArray orientation = rgbe_header("#?RADIANCE", 1U, 1U, {}, "32-bit_rle_rgbe", "+Y");
    orientation.append(QByteArray(4, '\0'));
    expect_rgbe_error(decoder.decode_memory(vector_bytes(orientation), CancellationToken{}),
                      ErrorCode::kUnsupported, "unsupported_rgbe_orientation");

    QByteArray reverse_x = rgbe_header("#?RADIANCE", 1U, 1U, {}, "32-bit_rle_rgbe", "-Y", "-X");
    reverse_x.append(QByteArray(4, '\0'));
    expect_rgbe_error(decoder.decode_memory(vector_bytes(reverse_x), CancellationToken{}),
                      ErrorCode::kUnsupported, "unsupported_rgbe_orientation");

    QByteArray leading_space("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n -Y 1 +X 1\n");
    leading_space.append(QByteArray(4, '\0'));
    expect_rgbe_error(decoder.decode_memory(vector_bytes(leading_space), CancellationToken{}),
                      ErrorCode::kValidation, "invalid_rgbe_resolution");

    QByteArray xyze = rgbe_header("#?RADIANCE", 1U, 1U, {}, "32-bit_rle_xyze");
    xyze.append(QByteArray(4, '\0'));
    expect_rgbe_error(decoder.decode_memory(vector_bytes(xyze), CancellationToken{}),
                      ErrorCode::kUnsupported, "unsupported_rgbe_format");

    QByteArray old_rle = rgbe_header("#?RADIANCE", 8U, 1U);
    append_pixel(old_rle, {16U, 8U, 4U, 129U});
    // The legacy routine does not implement old RLE: it reads this marker as
    // an ordinary flat pixel. Ravo rejects old-RLE markers deliberately, which
    // also excludes marker-like legal flat pixels. That subdomain is an
    // explicit product incompatibility and is not claimed as bit-exact.
    append_pixel(old_rle, {1U, 1U, 1U, 7U});
    for (std::uint32_t index = 2U; index < 8U; ++index)
    {
        append_pixel(old_rle, {0U, 0U, 0U, 0U});
    }
    expect_rgbe_error(decoder.decode_memory(vector_bytes(old_rle), CancellationToken{}),
                      ErrorCode::kUnsupported, "unsupported_rgbe_old_rle");
}

TEST(RgbeAdapterTest, ReportsStableSignatureHeaderAndPixelFailures)
{
    RadianceRgbeDecoder decoder;
    expect_rgbe_error(
        decoder.decode_memory(vector_bytes(QByteArray("random", 6)), CancellationToken{}),
        ErrorCode::kUnsupported, "unsupported_rgbe_signature");

    QByteArray missing_format("#?RADIANCE\n\n-Y 1 +X 1\n");
    missing_format.append(QByteArray(4, '\0'));
    expect_rgbe_error(decoder.decode_memory(vector_bytes(missing_format), CancellationToken{}),
                      ErrorCode::kValidation, "missing_rgbe_format");

    const QByteArray truncated = flat_rgbe(2U, 1U, {{{1U, 2U, 3U, 128U}}});
    expect_rgbe_error(decoder.decode_memory(vector_bytes(truncated), CancellationToken{}),
                      ErrorCode::kValidation, "truncated_rgbe_pixels");

    QByteArray wrong_width = rgbe_header("#?RADIANCE", 8U, 1U);
    append_pixel(wrong_width, {2U, 2U, 0U, 9U});
    expect_rgbe_error(decoder.decode_memory(vector_bytes(wrong_width), CancellationToken{}),
                      ErrorCode::kValidation, "rgbe_rle_width_mismatch");

    QByteArray zero_packet = rgbe_header("#?RADIANCE", 8U, 1U);
    append_pixel(zero_packet, {2U, 2U, 0U, 8U});
    append_byte(zero_packet, 0U);
    append_byte(zero_packet, 1U);
    expect_rgbe_error(decoder.decode_memory(vector_bytes(zero_packet), CancellationToken{}),
                      ErrorCode::kValidation, "invalid_rgbe_rle_packet");

    QByteArray overshoot = rgbe_header("#?RADIANCE", 8U, 1U);
    append_pixel(overshoot, {2U, 2U, 0U, 8U});
    append_byte(overshoot, 137U);
    append_byte(overshoot, 1U);
    expect_rgbe_error(decoder.decode_memory(vector_bytes(overshoot), CancellationToken{}),
                      ErrorCode::kValidation, "invalid_rgbe_rle_packet");

    QByteArray later_literal = rgbe_header("#?RADIANCE", 8U, 1U);
    append_pixel(later_literal, {2U, 2U, 0U, 8U});
    append_byte(later_literal, 136U);
    append_byte(later_literal, 8U);
    append_byte(later_literal, 8U);
    append_byte(later_literal, 1U);
    append_byte(later_literal, 2U);
    append_byte(later_literal, 3U);
    expect_rgbe_error(decoder.decode_memory(vector_bytes(later_literal), CancellationToken{}),
                      ErrorCode::kValidation, "truncated_rgbe_rle_packet");

    QByteArray later_run = rgbe_header("#?RADIANCE", 8U, 1U);
    append_pixel(later_run, {2U, 2U, 0U, 8U});
    append_byte(later_run, 136U);
    append_byte(later_run, 8U);
    append_byte(later_run, 136U);
    expect_rgbe_error(decoder.decode_memory(vector_bytes(later_run), CancellationToken{}),
                      ErrorCode::kValidation, "truncated_rgbe_rle_packet");
}

TEST(RgbeAdapterTest, RejectsAmbiguousOrUnboundedHeaders)
{
    RadianceRgbeDecoder decoder;
    // Duplicate FORMAT and fixed header resource caps are hardened input
    // boundaries, not claims about every malformed header legacy happened to
    // accept through fgets/strcmp.
    QByteArray duplicate("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\nFORMAT=32-bit_rle_rgbe\n\n"
                         "-Y 1 +X 1\n");
    duplicate.append(QByteArray(4, '\0'));
    expect_rgbe_error(decoder.decode_memory(vector_bytes(duplicate), CancellationToken{}),
                      ErrorCode::kValidation, "duplicate_rgbe_format");

    QByteArray long_line("#?RADIANCE\n");
    long_line.append(QByteArray(128, 'X'));
    long_line.append("\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 1\n");
    long_line.append(QByteArray(4, '\0'));
    expect_rgbe_error(decoder.decode_memory(vector_bytes(long_line), CancellationToken{}),
                      ErrorCode::kValidation, "oversized_rgbe_header_line");

    QByteArray exact_line("#?RADIANCE\n");
    exact_line.append(QByteArray(127, 'X'));
    exact_line.append("\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 1\n");
    exact_line.append(QByteArray(4, '\0'));
    const auto exact_line_result =
        decoder.decode_memory(vector_bytes(exact_line), CancellationToken{});
    ASSERT_TRUE(exact_line_result) << exact_line_result.error().message;

    QByteArray long_header("#?RADIANCE\n");
    while (long_header.size() <= 1024 * 1024)
    {
        long_header.append("X\n");
    }
    long_header.append("FORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 1\n");
    expect_rgbe_error(decoder.decode_memory(vector_bytes(long_header), CancellationToken{}),
                      ErrorCode::kValidation, "oversized_rgbe_header");
}

TEST(RgbeAdapterTest, AppliesDimensionAndDecodedAllocationBoundsBeforePixels)
{
    RadianceRgbeDecoder decoder;
    const QByteArray oversized = rgbe_header("#?RADIANCE", 7000U, 7000U);
    const auto decoded = decoder.decode_memory(vector_bytes(oversized), CancellationToken{});
    expect_rgbe_error(decoded, ErrorCode::kValidation, "oversized_rgbe_decoded_raster");
    ASSERT_TRUE(decoded.error().context.contains("decoded_bytes_per_pixel"));
    EXPECT_EQ(decoded.error().context.at("decoded_bytes_per_pixel"), "12");

    const QByteArray zero = rgbe_header("#?RADIANCE", 0U, 1U);
    expect_rgbe_error(decoder.decode_memory(vector_bytes(zero), CancellationToken{}),
                      ErrorCode::kValidation, "invalid_rgbe_dimensions");

    RadianceRgbeDecodeLimits limits;
    EXPECT_EQ(limits.max_encoded_bytes, 1024ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(limits.max_decoded_bytes, 512ULL * 1024ULL * 1024ULL);
    limits.max_encoded_bytes = 64U;
    RadianceRgbeDecoder bounded(limits);
    QByteArray encoded = flat_rgbe(1U, 1U, {{{0U, 0U, 0U, 0U}}});
    encoded.append(QByteArray(65 - encoded.size(), 'X'));
    ASSERT_EQ(encoded.size(), 65);
    expect_rgbe_error(bounded.decode_memory(vector_bytes(encoded), CancellationToken{}),
                      ErrorCode::kValidation, "oversized_rgbe_input");
}

TEST(RgbeAdapterTest, PathAndMemoryMatchAndSourcesRemainImmutable)
{
    RgbeTempDirectory temporary;
    const auto path = temporary.path() / "mislabeled.bin";
    const QByteArray encoded = flat_rgbe(2U, 1U, {{{128U, 64U, 32U, 129U}, {0U, 0U, 0U, 0U}}},
                                         "#?RGBE", {"GAMMA=1.8", "EXPOSURE=2"});
    write_file(path, encoded);
    const QByteArray hash_before = QCryptographicHash::hash(encoded, QCryptographicHash::Sha256);
    const auto size_before = std::filesystem::file_size(path);
    const auto mtime_before = std::filesystem::last_write_time(path);
    auto memory = vector_bytes(encoded);

    RadianceRgbeDecoder decoder;
    const auto from_path = decoder.decode(path.string(), CancellationToken{});
    const auto from_memory = decoder.decode_memory(memory, CancellationToken{});
    ASSERT_TRUE(from_path) << from_path.error().message;
    ASSERT_TRUE(from_memory) << from_memory.error().message;
    EXPECT_EQ(from_path.value().width, from_memory.value().width);
    EXPECT_EQ(from_path.value().height, from_memory.value().height);
    EXPECT_EQ(from_path.value().rgb, from_memory.value().rgb);
    EXPECT_EQ(from_path.value().radiance, from_memory.value().radiance);
    EXPECT_EQ(memory, vector_bytes(encoded));
    EXPECT_EQ(QCryptographicHash::hash(read_file(path), QCryptographicHash::Sha256), hash_before);
    EXPECT_EQ(std::filesystem::file_size(path), size_before);
    EXPECT_EQ(std::filesystem::last_write_time(path), mtime_before);
}

TEST(RgbeAdapterTest, PrioritizesCancellationAndDistinguishesMissingPath)
{
    RgbeTempDirectory temporary;
    const auto path = temporary.path() / "cancel.hdr";
    const QByteArray encoded = flat_rgbe(1U, 1U, {{{1U, 2U, 3U, 128U}}});
    write_file(path, encoded);
    CancellationSource cancelled;
    ASSERT_TRUE(cancelled.cancel("rgbe test"));

    RadianceRgbeDecoder decoder;
    const auto from_path = decoder.decode(path.string(), cancelled.token());
    ASSERT_FALSE(from_path);
    EXPECT_EQ(from_path.error().code, ErrorCode::kCancelled);
    const auto from_memory = decoder.decode_memory(vector_bytes(encoded), cancelled.token());
    ASSERT_FALSE(from_memory);
    EXPECT_EQ(from_memory.error().code, ErrorCode::kCancelled);

    const auto missing_path = temporary.path() / "missing.hdr";
    const auto missing = decoder.decode(missing_path.string(), CancellationToken{});
    expect_rgbe_error(missing, ErrorCode::kNotFound, "rgbe_input_not_found", missing_path.string());
}

TEST(RgbeAdapterTest, ObservesCancellationInsideHeaderAndPixelLoops)
{
    QByteArray header_heavy("#?RADIANCE\n");
    for (int index = 0; index < 1000; ++index)
    {
        header_heavy.append("COMMENT=legacy migration cancellation checkpoint\n");
    }
    header_heavy.append("FORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 1\n");
    header_heavy.append(QByteArray(4, '\0'));
    CancellationSource header_cancelled;
    struct CancellationFixture
    {
        CancellationSource *source = nullptr;
        RadianceRgbeDecodeCheckpoint target = RadianceRgbeDecodeCheckpoint::kHeader;
        std::size_t threshold = 0U;
        std::size_t progress = 0U;
    };
    CancellationFixture header_fixture{&header_cancelled, RadianceRgbeDecodeCheckpoint::kHeader,
                                       256U, 0U};
    RadianceRgbeDecoder header_decoder(
        {}, {+[](void *const context, const RadianceRgbeDecodeCheckpoint checkpoint,
                 const std::size_t progress) noexcept
             {
                 auto &fixture = *static_cast<CancellationFixture *>(context);
                 if (checkpoint == fixture.target && progress >= fixture.threshold)
                 {
                     fixture.progress = progress;
                     (void)fixture.source->cancel("inside RGBE decode");
                 }
             },
             &header_fixture});
    const auto header_result =
        header_decoder.decode_memory(vector_bytes(header_heavy), header_cancelled.token());
    ASSERT_FALSE(header_result);
    EXPECT_EQ(header_result.error().code, ErrorCode::kCancelled);
    EXPECT_GE(header_fixture.progress, 256U);

    std::vector<RgbePixel> pixels(4096U, {128U, 64U, 32U, 129U});
    const QByteArray pixel_heavy = flat_rgbe(4096U, 1U, pixels);
    CancellationSource pixels_cancelled;
    CancellationFixture pixel_fixture{&pixels_cancelled, RadianceRgbeDecodeCheckpoint::kPixels,
                                      1024U, 0U};
    RadianceRgbeDecoder pixel_decoder(
        {}, {+[](void *const context, const RadianceRgbeDecodeCheckpoint checkpoint,
                 const std::size_t progress) noexcept
             {
                 auto &fixture = *static_cast<CancellationFixture *>(context);
                 if (checkpoint == fixture.target && progress >= fixture.threshold)
                 {
                     fixture.progress = progress;
                     (void)fixture.source->cancel("inside RGBE decode");
                 }
             },
             &pixel_fixture});
    const auto pixel_result =
        pixel_decoder.decode_memory(vector_bytes(pixel_heavy), pixels_cancelled.token());
    ASSERT_FALSE(pixel_result);
    EXPECT_EQ(pixel_result.error().code, ErrorCode::kCancelled);
    EXPECT_GE(pixel_fixture.progress, 1024U);
}

TEST(RgbeAdapterTest, DistinguishesNonRegularOpenAndReadFailures)
{
    RgbeTempDirectory temporary;
    RadianceRgbeDecoder decoder;
    expect_rgbe_error(decoder.decode(temporary.path().string(), CancellationToken{}),
                      ErrorCode::kIo, "rgbe_input_not_regular", temporary.path().string());

#ifndef _WIN32
    const auto unreadable = temporary.path() / "unreadable.hdr";
    write_file(unreadable, flat_rgbe(1U, 1U, {{{0U, 0U, 0U, 0U}}}));
    std::error_code permission_error;
    std::filesystem::permissions(unreadable, std::filesystem::perms::none,
                                 std::filesystem::perm_options::replace, permission_error);
    ASSERT_FALSE(permission_error);
    const auto open_failed = decoder.decode(unreadable.string(), CancellationToken{});
    std::filesystem::permissions(unreadable, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, permission_error);
    ASSERT_FALSE(permission_error);
    expect_rgbe_error(open_failed, ErrorCode::kIo, "rgbe_input_open_failed", unreadable.string());
#endif

    const auto truncated_during_read = temporary.path() / "read-failure.hdr";
    write_file(truncated_during_read, flat_rgbe(1U, 1U, {{{0U, 0U, 0U, 0U}}}));
    struct ReadFailureFixture
    {
        QString path;
        bool invoked = false;
        bool truncated = false;
    } read_fixture{QString::fromStdString(truncated_during_read.string())};
    RadianceRgbeDecoder interrupted_reader(
        {},
        {+[](void *const context, const RadianceRgbeDecodeCheckpoint checkpoint,
             const std::size_t) noexcept
         {
             auto &fixture = *static_cast<ReadFailureFixture *>(context);
             if (checkpoint == RadianceRgbeDecodeCheckpoint::kBeforeFileRead && !fixture.invoked)
             {
                 fixture.invoked = true;
                 QFile source(fixture.path);
                 fixture.truncated = source.open(QIODevice::WriteOnly | QIODevice::Truncate);
                 source.close();
             }
         },
         &read_fixture});
    expect_rgbe_error(
        interrupted_reader.decode(truncated_during_read.string(), CancellationToken{}),
        ErrorCode::kIo, "rgbe_input_read_failed", truncated_during_read.string());
    EXPECT_TRUE(read_fixture.invoked);
    EXPECT_TRUE(read_fixture.truncated);
}

TEST(RgbeAdapterTest, RejectsSparseOversizedPathWithoutReadingOrMutatingIt)
{
    RgbeTempDirectory temporary;
    const auto path = temporary.path() / "oversized.hdr";
    QFile file(QString::fromStdString(path.string()));
    ASSERT_TRUE(file.open(QIODevice::ReadWrite | QIODevice::Truncate));
    ASSERT_EQ(file.write("#?RADIANCE\n", 11), 11);
    constexpr qint64 kSparseSize = 1024LL * 1024LL * 1024LL + 1LL;
    ASSERT_TRUE(file.resize(kSparseSize));
    file.close();
    const auto size_before = std::filesystem::file_size(path);
    const auto mtime_before = std::filesystem::last_write_time(path);

    RadianceRgbeDecoder decoder;
    expect_rgbe_error(decoder.decode(path.string(), CancellationToken{}), ErrorCode::kValidation,
                      "oversized_rgbe_input", path.string());
    EXPECT_EQ(std::filesystem::file_size(path), size_before);
    EXPECT_EQ(std::filesystem::last_write_time(path), mtime_before);
    QFile verify(QString::fromStdString(path.string()));
    ASSERT_TRUE(verify.open(QIODevice::ReadOnly));
    EXPECT_EQ(verify.read(11), QByteArray("#?RADIANCE\n", 11));
}

TEST(RgbeAdapterTest, QtRasterBoundaryRecognizesMagicOnlyAndPrioritizesCancellation)
{
    RgbeTempDirectory temporary;
    QtRasterDecoder decoder;
    int file_index = 0;
    for (const QByteArray &payload :
         {QByteArray("#?RADIANCE\n", 11), QByteArray("#?RGBE\nmalformed", 16)})
    {
        expect_rgbe_error(decoder.decode_memory(vector_bytes(payload), 0U, CancellationToken{}),
                          ErrorCode::kUnsupported, "unsupported_rgbe_input");
        const auto path = temporary.path() / ("magic-" + std::to_string(file_index++) + ".bin");
        write_file(path, payload);
        expect_rgbe_error(decoder.probe(path.string()), ErrorCode::kUnsupported,
                          "unsupported_rgbe_input", path.string());
        expect_rgbe_error(decoder.decode(path.string(), 0U, CancellationToken{}),
                          ErrorCode::kUnsupported, "unsupported_rgbe_input", path.string());
        if (file_index == 1)
        {
            CancellationSource path_cancelled;
            ASSERT_TRUE(path_cancelled.cancel("qt RGBE path"));
            const auto cancelled_path = decoder.decode(path.string(), 0U, path_cancelled.token());
            ASSERT_FALSE(cancelled_path);
            EXPECT_EQ(cancelled_path.error().code, ErrorCode::kCancelled);
        }
    }
    const auto random =
        decoder.decode_memory(vector_bytes(QByteArray("#?RADIAN", 8)), 0U, CancellationToken{});
    ASSERT_FALSE(random);
    EXPECT_EQ(random.error().code, ErrorCode::kUnsupported);
    const auto format = random.error().context.find("format");
    EXPECT_TRUE(format == random.error().context.end() || format->second != "rgbe");

    CancellationSource cancelled;
    ASSERT_TRUE(cancelled.cancel("qt rgbe test"));
    const auto cancelled_result =
        decoder.decode_memory(vector_bytes(QByteArray("#?RGBE\n", 7)), 0U, cancelled.token());
    ASSERT_FALSE(cancelled_result);
    EXPECT_EQ(cancelled_result.error().code, ErrorCode::kCancelled);
}

} // namespace
} // namespace ravo

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
