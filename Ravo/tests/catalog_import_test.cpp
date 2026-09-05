#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include <zlib.h>

#include <QBuffer>
#include <QByteArray>
#include <QByteArrayView>
#include <QColor>
#include <QColorSpace>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QImage>
#include <QIODevice>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <gtest/gtest.h>

#include "ravo/adapters/filesystem_preview_cache.h"
#include "ravo/adapters/filesystem_recovery_store.h"
#include "ravo/adapters/crs_xmp.h"
#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/adapters/text_file.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/json.h"
#include "ravo/foundation/log.h"
#include "ravo/recipe/color_contrast.h"
#include "ravo/recipe/color_correction.h"
#include "ravo/recipe/color_harmonizer.h"
#include "ravo/recipe/color_reconstruction.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/develop_mask.h"
#include "ravo/recipe/dehaze.h"
#include "ravo/recipe/profile_gamma.h"
#include "ravo/recipe/primaries.h"
#include "ravo/recipe/sharpen.h"
#include "ravo/recipe/texture.h"
#include "ravo/services/catalog_service.h"

#include "capture_metadata_test_support.h"
#include "catalog_test_support.h"
#include "catalog_service_test_support.h"
#include "color_balance_fixture.h"
#include "catalog_repository_test_control.h"
#include "recovery_publication_internal.h"
#include "temperature_fixture.h"

namespace ravo
{
namespace
{

[[nodiscard]] std::vector<std::uint8_t> make_synthetic_capture_exif_tiff()
{
    return {
        0x49U, 0x49U, 0x2AU, 0x00U, 0x08U, 0x00U, 0x00U, 0x00U, 0x02U, 0x00U, 0x69U, 0x87U, 0x04U,
        0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x26U, 0x00U, 0x00U, 0x00U, 0x25U, 0x88U, 0x04U, 0x00U,
        0x01U, 0x00U, 0x00U, 0x00U, 0x6CU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x03U,
        0x00U, 0x03U, 0x90U, 0x02U, 0x00U, 0x14U, 0x00U, 0x00U, 0x00U, 0x50U, 0x00U, 0x00U, 0x00U,
        0x11U, 0x90U, 0x02U, 0x00U, 0x07U, 0x00U, 0x00U, 0x00U, 0x64U, 0x00U, 0x00U, 0x00U, 0x91U,
        0x92U, 0x02U, 0x00U, 0x03U, 0x00U, 0x00U, 0x00U, 0x31U, 0x38U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x32U, 0x30U, 0x30U, 0x37U, 0x3AU, 0x30U, 0x39U, 0x3AU, 0x31U, 0x31U, 0x20U,
        0x31U, 0x33U, 0x3AU, 0x35U, 0x33U, 0x3AU, 0x33U, 0x33U, 0x00U, 0x2BU, 0x30U, 0x32U, 0x3AU,
        0x30U, 0x30U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x04U, 0x00U, 0x00U,
        0x00U, 0x02U, 0x03U, 0x00U, 0x00U, 0x01U, 0x00U, 0x02U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U,
        0x4EU, 0x00U, 0x00U, 0x00U, 0x02U, 0x00U, 0x05U, 0x00U, 0x03U, 0x00U, 0x00U, 0x00U, 0xC6U,
        0x00U, 0x00U, 0x00U, 0x03U, 0x00U, 0x02U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x45U, 0x00U,
        0x00U, 0x00U, 0x04U, 0x00U, 0x05U, 0x00U, 0x03U, 0x00U, 0x00U, 0x00U, 0xDEU, 0x00U, 0x00U,
        0x00U, 0x05U, 0x00U, 0x01U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x06U, 0x00U, 0x05U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0xF6U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x31U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x0FU, 0x00U,
        0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0xDFU, 0x71U, 0x00U, 0x00U, 0xC4U, 0x09U, 0x00U,
        0x00U, 0x03U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x03U, 0x00U, 0x00U, 0x00U,
        0x01U, 0x00U, 0x00U, 0x00U, 0xEEU, 0x1AU, 0x00U, 0x00U, 0xC4U, 0x09U, 0x00U, 0x00U, 0x48U,
        0x3CU, 0x00U, 0x00U, 0x7DU, 0x00U, 0x00U, 0x00U};
}

[[nodiscard]] std::string write_synthetic_jpeg_with_capture(const std::filesystem::path &path)
{
    QImage image(8, 8, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(20, 40, 60));
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    EXPECT_TRUE(image.save(&buffer, "JPEG", 90));
    EXPECT_GE(jpeg.size(), 2);
    EXPECT_EQ(static_cast<unsigned char>(jpeg[0]), 0xFF);
    EXPECT_EQ(static_cast<unsigned char>(jpeg[1]), 0xD8);
    const auto tiff = make_synthetic_capture_exif_tiff();
    QByteArray app1;
    app1.append(static_cast<char>(0xFF));
    app1.append(static_cast<char>(0xE1));
    const auto payload = 2 + 6 + static_cast<int>(tiff.size());
    app1.append(static_cast<char>((payload >> 8) & 0xFF));
    app1.append(static_cast<char>(payload & 0xFF));
    app1.append("Exif", 4);
    app1.append('\0');
    app1.append('\0');
    app1.append(reinterpret_cast<const char *>(tiff.data()), static_cast<int>(tiff.size()));
    jpeg.insert(2, app1);
    QFile file(QString::fromStdString(path.string()));
    EXPECT_TRUE(file.open(QIODevice::WriteOnly));
    EXPECT_EQ(file.write(jpeg), jpeg.size());
    file.close();
    return path.string();
}

TEST_F(CatalogServiceTest, CancellationAfterExifReadPreventsPublication)
{
    ASSERT_TRUE(open_service(true));
    auto before = service->snapshot();
    ASSERT_TRUE(before);
    const auto jpeg_path = write_synthetic_jpeg_with_capture(root / "cancel-after-exif.jpg");
    CancellationSource cancellation;
    testing::CatalogServiceTestControl::set_before_import_publication(
        *service, [&cancellation] { EXPECT_TRUE(cancellation.cancel("after-exif")); });
    auto imported = service->import_one(jpeg_path, cancellation.token());
    ASSERT_TRUE(imported);
    EXPECT_EQ(imported.value().status, ImportItemStatus::kFailed);
    ASSERT_TRUE(imported.value().error);
    EXPECT_EQ(imported.value().error->code, ErrorCode::kCancelled);
    auto assets = service->list_assets();
    ASSERT_TRUE(assets);
    EXPECT_TRUE(assets.value().empty());
    auto after = service->snapshot();
    ASSERT_TRUE(after);
    EXPECT_EQ(after.value().revision, before.value().revision);
}

[[nodiscard]] std::string write_png_with_exif_payload(const std::filesystem::path &path,
                                                      const std::vector<std::uint8_t> &tiff,
                                                      const bool bad_crc = false,
                                                      const bool duplicate = false,
                                                      const bool empty = false,
                                                      const bool jpeg_prefix = false)
{
    QImage image(8, 8, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(10, 20, 30));
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    EXPECT_TRUE(image.save(&buffer, "PNG"));
    std::vector<std::uint8_t> payload;
    if (jpeg_prefix)
    {
        payload.insert(payload.end(), {'E', 'x', 'i', 'f', 0, 0});
    }
    if (!empty)
    {
        payload.insert(payload.end(), tiff.begin(), tiff.end());
    }
    const auto append_exif = [&](QByteArray &out)
    {
        const auto length = static_cast<std::uint32_t>(payload.size());
        unsigned char header[8] = {static_cast<unsigned char>(length >> 24U),
                                   static_cast<unsigned char>(length >> 16U),
                                   static_cast<unsigned char>(length >> 8U),
                                   static_cast<unsigned char>(length),
                                   'e',
                                   'X',
                                   'I',
                                   'f'};
        out.append(reinterpret_cast<const char *>(header), 8);
        if (!payload.empty())
        {
            out.append(reinterpret_cast<const char *>(payload.data()),
                       static_cast<int>(payload.size()));
        }
        uLong crc = crc32(0L, Z_NULL, 0);
        crc = crc32(crc, reinterpret_cast<const Bytef *>("eXIf"), 4);
        if (!payload.empty())
        {
            crc = crc32(crc, payload.data(), static_cast<uInt>(payload.size()));
        }
        auto stored = static_cast<std::uint32_t>(crc);
        if (bad_crc)
        {
            stored ^= 1U;
        }
        const unsigned char crc_bytes[4] = {
            static_cast<unsigned char>(stored >> 24U), static_cast<unsigned char>(stored >> 16U),
            static_cast<unsigned char>(stored >> 8U), static_cast<unsigned char>(stored)};
        out.append(reinterpret_cast<const char *>(crc_bytes), 4);
    };
    QByteArray rebuilt;
    rebuilt.append(png.left(8));
    qsizetype offset = 8;
    bool inserted = false;
    while (offset + 12 <= png.size())
    {
        const auto length =
            (static_cast<std::uint32_t>(static_cast<unsigned char>(png[offset])) << 24U) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(png[offset + 1])) << 16U) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(png[offset + 2])) << 8U) |
            static_cast<std::uint32_t>(static_cast<unsigned char>(png[offset + 3]));
        const QByteArray type = png.mid(offset + 4, 4);
        if (!inserted && type == "IDAT")
        {
            append_exif(rebuilt);
            if (duplicate)
            {
                append_exif(rebuilt);
            }
            inserted = true;
        }
        rebuilt.append(png.mid(offset, 12 + static_cast<qsizetype>(length)));
        offset += 12 + static_cast<qsizetype>(length);
    }
    QFile file(QString::fromStdString(path.string()));
    EXPECT_TRUE(file.open(QIODevice::WriteOnly));
    EXPECT_EQ(file.write(rebuilt), rebuilt.size());
    file.close();
    return path.string();
}

TEST_F(CatalogServiceTest, ImportsPngAndTiffCaptureContainersIndependently)
{
    ASSERT_TRUE(open_service(true));
    const auto tiff = make_synthetic_capture_exif_tiff();
    const auto png_path = write_png_with_exif_payload(root / "located.png", tiff);
    const auto png_hash = file_sha256(png_path);
    auto png_imported = service->import_one(png_path, CancellationToken{});
    ASSERT_TRUE(png_imported) << png_imported.error().message;
    ASSERT_TRUE(png_imported.value().asset);
    ASSERT_TRUE(png_imported.value().asset->capture.location);
    EXPECT_EQ(png_imported.value().asset->capture.location->latitude_e6, 49253239);
    ASSERT_TRUE(png_imported.value().asset->capture.location->altitude);
    EXPECT_EQ(png_imported.value().asset->capture.location->altitude->magnitude_mm, 123456U);
    EXPECT_EQ(file_sha256(png_path), png_hash);

    const auto standalone_tiff = root / "independent-located.tif";
    const auto tiff_bytes = test_support::make_capture_exif_tiff();
    {
        std::ofstream output(standalone_tiff, std::ios::binary);
        output.write(reinterpret_cast<const char *>(tiff_bytes.data()),
                     static_cast<std::streamsize>(tiff_bytes.size()));
    }
    const auto tiff_hash = file_sha256(standalone_tiff.string());
    auto tiff_imported = service->import_one(standalone_tiff.string(), CancellationToken{});
    ASSERT_TRUE(tiff_imported) << tiff_imported.error().message;
    ASSERT_TRUE(tiff_imported.value().asset)
        << (tiff_imported.value().error ? tiff_imported.value().error->message : "");
    ASSERT_TRUE(tiff_imported.value().asset->capture.location);
    EXPECT_EQ(tiff_imported.value().asset->capture.location->longitude_e6, 3050766);
    EXPECT_EQ(file_sha256(standalone_tiff.string()), tiff_hash);

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto listed = service->list_assets();
    ASSERT_TRUE(listed);
    EXPECT_EQ(listed.value().size(), 2U);
}

[[nodiscard]] std::string write_truncated_raster(const std::filesystem::path &path,
                                                 const char *format)
{
    QImage image(12, 8, QImage::Format_RGB888);
    image.fill(QColor(40, 80, 120));
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    QByteArray encoded;
    QBuffer buffer(&encoded);
    EXPECT_TRUE(buffer.open(QIODevice::WriteOnly));
    EXPECT_TRUE(image.save(&buffer, format));
    EXPECT_GT(encoded.size(), 16);
    encoded.chop(8);
    QFile file(QString::fromStdString(path.string()));
    EXPECT_TRUE(file.open(QIODevice::WriteOnly));
    EXPECT_EQ(file.write(encoded), encoded.size());
    return path.string();
}

[[nodiscard]] std::string write_float_rgb_tiff(const std::filesystem::path &path)
{
    QByteArray encoded;
    const auto append_u16 = [&](const std::uint16_t value)
    {
        encoded.append(static_cast<char>(value & 0xFFU));
        encoded.append(static_cast<char>((value >> 8U) & 0xFFU));
    };
    const auto append_u32 = [&](const std::uint32_t value)
    {
        encoded.append(static_cast<char>(value & 0xFFU));
        encoded.append(static_cast<char>((value >> 8U) & 0xFFU));
        encoded.append(static_cast<char>((value >> 16U) & 0xFFU));
        encoded.append(static_cast<char>((value >> 24U) & 0xFFU));
    };
    encoded.append("II*\0", 4);
    append_u32(8U);
    append_u16(11U);
    const auto entry = [&](const std::uint16_t tag, const std::uint16_t type,
                           const std::uint32_t count, const std::uint32_t value)
    {
        append_u16(tag);
        append_u16(type);
        append_u32(count);
        append_u32(value);
    };
    entry(256U, 4U, 1U, 1U);
    entry(257U, 4U, 1U, 1U);
    entry(258U, 3U, 3U, 146U);
    entry(259U, 3U, 1U, 1U);
    entry(262U, 3U, 1U, 2U);
    entry(273U, 4U, 1U, 158U);
    entry(277U, 3U, 1U, 3U);
    entry(278U, 4U, 1U, 1U);
    entry(279U, 4U, 1U, 12U);
    entry(284U, 3U, 1U, 1U);
    entry(339U, 3U, 3U, 152U);
    append_u32(0U);
    append_u16(32U);
    append_u16(32U);
    append_u16(32U);
    append_u16(3U);
    append_u16(3U);
    append_u16(3U);
    encoded.append(12, '\0');
    QFile file(QString::fromStdString(path.string()));
    EXPECT_TRUE(file.open(QIODevice::WriteOnly));
    EXPECT_EQ(file.write(encoded), encoded.size());
    return path.string();
}

TEST_F(CatalogServiceTest, CorruptPngAndTiffNeverPublishAnAssetOrPreview)
{
    ASSERT_TRUE(open_service(true));
    const auto png_path = write_truncated_raster(root / "truncated.png", "PNG");
    const auto png_hash = file_sha256(png_path);
    auto png_imported = service->import_one(png_path, CancellationToken{});
    ASSERT_TRUE(png_imported) << png_imported.error().message;
    EXPECT_EQ(png_imported.value().status, ImportItemStatus::kFailed);
    EXPECT_FALSE(png_imported.value().asset);
    EXPECT_FALSE(png_imported.value().preview_cache_path);
    ASSERT_TRUE(png_imported.value().error);
    EXPECT_EQ(png_imported.value().error->code, ErrorCode::kValidation);
    EXPECT_EQ(png_imported.value().error->context.at("format"), "png");
    EXPECT_EQ(file_sha256(png_path), png_hash);

    const auto tiff_path = write_truncated_raster(root / "truncated.tif", "TIFF");
    const auto tiff_hash = file_sha256(tiff_path);
    auto tiff_imported = service->import_one(tiff_path, CancellationToken{});
    ASSERT_TRUE(tiff_imported) << tiff_imported.error().message;
    EXPECT_EQ(tiff_imported.value().status, ImportItemStatus::kFailed);
    EXPECT_FALSE(tiff_imported.value().asset);
    EXPECT_FALSE(tiff_imported.value().preview_cache_path);
    ASSERT_TRUE(tiff_imported.value().error);
    EXPECT_EQ(tiff_imported.value().error->code, ErrorCode::kValidation);
    EXPECT_EQ(tiff_imported.value().error->context.at("format"), "tiff");
    EXPECT_EQ(file_sha256(tiff_path), tiff_hash);

    auto assets = service->list_assets();
    ASSERT_TRUE(assets) << assets.error().message;
    EXPECT_TRUE(assets.value().empty());
    auto previews = service->list_previews();
    ASSERT_TRUE(previews) << previews.error().message;
    EXPECT_TRUE(previews.value().empty());
}

TEST_F(CatalogServiceTest, RecognizedTiffLayoutsDoNotStealRawRouting)
{
    ASSERT_TRUE(open_service(true));
    const auto float_path = write_float_rgb_tiff(root / "float.tif");
    const auto float_hash = file_sha256(float_path);
    auto floating = service->import_one(float_path, CancellationToken{});
    ASSERT_TRUE(floating) << floating.error().message;
    EXPECT_EQ(floating.value().status, ImportItemStatus::kUnsupported);
    EXPECT_FALSE(floating.value().asset);
    ASSERT_TRUE(floating.value().error);
    EXPECT_EQ(floating.value().error->code, ErrorCode::kUnsupported);
    EXPECT_EQ(floating.value().error->context.at("format"), "tiff");
    EXPECT_EQ(floating.value().error->context.at("reason"), "unsupported_tiff_float_samples");
    EXPECT_EQ(file_sha256(float_path), float_hash);

    const auto arw = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" / "fixtures" /
                     "frozen" / "images" / "hlrecovery.arw";
    const auto disguised = root / "camera.tif";
    std::filesystem::copy_file(arw, disguised);
    const auto disguised_hash = file_sha256(disguised.string());
    auto imported = service->import_one(disguised.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset)
        << (imported.value().error ? imported.value().error->message : "");
    EXPECT_EQ(imported.value().status, ImportItemStatus::kImported);
    EXPECT_EQ(imported.value().asset->media_type, kMediaTypeRaw);
    EXPECT_EQ(file_sha256(disguised.string()), disguised_hash);

    auto assets = service->list_assets();
    ASSERT_TRUE(assets) << assets.error().message;
    ASSERT_EQ(assets.value().size(), 1U);
    EXPECT_EQ(assets.value().front().media_type, kMediaTypeRaw);
}

TEST_F(CatalogServiceTest, CorruptAndUnrecognizedRawNeverPublishAnAsset)
{
    ASSERT_TRUE(open_service(true));
    const auto missing = service->import_one((root / "missing.cr2").string(), CancellationToken{});
    ASSERT_TRUE(missing) << missing.error().message;
    EXPECT_EQ(missing.value().status, ImportItemStatus::kFailed);
    EXPECT_FALSE(missing.value().asset);
    ASSERT_TRUE(missing.value().error);
    EXPECT_EQ(missing.value().error->code, ErrorCode::kNotFound);

    const auto garbage_path = root / "notes.cr2";
    {
        std::ofstream output(garbage_path, std::ios::binary);
        output << "not a camera raw";
    }
    const auto garbage_hash = file_sha256(garbage_path.string());
    auto garbage = service->import_one(garbage_path.string(), CancellationToken{});
    ASSERT_TRUE(garbage) << garbage.error().message;
    EXPECT_TRUE(garbage.value().status == ImportItemStatus::kUnsupported ||
                garbage.value().status == ImportItemStatus::kFailed);
    EXPECT_FALSE(garbage.value().asset);
    EXPECT_EQ(file_sha256(garbage_path.string()), garbage_hash);

    const auto truncated_path = root / "truncated.cr2";
    {
        std::ifstream input(std::filesystem::path(raw_fixture_path()), std::ios::binary);
        std::ofstream output(truncated_path, std::ios::binary);
        std::vector<char> prefix(1024);
        input.read(prefix.data(), static_cast<std::streamsize>(prefix.size()));
        output.write(prefix.data(), input.gcount());
    }
    const auto truncated_hash = file_sha256(truncated_path.string());
    auto truncated = service->import_one(truncated_path.string(), CancellationToken{});
    ASSERT_TRUE(truncated) << truncated.error().message;
    EXPECT_TRUE(truncated.value().status == ImportItemStatus::kUnsupported ||
                truncated.value().status == ImportItemStatus::kFailed);
    EXPECT_FALSE(truncated.value().asset);
    EXPECT_EQ(file_sha256(truncated_path.string()), truncated_hash);

    auto assets = service->list_assets();
    ASSERT_TRUE(assets) << assets.error().message;
    EXPECT_TRUE(assets.value().empty());
}

TEST_F(CatalogServiceTest, DngSuffixImportsAsRawWithoutRewritingTheSource)
{
    ASSERT_TRUE(open_service(true));
    const auto dng_path = root / "camera.dng";
    std::filesystem::copy_file(raw_fixture_path(), dng_path);
    const auto dng_hash = file_sha256(dng_path.string());
    auto imported = service->import_one(dng_path.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset)
        << (imported.value().error ? imported.value().error->message : "");
    EXPECT_EQ(imported.value().status, ImportItemStatus::kImported);
    EXPECT_EQ(imported.value().asset->media_type, kMediaTypeRaw);
    EXPECT_GT(imported.value().asset->width.value_or(0), 0U);
    EXPECT_GT(imported.value().asset->height.value_or(0), 0U);
    EXPECT_EQ(file_sha256(dng_path.string()), dng_hash);
    ASSERT_TRUE(imported.value().preview_cache_path);
    EXPECT_TRUE(std::filesystem::exists(*imported.value().preview_cache_path));
}

TEST_F(CatalogServiceTest, XTransImportsAndPublishesAnEngineRenderedPreview)
{
    ASSERT_TRUE(open_service(true));
    const auto path = xtrans_fixture_path();
    const auto hash = file_sha256(path);
    auto decoded = engine.decode_raw_frame(path, CancellationToken{});
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value().cfa_width, 6U);
    EXPECT_EQ(decoded.value().cfa_height, 6U);

    auto imported = service->import_one(path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    EXPECT_EQ(file_sha256(path), hash);
    ASSERT_TRUE(imported.value().asset)
        << (imported.value().error ? imported.value().error->message : "no item error")
        << " status=" << static_cast<int>(imported.value().status);
    EXPECT_EQ(imported.value().status, ImportItemStatus::kImported);
    EXPECT_EQ(imported.value().asset->media_type, kMediaTypeRaw);
    ASSERT_TRUE(imported.value().preview_cache_path);
    EXPECT_TRUE(std::filesystem::exists(*imported.value().preview_cache_path));

    PreviewRequest request;
    request.asset_id = imported.value().asset->id;
    request.max_edge = 320U;
    auto preview = service->request_preview(request);
    ASSERT_TRUE(preview) << preview.error().message;
    EXPECT_EQ(preview.value().width, 320U);
    EXPECT_GT(preview.value().height, 0U);
    EXPECT_TRUE(std::filesystem::exists(preview.value().cache_path));
}

TEST_F(CatalogServiceTest, CancellationAfterRawInspectPreventsPublication)
{
    ASSERT_TRUE(open_service(true));
    auto before = service->snapshot();
    ASSERT_TRUE(before);
    CancellationSource cancellation;
    testing::CatalogServiceTestControl::set_before_import_publication(
        *service, [&cancellation] { EXPECT_TRUE(cancellation.cancel("after-raw-inspect")); });
    auto imported = service->import_one(raw_fixture_path(), cancellation.token());
    ASSERT_TRUE(imported);
    EXPECT_EQ(imported.value().status, ImportItemStatus::kFailed);
    ASSERT_TRUE(imported.value().error);
    EXPECT_EQ(imported.value().error->code, ErrorCode::kCancelled);
    auto assets = service->list_assets();
    ASSERT_TRUE(assets);
    EXPECT_TRUE(assets.value().empty());
    auto after = service->snapshot();
    ASSERT_TRUE(after);
    EXPECT_EQ(after.value().revision, before.value().revision);
}

TEST_F(CatalogServiceTest, CancellationBeforePreviewCacheCommitPublishesNoFileOrRecord)
{
    ASSERT_TRUE(open_service(true));
    const auto jpeg_path = (root / "cancel-preview-cache.jpg").string();
    QImage image(24, 16, QImage::Format_RGB888);
    image.fill(QColor(10, 20, 30));
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 95));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    auto before = service->list_previews();
    ASSERT_TRUE(before) << before.error().message;

    const auto count_cache_files = [this]
    {
        std::size_t count = 0;
        const std::filesystem::path cache_root(database_path + ".preview");
        for (const auto &entry : std::filesystem::directory_iterator(cache_root))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".png")
                ++count;
        }
        return count;
    };
    const auto files_before = count_cache_files();
    CancellationSource cancellation;
    bool hook_called = false;
    testing::CatalogServiceTestControl::set_before_preview_cache_publication(
        *service,
        [&]
        {
            hook_called = true;
            EXPECT_TRUE(cancellation.cancel("before-preview-cache-commit"));
        });
    PreviewRequest request;
    request.asset_id = imported.value().asset->id;
    request.max_edge = 11;
    request.cancellation = cancellation.token();
    auto preview = service->request_preview(request);
    ASSERT_FALSE(preview);
    EXPECT_TRUE(hook_called);
    EXPECT_EQ(preview.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(preview.error().context.at("reason"), "before-preview-cache-commit");
    EXPECT_EQ(count_cache_files(), files_before);
    auto after = service->list_previews();
    ASSERT_TRUE(after) << after.error().message;
    EXPECT_EQ(after.value().size(), before.value().size());
}

TEST_F(CatalogServiceTest, CloseAndCorruptCacheStillAllowReopenPreview)
{
    ASSERT_TRUE(open_service(true));
    const auto jpeg_path = (root / "cached.jpg").string();
    QImage image(24, 16, QImage::Format_RGB888);
    image.fill(QColor(10, 20, 30));
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 95));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    ASSERT_TRUE(imported.value().preview_cache_path);
    const auto cache_path = *imported.value().preview_cache_path;
    EXPECT_TRUE(std::filesystem::exists(cache_path));

    {
        QFile corrupt(QString::fromStdString(cache_path));
        ASSERT_TRUE(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_EQ(corrupt.write("not-a-png", 9), 9);
    }
    PreviewRequest request;
    request.asset_id = asset_id;
    request.max_edge = kThumbnailMaxEdge;
    auto rebuilt = service->request_preview(request);
    ASSERT_TRUE(rebuilt) << rebuilt.error().message;
    EXPECT_TRUE(std::filesystem::exists(rebuilt.value().cache_path));
    QFile rebuilt_file(QString::fromStdString(rebuilt.value().cache_path));
    ASSERT_TRUE(rebuilt_file.open(QIODevice::ReadOnly));
    EXPECT_EQ(rebuilt_file.read(8), QByteArray("\x89PNG\r\n\x1a\n", 8));

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    PreviewRequest after_close;
    after_close.asset_id = asset_id;
    after_close.max_edge = kThumbnailMaxEdge;
    auto reopened = service->request_preview(after_close);
    ASSERT_TRUE(reopened) << reopened.error().message;
    EXPECT_TRUE(std::filesystem::exists(reopened.value().cache_path));
}

TEST_F(CatalogServiceTest, ExportsLocatedCaptureThroughJpegPngTiffDeterministically)
{
    ASSERT_TRUE(open_service(true));
    const auto jpeg_path = write_synthetic_jpeg_with_capture(root / "export-source.jpg");
    const auto source_hash = file_sha256(jpeg_path);
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto asset_id = imported.value().asset->id;

    const auto parse_has = [](const std::string &path, const char *needle)
    {
        QFile file(QString::fromStdString(path));
        EXPECT_TRUE(file.open(QIODevice::ReadOnly));
        const auto bytes = file.readAll();
        return bytes.contains(needle);
    };
    const auto verify_embedded_capture = [&](const std::string &path)
    {
        auto capture = engine.read_embedded_capture_metadata(path, CancellationToken{});
        ASSERT_TRUE(capture) << capture.error().message;
        ASSERT_TRUE(capture.value().captured_datetime);
        EXPECT_EQ(capture.value().captured_datetime->local_exif, "2007:09:11 13:53:33");
        EXPECT_EQ(capture.value().captured_datetime->subsecond_digits, "18");
        EXPECT_EQ(capture.value().captured_datetime->utc_offset_minutes, 120);
        ASSERT_TRUE(capture.value().location);
        EXPECT_EQ(capture.value().location->latitude_e6, 49253239);
        EXPECT_EQ(capture.value().location->longitude_e6, 3050766);
        ASSERT_TRUE(capture.value().location->altitude);
        EXPECT_EQ(capture.value().location->altitude->magnitude_mm, 123456U);
        EXPECT_EQ(capture.value().location->altitude->reference,
                  EngineCaptureAltitudeReference::kAboveSeaLevel);
    };

    ExportRequest jpeg;
    jpeg.asset_id = asset_id;
    jpeg.output_path = (root / "located-out.jpg").string();
    jpeg.format = ExportFormat::kJpeg;
    auto first = service->export_asset(jpeg);
    ASSERT_TRUE(first) << first.error().message;
    ExportRequest second_request = jpeg;
    second_request.output_path = (root / "located-out-2.jpg").string();
    auto second = service->export_asset(second_request);
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_EQ(file_sha256(jpeg.output_path), file_sha256(second.value().output_path));
    verify_embedded_capture(jpeg.output_path);
    EXPECT_TRUE(parse_has(jpeg.output_path, "2007:09:11 13:53:33"));
    EXPECT_TRUE(parse_has(jpeg.output_path, "2007-09-11T13:53:33.18+02:00"));

    ExportRequest png;
    png.asset_id = asset_id;
    png.output_path = (root / "located-out.png").string();
    png.format = ExportFormat::kPng;
    auto png_out = service->export_asset(png);
    ASSERT_TRUE(png_out) << png_out.error().message;
    verify_embedded_capture(png.output_path);
    EXPECT_TRUE(parse_has(png.output_path, "2007:09:11 13:53:33"));

    ExportRequest tiff;
    tiff.asset_id = asset_id;
    tiff.output_path = (root / "located-out.tif").string();
    tiff.format = ExportFormat::kTiff;
    auto tiff_out = service->export_asset(tiff);
    ASSERT_TRUE(tiff_out) << tiff_out.error().message;
    verify_embedded_capture(tiff.output_path);
    EXPECT_TRUE(parse_has(tiff.output_path, "2007:09:11 13:53:33"));
    EXPECT_EQ(file_sha256(jpeg_path), source_hash);
}

TEST_F(CatalogServiceTest, ExportMetadataPrivacyOmitsLocationOrAllPublicPackets)
{
    ASSERT_TRUE(open_service(true));
    const auto source_path = write_synthetic_jpeg_with_capture(root / "privacy-source.jpg");
    auto imported = service->import_one(source_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    WritableMetadata writable;
    writable.title = "PrivacyTitle";
    writable.creator = "PrivacyCreator";
    writable.copyright = "PrivacyCopyright";
    writable.country = "PrivacyCountry";
    writable.province_state = "PrivacyState";
    writable.city = "PrivacyCity";
    writable.sublocation = "PrivacySub";
    ASSERT_TRUE(service->set_writable_metadata(asset_id, writable));

    ExportRequest full;
    full.asset_id = asset_id;
    full.output_path = (root / "privacy-full.jpg").string();
    full.format = ExportFormat::kJpeg;
    full.metadata_mode = ExportMetadataMode::kFull;
    auto full_result = service->export_asset(full);
    ASSERT_TRUE(full_result) << full_result.error().message;
    {
        QFile file(QString::fromStdString(full.output_path));
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        EXPECT_TRUE(bytes.contains("PrivacyTitle"));
        EXPECT_TRUE(bytes.contains("PrivacyCreator"));
        EXPECT_TRUE(bytes.contains("PrivacyCopyright"));
        EXPECT_TRUE(bytes.contains("PrivacyCountry"));
        EXPECT_TRUE(bytes.contains("PrivacyCity"));
        EXPECT_TRUE(bytes.contains("PrivacySub"));
    }

    ExportRequest no_location;
    no_location.asset_id = asset_id;
    no_location.output_path = (root / "privacy-no-location.jpg").string();
    no_location.format = ExportFormat::kJpeg;
    no_location.metadata_mode = ExportMetadataMode::kNoLocation;
    auto no_location_result = service->export_asset(no_location);
    ASSERT_TRUE(no_location_result) << no_location_result.error().message;
    auto no_location_capture =
        engine.read_embedded_capture_metadata(no_location.output_path, CancellationToken{});
    ASSERT_TRUE(no_location_capture) << no_location_capture.error().message;
    EXPECT_TRUE(no_location_capture.value().captured_datetime.has_value());
    EXPECT_FALSE(no_location_capture.value().location.has_value());
    {
        QFile file(QString::fromStdString(no_location.output_path));
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        EXPECT_TRUE(bytes.contains("PrivacyTitle"));
        EXPECT_TRUE(bytes.contains("PrivacyCreator"));
        EXPECT_FALSE(bytes.contains("PrivacyCountry"));
        EXPECT_FALSE(bytes.contains("PrivacyState"));
        EXPECT_FALSE(bytes.contains("PrivacyCity"));
        EXPECT_FALSE(bytes.contains("PrivacySub"));
    }

    const std::array cases{
        std::pair{ExportFormat::kJpeg, std::string("privacy-none.jpg")},
        std::pair{ExportFormat::kPng, std::string("privacy-none.png")},
        std::pair{ExportFormat::kTiff, std::string("privacy-none.tif")},
    };
    for (const auto &[format, name] : cases)
    {
        ExportRequest request;
        request.asset_id = asset_id;
        request.output_path = (root / name).string();
        request.format = format;
        request.metadata_mode = ExportMetadataMode::kNone;
        auto exported = service->export_asset(request);
        ASSERT_TRUE(exported) << name << ": " << exported.error().message;
        QFile file(QString::fromStdString(request.output_path));
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        EXPECT_FALSE(bytes.contains("2007:09:11 13:53:33")) << name;
        EXPECT_FALSE(bytes.contains("PrivacyTitle")) << name;
        EXPECT_FALSE(bytes.contains("PrivacyCreator")) << name;
        EXPECT_FALSE(bytes.contains("PrivacyCopyright")) << name;
        EXPECT_FALSE(bytes.contains("PrivacyCountry")) << name;
        EXPECT_FALSE(bytes.contains("PrivacyCity")) << name;
        EXPECT_FALSE(bytes.contains("http://ns.adobe.com/xap/1.0/")) << name;
        EXPECT_FALSE(bytes.contains("XML:com.adobe.xmp")) << name;
        auto capture =
            engine.read_embedded_capture_metadata(request.output_path, CancellationToken{});
        ASSERT_TRUE(capture) << name << ": " << capture.error().message;
        EXPECT_EQ(capture.value(), EngineCaptureMetadata{}) << name;
    }

    ExportRequest original;
    original.asset_id = asset_id;
    original.output_path = (root / "privacy-original.jpg").string();
    original.format = ExportFormat::kOriginalCopy;
    original.metadata_mode = ExportMetadataMode::kNone;
    const auto rejected = service->export_asset(original);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "metadata_mode_not_applicable");
    EXPECT_FALSE(std::filesystem::exists(original.output_path));
}

TEST(PresetPerformanceProbe, AppliesCrsToWarmRawPreviewWithinRequestedBudget)
{
    const char *catalog_path = std::getenv("RAVO_PRESET_PERF_CATALOG");
    const char *asset_id = std::getenv("RAVO_PRESET_PERF_ASSET_ID");
    const char *preset_path = std::getenv("RAVO_PRESET_PERF_XMP");
    if (catalog_path == nullptr || asset_id == nullptr || preset_path == nullptr)
    {
        GTEST_SKIP() << "set the RAVO_PRESET_PERF_* environment variables";
    }

    auto created_engine = EngineFacade::create_phase1();
    ASSERT_TRUE(created_engine) << created_engine.error().message;
    auto repository = SqliteCatalogRepository::open(catalog_path);
    ASSERT_TRUE(repository) << repository.error().message;
    const auto cache_root = make_catalog_test_temp_root();
    auto cache = FilesystemPreviewCache::create((cache_root / "preview").string());
    ASSERT_TRUE(cache) << cache.error().message;
    auto recovery = FilesystemRecoveryStore::create((cache_root / "recovery").string());
    ASSERT_TRUE(recovery) << recovery.error().message;
    CatalogService measured(created_engine.value(), std::move(repository).value(),
                            std::make_unique<QtRasterDecoder>(), std::move(cache).value(),
                            std::move(recovery).value());

    auto baseline_recipe = measured.load_baseline_recipe(asset_id);
    ASSERT_TRUE(baseline_recipe) << baseline_recipe.error().message;
    auto baseline = develop_from_recipe(baseline_recipe.value());
    ASSERT_TRUE(baseline) << baseline.error().message;
    ASSERT_TRUE(measured.save_develop(asset_id, baseline.value()));

    PreviewRequest request;
    request.asset_id = asset_id;
    request.max_edge = kDefaultPreviewMaxEdge;
    request.persist_preview_record = true;
    request.prefer_embedded_preview = false;
    auto warmed = measured.request_preview(request);
    ASSERT_TRUE(warmed) << warmed.error().message;

    const auto started = std::chrono::steady_clock::now();
    auto text = read_utf8_text_file(preset_path, 8U * 1024U * 1024U);
    ASSERT_TRUE(text) << text.error().message;
    auto imported = import_crs_xmp({text.value(), baseline_recipe.value().asset});
    ASSERT_TRUE(imported) << imported.error().message;
    auto applied = baseline.value();
    apply_crs_look(applied, imported.value().look, imported.value().mask);
    const auto parsed_at = std::chrono::steady_clock::now();
    ASSERT_TRUE(measured.save_develop(asset_id, applied));
    const auto saved_at = std::chrono::steady_clock::now();
    PreviewRequest interactive = request;
    interactive.max_edge = kInteractivePreviewMaxEdge;
    interactive.persist_preview_record = false;
    auto first_preview = measured.request_preview(interactive, applied);
    ASSERT_TRUE(first_preview) << first_preview.error().message;
    const auto first_preview_at = std::chrono::steady_clock::now();
    auto preview = measured.request_preview(request);
    ASSERT_TRUE(preview) << preview.error().message;
    const auto finished = std::chrono::steady_clock::now();
    const auto parse_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(parsed_at - started).count();
    const auto save_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(saved_at - parsed_at).count();
    const auto first_preview_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(first_preview_at - saved_at).count();
    const auto settled_preview_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(finished - first_preview_at).count();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(finished - started);
    std::cerr << "preset_apply_elapsed_ms=" << elapsed.count() << " parse_ms=" << parse_ms
              << " save_ms=" << save_ms << " first_preview_ms=" << first_preview_ms
              << " settled_preview_ms=" << settled_preview_ms << '\n';

    if (const char *budget = std::getenv("RAVO_PRESET_PERF_BUDGET_MS"))
    {
        EXPECT_LT(elapsed.count(), std::stoll(budget));
    }
    if (const char *budget = std::getenv("RAVO_PRESET_PERF_FIRST_PREVIEW_BUDGET_MS"))
    {
        EXPECT_LT(first_preview_ms, std::stoll(budget));
    }
    ASSERT_TRUE(measured.close());
    std::error_code ignored;
    std::filesystem::remove_all(cache_root, ignored);
}

TEST(InteractivePreviewPerformanceProbe, MeasuresWarmExposureSweepWithoutCatalogMutation)
{
    const char *catalog_path = std::getenv("RAVO_INTERACTIVE_PERF_CATALOG");
    const char *asset_id = std::getenv("RAVO_INTERACTIVE_PERF_ASSET_ID");
    if (catalog_path == nullptr || asset_id == nullptr)
    {
        GTEST_SKIP() << "set RAVO_INTERACTIVE_PERF_CATALOG and RAVO_INTERACTIVE_PERF_ASSET_ID";
    }

    const std::uint32_t max_edge =
        std::getenv("RAVO_INTERACTIVE_PERF_MAX_EDGE") != nullptr ?
            static_cast<std::uint32_t>(std::stoul(std::getenv("RAVO_INTERACTIVE_PERF_MAX_EDGE"))) :
            kInteractivePreviewMaxEdge;
    const std::size_t runs =
        std::getenv("RAVO_INTERACTIVE_PERF_RUNS") != nullptr ?
            static_cast<std::size_t>(std::stoul(std::getenv("RAVO_INTERACTIVE_PERF_RUNS"))) :
            9U;
    ASSERT_GT(max_edge, 0U);
    ASSERT_GT(runs, 0U);

    auto created_engine = EngineFacade::create_phase1();
    ASSERT_TRUE(created_engine) << created_engine.error().message;
    auto repository = SqliteCatalogRepository::open(catalog_path);
    ASSERT_TRUE(repository) << repository.error().message;
    const auto cache_root = make_catalog_test_temp_root();
    auto cache = FilesystemPreviewCache::create((cache_root / "preview").string());
    ASSERT_TRUE(cache) << cache.error().message;
    auto recovery = FilesystemRecoveryStore::create((cache_root / "recovery").string());
    ASSERT_TRUE(recovery) << recovery.error().message;
    CatalogService measured(created_engine.value(), std::move(repository).value(),
                            std::make_unique<QtRasterDecoder>(), std::move(cache).value(),
                            std::move(recovery).value());

    auto recipe = measured.load_recipe(asset_id);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto baseline = develop_from_recipe(recipe.value());
    ASSERT_TRUE(baseline) << baseline.error().message;
    PreviewRequest request;
    request.asset_id = asset_id;
    request.max_edge = max_edge;
    request.persist_preview_record = false;
    request.prefer_embedded_preview = false;

    auto warm = measured.request_preview(request, baseline.value());
    ASSERT_TRUE(warm) << warm.error().message;
    ASSERT_FALSE(warm.value().rgb.empty());
    ASSERT_LE(std::max(warm.value().width, warm.value().height), max_edge);
    if (max_edge <= kInteractivePreviewMaxEdge)
    {
        PreviewRequest settled = request;
        settled.max_edge = kDefaultPreviewMaxEdge;
        auto settled_warm = measured.request_preview(settled);
        ASSERT_TRUE(settled_warm) << settled_warm.error().message;
        ASSERT_FALSE(settled_warm.value().rgb.empty());
    }

    std::vector<std::int64_t> render_elapsed_ms;
    std::vector<std::int64_t> visible_elapsed_ms;
    render_elapsed_ms.reserve(runs);
    visible_elapsed_ms.reserve(runs);
    for (std::size_t run = 0; run < runs; ++run)
    {
        auto adjusted = baseline.value();
        const double offset = static_cast<double>(static_cast<int>(run % 7U) - 3) * 0.01;
        adjusted.exposure_ev = std::clamp(adjusted.exposure_ev + offset, -18.0, 18.0);
        const auto started = std::chrono::steady_clock::now();
        auto preview = measured.request_preview(request, adjusted);
        const auto rendered = std::chrono::steady_clock::now();
        ASSERT_TRUE(preview) << preview.error().message;
        ASSERT_FALSE(preview.value().rgb.empty());
        const QImage view(preview.value().rgb.data(), static_cast<int>(preview.value().width),
                          static_cast<int>(preview.value().height),
                          static_cast<int>(preview.value().width * 3U), QImage::Format_RGB888);
        const QImage owned = view.copy();
        RasterBuffer raster;
        raster.width = preview.value().width;
        raster.height = preview.value().height;
        raster.srgb.resize(static_cast<std::size_t>(raster.width) * raster.height * 3U);
        for (std::uint32_t row = 0; row < raster.height; ++row)
        {
            std::copy_n(
                owned.constScanLine(static_cast<int>(row)),
                static_cast<std::size_t>(raster.width) * 3U,
                raster.srgb.begin() +
                    static_cast<std::ptrdiff_t>(static_cast<std::size_t>(row) * raster.width * 3U));
        }
        auto histogram = collect_rgb_histogram(raster);
        ASSERT_TRUE(histogram) << histogram.error().message;
        const auto visible = std::chrono::steady_clock::now();
        render_elapsed_ms.push_back(
            std::chrono::duration_cast<std::chrono::milliseconds>(rendered - started).count());
        visible_elapsed_ms.push_back(
            std::chrono::duration_cast<std::chrono::milliseconds>(visible - started).count());
    }
    std::sort(render_elapsed_ms.begin(), render_elapsed_ms.end());
    std::sort(visible_elapsed_ms.begin(), visible_elapsed_ms.end());
    const auto p90_index = (visible_elapsed_ms.size() * 9U - 1U) / 10U;
    const auto render_median = render_elapsed_ms[render_elapsed_ms.size() / 2U];
    const auto render_p90 = render_elapsed_ms[p90_index];
    const auto visible_median = visible_elapsed_ms[visible_elapsed_ms.size() / 2U];
    const auto visible_p90 = visible_elapsed_ms[p90_index];
    std::cerr << "interactive_preview_edge=" << max_edge << " runs=" << runs
              << " render_min_ms=" << render_elapsed_ms.front()
              << " render_median_ms=" << render_median << " render_p90_ms=" << render_p90
              << " render_max_ms=" << render_elapsed_ms.back()
              << " visible_min_ms=" << visible_elapsed_ms.front()
              << " visible_median_ms=" << visible_median << " visible_p90_ms=" << visible_p90
              << " visible_max_ms=" << visible_elapsed_ms.back() << '\n';
    if (const char *budget = std::getenv("RAVO_INTERACTIVE_PERF_P90_BUDGET_MS"))
    {
        EXPECT_LE(visible_p90, std::stoll(budget));
    }

    ASSERT_TRUE(measured.close());
    std::error_code ignored;
    std::filesystem::remove_all(cache_root, ignored);
}

TEST(PerspectiveInteractivePerformanceProbe, MeasuresWarmManualTransformWithoutCatalogMutation)
{
    const char *catalog_path = std::getenv("RAVO_INTERACTIVE_PERF_CATALOG");
    const char *asset_id = std::getenv("RAVO_INTERACTIVE_PERF_ASSET_ID");
    if (catalog_path == nullptr || asset_id == nullptr)
    {
        GTEST_SKIP() << "set RAVO_INTERACTIVE_PERF_CATALOG and RAVO_INTERACTIVE_PERF_ASSET_ID";
    }

    const std::uint32_t max_edge =
        std::getenv("RAVO_INTERACTIVE_PERF_MAX_EDGE") != nullptr ?
            static_cast<std::uint32_t>(std::stoul(std::getenv("RAVO_INTERACTIVE_PERF_MAX_EDGE"))) :
            kInteractivePreviewMaxEdge;
    const std::size_t runs =
        std::getenv("RAVO_INTERACTIVE_PERF_RUNS") != nullptr ?
            static_cast<std::size_t>(std::stoul(std::getenv("RAVO_INTERACTIVE_PERF_RUNS"))) :
            9U;
    ASSERT_GT(max_edge, 0U);
    ASSERT_GT(runs, 0U);

    auto created_engine = EngineFacade::create_phase1();
    ASSERT_TRUE(created_engine) << created_engine.error().message;
    auto repository = SqliteCatalogRepository::open(catalog_path);
    ASSERT_TRUE(repository) << repository.error().message;
    const auto cache_root = make_catalog_test_temp_root();
    auto cache = FilesystemPreviewCache::create((cache_root / "preview").string());
    ASSERT_TRUE(cache) << cache.error().message;
    auto recovery = FilesystemRecoveryStore::create((cache_root / "recovery").string());
    ASSERT_TRUE(recovery) << recovery.error().message;
    CatalogService measured(created_engine.value(), std::move(repository).value(),
                            std::make_unique<QtRasterDecoder>(), std::move(cache).value(),
                            std::move(recovery).value());

    auto recipe = measured.load_recipe(asset_id);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto baseline = develop_from_recipe(recipe.value());
    ASSERT_TRUE(baseline) << baseline.error().message;
    PreviewRequest request;
    request.asset_id = asset_id;
    request.max_edge = max_edge;
    request.persist_preview_record = false;
    request.prefer_embedded_preview = false;
    auto warm = measured.request_preview(request, baseline.value());
    ASSERT_TRUE(warm) << warm.error().message;
    ASSERT_FALSE(warm.value().rgb.empty());

    std::vector<std::int64_t> render_elapsed_ms;
    std::vector<std::int64_t> visible_elapsed_ms;
    render_elapsed_ms.reserve(runs);
    visible_elapsed_ms.reserve(runs);
    for (std::size_t run = 0U; run < runs; ++run)
    {
        auto adjusted = baseline.value();
        const double offset = static_cast<double>(static_cast<int>(run % 9U) - 4) * 0.015;
        adjusted.perspective_vertical = std::clamp(adjusted.perspective_vertical + offset,
                                                   kPerspectiveShiftMin, kPerspectiveShiftMax);
        adjusted.perspective_constrain_crop = true;
        adjusted.perspective_interpolation_index = 2;
        const auto started = std::chrono::steady_clock::now();
        auto preview = measured.request_preview(request, adjusted);
        const auto rendered = std::chrono::steady_clock::now();
        ASSERT_TRUE(preview) << preview.error().message;
        ASSERT_FALSE(preview.value().rgb.empty());
        const QImage view(preview.value().rgb.data(), static_cast<int>(preview.value().width),
                          static_cast<int>(preview.value().height),
                          static_cast<int>(preview.value().width * 3U), QImage::Format_RGB888);
        const QImage owned = view.copy();
        RasterBuffer raster;
        raster.width = preview.value().width;
        raster.height = preview.value().height;
        raster.srgb.resize(static_cast<std::size_t>(raster.width) * raster.height * 3U);
        for (std::uint32_t row = 0U; row < raster.height; ++row)
        {
            std::copy_n(
                owned.constScanLine(static_cast<int>(row)),
                static_cast<std::size_t>(raster.width) * 3U,
                raster.srgb.begin() +
                    static_cast<std::ptrdiff_t>(static_cast<std::size_t>(row) * raster.width * 3U));
        }
        auto histogram = collect_rgb_histogram(raster);
        ASSERT_TRUE(histogram) << histogram.error().message;
        const auto visible = std::chrono::steady_clock::now();
        render_elapsed_ms.push_back(
            std::chrono::duration_cast<std::chrono::milliseconds>(rendered - started).count());
        visible_elapsed_ms.push_back(
            std::chrono::duration_cast<std::chrono::milliseconds>(visible - started).count());
    }
    std::sort(render_elapsed_ms.begin(), render_elapsed_ms.end());
    std::sort(visible_elapsed_ms.begin(), visible_elapsed_ms.end());
    const auto p90_index = (visible_elapsed_ms.size() * 9U - 1U) / 10U;
    const auto visible_p90 = visible_elapsed_ms[p90_index];
    std::cerr << "perspective_interactive_edge=" << max_edge << " runs=" << runs
              << " render_min_ms=" << render_elapsed_ms.front()
              << " render_median_ms=" << render_elapsed_ms[render_elapsed_ms.size() / 2U]
              << " render_p90_ms=" << render_elapsed_ms[p90_index]
              << " render_max_ms=" << render_elapsed_ms.back()
              << " visible_min_ms=" << visible_elapsed_ms.front()
              << " visible_median_ms=" << visible_elapsed_ms[visible_elapsed_ms.size() / 2U]
              << " visible_p90_ms=" << visible_p90
              << " visible_max_ms=" << visible_elapsed_ms.back() << '\n';
    if (const char *budget = std::getenv("RAVO_PERSPECTIVE_PERF_P90_BUDGET_MS"))
        EXPECT_LE(visible_p90, std::stoll(budget));

    ASSERT_TRUE(measured.close());
    std::error_code ignored;
    std::filesystem::remove_all(cache_root, ignored);
}

TEST(InteractivePreviewQualityProbe, ComparesInteractiveEdgesWithSettledDisplayPixels)
{
    const char *catalog_path = std::getenv("RAVO_INTERACTIVE_PERF_CATALOG");
    const char *asset_id = std::getenv("RAVO_INTERACTIVE_PERF_ASSET_ID");
    if (catalog_path == nullptr || asset_id == nullptr)
    {
        GTEST_SKIP() << "set RAVO_INTERACTIVE_PERF_CATALOG and RAVO_INTERACTIVE_PERF_ASSET_ID";
    }

    auto created_engine = EngineFacade::create_phase1();
    ASSERT_TRUE(created_engine) << created_engine.error().message;
    auto repository = SqliteCatalogRepository::open(catalog_path);
    ASSERT_TRUE(repository) << repository.error().message;
    const auto cache_root = make_catalog_test_temp_root();
    auto cache = FilesystemPreviewCache::create((cache_root / "preview").string());
    ASSERT_TRUE(cache) << cache.error().message;
    auto recovery = FilesystemRecoveryStore::create((cache_root / "recovery").string());
    ASSERT_TRUE(recovery) << recovery.error().message;
    CatalogService measured(created_engine.value(), std::move(repository).value(),
                            std::make_unique<QtRasterDecoder>(), std::move(cache).value(),
                            std::move(recovery).value());
    auto recipe = measured.load_recipe(asset_id);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto develop = develop_from_recipe(recipe.value());
    ASSERT_TRUE(develop) << develop.error().message;

    const auto render = [&](const std::uint32_t edge) -> Result<PreviewResult>
    {
        PreviewRequest request;
        request.asset_id = asset_id;
        request.max_edge = edge;
        request.persist_preview_record = false;
        request.prefer_embedded_preview = false;
        return measured.request_preview(request, develop.value());
    };
    auto former = render(640U);
    auto interactive = render(kInteractivePreviewMaxEdge);
    auto settled = render(kDefaultPreviewMaxEdge);
    ASSERT_TRUE(former) << former.error().message;
    ASSERT_TRUE(interactive) << interactive.error().message;
    ASSERT_TRUE(settled) << settled.error().message;

    struct Quality
    {
        double psnr_db = 0.0;
        double mean_absolute_error = 0.0;
        int p99_absolute_error = 0;
    };
    const auto compare = [](const PreviewResult &candidate,
                            const PreviewResult &reference) -> Quality
    {
        const QImage reference_view(reference.rgb.data(), static_cast<int>(reference.width),
                                    static_cast<int>(reference.height),
                                    static_cast<int>(reference.width * 3U), QImage::Format_RGB888);
        const QImage scaled = reference_view.scaled(
            static_cast<int>(candidate.width), static_cast<int>(candidate.height),
            Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        std::vector<int> absolute_errors;
        absolute_errors.reserve(candidate.rgb.size());
        double squared_error = 0.0;
        double absolute_error = 0.0;
        for (std::uint32_t row = 0; row < candidate.height; ++row)
        {
            const auto *reference_row = scaled.constScanLine(static_cast<int>(row));
            const auto candidate_offset = static_cast<std::size_t>(row) * candidate.width * 3U;
            for (std::size_t column = 0; column < static_cast<std::size_t>(candidate.width) * 3U;
                 ++column)
            {
                const int difference = static_cast<int>(candidate.rgb[candidate_offset + column]) -
                                       static_cast<int>(reference_row[column]);
                const int magnitude = std::abs(difference);
                squared_error += static_cast<double>(difference * difference);
                absolute_error += static_cast<double>(magnitude);
                absolute_errors.push_back(magnitude);
            }
        }
        std::sort(absolute_errors.begin(), absolute_errors.end());
        const double count = static_cast<double>(absolute_errors.size());
        const double mse = squared_error / count;
        const auto p99_index = (absolute_errors.size() * 99U - 1U) / 100U;
        return Quality{
            .psnr_db = mse == 0.0 ? std::numeric_limits<double>::infinity() :
                                    20.0 * std::log10(255.0 / std::sqrt(mse)),
            .mean_absolute_error = absolute_error / count,
            .p99_absolute_error = absolute_errors[p99_index],
        };
    };
    const Quality former_quality = compare(former.value(), settled.value());
    const Quality interactive_quality = compare(interactive.value(), settled.value());
    std::cerr << "interactive_quality_edge=" << kInteractivePreviewMaxEdge
              << " settled_edge=" << kDefaultPreviewMaxEdge
              << " psnr_db=" << interactive_quality.psnr_db
              << " mean_abs_error=" << interactive_quality.mean_absolute_error
              << " p99_abs_error=" << interactive_quality.p99_absolute_error
              << " former_640_psnr_db=" << former_quality.psnr_db
              << " former_640_mean_abs_error=" << former_quality.mean_absolute_error
              << " former_640_p99_abs_error=" << former_quality.p99_absolute_error << '\n';
    EXPECT_GE(interactive_quality.psnr_db, former_quality.psnr_db);
    EXPECT_LE(interactive_quality.mean_absolute_error, former_quality.mean_absolute_error);
    EXPECT_LE(interactive_quality.p99_absolute_error, former_quality.p99_absolute_error);
    if (const char *minimum = std::getenv("RAVO_INTERACTIVE_QUALITY_MIN_PSNR_DB"))
    {
        EXPECT_GE(interactive_quality.psnr_db, std::stod(minimum));
    }

    ASSERT_TRUE(measured.close());
    std::error_code ignored;
    std::filesystem::remove_all(cache_root, ignored);
}

TEST_F(CatalogServiceTest, TexturePersistsAndReproducesPreviewAndExportAfterReopen)
{
    ASSERT_TRUE(open_service(true));
    const auto source_path = (root / "texture.png").string();
    QImage source(96, 64, QImage::Format_RGB888);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    for (int row = 0; row < source.height(); ++row)
    {
        for (int column = 0; column < source.width(); ++column)
        {
            const int checker = ((column / 2 + row / 2) & 1) == 0 ? -18 : 18;
            source.setPixelColor(column, row,
                                 QColor(std::clamp(110 + checker + column / 4, 0, 255),
                                        std::clamp(90 + checker + row / 3, 0, 255),
                                        std::clamp(70 + checker, 0, 255)));
        }
    }
    ASSERT_TRUE(source.save(QString::fromStdString(source_path), "PNG"));
    const auto source_hash = file_sha256(source_path);
    auto imported = service->import_one(source_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const std::string asset_id = imported.value().asset->id;

    PreviewRequest preview;
    preview.asset_id = asset_id;
    preview.max_edge = 96U;
    preview.persist_preview_record = true;
    auto baseline = service->request_preview(preview);
    ASSERT_TRUE(baseline) << baseline.error().message;
    const QImage baseline_image(QString::fromStdString(baseline.value().cache_path));
    ASSERT_FALSE(baseline_image.isNull());

    auto recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto develop = develop_from_recipe(recipe.value());
    ASSERT_TRUE(develop) << develop.error().message;
    develop.value().texture = TextureParams{0.75, 4.0, 1};
    ASSERT_TRUE(service->save_develop(asset_id, develop.value()));

    auto stored = service->load_recipe(asset_id);
    ASSERT_TRUE(stored) << stored.error().message;
    const auto operation =
        std::find_if(stored.value().operations.begin(), stored.value().operations.end(),
                     [](const OperationInstance &item) { return item.id == kTextureOperationId; });
    ASSERT_NE(operation, stored.value().operations.end());
    auto stored_params = texture_from_parameters(operation->parameters);
    ASSERT_TRUE(stored_params) << stored_params.error().message;
    EXPECT_EQ(stored_params.value(), develop.value().texture);

    auto before_reopen = service->request_preview(preview);
    ASSERT_TRUE(before_reopen) << before_reopen.error().message;
    const QImage before_reopen_image(QString::fromStdString(before_reopen.value().cache_path));
    ASSERT_FALSE(before_reopen_image.isNull());
    EXPECT_NE(before_reopen_image, baseline_image);

    const auto export_path = (root / "texture-export.png").string();
    ExportRequest export_request;
    export_request.asset_id = asset_id;
    export_request.output_path = export_path;
    export_request.format = ExportFormat::kPng;
    export_request.max_edge = 96U;
    auto exported = service->export_asset(export_request);
    ASSERT_TRUE(exported) << exported.error().message;
    const QImage exported_image(QString::fromStdString(export_path));
    ASSERT_FALSE(exported_image.isNull());
    EXPECT_EQ(exported_image, before_reopen_image);

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto restored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(restored_recipe) << restored_recipe.error().message;
    auto restored = develop_from_recipe(restored_recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_EQ(restored.value().texture, develop.value().texture);
    auto after_reopen = service->request_preview(preview);
    ASSERT_TRUE(after_reopen) << after_reopen.error().message;
    EXPECT_EQ(after_reopen.value().cache_key, before_reopen.value().cache_key);
    const QImage after_reopen_image(QString::fromStdString(after_reopen.value().cache_path));
    ASSERT_FALSE(after_reopen_image.isNull());
    EXPECT_EQ(after_reopen_image, before_reopen_image);
    EXPECT_EQ(file_sha256(source_path), source_hash);
}

TEST_F(CatalogServiceTest, RawJpegPairImportsAsOneAssetAndCopyKeepsCompanion)
{
    ASSERT_TRUE(open_service(true));
    const auto source_dir = root / "pair-source";
    const auto destination = root / "pair-destination";
    std::filesystem::create_directories(source_dir);
    std::filesystem::create_directories(destination);
    const auto raw_path = source_dir / "DSC0001.cr2";
    std::filesystem::copy_file(raw_fixture_path(), raw_path);
    const auto jpeg_path = source_dir / "DSC0001.jpg";
    QImage jpeg(48, 32, QImage::Format_RGB888);
    jpeg.setColorSpace(QColorSpace(QColorSpace::SRgb));
    jpeg.fill(QColor(20, 180, 40));
    ASSERT_TRUE(jpeg.save(QString::fromStdString(jpeg_path.string()), "JPEG", 95));
    const auto standalone_path = source_dir / "extra.jpg";
    ASSERT_TRUE(jpeg.save(QString::fromStdString(standalone_path.string()), "JPEG", 95));
    const auto jpeg_hash = file_sha256(jpeg_path.string());
    const auto raw_hash = file_sha256(raw_path.string());

    auto enumerated = service->enumerate_import_inputs({source_dir.string()}, CancellationToken{});
    ASSERT_TRUE(enumerated) << enumerated.error().message;
    ASSERT_EQ(enumerated.value().size(), 2U);
    std::vector<std::string> names;
    names.reserve(enumerated.value().size());
    for (const auto &path : enumerated.value())
        names.push_back(std::filesystem::path(path).filename().string());
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names, (std::vector<std::string>{"DSC0001.cr2", "extra.jpg"}));

    ImportRequest request;
    request.inputs = {source_dir.string()};
    request.source_root = source_dir.string();
    request.mode = ImportTransferMode::kCopy;
    request.organization = ImportOrganization::kSingleFolder;
    request.destination_directory = destination.string();
    request.preview = ImportPreviewPolicy::kMinimal;
    request.defer_previews = true;
    auto imported = service->execute_import(request);
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_EQ(imported.value().imported, 2U);
    ASSERT_EQ(imported.value().items.size(), 2U);

    EXPECT_TRUE(std::filesystem::exists(destination / "DSC0001.cr2"));
    EXPECT_TRUE(std::filesystem::exists(destination / "DSC0001.jpg"));
    EXPECT_TRUE(std::filesystem::exists(destination / "extra.jpg"));
    EXPECT_EQ(file_sha256((destination / "DSC0001.cr2").string()), raw_hash);
    EXPECT_EQ(file_sha256((destination / "DSC0001.jpg").string()), jpeg_hash);
    EXPECT_EQ(file_sha256(raw_path.string()), raw_hash);
    EXPECT_EQ(file_sha256(jpeg_path.string()), jpeg_hash);

    auto assets = service->list_assets();
    ASSERT_TRUE(assets) << assets.error().message;
    ASSERT_EQ(assets.value().size(), 2U);
    std::size_t raw_count = 0;
    std::size_t jpeg_count = 0;
    std::string raw_id;
    for (const auto &asset : assets.value())
    {
        if (is_raw_media_type(asset.media_type))
        {
            ++raw_count;
            raw_id = asset.id;
        }
        else
        {
            ++jpeg_count;
            EXPECT_TRUE(is_raster_media_type(asset.media_type));
        }
    }
    EXPECT_EQ(raw_count, 1U);
    EXPECT_EQ(jpeg_count, 1U);

    const ImportItemResult *raw_item = nullptr;
    for (const auto &item : imported.value().items)
    {
        if (item.asset && is_raw_media_type(item.asset->media_type))
            raw_item = &item;
    }
    ASSERT_NE(raw_item, nullptr);
    ASSERT_TRUE(raw_item->jpeg_companion_destination_path);
    EXPECT_TRUE(std::filesystem::equivalent(*raw_item->jpeg_companion_destination_path,
                                            destination / "DSC0001.jpg"));

    auto recipe = service->load_recipe(raw_id);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto develop = develop_from_recipe(recipe.value());
    ASSERT_TRUE(develop) << develop.error().message;
    EXPECT_TRUE(develop.value().rapidraw_basic_tone_enabled);
    EXPECT_NEAR(develop.value().sharpen, SharpenParams{}.amount, 1e-9);

    PreviewRequest browse;
    browse.asset_id = raw_id;
    browse.max_edge = kThumbnailMaxEdge;
    browse.purpose = PreviewPurpose::kBrowse;
    browse.prefer_embedded_preview = true;
    auto thumb = service->request_preview(browse);
    ASSERT_TRUE(thumb) << thumb.error().message;
    EXPECT_NE(thumb.value().cache_key.find(std::string(kCompanionJpegBrowsePreviewDigest)),
              std::string::npos);
    EXPECT_TRUE(std::filesystem::exists(thumb.value().cache_path));

    PreviewRequest loupe;
    loupe.asset_id = raw_id;
    loupe.max_edge = kDefaultPreviewMaxEdge;
    loupe.purpose = PreviewPurpose::kBrowse;
    loupe.prefer_embedded_preview = true;
    auto camera_look = service->request_preview(loupe);
    ASSERT_TRUE(camera_look) << camera_look.error().message;
    EXPECT_NE(camera_look.value().cache_key.find(std::string(kCompanionJpegBrowsePreviewDigest)),
              std::string::npos);
}

TEST_F(CatalogServiceTest, AmbiguousRawJpegCompanionsRejectImport)
{
    ASSERT_TRUE(open_service(true));
    const auto source_dir = root / "ambiguous-pair";
    std::filesystem::create_directories(source_dir);
    std::filesystem::copy_file(raw_fixture_path(), source_dir / "DSC0001.cr2");
    QImage jpeg(32, 24, QImage::Format_RGB888);
    jpeg.setColorSpace(QColorSpace(QColorSpace::SRgb));
    jpeg.fill(QColor(10, 20, 30));
    ASSERT_TRUE(
        jpeg.save(QString::fromStdString((source_dir / "DSC0001.jpg").string()), "JPEG", 95));
    ASSERT_TRUE(
        jpeg.save(QString::fromStdString((source_dir / "DSC0001.jpeg").string()), "JPEG", 95));

    auto enumerated = service->enumerate_import_inputs({source_dir.string()}, CancellationToken{});
    ASSERT_TRUE(enumerated) << enumerated.error().message;
    ASSERT_EQ(enumerated.value().size(), 1U);

    ImportRequest request;
    request.inputs = {source_dir.string()};
    request.source_root = source_dir.string();
    request.mode = ImportTransferMode::kAdd;
    auto imported = service->execute_import(request);
    ASSERT_FALSE(imported);
    EXPECT_EQ(imported.error().context.at("reason"), "import_jpeg_companion_ambiguous");
    auto assets = service->list_assets();
    ASSERT_TRUE(assets) << assets.error().message;
    EXPECT_TRUE(assets.value().empty());
}

TEST_F(CatalogServiceTest, DeferredImportOfUntaggedJpegPublishesBrowseThumbnail)
{
    ASSERT_TRUE(open_service(true));
    const auto jpeg_path = (root / "untagged.jpg").string();
    QImage image(32, 24, QImage::Format_RGB888);
    image.fill(QColor(40, 80, 120));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 95));
    QtRasterDecoder decoder;
    auto decoded = decoder.decode(jpeg_path, 0, CancellationToken{});
    ASSERT_TRUE(decoded) << decoded.error().message;
    ASSERT_EQ(decoded.value().color_profile.kind, ColorProfileKind::kMissing);

    auto imported =
        service->import_one(jpeg_path, CancellationToken{}, ImportPreviewPolicy::kStandard, true);
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_EQ(imported.value().status, ImportItemStatus::kImported);
    ASSERT_TRUE(imported.value().asset);
    EXPECT_TRUE(imported.value().preview_pending);
    ASSERT_TRUE(imported.value().preview_cache_path);
    EXPECT_TRUE(std::filesystem::exists(*imported.value().preview_cache_path));

    PreviewRequest browse;
    browse.asset_id = imported.value().asset->id;
    browse.max_edge = kThumbnailMaxEdge;
    browse.purpose = PreviewPurpose::kBrowse;
    browse.prefer_embedded_preview = true;
    auto thumb = service->request_preview(browse);
    ASSERT_TRUE(thumb) << thumb.error().message;
    EXPECT_TRUE(std::filesystem::exists(thumb.value().cache_path));
}

} // namespace
} // namespace ravo

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    ravo::init_logging("ravo-catalog-tests");
    testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    ravo::shutdown_logging();
    return result;
}
