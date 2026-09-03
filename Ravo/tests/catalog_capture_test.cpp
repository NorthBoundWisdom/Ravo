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

TEST_F(CatalogServiceTest, OutputDitherPersistsRebuildsAndExportsTheDisplayedPixels)
{
    ASSERT_TRUE(open_service(true));
    const auto source_path = (root / "output-dither-source.png").string();
    QImage image(12, 8, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    for (int row = 0; row < image.height(); ++row)
    {
        for (int column = 0; column < image.width(); ++column)
        {
            image.setPixelColor(column, row,
                                QColor((column * 23 + row * 7) % 256,
                                       (column * 11 + row * 29) % 256,
                                       (column * 31 + row * 3) % 256));
        }
    }
    ASSERT_TRUE(image.save(QString::fromStdString(source_path), "PNG"));
    const auto source_hash = file_sha256(source_path);
    auto imported = service->import_one(source_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    DevelopParams develop;
    develop.output_dither_present = true;
    develop.output_dither_enabled = true;
    develop.output_dither = {OutputDitherMethod::kPosterize4, -100.0};
    ASSERT_TRUE(service->save_develop(asset_id, develop));

    PreviewRequest preview_request;
    preview_request.asset_id = asset_id;
    auto preview = service->request_preview(preview_request);
    ASSERT_TRUE(preview) << preview.error().message;
    QImage preview_image(QString::fromStdString(preview.value().cache_path));
    ASSERT_FALSE(preview_image.isNull());
    const auto export_path = (root / "output-dither-export.png").string();
    ExportRequest export_request;
    export_request.asset_id = asset_id;
    export_request.output_path = export_path;
    export_request.format = ExportFormat::kPng;
    auto exported = service->export_asset(export_request);
    ASSERT_TRUE(exported) << exported.error().message;
    QImage export_image(QString::fromStdString(export_path));
    ASSERT_FALSE(export_image.isNull());
    EXPECT_EQ(preview_image, export_image);
    EXPECT_EQ(file_sha256(source_path), source_hash);

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(std::filesystem::remove(preview.value().cache_path));
    ASSERT_TRUE(open_service(false));
    auto restored = service->load_recipe(asset_id);
    ASSERT_TRUE(restored) << restored.error().message;
    auto restored_develop = develop_from_recipe(restored.value());
    ASSERT_TRUE(restored_develop) << restored_develop.error().message;
    EXPECT_TRUE(restored_develop.value().output_dither_enabled);
    EXPECT_EQ(restored_develop.value().output_dither.method, OutputDitherMethod::kPosterize4);
    auto rebuilt = service->request_preview(preview_request);
    ASSERT_TRUE(rebuilt) << rebuilt.error().message;
    QImage rebuilt_image(QString::fromStdString(rebuilt.value().cache_path));
    ASSERT_FALSE(rebuilt_image.isNull());
    EXPECT_EQ(rebuilt_image, export_image);
    EXPECT_EQ(file_sha256(source_path), source_hash);
}

TEST_F(CatalogServiceTest, VelviaPersistsRebuildsAndExportsTheDisplayedPixels)
{
    ASSERT_TRUE(open_service(true));
    const auto source_path = (root / "velvia-source.png").string();
    QImage image(12, 8, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    for (int row = 0; row < image.height(); ++row)
    {
        for (int column = 0; column < image.width(); ++column)
        {
            image.setPixelColor(column, row,
                                QColor((column * 23 + row * 7) % 256,
                                       (column * 11 + row * 29) % 256,
                                       (column * 31 + row * 3) % 256));
        }
    }
    ASSERT_TRUE(image.save(QString::fromStdString(source_path), "PNG"));
    const auto source_hash = file_sha256(source_path);
    auto imported = service->import_one(source_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    PreviewRequest preview_request;
    preview_request.asset_id = asset_id;
    auto baseline = service->request_preview(preview_request);
    ASSERT_TRUE(baseline) << baseline.error().message;
    const QImage baseline_image(QString::fromStdString(baseline.value().cache_path));
    ASSERT_FALSE(baseline_image.isNull());

    DevelopParams develop;
    develop.velvia_present = true;
    develop.velvia_enabled = true;
    develop.velvia = {100.0, 0.15};
    ASSERT_TRUE(service->save_develop(asset_id, develop));
    auto preview = service->request_preview(preview_request);
    ASSERT_TRUE(preview) << preview.error().message;
    const QImage preview_image(QString::fromStdString(preview.value().cache_path));
    ASSERT_FALSE(preview_image.isNull());
    EXPECT_NE(preview.value().cache_key, baseline.value().cache_key);
    EXPECT_NE(preview_image, baseline_image);

    const auto export_path = (root / "velvia-export.png").string();
    ExportRequest export_request;
    export_request.asset_id = asset_id;
    export_request.output_path = export_path;
    export_request.format = ExportFormat::kPng;
    auto exported = service->export_asset(export_request);
    ASSERT_TRUE(exported) << exported.error().message;
    const QImage export_image(QString::fromStdString(export_path));
    ASSERT_FALSE(export_image.isNull());
    EXPECT_EQ(preview_image, export_image);
    EXPECT_EQ(file_sha256(source_path), source_hash);

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(std::filesystem::remove(preview.value().cache_path));
    ASSERT_TRUE(open_service(false));
    auto restored = service->load_recipe(asset_id);
    ASSERT_TRUE(restored) << restored.error().message;
    auto restored_develop = develop_from_recipe(restored.value());
    ASSERT_TRUE(restored_develop) << restored_develop.error().message;
    EXPECT_TRUE(restored_develop.value().velvia_present);
    EXPECT_TRUE(restored_develop.value().velvia_enabled);
    EXPECT_EQ(restored_develop.value().velvia, (VelviaParams{100.0, 0.15}));
    auto rebuilt = service->request_preview(preview_request);
    ASSERT_TRUE(rebuilt) << rebuilt.error().message;
    const QImage rebuilt_image(QString::fromStdString(rebuilt.value().cache_path));
    ASSERT_FALSE(rebuilt_image.isNull());
    EXPECT_EQ(rebuilt_image, export_image);
    EXPECT_EQ(file_sha256(source_path), source_hash);
}

TEST_F(CatalogServiceTest, CanvasColorZonesMonochromeSplitFrameWatermarkPersistExactPixels)
{
    ASSERT_TRUE(open_service(true));
    const auto source_path = (root / "canvas-frame-source.png").string();
    QImage image(12, 8, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(80, 120, 160));
    ASSERT_TRUE(image.save(QString::fromStdString(source_path), "PNG"));
    const auto source_hash = file_sha256(source_path);
    auto imported = service->import_one(source_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    DevelopParams develop;
    develop.canvas_present = true;
    develop.canvas_enabled = true;
    develop.canvas = {25.0, 25.0, 25.0, 25.0, CanvasColor::kBlue};
    develop.frame_present = true;
    develop.frame_enabled = true;
    develop.frame.size = 0.1;
    develop.frame.border_color = {1.0, 1.0, 1.0};
    develop.watermark_present = true;
    develop.watermark_enabled = true;
    develop.watermark.text = "A";
    develop.watermark.color = {0.0, 0.0, 0.0};
    develop.watermark.opacity = 1.0;
    develop.watermark.scale_percent = 50.0;
    develop.watermark.alignment = WatermarkAlignment::kTopLeft;
    develop.color_zones_present = true;
    develop.color_zones_enabled = true;
    for (auto &point : develop.color_zones.curves[1].points)
        point.y = 0.75;
    develop.monochrome_present = true;
    develop.monochrome_enabled = true;
    develop.monochrome = {20.0, 10.0, 2.0, 0.25, 1.0};
    develop.split_toning_present = true;
    develop.split_toning_enabled = true;
    develop.split_toning = {0.34, 0.9, 0.93, 0.9, 0.35, 15.0, 1.0};
    ASSERT_TRUE(service->save_develop(asset_id, develop));
    PreviewRequest preview_request;
    preview_request.asset_id = asset_id;
    auto preview = service->request_preview(preview_request);
    ASSERT_TRUE(preview) << preview.error().message;
    EXPECT_EQ(preview.value().width, 20U);
    EXPECT_EQ(preview.value().height, 14U);
    QImage preview_image(QString::fromStdString(preview.value().cache_path));
    ASSERT_FALSE(preview_image.isNull());

    ExportRequest request;
    request.asset_id = asset_id;
    request.output_path = (root / "canvas-frame-export.png").string();
    request.format = ExportFormat::kPng;
    auto exported = service->export_asset(request);
    ASSERT_TRUE(exported) << exported.error().message;
    EXPECT_EQ(exported.value().width, 20U);
    EXPECT_EQ(exported.value().height, 14U);
    QImage exported_image(QString::fromStdString(request.output_path));
    EXPECT_EQ(exported_image, preview_image);
    EXPECT_EQ(file_sha256(source_path), source_hash);
    for (const auto &[format, filename] :
         {std::pair{ExportFormat::kJpeg, std::string("canvas-frame-export.jpg")},
          std::pair{ExportFormat::kTiff, std::string("canvas-frame-export.tif")}})
    {
        ExportRequest additional = request;
        additional.format = format;
        additional.output_path = (root / filename).string();
        auto result = service->export_asset(additional);
        ASSERT_TRUE(result) << filename << ": " << result.error().message;
        EXPECT_EQ(result.value().width, 20U);
        EXPECT_EQ(result.value().height, 14U);
        const QImage decoded(QString::fromStdString(additional.output_path));
        ASSERT_FALSE(decoded.isNull()) << filename;
        EXPECT_EQ(decoded.width(), 20);
        EXPECT_EQ(decoded.height(), 14);
    }

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(std::filesystem::remove(preview.value().cache_path));
    ASSERT_TRUE(open_service(false));
    auto restored = service->load_recipe(asset_id);
    ASSERT_TRUE(restored) << restored.error().message;
    auto params = develop_from_recipe(restored.value());
    ASSERT_TRUE(params) << params.error().message;
    EXPECT_TRUE(params.value().canvas_enabled);
    EXPECT_TRUE(params.value().frame_enabled);
    EXPECT_TRUE(params.value().watermark_enabled);
    EXPECT_EQ(params.value().watermark.text, "A");
    EXPECT_TRUE(params.value().color_zones_enabled);
    EXPECT_DOUBLE_EQ(params.value().color_zones.curves[1].points[0].y, 0.75);
    EXPECT_TRUE(params.value().monochrome_enabled);
    EXPECT_DOUBLE_EQ(params.value().monochrome.highlights, 0.25);
    EXPECT_TRUE(params.value().split_toning_enabled);
    EXPECT_DOUBLE_EQ(params.value().split_toning.compress, 15.0);
    EXPECT_EQ(params.value().canvas.color, CanvasColor::kBlue);
    auto rebuilt = service->request_preview(preview_request);
    ASSERT_TRUE(rebuilt) << rebuilt.error().message;
    EXPECT_EQ(QImage(QString::fromStdString(rebuilt.value().cache_path)), exported_image);
    EXPECT_EQ(file_sha256(source_path), source_hash);
}

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

TEST_F(CatalogServiceTest, ImportsMire1AsLocalCaptureWithoutOffsetOrGps)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto source_hash = file_sha256(raw_fixture_path());
    const auto source_mtime = std::filesystem::last_write_time(raw_fixture_path());
    auto imported = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    ASSERT_TRUE(imported.value().asset->capture.captured_datetime);
    EXPECT_EQ(imported.value().asset->capture.captured_datetime->local_exif, "2007:09:11 13:53:33");
    ASSERT_TRUE(imported.value().asset->capture.captured_datetime->subsecond_digits);
    EXPECT_EQ(*imported.value().asset->capture.captured_datetime->subsecond_digits, "18");
    EXPECT_FALSE(imported.value().asset->capture.captured_datetime->utc_offset_minutes);
    EXPECT_FALSE(imported.value().asset->capture.location);
    EXPECT_EQ(file_sha256(raw_fixture_path()), source_hash);
    EXPECT_EQ(std::filesystem::last_write_time(raw_fixture_path()), source_mtime);

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto listed = service->list_assets();
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed.value().size(), 1U);
    ASSERT_TRUE(listed.value().front().capture.captured_datetime);
    EXPECT_EQ(listed.value().front().capture.captured_datetime->local_exif, "2007:09:11 13:53:33");
    EXPECT_EQ(*listed.value().front().capture.captured_datetime->subsecond_digits, "18");
    EXPECT_FALSE(listed.value().front().capture.captured_datetime->utc_offset_minutes);
    EXPECT_FALSE(listed.value().front().capture.location);

    auto before_duplicate = service->snapshot();
    ASSERT_TRUE(before_duplicate);
    auto duplicate = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(duplicate) << duplicate.error().message;
    EXPECT_EQ(duplicate.value().status, ImportItemStatus::kDuplicate);
    EXPECT_EQ(duplicate.value().asset->id, imported.value().asset->id);
    EXPECT_EQ(duplicate.value().asset->capture.captured_datetime,
              imported.value().asset->capture.captured_datetime);
    EXPECT_EQ(duplicate.value().asset->capture.location, imported.value().asset->capture.location);
    auto after_duplicate = service->snapshot();
    ASSERT_TRUE(after_duplicate);
    EXPECT_EQ(after_duplicate.value().revision, before_duplicate.value().revision);
}

TEST_F(CatalogServiceTest, ImportsSyntheticJpegCaptureTimeAndGps)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto jpeg_path = write_synthetic_jpeg_with_capture(root / "zoned.jpg");
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    ASSERT_TRUE(imported.value().asset->capture.captured_datetime);
    EXPECT_EQ(imported.value().asset->capture.captured_datetime->local_exif, "2007:09:11 13:53:33");
    EXPECT_EQ(*imported.value().asset->capture.captured_datetime->subsecond_digits, "18");
    ASSERT_TRUE(imported.value().asset->capture.captured_datetime->utc_offset_minutes);
    EXPECT_EQ(*imported.value().asset->capture.captured_datetime->utc_offset_minutes, 120);
    ASSERT_TRUE(imported.value().asset->capture.location);
    EXPECT_EQ(imported.value().asset->capture.location->latitude_e6, 49253239);
    EXPECT_EQ(imported.value().asset->capture.location->longitude_e6, 3050766);
    ASSERT_TRUE(imported.value().asset->capture.location->altitude);
    EXPECT_EQ(imported.value().asset->capture.location->altitude->magnitude_mm, 123456U);
    EXPECT_EQ(imported.value().asset->capture.location->altitude->reference,
              CaptureAltitudeReference::kAboveSeaLevel);
}

void replace_synthetic_capture_year(const std::string &path, const QByteArray &year)
{
    QFile file(QString::fromStdString(path));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    QByteArray bytes = file.readAll();
    file.close();
    const auto position = bytes.indexOf("2007:09:11 13:53:33");
    ASSERT_GE(position, 0);
    ASSERT_EQ(year.size(), 4);
    bytes.replace(position, 4, year);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(file.write(bytes), bytes.size());
    file.close();
}

TEST_F(CatalogServiceTest, RefreshCaptureMetadataPublishesSourceChangesAtomically)
{
    ASSERT_TRUE(open_service(true));
    const auto jpeg_path = write_synthetic_jpeg_with_capture(root / "refresh-capture.jpg");
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    auto before = service->snapshot();
    ASSERT_TRUE(before);
    replace_synthetic_capture_year(jpeg_path, QByteArray("2008"));

    WritableMetadata writable;
    writable.title = "KeepMe";
    writable.creator = "CatalogOnly";
    writable.copyright = "© Keep";
    writable.country = "KeepCountry";
    writable.province_state = "KeepState";
    writable.city = "KeepCity";
    writable.sublocation = "KeepSub";
    auto saved = service->set_writable_metadata(asset_id, writable);
    ASSERT_TRUE(saved) << saved.error().message;
    before = service->snapshot();
    ASSERT_TRUE(before);

    auto refreshed = service->refresh_capture_metadata(asset_id, CancellationToken{});
    ASSERT_TRUE(refreshed) << refreshed.error().message;
    ASSERT_TRUE(refreshed.value().capture.captured_datetime);
    EXPECT_EQ(refreshed.value().capture.captured_datetime->local_exif, "2008:09:11 13:53:33");
    ASSERT_TRUE(refreshed.value().metadata.title);
    EXPECT_EQ(*refreshed.value().metadata.title, "KeepMe");
    ASSERT_TRUE(refreshed.value().metadata.creator);
    EXPECT_EQ(*refreshed.value().metadata.creator, "CatalogOnly");
    ASSERT_TRUE(refreshed.value().metadata.copyright);
    EXPECT_EQ(*refreshed.value().metadata.copyright, "© Keep");
    ASSERT_TRUE(refreshed.value().metadata.country);
    EXPECT_EQ(*refreshed.value().metadata.country, "KeepCountry");
    ASSERT_TRUE(refreshed.value().metadata.province_state);
    EXPECT_EQ(*refreshed.value().metadata.province_state, "KeepState");
    ASSERT_TRUE(refreshed.value().metadata.city);
    EXPECT_EQ(*refreshed.value().metadata.city, "KeepCity");
    ASSERT_TRUE(refreshed.value().metadata.sublocation);
    EXPECT_EQ(*refreshed.value().metadata.sublocation, "KeepSub");
    auto after = service->snapshot();
    ASSERT_TRUE(after);
    EXPECT_EQ(after.value().revision, before.value().revision + 1);

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto listed = service->list_assets();
    ASSERT_TRUE(listed);
    ASSERT_EQ(listed.value().size(), 1U);
    ASSERT_TRUE(listed.value().front().capture.captured_datetime);
    EXPECT_EQ(listed.value().front().capture.captured_datetime->local_exif, "2008:09:11 13:53:33");
    ASSERT_TRUE(listed.value().front().metadata.title);
    EXPECT_EQ(*listed.value().front().metadata.title, "KeepMe");
    ASSERT_TRUE(listed.value().front().metadata.copyright);
    EXPECT_EQ(*listed.value().front().metadata.copyright, "© Keep");
    ASSERT_TRUE(listed.value().front().metadata.city);
    EXPECT_EQ(*listed.value().front().metadata.city, "KeepCity");
    ASSERT_TRUE(listed.value().front().metadata.country);
    EXPECT_EQ(*listed.value().front().metadata.country, "KeepCountry");
}

TEST_F(CatalogServiceTest, RefreshFailurePreservesCaptureAndRevision)
{
    ASSERT_TRUE(open_service(true));
    const auto jpeg_path = write_synthetic_jpeg_with_capture(root / "refresh-rollback.jpg");
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    auto before = service->snapshot();
    ASSERT_TRUE(before);
    replace_synthetic_capture_year(jpeg_path, QByteArray("2009"));

    {
        const auto connection = QStringLiteral("ravo_refresh_failure_injection");
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(QString::fromStdString(database_path));
        ASSERT_TRUE(database.open()) << database.lastError().text().toStdString();
        QSqlQuery query(database);
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TRIGGER fail_refresh_revision BEFORE UPDATE OF revision ON schema_info "
            "BEGIN SELECT RAISE(ABORT, 'forced refresh revision failure'); END")))
            << query.lastError().text().toStdString();
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection);
    }
    auto refreshed = service->refresh_capture_metadata(asset_id, CancellationToken{});
    ASSERT_FALSE(refreshed);
    EXPECT_EQ(refreshed.error().code, ErrorCode::kIo);
    auto listed = service->list_assets();
    ASSERT_TRUE(listed);
    ASSERT_EQ(listed.value().size(), 1U);
    ASSERT_TRUE(listed.value().front().capture.captured_datetime);
    EXPECT_EQ(listed.value().front().capture.captured_datetime->local_exif, "2007:09:11 13:53:33");
    auto after = service->snapshot();
    ASSERT_TRUE(after);
    EXPECT_EQ(after.value().revision, before.value().revision);

    CancellationSource cancelled;
    ASSERT_TRUE(cancelled.cancel("refresh-cancelled"));
    auto cancelled_refresh = service->refresh_capture_metadata(asset_id, cancelled.token());
    ASSERT_FALSE(cancelled_refresh);
    EXPECT_EQ(cancelled_refresh.error().code, ErrorCode::kCancelled);
}

TEST_F(CatalogServiceTest, MigratesV4CatalogLeavingNewCaptureColumnsNull)
{
    {
        const auto connection = QStringLiteral("ravo_v4_seed");
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(QString::fromStdString(database_path));
        ASSERT_TRUE(database.open()) << database.lastError().text().toStdString();
        QSqlQuery query(database);
        ASSERT_TRUE(query.exec(QStringLiteral("PRAGMA foreign_keys = ON")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE schema_info (id INTEGER PRIMARY KEY CHECK (id = 1), "
            "schema_version INTEGER NOT NULL, catalog_id TEXT NOT NULL, revision INTEGER NOT NULL, "
            "created_unix_ms INTEGER NOT NULL, migrated_unix_ms INTEGER NOT NULL)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE asset (id TEXT PRIMARY KEY, normalized_uri TEXT NOT NULL UNIQUE, "
            "media_type TEXT NOT NULL, size_bytes INTEGER NOT NULL, mtime_unix_ms INTEGER NOT NULL, "
            "content_fingerprint TEXT, width INTEGER, height INTEGER, import_state TEXT NOT NULL, "
            "error_code TEXT, error_message TEXT, created_unix_ms INTEGER NOT NULL, "
            "rating INTEGER NOT NULL DEFAULT 0, color_label TEXT NOT NULL DEFAULT 'none', "
            "rejected INTEGER NOT NULL DEFAULT 0)")));
        ASSERT_TRUE(query.exec(
            QStringLiteral("CREATE TABLE asset_recipe ("
                           "  asset_id TEXT PRIMARY KEY REFERENCES asset(id) ON DELETE CASCADE,"
                           "  recipe_schema_version INTEGER NOT NULL,"
                           "  recipe_json TEXT NOT NULL,"
                           "  updated_unix_ms INTEGER NOT NULL)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE preview (asset_id TEXT PRIMARY KEY REFERENCES asset(id) ON DELETE CASCADE, "
            "contract_version INTEGER NOT NULL, cache_key TEXT NOT NULL, width INTEGER, "
            "height INTEGER, state TEXT NOT NULL, cache_relpath TEXT, last_success_unix_ms INTEGER)")));
        ASSERT_TRUE(query.exec(
            QStringLiteral("CREATE TABLE asset_tag ("
                           "  asset_id TEXT NOT NULL REFERENCES asset(id) ON DELETE CASCADE,"
                           "  name TEXT NOT NULL, PRIMARY KEY (asset_id, name))")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE asset_metadata (asset_id TEXT PRIMARY KEY REFERENCES asset(id) ON DELETE CASCADE, "
            "title TEXT, description TEXT, creator TEXT, copyright TEXT, camera_make TEXT, "
            "camera_model TEXT, iso REAL, aperture REAL, focal_length_mm REAL, shutter_s REAL, "
            "captured_unix_s INTEGER)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE asset_recipe_history (id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "asset_id TEXT NOT NULL REFERENCES asset(id) ON DELETE CASCADE, seq INTEGER NOT NULL, "
            "kind TEXT NOT NULL, label TEXT, recipe_json TEXT NOT NULL, created_unix_ms INTEGER NOT NULL, "
            "UNIQUE(asset_id, seq))")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "INSERT INTO schema_info(id, schema_version, catalog_id, revision, created_unix_ms, "
            "migrated_unix_ms) VALUES (1, 4, 'cat_v4', 3, 1, 1)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "INSERT INTO asset(id, normalized_uri, media_type, size_bytes, mtime_unix_ms, "
            "import_state, created_unix_ms) VALUES ('ast_old', 'file:///tmp/old.png', "
            "'image/png', 12, 1, 'imported', 1)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "INSERT INTO asset_metadata(asset_id, captured_unix_s) VALUES ('ast_old', 1189514013)")));
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection);
    }

    auto opened = open_service(false);
    ASSERT_TRUE(opened) << opened.error().message;
    auto snapshot = service->snapshot();
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    EXPECT_EQ(snapshot.value().schema_version, kCatalogSchemaVersion);
    auto listed = service->list_assets();
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed.value().size(), 1U);
    EXPECT_EQ(listed.value().front().capture.captured_unix_s, 1189514013);
    EXPECT_FALSE(listed.value().front().capture.captured_datetime);
    EXPECT_FALSE(listed.value().front().capture.location);
}

TEST_F(CatalogServiceTest, RepairsPreAdrV5CatalogsThatUsedSignedAltitudeMm)
{
    {
        const auto connection = QStringLiteral("ravo_v5_signed_altitude");
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(QString::fromStdString(database_path));
        ASSERT_TRUE(database.open()) << database.lastError().text().toStdString();
        QSqlQuery query(database);
        ASSERT_TRUE(query.exec(QStringLiteral("PRAGMA foreign_keys = ON")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE schema_info (id INTEGER PRIMARY KEY CHECK (id = 1), "
            "schema_version INTEGER NOT NULL, catalog_id TEXT NOT NULL, revision INTEGER NOT NULL, "
            "created_unix_ms INTEGER NOT NULL, migrated_unix_ms INTEGER NOT NULL)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE asset (id TEXT PRIMARY KEY, normalized_uri TEXT NOT NULL UNIQUE, "
            "media_type TEXT NOT NULL, size_bytes INTEGER NOT NULL, mtime_unix_ms INTEGER NOT NULL, "
            "content_fingerprint TEXT, width INTEGER, height INTEGER, import_state TEXT NOT NULL, "
            "error_code TEXT, error_message TEXT, created_unix_ms INTEGER NOT NULL, "
            "rating INTEGER NOT NULL DEFAULT 0, color_label TEXT NOT NULL DEFAULT 'none', "
            "rejected INTEGER NOT NULL DEFAULT 0)")));
        ASSERT_TRUE(query.exec(
            QStringLiteral("CREATE TABLE asset_recipe ("
                           "  asset_id TEXT PRIMARY KEY REFERENCES asset(id) ON DELETE CASCADE,"
                           "  recipe_schema_version INTEGER NOT NULL,"
                           "  recipe_json TEXT NOT NULL,"
                           "  updated_unix_ms INTEGER NOT NULL)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE preview (asset_id TEXT PRIMARY KEY REFERENCES asset(id) ON DELETE CASCADE, "
            "contract_version INTEGER NOT NULL, cache_key TEXT NOT NULL, width INTEGER, "
            "height INTEGER, state TEXT NOT NULL, cache_relpath TEXT, last_success_unix_ms INTEGER)")));
        ASSERT_TRUE(query.exec(
            QStringLiteral("CREATE TABLE asset_tag ("
                           "  asset_id TEXT NOT NULL REFERENCES asset(id) ON DELETE CASCADE,"
                           "  name TEXT NOT NULL, PRIMARY KEY (asset_id, name))")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE asset_metadata (asset_id TEXT PRIMARY KEY REFERENCES asset(id) ON DELETE CASCADE, "
            "title TEXT, description TEXT, creator TEXT, copyright TEXT, camera_make TEXT, "
            "camera_model TEXT, iso REAL, aperture REAL, focal_length_mm REAL, shutter_s REAL, "
            "captured_unix_s INTEGER, captured_local_exif TEXT, captured_subsecond_digits TEXT, "
            "captured_utc_offset_minutes INTEGER, gps_latitude_e6 INTEGER, gps_longitude_e6 INTEGER, "
            "gps_altitude_mm INTEGER)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE asset_recipe_history (id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "asset_id TEXT NOT NULL REFERENCES asset(id) ON DELETE CASCADE, seq INTEGER NOT NULL, "
            "kind TEXT NOT NULL, label TEXT, recipe_json TEXT NOT NULL, created_unix_ms INTEGER NOT NULL, "
            "UNIQUE(asset_id, seq))")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "INSERT INTO schema_info(id, schema_version, catalog_id, revision, created_unix_ms, "
            "migrated_unix_ms) VALUES (1, 5, 'cat_v5_signed', 8, 1, 1)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "INSERT INTO asset(id, normalized_uri, media_type, size_bytes, mtime_unix_ms, "
            "import_state, created_unix_ms) VALUES "
            "('ast_above', 'file:///tmp/above.png', 'image/png', 12, 1, 'imported', 1), "
            "('ast_below', 'file:///tmp/below.png', 'image/png', 12, 1, 'imported', 2)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "INSERT INTO asset_metadata(asset_id, gps_latitude_e6, gps_longitude_e6, gps_altitude_mm) "
            "VALUES ('ast_above', 49253239, 3050766, 123456), "
            "('ast_below', 1000000, 2000000, -2500)")));
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection);
    }

    auto opened = open_service(false);
    ASSERT_TRUE(opened) << opened.error().message;
    auto snapshot = service->snapshot();
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    EXPECT_EQ(snapshot.value().schema_version, kCatalogSchemaVersion);
    auto listed = service->list_assets();
    ASSERT_TRUE(listed)
        << listed.error().message << " action="
        << (listed.error().context.contains("action") ? listed.error().context.at("action") : "")
        << " qt="
        << (listed.error().context.contains("qt_error") ? listed.error().context.at("qt_error") :
                                                          "");
    ASSERT_EQ(listed.value().size(), 2U);
    const AssetRecord *above = nullptr;
    const AssetRecord *below = nullptr;
    for (const auto &asset : listed.value())
    {
        if (asset.id == "ast_above")
            above = &asset;
        if (asset.id == "ast_below")
            below = &asset;
    }
    ASSERT_NE(above, nullptr);
    ASSERT_NE(below, nullptr);
    ASSERT_TRUE(above->capture.location);
    ASSERT_TRUE(above->capture.location->altitude);
    EXPECT_EQ(above->capture.location->altitude->magnitude_mm, 123456U);
    EXPECT_EQ(above->capture.location->altitude->reference,
              CaptureAltitudeReference::kAboveSeaLevel);
    ASSERT_TRUE(below->capture.location);
    ASSERT_TRUE(below->capture.location->altitude);
    EXPECT_EQ(below->capture.location->altitude->magnitude_mm, 2500U);
    EXPECT_EQ(below->capture.location->altitude->reference,
              CaptureAltitudeReference::kBelowSeaLevel);

    ASSERT_TRUE(service->close());
    service.reset();
    auto reopened = open_service(false);
    ASSERT_TRUE(reopened) << reopened.error().message;
    listed = service->list_assets();
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed.value().size(), 2U);
}

TEST_F(CatalogServiceTest, V5MigrationFailureRollsBackUnchangedV4)
{
    {
        const auto connection = QStringLiteral("ravo_v4_conflict");
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(QString::fromStdString(database_path));
        ASSERT_TRUE(database.open()) << database.lastError().text().toStdString();
        QSqlQuery query(database);
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE schema_info (id INTEGER PRIMARY KEY CHECK (id = 1), "
            "schema_version INTEGER NOT NULL, catalog_id TEXT NOT NULL, revision INTEGER NOT NULL, "
            "created_unix_ms INTEGER NOT NULL, migrated_unix_ms INTEGER NOT NULL)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE asset (id TEXT PRIMARY KEY, normalized_uri TEXT NOT NULL UNIQUE, "
            "media_type TEXT NOT NULL, size_bytes INTEGER NOT NULL, mtime_unix_ms INTEGER NOT NULL, "
            "content_fingerprint TEXT, width INTEGER, height INTEGER, import_state TEXT NOT NULL, "
            "error_code TEXT, error_message TEXT, created_unix_ms INTEGER NOT NULL, "
            "rating INTEGER NOT NULL DEFAULT 0, color_label TEXT NOT NULL DEFAULT 'none', "
            "rejected INTEGER NOT NULL DEFAULT 0)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE asset_metadata (asset_id TEXT PRIMARY KEY, captured_unix_s INTEGER, "
            "captured_local_exif TEXT)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "INSERT INTO schema_info(id, schema_version, catalog_id, revision, created_unix_ms, "
            "migrated_unix_ms) VALUES (1, 4, 'cat_v4_conflict', 2, 1, 1)")));
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection);
    }

    auto opened = open_service(false);
    ASSERT_FALSE(opened);
    EXPECT_EQ(opened.error().code, ErrorCode::kIo);

    const auto connection = QStringLiteral("ravo_v4_conflict_check");
    auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
    database.setDatabaseName(QString::fromStdString(database_path));
    ASSERT_TRUE(database.open()) << database.lastError().text().toStdString();
    QSqlQuery query(database);
    ASSERT_TRUE(query.exec(
        QStringLiteral("SELECT schema_version, revision FROM schema_info WHERE id = 1")));
    ASSERT_TRUE(query.next());
    EXPECT_EQ(query.value(0).toLongLong(), 4);
    EXPECT_EQ(query.value(1).toLongLong(), 2);
    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connection);
}

TEST_F(CatalogServiceTest, EveryV5MigrationPublicationFailureRestoresTheV4Catalog)
{
    enum class FailureKind
    {
        kAlter,
        kSchemaInfo,
        kCommit,
    };
    struct FailureCase
    {
        const char *name;
        FailureKind kind;
        const char *conflicting_column;
        const char *conflicting_type;
    };
    const std::array<FailureCase, 9> cases{{
        {"captured-local", FailureKind::kAlter, "captured_local_exif", "TEXT"},
        {"captured-subsecond", FailureKind::kAlter, "captured_subsecond_digits", "TEXT"},
        {"captured-offset", FailureKind::kAlter, "captured_utc_offset_minutes", "INTEGER"},
        {"gps-latitude", FailureKind::kAlter, "gps_latitude_e6", "INTEGER"},
        {"gps-longitude", FailureKind::kAlter, "gps_longitude_e6", "INTEGER"},
        {"gps-altitude", FailureKind::kAlter, "gps_altitude_magnitude_mm", "INTEGER"},
        {"gps-altitude-ref", FailureKind::kAlter, "gps_altitude_ref", "INTEGER"},
        {"schema-info", FailureKind::kSchemaInfo, nullptr, nullptr},
        {"commit", FailureKind::kCommit, nullptr, nullptr},
    }};
    const std::array<std::string, 7> capture_columns{
        "captured_local_exif", "captured_subsecond_digits", "captured_utc_offset_minutes",
        "gps_latitude_e6",     "gps_longitude_e6",          "gps_altitude_magnitude_mm",
        "gps_altitude_ref",
    };

    for (std::size_t index = 0; index < cases.size(); ++index)
    {
        const auto &test_case = cases[index];
        const auto path =
            (root / (std::string("migration-") + test_case.name + ".sqlite")).string();
        const QString seed_connection = QStringLiteral("ravo_v5_failure_seed_%1").arg(index);
        {
            auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), seed_connection);
            database.setDatabaseName(QString::fromStdString(path));
            ASSERT_TRUE(database.open()) << database.lastError().text().toStdString();
            QSqlQuery query(database);
            ASSERT_TRUE(query.exec(QStringLiteral(
                "CREATE TABLE schema_info (id INTEGER PRIMARY KEY CHECK (id = 1), "
                "schema_version INTEGER NOT NULL, catalog_id TEXT NOT NULL, revision INTEGER NOT NULL, "
                "created_unix_ms INTEGER NOT NULL, migrated_unix_ms INTEGER NOT NULL)")));
            QString metadata = QStringLiteral(
                "CREATE TABLE asset_metadata (asset_id TEXT PRIMARY KEY, captured_unix_s INTEGER");
            if (test_case.kind == FailureKind::kAlter)
            {
                metadata +=
                    QStringLiteral(", %1 %2").arg(QString::fromLatin1(test_case.conflicting_column),
                                                  QString::fromLatin1(test_case.conflicting_type));
            }
            metadata += QLatin1Char(')');
            ASSERT_TRUE(query.exec(metadata)) << query.lastError().text().toStdString();
            ASSERT_TRUE(query.exec(QStringLiteral(
                "INSERT INTO schema_info(id, schema_version, catalog_id, revision, created_unix_ms, "
                "migrated_unix_ms) VALUES (1, 4, 'cat_v4_failure', 7, 11, 13)")));
            ASSERT_TRUE(query.exec(QStringLiteral(
                "INSERT INTO asset_metadata(asset_id, captured_unix_s) VALUES ('ast_old', 123)")));
            if (test_case.kind == FailureKind::kSchemaInfo)
            {
                ASSERT_TRUE(query.exec(QStringLiteral(
                    "CREATE TRIGGER reject_schema_info BEFORE UPDATE OF schema_version ON schema_info "
                    "BEGIN SELECT RAISE(ABORT, 'forced schema-info failure'); END")));
            }
            if (test_case.kind == FailureKind::kCommit)
            {
                ASSERT_TRUE(query.exec(
                    QStringLiteral("CREATE TABLE migration_parent(id INTEGER PRIMARY KEY)")));
                ASSERT_TRUE(query.exec(QStringLiteral(
                    "CREATE TABLE migration_child(parent_id INTEGER, FOREIGN KEY(parent_id) "
                    "REFERENCES migration_parent(id) DEFERRABLE INITIALLY DEFERRED)")));
                ASSERT_TRUE(query.exec(QStringLiteral(
                    "CREATE TRIGGER reject_migration_commit AFTER UPDATE OF schema_version ON "
                    "schema_info BEGIN INSERT INTO migration_child(parent_id) VALUES (99); END")));
            }
            database.close();
            database = QSqlDatabase();
        }
        QSqlDatabase::removeDatabase(seed_connection);

        auto opened = SqliteCatalogRepository::open(path);
        ASSERT_FALSE(opened) << test_case.name;
        EXPECT_EQ(opened.error().code, ErrorCode::kIo) << test_case.name;

        const QString check_connection = QStringLiteral("ravo_v5_failure_check_%1").arg(index);
        {
            auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), check_connection);
            database.setDatabaseName(QString::fromStdString(path));
            ASSERT_TRUE(database.open()) << database.lastError().text().toStdString();
            QSqlQuery query(database);
            ASSERT_TRUE(query.exec(QStringLiteral(
                "SELECT schema_version, revision, created_unix_ms, migrated_unix_ms FROM "
                "schema_info WHERE id = 1")));
            ASSERT_TRUE(query.next());
            EXPECT_EQ(query.value(0).toLongLong(), 4) << test_case.name;
            EXPECT_EQ(query.value(1).toLongLong(), 7) << test_case.name;
            EXPECT_EQ(query.value(2).toLongLong(), 11) << test_case.name;
            EXPECT_EQ(query.value(3).toLongLong(), 13) << test_case.name;
            ASSERT_TRUE(query.exec(QStringLiteral("SELECT captured_unix_s FROM asset_metadata "
                                                  "WHERE asset_id = 'ast_old'")));
            ASSERT_TRUE(query.next());
            EXPECT_EQ(query.value(0).toLongLong(), 123) << test_case.name;
            ASSERT_TRUE(query.exec(QStringLiteral("PRAGMA table_info(asset_metadata)")));
            std::vector<std::string> columns;
            while (query.next())
            {
                columns.push_back(query.value(1).toString().toStdString());
            }
            for (const auto &column : capture_columns)
            {
                const bool existed_before =
                    test_case.kind == FailureKind::kAlter && column == test_case.conflicting_column;
                EXPECT_EQ(std::find(columns.begin(), columns.end(), column) != columns.end(),
                          existed_before)
                    << test_case.name << ": " << column;
            }
            if (test_case.kind == FailureKind::kCommit)
            {
                ASSERT_TRUE(query.exec(QStringLiteral("SELECT COUNT(*) FROM migration_child")));
                ASSERT_TRUE(query.next());
                EXPECT_EQ(query.value(0).toLongLong(), 0) << test_case.name;
            }
            database.close();
            database = QSqlDatabase();
        }
        QSqlDatabase::removeDatabase(check_connection);
    }
}

TEST_F(CatalogServiceTest, V6RecoveryMigrationFailureRollsBackTheV5Catalog)
{
    auto repository = SqliteCatalogRepository::create(database_path);
    ASSERT_TRUE(repository) << repository.error().message;
    AssetRecord asset;
    asset.id = "ast_v5_recovery";
    asset.normalized_uri = "file:///tmp/v5-recovery.png";
    asset.media_type = "image/png";
    asset.size_bytes = 12U;
    asset.mtime_unix_ms = 1;
    asset.created_unix_ms = 2;
    ASSERT_TRUE(repository.value()->commit_imported_asset(asset));
    ASSERT_TRUE(repository.value()->close());
    repository.value().reset();

    const auto seed_connection = QStringLiteral("ravo_v6_failure_seed");
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), seed_connection);
        database.setDatabaseName(QString::fromStdString(database_path));
        ASSERT_TRUE(database.open()) << database.lastError().text().toStdString();
        QSqlQuery query(database);
        ASSERT_TRUE(query.exec(QStringLiteral(
            "SELECT name FROM sqlite_master WHERE type = 'trigger' AND instr(name, 'recovery') > 0")));
        std::vector<QString> recovery_triggers;
        while (query.next())
        {
            recovery_triggers.push_back(query.value(0).toString());
        }
        ASSERT_FALSE(recovery_triggers.empty());
        for (const auto &trigger : recovery_triggers)
        {
            ASSERT_TRUE(query.exec(QStringLiteral("DROP TRIGGER \"%1\"").arg(trigger)))
                << query.lastError().text().toStdString();
        }
        ASSERT_TRUE(query.exec(QStringLiteral("DROP TABLE asset_recovery_state")))
            << query.lastError().text().toStdString();
        ASSERT_TRUE(query.exec(QStringLiteral(
            "UPDATE schema_info SET schema_version = 5, migrated_unix_ms = 1 WHERE id = 1")))
            << query.lastError().text().toStdString();
        ASSERT_TRUE(
            query.exec(QStringLiteral("CREATE TRIGGER asset_recovery_insert AFTER INSERT ON asset "
                                      "BEGIN SELECT 1; END")))
            << query.lastError().text().toStdString();
        database.close();
        database = QSqlDatabase();
    }
    QSqlDatabase::removeDatabase(seed_connection);

    auto failed = SqliteCatalogRepository::open(database_path);
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, ErrorCode::kIo);
    EXPECT_EQ(failed.error().context.at("action"), "migrate_v6_recovery_trigger");

    const auto check_connection = QStringLiteral("ravo_v6_failure_check");
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), check_connection);
        database.setDatabaseName(QString::fromStdString(database_path));
        ASSERT_TRUE(database.open()) << database.lastError().text().toStdString();
        QSqlQuery query(database);
        ASSERT_TRUE(query.exec(QStringLiteral(
            "SELECT schema_version, revision, migrated_unix_ms FROM schema_info WHERE id = 1")));
        ASSERT_TRUE(query.next());
        EXPECT_EQ(query.value(0).toLongLong(), 5);
        EXPECT_EQ(query.value(1).toLongLong(), 1);
        EXPECT_EQ(query.value(2).toLongLong(), 1);
        ASSERT_TRUE(
            query.exec(QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' "
                                      "AND name = 'asset_recovery_state'")));
        ASSERT_TRUE(query.next());
        EXPECT_EQ(query.value(0).toLongLong(), 0);
        ASSERT_TRUE(
            query.exec(QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE type = 'trigger' "
                                      "AND name = 'asset_recovery_insert'")));
        ASSERT_TRUE(query.next());
        EXPECT_EQ(query.value(0).toLongLong(), 1);
        ASSERT_TRUE(query.exec(QStringLiteral("SELECT COUNT(*) FROM asset")));
        ASSERT_TRUE(query.next());
        EXPECT_EQ(query.value(0).toLongLong(), 1);
        ASSERT_TRUE(query.exec(QStringLiteral("DROP TRIGGER asset_recovery_insert")))
            << query.lastError().text().toStdString();
        database.close();
        database = QSqlDatabase();
    }
    QSqlDatabase::removeDatabase(check_connection);

    auto reopened = open_service(false);
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto snapshot = service->snapshot();
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    EXPECT_EQ(snapshot.value().schema_version, kCatalogSchemaVersion);
    EXPECT_EQ(snapshot.value().revision, 1);
    auto recovery = service->recovery_state(asset.id);
    ASSERT_TRUE(recovery) << recovery.error().message;
    EXPECT_EQ(recovery.value().generation, 1);
    EXPECT_EQ(recovery.value().synchronized_generation, 1);
}

TEST_F(CatalogServiceTest, CaptureRowFailureRollsBackInvisibleAsset)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    auto snapshot_before = service->snapshot();
    ASSERT_TRUE(snapshot_before) << snapshot_before.error().message;
    {
        const auto connection = QStringLiteral("ravo_capture_failure_injection");
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(QString::fromStdString(database_path));
        ASSERT_TRUE(database.open()) << database.lastError().text().toStdString();
        QSqlQuery query(database);
        ASSERT_TRUE(
            query.exec(QStringLiteral("CREATE TRIGGER fail_capture BEFORE INSERT ON asset_metadata "
                                      "BEGIN SELECT RAISE(ABORT, 'forced capture failure'); END")))
            << query.lastError().text().toStdString();
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection);
    }

    auto imported = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported);
    EXPECT_EQ(imported.value().status, ImportItemStatus::kFailed);
    EXPECT_FALSE(imported.value().asset);
    auto listed = service->list_assets();
    ASSERT_TRUE(listed) << listed.error().message;
    EXPECT_TRUE(listed.value().empty());
    auto snapshot_after = service->snapshot();
    ASSERT_TRUE(snapshot_after) << snapshot_after.error().message;
    EXPECT_EQ(snapshot_after.value().revision, snapshot_before.value().revision);
}

TEST_F(CatalogServiceTest, RejectsPartialAndOutOfRangePersistedCaptureCoordinates)
{
    ASSERT_TRUE(open_service(true));
    auto imported = service->import_one(png_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    ASSERT_TRUE(service->close());
    service.reset();

    const auto write_capture_sql = [&](const QString &connection, const QString &statement)
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(QString::fromStdString(database_path));
        EXPECT_TRUE(database.open()) << database.lastError().text().toStdString();
        QSqlQuery query(database);
        query.prepare(statement);
        query.addBindValue(QString::fromStdString(asset_id));
        EXPECT_TRUE(query.exec()) << query.lastError().text().toStdString();
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection);
    };

    write_capture_sql(
        QStringLiteral("ravo_partial_capture_state"),
        QStringLiteral(
            "UPDATE asset_metadata SET gps_latitude_e6 = 1, gps_longitude_e6 = NULL, "
            "gps_altitude_magnitude_mm = NULL, gps_altitude_ref = NULL WHERE asset_id = ?"));
    ASSERT_TRUE(open_service(false, false));
    auto listed = service->list_assets();
    ASSERT_FALSE(listed);
    EXPECT_EQ(listed.error().context.at("reason"), "invalid_persisted_capture_location");
    auto closed = service->close();
    ASSERT_FALSE(closed);
    EXPECT_EQ(closed.error().context.at("reason"), "invalid_persisted_capture_location");
    service.reset();

    write_capture_sql(
        QStringLiteral("ravo_oversized_capture_state"),
        QStringLiteral("UPDATE asset_metadata SET gps_latitude_e6 = 9223372036854775807, "
                       "gps_longitude_e6 = 0 WHERE asset_id = ?"));
    ASSERT_TRUE(open_service(false, false));
    listed = service->list_assets();
    ASSERT_FALSE(listed);
    EXPECT_EQ(listed.error().context.at("reason"), "invalid_persisted_capture_integer");
    EXPECT_EQ(listed.error().context.at("field"), "gps_latitude_e6");
    closed = service->close();
    ASSERT_FALSE(closed);
    EXPECT_EQ(closed.error().context.at("reason"), "invalid_persisted_capture_integer");
}

TEST_F(CatalogServiceTest, RejectsWrongStorageClassesAndPartialDatetimeAltitude)
{
    ASSERT_TRUE(open_service(true));
    auto imported = service->import_one(png_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    ASSERT_TRUE(service->close());
    service.reset();

    const auto write_sql = [&](const QString &connection, const QString &statement)
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(QString::fromStdString(database_path));
        EXPECT_TRUE(database.open()) << database.lastError().text().toStdString();
        QSqlQuery ensure(database);
        ensure.prepare(QStringLiteral("INSERT OR IGNORE INTO asset_metadata(asset_id) VALUES (?)"));
        ensure.addBindValue(QString::fromStdString(asset_id));
        EXPECT_TRUE(ensure.exec()) << ensure.lastError().text().toStdString();
        QSqlQuery reset(database);
        reset.prepare(QStringLiteral(
            "UPDATE asset_metadata SET captured_local_exif = NULL, "
            "captured_subsecond_digits = NULL, captured_utc_offset_minutes = NULL, "
            "gps_latitude_e6 = NULL, gps_longitude_e6 = NULL, "
            "gps_altitude_magnitude_mm = NULL, gps_altitude_ref = NULL WHERE asset_id = ?"));
        reset.addBindValue(QString::fromStdString(asset_id));
        EXPECT_TRUE(reset.exec()) << reset.lastError().text().toStdString();
        QSqlQuery query(database);
        query.prepare(statement);
        query.addBindValue(QString::fromStdString(asset_id));
        EXPECT_TRUE(query.exec()) << query.lastError().text().toStdString();
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection);
    };

    struct WrongStorageCase
    {
        const char *connection;
        const char *field;
        const char *statement;
    };
    const std::array<WrongStorageCase, 7U> wrong_storage{{
        {"ravo_blob_in_local", "captured_local_exif",
         "UPDATE asset_metadata SET captured_local_exif = X'31' WHERE asset_id = ?"},
        {"ravo_blob_in_subsecond", "captured_subsecond_digits",
         "UPDATE asset_metadata SET captured_subsecond_digits = X'31' WHERE asset_id = ?"},
        {"ravo_text_in_offset", "captured_utc_offset_minutes",
         "UPDATE asset_metadata SET captured_utc_offset_minutes = 'bad' WHERE asset_id = ?"},
        {"ravo_text_in_latitude", "gps_latitude_e6",
         "UPDATE asset_metadata SET gps_latitude_e6 = 'north' WHERE asset_id = ?"},
        {"ravo_text_in_longitude", "gps_longitude_e6",
         "UPDATE asset_metadata SET gps_longitude_e6 = 'east' WHERE asset_id = ?"},
        {"ravo_text_in_altitude", "gps_altitude_magnitude_mm",
         "UPDATE asset_metadata SET gps_altitude_magnitude_mm = 'high' WHERE asset_id = ?"},
        {"ravo_text_in_altitude_ref", "gps_altitude_ref",
         "UPDATE asset_metadata SET gps_altitude_ref = 'below' WHERE asset_id = ?"},
    }};
    for (const auto &test_case : wrong_storage)
    {
        write_sql(QString::fromUtf8(test_case.connection), QString::fromUtf8(test_case.statement));
        ASSERT_TRUE(open_service(false, false));
        auto listed = service->list_assets();
        ASSERT_FALSE(listed) << test_case.field;
        EXPECT_EQ(listed.error().context.at("reason"), "invalid_persisted_capture_storage_class")
            << test_case.field;
        EXPECT_EQ(listed.error().context.at("field"), test_case.field);
        auto closed = service->close();
        ASSERT_FALSE(closed);
        EXPECT_EQ(closed.error().context.at("reason"), "invalid_persisted_capture_storage_class");
        service.reset();
    }

    write_sql(QStringLiteral("ravo_real_in_int"),
              QStringLiteral("UPDATE asset_metadata SET gps_latitude_e6 = 1.5 WHERE asset_id = ?"));
    ASSERT_TRUE(open_service(false, false));
    auto listed = service->list_assets();
    ASSERT_FALSE(listed);
    EXPECT_EQ(listed.error().context.at("reason"), "invalid_persisted_capture_storage_class");
    auto closed = service->close();
    ASSERT_FALSE(closed);
    EXPECT_EQ(closed.error().context.at("reason"), "invalid_persisted_capture_storage_class");
    service.reset();

    write_sql(QStringLiteral("ravo_partial_datetime"),
              QStringLiteral("UPDATE asset_metadata SET gps_latitude_e6 = NULL, "
                             "gps_longitude_e6 = NULL, captured_local_exif = NULL, "
                             "captured_subsecond_digits = '18' WHERE asset_id = ?"));
    ASSERT_TRUE(open_service(false, false));
    listed = service->list_assets();
    ASSERT_FALSE(listed);
    EXPECT_EQ(listed.error().context.at("reason"), "invalid_persisted_capture_datetime");
    closed = service->close();
    ASSERT_FALSE(closed);
    EXPECT_EQ(closed.error().context.at("reason"), "invalid_persisted_capture_datetime");
    service.reset();

    write_sql(QStringLiteral("ravo_partial_altitude"),
              QStringLiteral("UPDATE asset_metadata SET captured_subsecond_digits = NULL, "
                             "gps_latitude_e6 = 1, gps_longitude_e6 = 2, "
                             "gps_altitude_magnitude_mm = 0, gps_altitude_ref = NULL "
                             "WHERE asset_id = ?"));
    ASSERT_TRUE(open_service(false, false));
    listed = service->list_assets();
    ASSERT_FALSE(listed);
    EXPECT_EQ(listed.error().context.at("reason"), "invalid_persisted_capture_altitude");
}

TEST_F(CatalogServiceTest, ReopensZeroOffsetCoordinatesAndBothZeroAltitudeReferencesExactly)
{
    auto repository = SqliteCatalogRepository::create(database_path);
    ASSERT_TRUE(repository) << repository.error().message;
    for (const auto reference :
         {CaptureAltitudeReference::kAboveSeaLevel, CaptureAltitudeReference::kBelowSeaLevel})
    {
        AssetRecord asset;
        asset.id = reference == CaptureAltitudeReference::kAboveSeaLevel ? "ast_zero_above" :
                                                                           "ast_zero_below";
        asset.normalized_uri = "file:///tmp/" + asset.id + ".tif";
        asset.media_type = "image/tiff";
        asset.size_bytes = 3U;
        asset.mtime_unix_ms = 1;
        asset.created_unix_ms = 2;
        asset.capture.captured_datetime = CaptureDateTime{"2007:09:11 13:53:33", std::nullopt, 0};
        asset.capture.location = CaptureLocation{0, 0, CaptureAltitude{0U, reference}};
        ASSERT_TRUE(repository.value()->commit_imported_asset(asset));
    }
    auto snapshot = repository.value()->snapshot();
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().revision, 2);
    ASSERT_TRUE(repository.value()->close());
    repository = SqliteCatalogRepository::open(database_path);
    ASSERT_TRUE(repository) << repository.error().message;
    auto assets = repository.value()->list_assets();
    ASSERT_TRUE(assets) << assets.error().message;
    ASSERT_EQ(assets.value().size(), 2U);
    for (const auto &asset : assets.value())
    {
        ASSERT_TRUE(asset.capture.captured_datetime);
        ASSERT_TRUE(asset.capture.captured_datetime->utc_offset_minutes);
        EXPECT_EQ(*asset.capture.captured_datetime->utc_offset_minutes, 0);
        ASSERT_TRUE(asset.capture.location);
        EXPECT_EQ(asset.capture.location->latitude_e6, 0);
        EXPECT_EQ(asset.capture.location->longitude_e6, 0);
        ASSERT_TRUE(asset.capture.location->altitude);
        EXPECT_EQ(asset.capture.location->altitude->magnitude_mm, 0U);
        const auto expected = asset.id == "ast_zero_above" ?
                                  CaptureAltitudeReference::kAboveSeaLevel :
                                  CaptureAltitudeReference::kBelowSeaLevel;
        EXPECT_EQ(asset.capture.location->altitude->reference, expected);
    }
}

TEST_F(CatalogServiceTest, ImportInjectionMatrixLeavesNoVisibleAsset)
{
    ASSERT_TRUE(open_service(true));
    auto snapshot_before = service->snapshot();
    ASSERT_TRUE(snapshot_before);
    const auto jpeg_path = write_synthetic_jpeg_with_capture(root / "inject.jpg");
    const std::array<testing::SqliteImportFailure, 10> failures{
        testing::SqliteImportFailure::kTransactionBegin,
        testing::SqliteImportFailure::kAssetBind,
        testing::SqliteImportFailure::kAssetWrite,
        testing::SqliteImportFailure::kCaptureBind,
        testing::SqliteImportFailure::kCaptureWrite,
        testing::SqliteImportFailure::kRevisionUpdate,
        testing::SqliteImportFailure::kRevisionRead,
        testing::SqliteImportFailure::kCommit,
        testing::SqliteImportFailure::kRollback,
        testing::SqliteImportFailure::kNone,
    };
    for (const auto failure : failures)
    {
        if (failure == testing::SqliteImportFailure::kNone)
        {
            continue;
        }
        ASSERT_NE(sqlite_repository, nullptr);
        testing::SqliteCatalogTestControl::inject(*sqlite_repository, failure);
        auto imported = service->import_one(jpeg_path, CancellationToken{});
        ASSERT_TRUE(imported) << static_cast<int>(failure);
        EXPECT_EQ(imported.value().status, ImportItemStatus::kFailed);
        EXPECT_FALSE(imported.value().asset);
        auto listed = service->list_assets();
        ASSERT_TRUE(listed) << listed.error().message;
        EXPECT_TRUE(listed.value().empty());
        auto snapshot = service->snapshot();
        ASSERT_TRUE(snapshot);
        EXPECT_EQ(snapshot.value().revision, snapshot_before.value().revision);
        if (failure == testing::SqliteImportFailure::kRollback)
        {
            ASSERT_TRUE(imported.value().error);
            EXPECT_EQ(imported.value().error->context.at("rollback_failed"), "true");
            EXPECT_EQ(imported.value().error->context.at("rollback_error"),
                      "injected_import_rollback");
        }
        ASSERT_TRUE(service->close());
        service.reset();
        sqlite_repository = nullptr;
        ASSERT_TRUE(open_service(false));
        auto reopened = service->list_assets();
        ASSERT_TRUE(reopened) << reopened.error().message;
        EXPECT_TRUE(reopened.value().empty());
    }
    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto reopened = service->list_assets();
    ASSERT_TRUE(reopened) << reopened.error().message;
    EXPECT_TRUE(reopened.value().empty());
}

} // namespace
} // namespace ravo
