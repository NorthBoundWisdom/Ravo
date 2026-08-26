#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
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

} // namespace
} // namespace ravo
