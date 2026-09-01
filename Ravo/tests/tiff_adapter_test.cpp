#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <lcms2.h>
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

enum class ByteOrder
{
    kLittle,
    kBig,
};

inline constexpr std::uint16_t kTypeByte = 1U;
inline constexpr std::uint16_t kTypeShort = 3U;
inline constexpr std::uint16_t kTypeLong = 4U;
inline constexpr std::uint16_t kTypeUndefined = 7U;
inline constexpr std::uint16_t kTypeLong8 = 16U;

inline constexpr std::uint16_t kCompressionNone = 1U;
inline constexpr std::uint16_t kCompressionLzw = 5U;
inline constexpr std::uint16_t kCompressionDeflate = 8U;
inline constexpr std::uint16_t kCompressionAdobeDeflate = 32946U;
inline constexpr std::uint16_t kCompressionPackBits = 32773U;

struct Entry
{
    std::uint16_t tag = 0U;
    std::uint16_t type = 0U;
    std::uint64_t count = 0U;
    QByteArray payload;
    std::uint64_t external_offset = 0U;
};

struct TiffOptions
{
    ByteOrder order = ByteOrder::kLittle;
    bool big_tiff = false;
    std::uint32_t width = 3U;
    std::uint32_t height = 2U;
    std::uint16_t bits = 8U;
    std::uint16_t samples = 3U;
    std::uint16_t sample_format = 1U;
    std::uint16_t photometric = 2U;
    std::uint16_t compression = kCompressionNone;
    std::uint16_t orientation = 1U;
    std::uint32_t rows_per_strip = 0U;
    std::optional<std::uint16_t> extra_sample;
    std::uint16_t extra_sample_type = kTypeShort;
    std::uint64_t extra_sample_count = 1U;
    QByteArray pixels;
    QByteArray icc;
    bool duplicate_icc = false;
    bool duplicate_sample_format = false;
    bool multi_page = false;
    bool sub_ifd = false;
    bool dng_tag = false;
    QByteArray filler;
};

class TiffTempDirectory
{
public:
    TiffTempDirectory()
        : path_(std::filesystem::temp_directory_path() /
                ("ravo-tiff-adapter-" + generate_catalog_id()))
    {
        std::filesystem::create_directories(path_);
    }

    ~TiffTempDirectory()
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

void append_u16(QByteArray &bytes, const std::uint16_t value, const ByteOrder order)
{
    if (order == ByteOrder::kLittle)
    {
        bytes.append(static_cast<char>(value & 0xFFU));
        bytes.append(static_cast<char>((value >> 8U) & 0xFFU));
    }
    else
    {
        bytes.append(static_cast<char>((value >> 8U) & 0xFFU));
        bytes.append(static_cast<char>(value & 0xFFU));
    }
}

void append_u32(QByteArray &bytes, const std::uint32_t value, const ByteOrder order)
{
    if (order == ByteOrder::kLittle)
    {
        for (unsigned int shift = 0U; shift < 32U; shift += 8U)
        {
            bytes.append(static_cast<char>((value >> shift) & 0xFFU));
        }
    }
    else
    {
        for (unsigned int shift = 32U; shift > 0U; shift -= 8U)
        {
            bytes.append(static_cast<char>((value >> (shift - 8U)) & 0xFFU));
        }
    }
}

void append_u64(QByteArray &bytes, const std::uint64_t value, const ByteOrder order)
{
    if (order == ByteOrder::kLittle)
    {
        for (unsigned int shift = 0U; shift < 64U; shift += 8U)
        {
            bytes.append(static_cast<char>((value >> shift) & 0xFFU));
        }
    }
    else
    {
        for (unsigned int shift = 64U; shift > 0U; shift -= 8U)
        {
            bytes.append(static_cast<char>((value >> (shift - 8U)) & 0xFFU));
        }
    }
}

[[nodiscard]] QByteArray short_values(const std::vector<std::uint16_t> &values,
                                      const ByteOrder order)
{
    QByteArray result;
    for (const std::uint16_t value : values)
    {
        append_u16(result, value, order);
    }
    return result;
}

[[nodiscard]] QByteArray long_value(const std::uint64_t value, const bool big_tiff,
                                    const ByteOrder order)
{
    QByteArray result;
    if (big_tiff)
    {
        append_u64(result, value, order);
    }
    else
    {
        append_u32(result, static_cast<std::uint32_t>(value), order);
    }
    return result;
}

[[nodiscard]] QByteArray deflate(const QByteArray &input)
{
    uLongf size = compressBound(static_cast<uLong>(input.size()));
    QByteArray result(static_cast<qsizetype>(size), Qt::Uninitialized);
    const int status = compress2(reinterpret_cast<Bytef *>(result.data()), &size,
                                 reinterpret_cast<const Bytef *>(input.constData()),
                                 static_cast<uLong>(input.size()), Z_BEST_SPEED);
    EXPECT_EQ(status, Z_OK);
    result.resize(static_cast<qsizetype>(size));
    return result;
}

[[nodiscard]] QByteArray pack_bits(const QByteArray &input)
{
    QByteArray result;
    for (qsizetype position = 0; position < input.size();)
    {
        const qsizetype count = std::min<qsizetype>(128, input.size() - position);
        result.append(static_cast<char>(count - 1));
        result.append(input.constData() + position, count);
        position += count;
    }
    return result;
}

[[nodiscard]] QByteArray literal_lzw(const QByteArray &input)
{
    std::vector<std::uint16_t> codes;
    codes.reserve(static_cast<std::size_t>(input.size()) + 2U);
    codes.push_back(256U);
    for (const char value : input)
    {
        codes.push_back(static_cast<unsigned char>(value));
    }
    codes.push_back(257U);

    QByteArray result;
    std::uint64_t accumulator = 0U;
    unsigned int bits = 0U;
    for (const std::uint16_t code : codes)
    {
        accumulator = (accumulator << 9U) | code;
        bits += 9U;
        while (bits >= 8U)
        {
            bits -= 8U;
            result.append(static_cast<char>((accumulator >> bits) & 0xFFU));
        }
    }
    if (bits != 0U)
    {
        result.append(static_cast<char>((accumulator << (8U - bits)) & 0xFFU));
    }
    return result;
}

[[nodiscard]] QByteArray encode_strip(const QByteArray &pixels, const std::uint16_t compression)
{
    if (compression == kCompressionNone)
    {
        return pixels;
    }
    if (compression == kCompressionLzw)
    {
        return literal_lzw(pixels);
    }
    if (compression == kCompressionDeflate || compression == kCompressionAdobeDeflate)
    {
        return deflate(pixels);
    }
    if (compression == kCompressionPackBits)
    {
        return pack_bits(pixels);
    }
    return pixels;
}

[[nodiscard]] std::uint64_t align_up(const std::uint64_t value, const std::uint64_t alignment)
{
    return (value + alignment - 1U) / alignment * alignment;
}

void write_at(QByteArray &bytes, const std::uint64_t offset, const QByteArray &payload)
{
    ASSERT_LE(offset + static_cast<std::uint64_t>(payload.size()),
              static_cast<std::uint64_t>(std::numeric_limits<qsizetype>::max()));
    const qsizetype end =
        static_cast<qsizetype>(offset + static_cast<std::uint64_t>(payload.size()));
    if (bytes.size() < end)
    {
        bytes.resize(end);
    }
    std::copy(payload.cbegin(), payload.cend(), bytes.begin() + static_cast<qsizetype>(offset));
}

[[nodiscard]] QByteArray build_tiff(const TiffOptions &options)
{
    const std::uint64_t inline_size = options.big_tiff ? 8U : 4U;
    const std::uint64_t ifd_offset = options.big_tiff ? 16U : 8U;
    const std::uint64_t count_size = options.big_tiff ? 8U : 2U;
    const std::uint64_t entry_size = options.big_tiff ? 20U : 12U;
    const std::uint16_t offset_type = options.big_tiff ? kTypeLong8 : kTypeLong;

    std::vector<Entry> entries;
    const auto add = [&](const std::uint16_t tag, const std::uint16_t type,
                         const std::uint64_t count, QByteArray payload)
    { entries.push_back({tag, type, count, std::move(payload), 0U}); };
    add(254U, kTypeLong, 1U, long_value(0U, false, options.order));
    add(256U, kTypeLong, 1U, long_value(options.width, false, options.order));
    add(257U, kTypeLong, 1U, long_value(options.height, false, options.order));
    add(258U, kTypeShort, options.samples,
        short_values(std::vector<std::uint16_t>(options.samples, options.bits), options.order));
    add(259U, kTypeShort, 1U, short_values({options.compression}, options.order));
    add(262U, kTypeShort, 1U, short_values({options.photometric}, options.order));
    add(273U, offset_type, 1U, long_value(0U, options.big_tiff, options.order));
    add(274U, kTypeShort, 1U, short_values({options.orientation}, options.order));
    add(277U, kTypeShort, 1U, short_values({options.samples}, options.order));
    add(278U, kTypeLong, 1U,
        long_value(options.rows_per_strip == 0U ? options.height : options.rows_per_strip, false,
                   options.order));

    const QByteArray strip = encode_strip(options.pixels, options.compression);
    add(279U, offset_type, 1U,
        long_value(static_cast<std::uint64_t>(strip.size()), options.big_tiff, options.order));
    add(284U, kTypeShort, 1U, short_values({1U}, options.order));
    if (options.sub_ifd)
    {
        add(330U, offset_type, 1U, long_value(0U, options.big_tiff, options.order));
    }
    if (options.extra_sample)
    {
        add(338U, options.extra_sample_type, options.extra_sample_count,
            short_values({*options.extra_sample}, options.order));
    }
    add(339U, kTypeShort, options.samples,
        short_values(std::vector<std::uint16_t>(options.samples, options.sample_format),
                     options.order));
    if (options.duplicate_sample_format)
    {
        add(339U, kTypeShort, options.samples,
            short_values(std::vector<std::uint16_t>(options.samples, options.sample_format),
                         options.order));
    }
    if (!options.icc.isEmpty())
    {
        add(34675U, kTypeUndefined, static_cast<std::uint64_t>(options.icc.size()), options.icc);
        if (options.duplicate_icc)
        {
            add(34675U, kTypeUndefined, static_cast<std::uint64_t>(options.icc.size()),
                options.icc);
        }
    }
    if (options.dng_tag)
    {
        add(50706U, kTypeByte, 4U, QByteArray("\x01\x04\0\0", 4));
    }
    if (!options.filler.isEmpty())
    {
        add(65000U, kTypeUndefined, static_cast<std::uint64_t>(options.filler.size()),
            options.filler);
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry &left, const Entry &right) { return left.tag < right.tag; });

    const std::uint64_t ifd_end =
        ifd_offset + count_size + entries.size() * entry_size + inline_size;
    std::uint64_t cursor = ifd_end;
    for (Entry &entry : entries)
    {
        if (static_cast<std::uint64_t>(entry.payload.size()) > inline_size)
        {
            cursor = align_up(cursor, options.big_tiff ? 8U : 2U);
            entry.external_offset = cursor;
            cursor += static_cast<std::uint64_t>(entry.payload.size());
        }
    }
    cursor = align_up(cursor, options.big_tiff ? 8U : 2U);
    const std::uint64_t strip_offset = cursor;
    cursor += static_cast<std::uint64_t>(strip.size());
    const std::uint64_t second_ifd_offset = align_up(cursor, options.big_tiff ? 8U : 2U);

    for (Entry &entry : entries)
    {
        if (entry.tag == 273U)
        {
            entry.payload = long_value(strip_offset, options.big_tiff, options.order);
        }
        else if (entry.tag == 330U)
        {
            entry.payload = long_value(second_ifd_offset, options.big_tiff, options.order);
        }
    }

    QByteArray result;
    result.append(options.order == ByteOrder::kLittle ? "II" : "MM", 2);
    if (options.big_tiff)
    {
        append_u16(result, 43U, options.order);
        append_u16(result, 8U, options.order);
        append_u16(result, 0U, options.order);
        append_u64(result, ifd_offset, options.order);
        append_u64(result, static_cast<std::uint64_t>(entries.size()), options.order);
    }
    else
    {
        append_u16(result, 42U, options.order);
        append_u32(result, static_cast<std::uint32_t>(ifd_offset), options.order);
        append_u16(result, static_cast<std::uint16_t>(entries.size()), options.order);
    }
    for (const Entry &entry : entries)
    {
        append_u16(result, entry.tag, options.order);
        append_u16(result, entry.type, options.order);
        if (options.big_tiff)
        {
            append_u64(result, entry.count, options.order);
        }
        else
        {
            append_u32(result, static_cast<std::uint32_t>(entry.count), options.order);
        }
        if (static_cast<std::uint64_t>(entry.payload.size()) > inline_size)
        {
            if (options.big_tiff)
            {
                append_u64(result, entry.external_offset, options.order);
            }
            else
            {
                append_u32(result, static_cast<std::uint32_t>(entry.external_offset),
                           options.order);
            }
        }
        else
        {
            result.append(entry.payload);
            result.append(static_cast<qsizetype>(inline_size -
                                                 static_cast<std::uint64_t>(entry.payload.size())),
                          '\0');
        }
    }
    if (options.big_tiff)
    {
        append_u64(result, options.multi_page ? second_ifd_offset : 0U, options.order);
    }
    else
    {
        append_u32(result, options.multi_page ? static_cast<std::uint32_t>(second_ifd_offset) : 0U,
                   options.order);
    }
    for (const Entry &entry : entries)
    {
        if (entry.external_offset != 0U)
        {
            write_at(result, entry.external_offset, entry.payload);
        }
    }
    write_at(result, strip_offset, strip);
    if (options.multi_page || options.sub_ifd)
    {
        QByteArray empty_ifd;
        if (options.big_tiff)
        {
            append_u64(empty_ifd, 0U, options.order);
            append_u64(empty_ifd, 0U, options.order);
        }
        else
        {
            append_u16(empty_ifd, 0U, options.order);
            append_u32(empty_ifd, 0U, options.order);
        }
        write_at(result, second_ifd_offset, empty_ifd);
    }
    return result;
}

[[nodiscard]] QByteArray rgb8_pixels(const std::uint32_t width = 3U,
                                     const std::uint32_t height = 2U,
                                     const std::uint16_t samples = 3U,
                                     const std::uint8_t alpha = 255U,
                                     const bool premultiply = false)
{
    QByteArray result;
    result.reserve(static_cast<qsizetype>(width) * height * samples);
    for (std::uint32_t y = 0U; y < height; ++y)
    {
        for (std::uint32_t x = 0U; x < width; ++x)
        {
            const auto stored = [&](const std::uint8_t value)
            {
                return premultiply ? static_cast<std::uint8_t>(
                                         (static_cast<unsigned int>(value) * alpha + 127U) / 255U) :
                                     value;
            };
            result.append(static_cast<char>(
                stored(static_cast<std::uint8_t>((x * 37U + y * 17U + 11U) & 0xFFU))));
            if (samples >= 3U)
            {
                result.append(static_cast<char>(
                    stored(static_cast<std::uint8_t>((x * 13U + y * 61U + 23U) & 0xFFU))));
                result.append(static_cast<char>(
                    stored(static_cast<std::uint8_t>((x * 71U + y * 29U + 31U) & 0xFFU))));
            }
            if (samples == 2U || samples == 4U)
            {
                result.append(static_cast<char>(alpha));
            }
        }
    }
    return result;
}

[[nodiscard]] std::array<std::uint8_t, 3> source_pixel(const std::uint32_t x, const std::uint32_t y)
{
    return {static_cast<std::uint8_t>((x * 37U + y * 17U + 11U) & 0xFFU),
            static_cast<std::uint8_t>((x * 13U + y * 61U + 23U) & 0xFFU),
            static_cast<std::uint8_t>((x * 71U + y * 29U + 31U) & 0xFFU)};
}

[[nodiscard]] std::array<std::uint8_t, 3>
premultiplied_source_pixel(const std::uint32_t x, const std::uint32_t y, const std::uint8_t alpha)
{
    auto result = source_pixel(x, y);
    for (std::uint8_t &channel : result)
    {
        channel =
            static_cast<std::uint8_t>((static_cast<unsigned int>(channel) * alpha + 127U) / 255U);
    }
    return result;
}

[[nodiscard]] QByteArray gray8_pixels(const bool invert = false)
{
    QByteArray result("\x11\x80\xff", 3);
    if (invert)
    {
        for (char &value : result)
        {
            value = static_cast<char>(255U - static_cast<unsigned char>(value));
        }
    }
    return result;
}

[[nodiscard]] QByteArray rgb16_pixels(const ByteOrder order)
{
    QByteArray result;
    static constexpr std::array<std::uint16_t, 6U> kValues{0x0000U, 0x0080U, 0x8080U,
                                                           0xFFFFU, 0x0100U, 0x7FFFU};
    for (const std::uint16_t value : kValues)
    {
        append_u16(result, value, order);
    }
    return result;
}

[[nodiscard]] QByteArray gray_icc()
{
    cmsToneCurve *curve = cmsBuildGamma(nullptr, 2.2);
    EXPECT_NE(curve, nullptr);
    const cmsCIExyY white = *cmsD50_xyY();
    cmsHPROFILE profile = cmsCreateGrayProfile(&white, curve);
    cmsFreeToneCurve(curve);
    EXPECT_NE(profile, nullptr);
    cmsUInt32Number size = 0U;
    EXPECT_TRUE(cmsSaveProfileToMem(profile, nullptr, &size));
    QByteArray result(static_cast<qsizetype>(size), Qt::Uninitialized);
    EXPECT_TRUE(cmsSaveProfileToMem(profile, result.data(), &size));
    result.resize(static_cast<qsizetype>(size));
    cmsCloseProfile(profile);
    return result;
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

[[nodiscard]] QByteArray hash(const QByteArray &bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
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
void expect_tiff_error(const Result<T> &result, const ErrorCode code, const std::string_view reason,
                       const std::string_view source)
{
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, code);
    ASSERT_TRUE(result.error().context.contains("format"));
    EXPECT_EQ(result.error().context.at("format"), "tiff");
    ASSERT_TRUE(result.error().context.contains("reason"));
    EXPECT_EQ(result.error().context.at("reason"), reason);
    ASSERT_TRUE(result.error().context.contains("source"));
    EXPECT_EQ(result.error().context.at("source"), source);
}

TEST(TiffAdapterTest, RecognizesClassicAndBigTiffByContentForPathAndMemory)
{
    TiffOptions classic;
    classic.pixels = rgb8_pixels();
    const QByteArray encoded = build_tiff(classic);
    TiffTempDirectory temporary;
    const auto path = temporary.path() / "misleading.bin";
    write_file(path, encoded);
    std::vector<std::uint8_t> memory = vector_bytes(encoded);
    const QByteArray before = hash(memory);
    QtRasterDecoder decoder;

    const auto probed = decoder.probe(path.string());
    ASSERT_TRUE(probed) << probed.error().message;
    EXPECT_EQ(probed.value().media_type, kMediaTypeTiff);
    EXPECT_EQ(probed.value().width, 3U);
    EXPECT_EQ(probed.value().height, 2U);
    const auto from_path = decoder.decode(path.string(), 0U, CancellationToken{});
    const auto from_memory = decoder.decode_memory(memory, 0U, CancellationToken{});
    ASSERT_TRUE(from_path) << from_path.error().message;
    ASSERT_TRUE(from_memory) << from_memory.error().message;
    EXPECT_EQ(from_path.value().rgb, from_memory.value().rgb);
    EXPECT_EQ(pixel(from_path.value(), 2U, 1U), source_pixel(2U, 1U));
    EXPECT_EQ(hash(memory), before);
    EXPECT_EQ(read_file(path), encoded);

    TiffOptions big;
    big.order = ByteOrder::kBig;
    big.big_tiff = true;
    big.pixels = rgb8_pixels();
    const auto big_decoded =
        decoder.decode_memory(vector_bytes(build_tiff(big)), 0U, CancellationToken{});
    ASSERT_TRUE(big_decoded) << big_decoded.error().message;
    EXPECT_EQ(big_decoded.value().rgb, from_memory.value().rgb);
}

TEST(TiffAdapterTest, DecodesGrayAndPinnedCompressionModesAsOpaqueRgb8)
{
    QtRasterDecoder decoder;
    for (const std::uint16_t compression : {kCompressionNone, kCompressionLzw, kCompressionDeflate,
                                            kCompressionAdobeDeflate, kCompressionPackBits})
    {
        TiffOptions options;
        options.compression = compression;
        options.pixels = rgb8_pixels();
        const auto decoded =
            decoder.decode_memory(vector_bytes(build_tiff(options)), 0U, CancellationToken{});
        ASSERT_TRUE(decoded) << "compression=" << compression << ' ' << decoded.error().message;
        EXPECT_EQ(decoded.value().pixel_format, RasterPixelFormat::kRgb8);
        EXPECT_EQ(decoded.value().alpha_mode, RasterAlphaMode::kOpaque);
        EXPECT_EQ(pixel(decoded.value(), 2U, 1U), source_pixel(2U, 1U));
    }

    TiffOptions gray;
    gray.width = 3U;
    gray.height = 1U;
    gray.samples = 1U;
    gray.photometric = 1U;
    gray.pixels = gray8_pixels();
    const auto gray_decoded =
        decoder.decode_memory(vector_bytes(build_tiff(gray)), 0U, CancellationToken{});
    ASSERT_TRUE(gray_decoded) << gray_decoded.error().message;
    EXPECT_EQ(pixel(gray_decoded.value(), 0U, 0U), (std::array<std::uint8_t, 3>{17U, 17U, 17U}));
    EXPECT_EQ(pixel(gray_decoded.value(), 1U, 0U), (std::array<std::uint8_t, 3>{128U, 128U, 128U}));

    gray.photometric = 0U;
    gray.pixels = gray8_pixels(true);
    const auto white_zero =
        decoder.decode_memory(vector_bytes(build_tiff(gray)), 0U, CancellationToken{});
    ASSERT_TRUE(white_zero) << white_zero.error().message;
    EXPECT_EQ(white_zero.value().rgb, gray_decoded.value().rgb);
}

TEST(TiffAdapterTest, QuantizesSixteenBitRgbAndGrayWithPinnedHighBytePolicy)
{
    QtRasterDecoder decoder;
    for (const ByteOrder order : {ByteOrder::kLittle, ByteOrder::kBig})
    {
        TiffOptions options;
        options.order = order;
        options.width = 2U;
        options.height = 1U;
        options.bits = 16U;
        options.pixels = rgb16_pixels(order);
        const auto decoded =
            decoder.decode_memory(vector_bytes(build_tiff(options)), 0U, CancellationToken{});
        ASSERT_TRUE(decoded) << decoded.error().message;
        EXPECT_EQ(pixel(decoded.value(), 0U, 0U), (std::array<std::uint8_t, 3>{0U, 0U, 128U}));
        EXPECT_EQ(pixel(decoded.value(), 1U, 0U), (std::array<std::uint8_t, 3>{255U, 1U, 127U}));
    }

    TiffOptions gray;
    gray.width = 1U;
    gray.height = 1U;
    gray.bits = 16U;
    gray.samples = 1U;
    gray.photometric = 1U;
    append_u16(gray.pixels, 0x80FFU, gray.order);
    const auto decoded_gray =
        decoder.decode_memory(vector_bytes(build_tiff(gray)), 0U, CancellationToken{});
    ASSERT_TRUE(decoded_gray) << decoded_gray.error().message;
    EXPECT_EQ(pixel(decoded_gray.value(), 0U, 0U), (std::array<std::uint8_t, 3>{128U, 128U, 128U}));
}

TEST(TiffAdapterTest, DiscardsRawAlphaWithoutUnpremultiplyingStoredRgb)
{
    QtRasterDecoder decoder;
    static constexpr std::array<std::uint16_t, 2U> kExtraSamples{1U, 2U};
    for (const std::uint16_t extra_sample : kExtraSamples)
    {
        constexpr std::uint8_t kAlpha = 128U;
        TiffOptions options;
        options.samples = 4U;
        options.extra_sample = extra_sample;
        // ExtraSamples=1 stores valid premultiplied RGB. The legacy input owner
        // discarded the extra plane without unpremultiplication; freeze that raw
        // discard parity while ExtraSamples=2 remains straight RGB.
        options.pixels = rgb8_pixels(3U, 2U, 4U, kAlpha, extra_sample == 1U);
        const auto decoded =
            decoder.decode_memory(vector_bytes(build_tiff(options)), 0U, CancellationToken{});
        ASSERT_TRUE(decoded) << decoded.error().message;
        EXPECT_EQ(decoded.value().alpha_mode, RasterAlphaMode::kOpaque);
        const auto expected =
            extra_sample == 1U ? premultiplied_source_pixel(2U, 1U, kAlpha) : source_pixel(2U, 1U);
        EXPECT_EQ(pixel(decoded.value(), 2U, 1U), expected);
    }
}

TEST(TiffAdapterTest, OwnsExactRgbIccAndLeavesUntaggedInputMissing)
{
    QtRasterDecoder decoder;
    const QByteArray display_p3 = QColorSpace(QColorSpace::DisplayP3).iccProfile();
    ASSERT_FALSE(display_p3.isEmpty());
    TiffOptions tagged;
    tagged.pixels = rgb8_pixels();
    tagged.icc = display_p3;
    const auto icc =
        decoder.decode_memory(vector_bytes(build_tiff(tagged)), 0U, CancellationToken{});
    ASSERT_TRUE(icc) << icc.error().message;
    EXPECT_EQ(icc.value().color_profile.kind, ColorProfileKind::kIcc);
    EXPECT_EQ(icc.value().color_profile.model, ColorModel::kRgb);
    EXPECT_EQ(icc.value().color_profile.identifier, "embedded_icc");
    EXPECT_EQ(icc.value().color_profile.icc_bytes, vector_bytes(display_p3));

    TiffOptions untagged;
    untagged.pixels = rgb8_pixels();
    const auto missing =
        decoder.decode_memory(vector_bytes(build_tiff(untagged)), 0U, CancellationToken{});
    ASSERT_TRUE(missing) << missing.error().message;
    EXPECT_EQ(missing.value().color_profile.kind, ColorProfileKind::kMissing);
    EXPECT_TRUE(missing.value().color_profile.icc_bytes.empty());
}

TEST(TiffAdapterTest, RejectsCorruptNonRgbDuplicateAndOversizedIcc)
{
    QtRasterDecoder decoder;
    TiffOptions corrupt;
    corrupt.pixels = rgb8_pixels();
    corrupt.icc = QByteArrayLiteral("not-an-icc-profile");
    expect_tiff_error(
        decoder.decode_memory(vector_bytes(build_tiff(corrupt)), 0U, CancellationToken{}),
        ErrorCode::kValidation, "corrupt_tiff_icc_profile", "memory");

    TiffOptions non_rgb = corrupt;
    non_rgb.icc = gray_icc();
    expect_tiff_error(
        decoder.decode_memory(vector_bytes(build_tiff(non_rgb)), 0U, CancellationToken{}),
        ErrorCode::kUnsupported, "unsupported_tiff_icc_color_model", "memory");

    TiffOptions duplicate = non_rgb;
    duplicate.icc = QColorSpace(QColorSpace::SRgb).iccProfile();
    duplicate.duplicate_icc = true;
    expect_tiff_error(
        decoder.decode_memory(vector_bytes(build_tiff(duplicate)), 0U, CancellationToken{}),
        ErrorCode::kValidation, "duplicate_tiff_icc_profile", "memory");

    TiffOptions oversized;
    oversized.pixels = rgb8_pixels();
    oversized.icc = QByteArray(16 * 1024 * 1024 + 1, 'x');
    expect_tiff_error(
        decoder.decode_memory(vector_bytes(build_tiff(oversized)), 0U, CancellationToken{}),
        ErrorCode::kValidation, "oversized_tiff_icc_profile", "memory");
}

TEST(TiffAdapterTest, AppliesAllOrientationsAndScalingBeforeExplicitRotation)
{
    QtRasterDecoder decoder;
    for (std::uint16_t orientation = 1U; orientation <= 8U; ++orientation)
    {
        TiffOptions options;
        options.orientation = orientation;
        options.pixels = rgb8_pixels();
        const auto decoded =
            decoder.decode_memory(vector_bytes(build_tiff(options)), 0U, CancellationToken{});
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

    TiffOptions large;
    large.width = 80U;
    large.height = 40U;
    large.orientation = 6U;
    large.pixels = rgb8_pixels(large.width, large.height);
    const auto scaled =
        decoder.decode_memory(vector_bytes(build_tiff(large)), 20U, CancellationToken{});
    ASSERT_TRUE(scaled) << scaled.error().message;
    EXPECT_EQ(scaled.value().width, 10U);
    EXPECT_EQ(scaled.value().height, 20U);
    EXPECT_EQ(scaled.value().source_width, 40U);
    EXPECT_EQ(scaled.value().source_height, 80U);

    TiffOptions rotated_options;
    rotated_options.pixels = rgb8_pixels();
    const auto rotated = decoder.decode_memory(vector_bytes(build_tiff(rotated_options)), 0U,
                                               CancellationToken{}, 1);
    ASSERT_TRUE(rotated) << rotated.error().message;
    EXPECT_EQ(rotated.value().width, 2U);
    EXPECT_EQ(rotated.value().height, 3U);
    EXPECT_EQ(rotated.value().source_width, 2U);
    EXPECT_EQ(rotated.value().source_height, 3U);
}

TEST(TiffAdapterTest, RejectsFloatPagesSubIfdsAndRawContainersWithoutStealingRawRouting)
{
    QtRasterDecoder decoder;
    static constexpr std::array<std::uint16_t, 2U> kFloatDepths{16U, 32U};
    for (const std::uint16_t bits : kFloatDepths)
    {
        TiffOptions floating;
        floating.width = 1U;
        floating.height = 1U;
        floating.bits = bits;
        floating.sample_format = 3U;
        floating.pixels = QByteArray(bits == 16U ? 6 : 12, '\0');
        expect_tiff_error(
            decoder.decode_memory(vector_bytes(build_tiff(floating)), 0U, CancellationToken{}),
            ErrorCode::kUnsupported, "unsupported_tiff_float_samples", "memory");
    }

    TiffOptions multi;
    multi.pixels = rgb8_pixels();
    multi.multi_page = true;
    expect_tiff_error(
        decoder.decode_memory(vector_bytes(build_tiff(multi)), 0U, CancellationToken{}),
        ErrorCode::kUnsupported, "unsupported_tiff_multi_page", "memory");

    TiffOptions sub_ifd;
    sub_ifd.pixels = rgb8_pixels();
    sub_ifd.sub_ifd = true;
    expect_tiff_error(
        decoder.decode_memory(vector_bytes(build_tiff(sub_ifd)), 0U, CancellationToken{}),
        ErrorCode::kUnsupported, "unsupported_tiff_subifd", "memory");

    TiffOptions dng;
    dng.pixels = rgb8_pixels();
    dng.dng_tag = true;
    expect_tiff_error(decoder.decode_memory(vector_bytes(build_tiff(dng)), 0U, CancellationToken{}),
                      ErrorCode::kUnsupported, "unsupported_tiff_raw_container", "memory");
    dng.filler = QByteArray(32, 'x');
    QByteArray damaged_raw = build_tiff(dng);
    damaged_raw.chop(16);
    expect_tiff_error(decoder.decode_memory(vector_bytes(damaged_raw), 0U, CancellationToken{}),
                      ErrorCode::kUnsupported, "unsupported_tiff_raw_container", "memory");

    const std::filesystem::path arw =
        std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo/tests/fixtures/frozen/images/hlrecovery.arw";
    const QByteArray arw_bytes = read_file(arw);
    ASSERT_FALSE(arw_bytes.isEmpty());
    const QByteArray before = hash(arw_bytes);
    EXPECT_EQ(
        before.toHex(),
        QByteArrayLiteral("0cc9c842f163c40b02f7dc97e4b6d6a4f159c58a77f9b338338ff45621ed32d0"));
    const auto path_result = decoder.probe(arw.string());
    expect_tiff_error(path_result, ErrorCode::kUnsupported, "unsupported_tiff_raw_container",
                      arw.string());
    const auto memory_result =
        decoder.decode_memory(vector_bytes(arw_bytes), 0U, CancellationToken{});
    expect_tiff_error(memory_result, ErrorCode::kUnsupported, "unsupported_tiff_raw_container",
                      "memory");
    EXPECT_EQ(hash(read_file(arw)), before);
}

TEST(TiffAdapterTest, ClassifiesMalformedTruncatedUnsupportedAndPixelCorruption)
{
    QtRasterDecoder decoder;
    expect_tiff_error(
        decoder.decode_memory(vector_bytes(QByteArray("II*\0", 4)), 0U, CancellationToken{}),
        ErrorCode::kValidation, "truncated_tiff_header", "memory");

    TiffOptions duplicate_sample_format;
    duplicate_sample_format.pixels = rgb8_pixels();
    duplicate_sample_format.duplicate_sample_format = true;
    const auto duplicate_result = decoder.decode_memory(
        vector_bytes(build_tiff(duplicate_sample_format)), 0U, CancellationToken{});
    expect_tiff_error(duplicate_result, ErrorCode::kValidation, "duplicate_tiff_field", "memory");
    ASSERT_TRUE(duplicate_result.error().context.contains("tag"));
    EXPECT_EQ(duplicate_result.error().context.at("tag"), "339");

    TiffOptions malformed_extra_sample;
    malformed_extra_sample.samples = 4U;
    malformed_extra_sample.extra_sample = 1U;
    malformed_extra_sample.extra_sample_type = kTypeByte;
    malformed_extra_sample.pixels = rgb8_pixels(3U, 2U, 4U, 128U, true);
    expect_tiff_error(decoder.decode_memory(vector_bytes(build_tiff(malformed_extra_sample)), 0U,
                                            CancellationToken{}),
                      ErrorCode::kValidation, "invalid_tiff_extra_samples_field", "memory");
    malformed_extra_sample.extra_sample_type = kTypeShort;
    malformed_extra_sample.extra_sample_count = 2U;
    expect_tiff_error(decoder.decode_memory(vector_bytes(build_tiff(malformed_extra_sample)), 0U,
                                            CancellationToken{}),
                      ErrorCode::kValidation, "invalid_tiff_extra_samples_field", "memory");

    TiffOptions maximum_rows_per_strip;
    maximum_rows_per_strip.rows_per_strip = std::numeric_limits<std::uint32_t>::max();
    maximum_rows_per_strip.pixels = rgb8_pixels();
    const auto maximum_rows_result = decoder.decode_memory(
        vector_bytes(build_tiff(maximum_rows_per_strip)), 0U, CancellationToken{});
    ASSERT_TRUE(maximum_rows_result) << maximum_rows_result.error().message;
    EXPECT_EQ(pixel(maximum_rows_result.value(), 2U, 1U), source_pixel(2U, 1U));

    TiffOptions unsupported;
    unsupported.pixels = rgb8_pixels();
    unsupported.compression = 65000U;
    expect_tiff_error(
        decoder.decode_memory(vector_bytes(build_tiff(unsupported)), 0U, CancellationToken{}),
        ErrorCode::kUnsupported, "unsupported_tiff_compression", "memory");

    unsupported.compression = kCompressionNone;
    unsupported.photometric = 5U;
    expect_tiff_error(
        decoder.decode_memory(vector_bytes(build_tiff(unsupported)), 0U, CancellationToken{}),
        ErrorCode::kUnsupported, "unsupported_tiff_photometric", "memory");

    TiffOptions corrupt;
    corrupt.compression = kCompressionDeflate;
    corrupt.pixels = rgb8_pixels();
    QByteArray corrupt_pixels = build_tiff(corrupt);
    corrupt_pixels[corrupt_pixels.size() - 1] =
        static_cast<char>(corrupt_pixels.back() ^ static_cast<char>(0x7F));
    expect_tiff_error(decoder.decode_memory(vector_bytes(corrupt_pixels), 0U, CancellationToken{}),
                      ErrorCode::kValidation, "tiff_pixel_decode_failed", "memory");

    TiffOptions truncated_options;
    truncated_options.pixels = rgb8_pixels();
    QByteArray truncated = build_tiff(truncated_options);
    truncated.chop(2);
    expect_tiff_error(decoder.decode_memory(vector_bytes(truncated), 0U, CancellationToken{}),
                      ErrorCode::kValidation, "truncated_tiff_strip_data", "memory");

    const std::vector<std::uint8_t> random{'n', 'o', 't', '-', 't', 'i', 'f', 'f'};
    const auto random_result = decoder.decode_memory(random, 0U, CancellationToken{});
    ASSERT_FALSE(random_result);
    EXPECT_EQ(random_result.error().code, ErrorCode::kUnsupported);
}

TEST(TiffAdapterTest, ProbeFullyValidatesPixelsBeforeAtomicPublication)
{
    TiffOptions corrupt;
    corrupt.compression = kCompressionDeflate;
    corrupt.pixels = rgb8_pixels();
    QByteArray encoded = build_tiff(corrupt);
    encoded[encoded.size() - 1] = static_cast<char>(encoded.back() ^ static_cast<char>(0x7F));
    TiffTempDirectory temporary;
    const auto path = temporary.path() / "corrupt.tif";
    write_file(path, encoded);
    QtRasterDecoder decoder;

    // RasterDecoder::probe has no cancellation token. Like strict PNG import,
    // TIFF pays a second decode so Catalog cannot publish before pixels validate.
    expect_tiff_error(decoder.probe(path.string()), ErrorCode::kValidation,
                      "tiff_pixel_decode_failed", path.string());
    expect_tiff_error(decoder.decode(path.string(), 0U, CancellationToken{}),
                      ErrorCode::kValidation, "tiff_pixel_decode_failed", path.string());
}

TEST(TiffAdapterTest, RejectsUnsafeDimensionsAndHonorsCancellationWithoutMutation)
{
    QtRasterDecoder decoder;
    TiffOptions unsafe;
    unsafe.width = 10000U;
    unsafe.height = 8000U;
    unsafe.bits = 16U;
    unsafe.pixels = QByteArray("bad", 3);
    const auto unsafe_result =
        decoder.decode_memory(vector_bytes(build_tiff(unsafe)), 0U, CancellationToken{});
    expect_tiff_error(unsafe_result, ErrorCode::kValidation,
                      "tiff_dimensions_exceed_allocation_limit", "memory");
    ASSERT_TRUE(unsafe_result.error().context.contains("native_bytes_per_pixel"));
    EXPECT_EQ(unsafe_result.error().context.at("native_bytes_per_pixel"), "8");

    TiffOptions large;
    large.pixels = rgb8_pixels();
    large.filler = QByteArray(16 * 1024 * 1024, 'x');
    const QByteArray encoded = build_tiff(large);
    std::vector<std::uint8_t> memory = vector_bytes(encoded);
    const QByteArray before = hash(memory);
    CancellationSource cancelled;
    ASSERT_TRUE(cancelled.cancel("test"));
    const auto memory_cancelled = decoder.decode_memory(memory, 0U, cancelled.token());
    ASSERT_FALSE(memory_cancelled);
    EXPECT_EQ(memory_cancelled.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(hash(memory), before);

    TiffTempDirectory temporary;
    const auto path = temporary.path() / "large.tif";
    write_file(path, encoded);
    const QByteArray file_before = hash(read_file(path));
    const CancellationSource deadline = CancellationSource::with_deadline(
        std::chrono::steady_clock::now() + std::chrono::milliseconds(1));
    const auto file_cancelled = decoder.decode(path.string(), 0U, deadline.token());
    ASSERT_FALSE(file_cancelled);
    EXPECT_EQ(file_cancelled.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(hash(read_file(path)), file_before);
}

TEST(TiffAdapterTest, DistinguishesPathFailures)
{
    TiffTempDirectory temporary;
    QtRasterDecoder decoder;
    const auto empty = decoder.probe("");
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().code, ErrorCode::kInvalidArgument);
    const auto missing = decoder.probe((temporary.path() / "missing.tif").string());
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
