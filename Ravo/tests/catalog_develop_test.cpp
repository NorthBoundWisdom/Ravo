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

TEST_F(CatalogServiceTest, ExplicitDefaultLegacyColorBalancePersistsReopensAndExportsExactPixels)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    auto imported = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    PreviewRequest preview;
    preview.asset_id = asset_id;
    preview.max_edge = 64U;
    preview.persist_preview_record = true;
    preview.prefer_embedded_preview = false;
    auto absent = service->request_preview(preview);
    ASSERT_TRUE(absent) << absent.error().message;

    auto baseline_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(baseline_recipe) << baseline_recipe.error().message;
    auto develop = develop_from_recipe(baseline_recipe.value());
    ASSERT_TRUE(develop) << develop.error().message;
    EXPECT_FALSE(develop.value().color_balance_enabled);
    develop.value().color_balance_enabled = true;
    develop.value().color_balance = ColorBalanceParams{};
    ASSERT_TRUE(service->save_develop(asset_id, develop.value()));

    auto stored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(stored_recipe) << stored_recipe.error().message;
    const auto operation = std::find_if(
        stored_recipe.value().operations.begin(), stored_recipe.value().operations.end(),
        [](const OperationInstance &item) { return item.id == kColorBalanceOperationId; });
    ASSERT_NE(operation, stored_recipe.value().operations.end());
    EXPECT_TRUE(operation->enabled);
    EXPECT_EQ(operation->schema_version, kColorBalanceOperationSchemaVersion);
    EXPECT_EQ(operation->parameters.size(), 19U);
    auto decoded = color_balance_from_parameters(operation->parameters);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value(), ColorBalanceParams{});

    auto before_reopen = service->request_preview(preview);
    ASSERT_TRUE(before_reopen) << before_reopen.error().message;
    EXPECT_NE(before_reopen.value().cache_key, absent.value().cache_key);
    ASSERT_FALSE(before_reopen.value().cache_path.empty());
    const QImage before_reopen_image(QString::fromStdString(before_reopen.value().cache_path));
    ASSERT_FALSE(before_reopen_image.isNull());

    const auto export_path = (root / "legacy-colorbalance-default.png").string();
    ExportRequest export_request;
    export_request.asset_id = asset_id;
    export_request.output_path = export_path;
    export_request.format = ExportFormat::kPng;
    export_request.max_edge = 64U;
    auto exported = service->export_asset(export_request);
    ASSERT_TRUE(exported) << exported.error().message;
    const QImage export_image(QString::fromStdString(export_path));
    ASSERT_FALSE(export_image.isNull());
    ASSERT_EQ(export_image.size(), before_reopen_image.size());
    for (int y = 0; y < export_image.height(); ++y)
    {
        for (int x = 0; x < export_image.width(); ++x)
        {
            EXPECT_EQ(export_image.pixel(x, y), before_reopen_image.pixel(x, y)) << x << ',' << y;
        }
    }

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto restored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(restored_recipe) << restored_recipe.error().message;
    auto restored = develop_from_recipe(restored_recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_TRUE(restored.value().color_balance_enabled);
    EXPECT_EQ(restored.value().color_balance, ColorBalanceParams{});
    auto after_reopen = service->request_preview(preview);
    ASSERT_TRUE(after_reopen) << after_reopen.error().message;
    EXPECT_EQ(after_reopen.value().cache_key, before_reopen.value().cache_key);
    const QImage after_reopen_image(QString::fromStdString(after_reopen.value().cache_path));
    ASSERT_FALSE(after_reopen_image.isNull());
    EXPECT_EQ(after_reopen_image, before_reopen_image);
}

TEST_F(CatalogServiceTest, ExplicitDefaultColorCheckerPersistsReopensAndExportsExactPixels)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    auto imported = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    PreviewRequest preview;
    preview.asset_id = asset_id;
    preview.max_edge = 64U;
    preview.persist_preview_record = true;
    preview.prefer_embedded_preview = false;
    auto absent = service->request_preview(preview);
    ASSERT_TRUE(absent) << absent.error().message;

    auto baseline_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(baseline_recipe) << baseline_recipe.error().message;
    auto develop = develop_from_recipe(baseline_recipe.value());
    ASSERT_TRUE(develop) << develop.error().message;
    EXPECT_FALSE(develop.value().color_checker_enabled);
    develop.value().color_checker_enabled = true;
    develop.value().color_checker = ColorCheckerParams{};
    ASSERT_TRUE(service->save_develop(asset_id, develop.value()));

    auto stored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(stored_recipe) << stored_recipe.error().message;
    const auto operation = std::find_if(
        stored_recipe.value().operations.begin(), stored_recipe.value().operations.end(),
        [](const OperationInstance &item) { return item.id == kColorCheckerOperationId; });
    ASSERT_NE(operation, stored_recipe.value().operations.end());
    EXPECT_TRUE(operation->enabled);
    EXPECT_EQ(operation->schema_version, kColorCheckerOperationSchemaVersion);
    EXPECT_EQ(operation->parameters.size(), 3U);
    auto decoded = color_checker_from_parameters(operation->parameters);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value(), ColorCheckerParams{});

    auto before_reopen = service->request_preview(preview);
    ASSERT_TRUE(before_reopen) << before_reopen.error().message;
    EXPECT_NE(before_reopen.value().cache_key, absent.value().cache_key);
    ASSERT_FALSE(before_reopen.value().cache_path.empty());
    const QImage before_reopen_image(QString::fromStdString(before_reopen.value().cache_path));
    ASSERT_FALSE(before_reopen_image.isNull());

    const auto export_path = (root / "colorchecker-default.png").string();
    ExportRequest export_request;
    export_request.asset_id = asset_id;
    export_request.output_path = export_path;
    export_request.format = ExportFormat::kPng;
    export_request.max_edge = 64U;
    auto exported = service->export_asset(export_request);
    ASSERT_TRUE(exported) << exported.error().message;
    const QImage export_image(QString::fromStdString(export_path));
    ASSERT_FALSE(export_image.isNull());
    EXPECT_EQ(export_image, before_reopen_image);

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto restored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(restored_recipe) << restored_recipe.error().message;
    auto restored = develop_from_recipe(restored_recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_TRUE(restored.value().color_checker_enabled);
    EXPECT_EQ(restored.value().color_checker, ColorCheckerParams{});
    auto after_reopen = service->request_preview(preview);
    ASSERT_TRUE(after_reopen) << after_reopen.error().message;
    EXPECT_EQ(after_reopen.value().cache_key, before_reopen.value().cache_key);
    const QImage after_reopen_image(QString::fromStdString(after_reopen.value().cache_path));
    ASSERT_FALSE(after_reopen_image.isNull());
    EXPECT_EQ(after_reopen_image, before_reopen_image);
}

TEST_F(CatalogServiceTest, ExplicitDefaultColorCorrectionPersistsReopensAndExportsExactPixels)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    auto imported = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    PreviewRequest preview;
    preview.asset_id = asset_id;
    preview.max_edge = 64U;
    preview.persist_preview_record = true;
    preview.prefer_embedded_preview = false;
    auto absent = service->request_preview(preview);
    ASSERT_TRUE(absent) << absent.error().message;
    ASSERT_FALSE(absent.value().cache_path.empty());

    auto baseline_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(baseline_recipe) << baseline_recipe.error().message;
    auto develop = develop_from_recipe(baseline_recipe.value());
    ASSERT_TRUE(develop) << develop.error().message;
    EXPECT_FALSE(develop.value().color_correction_enabled);
    develop.value().color_correction_enabled = true;
    develop.value().color_correction = ColorCorrectionParams{};
    ASSERT_TRUE(service->save_develop(asset_id, develop.value()));

    auto stored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(stored_recipe) << stored_recipe.error().message;
    const auto operation = std::find_if(
        stored_recipe.value().operations.begin(), stored_recipe.value().operations.end(),
        [](const OperationInstance &item) { return item.id == kColorCorrectionOperationId; });
    ASSERT_NE(operation, stored_recipe.value().operations.end());
    EXPECT_TRUE(operation->enabled);
    EXPECT_EQ(operation->schema_version, kColorCorrectionOperationSchemaVersion);
    EXPECT_EQ(operation->parameters.size(), 7U);
    auto decoded = color_correction_from_parameters(operation->parameters);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value(), ColorCorrectionParams{});

    auto before_reopen = service->request_preview(preview);
    ASSERT_TRUE(before_reopen) << before_reopen.error().message;
    EXPECT_NE(before_reopen.value().cache_key, absent.value().cache_key);
    ASSERT_FALSE(before_reopen.value().cache_path.empty());
    const QImage before_reopen_image(QString::fromStdString(before_reopen.value().cache_path));
    ASSERT_FALSE(before_reopen_image.isNull());

    const auto export_path = (root / "colorcorrection-default.png").string();
    ExportRequest export_request;
    export_request.asset_id = asset_id;
    export_request.output_path = export_path;
    export_request.format = ExportFormat::kPng;
    export_request.max_edge = 64U;
    auto exported = service->export_asset(export_request);
    ASSERT_TRUE(exported) << exported.error().message;
    const QImage export_image(QString::fromStdString(export_path));
    ASSERT_FALSE(export_image.isNull());
    EXPECT_EQ(export_image, before_reopen_image);

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto restored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(restored_recipe) << restored_recipe.error().message;
    auto restored = develop_from_recipe(restored_recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_TRUE(restored.value().color_correction_enabled);
    EXPECT_EQ(restored.value().color_correction, ColorCorrectionParams{});
    auto after_reopen = service->request_preview(preview);
    ASSERT_TRUE(after_reopen) << after_reopen.error().message;
    EXPECT_EQ(after_reopen.value().cache_key, before_reopen.value().cache_key);
    const QImage after_reopen_image(QString::fromStdString(after_reopen.value().cache_path));
    ASSERT_FALSE(after_reopen_image.isNull());
    EXPECT_EQ(after_reopen_image, before_reopen_image);
}

TEST_F(CatalogServiceTest, ExplicitDefaultColorContrastPersistsReopensAndExportsExactPixels)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    auto imported = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    PreviewRequest preview;
    preview.asset_id = asset_id;
    preview.max_edge = 64U;
    preview.persist_preview_record = true;
    preview.prefer_embedded_preview = false;
    auto absent = service->request_preview(preview);
    ASSERT_TRUE(absent) << absent.error().message;
    ASSERT_FALSE(absent.value().cache_path.empty());

    auto baseline_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(baseline_recipe) << baseline_recipe.error().message;
    auto develop = develop_from_recipe(baseline_recipe.value());
    ASSERT_TRUE(develop) << develop.error().message;
    EXPECT_FALSE(develop.value().color_contrast_enabled);
    develop.value().color_contrast_enabled = true;
    develop.value().color_contrast = ColorContrastParams{};
    ASSERT_TRUE(service->save_develop(asset_id, develop.value()));

    auto stored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(stored_recipe) << stored_recipe.error().message;
    const auto operation = std::find_if(
        stored_recipe.value().operations.begin(), stored_recipe.value().operations.end(),
        [](const OperationInstance &item) { return item.id == kColorContrastOperationId; });
    ASSERT_NE(operation, stored_recipe.value().operations.end());
    EXPECT_TRUE(operation->enabled);
    EXPECT_EQ(operation->schema_version, kColorContrastOperationSchemaVersion);
    EXPECT_EQ(operation->parameters.size(), 7U);
    auto decoded = color_contrast_from_parameters(operation->parameters);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value(), ColorContrastParams{});

    auto before_reopen = service->request_preview(preview);
    ASSERT_TRUE(before_reopen) << before_reopen.error().message;
    EXPECT_NE(before_reopen.value().cache_key, absent.value().cache_key);
    ASSERT_FALSE(before_reopen.value().cache_path.empty());
    const QImage before_reopen_image(QString::fromStdString(before_reopen.value().cache_path));
    ASSERT_FALSE(before_reopen_image.isNull());

    const auto export_path = (root / "colorcontrast-default.png").string();
    ExportRequest export_request;
    export_request.asset_id = asset_id;
    export_request.output_path = export_path;
    export_request.format = ExportFormat::kPng;
    export_request.max_edge = 64U;
    auto exported = service->export_asset(export_request);
    ASSERT_TRUE(exported) << exported.error().message;
    const QImage export_image(QString::fromStdString(export_path));
    ASSERT_FALSE(export_image.isNull());
    EXPECT_EQ(export_image, before_reopen_image);

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto restored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(restored_recipe) << restored_recipe.error().message;
    auto restored = develop_from_recipe(restored_recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_TRUE(restored.value().color_contrast_enabled);
    EXPECT_EQ(restored.value().color_contrast, ColorContrastParams{});
    auto after_reopen = service->request_preview(preview);
    ASSERT_TRUE(after_reopen) << after_reopen.error().message;
    EXPECT_EQ(after_reopen.value().cache_key, before_reopen.value().cache_key);
    const QImage after_reopen_image(QString::fromStdString(after_reopen.value().cache_path));
    ASSERT_FALSE(after_reopen_image.isNull());
    EXPECT_EQ(after_reopen_image, before_reopen_image);
}

TEST_F(CatalogServiceTest, ExplicitDefaultColorHarmonizerPersistsReopensAndExportsExactPixels)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    auto imported = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    PreviewRequest preview;
    preview.asset_id = asset_id;
    preview.max_edge = 64U;
    preview.persist_preview_record = true;
    preview.prefer_embedded_preview = false;
    auto absent = service->request_preview(preview);
    ASSERT_TRUE(absent) << absent.error().message;
    ASSERT_FALSE(absent.value().cache_path.empty());

    auto baseline_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(baseline_recipe) << baseline_recipe.error().message;
    auto develop = develop_from_recipe(baseline_recipe.value());
    ASSERT_TRUE(develop) << develop.error().message;
    EXPECT_FALSE(develop.value().color_harmonizer_enabled);
    develop.value().color_harmonizer_enabled = true;
    develop.value().color_harmonizer = ColorHarmonizerParams{};
    ASSERT_TRUE(service->save_develop(asset_id, develop.value()));

    auto stored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(stored_recipe) << stored_recipe.error().message;
    const auto operation = std::find_if(
        stored_recipe.value().operations.begin(), stored_recipe.value().operations.end(),
        [](const OperationInstance &item) { return item.id == kColorHarmonizerOperationId; });
    ASSERT_NE(operation, stored_recipe.value().operations.end());
    EXPECT_TRUE(operation->enabled);
    EXPECT_EQ(operation->schema_version, kColorHarmonizerOperationSchemaVersion);
    EXPECT_EQ(operation->parameters.size(), 17U);
    auto decoded = color_harmonizer_from_parameters(operation->parameters);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value(), ColorHarmonizerParams{});

    auto before_reopen = service->request_preview(preview);
    ASSERT_TRUE(before_reopen) << before_reopen.error().message;
    EXPECT_NE(before_reopen.value().cache_key, absent.value().cache_key);
    ASSERT_FALSE(before_reopen.value().cache_path.empty());
    const QImage before_reopen_image(QString::fromStdString(before_reopen.value().cache_path));
    ASSERT_FALSE(before_reopen_image.isNull());

    const auto export_path = (root / "colorharmonizer-default.png").string();
    ExportRequest export_request;
    export_request.asset_id = asset_id;
    export_request.output_path = export_path;
    export_request.format = ExportFormat::kPng;
    export_request.max_edge = 64U;
    auto exported = service->export_asset(export_request);
    ASSERT_TRUE(exported) << exported.error().message;
    const QImage export_image(QString::fromStdString(export_path));
    ASSERT_FALSE(export_image.isNull());
    EXPECT_EQ(export_image, before_reopen_image);

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto restored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(restored_recipe) << restored_recipe.error().message;
    auto restored = develop_from_recipe(restored_recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_TRUE(restored.value().color_harmonizer_enabled);
    EXPECT_EQ(restored.value().color_harmonizer, ColorHarmonizerParams{});
    auto after_reopen = service->request_preview(preview);
    ASSERT_TRUE(after_reopen) << after_reopen.error().message;
    EXPECT_EQ(after_reopen.value().cache_key, before_reopen.value().cache_key);
    const QImage after_reopen_image(QString::fromStdString(after_reopen.value().cache_path));
    ASSERT_FALSE(after_reopen_image.isNull());
    EXPECT_EQ(after_reopen_image, before_reopen_image);
}

TEST_F(CatalogServiceTest, ColorReconstructionPersistsReopensAndExportsExactPixels)
{
    const auto source_hash = file_sha256(raw_fixture_path());
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    auto imported = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    PreviewRequest preview;
    preview.asset_id = asset_id;
    preview.max_edge = 64U;
    preview.persist_preview_record = true;
    preview.prefer_embedded_preview = false;
    auto absent = service->request_preview(preview);
    ASSERT_TRUE(absent) << absent.error().message;
    const QImage absent_image(QString::fromStdString(absent.value().cache_path));
    ASSERT_FALSE(absent_image.isNull());

    auto baseline_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(baseline_recipe) << baseline_recipe.error().message;
    auto develop = develop_from_recipe(baseline_recipe.value());
    ASSERT_TRUE(develop) << develop.error().message;
    EXPECT_FALSE(develop.value().color_reconstruction_enabled);
    develop.value().color_reconstruction_enabled = true;
    develop.value().color_reconstruction = ColorReconstructionParams{
        60.0, 300.0, 10.0, 0.6600000262260437, ColorReconstructionPrecedence::kChroma};
    ASSERT_TRUE(service->save_develop(asset_id, develop.value()));

    auto stored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(stored_recipe) << stored_recipe.error().message;
    const auto operation = std::find_if(
        stored_recipe.value().operations.begin(), stored_recipe.value().operations.end(),
        [](const OperationInstance &item) { return item.id == kColorReconstructionOperationId; });
    ASSERT_NE(operation, stored_recipe.value().operations.end());
    EXPECT_TRUE(operation->enabled);
    EXPECT_EQ(operation->schema_version, kColorReconstructionOperationSchemaVersion);
    auto decoded = color_reconstruction_from_parameters(operation->parameters);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value(), develop.value().color_reconstruction);
    ASSERT_NE(std::next(operation), stored_recipe.value().operations.end());
    EXPECT_EQ(std::next(operation)->id, "ravo.color.output");

    auto before_reopen = service->request_preview(preview);
    ASSERT_TRUE(before_reopen) << before_reopen.error().message;
    EXPECT_NE(before_reopen.value().cache_key, absent.value().cache_key);
    const QImage before_reopen_image(QString::fromStdString(before_reopen.value().cache_path));
    ASSERT_FALSE(before_reopen_image.isNull());
    EXPECT_NE(before_reopen_image, absent_image);

    const auto export_path = (root / "colorreconstruct.png").string();
    ExportRequest export_request;
    export_request.asset_id = asset_id;
    export_request.output_path = export_path;
    export_request.format = ExportFormat::kPng;
    export_request.max_edge = 64U;
    auto exported = service->export_asset(export_request);
    ASSERT_TRUE(exported) << exported.error().message;
    const QImage export_image(QString::fromStdString(export_path));
    ASSERT_FALSE(export_image.isNull());
    EXPECT_EQ(export_image, before_reopen_image);

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto restored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(restored_recipe) << restored_recipe.error().message;
    auto restored = develop_from_recipe(restored_recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_TRUE(restored.value().color_reconstruction_enabled);
    EXPECT_EQ(restored.value().color_reconstruction, develop.value().color_reconstruction);
    auto after_reopen = service->request_preview(preview);
    ASSERT_TRUE(after_reopen) << after_reopen.error().message;
    EXPECT_EQ(after_reopen.value().cache_key, before_reopen.value().cache_key);
    const QImage after_reopen_image(QString::fromStdString(after_reopen.value().cache_path));
    ASSERT_FALSE(after_reopen_image.isNull());
    EXPECT_EQ(after_reopen_image, before_reopen_image);
    EXPECT_EQ(file_sha256(raw_fixture_path()), source_hash);
}

TEST_F(CatalogServiceTest, SourceExactSharpenPersistsReopensAndExportsExactPixels)
{
    const auto source_hash = file_sha256(raw_fixture_path());
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    auto imported = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    PreviewRequest preview;
    preview.asset_id = asset_id;
    preview.max_edge = 128U;
    preview.persist_preview_record = true;
    preview.prefer_embedded_preview = false;
    auto absent = service->request_preview(preview);
    ASSERT_TRUE(absent) << absent.error().message;
    const QImage absent_image(QString::fromStdString(absent.value().cache_path));
    ASSERT_FALSE(absent_image.isNull());

    auto baseline_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(baseline_recipe) << baseline_recipe.error().message;
    auto develop = develop_from_recipe(baseline_recipe.value());
    ASSERT_TRUE(develop) << develop.error().message;
    develop.value().sharpen = 1.0;
    develop.value().sharpen_radius = 99.0;
    develop.value().sharpen_threshold = 0.0;
    ASSERT_TRUE(service->save_develop(asset_id, develop.value()));

    auto stored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(stored_recipe) << stored_recipe.error().message;
    const auto operation = std::find_if(
        stored_recipe.value().operations.begin(), stored_recipe.value().operations.end(),
        [](const OperationInstance &item) { return item.id == kSharpenOperationId; });
    ASSERT_NE(operation, stored_recipe.value().operations.end());
    EXPECT_EQ(operation->schema_version, kSharpenOperationSchemaVersion);
    auto decoded = sharpen_from_parameters(operation->parameters);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value(), (SharpenParams{99.0, 1.0, 0.0}));

    auto before_reopen = service->request_preview(preview);
    ASSERT_TRUE(before_reopen) << before_reopen.error().message;
    EXPECT_NE(before_reopen.value().cache_key, absent.value().cache_key);
    const QImage before_reopen_image(QString::fromStdString(before_reopen.value().cache_path));
    ASSERT_FALSE(before_reopen_image.isNull());
    EXPECT_NE(before_reopen_image, absent_image);

    const auto export_path = (root / "sharpen.png").string();
    ExportRequest export_request;
    export_request.asset_id = asset_id;
    export_request.output_path = export_path;
    export_request.format = ExportFormat::kPng;
    export_request.max_edge = 128U;
    auto exported = service->export_asset(export_request);
    ASSERT_TRUE(exported) << exported.error().message;
    const QImage export_image(QString::fromStdString(export_path));
    ASSERT_FALSE(export_image.isNull());
    EXPECT_EQ(export_image, before_reopen_image);

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto restored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(restored_recipe) << restored_recipe.error().message;
    auto restored = develop_from_recipe(restored_recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_DOUBLE_EQ(restored.value().sharpen, 1.0);
    EXPECT_DOUBLE_EQ(restored.value().sharpen_radius, 99.0);
    EXPECT_DOUBLE_EQ(restored.value().sharpen_threshold, 0.0);
    auto after_reopen = service->request_preview(preview);
    ASSERT_TRUE(after_reopen) << after_reopen.error().message;
    EXPECT_EQ(after_reopen.value().cache_key, before_reopen.value().cache_key);
    const QImage after_reopen_image(QString::fromStdString(after_reopen.value().cache_path));
    ASSERT_FALSE(after_reopen_image.isNull());
    EXPECT_EQ(after_reopen_image, before_reopen_image);
    EXPECT_EQ(file_sha256(raw_fixture_path()), source_hash);
}

TEST_F(CatalogServiceTest, DarkChannelDehazePersistsReopensAndExportsExactPixels)
{
    const auto source_hash = file_sha256(raw_fixture_path());
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    auto imported = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    PreviewRequest preview;
    preview.asset_id = asset_id;
    preview.max_edge = 128U;
    preview.persist_preview_record = true;
    preview.prefer_embedded_preview = false;
    auto absent = service->request_preview(preview);
    ASSERT_TRUE(absent) << absent.error().message;
    const QImage absent_image(QString::fromStdString(absent.value().cache_path));
    ASSERT_FALSE(absent_image.isNull());

    auto baseline_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(baseline_recipe) << baseline_recipe.error().message;
    auto develop = develop_from_recipe(baseline_recipe.value());
    ASSERT_TRUE(develop) << develop.error().message;
    develop.value().dehaze = 0.9;
    develop.value().dehaze_distance = 0.8;
    develop.value().dehaze_adaptive = false;
    ASSERT_TRUE(service->save_develop(asset_id, develop.value()));

    auto stored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(stored_recipe) << stored_recipe.error().message;
    const auto operation = std::find_if(
        stored_recipe.value().operations.begin(), stored_recipe.value().operations.end(),
        [](const OperationInstance &item) { return item.id == kDehazeOperationId; });
    ASSERT_NE(operation, stored_recipe.value().operations.end());
    EXPECT_EQ(operation->schema_version, kDehazeOperationSchemaVersion);
    auto decoded = dehaze_from_parameters(operation->parameters);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value(), (DehazeParams{0.9, 0.8, false}));

    auto before_reopen = service->request_preview(preview);
    ASSERT_TRUE(before_reopen) << before_reopen.error().message;
    EXPECT_NE(before_reopen.value().cache_key, absent.value().cache_key);
    const QImage before_reopen_image(QString::fromStdString(before_reopen.value().cache_path));
    ASSERT_FALSE(before_reopen_image.isNull());
    EXPECT_NE(before_reopen_image, absent_image);

    const auto export_path = (root / "dehaze.png").string();
    ExportRequest export_request;
    export_request.asset_id = asset_id;
    export_request.output_path = export_path;
    export_request.format = ExportFormat::kPng;
    export_request.max_edge = 128U;
    auto exported = service->export_asset(export_request);
    ASSERT_TRUE(exported) << exported.error().message;
    const QImage export_image(QString::fromStdString(export_path));
    ASSERT_FALSE(export_image.isNull());
    EXPECT_EQ(export_image, before_reopen_image);

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto restored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(restored_recipe) << restored_recipe.error().message;
    auto restored = develop_from_recipe(restored_recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_DOUBLE_EQ(restored.value().dehaze, 0.9);
    EXPECT_DOUBLE_EQ(restored.value().dehaze_distance, 0.8);
    EXPECT_FALSE(restored.value().dehaze_adaptive);
    auto after_reopen = service->request_preview(preview);
    ASSERT_TRUE(after_reopen) << after_reopen.error().message;
    EXPECT_EQ(after_reopen.value().cache_key, before_reopen.value().cache_key);
    const QImage after_reopen_image(QString::fromStdString(after_reopen.value().cache_path));
    ASSERT_FALSE(after_reopen_image.isNull());
    EXPECT_EQ(after_reopen_image, before_reopen_image);
    EXPECT_EQ(file_sha256(raw_fixture_path()), source_hash);
}

TEST_F(CatalogServiceTest, OrderedRetouchPersistsReopensAndExportsExactPixels)
{
    const auto source_hash = file_sha256(raw_fixture_path());
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    auto imported = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    PreviewRequest preview;
    preview.asset_id = asset_id;
    preview.max_edge = 64U;
    preview.persist_preview_record = true;
    preview.prefer_embedded_preview = false;
    auto absent = service->request_preview(preview);
    ASSERT_TRUE(absent) << absent.error().message;
    const QImage absent_image(QString::fromStdString(absent.value().cache_path));
    ASSERT_FALSE(absent_image.isNull());

    auto baseline_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(baseline_recipe) << baseline_recipe.error().message;
    auto develop = develop_from_recipe(baseline_recipe.value());
    ASSERT_TRUE(develop) << develop.error().message;
    Mask spot{"catalog-retouch-spot", kCanonicalMaskSchemaVersion, MaskKind::kCircle};
    spot.payload = CircleMask{0.5, 0.5, 0.16, 0.04};
    develop.value().masks.push_back(spot);
    RetouchRegion region;
    region.mask_id = spot.id;
    region.mode = RetouchMode::kFill;
    region.opacity = 0.85;
    region.fill_mode = RetouchFillMode::kColor;
    region.fill_color = {0.9, 0.15, 0.05};
    region.fill_brightness = -0.03;
    develop.value().retouch.regions.push_back(region);
    ASSERT_TRUE(service->save_develop(asset_id, develop.value()));

    auto stored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(stored_recipe) << stored_recipe.error().message;
    const auto operation = std::find_if(
        stored_recipe.value().operations.begin(), stored_recipe.value().operations.end(),
        [](const OperationInstance &item) { return item.id == kRetouchOperationId; });
    ASSERT_NE(operation, stored_recipe.value().operations.end());
    auto decoded = retouch_from_parameters(operation->parameters);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value(), develop.value().retouch);

    auto before_reopen = service->request_preview(preview);
    ASSERT_TRUE(before_reopen) << before_reopen.error().message;
    EXPECT_NE(before_reopen.value().cache_key, absent.value().cache_key);
    const QImage before_reopen_image(QString::fromStdString(before_reopen.value().cache_path));
    ASSERT_FALSE(before_reopen_image.isNull());
    EXPECT_NE(before_reopen_image, absent_image);

    const auto export_path = (root / "retouch.png").string();
    ExportRequest export_request;
    export_request.asset_id = asset_id;
    export_request.output_path = export_path;
    export_request.format = ExportFormat::kPng;
    export_request.max_edge = 64U;
    auto exported = service->export_asset(export_request);
    ASSERT_TRUE(exported) << exported.error().message;
    const QImage export_image(QString::fromStdString(export_path));
    ASSERT_FALSE(export_image.isNull());
    EXPECT_EQ(export_image, before_reopen_image);

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto restored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(restored_recipe) << restored_recipe.error().message;
    auto restored = develop_from_recipe(restored_recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_EQ(restored.value().retouch, develop.value().retouch);
    EXPECT_EQ(restored.value().masks, develop.value().masks);
    auto after_reopen = service->request_preview(preview);
    ASSERT_TRUE(after_reopen) << after_reopen.error().message;
    EXPECT_EQ(after_reopen.value().cache_key, before_reopen.value().cache_key);
    const QImage after_reopen_image(QString::fromStdString(after_reopen.value().cache_path));
    ASSERT_FALSE(after_reopen_image.isNull());
    EXPECT_EQ(after_reopen_image, before_reopen_image);
    EXPECT_EQ(file_sha256(raw_fixture_path()), source_hash);
}

TEST_F(CatalogServiceTest, PositiveColorHarmonizerSmoothingPersistsReopensAndExports)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    auto imported = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    auto baseline_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(baseline_recipe) << baseline_recipe.error().message;
    const auto baseline_serialized = serialize_recipe(baseline_recipe.value());
    ASSERT_TRUE(baseline_serialized) << baseline_serialized.error().message;
    auto develop = develop_from_recipe(baseline_recipe.value());
    ASSERT_TRUE(develop) << develop.error().message;
    const DevelopParams before_reject = develop.value();
    EXPECT_FALSE(apply_develop_field_strict(develop.value(), "colorHarmonizerPullStrength", 1.5));
    EXPECT_EQ(develop.value(), before_reject);
    auto still_baseline = service->load_recipe(asset_id);
    ASSERT_TRUE(still_baseline) << still_baseline.error().message;
    auto still_serialized = serialize_recipe(still_baseline.value());
    ASSERT_TRUE(still_serialized) << still_serialized.error().message;
    EXPECT_EQ(still_serialized.value(), baseline_serialized.value());

    ColorHarmonizerParams edited;
    edited.rule = ColorHarmonizerRule::kSplitComplementary;
    edited.anchor_hue = 0.55000001192092896;
    edited.pull_strength = 0.81999999284744263;
    edited.pull_width = 1.8400000333786011;
    edited.node_saturation = {1.2599999904632568, 0.18000000715255737, 1.5199999809265137, 1.0};
    edited.smoothing = 0.5;
    develop.value().color_harmonizer_enabled = true;
    develop.value().color_harmonizer = edited;
    ASSERT_TRUE(service->save_develop(asset_id, develop.value()));

    PreviewRequest preview;
    preview.asset_id = asset_id;
    preview.max_edge = 64U;
    preview.persist_preview_record = true;
    preview.prefer_embedded_preview = false;
    auto live = service->request_preview(preview);
    ASSERT_TRUE(live) << live.error().message;
    const auto cache_key = live.value().cache_key;

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto restored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(restored_recipe) << restored_recipe.error().message;
    auto restored = develop_from_recipe(restored_recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_TRUE(restored.value().color_harmonizer_enabled);
    EXPECT_EQ(restored.value().color_harmonizer, edited);
    auto after = service->request_preview(preview);
    ASSERT_TRUE(after) << after.error().message;
    EXPECT_EQ(after.value().cache_key, cache_key);
    const auto export_path = (root / "colorharmonizer-0176.png").string();
    ExportRequest export_request;
    export_request.asset_id = asset_id;
    export_request.output_path = export_path;
    export_request.format = ExportFormat::kPng;
    export_request.max_edge = 64U;
    auto exported = service->export_asset(export_request);
    ASSERT_TRUE(exported) << exported.error().message;
    const QImage export_image(QString::fromStdString(export_path));
    const QImage preview_image(QString::fromStdString(after.value().cache_path));
    ASSERT_FALSE(export_image.isNull());
    ASSERT_FALSE(preview_image.isNull());
    EXPECT_EQ(export_image, preview_image);
}

TEST_F(CatalogServiceTest, MigratedDevelopControlsPersistAndReproducePixelsAfterReopen)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    auto imported = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    auto baseline_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(baseline_recipe) << baseline_recipe.error().message;
    auto edited = develop_from_recipe(baseline_recipe.value());
    ASSERT_TRUE(edited) << edited.error().message;
    edited.value().raw_highlights = 0.35;
    edited.value().hot_pixels_strength = 0.25;
    edited.value().hot_pixels_threshold = 0.05;
    edited.value().hot_pixels_permissive = true;
    edited.value().raw_ca_iterations = 1;
    edited.value().raw_ca_avoid_shift = false;
    edited.value().raw_highlights_mode = std::string(kRawHighlightsModeOpposed);
    edited.value().denoise = 0.2;
    edited.value().denoise_chroma = 0.7;
    edited.value().denoise_radius = 1.5;
    edited.value().lens_mode = std::string(kLensModeManual);
    edited.value().lens_k1 = -0.04;
    edited.value().lens_vignetting = 0.15;
    // The selected Inspector band is transient presentation state; all eight algorithm bands
    // are canonical recipe data.
    edited.value().color_eq_band = 0;
    edited.value().color_eq_hue[2] = 0.1;
    edited.value().color_eq_sat[2] = 0.25;
    edited.value().color_eq_light[2] = -0.15;
    edited.value().graduated_density = 0.4;
    edited.value().graduated_hardness = 0.65;
    edited.value().graduated_rotation = 15.0;
    edited.value().graduated_offset = -0.1;
    edited.value().tone_eq_blacks = -0.2;
    edited.value().tone_eq_shadows = 0.15;
    edited.value().tone_eq_midtones = 0.25;
    edited.value().tone_eq_highlights = -0.1;
    edited.value().tone_eq_whites = -0.3;
    edited.value().channel_mixer.red = {0.95, 0.05, 0.0};
    edited.value().channel_mixer.green = {0.02, 0.96, 0.02};
    edited.value().channel_mixer.blue = {0.0, 0.08, 0.92};
    edited.value().temperature = test::temperature_0000_params();
    edited.value().input_color.input_profile = std::string(kInputProfileEnhancedMatrix);
    edited.value().input_color.rendering_intent = std::string(kColorIntentRelative);
    edited.value().input_color.gamut_normalize = std::string(kColorNormalizeSrgb);
    edited.value().input_color.blue_mapping = true;
    edited.value().input_color.working_profile = std::string(kInputProfileLinearRec2020);
    edited.value().output_color.output_profile = std::string(kInputProfileDisplayP3);
    edited.value().output_color.rendering_intent = std::string(kColorIntentRelative);
    edited.value().color_balance_rgb = test::color_balance_0093_params();
    edited.value().straighten_degrees = 2.0;
    edited.value().perspective_vertical = 0.08;
    edited.value().perspective_horizontal = -0.05;
    edited.value().perspective_shear = 0.015;
    edited.value().perspective_constrain_crop = true;
    edited.value().perspective_interpolation_index = 2;
    clamp_develop(edited.value());

    PreviewRequest preview;
    preview.asset_id = asset_id;
    preview.max_edge = 96;
    preview.persist_preview_record = false;
    auto baseline = service->request_preview(preview);
    ASSERT_TRUE(baseline) << baseline.error().message;
    ASSERT_FALSE(baseline.value().rgb.empty());

    auto saved = service->save_develop(asset_id, edited.value());
    ASSERT_TRUE(saved) << saved.error().message;
    auto before_reopen = service->request_preview(preview);
    ASSERT_TRUE(before_reopen) << before_reopen.error().message;
    ASSERT_FALSE(before_reopen.value().rgb.empty());
    EXPECT_EQ(before_reopen.value().color_profile.identifier, kInputProfileDisplayP3);
    EXPECT_FALSE(before_reopen.value().color_profile.icc_bytes.empty());
    EXPECT_NE(before_reopen.value().rgb, baseline.value().rgb);

    ASSERT_TRUE(service->close());
    service.reset();
    auto reopened = open_service(false);
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto restored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(restored_recipe) << restored_recipe.error().message;
    auto restored = develop_from_recipe(restored_recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    auto comparable = restored.value();
    // ADR-0145: a singleton recipe op loads as one instance while empty
    // instance vectors remain the in-memory legacy form.
    if (edited.value().exposure_instances.empty() && comparable.exposure_instances.size() == 1U)
    {
        EXPECT_EQ(comparable.exposure_instances.front().exposure_ev, comparable.exposure_ev);
        EXPECT_EQ(comparable.exposure_instance_id_high_water, 1U);
        comparable.exposure_instances.clear();
        comparable.exposure_instance_id_high_water = edited.value().exposure_instance_id_high_water;
    }
    if (edited.value().color_balance_rgb_instances.empty() &&
        comparable.color_balance_rgb_instances.size() == 1U)
    {
        EXPECT_EQ(comparable.color_balance_rgb_instances.front().params,
                  comparable.color_balance_rgb);
        EXPECT_EQ(comparable.color_balance_rgb_instance_id_high_water, 1U);
        comparable.color_balance_rgb_instances.clear();
        comparable.color_balance_rgb_instance_id_high_water =
            edited.value().color_balance_rgb_instance_id_high_water;
    }
    EXPECT_EQ(comparable, edited.value());

    auto after_reopen = service->request_preview(preview);
    ASSERT_TRUE(after_reopen) << after_reopen.error().message;
    EXPECT_EQ(after_reopen.value().rgb, before_reopen.value().rgb);
    EXPECT_EQ(after_reopen.value().color_profile, before_reopen.value().color_profile);

    PreviewRequest settled = preview;
    settled.persist_preview_record = true;
    auto settled_preview = service->request_preview(settled);
    ASSERT_TRUE(settled_preview) << settled_preview.error().message;
    const auto export_path = (root / "migrated-develop-perspective.png").string();
    ExportRequest export_request;
    export_request.asset_id = asset_id;
    export_request.output_path = export_path;
    export_request.format = ExportFormat::kPng;
    export_request.max_edge = preview.max_edge;
    auto exported = service->export_asset(export_request);
    ASSERT_TRUE(exported) << exported.error().message;
    const QImage preview_image(QString::fromStdString(settled_preview.value().cache_path));
    const QImage export_image(QString::fromStdString(export_path));
    ASSERT_FALSE(preview_image.isNull());
    ASSERT_FALSE(export_image.isNull());
    EXPECT_EQ(export_image, preview_image);
}

TEST_F(CatalogServiceTest, IgnoreStraightenRemovesCanonicalPerspectiveForAnalysis)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto jpeg_path = (root / "tilt.jpg").string();
    QImage image(48, 32, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(200, 80, 40));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    DevelopParams tilted;
    tilted.straighten_degrees = 20.0;
    tilted.perspective_vertical = 0.15;
    tilted.perspective_horizontal = -0.08;
    tilted.perspective_shear = 0.02;
    tilted.perspective_constrain_crop = false;
    PreviewRequest baked;
    baked.asset_id = asset_id;
    baked.max_edge = kInteractivePreviewMaxEdge;
    baked.persist_preview_record = false;
    auto straightened = service->request_preview(baked, tilted);
    ASSERT_TRUE(straightened) << straightened.error().message;
    ASSERT_FALSE(straightened.value().rgb.empty());
    PreviewRequest guide = baked;
    guide.ignore_straighten = true;
    auto unstraightened = service->request_preview(guide, tilted);
    ASSERT_TRUE(unstraightened) << unstraightened.error().message;
    ASSERT_FALSE(unstraightened.value().rgb.empty());
    EXPECT_EQ(unstraightened.value().width, 48U);
    EXPECT_EQ(unstraightened.value().height, 32U);
    EXPECT_TRUE(straightened.value().width != unstraightened.value().width ||
                straightened.value().height != unstraightened.value().height);
    EXPECT_LT(straightened.value().rgb[0] + straightened.value().rgb[1] +
                  straightened.value().rgb[2],
              unstraightened.value().rgb[0] + unstraightened.value().rgb[1] +
                  unstraightened.value().rgb[2]);
}

TEST_F(CatalogServiceTest, InvalidStoredRecipeFailsStructuredWithoutTouchingReview)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto jpeg_path = (root / "bad-recipe.jpg").string();
    QImage image(16, 16, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(10, 80, 10));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto asset_id = imported.value().asset->id;
    ASSERT_TRUE(service->set_rating(asset_id, 2));

    auto repository = SqliteCatalogRepository::open(database_path);
    ASSERT_TRUE(repository) << repository.error().message;
    ASSERT_TRUE(repository.value()->save_recipe_json(asset_id, 1, R"({"not":"a-recipe")"));
    ASSERT_TRUE(repository.value()->close());

    auto loaded = service->load_recipe(asset_id);
    ASSERT_FALSE(loaded);
    EXPECT_EQ(loaded.error().code, ErrorCode::kValidation);
    auto listed = service->list_assets();
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed.value().size(), 1U);
    EXPECT_EQ(listed.value().front().review.rating, 2);
    EXPECT_TRUE(listed.value().front().has_edits);
}

TEST_F(CatalogServiceTest, ExportJpegPngOriginalCopyConflictAndCancel)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto jpeg_path = (root / "source.jpg").string();
    QImage image(32, 24, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(40, 120, 200));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    const auto original_hash = file_sha256(jpeg_path);
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    const auto export_profile_path = root / "export-display-p3.icc";
    const QByteArray export_profile = QColorSpace(QColorSpace::DisplayP3).iccProfile();
    ASSERT_FALSE(export_profile.isEmpty());
    QFile profile_file(QString::fromStdString(export_profile_path.string()));
    ASSERT_TRUE(profile_file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(profile_file.write(export_profile), export_profile.size());
    profile_file.close();
    DevelopParams params;
    params.exposure_ev = 0.5;
    params.output_color.output_profile = std::string(kInputProfileFileIcc);
    params.output_color.output_profile_filename = export_profile_path.string();
    ASSERT_TRUE(service->save_develop(asset_id, params));

    const auto png_out = (root / "out.png").string();
    ExportRequest png;
    png.asset_id = asset_id;
    png.output_path = png_out;
    png.format = ExportFormat::kPng;
    auto exported_png = service->export_asset(png);
    ASSERT_TRUE(exported_png) << exported_png.error().message;
    EXPECT_EQ(exported_png.value().format, ExportFormat::kPng);
    EXPECT_GT(exported_png.value().bytes_written, 0U);
    EXPECT_TRUE(std::filesystem::exists(png_out));
    QImage read_png(QString::fromStdString(png_out));
    ASSERT_FALSE(read_png.isNull());
    EXPECT_EQ(read_png.width(), static_cast<int>(exported_png.value().width));
    EXPECT_EQ(read_png.height(), static_cast<int>(exported_png.value().height));
    EXPECT_TRUE(read_png.colorSpace().isValid());
    EXPECT_EQ(read_png.colorSpace(), QColorSpace(QColorSpace::DisplayP3));
    EXPECT_EQ(file_sha256(jpeg_path), original_hash);

    auto conflict = service->export_asset(png);
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().code, ErrorCode::kConflict);
    EXPECT_TRUE(std::filesystem::exists(png_out));
    const auto after_conflict = file_sha256(png_out);

    const auto jpeg_out = (root / "out.jpg").string();
    ExportRequest jpeg;
    jpeg.asset_id = asset_id;
    jpeg.output_path = jpeg_out;
    jpeg.format = ExportFormat::kJpeg;
    jpeg.jpeg_options.quality = 85;
    auto exported_jpeg = service->export_asset(jpeg);
    ASSERT_TRUE(exported_jpeg) << exported_jpeg.error().message;
    EXPECT_TRUE(std::filesystem::exists(jpeg_out));
    const QImage read_jpeg(QString::fromStdString(jpeg_out));
    EXPECT_FALSE(read_jpeg.isNull());
    EXPECT_TRUE(read_jpeg.colorSpace().isValid());
    EXPECT_EQ(read_jpeg.colorSpace(), QColorSpace(QColorSpace::DisplayP3));

    const auto tiff_out = (root / "out.tif").string();
    ExportRequest tiff;
    tiff.asset_id = asset_id;
    tiff.output_path = tiff_out;
    tiff.format = ExportFormat::kTiff;
    auto exported_tiff = service->export_asset(tiff);
    if (exported_tiff)
    {
        EXPECT_TRUE(std::filesystem::exists(tiff_out));
        const QImage read_tiff(QString::fromStdString(tiff_out));
        EXPECT_FALSE(read_tiff.isNull());
        EXPECT_TRUE(read_tiff.colorSpace().isValid());
        EXPECT_EQ(read_tiff.colorSpace(), QColorSpace(QColorSpace::DisplayP3));
    }
    else
    {
        EXPECT_EQ(exported_tiff.error().code, ErrorCode::kUnsupported);
        EXPECT_FALSE(std::filesystem::exists(tiff_out));
    }

    const auto copy_out = (root / "original-copy.jpg").string();
    ExportRequest copy;
    copy.asset_id = asset_id;
    copy.output_path = copy_out;
    copy.format = ExportFormat::kOriginalCopy;
    auto exported_copy = service->export_asset(copy);
    ASSERT_TRUE(exported_copy) << exported_copy.error().message;
    EXPECT_EQ(file_sha256(copy_out), original_hash);
    EXPECT_EQ(file_sha256(jpeg_path), original_hash);
    EXPECT_EQ(file_sha256(png_out), after_conflict);

    const auto cancelled_out = (root / "cancelled.png").string();
    CancellationSource cancelled;
    ASSERT_TRUE(cancelled.cancel("test"));
    ExportRequest cancel;
    cancel.asset_id = asset_id;
    cancel.output_path = cancelled_out;
    cancel.format = ExportFormat::kPng;
    cancel.cancellation = cancelled.token();
    auto exported_cancel = service->export_asset(cancel);
    ASSERT_FALSE(exported_cancel);
    EXPECT_EQ(exported_cancel.error().code, ErrorCode::kCancelled);
    EXPECT_FALSE(std::filesystem::exists(cancelled_out));
    EXPECT_FALSE(std::filesystem::exists(cancelled_out + ".ravo-export-tmp"));

    ExportRequest bad_quality = jpeg;
    bad_quality.output_path = (root / "bad-quality.jpg").string();
    bad_quality.jpeg_options.quality = 0;
    auto invalid_quality = service->export_asset(bad_quality);
    ASSERT_FALSE(invalid_quality);
    EXPECT_EQ(invalid_quality.error().code, ErrorCode::kValidation);
    EXPECT_FALSE(std::filesystem::exists(bad_quality.output_path));

    ExportRequest missing_directory = png;
    missing_directory.output_path = (root / "missing" / "out.png").string();
    auto missing_result = service->export_asset(missing_directory);
    ASSERT_FALSE(missing_result);
    EXPECT_EQ(missing_result.error().code, ErrorCode::kIo);
    EXPECT_FALSE(std::filesystem::exists(missing_directory.output_path));
}

TEST_F(CatalogServiceTest, BatchExportExpandsInOrderAndPreflightsEveryConflict)
{
    ASSERT_TRUE(open_service(true));
    const auto import_image = [this](const std::string &name, const QColor color)
    {
        const auto path = (root / name).string();
        QImage image(20, 12, QImage::Format_RGB888);
        image.setColorSpace(QColorSpace(QColorSpace::SRgb));
        image.fill(color);
        EXPECT_TRUE(image.save(QString::fromStdString(path), "JPEG", 90));
        auto imported = service->import_one(path, CancellationToken{});
        EXPECT_TRUE(imported) << imported.error().message;
        EXPECT_TRUE(imported.value().asset.has_value());
        return imported.value().asset->id;
    };
    const auto first_id = import_image("batch one.jpg", QColor(200, 20, 30));
    const auto second_id = import_image("batch two.jpg", QColor(20, 200, 30));
    const auto output_directory = root / "batch-output";
    std::filesystem::create_directory(output_directory);

    ExportBatchRequest request;
    request.asset_ids = {first_id, second_id};
    request.output_directory = output_directory.string();
    request.filename_template = "{sequence}-{stem}{ext}";
    request.options.format = ExportFormat::kPng;
    std::vector<std::size_t> progress_indices;
    auto exported = service->export_assets(request,
                                           [&progress_indices](const std::size_t current,
                                                               const std::size_t total,
                                                               const ExportResult *result)
                                           {
                                               EXPECT_EQ(total, 2U);
                                               ASSERT_NE(result, nullptr);
                                               progress_indices.push_back(current);
                                           });
    ASSERT_TRUE(exported) << exported.error().message;
    ASSERT_EQ(exported.value().size(), 2U);
    EXPECT_EQ(progress_indices, (std::vector<std::size_t>{1U, 2U}));
    EXPECT_EQ(std::filesystem::path(exported.value()[0].output_path).filename(),
              "0001-batch one.png");
    EXPECT_EQ(std::filesystem::path(exported.value()[1].output_path).filename(),
              "0002-batch two.png");
    EXPECT_FALSE(QImage(QString::fromStdString(exported.value()[0].output_path)).isNull());
    EXPECT_FALSE(QImage(QString::fromStdString(exported.value()[1].output_path)).isNull());

    const auto conflict_directory = root / "batch-conflict";
    std::filesystem::create_directory(conflict_directory);
    const auto conflict_path = conflict_directory / "0002-batch two.png";
    {
        std::ofstream sentinel(conflict_path, std::ios::binary);
        sentinel << "keep-existing";
    }
    const auto conflict_hash = file_sha256(conflict_path.string());
    request.output_directory = conflict_directory.string();
    auto conflict = service->export_assets(request);
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().code, ErrorCode::kConflict);
    EXPECT_EQ(conflict.error().context.at("completed_count"), "0");
    EXPECT_EQ(conflict.error().context.at("partial_batch"), "false");
    EXPECT_FALSE(std::filesystem::exists(conflict_directory / "0001-batch one.png"));
    EXPECT_EQ(file_sha256(conflict_path.string()), conflict_hash);

    request.output_directory = (root / "batch-duplicate-name").string();
    std::filesystem::create_directory(request.output_directory);
    request.filename_template = "same{ext}";
    auto duplicate = service->export_assets(request);
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().context.at("reason"), "duplicate_export_output");
    EXPECT_TRUE(std::filesystem::is_empty(request.output_directory));
}

TEST_F(CatalogServiceTest, BatchExportCancellationReportsStablePartialDelivery)
{
    ASSERT_TRUE(open_service(true));
    std::vector<std::string> asset_ids;
    for (int index = 0; index < 2; ++index)
    {
        const auto path = (root / ("cancel-batch-" + std::to_string(index + 1) + ".jpg")).string();
        QImage image(20, 12, QImage::Format_RGB888);
        image.setColorSpace(QColorSpace(QColorSpace::SRgb));
        image.fill(index == 0 ? QColor(200, 20, 30) : QColor(20, 200, 30));
        ASSERT_TRUE(image.save(QString::fromStdString(path), "JPEG", 90));
        auto imported = service->import_one(path, CancellationToken{});
        ASSERT_TRUE(imported) << imported.error().message;
        ASSERT_TRUE(imported.value().asset);
        asset_ids.push_back(imported.value().asset->id);
    }
    const auto output_directory = root / "cancel-batch-output";
    std::filesystem::create_directory(output_directory);
    CancellationSource cancellation;
    ExportBatchRequest request;
    request.asset_ids = asset_ids;
    request.output_directory = output_directory.string();
    request.options.format = ExportFormat::kJpeg;
    request.cancellation = cancellation.token();
    auto exported = service->export_assets(
        request,
        [&cancellation](const std::size_t current, const std::size_t, const ExportResult *)
        {
            if (current == 1U)
                EXPECT_TRUE(cancellation.cancel("after-first-batch-output"));
        });
    ASSERT_FALSE(exported);
    EXPECT_EQ(exported.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(exported.error().context.at("completed_count"), "1");
    EXPECT_EQ(exported.error().context.at("partial_batch"), "true");
    EXPECT_EQ(exported.error().context.at("reason"), "after-first-batch-output");
    EXPECT_TRUE(std::filesystem::exists(output_directory / "cancel-batch-1-0001.jpg"));
    EXPECT_FALSE(std::filesystem::exists(output_directory / "cancel-batch-2-0002.jpg"));
}

TEST_F(CatalogServiceTest, BatchExportRuntimeFailureNamesAlreadyDeliveredOutputs)
{
    ASSERT_TRUE(open_service(true));
    std::vector<std::string> asset_ids;
    std::vector<std::string> source_paths;
    for (int index = 0; index < 2; ++index)
    {
        const auto path = (root / ("failure-batch-" + std::to_string(index + 1) + ".jpg")).string();
        QImage image(20, 12, QImage::Format_RGB888);
        image.setColorSpace(QColorSpace(QColorSpace::SRgb));
        image.fill(index == 0 ? QColor(200, 20, 30) : QColor(20, 200, 30));
        ASSERT_TRUE(image.save(QString::fromStdString(path), "JPEG", 90));
        auto imported = service->import_one(path, CancellationToken{});
        ASSERT_TRUE(imported) << imported.error().message;
        ASSERT_TRUE(imported.value().asset);
        asset_ids.push_back(imported.value().asset->id);
        source_paths.push_back(path);
    }
    const auto output_directory = root / "failure-batch-output";
    std::filesystem::create_directory(output_directory);
    ExportBatchRequest request;
    request.asset_ids = asset_ids;
    request.output_directory = output_directory.string();
    request.options.format = ExportFormat::kPng;
    auto exported = service->export_assets(
        request,
        [&source_paths](const std::size_t current, const std::size_t, const ExportResult *)
        {
            if (current == 1U)
                EXPECT_TRUE(std::filesystem::remove(source_paths[1]));
        });
    ASSERT_FALSE(exported);
    EXPECT_EQ(exported.error().code, ErrorCode::kNotFound);
    EXPECT_EQ(exported.error().context.at("completed_count"), "1");
    EXPECT_EQ(exported.error().context.at("partial_batch"), "true");
    EXPECT_EQ(exported.error().context.at("batch_index"), "2");
    EXPECT_TRUE(std::filesystem::exists(output_directory / "failure-batch-1-0001.png"));
    EXPECT_FALSE(std::filesystem::exists(output_directory / "failure-batch-2-0002.png"));
    EXPECT_TRUE(std::filesystem::exists(source_paths[0]));
}

} // namespace
} // namespace ravo
