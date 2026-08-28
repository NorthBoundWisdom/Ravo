#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <QBuffer>
#include <QByteArray>
#include <QColor>
#include <QColorSpace>
#include <QCryptographicHash>
#include <QFile>
#include <QImage>
#include <QIODevice>
#include <gtest/gtest.h>

#include "../adapters/src/jpeg_encoder.h"
#include "ravo/adapters/filesystem_preview_cache.h"
#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/domain/types.h"
#include "ravo/engine/engine.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/services/catalog_service.h"

namespace ravo
{
namespace
{

class JpegTempDirectory
{
public:
    JpegTempDirectory()
        : path_(std::filesystem::temp_directory_path() /
                ("ravo-jpeg-adapter-" + generate_catalog_id()))
    {
        std::filesystem::create_directories(path_);
    }

    ~JpegTempDirectory()
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

[[nodiscard]] QByteArray encode_asymmetric_jpeg()
{
    QImage image(80, 48, QImage::Format_RGB888);
    for (int y = 0; y < image.height(); ++y)
    {
        for (int x = 0; x < image.width(); ++x)
        {
            image.setPixelColor(
                x, y,
                QColor((x * 3 + y * 5) % 256, (x * 7 + y * 11) % 256, (x * 13 + y * 17) % 256));
        }
    }
    QByteArray encoded;
    QBuffer buffer(&encoded);
    EXPECT_TRUE(buffer.open(QIODevice::WriteOnly));
    EXPECT_TRUE(image.save(&buffer, "JPEG", 100));
    return encoded;
}

[[nodiscard]] QByteArray encode_grayscale_jpeg()
{
    QImage image(40, 24, QImage::Format_Grayscale8);
    for (int y = 0; y < image.height(); ++y)
    {
        auto *row = image.scanLine(y);
        for (int x = 0; x < image.width(); ++x)
        {
            row[x] = static_cast<std::uint8_t>((x * 5 + y * 9) % 256);
        }
    }
    QByteArray encoded;
    QBuffer buffer(&encoded);
    EXPECT_TRUE(buffer.open(QIODevice::WriteOnly));
    EXPECT_TRUE(image.save(&buffer, "JPEG", 100));
    return encoded;
}

void append_u16_be(QByteArray &bytes, const std::uint16_t value)
{
    bytes.append(static_cast<char>((value >> 8U) & 0xFFU));
    bytes.append(static_cast<char>(value & 0xFFU));
}

struct JpegSamplingFactors
{
    std::uint8_t y_horizontal = 0U;
    std::uint8_t y_vertical = 0U;
    std::uint8_t cb_horizontal = 0U;
    std::uint8_t cb_vertical = 0U;
    std::uint8_t cr_horizontal = 0U;
    std::uint8_t cr_vertical = 0U;
};

struct JpegHeaderSegment
{
    std::uint8_t id = 0U;
    std::size_t payload_offset = 0U;
    std::size_t payload_size = 0U;
};

struct JpegFrameContract
{
    std::uint8_t precision = 0U;
    std::uint16_t width = 0U;
    std::uint16_t height = 0U;
    JpegSamplingFactors sampling;
};

struct JpegDensity
{
    std::uint8_t unit = 0U;
    std::uint16_t horizontal = 0U;
    std::uint16_t vertical = 0U;
};

struct JpegIccSegment
{
    std::uint8_t sequence = 0U;
    std::uint8_t total = 0U;
    std::vector<std::uint8_t> bytes;
};

[[nodiscard]] std::optional<std::vector<JpegHeaderSegment>>
jpeg_header_segments(const std::vector<std::uint8_t> &bytes)
{
    if (bytes.size() < 4U || bytes[0] != 0xFFU || bytes[1] != 0xD8U)
    {
        return std::nullopt;
    }
    std::vector<JpegHeaderSegment> segments;
    std::size_t offset = 2U;
    while (offset < bytes.size())
    {
        if (bytes[offset] != 0xFFU)
        {
            return std::nullopt;
        }
        while (offset < bytes.size() && bytes[offset] == 0xFFU)
        {
            ++offset;
        }
        if (offset >= bytes.size())
        {
            return std::nullopt;
        }
        const std::uint8_t id = bytes[offset++];
        if (id == 0xD9U || id == 0xDAU)
        {
            return segments;
        }
        if (id == 0xD8U || (id >= 0xD0U && id <= 0xD7U))
        {
            continue;
        }
        if (bytes.size() - offset < 2U)
        {
            return std::nullopt;
        }
        const std::uint16_t length = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(bytes[offset]) << 8U) | bytes[offset + 1U]);
        if (length < 2U || length > bytes.size() - offset)
        {
            return std::nullopt;
        }
        segments.push_back({id, offset + 2U, static_cast<std::size_t>(length) - 2U});
        offset += length;
    }
    return std::nullopt;
}

[[nodiscard]] bool is_jpeg_frame_marker(const std::uint8_t id) noexcept
{
    return (id >= 0xC0U && id <= 0xC3U) || (id >= 0xC5U && id <= 0xC7U) ||
           (id >= 0xC9U && id <= 0xCBU) || (id >= 0xCDU && id <= 0xCFU);
}

[[nodiscard]] std::optional<JpegFrameContract>
jpeg_frame_contract(const std::vector<std::uint8_t> &bytes)
{
    const auto segments = jpeg_header_segments(bytes);
    if (!segments)
    {
        return std::nullopt;
    }
    for (const JpegHeaderSegment &segment : *segments)
    {
        if (!is_jpeg_frame_marker(segment.id))
        {
            continue;
        }
        if (segment.payload_size != 15U)
        {
            return std::nullopt;
        }
        const std::size_t offset = segment.payload_offset;
        if (bytes[offset + 5U] != 3U || bytes[offset + 6U] != 1U || bytes[offset + 9U] != 2U ||
            bytes[offset + 12U] != 3U)
        {
            return std::nullopt;
        }
        const auto factors = [&](const std::size_t index)
        {
            return std::pair{static_cast<std::uint8_t>(bytes[offset + index] >> 4U),
                             static_cast<std::uint8_t>(bytes[offset + index] & 0x0FU)};
        };
        const auto y = factors(7U);
        const auto cb = factors(10U);
        const auto cr = factors(13U);
        return JpegFrameContract{
            bytes[offset],
            static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[offset + 3U]) << 8U) |
                                       bytes[offset + 4U]),
            static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U) |
                                       bytes[offset + 2U]),
            {y.first, y.second, cb.first, cb.second, cr.first, cr.second}};
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<JpegSamplingFactors>
jpeg_sampling_factors(const std::vector<std::uint8_t> &bytes)
{
    const auto frame = jpeg_frame_contract(bytes);
    return frame ? std::optional(frame->sampling) : std::nullopt;
}

[[nodiscard]] std::optional<JpegDensity> jpeg_density(const std::vector<std::uint8_t> &bytes)
{
    static constexpr std::array<std::uint8_t, 5U> kJfif{'J', 'F', 'I', 'F', 0U};
    const auto segments = jpeg_header_segments(bytes);
    if (!segments)
    {
        return std::nullopt;
    }
    for (const JpegHeaderSegment &segment : *segments)
    {
        const auto payload_offset = static_cast<std::ptrdiff_t>(segment.payload_offset);
        if (segment.id != 0xE0U || segment.payload_size < 12U ||
            !std::equal(kJfif.begin(), kJfif.end(), bytes.begin() + payload_offset))
        {
            continue;
        }
        const std::size_t offset = segment.payload_offset;
        return JpegDensity{
            bytes[offset + 7U],
            static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[offset + 8U]) << 8U) |
                                       bytes[offset + 9U]),
            static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[offset + 10U]) << 8U) |
                                       bytes[offset + 11U])};
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::vector<JpegIccSegment>>
jpeg_icc_segments(const std::vector<std::uint8_t> &bytes)
{
    static constexpr std::array<std::uint8_t, 12U> kIccSignature{'I', 'C', 'C', '_', 'P', 'R',
                                                                 'O', 'F', 'I', 'L', 'E', 0U};
    const auto segments = jpeg_header_segments(bytes);
    if (!segments)
    {
        return std::nullopt;
    }
    std::vector<JpegIccSegment> result;
    for (const JpegHeaderSegment &segment : *segments)
    {
        const auto payload_offset = static_cast<std::ptrdiff_t>(segment.payload_offset);
        if (segment.id != 0xE2U || segment.payload_size < 14U ||
            !std::equal(kIccSignature.begin(), kIccSignature.end(), bytes.begin() + payload_offset))
        {
            continue;
        }
        const std::size_t offset = segment.payload_offset;
        result.push_back(
            {bytes[offset + 12U],
             bytes[offset + 13U],
             {bytes.begin() + static_cast<std::ptrdiff_t>(offset + 14U),
              bytes.begin() + static_cast<std::ptrdiff_t>(offset + segment.payload_size)}});
    }
    return result;
}

[[nodiscard]] std::optional<std::uint16_t>
jpeg_first_luminance_quantizer(const std::vector<std::uint8_t> &bytes)
{
    const auto segments = jpeg_header_segments(bytes);
    if (!segments)
    {
        return std::nullopt;
    }
    for (const JpegHeaderSegment &segment : *segments)
    {
        if (segment.id != 0xDBU)
        {
            continue;
        }
        std::size_t offset = segment.payload_offset;
        const std::size_t end = offset + segment.payload_size;
        while (offset < end)
        {
            const std::uint8_t table = bytes[offset++];
            const bool sixteen_bit = (table >> 4U) == 1U;
            const std::size_t table_bytes = sixteen_bit ? 128U : 64U;
            if ((table >> 4U) > 1U || end - offset < table_bytes)
            {
                return std::nullopt;
            }
            if ((table & 0x0FU) == 0U)
            {
                return sixteen_bit ? std::optional<std::uint16_t>(static_cast<std::uint16_t>(
                                         (static_cast<std::uint16_t>(bytes[offset]) << 8U) |
                                         bytes[offset + 1U])) :
                                     std::optional<std::uint16_t>(bytes[offset]);
            }
            offset += table_bytes;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::vector<std::uint8_t> jpeg_test_pixels(const std::uint32_t width,
                                                         const std::uint32_t height)
{
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 3U);
    for (std::size_t index = 0U; index < pixels.size(); ++index)
    {
        pixels[index] = static_cast<std::uint8_t>((index * 29U + index / 7U) & 0xFFU);
    }
    return pixels;
}

[[nodiscard]] std::vector<std::uint8_t> display_p3_icc()
{
    const QByteArray profile = QColorSpace(QColorSpace::DisplayP3).iccProfile();
    EXPECT_FALSE(profile.isEmpty());
    return {reinterpret_cast<const std::uint8_t *>(profile.constData()),
            reinterpret_cast<const std::uint8_t *>(profile.constData()) + profile.size()};
}

[[nodiscard]] constexpr JpegExportOptions jpeg_options(const int quality)
{
    return {quality, JpegSubsampling::kAuto};
}

template <typename T>
void expect_jpeg_encode_error(const Result<T> &result, const ErrorCode code,
                              const std::string_view reason)
{
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, code);
    EXPECT_EQ(result.error().context.at("format"), "jpeg");
    EXPECT_EQ(result.error().context.at("reason"), reason);
}

struct JpegScanlineCancellation
{
    CancellationSource source;
    bool reached = false;
};

void cancel_jpeg_at_scanline(void *const context, const detail::JpegEncodeCheckpoint checkpoint,
                             const std::uint32_t progress) noexcept
{
    auto *const cancellation = static_cast<JpegScanlineCancellation *>(context);
    if (checkpoint == detail::JpegEncodeCheckpoint::kScanline && progress == 4U)
    {
        cancellation->reached = true;
        (void)cancellation->source.cancel("jpeg-scanline-test");
    }
}

[[nodiscard]] QByteArray marker(const std::uint8_t id, const QByteArray &payload)
{
    EXPECT_LE(payload.size() + 2, 65535);
    QByteArray result;
    result.append(static_cast<char>(0xFF));
    result.append(static_cast<char>(id));
    append_u16_be(result, static_cast<std::uint16_t>(payload.size() + 2));
    result.append(payload);
    return result;
}

TEST(JpegExportContractTest, AutoSubsamplingMatchesFrozenLegacyQualityThresholds)
{
    struct Expectation
    {
        int quality = 0;
        std::uint8_t y_horizontal = 0U;
        std::uint8_t y_vertical = 0U;
    };
    constexpr std::array<Expectation, 4U> kExpectations{
        {{90, 2U, 2U}, {91, 2U, 1U}, {92, 2U, 1U}, {93, 1U, 1U}}};
    std::vector<std::uint8_t> pixels = jpeg_test_pixels(32U, 24U);
    const auto source = pixels;
    ColorProfileState srgb;
    srgb.kind = ColorProfileKind::kBuiltin;
    srgb.model = ColorModel::kRgb;
    srgb.identifier = "srgb";
    QtRasterDecoder decoder;

    for (const Expectation &expectation : kExpectations)
    {
        SCOPED_TRACE(expectation.quality);
        const auto encoded = decoder.encode(32U, 24U, pixels, srgb, ExportFormat::kJpeg,
                                            jpeg_options(expectation.quality), CancellationToken{});
        ASSERT_TRUE(encoded) << encoded.error().message;
        const auto sampling = jpeg_sampling_factors(encoded.value());
        ASSERT_TRUE(sampling);
        EXPECT_EQ(sampling->y_horizontal, expectation.y_horizontal);
        EXPECT_EQ(sampling->y_vertical, expectation.y_vertical);
        EXPECT_EQ(sampling->cb_horizontal, 1U);
        EXPECT_EQ(sampling->cb_vertical, 1U);
        EXPECT_EQ(sampling->cr_horizontal, 1U);
        EXPECT_EQ(sampling->cr_vertical, 1U);
    }
    EXPECT_EQ(pixels, source);
}

TEST(JpegExportContractTest, ExplicitSubsamplingOverridesOnlyTheFrozenSamplingFactors)
{
    struct Expectation
    {
        JpegSubsampling subsampling = JpegSubsampling::kAuto;
        std::uint8_t y_horizontal = 0U;
        std::uint8_t y_vertical = 0U;
    };
    constexpr std::array<Expectation, 5U> kExpectations{{
        {JpegSubsampling::kAuto, 2U, 2U},
        {JpegSubsampling::k444, 1U, 1U},
        {JpegSubsampling::k440, 1U, 2U},
        {JpegSubsampling::k422, 2U, 1U},
        {JpegSubsampling::k420, 2U, 2U},
    }};
    const auto pixels = jpeg_test_pixels(32U, 24U);
    ColorProfileState srgb;
    srgb.kind = ColorProfileKind::kBuiltin;
    srgb.model = ColorModel::kRgb;
    srgb.identifier = "srgb";
    QtRasterDecoder decoder;
    std::optional<std::uint16_t> automatic_quantizer;

    for (const Expectation &expectation : kExpectations)
    {
        SCOPED_TRACE(jpeg_subsampling_name(expectation.subsampling));
        const JpegExportOptions options{85, expectation.subsampling};
        const auto configuration = detail::jpeg_encode_configuration(options);
        ASSERT_TRUE(configuration) << configuration.error().message;
        EXPECT_EQ(configuration.value().quality, 85);
        EXPECT_EQ(configuration.value().smoothing_factor, 0);
        EXPECT_EQ(configuration.value().dct_method, detail::JpegDctMethod::kIntegerSlow);
        EXPECT_TRUE(configuration.value().optimize_coding);
        EXPECT_EQ(configuration.value().y_horizontal, expectation.y_horizontal);
        EXPECT_EQ(configuration.value().y_vertical, expectation.y_vertical);

        const auto encoded = decoder.encode(32U, 24U, pixels, srgb, ExportFormat::kJpeg, options,
                                            CancellationToken{});
        ASSERT_TRUE(encoded) << encoded.error().message;
        const auto sampling = jpeg_sampling_factors(encoded.value());
        ASSERT_TRUE(sampling);
        EXPECT_EQ(sampling->y_horizontal, expectation.y_horizontal);
        EXPECT_EQ(sampling->y_vertical, expectation.y_vertical);
        EXPECT_EQ(sampling->cb_horizontal, 1U);
        EXPECT_EQ(sampling->cb_vertical, 1U);
        EXPECT_EQ(sampling->cr_horizontal, 1U);
        EXPECT_EQ(sampling->cr_vertical, 1U);
        const auto quantizer = jpeg_first_luminance_quantizer(encoded.value());
        ASSERT_TRUE(quantizer);
        if (expectation.subsampling == JpegSubsampling::kAuto)
        {
            automatic_quantizer = *quantizer;
        }
        else
        {
            ASSERT_TRUE(automatic_quantizer);
            EXPECT_EQ(*quantizer, *automatic_quantizer);
        }
    }
}

TEST(JpegExportContractTest, QualityConfigurationMatchesFrozenLegacySource)
{
    struct Expectation
    {
        int quality = 0;
        int smoothing = 0;
        detail::JpegDctMethod dct = detail::JpegDctMethod::kIntegerSlow;
        std::uint8_t y_horizontal = 0U;
        std::uint8_t y_vertical = 0U;
    };
    constexpr std::array<Expectation, 13U> kExpectations{{
        {39, 60, detail::JpegDctMethod::kIntegerFast, 2U, 2U},
        {40, 40, detail::JpegDctMethod::kIntegerFast, 2U, 2U},
        {49, 40, detail::JpegDctMethod::kIntegerFast, 2U, 2U},
        {50, 40, detail::JpegDctMethod::kIntegerSlow, 2U, 2U},
        {59, 40, detail::JpegDctMethod::kIntegerSlow, 2U, 2U},
        {60, 20, detail::JpegDctMethod::kIntegerSlow, 2U, 2U},
        {79, 20, detail::JpegDctMethod::kIntegerSlow, 2U, 2U},
        {80, 0, detail::JpegDctMethod::kIntegerSlow, 2U, 2U},
        {90, 0, detail::JpegDctMethod::kIntegerSlow, 2U, 2U},
        {91, 0, detail::JpegDctMethod::kIntegerSlow, 2U, 1U},
        {92, 0, detail::JpegDctMethod::kIntegerSlow, 2U, 1U},
        {95, 0, detail::JpegDctMethod::kIntegerSlow, 1U, 1U},
        {96, 0, detail::JpegDctMethod::kFloat, 1U, 1U},
    }};
    for (const Expectation &expectation : kExpectations)
    {
        SCOPED_TRACE(expectation.quality);
        const auto configuration =
            detail::jpeg_encode_configuration(jpeg_options(expectation.quality));
        ASSERT_TRUE(configuration) << configuration.error().message;
        EXPECT_EQ(configuration.value().quality, expectation.quality);
        EXPECT_EQ(configuration.value().smoothing_factor, expectation.smoothing);
        EXPECT_EQ(configuration.value().dct_method, expectation.dct);
        EXPECT_TRUE(configuration.value().optimize_coding);
        EXPECT_EQ(configuration.value().y_horizontal, expectation.y_horizontal);
        EXPECT_EQ(configuration.value().y_vertical, expectation.y_vertical);
        EXPECT_EQ(configuration.value().cb_horizontal, 1U);
        EXPECT_EQ(configuration.value().cb_vertical, 1U);
        EXPECT_EQ(configuration.value().cr_horizontal, 1U);
        EXPECT_EQ(configuration.value().cr_vertical, 1U);
    }
    expect_jpeg_encode_error(detail::jpeg_encode_configuration(jpeg_options(0)),
                             ErrorCode::kValidation, "invalid_jpeg_quality");
    expect_jpeg_encode_error(detail::jpeg_encode_configuration(jpeg_options(101)),
                             ErrorCode::kValidation, "invalid_jpeg_quality");
}

TEST(JpegExportContractTest, QualityControlsBaselineQuantization)
{
    struct Expectation
    {
        int quality = 0;
        std::uint16_t first_luminance_quantizer = 0U;
    };
    constexpr std::array<Expectation, 6U> kExpectations{
        {{5, 160U}, {39, 20U}, {50, 16U}, {90, 3U}, {95, 2U}, {100, 1U}}};
    const auto pixels = jpeg_test_pixels(32U, 24U);
    ColorProfileState srgb;
    srgb.kind = ColorProfileKind::kBuiltin;
    srgb.model = ColorModel::kRgb;
    srgb.identifier = "srgb";
    QtRasterDecoder decoder;
    for (const Expectation &expectation : kExpectations)
    {
        SCOPED_TRACE(expectation.quality);
        const auto encoded = decoder.encode(32U, 24U, pixels, srgb, ExportFormat::kJpeg,
                                            jpeg_options(expectation.quality), CancellationToken{});
        ASSERT_TRUE(encoded) << encoded.error().message;
        const auto quantizer = jpeg_first_luminance_quantizer(encoded.value());
        ASSERT_TRUE(quantizer);
        EXPECT_EQ(*quantizer, expectation.first_luminance_quantizer);
    }
}

TEST(JpegExportContractTest, EmbedsResolvedRgbIccExactlyAndUsesFrozenJfifDensity)
{
    const auto pixels = jpeg_test_pixels(37U, 19U);
    const auto profile = display_p3_icc();
    const auto pixels_before = pixels;
    const auto profile_before = profile;
    ColorProfileState display_p3;
    display_p3.kind = ColorProfileKind::kIcc;
    display_p3.model = ColorModel::kRgb;
    display_p3.identifier = "display-p3-test";
    display_p3.icc_bytes = profile;
    QtRasterDecoder decoder;
    const auto encoded = decoder.encode(37U, 19U, pixels, display_p3, ExportFormat::kJpeg,
                                        jpeg_options(95), CancellationToken{});
    ASSERT_TRUE(encoded) << encoded.error().message;
    const auto frame = jpeg_frame_contract(encoded.value());
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->precision, 8U);
    EXPECT_EQ(frame->width, 37U);
    EXPECT_EQ(frame->height, 19U);
    const auto density = jpeg_density(encoded.value());
    ASSERT_TRUE(density);
    EXPECT_EQ(density->unit, 1U);
    EXPECT_EQ(density->horizontal, 300U);
    EXPECT_EQ(density->vertical, 300U);
    const auto segments = jpeg_icc_segments(encoded.value());
    ASSERT_TRUE(segments);
    ASSERT_EQ(segments->size(), 1U);
    EXPECT_EQ(segments->front().sequence, 1U);
    EXPECT_EQ(segments->front().total, 1U);
    EXPECT_EQ(segments->front().bytes, profile);
    EXPECT_EQ(pixels, pixels_before);
    EXPECT_EQ(profile, profile_before);
}

TEST(JpegExportContractTest, SplitsResolvedIccAtTheFrozenAppTwoPayloadBoundary)
{
    const auto pixels = jpeg_test_pixels(8U, 8U);
    std::vector<std::uint8_t> profile(detail::kJpegIccSegmentBytes + 257U);
    for (std::size_t index = 0U; index < profile.size(); ++index)
    {
        profile[index] = static_cast<std::uint8_t>((index * 17U + 3U) & 0xFFU);
    }
    const auto profile_before = profile;
    const auto encoded =
        detail::encode_jpeg_rgb8(8U, 8U, pixels, profile, jpeg_options(90), CancellationToken{});
    ASSERT_TRUE(encoded) << encoded.error().message;
    const auto segments = jpeg_icc_segments(encoded.value());
    ASSERT_TRUE(segments);
    ASSERT_EQ(segments->size(), 2U);
    EXPECT_EQ((*segments)[0].sequence, 1U);
    EXPECT_EQ((*segments)[0].total, 2U);
    EXPECT_EQ((*segments)[0].bytes.size(), detail::kJpegIccSegmentBytes);
    EXPECT_EQ((*segments)[1].sequence, 2U);
    EXPECT_EQ((*segments)[1].total, 2U);
    EXPECT_EQ((*segments)[1].bytes.size(), 257U);
    std::vector<std::uint8_t> reassembled = (*segments)[0].bytes;
    reassembled.insert(reassembled.end(), (*segments)[1].bytes.begin(), (*segments)[1].bytes.end());
    EXPECT_EQ(reassembled, profile);
    EXPECT_EQ(profile, profile_before);
}

TEST(JpegExportContractTest, RejectsInvalidAndOversizedInputsWithoutPublishingBytes)
{
    const auto pixels = jpeg_test_pixels(16U, 8U);
    const auto profile = display_p3_icc();
    expect_jpeg_encode_error(
        detail::encode_jpeg_rgb8(0U, 8U, pixels, profile, jpeg_options(90), CancellationToken{}),
        ErrorCode::kValidation, "invalid_jpeg_dimensions");
    expect_jpeg_encode_error(detail::encode_jpeg_rgb8(detail::kJpegMaxDimension + 1U, 1U, pixels,
                                                      profile, jpeg_options(90),
                                                      CancellationToken{}),
                             ErrorCode::kValidation, "invalid_jpeg_dimensions");
    expect_jpeg_encode_error(detail::encode_jpeg_rgb8(65535U, 1U, pixels, profile, jpeg_options(90),
                                                      CancellationToken{}),
                             ErrorCode::kValidation, "invalid_jpeg_dimensions");
    expect_jpeg_encode_error(
        detail::encode_jpeg_rgb8(16U, 8U, std::span<const std::uint8_t>(pixels).first(10U), profile,
                                 jpeg_options(90), CancellationToken{}),
        ErrorCode::kValidation, "jpeg_source_size_mismatch");
    expect_jpeg_encode_error(detail::encode_jpeg_rgb8(14000U, 14000U, {}, profile, jpeg_options(90),
                                                      CancellationToken{}),
                             ErrorCode::kValidation, "jpeg_source_too_large");
    expect_jpeg_encode_error(
        detail::encode_jpeg_rgb8(16U, 8U, pixels, {}, jpeg_options(90), CancellationToken{}),
        ErrorCode::kValidation, "missing_jpeg_output_icc");
    std::vector<std::uint8_t> oversized_profile(detail::kJpegMaxIccBytes + 1U);
    expect_jpeg_encode_error(detail::encode_jpeg_rgb8(16U, 8U, pixels, oversized_profile,
                                                      jpeg_options(90), CancellationToken{}),
                             ErrorCode::kValidation, "oversized_jpeg_output_icc");

    detail::JpegEncodeControl tiny_output;
    tiny_output.max_output_bytes = 32U;
    expect_jpeg_encode_error(detail::encode_jpeg_rgb8(16U, 8U, pixels, profile, jpeg_options(90),
                                                      CancellationToken{}, tiny_output),
                             ErrorCode::kValidation, "jpeg_output_too_large");

    // legacy dimension() advertised 65535, but its jpeg_start_compress()
    // owner enforced JPEG_MAX_DIMENSION=65500.
    for (const auto [width, height] : std::array<std::pair<std::uint32_t, std::uint32_t>, 2U>{
             {{detail::kJpegMaxDimension, 1U}, {1U, detail::kJpegMaxDimension}}})
    {
        SCOPED_TRACE(std::to_string(width) + "x" + std::to_string(height));
        const auto boundary_pixels = jpeg_test_pixels(width, height);
        const auto encoded = detail::encode_jpeg_rgb8(width, height, boundary_pixels, profile,
                                                      jpeg_options(90), CancellationToken{});
        ASSERT_TRUE(encoded) << encoded.error().message;
        const auto frame = jpeg_frame_contract(encoded.value());
        ASSERT_TRUE(frame);
        EXPECT_EQ(frame->width, width);
        EXPECT_EQ(frame->height, height);
    }
}

TEST(JpegExportContractTest, HonorsEntryAndScanlineCancellationWithoutMutatingInputs)
{
    const auto pixels = jpeg_test_pixels(128U, 64U);
    const auto profile = display_p3_icc();
    const auto pixels_before = pixels;
    const auto profile_before = profile;
    CancellationSource entry;
    ASSERT_TRUE(entry.cancel("jpeg-entry-test"));
    const auto entry_result =
        detail::encode_jpeg_rgb8(128U, 64U, pixels, profile, jpeg_options(95), entry.token());
    ASSERT_FALSE(entry_result);
    EXPECT_EQ(entry_result.error().code, ErrorCode::kCancelled);

    JpegScanlineCancellation scanline;
    detail::JpegEncodeControl control;
    control.checkpoint_observer = {&scanline, cancel_jpeg_at_scanline};
    const auto scanline_result = detail::encode_jpeg_rgb8(
        128U, 64U, pixels, profile, jpeg_options(95), scanline.source.token(), control);
    ASSERT_FALSE(scanline_result);
    EXPECT_EQ(scanline_result.error().code, ErrorCode::kCancelled);
    EXPECT_TRUE(scanline.reached);
    EXPECT_EQ(pixels, pixels_before);
    EXPECT_EQ(profile, profile_before);
}

[[nodiscard]] QByteArray inject_after_soi(const QByteArray &jpeg,
                                          const std::vector<QByteArray> &markers)
{
    EXPECT_GE(jpeg.size(), 2);
    QByteArray result = jpeg.first(2);
    for (const auto &item : markers)
    {
        result.append(item);
    }
    result.append(jpeg.sliced(2));
    return result;
}

[[nodiscard]] QByteArray exif_orientation_marker(const std::uint16_t orientation)
{
    QByteArray payload("Exif\0\0", 6);
    payload.append("II", 2);
    payload.append("\x2a\x00", 2);
    payload.append("\x08\x00\x00\x00", 4);
    payload.append("\x01\x00", 2);
    payload.append("\x12\x01", 2);
    payload.append("\x03\x00", 2);
    payload.append("\x01\x00\x00\x00", 4);
    payload.append(static_cast<char>(orientation & 0xFFU));
    payload.append(static_cast<char>((orientation >> 8U) & 0xFFU));
    payload.append("\x00\x00", 2);
    payload.append("\x00\x00\x00\x00", 4);
    return marker(0xE1U, payload);
}

[[nodiscard]] QByteArray icc_marker(const QByteArray &profile_part, const std::uint8_t sequence,
                                    const std::uint8_t count)
{
    QByteArray payload("ICC_PROFILE\0", 12);
    payload.append(static_cast<char>(sequence));
    payload.append(static_cast<char>(count));
    payload.append(profile_part);
    return marker(0xE2U, payload);
}

[[nodiscard]] std::vector<std::uint8_t> vector_bytes(const QByteArray &bytes)
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

void write_file(const std::filesystem::path &path, const QByteArray &bytes)
{
    std::ofstream stream(path, std::ios::binary);
    ASSERT_TRUE(stream.is_open());
    stream.write(bytes.constData(), bytes.size());
    ASSERT_TRUE(stream.good());
}

[[nodiscard]] std::array<std::uint8_t, 3> pixel(const DecodedRaster &raster, const std::uint32_t x,
                                                const std::uint32_t y)
{
    const auto offset = (static_cast<std::size_t>(y) * raster.width + x) * 3U;
    return {raster.rgb[offset], raster.rgb[offset + 1U], raster.rgb[offset + 2U]};
}

[[nodiscard]] std::pair<std::uint32_t, std::uint32_t>
oriented_source_coordinate(const std::uint16_t orientation, const std::uint32_t x,
                           const std::uint32_t y, const std::uint32_t width,
                           const std::uint32_t height)
{
    switch (orientation)
    {
    case 1:
        return {x, y};
    case 2:
        return {width - 1U - x, y};
    case 3:
        return {width - 1U - x, height - 1U - y};
    case 4:
        return {x, height - 1U - y};
    case 5:
        return {y, x};
    case 6:
        return {y, height - 1U - x};
    case 7:
        return {width - 1U - y, height - 1U - x};
    case 8:
        return {width - 1U - y, x};
    default:
        return {x, y};
    }
}

void expect_jpeg_error(const Result<DecodedRaster> &result, const ErrorCode code,
                       const std::string_view reason)
{
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, code);
    const auto format = result.error().context.find("format");
    ASSERT_NE(format, result.error().context.end());
    EXPECT_EQ(format->second, "jpeg");
    const auto actual_reason = result.error().context.find("reason");
    ASSERT_NE(actual_reason, result.error().context.end());
    EXPECT_EQ(actual_reason->second, reason);
}

[[nodiscard]] QByteArray four_component_frame(QByteArray jpeg)
{
    for (qsizetype index = 2; index + 10 < jpeg.size(); ++index)
    {
        const auto marker_id = static_cast<std::uint8_t>(jpeg[index + 1]);
        if (static_cast<std::uint8_t>(jpeg[index]) != 0xFFU ||
            !((marker_id >= 0xC0U && marker_id <= 0xC3U) ||
              (marker_id >= 0xC5U && marker_id <= 0xC7U) ||
              (marker_id >= 0xC9U && marker_id <= 0xCBU) ||
              (marker_id >= 0xCDU && marker_id <= 0xCFU)))
        {
            continue;
        }
        const auto old_length =
            static_cast<std::uint16_t>((static_cast<std::uint8_t>(jpeg[index + 2]) << 8U) |
                                       static_cast<std::uint8_t>(jpeg[index + 3]));
        EXPECT_EQ(old_length, 17U);
        jpeg[index + 2] = 0;
        jpeg[index + 3] = 20;
        jpeg[index + 9] = 4;
        jpeg.insert(index + 2 + old_length, QByteArray("\x04\x11\x00", 3));
        return jpeg;
    }
    ADD_FAILURE() << "JPEG fixture has no supported frame marker";
    return jpeg;
}

TEST(JpegAdapterTest, AppliesAllExifOrientationsAndPreservesExactPixelMapping)
{
    const QByteArray base = encode_asymmetric_jpeg();
    QtRasterDecoder decoder;
    const auto baseline = decoder.decode_memory(vector_bytes(base), 0, CancellationToken{});
    ASSERT_TRUE(baseline) << baseline.error().message;
    ASSERT_EQ(baseline.value().width, 80U);
    ASSERT_EQ(baseline.value().height, 48U);

    for (std::uint16_t orientation = 1; orientation <= 8; ++orientation)
    {
        SCOPED_TRACE(orientation);
        const QByteArray oriented = inject_after_soi(base, {exif_orientation_marker(orientation)});
        const auto decoded = decoder.decode_memory(vector_bytes(oriented), 0, CancellationToken{});
        ASSERT_TRUE(decoded) << decoded.error().message;
        const bool transposed = orientation >= 5U;
        EXPECT_EQ(decoded.value().width,
                  transposed ? baseline.value().height : baseline.value().width);
        EXPECT_EQ(decoded.value().height,
                  transposed ? baseline.value().width : baseline.value().height);
        for (std::uint32_t y = 0; y < decoded.value().height; ++y)
        {
            for (std::uint32_t x = 0; x < decoded.value().width; ++x)
            {
                const auto source = oriented_source_coordinate(
                    orientation, x, y, baseline.value().width, baseline.value().height);
                ASSERT_EQ(pixel(decoded.value(), x, y),
                          pixel(baseline.value(), source.first, source.second));
            }
        }
    }
}

TEST(JpegAdapterTest, ScalesExifSixAndEightInEncodedCoordinates)
{
    const QByteArray base = encode_asymmetric_jpeg();
    QtRasterDecoder decoder;
    for (const std::uint16_t orientation : std::array<std::uint16_t, 2>{6U, 8U})
    {
        SCOPED_TRACE(orientation);
        const QByteArray oriented = inject_after_soi(base, {exif_orientation_marker(orientation)});
        const auto decoded = decoder.decode_memory(vector_bytes(oriented), 20, CancellationToken{});
        ASSERT_TRUE(decoded) << decoded.error().message;
        EXPECT_EQ(decoded.value().width, 12U);
        EXPECT_EQ(decoded.value().height, 20U);
        EXPECT_EQ(decoded.value().source_width, 48U);
        EXPECT_EQ(decoded.value().source_height, 80U);
        EXPECT_EQ(decoded.value().rgb.size(), 12U * 20U * 3U);
    }
}

TEST(JpegAdapterTest, FileAndMemoryRecognitionMatchAndNeverMutateTheSource)
{
    const QByteArray encoded =
        inject_after_soi(encode_asymmetric_jpeg(), {exif_orientation_marker(6)});
    const QByteArray original_hash = QCryptographicHash::hash(encoded, QCryptographicHash::Sha256);
    auto memory_bytes = vector_bytes(encoded);
    const auto memory_before = memory_bytes;
    JpegTempDirectory temporary;
    const auto path = temporary.path() / "jpeg-content-with-png-extension.png";
    write_file(path, encoded);

    QtRasterDecoder decoder;
    const auto probed = decoder.probe(path.string());
    ASSERT_TRUE(probed) << probed.error().message;
    EXPECT_EQ(probed.value().media_type, kMediaTypeJpeg);
    EXPECT_EQ(probed.value().width, 48U);
    EXPECT_EQ(probed.value().height, 80U);
    const auto from_file = decoder.decode(path.string(), 20, CancellationToken{});
    const auto from_memory = decoder.decode_memory(memory_bytes, 20, CancellationToken{});
    ASSERT_TRUE(from_file) << from_file.error().message;
    ASSERT_TRUE(from_memory) << from_memory.error().message;
    EXPECT_EQ(from_file.value().width, from_memory.value().width);
    EXPECT_EQ(from_file.value().height, from_memory.value().height);
    EXPECT_EQ(from_file.value().rgb, from_memory.value().rgb);
    EXPECT_EQ(from_file.value().color_profile, from_memory.value().color_profile);
    EXPECT_EQ(memory_bytes, memory_before);
    EXPECT_EQ(QCryptographicHash::hash(read_file(path), QCryptographicHash::Sha256), original_hash);
}

TEST(JpegAdapterTest, OwnsValidSingleAndOutOfOrderMultipartRgbIccProfiles)
{
    const QByteArray base = encode_asymmetric_jpeg();
    const QByteArray profile = QColorSpace(QColorSpace::DisplayP3).iccProfile();
    ASSERT_FALSE(profile.isEmpty());
    QtRasterDecoder decoder;

    const QByteArray single = inject_after_soi(base, {icc_marker(profile, 1, 1)});
    const auto single_decoded = decoder.decode_memory(vector_bytes(single), 0, CancellationToken{});
    ASSERT_TRUE(single_decoded) << single_decoded.error().message;
    EXPECT_EQ(single_decoded.value().color_profile.kind, ColorProfileKind::kIcc);
    EXPECT_EQ(single_decoded.value().color_profile.model, ColorModel::kRgb);
    EXPECT_EQ(single_decoded.value().color_profile.identifier, "embedded_icc");
    EXPECT_EQ(single_decoded.value().color_profile.icc_bytes, vector_bytes(profile));

    const qsizetype first_end = profile.size() / 3;
    const qsizetype second_end = (profile.size() * 2) / 3;
    const QByteArray multipart =
        inject_after_soi(base, {icc_marker(profile.sliced(first_end, second_end - first_end), 2, 3),
                                icc_marker(profile.first(first_end), 1, 3),
                                icc_marker(profile.sliced(second_end), 3, 3)});
    const auto multipart_decoded =
        decoder.decode_memory(vector_bytes(multipart), 0, CancellationToken{});
    ASSERT_TRUE(multipart_decoded) << multipart_decoded.error().message;
    EXPECT_EQ(multipart_decoded.value().color_profile.icc_bytes, vector_bytes(profile));
}

TEST(JpegAdapterTest, RejectsIncompleteDuplicateInconsistentAndCorruptIccProfiles)
{
    const QByteArray base = encode_asymmetric_jpeg();
    const QByteArray profile = QColorSpace(QColorSpace::SRgb).iccProfile();
    ASSERT_FALSE(profile.isEmpty());
    const QByteArray first = profile.first(profile.size() / 2);
    const QByteArray second = profile.sliced(profile.size() / 2);
    QtRasterDecoder decoder;

    expect_jpeg_error(
        decoder.decode_memory(vector_bytes(inject_after_soi(
                                  base, {icc_marker(first, 1, 3), icc_marker(second, 3, 3)})),
                              0, CancellationToken{}),
        ErrorCode::kValidation, "missing_jpeg_icc_segment");
    expect_jpeg_error(
        decoder.decode_memory(vector_bytes(inject_after_soi(
                                  base, {icc_marker(first, 1, 2), icc_marker(second, 1, 2)})),
                              0, CancellationToken{}),
        ErrorCode::kValidation, "duplicate_jpeg_icc_segment");
    expect_jpeg_error(
        decoder.decode_memory(vector_bytes(inject_after_soi(
                                  base, {icc_marker(first, 1, 2), icc_marker(second, 2, 3)})),
                              0, CancellationToken{}),
        ErrorCode::kValidation, "inconsistent_jpeg_icc_segment_count");
    expect_jpeg_error(decoder.decode_memory(vector_bytes(inject_after_soi(
                                                base, {icc_marker(QByteArray(128, '\0'), 1, 1)})),
                                            0, CancellationToken{}),
                      ErrorCode::kValidation, "corrupt_jpeg_icc_profile");

    const QColorSpace gray_profile(QPointF(0.3127, 0.3290), QColorSpace::TransferFunction::Gamma,
                                   2.2F);
    ASSERT_TRUE(gray_profile.isValid());
    ASSERT_EQ(gray_profile.colorModel(), QColorSpace::ColorModel::Gray);
    ASSERT_FALSE(gray_profile.iccProfile().isEmpty());
    expect_jpeg_error(
        decoder.decode_memory(
            vector_bytes(inject_after_soi(base, {icc_marker(gray_profile.iccProfile(), 1, 1)})), 0,
            CancellationToken{}),
        ErrorCode::kUnsupported, "unsupported_jpeg_icc_color_model");

    QByteArray malformed("ICC_PROFILEX", 12);
    malformed.append("\x01\x01", 2);
    malformed.append(profile);
    expect_jpeg_error(
        decoder.decode_memory(vector_bytes(inject_after_soi(base, {marker(0xE2U, malformed)})), 0,
                              CancellationToken{}),
        ErrorCode::kValidation, "malformed_jpeg_icc_header");
}

TEST(JpegAdapterTest, DefinesRgbGrayCmykAndOpaqueOutputDecisions)
{
    QtRasterDecoder decoder;
    const auto rgb =
        decoder.decode_memory(vector_bytes(encode_asymmetric_jpeg()), 0, CancellationToken{});
    ASSERT_TRUE(rgb) << rgb.error().message;
    EXPECT_EQ(rgb.value().pixel_format, RasterPixelFormat::kRgb8);
    EXPECT_EQ(rgb.value().alpha_mode, RasterAlphaMode::kOpaque);
    EXPECT_EQ(rgb.value().rgb.size(), rgb.value().width * rgb.value().height * 3U);
    EXPECT_EQ(rgb.value().color_profile.kind, ColorProfileKind::kMissing);

    const auto gray =
        decoder.decode_memory(vector_bytes(encode_grayscale_jpeg()), 0, CancellationToken{});
    ASSERT_TRUE(gray) << gray.error().message;
    EXPECT_EQ(gray.value().pixel_format, RasterPixelFormat::kRgb8);
    EXPECT_EQ(gray.value().alpha_mode, RasterAlphaMode::kOpaque);
    EXPECT_EQ(gray.value().rgb.size(), gray.value().width * gray.value().height * 3U);
    EXPECT_EQ(gray.value().color_profile.kind, ColorProfileKind::kMissing);

    expect_jpeg_error(
        decoder.decode_memory(vector_bytes(four_component_frame(encode_asymmetric_jpeg())), 0,
                              CancellationToken{}),
        ErrorCode::kUnsupported, "unsupported_jpeg_components");
}

TEST(JpegAdapterTest, SeparatesRecognizedCorruptionUnsupportedContentAndPathErrors)
{
    QtRasterDecoder decoder;
    const std::vector<std::uint8_t> soi{0xFFU, 0xD8U};
    expect_jpeg_error(decoder.decode_memory(soi, 0, CancellationToken{}), ErrorCode::kValidation,
                      "incomplete_jpeg_stream");
    const std::vector<std::uint8_t> bad_third_byte{0xFFU, 0xD8U, 0x41U};
    expect_jpeg_error(decoder.decode_memory(bad_third_byte, 0, CancellationToken{}),
                      ErrorCode::kValidation, "jpeg_marker_sync_lost");

    QByteArray truncated = encode_asymmetric_jpeg();
    truncated.chop(2);
    expect_jpeg_error(decoder.decode_memory(vector_bytes(truncated), 0, CancellationToken{}),
                      ErrorCode::kValidation, "incomplete_jpeg_stream");

    const auto unsupported = decoder.decode_memory(
        std::vector<std::uint8_t>{'n', 'o', 't', '-', 'j', 'p', 'e', 'g'}, 0, CancellationToken{});
    ASSERT_FALSE(unsupported);
    EXPECT_EQ(unsupported.error().code, ErrorCode::kUnsupported);

    JpegTempDirectory temporary;
    const auto random_path = temporary.path() / "random.jpg";
    write_file(random_path, QByteArray("not-jpeg"));
    const auto random_file = decoder.decode(random_path.string(), 0, CancellationToken{});
    ASSERT_FALSE(random_file);
    EXPECT_EQ(random_file.error().code, ErrorCode::kUnsupported);
    const auto missing =
        decoder.decode((temporary.path() / "missing.jpg").string(), 0, CancellationToken{});
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::kNotFound);
    const auto directory = decoder.decode(temporary.path().string(), 0, CancellationToken{});
    ASSERT_FALSE(directory);
    EXPECT_EQ(directory.error().code, ErrorCode::kInvalidArgument);
    const auto empty_path = decoder.decode("", 0, CancellationToken{});
    ASSERT_FALSE(empty_path);
    EXPECT_EQ(empty_path.error().code, ErrorCode::kInvalidArgument);
}

TEST(JpegAdapterTest, CancellationLeavesMemoryAndFileSourcesUnchanged)
{
    const QByteArray encoded = encode_asymmetric_jpeg();
    auto source_bytes = vector_bytes(encoded);
    const auto original_bytes = source_bytes;
    JpegTempDirectory temporary;
    const auto path = temporary.path() / "cancelled.jpg";
    write_file(path, encoded);
    const QByteArray original_hash =
        QCryptographicHash::hash(read_file(path), QCryptographicHash::Sha256);
    CancellationSource cancellation;
    ASSERT_TRUE(cancellation.cancel("jpeg-test"));

    QtRasterDecoder decoder;
    const auto memory = decoder.decode_memory(source_bytes, 0, cancellation.token());
    ASSERT_FALSE(memory);
    EXPECT_EQ(memory.error().code, ErrorCode::kCancelled);
    const auto file = decoder.decode(path.string(), 0, cancellation.token());
    ASSERT_FALSE(file);
    EXPECT_EQ(file.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(source_bytes, original_bytes);
    EXPECT_EQ(QCryptographicHash::hash(read_file(path), QCryptographicHash::Sha256), original_hash);
}

TEST(JpegCatalogTest, ForwardsTypedOptionsAndIgnoresThemForOtherFormats)
{
    JpegTempDirectory temporary;
    const auto input_path = temporary.path() / "source.jpg";
    QImage source(32, 24, QImage::Format_RGB888);
    source.fill(QColor(40, 120, 200));
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    ASSERT_TRUE(source.save(QString::fromStdString(input_path.string()), "JPEG", 100));
    const QByteArray original_hash =
        QCryptographicHash::hash(read_file(input_path), QCryptographicHash::Sha256);
    const auto database_path = (temporary.path() / "library.sqlite").string();

    auto engine_result = EngineFacade::create_phase1();
    ASSERT_TRUE(engine_result) << engine_result.error().message;
    auto engine = std::move(engine_result).value();
    auto repository = SqliteCatalogRepository::create(database_path);
    ASSERT_TRUE(repository) << repository.error().message;
    auto cache = FilesystemPreviewCache::create(database_path + ".preview");
    ASSERT_TRUE(cache) << cache.error().message;
    CatalogService service(engine, std::move(repository).value(),
                           std::make_unique<QtRasterDecoder>(), std::move(cache).value());

    const auto imported = service.import_one(input_path.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);

    ExportRequest default_request;
    default_request.asset_id = imported.value().asset->id;
    default_request.output_path = (temporary.path() / "default.jpg").string();
    default_request.format = ExportFormat::kJpeg;
    const auto default_export = service.export_asset(default_request);
    ASSERT_TRUE(default_export) << default_export.error().message;
    const auto default_bytes = vector_bytes(read_file(default_request.output_path));
    const auto default_sampling = jpeg_sampling_factors(default_bytes);
    ASSERT_TRUE(default_sampling);
    EXPECT_EQ(default_sampling->y_horizontal, 1U);
    EXPECT_EQ(default_sampling->y_vertical, 1U);
    const auto default_quantizer = jpeg_first_luminance_quantizer(default_bytes);
    ASSERT_TRUE(default_quantizer);
    EXPECT_EQ(*default_quantizer, 2U);

    struct Expectation
    {
        JpegSubsampling subsampling = JpegSubsampling::kAuto;
        std::uint8_t y_horizontal = 0U;
        std::uint8_t y_vertical = 0U;
    };
    constexpr std::array<Expectation, 5U> kExpectations{{
        {JpegSubsampling::kAuto, 2U, 2U},
        {JpegSubsampling::k444, 1U, 1U},
        {JpegSubsampling::k440, 1U, 2U},
        {JpegSubsampling::k422, 2U, 1U},
        {JpegSubsampling::k420, 2U, 2U},
    }};
    std::size_t index = 0U;
    for (const Expectation &expectation : kExpectations)
    {
        SCOPED_TRACE(jpeg_subsampling_name(expectation.subsampling));
        ExportRequest request;
        request.asset_id = imported.value().asset->id;
        request.output_path =
            (temporary.path() / ("output-" + std::to_string(index++) + ".jpg")).string();
        request.format = ExportFormat::kJpeg;
        request.jpeg_options = {85, expectation.subsampling};
        const auto exported = service.export_asset(request);
        ASSERT_TRUE(exported) << exported.error().message;
        const auto sampling = jpeg_sampling_factors(vector_bytes(read_file(request.output_path)));
        ASSERT_TRUE(sampling);
        EXPECT_EQ(sampling->y_horizontal, expectation.y_horizontal);
        EXPECT_EQ(sampling->y_vertical, expectation.y_vertical);
    }

    ExportRequest invalid_jpeg;
    invalid_jpeg.asset_id = imported.value().asset->id;
    invalid_jpeg.output_path = (temporary.path() / "invalid.jpg").string();
    invalid_jpeg.format = ExportFormat::kJpeg;
    invalid_jpeg.jpeg_options.quality = 4;
    const auto invalid = service.export_asset(invalid_jpeg);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, ErrorCode::kValidation);
    EXPECT_EQ(invalid.error().context.at("reason"), "invalid_jpeg_quality");
    EXPECT_FALSE(std::filesystem::exists(invalid_jpeg.output_path));

    ExportRequest png;
    png.asset_id = imported.value().asset->id;
    png.output_path = (temporary.path() / "unrelated.png").string();
    png.format = ExportFormat::kPng;
    png.jpeg_options = {4, static_cast<JpegSubsampling>(255U)};
    const auto exported_png = service.export_asset(png);
    ASSERT_TRUE(exported_png) << exported_png.error().message;
    EXPECT_TRUE(std::filesystem::exists(png.output_path));
    EXPECT_EQ(QCryptographicHash::hash(read_file(input_path), QCryptographicHash::Sha256),
              original_hash);
    EXPECT_TRUE(service.close());
}

TEST(JpegCatalogTest, CorruptJpegNeverPublishesAnAssetOrPreview)
{
    JpegTempDirectory temporary;
    QByteArray truncated = encode_asymmetric_jpeg();
    truncated.chop(2);
    const auto input_path = temporary.path() / "truncated.jpg";
    write_file(input_path, truncated);
    const QByteArray original_hash =
        QCryptographicHash::hash(read_file(input_path), QCryptographicHash::Sha256);
    const auto database_path = (temporary.path() / "library.sqlite").string();

    auto engine_result = EngineFacade::create_phase1();
    ASSERT_TRUE(engine_result) << engine_result.error().message;
    auto engine = std::move(engine_result).value();
    auto repository = SqliteCatalogRepository::create(database_path);
    ASSERT_TRUE(repository) << repository.error().message;
    auto cache = FilesystemPreviewCache::create(database_path + ".preview");
    ASSERT_TRUE(cache) << cache.error().message;
    CatalogService service(engine, std::move(repository).value(),
                           std::make_unique<QtRasterDecoder>(), std::move(cache).value());

    const auto imported = service.import_one(input_path.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    EXPECT_EQ(imported.value().status, ImportItemStatus::kFailed);
    EXPECT_FALSE(imported.value().asset);
    EXPECT_FALSE(imported.value().preview_cache_path);
    ASSERT_TRUE(imported.value().error);
    EXPECT_EQ(imported.value().error->code, ErrorCode::kValidation);
    EXPECT_EQ(imported.value().error->context.at("format"), "jpeg");
    EXPECT_EQ(imported.value().error->context.at("reason"), "incomplete_jpeg_stream");
    const auto assets = service.list_assets();
    ASSERT_TRUE(assets) << assets.error().message;
    EXPECT_TRUE(assets.value().empty());
    const auto previews = service.list_previews();
    ASSERT_TRUE(previews) << previews.error().message;
    EXPECT_TRUE(previews.value().empty());
    EXPECT_EQ(QCryptographicHash::hash(read_file(input_path), QCryptographicHash::Sha256),
              original_hash);
    EXPECT_TRUE(service.close());
}

TEST(JpegExportContractTest, EmbedsExifXmpAndIptcMarkersBeforeIcc)
{
    const auto pixels = jpeg_test_pixels(16U, 12U);
    const auto profile = display_p3_icc();
    ASSERT_FALSE(profile.empty());
    ExportMetadataSnapshot metadata;
    metadata.writable.title = "Title";
    metadata.writable.description = "Desc";
    metadata.writable.creator = "Alice";
    metadata.writable.copyright = "©";
    metadata.capture.camera_make = "RavoCam";
    metadata.capture.iso = 200.0;
    metadata.capture.aperture = 2.8;
    metadata.tags = {"alpha", "zeta"};
    const auto first = detail::encode_jpeg_rgb8(16U, 12U, pixels, profile, jpeg_options(95),
                                                metadata, false, CancellationToken{});
    ASSERT_TRUE(first) << first.error().message;
    const auto second = detail::encode_jpeg_rgb8(16U, 12U, pixels, profile, jpeg_options(95),
                                                 metadata, false, CancellationToken{});
    ASSERT_TRUE(second);
    EXPECT_EQ(first.value(), second.value());
    const auto segments = jpeg_header_segments(first.value());
    ASSERT_TRUE(segments);
    std::vector<std::uint8_t> ids;
    std::string exif;
    std::string xmp;
    bool saw_iptc = false;
    bool saw_icc = false;
    for (const auto &segment : *segments)
    {
        if (segment.id == 0xE0U)
        {
            continue;
        }
        ids.push_back(segment.id);
        const auto payload =
            std::string(first.value().begin() + static_cast<std::ptrdiff_t>(segment.payload_offset),
                        first.value().begin() + static_cast<std::ptrdiff_t>(segment.payload_offset +
                                                                            segment.payload_size));
        if (segment.id == 0xE1U && payload.rfind(std::string("Exif", 4), 0) == 0)
        {
            exif = payload;
        }
        if (segment.id == 0xE1U && payload.find("http://ns.adobe.com/xap/1.0/") == 0)
        {
            xmp = payload;
        }
        if (segment.id == 0xEDU)
        {
            saw_iptc = payload.find("Photoshop 3.0") == 0;
        }
        if (segment.id == 0xE2U)
        {
            saw_icc = payload.find("ICC_PROFILE") == 0;
        }
    }
    ASSERT_GE(ids.size(), 4U);
    EXPECT_EQ(ids[0], 0xE1U);
    EXPECT_EQ(ids[1], 0xE1U);
    EXPECT_EQ(ids[2], 0xE2U);
    EXPECT_EQ(ids[3], 0xEDU);
    EXPECT_EQ(exif.find(std::string("Exif", 4)), 0U);
    EXPECT_NE(xmp.find("<xmp:CreatorTool>Ravo</xmp:CreatorTool>"), std::string::npos);
    EXPECT_NE(xmp.find("<exif:FNumber>14/5</exif:FNumber>"), std::string::npos);
    EXPECT_EQ(xmp.find("DateTimeOriginal"), std::string::npos);
    EXPECT_TRUE(saw_iptc);
    EXPECT_TRUE(saw_icc);
    const auto frame = jpeg_frame_contract(first.value());
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->width, 16U);
    EXPECT_EQ(frame->height, 12U);
}

TEST(JpegExportContractTest, PresentEmptyTitleStillEmitsPhotoshopIptc)
{
    const auto pixels = jpeg_test_pixels(8U, 8U);
    const auto profile = display_p3_icc();
    ASSERT_FALSE(profile.empty());
    ExportMetadataSnapshot metadata;
    metadata.writable.title = "";
    const auto encoded = detail::encode_jpeg_rgb8(8U, 8U, pixels, profile, jpeg_options(90),
                                                  metadata, false, CancellationToken{});
    ASSERT_TRUE(encoded) << encoded.error().message;
    const auto absent =
        detail::encode_jpeg_rgb8(8U, 8U, pixels, profile, jpeg_options(90),
                                 ExportMetadataSnapshot{}, false, CancellationToken{});
    ASSERT_TRUE(absent) << absent.error().message;
    const auto segments = jpeg_header_segments(encoded.value());
    ASSERT_TRUE(segments);
    bool saw_iptc = false;
    for (const auto &segment : *segments)
    {
        if (segment.id != 0xEDU)
        {
            continue;
        }
        const auto payload = std::string(
            encoded.value().begin() + static_cast<std::ptrdiff_t>(segment.payload_offset),
            encoded.value().begin() +
                static_cast<std::ptrdiff_t>(segment.payload_offset + segment.payload_size));
        saw_iptc = payload.find("Photoshop 3.0") == 0;
    }
    EXPECT_TRUE(saw_iptc);
    const auto absent_segments = jpeg_header_segments(absent.value());
    ASSERT_TRUE(absent_segments);
    bool absent_iptc = false;
    for (const auto &segment : *absent_segments)
    {
        if (segment.id == 0xEDU)
        {
            absent_iptc = true;
        }
    }
    EXPECT_FALSE(absent_iptc);
}

} // namespace
} // namespace ravo
