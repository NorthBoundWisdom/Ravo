#include "catalog_test_support.h"
#include "ravo/services/asset_annotation.h"
#include "ravo/adapters/filesystem_recovery_store.h"
#include "ravo/adapters/filesystem_preview_cache.h"
#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/foundation/executor.h"
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QScopeGuard>
#include <fstream>
#include <future>
#include <stdexcept>

namespace ravo
{
namespace
{
JsonValue edit(const char *kind, JsonValue fields)
{
    return JsonValue::Object{
        {"version", JsonValue::number("1")}, {"kind", kind}, {"fields", std::move(fields)}};
}
JsonValue example(const int kind)
{
    if (kind == 0)
        return edit("review",
                    JsonValue::Object{{"rating", JsonValue::number("4")}, {"flag", "pick"}});
    if (kind == 1)
        return edit("metadata", JsonValue::Object{{"title", "Chosen"}, {"creator", "Ravo"}});
    return edit("keywords", JsonValue::Array{"People|Alice", "Travel|Japan"});
}
class ControlledRecoveryPublish final : public RecoveryStore
{
public:
    ControlledRecoveryPublish(std::unique_ptr<RecoveryStore> store,
                              std::function<void()> checkpoint)
        : store_(std::move(store))
        , checkpoint_(std::move(checkpoint))
    {
    }
    const std::string &root() const noexcept override
    {
        return store_->root();
    }
    Result<RecoveryArtifact> publish(const AssetRecoverySnapshot &snapshot,
                                     const CancellationToken &token) override
    {
        checkpoint_();
        return store_->publish(snapshot, token);
    }
    Result<RecoveryArtifact> verify(std::string_view id, std::int64_t generation,
                                    const CancellationToken &token) const override
    {
        return store_->verify(id, generation, token);
    }
    Result<RecoveryArtifact> verify_artifact(std::string_view path, std::string_view id,
                                             std::int64_t generation,
                                             const CancellationToken &token) const override
    {
        return store_->verify_artifact(path, id, generation, token);
    }
    Result<void> remove_older(std::string_view id, std::int64_t generation) override
    {
        return store_->remove_older(id, generation);
    }
    Result<void> remove_asset(std::string_view id) override
    {
        return store_->remove_asset(id);
    }

private:
    std::unique_ptr<RecoveryStore> store_;
    std::function<void()> checkpoint_;
};
} // namespace

TEST(AssetAnnotationContract, StrictVersionFieldsTypesAndBoundedState)
{
    for (const auto &payload : std::vector<JsonValue>{
             edit("review", JsonValue::Object{}),
             edit("review", JsonValue::Object{{"rating", JsonValue::number("6")}}),
             edit("review", JsonValue::Object{{"rating", JsonValue::number("1.0")}}),
             edit("review", JsonValue::Object{{"rating", true}}),
             edit("review", JsonValue::Object{{"flag", "toggle"}}),
             edit("review", JsonValue::Object{{"picked", true}}),
             edit("metadata", JsonValue::Object{{"title", true}}),
             edit("metadata", JsonValue::Object{{"capture_time", "2026"}}),
             edit("keywords", JsonValue::Array{"A", "A"}),
             edit("keywords", JsonValue::Array{JsonValue::number("1")}),
             edit("unknown", JsonValue::Object{{"title", "A"}})})
        EXPECT_FALSE(parse_asset_annotation_edit(payload)) << serialize_json(payload);
    auto version = *example(0).object_if();
    version["version"] = JsonValue::number("2");
    EXPECT_FALSE(parse_asset_annotation_edit(version));
    version["version"] = JsonValue::number("1");
    version["extra"] = true;
    EXPECT_FALSE(parse_asset_annotation_edit(version));
    auto cleared = parse_asset_annotation_edit(
        edit("metadata", JsonValue::Object{{"description", JsonValue{nullptr}}}));
    ASSERT_TRUE(cleared);
    EXPECT_TRUE(cleared.value().metadata.update_description);
    EXPECT_FALSE(cleared.value().metadata.description);
    auto keywords = parse_asset_annotation_edit(edit("keywords", JsonValue::Array{}));
    ASSERT_TRUE(keywords);
    EXPECT_TRUE(keywords.value().keywords.empty());
    AssetRecord large;
    large.id = "ast_budget";
    large.metadata.description = std::string(1024 * 1024, 'x');
    auto state = asset_annotation_state_json(large);
    EXPECT_FALSE(*state.find("available")->boolean_if());
    EXPECT_LT(serialize_json(state).size(), 1024U);
    AssetAnnotationResult result;
    result.asset_id = large.id;
    result.asset = large;
    EXPECT_FALSE(*asset_annotation_result_json(result).find("readback_available")->boolean_if());
}

TEST_F(CatalogServiceTest, AnnotationsShareTransactionsAndPreserveSourceRecipeHistory)
{
    ASSERT_TRUE(open_service(true));
    const auto source_hash = file_sha256(png_fixture_path());
    auto imported = service->import().import_one(png_fixture_path(), {});
    ASSERT_TRUE(imported);
    const auto id = imported.value().asset->id;
    auto recipe = service->develop().load_recipe(id);
    ASSERT_TRUE(recipe);
    const auto recipe_text = serialize_recipe(recipe.value()).value();
    auto history = sqlite_repository->list_recipe_history(id);
    ASSERT_TRUE(history);
    for (int kind = 0; kind < 3; ++kind)
    {
        auto parsed = parse_asset_annotation_edit(example(kind));
        ASSERT_TRUE(parsed);
        const auto revision = sqlite_repository->snapshot().value().revision;
        auto changed = apply_asset_annotation(*service, id, parsed.value(), revision, {});
        ASSERT_TRUE(changed) << changed.error().message;
        EXPECT_EQ(changed.value().revision, revision + 1);
        EXPECT_TRUE(changed.value().warnings.empty());
        ASSERT_TRUE(changed.value().asset);
        auto stale = apply_asset_annotation(*service, id, parsed.value(), revision, {});
        ASSERT_FALSE(stale);
        EXPECT_EQ(stale.error().code, ErrorCode::kConflict);
        CancellationSource cancellation;
        ASSERT_TRUE(cancellation.cancel("annotation_test"));
        auto cancelled = apply_asset_annotation(*service, id, parsed.value(), revision + 1,
                                                cancellation.token());
        ASSERT_FALSE(cancelled);
        EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
        EXPECT_EQ(sqlite_repository->snapshot().value().revision, revision + 1);
    }
    auto asset = service->library().find_asset(id);
    ASSERT_TRUE(asset);
    EXPECT_EQ(asset.value().review.rating, 4);
    EXPECT_TRUE(asset.value().review.picked);
    EXPECT_FALSE(asset.value().review.rejected);
    EXPECT_EQ(asset.value().metadata.title, "Chosen");
    EXPECT_EQ(asset.value().metadata.creator, "Ravo");
    EXPECT_EQ(asset.value().tags.size(), 2U);
    auto clear = parse_asset_annotation_edit(
        edit("metadata", JsonValue::Object{{"title", JsonValue{nullptr}}}));
    ASSERT_TRUE(clear);
    ASSERT_TRUE(apply_asset_annotation(*service, id, clear.value(),
                                       sqlite_repository->snapshot().value().revision, {}));
    asset = service->library().find_asset(id);
    ASSERT_TRUE(asset);
    EXPECT_FALSE(asset.value().metadata.title);
    EXPECT_EQ(asset.value().metadata.creator, "Ravo");
    EXPECT_EQ(serialize_recipe(service->develop().load_recipe(id).value()).value(), recipe_text);
    EXPECT_EQ(sqlite_repository->list_recipe_history(id).value().size(), history.value().size());
    EXPECT_EQ(file_sha256(png_fixture_path()), source_hash);
}

TEST_F(CatalogServiceTest, AnnotationMissingSelectionRollsBackEveryRecordAndRevision)
{
    ASSERT_TRUE(open_service(true));
    auto imported = service->import().import_one(png_fixture_path(), {});
    ASSERT_TRUE(imported);
    const auto id = imported.value().asset->id;
    auto patch = writable_metadata_patch_for_field("title", std::string("No partial write"));
    ASSERT_TRUE(patch);
    const auto revision = sqlite_repository->snapshot().value().revision;
    EXPECT_FALSE(service->metadata().set_writable_metadata_selection({id, "missing"}, patch.value(),
                                                                     revision));
    EXPECT_FALSE(service->metadata().set_tags_selection({id, "missing"}, {"Unused"}, revision));
    EXPECT_EQ(sqlite_repository->snapshot().value().revision, revision);
    auto asset = service->library().find_asset(id);
    ASSERT_TRUE(asset);
    EXPECT_FALSE(asset.value().metadata.title);
    EXPECT_TRUE(asset.value().tags.empty());
}

TEST_F(CatalogServiceTest, AnnotationWriterLockCancellationAndCompetingRevisionCannotPublish)
{
    ASSERT_TRUE(open_service(true));
    auto imported = service->import().import_one(png_fixture_path(), {});
    ASSERT_TRUE(imported);
    const auto id = imported.value().asset->id;
    for (const int kind : {0, 1, 2})
        for (const bool cancel : {true, false})
        {
            SCOPED_TRACE(std::to_string(kind) + (cancel ? " cancel" : " conflict"));
            const auto revision = sqlite_repository->snapshot().value().revision;
            CancellationSource cancellation;
            SerialExecutor worker;
            std::promise<void> opened, start;
            auto start_future = start.get_future().share();
            std::promise<Result<std::int64_t>> finished;
            auto result_future = finished.get_future();
            ASSERT_TRUE(worker.post(
                [&]
                {
                    auto repository = SqliteCatalogRepository::open(database_path);
                    opened.set_value();
                    start_future.wait();
                    if (!repository)
                    {
                        finished.set_value(repository.error());
                        return;
                    }
                    if (kind == 0)
                        finished.set_value(repository.value()->commit_review_patch(
                            id, {5, {}, {}, {}}, revision, cancellation.token()));
                    else if (kind == 1)
                    {
                        auto patch =
                            writable_metadata_patch_for_field("title", std::string("Blocked"));
                        auto result = repository.value()->patch_assets_writable_metadata(
                            {id}, patch.value(), revision, cancellation.token());
                        finished.set_value(result ? Result<std::int64_t>{result.value().revision} :
                                                    Result<std::int64_t>{result.error()});
                    }
                    else
                    {
                        auto result = repository.value()->replace_assets_tags(
                            {id}, {"Blocked"}, revision, cancellation.token());
                        finished.set_value(result ? Result<std::int64_t>{result.value().revision} :
                                                    Result<std::int64_t>{result.error()});
                    }
                }));
            bool started = false;
            auto worker_cleanup = qScopeGuard(
                [&]
                {
                    static_cast<void>(cancellation.cancel("cleanup"));
                    if (!started)
                        start.set_value();
                    worker.wait_idle();
                });
            ASSERT_EQ(opened.get_future().wait_for(std::chrono::seconds(5)),
                      std::future_status::ready);
            const auto name = QStringLiteral("ravo_annotation_lock");
            auto database = QSqlDatabase::addDatabase("QSQLITE", name);
            auto connection_cleanup = qScopeGuard(
                [&]
                {
                    database.close();
                    database = {};
                    QSqlDatabase::removeDatabase(name);
                });
            database.setDatabaseName(QString::fromStdString(database_path));
            ASSERT_TRUE(database.open());
            ASSERT_TRUE(database.transaction());
            QSqlQuery lock(database);
            ASSERT_TRUE(lock.exec("UPDATE schema_info SET revision=revision+1 WHERE id=1"));
            lock.finish();
            start.set_value();
            started = true;
            EXPECT_EQ(result_future.wait_for(std::chrono::milliseconds(100)),
                      std::future_status::timeout);
            if (cancel)
                ASSERT_TRUE(cancellation.cancel("cancelled_while_waiting"));
            ASSERT_TRUE(cancel ? database.rollback() : database.commit());
            ASSERT_EQ(result_future.wait_for(std::chrono::seconds(10)), std::future_status::ready);
            auto result = result_future.get();
            ASSERT_FALSE(result);
            EXPECT_EQ(result.error().code, cancel ? ErrorCode::kCancelled : ErrorCode::kConflict);
            worker.wait_idle();
            EXPECT_EQ(sqlite_repository->snapshot().value().revision, revision + (cancel ? 0 : 1));
            auto asset = service->library().find_asset(id);
            ASSERT_TRUE(asset);
            EXPECT_EQ(asset.value().review.rating, 0);
            EXPECT_FALSE(asset.value().metadata.title);
            EXPECT_TRUE(asset.value().tags.empty());
        }
}

TEST_F(CatalogServiceTest, AnnotationRecoveryFailureRetainsKnownCommitAndRevision)
{
    ASSERT_TRUE(open_service(true));
    auto imported = service->import().import_one(png_fixture_path(), {});
    ASSERT_TRUE(imported);
    const auto id = imported.value().asset->id;
    const auto generation = service->recovery().recovery_state(id).value().generation;
    const auto obsolete =
        std::filesystem::path(FilesystemRecoveryStore::default_root_for_catalog(database_path)) /
        (id + "." + std::to_string(generation) + ".ravo.json");
    ASSERT_TRUE(std::filesystem::remove(obsolete));
    ASSERT_TRUE(std::filesystem::create_directory(obsolete));
    std::ofstream(obsolete / "blocker") << "keep cleanup failure visible";
    for (const int kind : {0, 1, 2})
    {
        auto parsed = parse_asset_annotation_edit(example(kind));
        ASSERT_TRUE(parsed);
        const auto revision = sqlite_repository->snapshot().value().revision;
        auto result = apply_asset_annotation(*service, id, parsed.value(), revision, {});
        ASSERT_TRUE(result) << result.error().message;
        EXPECT_EQ(result.value().revision, revision + 1);
        ASSERT_EQ(result.value().warnings.size(), 1U);
        EXPECT_EQ(result.value().warnings.front().context.at("catalog_committed"), "true");
        EXPECT_EQ(result.value().warnings.front().context.at("committed_revision"),
                  std::to_string(revision + 1));
        EXPECT_EQ(sqlite_repository->snapshot().value().revision, revision + 1);
    }
    auto asset = service->library().find_asset(id);
    ASSERT_TRUE(asset);
    EXPECT_EQ(asset.value().review.rating, 4);
    EXPECT_EQ(asset.value().metadata.title, "Chosen");
    EXPECT_EQ(asset.value().tags.size(), 2U);
}

TEST_F(CatalogServiceTest, AnnotationCancellationAfterCommitIsACommittedWarning)
{
    ASSERT_TRUE(open_service(true));
    auto imported = service->import().import_one(png_fixture_path(), {});
    ASSERT_TRUE(imported);
    const auto id = imported.value().asset->id;
    for (const int kind : {0, 1, 2})
    {
        CancellationSource cancellation;
        auto repository = SqliteCatalogRepository::open(database_path);
        ASSERT_TRUE(repository);
        auto cache = FilesystemPreviewCache::create((root / "late-cancel-cache").string());
        ASSERT_TRUE(cache);
        auto recovery = FilesystemRecoveryStore::create_for_catalog(database_path);
        ASSERT_TRUE(recovery);
        CatalogService isolated(
            engine, std::move(repository).value(), std::make_unique<QtRasterDecoder>(),
            std::move(cache).value(),
            std::make_unique<ControlledRecoveryPublish>(
                std::move(recovery).value(), [&]
                { static_cast<void>(cancellation.cancel("cancelled_after_annotation_commit")); }));
        const auto revision = isolated.library().snapshot().value().revision;
        auto parsed = parse_asset_annotation_edit(example(kind));
        ASSERT_TRUE(parsed);
        auto result =
            apply_asset_annotation(isolated, id, parsed.value(), revision, cancellation.token());
        ASSERT_TRUE(result) << result.error().message;
        EXPECT_TRUE(cancellation.token().is_cancellation_requested());
        EXPECT_EQ(result.value().revision, revision + 1);
        ASSERT_EQ(result.value().warnings.size(), 1U);
        EXPECT_EQ(result.value().warnings.front().code, ErrorCode::kCancelled);
        EXPECT_EQ(result.value().warnings.front().context.at("catalog_committed"), "true");
        EXPECT_EQ(sqlite_repository->snapshot().value().revision, revision + 1);
    }
}
TEST_F(CatalogServiceTest, AnnotationPortExceptionReportsUnknownInsteadOfFalseRollback)
{
    ASSERT_TRUE(open_service(true));
    auto imported = service->import().import_one(png_fixture_path(), {});
    ASSERT_TRUE(imported);
    const auto id = imported.value().asset->id;
    auto repository = SqliteCatalogRepository::open(database_path);
    ASSERT_TRUE(repository);
    auto cache = FilesystemPreviewCache::create((root / "exception-cache").string());
    ASSERT_TRUE(cache);
    auto recovery = FilesystemRecoveryStore::create_for_catalog(database_path);
    ASSERT_TRUE(recovery);
    bool injected = false;
    CatalogService isolated(
        engine, std::move(repository).value(), std::make_unique<QtRasterDecoder>(),
        std::move(cache).value(),
        std::make_unique<ControlledRecoveryPublish>(std::move(recovery).value(),
                                                    [&]
                                                    {
                                                        if (!injected)
                                                        {
                                                            injected = true;
                                                            throw std::runtime_error(
                                                                "Injected recovery-port exception");
                                                        }
                                                    }));
    const auto revision = isolated.library().snapshot().value().revision;
    auto parsed = parse_asset_annotation_edit(example(1));
    ASSERT_TRUE(parsed);
    auto result = apply_asset_annotation(isolated, id, parsed.value(), revision, {});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().context.at("mutation_applied"), "unknown");
    EXPECT_EQ(result.error().context.at("reason"), "annotation_execution_exception");
    EXPECT_EQ(sqlite_repository->snapshot().value().revision, revision + 1);
    EXPECT_EQ(service->library().find_asset(id).value().metadata.title, "Chosen");
}
} // namespace ravo
