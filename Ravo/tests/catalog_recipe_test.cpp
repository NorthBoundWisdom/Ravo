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

TEST_F(CatalogServiceTest, RenameRecipeSnapshotUpdatesLabelAndRejectsHistoryRows)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto jpeg_path = (root / "snapshot-rename.jpg").string();
    QImage image(16, 16, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(8, 16, 32));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    DevelopParams params;
    params.exposure_ev = 0.4;
    ASSERT_TRUE(service->save_develop(asset_id, params));
    auto snapshot = service->create_recipe_snapshot(asset_id, "keep");
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    auto history = service->list_recipe_history(asset_id);
    ASSERT_TRUE(history) << history.error().message;
    std::int64_t snapshot_id = 0;
    std::int64_t history_id = 0;
    for (const auto &entry : history.value())
    {
        if (entry.kind == kRecipeHistoryKindSnapshot)
            snapshot_id = entry.id;
        else if (history_id == 0)
            history_id = entry.id;
    }
    ASSERT_NE(snapshot_id, 0);
    ASSERT_NE(history_id, 0);

    auto renamed = service->rename_recipe_snapshot(asset_id, snapshot_id, "  look-a  ");
    ASSERT_TRUE(renamed) << renamed.error().message;
    auto after = service->list_recipe_history(asset_id);
    ASSERT_TRUE(after) << after.error().message;
    bool found = false;
    for (const auto &entry : after.value())
    {
        if (entry.id != snapshot_id)
            continue;
        found = true;
        ASSERT_TRUE(entry.label.has_value());
        EXPECT_EQ(*entry.label, "look-a");
    }
    EXPECT_TRUE(found);

    auto empty = service->rename_recipe_snapshot(asset_id, snapshot_id, "   ");
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().code, ErrorCode::kValidation);

    auto history_row = service->rename_recipe_snapshot(asset_id, history_id, "nope");
    ASSERT_FALSE(history_row);
    EXPECT_EQ(history_row.error().code, ErrorCode::kValidation);
}

TEST_F(CatalogServiceTest, ReopenUpgradesStoredRecipeV1ToExplicitColorBoundaries)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto jpeg_path = (root / "recipe-v1.jpg").string();
    QImage image(16, 16, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(30, 60, 90));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    auto repository = SqliteCatalogRepository::open(database_path);
    ASSERT_TRUE(repository) << repository.error().message;
    Recipe recipe_v1;
    recipe_v1.schema_version = 1;
    recipe_v1.asset = {asset_id, imported.value().asset->normalized_uri,
                       imported.value().asset->content_fingerprint};
    recipe_v1.operations.push_back({"ravo.core.exposure",
                                    1,
                                    "exposure-1",
                                    true,
                                    {{"exposure_ev", ParameterValue{0.5}}},
                                    std::nullopt});
    auto serialized_v1 = serialize_recipe(recipe_v1);
    ASSERT_TRUE(serialized_v1) << serialized_v1.error().message;
    ASSERT_TRUE(repository.value()->save_recipe_json(asset_id, 1, serialized_v1.value()));
    ASSERT_TRUE(repository.value()->close());
    ASSERT_TRUE(service->close());
    service.reset();

    ASSERT_TRUE(open_service(false));
    auto restored = service->load_recipe(asset_id);
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_EQ(restored.value().schema_version, 3);
    ASSERT_EQ(restored.value().operations.size(), 3U);
    EXPECT_EQ(restored.value().operations.front().id, "ravo.color.input");
    EXPECT_EQ(restored.value().operations[1].id, "ravo.core.exposure");
    EXPECT_EQ(restored.value().operations.back().id, "ravo.color.output");
}

TEST_F(CatalogServiceTest, RecipeTransactionFailurePreservesCurrentRecipeAndRevision)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto jpeg_path = (root / "recipe-transaction.jpg").string();
    QImage image(16, 16, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(90, 45, 20));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    DevelopParams accepted;
    accepted.exposure_ev = 0.25;
    ASSERT_TRUE(service->save_develop(asset_id, accepted));
    auto snapshot_before = service->snapshot();
    ASSERT_TRUE(snapshot_before) << snapshot_before.error().message;
    auto history_before = service->list_recipe_history(asset_id);
    ASSERT_TRUE(history_before) << history_before.error().message;

    {
        const auto connection = QStringLiteral("ravo_recipe_failure_injection");
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(QString::fromStdString(database_path));
        ASSERT_TRUE(database.open()) << database.lastError().text().toStdString();
        QSqlQuery query(database);
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TRIGGER fail_recipe_history BEFORE INSERT ON asset_recipe_history "
            "BEGIN SELECT RAISE(ABORT, 'forced recipe history failure'); END")))
            << query.lastError().text().toStdString();
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection);
    }

    DevelopParams rejected = accepted;
    rejected.exposure_ev = -0.75;
    auto failed = service->save_develop(asset_id, rejected);
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, ErrorCode::kIo);

    auto current = service->load_recipe(asset_id);
    ASSERT_TRUE(current) << current.error().message;
    auto current_params = develop_from_recipe(current.value());
    ASSERT_TRUE(current_params) << current_params.error().message;
    EXPECT_NEAR(current_params.value().exposure_ev, accepted.exposure_ev, 1e-9);
    auto snapshot_after = service->snapshot();
    ASSERT_TRUE(snapshot_after) << snapshot_after.error().message;
    EXPECT_EQ(snapshot_after.value().revision, snapshot_before.value().revision);
    auto history_after = service->list_recipe_history(asset_id);
    ASSERT_TRUE(history_after) << history_after.error().message;
    EXPECT_EQ(history_after.value().size(), history_before.value().size());

    ASSERT_TRUE(service->close());
    service.reset();
    auto reopened = open_service(false);
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto restored = service->load_recipe(asset_id);
    ASSERT_TRUE(restored) << restored.error().message;
    auto restored_params = develop_from_recipe(restored.value());
    ASSERT_TRUE(restored_params) << restored_params.error().message;
    EXPECT_NEAR(restored_params.value().exposure_ev, accepted.exposure_ev, 1e-9);
}

TEST_F(CatalogServiceTest, RecipeHistoryCoalescesOnlyTheExpectedLatestOrdinaryRow)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto jpeg_path = (root / "recipe-history-coalesce.jpg").string();
    QImage image(16, 16, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(40, 80, 120));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    DevelopParams first;
    first.exposure_black = 0.01;
    auto first_saved = service->save_develop_with_history(asset_id, first);
    ASSERT_TRUE(first_saved) << first_saved.error().message;
    ASSERT_TRUE(first_saved.value().history_id);
    const auto coalesce_id = *first_saved.value().history_id;

    DevelopParams second = first;
    second.exposure_black = 0.02;
    auto second_saved = service->save_develop_with_history(
        asset_id, second,
        RecipeSaveOptions{.history_write = RecipeHistoryWrite::kAppendIfNew,
                          .discard_history_after_seq = {},
                          .coalesce_history_id = coalesce_id});
    ASSERT_TRUE(second_saved) << second_saved.error().message;
    ASSERT_TRUE(second_saved.value().history_id);
    EXPECT_EQ(*second_saved.value().history_id, coalesce_id);
    auto coalesced = service->list_recipe_history(asset_id);
    ASSERT_TRUE(coalesced) << coalesced.error().message;
    ASSERT_EQ(coalesced.value().size(), 1U);
    EXPECT_EQ(coalesced.value().front().id, coalesce_id);
    auto coalesced_recipe = parse_recipe_json(coalesced.value().front().recipe_json);
    ASSERT_TRUE(coalesced_recipe) << coalesced_recipe.error().message;
    auto coalesced_params = develop_from_recipe(coalesced_recipe.value());
    ASSERT_TRUE(coalesced_params) << coalesced_params.error().message;
    EXPECT_NEAR(coalesced_params.value().exposure_black, second.exposure_black, 1e-9);

    auto before_invalid = service->snapshot();
    ASSERT_TRUE(before_invalid) << before_invalid.error().message;
    auto invalid = service->save_develop_with_history(
        asset_id, second,
        RecipeSaveOptions{.history_write = RecipeHistoryWrite::kUnchanged,
                          .discard_history_after_seq = {},
                          .coalesce_history_id = coalesce_id});
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, ErrorCode::kValidation);
    auto after_invalid = service->snapshot();
    ASSERT_TRUE(after_invalid) << after_invalid.error().message;
    EXPECT_EQ(after_invalid.value().revision, before_invalid.value().revision);

    auto snapshot = service->create_recipe_snapshot(asset_id, "boundary");
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    DevelopParams third = second;
    third.exposure_black = 0.03;
    auto after_snapshot = service->save_develop_with_history(
        asset_id, third,
        RecipeSaveOptions{.history_write = RecipeHistoryWrite::kAppendIfNew,
                          .discard_history_after_seq = {},
                          .coalesce_history_id = coalesce_id});
    ASSERT_TRUE(after_snapshot) << after_snapshot.error().message;
    ASSERT_TRUE(after_snapshot.value().history_id);
    EXPECT_NE(*after_snapshot.value().history_id, coalesce_id);
    auto separated = service->list_recipe_history(asset_id);
    ASSERT_TRUE(separated) << separated.error().message;
    ASSERT_EQ(separated.value().size(), 3U);
    EXPECT_EQ(separated.value()[1].kind, kRecipeHistoryKindSnapshot);
    EXPECT_EQ(separated.value()[2].id, coalesce_id);
}

TEST_F(CatalogServiceTest, RecipeHistoryCoalesceFailureRollsBackRecipeHistoryAndRevision)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto jpeg_path = (root / "recipe-history-coalesce-failure.jpg").string();
    QImage image(16, 16, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(120, 80, 40));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    DevelopParams first;
    first.exposure_black = 0.01;
    auto first_saved = service->save_develop_with_history(asset_id, first);
    ASSERT_TRUE(first_saved) << first_saved.error().message;
    ASSERT_TRUE(first_saved.value().history_id);
    auto snapshot_before = service->snapshot();
    ASSERT_TRUE(snapshot_before) << snapshot_before.error().message;
    auto history_before = service->list_recipe_history(asset_id);
    ASSERT_TRUE(history_before) << history_before.error().message;

    {
        const auto connection = QStringLiteral("ravo_recipe_coalesce_failure_injection");
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(QString::fromStdString(database_path));
        ASSERT_TRUE(database.open()) << database.lastError().text().toStdString();
        QSqlQuery query(database);
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TRIGGER fail_recipe_history_coalesce "
            "BEFORE UPDATE OF recipe_json ON asset_recipe_history "
            "BEGIN SELECT RAISE(ABORT, 'forced recipe history coalesce failure'); END")))
            << query.lastError().text().toStdString();
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection);
    }

    DevelopParams rejected = first;
    rejected.exposure_black = 0.03;
    auto failed = service->save_develop_with_history(
        asset_id, rejected,
        RecipeSaveOptions{.history_write = RecipeHistoryWrite::kAppendIfNew,
                          .discard_history_after_seq = {},
                          .coalesce_history_id = *first_saved.value().history_id});
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, ErrorCode::kIo);

    auto current = service->load_recipe(asset_id);
    ASSERT_TRUE(current) << current.error().message;
    auto current_params = develop_from_recipe(current.value());
    ASSERT_TRUE(current_params) << current_params.error().message;
    EXPECT_NEAR(current_params.value().exposure_black, first.exposure_black, 1e-9);
    auto snapshot_after = service->snapshot();
    ASSERT_TRUE(snapshot_after) << snapshot_after.error().message;
    EXPECT_EQ(snapshot_after.value().revision, snapshot_before.value().revision);
    auto history_after = service->list_recipe_history(asset_id);
    ASSERT_TRUE(history_after) << history_after.error().message;
    ASSERT_EQ(history_after.value().size(), history_before.value().size());
    EXPECT_EQ(history_after.value().front().id, history_before.value().front().id);
    EXPECT_EQ(history_after.value().front().recipe_json,
              history_before.value().front().recipe_json);
}

TEST_F(CatalogServiceTest, HistoryPreviewLeavesStackAndEditDiscardsNewerSteps)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto jpeg_path = (root / "history-cursor.jpg").string();
    QImage image(16, 16, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(20, 40, 80));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    DevelopParams first;
    first.exposure_ev = 0.4;
    ASSERT_TRUE(service->save_develop(asset_id, first));
    DevelopParams second;
    second.exposure_ev = -0.5;
    ASSERT_TRUE(service->save_develop(asset_id, second));
    DevelopParams third;
    third.exposure_ev = -0.5;
    third.highlights = 0.3;
    ASSERT_TRUE(service->save_develop(asset_id, third));

    auto history = service->list_recipe_history(asset_id);
    ASSERT_TRUE(history) << history.error().message;
    ASSERT_EQ(history.value().size(), 3U);
    const auto newest = history.value()[0];
    const auto middle = history.value()[1];
    const auto oldest = history.value()[2];
    EXPECT_GT(newest.seq, middle.seq);
    EXPECT_GT(middle.seq, oldest.seq);

    auto previewed =
        service->save_develop(asset_id, second,
                              RecipeSaveOptions{.history_write = RecipeHistoryWrite::kUnchanged,
                                                .discard_history_after_seq = {},
                                                .coalesce_history_id = {}});
    ASSERT_TRUE(previewed) << previewed.error().message;
    auto preview_history = service->list_recipe_history(asset_id);
    ASSERT_TRUE(preview_history) << preview_history.error().message;
    ASSERT_EQ(preview_history.value().size(), 3U);
    EXPECT_EQ(preview_history.value()[0].id, newest.id);
    EXPECT_EQ(preview_history.value()[1].id, middle.id);
    auto preview_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(preview_recipe) << preview_recipe.error().message;
    auto preview_params = develop_from_recipe(preview_recipe.value());
    ASSERT_TRUE(preview_params) << preview_params.error().message;
    EXPECT_NEAR(preview_params.value().exposure_ev, second.exposure_ev, 1e-9);
    EXPECT_NEAR(preview_params.value().highlights, 0.0, 1e-9);

    DevelopParams branched = second;
    branched.contrast = 0.2;
    auto edited = service->save_develop(asset_id, branched,
                                        RecipeSaveOptions{
                                            .history_write = RecipeHistoryWrite::kAppendIfNew,
                                            .discard_history_after_seq = middle.seq,
                                            .coalesce_history_id = {},
                                        });
    ASSERT_TRUE(edited) << edited.error().message;
    auto truncated = service->list_recipe_history(asset_id);
    ASSERT_TRUE(truncated) << truncated.error().message;
    ASSERT_EQ(truncated.value().size(), 3U);
    EXPECT_EQ(truncated.value()[1].id, middle.id);
    EXPECT_EQ(truncated.value()[2].id, oldest.id);
    EXPECT_NE(truncated.value()[0].id, newest.id);
    EXPECT_GT(truncated.value()[0].seq, middle.seq);
    auto current = service->load_recipe(asset_id);
    ASSERT_TRUE(current) << current.error().message;
    auto current_params = develop_from_recipe(current.value());
    ASSERT_TRUE(current_params) << current_params.error().message;
    EXPECT_NEAR(current_params.value().exposure_ev, second.exposure_ev, 1e-9);
    EXPECT_NEAR(current_params.value().contrast, branched.contrast, 1e-9);
    EXPECT_NEAR(current_params.value().highlights, 0.0, 1e-9);
}

TEST_F(CatalogServiceTest, RestoreRecipeHistoryStillAppendsCurrentStep)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto jpeg_path = (root / "history-restore-append.jpg").string();
    QImage image(16, 16, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(8, 16, 32));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    DevelopParams first;
    first.exposure_ev = 0.25;
    ASSERT_TRUE(service->save_develop(asset_id, first));
    DevelopParams second;
    second.exposure_ev = -0.25;
    ASSERT_TRUE(service->save_develop(asset_id, second));
    auto before = service->list_recipe_history(asset_id);
    ASSERT_TRUE(before) << before.error().message;
    ASSERT_EQ(before.value().size(), 2U);
    const auto oldest_id = before.value().back().id;

    auto restored = service->restore_recipe_history(asset_id, oldest_id);
    ASSERT_TRUE(restored) << restored.error().message;
    auto after = service->list_recipe_history(asset_id);
    ASSERT_TRUE(after) << after.error().message;
    EXPECT_EQ(after.value().size(), 3U);
    auto recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto params = develop_from_recipe(recipe.value());
    ASSERT_TRUE(params) << params.error().message;
    EXPECT_NEAR(params.value().exposure_ev, first.exposure_ev, 1e-9);
}

TEST_F(CatalogServiceTest, HistoryDiscardAndAppendShareRecipeTransaction)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto jpeg_path = (root / "history-discard-transaction.jpg").string();
    QImage image(16, 16, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(64, 32, 16));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    DevelopParams first;
    first.exposure_ev = 0.25;
    ASSERT_TRUE(service->save_develop(asset_id, first));
    DevelopParams second;
    second.exposure_ev = -0.75;
    ASSERT_TRUE(service->save_develop(asset_id, second));
    auto snapshot_before = service->snapshot();
    ASSERT_TRUE(snapshot_before) << snapshot_before.error().message;
    auto history_before = service->list_recipe_history(asset_id);
    ASSERT_TRUE(history_before) << history_before.error().message;
    ASSERT_EQ(history_before.value().size(), 2U);
    const auto cursor_seq = history_before.value().back().seq;

    {
        const auto connection = QStringLiteral("ravo_history_discard_failure_injection");
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(QString::fromStdString(database_path));
        ASSERT_TRUE(database.open()) << database.lastError().text().toStdString();
        QSqlQuery query(database);
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TRIGGER fail_recipe_history_discard BEFORE INSERT ON asset_recipe_history "
            "BEGIN SELECT RAISE(ABORT, 'forced recipe history discard failure'); END")))
            << query.lastError().text().toStdString();
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection);
    }

    DevelopParams branched = first;
    branched.contrast = 0.4;
    auto failed = service->save_develop(asset_id, branched,
                                        RecipeSaveOptions{
                                            .history_write = RecipeHistoryWrite::kAppendIfNew,
                                            .discard_history_after_seq = cursor_seq,
                                            .coalesce_history_id = {},
                                        });
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, ErrorCode::kIo);

    auto current = service->load_recipe(asset_id);
    ASSERT_TRUE(current) << current.error().message;
    auto current_params = develop_from_recipe(current.value());
    ASSERT_TRUE(current_params) << current_params.error().message;
    EXPECT_NEAR(current_params.value().exposure_ev, second.exposure_ev, 1e-9);
    EXPECT_NEAR(current_params.value().contrast, 0.0, 1e-9);
    auto snapshot_after = service->snapshot();
    ASSERT_TRUE(snapshot_after) << snapshot_after.error().message;
    EXPECT_EQ(snapshot_after.value().revision, snapshot_before.value().revision);
    auto history_after = service->list_recipe_history(asset_id);
    ASSERT_TRUE(history_after) << history_after.error().message;
    ASSERT_EQ(history_after.value().size(), history_before.value().size());
    EXPECT_EQ(history_after.value()[0].id, history_before.value()[0].id);
    EXPECT_EQ(history_after.value()[1].id, history_before.value()[1].id);
}

TEST_F(CatalogServiceTest, RawSigmoidBaselinePersistsOnlyUserOverrides)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    auto imported = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    EXPECT_FALSE(imported.value().asset->has_edits);

    auto baseline = service->load_recipe(asset_id);
    ASSERT_TRUE(baseline) << baseline.error().message;
    ASSERT_EQ(baseline.value().operations.size(), 5U);
    EXPECT_NE(std::find_if(baseline.value().operations.begin(), baseline.value().operations.end(),
                           [](const OperationInstance &operation)
                           { return operation.id == "ravo.color.temperature"; }),
              baseline.value().operations.end());
    EXPECT_NE(std::find_if(baseline.value().operations.begin(), baseline.value().operations.end(),
                           [](const OperationInstance &operation)
                           { return operation.id == "ravo.color.input"; }),
              baseline.value().operations.end());
    EXPECT_NE(std::find_if(baseline.value().operations.begin(), baseline.value().operations.end(),
                           [](const OperationInstance &operation)
                           { return operation.id == "ravo.display.sigmoid"; }),
              baseline.value().operations.end());
    EXPECT_NE(std::find_if(baseline.value().operations.begin(), baseline.value().operations.end(),
                           [](const OperationInstance &operation)
                           { return operation.id == "ravo.color.output"; }),
              baseline.value().operations.end());
    EXPECT_NE(std::find_if(baseline.value().operations.begin(), baseline.value().operations.end(),
                           [](const OperationInstance &operation)
                           { return operation.id == std::string(kSharpenOperationId); }),
              baseline.value().operations.end());
    auto baseline_params = develop_from_recipe(baseline.value());
    ASSERT_TRUE(baseline_params) << baseline_params.error().message;
    EXPECT_TRUE(baseline_params.value().sigmoid_enabled);
    EXPECT_NEAR(baseline_params.value().sigmoid_contrast, kSigmoidContrastDefault, 1e-9);
    EXPECT_NEAR(baseline_params.value().sharpen, SharpenParams{}.amount, 1e-9);
    EXPECT_NEAR(baseline_params.value().sharpen_radius, SharpenParams{}.radius, 1e-9);
    EXPECT_NEAR(baseline_params.value().sharpen_threshold, SharpenParams{}.threshold, 1e-9);
    auto baseline_has_edits = service->asset_has_edits(asset_id);
    ASSERT_TRUE(baseline_has_edits) << baseline_has_edits.error().message;
    EXPECT_FALSE(baseline_has_edits.value());

    auto adjusted = baseline_params.value();
    adjusted.sigmoid_skew = -0.35;
    auto saved = service->save_develop(asset_id, adjusted);
    ASSERT_TRUE(saved) << saved.error().message;
    EXPECT_TRUE(saved.value().has_edits);
    ASSERT_TRUE(service->close());
    service.reset();

    auto reopened = open_service(false);
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto restored = service->load_recipe(asset_id);
    ASSERT_TRUE(restored) << restored.error().message;
    auto restored_params = develop_from_recipe(restored.value());
    ASSERT_TRUE(restored_params) << restored_params.error().message;
    EXPECT_NEAR(restored_params.value().sigmoid_skew, -0.35, 1e-9);

    auto reset = service->reset_recipe(asset_id);
    ASSERT_TRUE(reset) << reset.error().message;
    EXPECT_FALSE(reset.value().has_edits);
    auto reset_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(reset_recipe) << reset_recipe.error().message;
    auto reset_params = develop_from_recipe(reset_recipe.value());
    ASSERT_TRUE(reset_params) << reset_params.error().message;
    EXPECT_TRUE(reset_params.value().sigmoid_enabled);
    EXPECT_NEAR(reset_params.value().sigmoid_skew, kSigmoidSkewDefault, 1e-9);
    EXPECT_NEAR(reset_params.value().sharpen, SharpenParams{}.amount, 1e-9);
}

TEST_F(CatalogServiceTest, LiveDevelopPreviewAppliesWithoutSavingRecipe)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto jpeg_path = (root / "live.jpg").string();
    QImage image(48, 32, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(200, 80, 40));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    DevelopParams live;
    live.exposure_ev = 1.25;
    live.temperature.mode = std::string(kTemperatureModeManual);
    live.temperature.coefficients =
        std::array<double, kTemperatureChannelCount>{1.0, 1.0, 1.0, 1.0};
    PreviewRequest request;
    request.asset_id = asset_id;
    request.max_edge = kInteractivePreviewMaxEdge;
    request.persist_preview_record = false;
    auto first = service->request_preview(request, live);
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_TRUE(first.value().cache_path.empty());
    EXPECT_FALSE(first.value().rgb.empty());
    EXPECT_EQ(first.value().rgb.size(),
              static_cast<std::size_t>(first.value().width) * first.value().height * 3U);
    EXPECT_LE(std::max(first.value().width, first.value().height), kInteractivePreviewMaxEdge);

    auto stored = service->load_recipe(asset_id);
    ASSERT_TRUE(stored) << stored.error().message;
    auto stored_params = develop_from_recipe(stored.value());
    ASSERT_TRUE(stored_params) << stored_params.error().message;
    EXPECT_TRUE(stored_params.value().is_identity());
    auto listed = service->list_assets();
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed.value().size(), 1U);
    EXPECT_FALSE(listed.value().front().has_edits);

    const auto first_pixels = first.value().rgb;
    live.exposure_ev = -0.75;
    auto second = service->request_preview(request, live);
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_TRUE(second.value().cache_path.empty());
    EXPECT_NE(second.value().rgb, first_pixels);
}

TEST_F(CatalogServiceTest, RgbPrimariesPersistAndReproducePixelsAfterReopen)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto png_path = (root / "primaries.png").string();
    QImage image(48, 32, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    for (int y = 0; y < image.height(); ++y)
    {
        for (int x = 0; x < image.width(); ++x)
        {
            image.setPixelColor(
                x, y,
                QColor((x * 17 + y * 3) % 256, (x * 5 + y * 13) % 256, (x * 11 + y * 7) % 256));
        }
    }
    ASSERT_TRUE(image.save(QString::fromStdString(png_path), "PNG"));
    const auto original_hash = file_sha256(png_path);
    auto imported = service->import_one(png_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    PreviewRequest preview;
    preview.asset_id = asset_id;
    preview.max_edge = 48;
    preview.persist_preview_record = true;
    auto baseline = service->request_preview(preview);
    ASSERT_TRUE(baseline) << baseline.error().message;
    ASSERT_FALSE(baseline.value().cache_path.empty());
    ASSERT_TRUE(std::filesystem::exists(baseline.value().cache_path));
    const QImage baseline_image(QString::fromStdString(baseline.value().cache_path));
    ASSERT_FALSE(baseline_image.isNull());
    const auto same_pixels = [](const QImage &left, const QImage &right)
    {
        if (left.size() != right.size())
        {
            return false;
        }
        for (int y = 0; y < left.height(); ++y)
        {
            for (int x = 0; x < left.width(); ++x)
            {
                if (left.pixel(x, y) != right.pixel(x, y))
                {
                    return false;
                }
            }
        }
        return true;
    };

    auto recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto edited = develop_from_recipe(recipe.value());
    ASSERT_TRUE(edited) << edited.error().message;
    edited.value().primaries.achromatic_tint_hue = 0.3;
    edited.value().primaries.achromatic_tint_purity = 0.1;
    edited.value().primaries.red_hue = -0.05;
    edited.value().primaries.red_purity = 0.96;
    edited.value().primaries.green_hue = 0.04;
    edited.value().primaries.green_purity = 1.04;
    edited.value().primaries.blue_hue = -0.03;
    edited.value().primaries.blue_purity = 1.02;
    clamp_develop(edited.value());
    auto saved = service->save_develop(asset_id, edited.value());
    ASSERT_TRUE(saved) << saved.error().message;

    auto stored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(stored_recipe) << stored_recipe.error().message;
    const auto primaries_operation = std::find_if(
        stored_recipe.value().operations.begin(), stored_recipe.value().operations.end(),
        [](const OperationInstance &operation) { return operation.id == kPrimariesOperationId; });
    ASSERT_NE(primaries_operation, stored_recipe.value().operations.end());
    EXPECT_EQ(primaries_operation->schema_version, 1);
    EXPECT_EQ(primaries_operation->parameters.size(), 8U);

    auto before_reopen = service->request_preview(preview);
    ASSERT_TRUE(before_reopen) << before_reopen.error().message;
    ASSERT_FALSE(before_reopen.value().cache_path.empty());
    ASSERT_TRUE(std::filesystem::exists(before_reopen.value().cache_path));
    const QImage before_reopen_image(QString::fromStdString(before_reopen.value().cache_path));
    ASSERT_FALSE(before_reopen_image.isNull());
    EXPECT_FALSE(same_pixels(before_reopen_image, baseline_image));
    EXPECT_NE(before_reopen.value().cache_key, baseline.value().cache_key);

    PreviewRequest interactive = preview;
    interactive.persist_preview_record = false;
    auto interactive_before_reopen = service->request_preview(interactive);
    ASSERT_TRUE(interactive_before_reopen) << interactive_before_reopen.error().message;
    EXPECT_TRUE(interactive_before_reopen.value().cache_path.empty());
    ASSERT_FALSE(interactive_before_reopen.value().rgb.empty());

    const auto export_path = (root / "primaries-export.png").string();
    ExportRequest export_request;
    export_request.asset_id = asset_id;
    export_request.output_path = export_path;
    export_request.format = ExportFormat::kPng;
    export_request.max_edge = 48U;
    auto exported = service->export_asset(export_request);
    ASSERT_TRUE(exported) << exported.error().message;
    const QImage export_image(QString::fromStdString(export_path));
    ASSERT_FALSE(export_image.isNull());
    EXPECT_TRUE(same_pixels(export_image, before_reopen_image));

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto restored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(restored_recipe) << restored_recipe.error().message;
    auto restored = develop_from_recipe(restored_recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_EQ(restored.value().primaries, edited.value().primaries);

    auto after_reopen = service->request_preview(preview);
    ASSERT_TRUE(after_reopen) << after_reopen.error().message;
    ASSERT_FALSE(after_reopen.value().cache_path.empty());
    const QImage after_reopen_image(QString::fromStdString(after_reopen.value().cache_path));
    ASSERT_FALSE(after_reopen_image.isNull());
    EXPECT_TRUE(same_pixels(after_reopen_image, before_reopen_image));
    EXPECT_EQ(after_reopen.value().cache_key, before_reopen.value().cache_key);

    auto interactive_after_reopen = service->request_preview(interactive);
    ASSERT_TRUE(interactive_after_reopen) << interactive_after_reopen.error().message;
    EXPECT_EQ(interactive_after_reopen.value().rgb, interactive_before_reopen.value().rgb);
    EXPECT_EQ(file_sha256(png_path), original_hash);
}

TEST_F(CatalogServiceTest, ProfileGammaModesPersistAndReproducePreviewAndExportAfterReopen)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto png_path = (root / "profile-gamma.png").string();
    QImage image(48, 32, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    for (int y = 0; y < image.height(); ++y)
    {
        for (int x = 0; x < image.width(); ++x)
        {
            image.setPixelColor(
                x, y,
                QColor((x * 11 + y * 7) % 256, (x * 3 + y * 17) % 256, (x * 19 + y * 5) % 256));
        }
    }
    ASSERT_TRUE(image.save(QString::fromStdString(png_path), "PNG"));
    const auto original_hash = file_sha256(png_path);
    auto imported = service->import_one(png_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    const auto image_rgb = [](const QImage &source)
    {
        const QImage rgb = source.convertToFormat(QImage::Format_RGB888);
        const auto row_bytes = static_cast<std::size_t>(rgb.width()) * 3U;
        std::vector<std::uint8_t> result;
        result.reserve(row_bytes * static_cast<std::size_t>(rgb.height()));
        for (int y = 0; y < rgb.height(); ++y)
        {
            const auto *line = rgb.constScanLine(y);
            result.insert(result.end(), line, line + row_bytes);
        }
        return result;
    };

    PreviewRequest persisted;
    persisted.asset_id = asset_id;
    persisted.max_edge = 48;
    persisted.persist_preview_record = true;
    auto baseline = service->request_preview(persisted);
    ASSERT_TRUE(baseline) << baseline.error().message;
    const QImage baseline_image(QString::fromStdString(baseline.value().cache_path));
    ASSERT_FALSE(baseline_image.isNull());
    const auto same_pixels = [](const QImage &left, const QImage &right)
    {
        if (left.size() != right.size())
        {
            return false;
        }
        for (int y = 0; y < left.height(); ++y)
        {
            for (int x = 0; x < left.width(); ++x)
            {
                if (left.pixel(x, y) != right.pixel(x, y))
                {
                    return false;
                }
            }
        }
        return true;
    };

    auto recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto logarithmic = develop_from_recipe(recipe.value());
    ASSERT_TRUE(logarithmic) << logarithmic.error().message;
    logarithmic.value().profile_gamma_enabled = true;
    logarithmic.value().profile_gamma.mode = std::string(kProfileGammaModeLogarithmic);
    logarithmic.value().profile_gamma.linear = 0.2;
    logarithmic.value().profile_gamma.gamma = 0.65;
    logarithmic.value().profile_gamma.dynamic_range = 8.5;
    logarithmic.value().profile_gamma.grey_point = 20.0;
    logarithmic.value().profile_gamma.shadows_range = -6.5;
    logarithmic.value().profile_gamma.security_factor = 12.0;
    clamp_develop(logarithmic.value());
    ASSERT_TRUE(service->save_develop(asset_id, logarithmic.value()));

    auto log_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(log_recipe) << log_recipe.error().message;
    const auto profile_gamma_operation =
        std::find_if(log_recipe.value().operations.begin(), log_recipe.value().operations.end(),
                     [](const OperationInstance &operation)
                     { return operation.id == kProfileGammaOperationId; });
    ASSERT_NE(profile_gamma_operation, log_recipe.value().operations.end());
    EXPECT_EQ(profile_gamma_operation->schema_version, kProfileGammaOperationSchemaVersion);
    EXPECT_TRUE(profile_gamma_operation->enabled);
    EXPECT_EQ(profile_gamma_operation->parameters.size(), 7U);

    auto log_persisted = service->request_preview(persisted);
    ASSERT_TRUE(log_persisted) << log_persisted.error().message;
    const QImage log_image(QString::fromStdString(log_persisted.value().cache_path));
    ASSERT_FALSE(log_image.isNull());
    EXPECT_NE(log_persisted.value().cache_key, baseline.value().cache_key);
    EXPECT_FALSE(same_pixels(log_image, baseline_image));

    PreviewRequest interactive = persisted;
    interactive.persist_preview_record = false;
    auto log_interactive = service->request_preview(interactive);
    ASSERT_TRUE(log_interactive) << log_interactive.error().message;
    ASSERT_FALSE(log_interactive.value().rgb.empty());
    EXPECT_EQ(log_interactive.value().rgb, image_rgb(log_image));

    ExportRequest log_export;
    log_export.asset_id = asset_id;
    log_export.output_path = (root / "profile-gamma-log.png").string();
    log_export.format = ExportFormat::kPng;
    log_export.max_edge = 48U;
    auto exported_log = service->export_asset(log_export);
    ASSERT_TRUE(exported_log) << exported_log.error().message;
    const QImage log_export_image(QString::fromStdString(log_export.output_path));
    ASSERT_FALSE(log_export_image.isNull());
    EXPECT_TRUE(same_pixels(log_export_image, log_image));

    auto gamma = logarithmic.value();
    gamma.profile_gamma.mode = std::string(kProfileGammaModeGamma);
    gamma.profile_gamma.linear = 0.12;
    gamma.profile_gamma.gamma = 0.72;
    clamp_develop(gamma);
    ASSERT_TRUE(service->save_develop(asset_id, gamma));

    auto gamma_persisted = service->request_preview(persisted);
    ASSERT_TRUE(gamma_persisted) << gamma_persisted.error().message;
    const QImage gamma_image(QString::fromStdString(gamma_persisted.value().cache_path));
    ASSERT_FALSE(gamma_image.isNull());
    EXPECT_NE(gamma_persisted.value().cache_key, log_persisted.value().cache_key);
    EXPECT_FALSE(same_pixels(gamma_image, log_image));

    auto gamma_interactive = service->request_preview(interactive);
    ASSERT_TRUE(gamma_interactive) << gamma_interactive.error().message;
    ASSERT_FALSE(gamma_interactive.value().rgb.empty());
    EXPECT_EQ(gamma_interactive.value().rgb, image_rgb(gamma_image));

    ExportRequest gamma_export;
    gamma_export.asset_id = asset_id;
    gamma_export.output_path = (root / "profile-gamma-gamma.png").string();
    gamma_export.format = ExportFormat::kPng;
    gamma_export.max_edge = 48U;
    auto exported_gamma = service->export_asset(gamma_export);
    ASSERT_TRUE(exported_gamma) << exported_gamma.error().message;
    const QImage gamma_export_image(QString::fromStdString(gamma_export.output_path));
    ASSERT_FALSE(gamma_export_image.isNull());
    EXPECT_TRUE(same_pixels(gamma_export_image, gamma_image));

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto restored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(restored_recipe) << restored_recipe.error().message;
    auto restored = develop_from_recipe(restored_recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_TRUE(restored.value().profile_gamma_enabled);
    EXPECT_EQ(restored.value().profile_gamma, gamma.profile_gamma);

    auto reopened_persisted = service->request_preview(persisted);
    ASSERT_TRUE(reopened_persisted) << reopened_persisted.error().message;
    const QImage reopened_image(QString::fromStdString(reopened_persisted.value().cache_path));
    ASSERT_FALSE(reopened_image.isNull());
    EXPECT_EQ(reopened_persisted.value().cache_key, gamma_persisted.value().cache_key);
    EXPECT_TRUE(same_pixels(reopened_image, gamma_image));

    auto reopened_interactive = service->request_preview(interactive);
    ASSERT_TRUE(reopened_interactive) << reopened_interactive.error().message;
    EXPECT_EQ(reopened_interactive.value().rgb, gamma_interactive.value().rgb);
    EXPECT_EQ(file_sha256(png_path), original_hash);
}

TEST_F(CatalogServiceTest, FileIccContentInvalidatesPreviewAndSurvivesRecipeReopen)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto jpeg_path = (root / "profiled.jpg").string();
    QImage image(32, 24, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(180, 70, 30));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    const auto profile_path = root / "input.icc";
    const auto write_profile = [&profile_path](const QColorSpace::NamedColorSpace named)
    {
        const QByteArray bytes = QColorSpace(named).iccProfile();
        QFile file(QString::fromStdString(profile_path.string()));
        return !bytes.isEmpty() && file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
               file.write(bytes) == bytes.size();
    };
    ASSERT_TRUE(write_profile(QColorSpace::SRgb));

    auto baseline = service->load_recipe(asset_id);
    ASSERT_TRUE(baseline) << baseline.error().message;
    auto develop = develop_from_recipe(baseline.value());
    ASSERT_TRUE(develop) << develop.error().message;
    develop.value().input_color.input_profile = std::string(kInputProfileFileIcc);
    develop.value().input_color.input_profile_filename = profile_path.string();
    auto saved = service->save_develop(asset_id, develop.value());
    ASSERT_TRUE(saved) << saved.error().message;

    PreviewRequest request;
    request.asset_id = asset_id;
    request.max_edge = 64;
    auto first = service->request_preview(request);
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_TRUE(std::filesystem::exists(first.value().cache_path));

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto restored = service->load_recipe(asset_id);
    ASSERT_TRUE(restored) << restored.error().message;
    auto restored_develop = develop_from_recipe(restored.value());
    ASSERT_TRUE(restored_develop) << restored_develop.error().message;
    EXPECT_EQ(restored_develop.value().input_color, develop.value().input_color);

    ASSERT_TRUE(write_profile(QColorSpace::DisplayP3));
    auto second = service->request_preview(request);
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_NE(second.value().cache_key, first.value().cache_key);
    EXPECT_NE(second.value().cache_path, first.value().cache_path);
    EXPECT_TRUE(std::filesystem::exists(first.value().cache_path));
    EXPECT_TRUE(std::filesystem::exists(second.value().cache_path));
}

TEST_F(CatalogServiceTest, OutputIccContentInvalidatesPreviewBeforeCachePublication)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto jpeg_path = (root / "output-profiled.jpg").string();
    QImage image(32, 24, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(170, 90, 35));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    const auto profile_path = root / "output.icc";
    const auto write_profile = [&profile_path](const QColorSpace::NamedColorSpace named)
    {
        const QByteArray bytes = QColorSpace(named).iccProfile();
        QFile file(QString::fromStdString(profile_path.string()));
        return !bytes.isEmpty() && file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
               file.write(bytes) == bytes.size();
    };
    ASSERT_TRUE(write_profile(QColorSpace::SRgb));

    auto baseline = service->load_recipe(asset_id);
    ASSERT_TRUE(baseline) << baseline.error().message;
    auto develop = develop_from_recipe(baseline.value());
    ASSERT_TRUE(develop) << develop.error().message;
    develop.value().output_color.output_profile = std::string(kInputProfileFileIcc);
    develop.value().output_color.output_profile_filename = profile_path.string();
    ASSERT_TRUE(service->save_develop(asset_id, develop.value()));

    PreviewRequest request;
    request.asset_id = asset_id;
    request.max_edge = 64;
    auto first = service->request_preview(request);
    ASSERT_TRUE(first) << first.error().message;
    const QImage first_image(QString::fromStdString(first.value().cache_path));
    ASSERT_FALSE(first_image.isNull());
    ASSERT_TRUE(first_image.colorSpace().isValid());
    EXPECT_EQ(first_image.colorSpace().iccProfile(), QColorSpace(QColorSpace::SRgb).iccProfile());

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto restored = service->load_recipe(asset_id);
    ASSERT_TRUE(restored) << restored.error().message;
    auto restored_develop = develop_from_recipe(restored.value());
    ASSERT_TRUE(restored_develop) << restored_develop.error().message;
    EXPECT_EQ(restored_develop.value().output_color, develop.value().output_color);

    ASSERT_TRUE(write_profile(QColorSpace::DisplayP3));
    auto second = service->request_preview(request);
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_NE(second.value().cache_key, first.value().cache_key);
    EXPECT_NE(second.value().cache_path, first.value().cache_path);
    const QImage second_image(QString::fromStdString(second.value().cache_path));
    ASSERT_FALSE(second_image.isNull());
    ASSERT_TRUE(second_image.colorSpace().isValid());
    EXPECT_EQ(second_image.colorSpace().iccProfile(),
              QColorSpace(QColorSpace::DisplayP3).iccProfile());

    QFile corrupt(QString::fromStdString(profile_path.string()));
    ASSERT_TRUE(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(corrupt.write("bad", 3), 3);
    corrupt.close();
    auto rejected = service->request_preview(request);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kValidation);
    EXPECT_TRUE(std::filesystem::exists(first.value().cache_path));
    EXPECT_TRUE(std::filesystem::exists(second.value().cache_path));
}

TEST_F(CatalogServiceTest, RawLivePreviewReusesLinearWorkingWithoutSaving)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    auto imported = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    DevelopParams live;
    live.sigmoid_enabled = true;
    live.exposure_ev = 0.75;
    PreviewRequest request;
    request.asset_id = asset_id;
    request.max_edge = kInteractivePreviewMaxEdge;
    request.persist_preview_record = false;
    request.prefer_embedded_preview = true;
    auto first = service->request_preview(request, live);
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_TRUE(first.value().cache_path.empty());
    EXPECT_FALSE(first.value().rgb.empty());
    EXPECT_LE(std::max(first.value().width, first.value().height), kInteractivePreviewMaxEdge);
    auto cache_state = testing::CatalogServiceTestControl::linear_working_max_edges(*service);
    EXPECT_EQ(cache_state[0], kInteractivePreviewMaxEdge);
    EXPECT_EQ(cache_state[1], kDefaultPreviewMaxEdge);

    auto direct_recipe = recipe_from_develop(
        {asset_id, raw_fixture_path(), imported.value().asset->content_fingerprint}, live);
    ASSERT_TRUE(direct_recipe) << direct_recipe.error().message;
    PreviewRequest settled = request;
    settled.max_edge = kDefaultPreviewMaxEdge;
    auto settled_live = service->request_preview(settled, live);
    ASSERT_TRUE(settled_live) << settled_live.error().message;
    EXPECT_TRUE(settled_live.value().cache_path.empty());
    RenderRequest direct_request;
    direct_request.asset = direct_recipe.value().asset;
    direct_request.recipe = direct_recipe.value();
    direct_request.output_width = settled_live.value().width;
    direct_request.output_height = settled_live.value().height;
    auto direct = engine.render_to_image(direct_request);
    ASSERT_TRUE(direct) << direct.error().message;
    EXPECT_EQ(settled_live.value().rgb, direct.value().rgb);

    auto stored = service->load_recipe(asset_id);
    ASSERT_TRUE(stored) << stored.error().message;
    auto stored_params = develop_from_recipe(stored.value());
    ASSERT_TRUE(stored_params) << stored_params.error().message;
    EXPECT_NEAR(stored_params.value().exposure_ev, 0.0, 1e-9);
    EXPECT_TRUE(stored_params.value().sigmoid_enabled);

    live.exposure_ev = -0.5;
    auto second = service->request_preview(request, live);
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_TRUE(second.value().cache_path.empty());
    EXPECT_NE(second.value().rgb, first.value().rgb);

    live.exposure_ev = 0.75;
    auto third = service->request_preview(request, live);
    ASSERT_TRUE(third) << third.error().message;
    EXPECT_EQ(third.value().rgb, first.value().rgb);

    live.temperature.mode = std::string(kTemperatureModeManual);
    live.temperature.coefficients =
        std::array<double, kTemperatureChannelCount>{1.0, 1.0, 1.0, 1.0};
    auto balanced = service->request_preview(request, live);
    ASSERT_TRUE(balanced) << balanced.error().message;
    EXPECT_NE(balanced.value().rgb, first.value().rgb);
    ASSERT_TRUE(live.temperature.coefficients);
    (*live.temperature.coefficients)[0] += 0.25;
    auto rebalanced = service->request_preview(request, live);
    ASSERT_TRUE(rebalanced) << rebalanced.error().message;
    EXPECT_NE(rebalanced.value().rgb, balanced.value().rgb);

    live.raw_highlights = 1.0;
    auto highlighted = service->request_preview(request, live);
    ASSERT_TRUE(highlighted) << highlighted.error().message;
    EXPECT_EQ(highlighted.value().width, first.value().width);
    EXPECT_EQ(highlighted.value().height, first.value().height);
}

TEST_F(CatalogServiceTest, ExposureDeflickerPreviewPersistsReopensAndExportsIdenticalPixels)
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
    auto baseline = service->request_preview(preview);
    ASSERT_TRUE(baseline) << baseline.error().message;
    ASSERT_FALSE(baseline.value().cache_path.empty());
    const QImage baseline_image(QString::fromStdString(baseline.value().cache_path));
    ASSERT_FALSE(baseline_image.isNull());

    auto recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto develop = develop_from_recipe(recipe.value());
    ASSERT_TRUE(develop) << develop.error().message;
    develop.value().exposure_mode = std::string(kExposureModeDeflicker);
    develop.value().exposure_black = -0.01;
    develop.value().exposure_ev = 3.0;
    develop.value().exposure_deflicker_percentile = 65.0;
    develop.value().exposure_deflicker_target_ev = -3.5;
    develop.value().exposure_compensate_exposure_bias = true;
    develop.value().exposure_compensate_highlight_preservation = true;
    ASSERT_TRUE(service->save_develop(asset_id, develop.value()));

    auto before_reopen = service->request_preview(preview);
    ASSERT_TRUE(before_reopen) << before_reopen.error().message;
    ASSERT_FALSE(before_reopen.value().cache_path.empty());
    EXPECT_NE(before_reopen.value().cache_key, baseline.value().cache_key);
    const QImage before_reopen_image(QString::fromStdString(before_reopen.value().cache_path));
    ASSERT_FALSE(before_reopen_image.isNull());
    EXPECT_NE(before_reopen_image, baseline_image);

    const auto export_path = (root / "exposure-deflicker-export.png").string();
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
    EXPECT_EQ(restored.value().exposure_mode, develop.value().exposure_mode);
    EXPECT_DOUBLE_EQ(restored.value().exposure_black, develop.value().exposure_black);
    EXPECT_DOUBLE_EQ(restored.value().exposure_ev, develop.value().exposure_ev);
    EXPECT_DOUBLE_EQ(restored.value().exposure_deflicker_percentile,
                     develop.value().exposure_deflicker_percentile);
    EXPECT_DOUBLE_EQ(restored.value().exposure_deflicker_target_ev,
                     develop.value().exposure_deflicker_target_ev);
    EXPECT_EQ(restored.value().exposure_compensate_exposure_bias,
              develop.value().exposure_compensate_exposure_bias);
    EXPECT_EQ(restored.value().exposure_compensate_highlight_preservation,
              develop.value().exposure_compensate_highlight_preservation);
    auto after_reopen = service->request_preview(preview);
    ASSERT_TRUE(after_reopen) << after_reopen.error().message;
    const QImage after_reopen_image(QString::fromStdString(after_reopen.value().cache_path));
    ASSERT_FALSE(after_reopen_image.isNull());
    EXPECT_EQ(after_reopen_image, before_reopen_image);
}

} // namespace
} // namespace ravo
