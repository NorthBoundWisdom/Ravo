#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <zlib.h>

#include <QByteArray>
#include <QColorSpace>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QIODevice>
#include <gtest/gtest.h>

#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/domain/types.h"
#include "ravo/foundation/cancellation.h"

namespace ravo
{
namespace
{

inline constexpr std::array<std::uint8_t, 8> kSignature{0x89U, 'P',   'N',   'G',
                                                        0x0DU, 0x0AU, 0x1AU, 0x0AU};

class PngTempDirectory
{
public:
    PngTempDirectory()
        : path_(std::filesystem::temp_directory_path() /
                ("ravo-png-adapter-" + generate_catalog_id()))
    {
        std::filesystem::create_directories(path_);
    }

    ~PngTempDirectory()
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

void append_u16_le(QByteArray &bytes, const std::uint16_t value)
{
    bytes.append(static_cast<char>(value & 0xFFU));
    bytes.append(static_cast<char>((value >> 8U) & 0xFFU));
}

void append_u16_be(QByteArray &bytes, const std::uint16_t value)
{
    bytes.append(static_cast<char>((value >> 8U) & 0xFFU));
    bytes.append(static_cast<char>(value & 0xFFU));
}

void append_u32_be(QByteArray &bytes, const std::uint32_t value)
{
    bytes.append(static_cast<char>((value >> 24U) & 0xFFU));
    bytes.append(static_cast<char>((value >> 16U) & 0xFFU));
    bytes.append(static_cast<char>((value >> 8U) & 0xFFU));
    bytes.append(static_cast<char>(value & 0xFFU));
}

void append_u32_le(QByteArray &bytes, const std::uint32_t value)
{
    bytes.append(static_cast<char>(value & 0xFFU));
    bytes.append(static_cast<char>((value >> 8U) & 0xFFU));
    bytes.append(static_cast<char>((value >> 16U) & 0xFFU));
    bytes.append(static_cast<char>((value >> 24U) & 0xFFU));
}

[[nodiscard]] QByteArray compressed(const QByteArray &input)
{
    uLongf size = compressBound(static_cast<uLong>(input.size()));
    QByteArray output(static_cast<qsizetype>(size), Qt::Uninitialized);
    const int status = compress2(reinterpret_cast<Bytef *>(output.data()), &size,
                                 reinterpret_cast<const Bytef *>(input.constData()),
                                 static_cast<uLong>(input.size()), Z_BEST_SPEED);
    EXPECT_EQ(status, Z_OK);
    output.resize(static_cast<qsizetype>(size));
    return output;
}

void append_chunk(QByteArray &png, const QByteArray &type, const QByteArray &payload)
{
    ASSERT_EQ(type.size(), 4);
    ASSERT_LE(payload.size(), static_cast<qsizetype>(std::numeric_limits<std::uint32_t>::max()));
    append_u32_be(png, static_cast<std::uint32_t>(payload.size()));
    png.append(type);
    png.append(payload);
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, reinterpret_cast<const Bytef *>(type.constData()), 4U);
    crc = crc32(crc, reinterpret_cast<const Bytef *>(payload.constData()),
                static_cast<uInt>(payload.size()));
    append_u32_be(png, static_cast<std::uint32_t>(crc));
}

using Chunk = std::pair<QByteArray, QByteArray>;

[[nodiscard]] QByteArray png_with_idat(const std::uint32_t width, const std::uint32_t height,
                                       const std::uint8_t bit_depth, const std::uint8_t color_type,
                                       const QByteArray &idat,
                                       const std::vector<Chunk> &before_idat = {},
                                       const std::vector<Chunk> &after_idat = {},
                                       const std::uint8_t interlace = 0U)
{
    QByteArray png(reinterpret_cast<const char *>(kSignature.data()),
                   static_cast<qsizetype>(kSignature.size()));
    QByteArray ihdr;
    append_u32_be(ihdr, width);
    append_u32_be(ihdr, height);
    ihdr.append(static_cast<char>(bit_depth));
    ihdr.append(static_cast<char>(color_type));
    ihdr.append('\0');
    ihdr.append('\0');
    ihdr.append(static_cast<char>(interlace));
    append_chunk(png, QByteArrayLiteral("IHDR"), ihdr);
    for (const auto &[type, payload] : before_idat)
    {
        append_chunk(png, type, payload);
    }
    append_chunk(png, QByteArrayLiteral("IDAT"), idat);
    for (const auto &[type, payload] : after_idat)
    {
        append_chunk(png, type, payload);
    }
    append_chunk(png, QByteArrayLiteral("IEND"), {});
    return png;
}

[[nodiscard]] QByteArray make_png(const std::uint32_t width, const std::uint32_t height,
                                  const std::uint8_t bit_depth, const std::uint8_t color_type,
                                  const QByteArray &scanlines,
                                  const std::vector<Chunk> &before_idat = {},
                                  const std::vector<Chunk> &after_idat = {})
{
    return png_with_idat(width, height, bit_depth, color_type, compressed(scanlines), before_idat,
                         after_idat);
}

[[nodiscard]] QByteArray rgb8_scanlines(const std::uint32_t width, const std::uint32_t height)
{
    QByteArray result;
    result.reserve(static_cast<qsizetype>(height) * (1 + static_cast<qsizetype>(width) * 3));
    for (std::uint32_t y = 0; y < height; ++y)
    {
        result.append('\0');
        for (std::uint32_t x = 0; x < width; ++x)
        {
            result.append(static_cast<char>((x * 37U + y * 17U + 11U) & 0xFFU));
            result.append(static_cast<char>((x * 13U + y * 61U + 23U) & 0xFFU));
            result.append(static_cast<char>((x * 71U + y * 29U + 31U) & 0xFFU));
        }
    }
    return result;
}

[[nodiscard]] QByteArray rgb8_png(const std::uint32_t width = 3U, const std::uint32_t height = 2U,
                                  const std::vector<Chunk> &chunks = {})
{
    return make_png(width, height, 8U, 2U, rgb8_scanlines(width, height), chunks);
}

[[nodiscard]] QByteArray iccp_chunk_payload(const QByteArray &profile)
{
    QByteArray result("Ravo", 4);
    result.append('\0');
    result.append('\0');
    result.append(compressed(profile));
    return result;
}

[[nodiscard]] QByteArray cicp_chunk_payload(const std::uint8_t primaries,
                                            const std::uint8_t transfer,
                                            const std::uint8_t matrix = 0U,
                                            const std::uint8_t full_range = 1U)
{
    QByteArray result;
    result.append(static_cast<char>(primaries));
    result.append(static_cast<char>(transfer));
    result.append(static_cast<char>(matrix));
    result.append(static_cast<char>(full_range));
    return result;
}

[[nodiscard]] QByteArray exif_orientation_payload(const std::uint16_t orientation,
                                                  const bool little_endian = true)
{
    QByteArray payload(little_endian ? "II" : "MM", 2);
    const auto append_u16 = little_endian ? append_u16_le : append_u16_be;
    const auto append_u32 = little_endian ? append_u32_le : append_u32_be;
    append_u16(payload, 42U);
    append_u32(payload, 8U);
    append_u16(payload, 1U);
    append_u16(payload, 0x0112U);
    append_u16(payload, 3U);
    append_u32(payload, 1U);
    append_u16(payload, orientation);
    append_u16(payload, 0U);
    append_u32(payload, 0U);
    return payload;
}

[[nodiscard]] std::vector<std::uint8_t> vector_bytes(const QByteArray &bytes)
{
    return {reinterpret_cast<const std::uint8_t *>(bytes.constData()),
            reinterpret_cast<const std::uint8_t *>(bytes.constData()) + bytes.size()};
}

[[nodiscard]] QByteArray hash(const std::vector<std::uint8_t> &bytes)
{
    return QCryptographicHash::hash(QByteArrayView(reinterpret_cast<const char *>(bytes.data()),
                                                   static_cast<qsizetype>(bytes.size())),
                                    QCryptographicHash::Sha256);
}

void write_file(const std::filesystem::path &path, const QByteArray &bytes)
{
    QFile file(QString::fromStdString(path.string()));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(file.write(bytes), bytes.size());
    ASSERT_TRUE(file.flush());
}

[[nodiscard]] QByteArray read_file(const std::filesystem::path &path)
{
    QFile file(QString::fromStdString(path.string()));
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    return file.readAll();
}

[[nodiscard]] std::array<std::uint8_t, 3> pixel(const DecodedRaster &raster, const std::uint32_t x,
                                                const std::uint32_t y)
{
    const std::size_t offset = (static_cast<std::size_t>(y) * raster.width + x) * 3U;
    return {raster.rgb[offset], raster.rgb[offset + 1U], raster.rgb[offset + 2U]};
}

[[nodiscard]] std::array<std::uint8_t, 3> source_pixel(const std::uint32_t x, const std::uint32_t y)
{
    return {static_cast<std::uint8_t>((x * 37U + y * 17U + 11U) & 0xFFU),
            static_cast<std::uint8_t>((x * 13U + y * 61U + 23U) & 0xFFU),
            static_cast<std::uint8_t>((x * 71U + y * 29U + 31U) & 0xFFU)};
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

template <typename T>
void expect_png_error(const Result<T> &result, const ErrorCode code, const std::string_view reason,
                      const std::string_view source)
{
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, code);
    ASSERT_TRUE(result.error().context.contains("format"));
    EXPECT_EQ(result.error().context.at("format"), "png");
    ASSERT_TRUE(result.error().context.contains("reason"));
    EXPECT_EQ(result.error().context.at("reason"), reason);
    ASSERT_TRUE(result.error().context.contains("source"));
    EXPECT_EQ(result.error().context.at("source"), source);
}

TEST(PngAdapterTest, DecodesRgbGrayPaletteAndTransparencyAsOpaqueRgb8)
{
    QtRasterDecoder decoder;

    const auto rgb = decoder.decode_memory(vector_bytes(rgb8_png()), 0U, CancellationToken{});
    ASSERT_TRUE(rgb) << rgb.error().message;
    EXPECT_EQ(rgb.value().width, 3U);
    EXPECT_EQ(rgb.value().height, 2U);
    EXPECT_EQ(rgb.value().pixel_format, RasterPixelFormat::kRgb8);
    EXPECT_EQ(rgb.value().alpha_mode, RasterAlphaMode::kOpaque);
    EXPECT_EQ(pixel(rgb.value(), 2U, 1U), source_pixel(2U, 1U));

    QByteArray gray_scanlines("\0\x11\x80\xff", 4);
    const auto gray = decoder.decode_memory(vector_bytes(make_png(3U, 1U, 8U, 0U, gray_scanlines)),
                                            0U, CancellationToken{});
    ASSERT_TRUE(gray) << gray.error().message;
    EXPECT_EQ(pixel(gray.value(), 0U, 0U), (std::array<std::uint8_t, 3>{17U, 17U, 17U}));
    EXPECT_EQ(pixel(gray.value(), 1U, 0U), (std::array<std::uint8_t, 3>{128U, 128U, 128U}));
    EXPECT_EQ(pixel(gray.value(), 2U, 0U), (std::array<std::uint8_t, 3>{255U, 255U, 255U}));

    QByteArray palette("\x0a\x14\x1e\xc8\x96\x64", 6);
    QByteArray palette_alpha("\0\xff", 2);
    QByteArray palette_scanlines("\0\x01", 2);
    const auto indexed =
        decoder.decode_memory(vector_bytes(make_png(1U, 1U, 8U, 3U, palette_scanlines,
                                                    {{QByteArrayLiteral("PLTE"), palette},
                                                     {QByteArrayLiteral("tRNS"), palette_alpha}})),
                              0U, CancellationToken{});
    ASSERT_TRUE(indexed) << indexed.error().message;
    EXPECT_EQ(indexed.value().alpha_mode, RasterAlphaMode::kOpaque);
    EXPECT_EQ(pixel(indexed.value(), 0U, 0U), (std::array<std::uint8_t, 3>{200U, 150U, 100U}));

    QByteArray rgba_scanlines("\0\x0b\x16\x21\0\x2c\x37\x42\x80", 9);
    const auto rgba = decoder.decode_memory(vector_bytes(make_png(2U, 1U, 8U, 6U, rgba_scanlines)),
                                            0U, CancellationToken{});
    ASSERT_TRUE(rgba) << rgba.error().message;
    EXPECT_EQ(rgba.value().alpha_mode, RasterAlphaMode::kOpaque);
    EXPECT_EQ(pixel(rgba.value(), 0U, 0U), (std::array<std::uint8_t, 3>{11U, 22U, 33U}));
    EXPECT_EQ(pixel(rgba.value(), 1U, 0U), (std::array<std::uint8_t, 3>{44U, 55U, 66U}));

    const auto gray_alpha = decoder.decode_memory(
        vector_bytes(make_png(1U, 1U, 8U, 4U, QByteArray("\0\x4d\0", 3))), 0U, CancellationToken{});
    ASSERT_TRUE(gray_alpha) << gray_alpha.error().message;
    EXPECT_EQ(gray_alpha.value().alpha_mode, RasterAlphaMode::kOpaque);
    EXPECT_EQ(pixel(gray_alpha.value(), 0U, 0U), (std::array<std::uint8_t, 3>{77U, 77U, 77U}));

    const auto ancillary = decoder.decode_memory(
        vector_bytes(rgb8_png(3U, 2U, {{QByteArrayLiteral("raVo"), QByteArrayLiteral("ignored")}})),
        0U, CancellationToken{});
    ASSERT_TRUE(ancillary) << ancillary.error().message;
    EXPECT_EQ(ancillary.value().rgb, rgb.value().rgb);
}

TEST(PngAdapterTest, QuantizesSixteenBitRgbAndRgbaWithPinnedHighBytePolicy)
{
    QtRasterDecoder decoder;
    QByteArray rgb16("\0\x00\x00\x00\x80\x80\x80\xff\xff"
                     "\x01\x00\x7f\xff\xff\x00",
                     13);
    const auto rgb = decoder.decode_memory(vector_bytes(make_png(2U, 1U, 16U, 2U, rgb16)), 0U,
                                           CancellationToken{});
    ASSERT_TRUE(rgb) << rgb.error().message;
    EXPECT_EQ(pixel(rgb.value(), 0U, 0U), (std::array<std::uint8_t, 3>{0U, 0U, 128U}));
    EXPECT_EQ(pixel(rgb.value(), 1U, 0U), (std::array<std::uint8_t, 3>{255U, 1U, 127U}));

    QByteArray rgba16("\0\x12\x34\x56\x78\x9a\xbc\0\0", 9);
    const auto rgba = decoder.decode_memory(vector_bytes(make_png(1U, 1U, 16U, 6U, rgba16)), 0U,
                                            CancellationToken{});
    ASSERT_TRUE(rgba) << rgba.error().message;
    EXPECT_EQ(rgba.value().alpha_mode, RasterAlphaMode::kOpaque);
    EXPECT_EQ(pixel(rgba.value(), 0U, 0U), (std::array<std::uint8_t, 3>{18U, 86U, 154U}));

    const auto gray =
        decoder.decode_memory(vector_bytes(make_png(1U, 1U, 16U, 0U, QByteArray("\0\x80\xff", 3))),
                              0U, CancellationToken{});
    ASSERT_TRUE(gray) << gray.error().message;
    EXPECT_EQ(pixel(gray.value(), 0U, 0U), (std::array<std::uint8_t, 3>{128U, 128U, 128U}));
}

TEST(PngAdapterTest, OwnsExactIccAndDistinguishesSrgbAndMissingProfiles)
{
    QtRasterDecoder decoder;
    const QByteArray display_p3 = QColorSpace(QColorSpace::DisplayP3).iccProfile();
    ASSERT_FALSE(display_p3.isEmpty());
    ASSERT_GT(display_p3.size(), 256);

    const auto icc = decoder.decode_memory(
        vector_bytes(
            rgb8_png(3U, 2U, {{QByteArrayLiteral("iCCP"), iccp_chunk_payload(display_p3)}})),
        0U, CancellationToken{});
    ASSERT_TRUE(icc) << icc.error().message;
    EXPECT_EQ(icc.value().color_profile.kind, ColorProfileKind::kIcc);
    EXPECT_EQ(icc.value().color_profile.model, ColorModel::kRgb);
    EXPECT_EQ(icc.value().color_profile.identifier, "embedded_icc");
    EXPECT_EQ(icc.value().color_profile.icc_bytes, vector_bytes(display_p3));

    const auto srgb = decoder.decode_memory(
        vector_bytes(rgb8_png(3U, 2U, {{QByteArrayLiteral("sRGB"), QByteArray(1, '\0')}})), 0U,
        CancellationToken{});
    ASSERT_TRUE(srgb) << srgb.error().message;
    EXPECT_EQ(srgb.value().color_profile.kind, ColorProfileKind::kBuiltin);
    EXPECT_EQ(srgb.value().color_profile.identifier, "srgb");
    EXPECT_TRUE(srgb.value().color_profile.icc_bytes.empty());

    const auto missing = decoder.decode_memory(vector_bytes(rgb8_png()), 0U, CancellationToken{});
    ASSERT_TRUE(missing) << missing.error().message;
    EXPECT_EQ(missing.value().color_profile.kind, ColorProfileKind::kMissing);
    EXPECT_TRUE(missing.value().color_profile.identifier.empty());
    EXPECT_TRUE(missing.value().color_profile.icc_bytes.empty());
}

TEST(PngAdapterTest, MapsOnlyFrozenFullRangeRgbCicpProfiles)
{
    struct CicpCase
    {
        std::uint8_t primaries;
        std::uint8_t transfer;
        std::string_view identifier;
    };
    static constexpr std::array<CicpCase, 9> cases{{
        {1U, 13U, "srgb"},
        {1U, 1U, "rec709"},
        {1U, 8U, "linear_rec709"},
        {9U, 8U, "linear_rec2020"},
        {9U, 16U, "pq_rec2020"},
        {9U, 18U, "hlg_rec2020"},
        {12U, 13U, "display_p3"},
        {12U, 16U, "pq_p3"},
        {12U, 18U, "hlg_p3"},
    }};
    QtRasterDecoder decoder;
    for (const auto &item : cases)
    {
        const auto decoded = decoder.decode_memory(
            vector_bytes(rgb8_png(
                3U, 2U,
                {{QByteArrayLiteral("cICP"), cicp_chunk_payload(item.primaries, item.transfer)}})),
            0U, CancellationToken{});
        ASSERT_TRUE(decoded) << decoded.error().message;
        EXPECT_EQ(decoded.value().color_profile.kind, ColorProfileKind::kBuiltin);
        EXPECT_EQ(decoded.value().color_profile.identifier, item.identifier);
        EXPECT_TRUE(decoded.value().color_profile.icc_bytes.empty());
    }

    QByteArray gamma("\0\0\xb1\x8f", 4);
    const auto redundant_gamma = decoder.decode_memory(
        vector_bytes(rgb8_png(3U, 2U,
                              {{QByteArrayLiteral("gAMA"), gamma},
                               {QByteArrayLiteral("cICP"), cicp_chunk_payload(1U, 13U)}})),
        0U, CancellationToken{});
    ASSERT_TRUE(redundant_gamma) << redundant_gamma.error().message;
    EXPECT_EQ(redundant_gamma.value().color_profile.identifier, "srgb");

    const QByteArray display_p3 = QColorSpace(QColorSpace::DisplayP3).iccProfile();
    for (const std::vector<Chunk> &chunks :
         {std::vector<Chunk>{{QByteArrayLiteral("iCCP"), iccp_chunk_payload(display_p3)},
                             {QByteArrayLiteral("cICP"), cicp_chunk_payload(12U, 13U)}},
          std::vector<Chunk>{{QByteArrayLiteral("cICP"), cicp_chunk_payload(12U, 13U)},
                             {QByteArrayLiteral("iCCP"), iccp_chunk_payload(display_p3)}}})
    {
        const auto redundant_icc =
            decoder.decode_memory(vector_bytes(rgb8_png(3U, 2U, chunks)), 0U, CancellationToken{});
        ASSERT_TRUE(redundant_icc) << redundant_icc.error().message;
        EXPECT_EQ(redundant_icc.value().color_profile.kind, ColorProfileKind::kIcc);
        EXPECT_EQ(redundant_icc.value().color_profile.identifier, "embedded_icc");
        EXPECT_EQ(redundant_icc.value().color_profile.icc_bytes, vector_bytes(display_p3));
    }
}

TEST(PngAdapterTest, DecodesFrozenCicpAndIccFixtureEquallyFromPathAndMemory)
{
    const std::filesystem::path path =
        std::filesystem::path(RAVO_REPOSITORY_ROOT) / "legacy/tests/0000-nop/expected.png";
    const QByteArray encoded = read_file(path);
    ASSERT_FALSE(encoded.isEmpty());
    std::vector<std::uint8_t> memory = vector_bytes(encoded);
    const QByteArray before = hash(memory);
    const QByteArray file_before = QCryptographicHash::hash(encoded, QCryptographicHash::Sha256);
    QtRasterDecoder decoder;

    const auto probed = decoder.probe(path.string());
    ASSERT_TRUE(probed) << probed.error().message;
    EXPECT_EQ(probed.value().media_type, kMediaTypePng);
    EXPECT_EQ(probed.value().width, 2048U);
    EXPECT_EQ(probed.value().height, 1363U);
    const auto from_path = decoder.decode(path.string(), 128U, CancellationToken{});
    const auto from_memory = decoder.decode_memory(memory, 128U, CancellationToken{});
    ASSERT_TRUE(from_path) << from_path.error().message;
    ASSERT_TRUE(from_memory) << from_memory.error().message;
    EXPECT_EQ(from_path.value().rgb, from_memory.value().rgb);
    EXPECT_EQ(from_path.value().color_profile.kind, ColorProfileKind::kIcc);
    EXPECT_EQ(from_path.value().color_profile.identifier, "embedded_icc");
    EXPECT_FALSE(from_path.value().color_profile.icc_bytes.empty());
    EXPECT_EQ(from_path.value().color_profile, from_memory.value().color_profile);
    EXPECT_EQ(hash(memory), before);
    EXPECT_EQ(QCryptographicHash::hash(read_file(path), QCryptographicHash::Sha256), file_before);
}

TEST(PngAdapterTest, RejectsMalformedDuplicateConflictingAndCorruptProfiles)
{
    QtRasterDecoder decoder;
    const QByteArray display_p3 = QColorSpace(QColorSpace::DisplayP3).iccProfile();
    const QByteArray iccp = iccp_chunk_payload(display_p3);

    const auto duplicate = decoder.decode_memory(
        vector_bytes(rgb8_png(
            3U, 2U, {{QByteArrayLiteral("iCCP"), iccp}, {QByteArrayLiteral("iCCP"), iccp}})),
        0U, CancellationToken{});
    expect_png_error(duplicate, ErrorCode::kValidation, "invalid_png_iccp_order", "memory");

    const auto conflict = decoder.decode_memory(
        vector_bytes(rgb8_png(
            3U, 2U,
            {{QByteArrayLiteral("sRGB"), QByteArray(1, '\0')}, {QByteArrayLiteral("iCCP"), iccp}})),
        0U, CancellationToken{});
    expect_png_error(conflict, ErrorCode::kValidation, "invalid_png_iccp_order", "memory");

    QByteArray malformed("Ravo", 4);
    malformed.append('\0');
    malformed.append('\x01');
    malformed.append("bad", 3);
    const auto bad_method = decoder.decode_memory(
        vector_bytes(rgb8_png(3U, 2U, {{QByteArrayLiteral("iCCP"), malformed}})), 0U,
        CancellationToken{});
    expect_png_error(bad_method, ErrorCode::kValidation, "malformed_png_iccp_header", "memory");

    QByteArray corrupt("Ravo\0\0not-zlib", 14);
    const auto bad_stream = decoder.decode_memory(
        vector_bytes(rgb8_png(3U, 2U, {{QByteArrayLiteral("iCCP"), corrupt}})), 0U,
        CancellationToken{});
    expect_png_error(bad_stream, ErrorCode::kValidation, "corrupt_png_iccp_profile", "memory");

    const auto bad_profile = decoder.decode_memory(
        vector_bytes(rgb8_png(
            3U, 2U,
            {{QByteArrayLiteral("iCCP"), iccp_chunk_payload(QByteArrayLiteral("not-icc"))}})),
        0U, CancellationToken{});
    expect_png_error(bad_profile, ErrorCode::kValidation, "corrupt_png_iccp_profile", "memory");

    const QByteArray oversized_profile(16 * 1024 * 1024 + 1, 'x');
    const auto oversized = decoder.decode_memory(
        vector_bytes(
            rgb8_png(3U, 2U, {{QByteArrayLiteral("iCCP"), iccp_chunk_payload(oversized_profile)}})),
        0U, CancellationToken{});
    expect_png_error(oversized, ErrorCode::kValidation, "oversized_png_iccp_profile", "memory");

    const auto narrow_cicp = decoder.decode_memory(
        vector_bytes(
            rgb8_png(3U, 2U, {{QByteArrayLiteral("cICP"), cicp_chunk_payload(1U, 13U, 1U, 0U)}})),
        0U, CancellationToken{});
    expect_png_error(narrow_cicp, ErrorCode::kUnsupported, "unsupported_png_cicp_layout", "memory");

    const auto unknown_cicp = decoder.decode_memory(
        vector_bytes(rgb8_png(3U, 2U, {{QByteArrayLiteral("cICP"), cicp_chunk_payload(2U, 2U)}})),
        0U, CancellationToken{});
    expect_png_error(unknown_cicp, ErrorCode::kUnsupported, "unsupported_png_cicp_profile",
                     "memory");

    const auto duplicate_cicp = decoder.decode_memory(
        vector_bytes(rgb8_png(3U, 2U,
                              {{QByteArrayLiteral("cICP"), cicp_chunk_payload(1U, 13U)},
                               {QByteArrayLiteral("cICP"), cicp_chunk_payload(1U, 13U)}})),
        0U, CancellationToken{});
    expect_png_error(duplicate_cicp, ErrorCode::kValidation, "invalid_png_cicp", "memory");

    const auto conflicting_cicp = decoder.decode_memory(
        vector_bytes(rgb8_png(3U, 2U,
                              {{QByteArrayLiteral("sRGB"), QByteArray(1, '\0')},
                               {QByteArrayLiteral("cICP"), cicp_chunk_payload(12U, 13U)}})),
        0U, CancellationToken{});
    expect_png_error(conflicting_cicp, ErrorCode::kValidation, "conflicting_png_color_profiles",
                     "memory");

    for (const std::vector<Chunk> &chunks :
         {std::vector<Chunk>{{QByteArrayLiteral("iCCP"), iccp},
                             {QByteArrayLiteral("cICP"), cicp_chunk_payload(1U, 13U)}},
          std::vector<Chunk>{{QByteArrayLiteral("cICP"), cicp_chunk_payload(1U, 13U)},
                             {QByteArrayLiteral("iCCP"), iccp}}})
    {
        const auto conflicting_icc =
            decoder.decode_memory(vector_bytes(rgb8_png(3U, 2U, chunks)), 0U, CancellationToken{});
        expect_png_error(conflicting_icc, ErrorCode::kValidation, "conflicting_png_color_profiles",
                         "memory");
    }

    const auto legacy_gamma = decoder.decode_memory(
        vector_bytes(
            rgb8_png(3U, 2U, {{QByteArrayLiteral("gAMA"), QByteArray("\0\0\xb1\x8f", 4)}})),
        0U, CancellationToken{});
    expect_png_error(legacy_gamma, ErrorCode::kUnsupported, "unsupported_png_legacy_color_profile",
                     "memory");

    const auto legacy_chromaticities = decoder.decode_memory(
        vector_bytes(rgb8_png(3U, 2U, {{QByteArrayLiteral("cHRM"), QByteArray(32, '\0')}})), 0U,
        CancellationToken{});
    expect_png_error(legacy_chromaticities, ErrorCode::kUnsupported,
                     "unsupported_png_legacy_color_profile", "memory");
}

TEST(PngAdapterTest, AppliesEveryExifOrientationAndScalingBeforeExplicitRotation)
{
    QtRasterDecoder decoder;
    for (std::uint16_t orientation = 1U; orientation <= 8U; ++orientation)
    {
        const QByteArray encoded =
            rgb8_png(3U, 2U,
                     {{QByteArrayLiteral("eXIf"),
                       exif_orientation_payload(orientation, orientation != 8U)}});
        const auto decoded = decoder.decode_memory(vector_bytes(encoded), 0U, CancellationToken{});
        ASSERT_TRUE(decoded) << decoded.error().message;
        const bool transposed = orientation >= 5U;
        ASSERT_EQ(decoded.value().width, transposed ? 2U : 3U);
        ASSERT_EQ(decoded.value().height, transposed ? 3U : 2U);
        for (std::uint32_t y = 0U; y < decoded.value().height; ++y)
        {
            for (std::uint32_t x = 0U; x < decoded.value().width; ++x)
            {
                const auto source = oriented_source_coordinate(orientation, x, y, 3U, 2U);
                EXPECT_EQ(pixel(decoded.value(), x, y), source_pixel(source.first, source.second));
            }
        }
    }

    const QByteArray large =
        rgb8_png(80U, 40U, {{QByteArrayLiteral("eXIf"), exif_orientation_payload(6U)}});
    const auto scaled = decoder.decode_memory(vector_bytes(large), 20U, CancellationToken{});
    ASSERT_TRUE(scaled) << scaled.error().message;
    EXPECT_EQ(scaled.value().width, 10U);
    EXPECT_EQ(scaled.value().height, 20U);

    const auto rotated =
        decoder.decode_memory(vector_bytes(rgb8_png()), 0U, CancellationToken{}, 1);
    ASSERT_TRUE(rotated) << rotated.error().message;
    EXPECT_EQ(rotated.value().width, 2U);
    EXPECT_EQ(rotated.value().height, 3U);
}

TEST(PngAdapterTest, RecognizesContentEquallyForPathAndMemoryAndPreservesSources)
{
    PngTempDirectory temporary;
    const auto path = temporary.path() / "misleading.bin";
    const QByteArray encoded = rgb8_png();
    write_file(path, encoded);
    std::vector<std::uint8_t> memory = vector_bytes(encoded);
    const QByteArray before = hash(memory);
    QtRasterDecoder decoder;

    const auto probed = decoder.probe(path.string());
    ASSERT_TRUE(probed) << probed.error().message;
    EXPECT_EQ(probed.value().media_type, kMediaTypePng);
    EXPECT_EQ(probed.value().width, 3U);
    EXPECT_EQ(probed.value().height, 2U);
    const auto from_path = decoder.decode(path.string(), 0U, CancellationToken{});
    const auto from_memory = decoder.decode_memory(memory, 0U, CancellationToken{});
    ASSERT_TRUE(from_path) << from_path.error().message;
    ASSERT_TRUE(from_memory) << from_memory.error().message;
    EXPECT_EQ(from_path.value().rgb, from_memory.value().rgb);
    EXPECT_EQ(from_path.value().color_profile.kind, from_memory.value().color_profile.kind);
    EXPECT_EQ(hash(memory), before);

    QFile source(QString::fromStdString(path.string()));
    ASSERT_TRUE(source.open(QIODevice::ReadOnly));
    EXPECT_EQ(source.readAll(), encoded);
}

TEST(PngAdapterTest, ClassifiesMalformedTruncatedUnsupportedAndPixelCorruption)
{
    QtRasterDecoder decoder;
    QByteArray signature(reinterpret_cast<const char *>(kSignature.data()),
                         static_cast<qsizetype>(kSignature.size()));
    expect_png_error(decoder.decode_memory(vector_bytes(signature), 0U, CancellationToken{}),
                     ErrorCode::kValidation, "incomplete_png_stream", "memory");

    QByteArray bad_crc = rgb8_png();
    bad_crc[29] = static_cast<char>(bad_crc[29] ^ 0x01);
    expect_png_error(decoder.decode_memory(vector_bytes(bad_crc), 0U, CancellationToken{}),
                     ErrorCode::kValidation, "png_chunk_crc_mismatch", "memory");

    QByteArray truncated = rgb8_png();
    truncated.chop(7);
    expect_png_error(decoder.decode_memory(vector_bytes(truncated), 0U, CancellationToken{}),
                     ErrorCode::kValidation, "truncated_png_chunk_header", "memory");

    QByteArray unknown_critical =
        rgb8_png(3U, 2U, {{QByteArrayLiteral("ABCD"), QByteArrayLiteral("owner")}});
    expect_png_error(decoder.decode_memory(vector_bytes(unknown_critical), 0U, CancellationToken{}),
                     ErrorCode::kUnsupported, "unsupported_png_critical_chunk", "memory");

    QByteArray invalid_bit_depth = make_png(3U, 2U, 4U, 2U, rgb8_scanlines(3U, 2U));
    expect_png_error(
        decoder.decode_memory(vector_bytes(invalid_bit_depth), 0U, CancellationToken{}),
        ErrorCode::kValidation, "invalid_png_bit_depth", "memory");

    const QByteArray packed = make_png(1U, 1U, 1U, 0U, QByteArray("\0\0", 2));
    expect_png_error(decoder.decode_memory(vector_bytes(packed), 0U, CancellationToken{}),
                     ErrorCode::kUnsupported, "unsupported_png_packed_bit_depth", "memory");

    const QByteArray adam7 =
        png_with_idat(3U, 2U, 8U, 2U, compressed(rgb8_scanlines(3U, 2U)), {}, {}, 1U);
    expect_png_error(decoder.decode_memory(vector_bytes(adam7), 0U, CancellationToken{}),
                     ErrorCode::kUnsupported, "unsupported_png_adam7", "memory");

    const QByteArray invalid_orientation =
        rgb8_png(3U, 2U, {{QByteArrayLiteral("eXIf"), exif_orientation_payload(9U)}});
    expect_png_error(
        decoder.decode_memory(vector_bytes(invalid_orientation), 0U, CancellationToken{}),
        ErrorCode::kValidation, "invalid_png_exif_orientation", "memory");

    QByteArray corrupt_idat("\x78\x9c\0\0", 4);
    const QByteArray corrupt_pixels = png_with_idat(3U, 2U, 8U, 2U, corrupt_idat);
    expect_png_error(decoder.decode_memory(vector_bytes(corrupt_pixels), 0U, CancellationToken{}),
                     ErrorCode::kValidation, "png_pixel_decode_failed", "memory");

    const std::vector<std::uint8_t> random{'n', 'o', 't', '-', 'p', 'n', 'g'};
    const auto unsupported = decoder.decode_memory(random, 0U, CancellationToken{});
    ASSERT_FALSE(unsupported);
    EXPECT_EQ(unsupported.error().code, ErrorCode::kUnsupported);
}

TEST(PngAdapterTest, ProbeFullyValidatesPixelsBeforePublicationAndMatchesDecodeErrors)
{
    PngTempDirectory temporary;
    const auto path = temporary.path() / "corrupt.png";
    const QByteArray corrupt = png_with_idat(3U, 2U, 8U, 2U, QByteArray("\x78\x9c\0\0", 4));
    write_file(path, corrupt);
    QtRasterDecoder decoder;

    const auto probed = decoder.probe(path.string());
    expect_png_error(probed, ErrorCode::kValidation, "png_pixel_decode_failed", path.string());
    const auto file_decode = decoder.decode(path.string(), 0U, CancellationToken{});
    expect_png_error(file_decode, ErrorCode::kValidation, "png_pixel_decode_failed", path.string());
    const auto memory_decode =
        decoder.decode_memory(vector_bytes(corrupt), 0U, CancellationToken{});
    expect_png_error(memory_decode, ErrorCode::kValidation, "png_pixel_decode_failed", "memory");
}

TEST(PngAdapterTest, RejectsUnsafeDimensionsAndHonorsCancellationWithoutMutation)
{
    QtRasterDecoder decoder;
    QByteArray unsafe = png_with_idat(20000U, 20000U, 8U, 2U, QByteArray("\x78\x9c\0\0", 4));
    expect_png_error(decoder.decode_memory(vector_bytes(unsafe), 0U, CancellationToken{}),
                     ErrorCode::kValidation, "png_dimensions_exceed_allocation_limit", "memory");

    // 80 million pixels fit under 512 MiB with an incorrect 4 Bpp estimate,
    // but a 16-bit Qt native image may require 8 Bpp (640,000,000 bytes).
    const QByteArray unsafe_16_bit =
        png_with_idat(10000U, 8000U, 16U, 2U, QByteArray("\x78\x9c\0\0", 4));
    const auto rejected_16_bit =
        decoder.decode_memory(vector_bytes(unsafe_16_bit), 0U, CancellationToken{});
    expect_png_error(rejected_16_bit, ErrorCode::kValidation,
                     "png_dimensions_exceed_allocation_limit", "memory");
    ASSERT_TRUE(rejected_16_bit.error().context.contains("native_bytes_per_pixel"));
    EXPECT_EQ(rejected_16_bit.error().context.at("native_bytes_per_pixel"), "8");
    ASSERT_TRUE(rejected_16_bit.error().context.contains("worst_case_native_bytes"));
    EXPECT_EQ(rejected_16_bit.error().context.at("worst_case_native_bytes"), "640000000");

    QByteArray large_ancillary(8 * 1024 * 1024, 'x');
    const QByteArray encoded = rgb8_png(3U, 2U, {{QByteArrayLiteral("raVo"), large_ancillary}});
    std::vector<std::uint8_t> source = vector_bytes(encoded);
    const QByteArray before = hash(source);
    const CancellationSource deadline = CancellationSource::with_deadline(
        std::chrono::steady_clock::now() + std::chrono::milliseconds(1));
    const auto cancelled = decoder.decode_memory(source, 0U, deadline.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(hash(source), before);
}

TEST(PngAdapterTest, DistinguishesPathFailures)
{
    PngTempDirectory temporary;
    QtRasterDecoder decoder;
    const auto empty = decoder.probe("");
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().code, ErrorCode::kInvalidArgument);
    const auto missing = decoder.probe((temporary.path() / "missing.png").string());
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::kNotFound);
    const auto directory = decoder.probe(temporary.path().string());
    ASSERT_FALSE(directory);
    EXPECT_EQ(directory.error().code, ErrorCode::kInvalidArgument);
}

} // namespace
} // namespace ravo

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
