#include "catalog_test_support.h"
#include "ravo/services/asset_annotation.h"
#include "ravo/services/asset_annotation_batch.h"
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
JsonValue batch_item(const std::string &id, const char *kind, JsonValue fields)
{
    return JsonValue::Object{{"asset_id", id}, {"annotation", edit(kind, std::move(fields))}};
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
    const auto metadata_schema = asset_annotation_fields_schema(AssetAnnotationKind::kMetadata);
    EXPECT_EQ(
        metadata_schema.find("properties")->find("title")->find("maxLength")->number_if()->text,
        std::to_string(kMetadataFieldMaxLength));
    const auto keyword_schema = asset_annotation_fields_schema(AssetAnnotationKind::kKeywords);
    EXPECT_EQ(keyword_schema.find("items")->find("maxLength")->number_if()->text,
              std::to_string(kKeywordPathMaxLength));
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
TEST(AssetAnnotationBatchContract, RejectsMalformedDuplicateAndOverBudgetItems)
{
    const auto valid =
        batch_item("asset", "review", JsonValue::Object{{"rating", JsonValue::number("3")}});
    EXPECT_TRUE(parse_asset_annotation_batch(JsonValue::Array{valid}));
    EXPECT_FALSE(parse_asset_annotation_batch(JsonValue::Array{}));
    EXPECT_FALSE(parse_asset_annotation_batch(JsonValue::Array{valid, valid}));
    EXPECT_FALSE(parse_asset_annotation_batch(JsonValue::Array(33, valid)));
    EXPECT_FALSE(parse_asset_annotation_batch(JsonValue::Array{
        batch_item("asset", "metadata", JsonValue::Object{{"original_path", "forbidden"}})}));
    EXPECT_FALSE(parse_asset_annotation_batch(JsonValue::Array{
        batch_item("asset", "metadata", JsonValue::Object{{"title", std::string(65536, 'x')}})}));
    auto extra = *valid.object_if();
    extra.emplace("selection", true);
    EXPECT_FALSE(parse_asset_annotation_batch(JsonValue::Array{extra}));
}

TEST(AssetAnnotationBatchContract, BoundsDiagnosticsWithoutChangingConfirmedCommitFacts)
{
    AssetAnnotationBatchResult result;
    result.initial_revision = 10;
    result.revision = 11;
    result.committed_count = 1;
    TaskError failure = make_error(ErrorCode::kIo, std::string(9000, '\x01'),
                                   {{"reason", std::string(5000, '\x02')},
                                    {"catalog_committed", "true"},
                                    {"committed_revision", "11"},
                                    {"mutation_applied", "unknown"}});
    AssetAnnotationResult committed;
    committed.asset_id = "first";
    committed.revision = 11;
    committed.warnings.push_back(failure);
    result.items.push_back({"first", AssetAnnotationBatchStatus::kCommitted, committed, {}});
    result.items.push_back({"second", AssetAnnotationBatchStatus::kFailed, {}, failure});
    auto json = asset_annotation_batch_result_json(result);
    EXPECT_EQ(*json.find("outcome")->string_if(), "partial");
    EXPECT_EQ(json.find("committed_count")->number_if()->text, "1");
    const auto &items = *json.find("items")->array_if();
    const auto &warning = items[0].find("warnings")->array_if()->front();
    EXPECT_LE(serialize_json(warning).size(), 4096U);
    EXPECT_TRUE(*warning.find("diagnostics_truncated")->boolean_if());
    EXPECT_EQ(*warning.find("context")->find("catalog_committed")->string_if(), "true");
    EXPECT_LE(serialize_json(*items[1].find("error")).size(), 4096U);
    EXPECT_EQ(*items[0].find("status")->string_if(), "committed");
}

TEST_F(CatalogServiceTest, AnnotationBatchReadsAndCommitsMixedExplicitTargetsWithoutRecipeChanges)
{
    ASSERT_TRUE(open_service(true));
    const auto hash = file_sha256(png_fixture_path());
    auto imported = service->import().import_one(png_fixture_path(), {});
    ASSERT_TRUE(imported);
    const auto first = imported.value().asset->id;
    auto second = service->library().create_asset_version(first);
    auto third = service->library().create_asset_version(first);
    ASSERT_TRUE(second);
    ASSERT_TRUE(third);
    const std::vector<std::string> ids{first, second.value().version.id, third.value().version.id};
    std::vector<std::string> recipes;
    std::vector<std::size_t> history;
    for (const auto &id : ids)
    {
        recipes.push_back(serialize_recipe(service->develop().load_recipe(id).value()).value());
        history.push_back(sqlite_repository->list_recipe_history(id).value().size());
    }
    const auto revision = service->library().snapshot().value().revision;
    auto before = read_asset_annotations(*service, ids, revision);
    ASSERT_TRUE(before);
    ASSERT_EQ(before.value().find("assets")->array_if()->size(), 3U);
    JsonValue items = JsonValue::Array{
        batch_item(ids[0], "review", JsonValue::Object{{"rating", JsonValue::number("5")}}),
        batch_item(ids[1], "metadata",
                   JsonValue::Object{{"title", "Second"}, {"description", nullptr}}),
        batch_item(ids[2], "keywords", JsonValue::Array{"Travel|Japan"})};
    auto result = apply_asset_annotation_batch(*service, items, revision);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result.value().committed_count, 3U);
    EXPECT_EQ(result.value().revision, revision + 3);
    EXPECT_FALSE(result.value().unknown);
    EXPECT_EQ(*asset_annotation_batch_result_json(result.value()).find("outcome")->string_if(),
              "completed");
    EXPECT_FALSE(read_asset_annotations(*service, ids, revision));
    EXPECT_FALSE(apply_asset_annotation_batch(*service, items, revision));
    auto after = read_asset_annotations(*service, ids, result.value().revision);
    ASSERT_TRUE(after);
    EXPECT_EQ(service->library().find_asset(ids[0]).value().review.rating, 5);
    EXPECT_EQ(service->library().find_asset(ids[1]).value().metadata.title, "Second");
    EXPECT_EQ(service->library().find_asset(ids[2]).value().tags.size(), 1U);
    for (std::size_t index = 0; index < ids.size(); ++index)
    {
        EXPECT_EQ(serialize_recipe(service->develop().load_recipe(ids[index]).value()).value(),
                  recipes[index]);
        EXPECT_EQ(sqlite_repository->list_recipe_history(ids[index]).value().size(),
                  history[index]);
    }
    EXPECT_EQ(file_sha256(png_fixture_path()), hash);
}

TEST_F(CatalogServiceTest, AnnotationBatchPreflightsEveryTargetAndPayloadBeforeWriting)
{
    ASSERT_TRUE(open_service(true));
    auto imported = service->import().import_one(png_fixture_path(), {});
    ASSERT_TRUE(imported);
    const auto id = imported.value().asset->id;
    const auto revision = service->library().snapshot().value().revision;
    auto first = batch_item(id, "metadata", JsonValue::Object{{"title", "Must not write"}});
    for (const auto &items : std::vector<JsonValue>{
             JsonValue::Array{first,
                              batch_item("missing", "review",
                                         JsonValue::Object{{"rating", JsonValue::number("3")}})},
             JsonValue::Array{first, first},
             JsonValue::Array{
                 first, batch_item("other", "metadata", JsonValue::Object{{"invalid", "x"}})}})
        EXPECT_FALSE(apply_asset_annotation_batch(*service, items, revision));
    CancellationSource stop;
    ASSERT_TRUE(stop.cancel("before_batch"));
    EXPECT_FALSE(
        apply_asset_annotation_batch(*service, JsonValue::Array{first}, revision, stop.token()));
    EXPECT_EQ(service->library().snapshot().value().revision, revision);
    EXPECT_FALSE(service->library().find_asset(id).value().metadata.title);
}

TEST_F(CatalogServiceTest, AnnotationBatchLateCancelKeepsCommittedWarningAndStopsRemainingItems)
{
    ASSERT_TRUE(open_service(true));
    auto imported = service->import().import_one(png_fixture_path(), {});
    ASSERT_TRUE(imported);
    const auto first = imported.value().asset->id;
    auto second = service->library().create_asset_version(first);
    auto third = service->library().create_asset_version(first);
    ASSERT_TRUE(second);
    ASSERT_TRUE(third);
    CancellationSource cancellation;
    auto repository = SqliteCatalogRepository::open(database_path);
    auto cache = FilesystemPreviewCache::create((root / "batch-cancel-cache").string());
    auto recovery = FilesystemRecoveryStore::create_for_catalog(database_path);
    ASSERT_TRUE(repository);
    ASSERT_TRUE(cache);
    ASSERT_TRUE(recovery);
    CatalogService isolated(engine, std::move(repository).value(),
                            std::make_unique<QtRasterDecoder>(), std::move(cache).value(),
                            std::make_unique<ControlledRecoveryPublish>(
                                std::move(recovery).value(), [&]
                                { static_cast<void>(cancellation.cancel("after_first_commit")); }));
    const auto revision = isolated.library().snapshot().value().revision;
    JsonValue items =
        JsonValue::Array{batch_item(first, "metadata", JsonValue::Object{{"title", "Committed"}}),
                         batch_item(second.value().version.id, "metadata",
                                    JsonValue::Object{{"title", "Cancelled"}}),
                         batch_item(third.value().version.id, "metadata",
                                    JsonValue::Object{{"title", "Not started"}})};
    auto result = apply_asset_annotation_batch(isolated, items, revision, cancellation.token());
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().committed_count, 1U);
    EXPECT_EQ(result.value().items[0].status, AssetAnnotationBatchStatus::kCommitted);
    ASSERT_TRUE(result.value().items[0].result);
    EXPECT_FALSE(result.value().items[0].result->warnings.empty());
    EXPECT_EQ(result.value().items[1].status, AssetAnnotationBatchStatus::kCancelled);
    EXPECT_EQ(result.value().items[2].status, AssetAnnotationBatchStatus::kNotStarted);
    EXPECT_EQ(service->library().find_asset(first).value().metadata.title, "Committed");
    EXPECT_FALSE(service->library().find_asset(second.value().version.id).value().metadata.title);
    EXPECT_FALSE(service->library().find_asset(third.value().version.id).value().metadata.title);
}

TEST_F(CatalogServiceTest, AnnotationBatchCompetingWriterStopsWithoutRetargeting)
{
    ASSERT_TRUE(open_service(true));
    auto imported = service->import().import_one(png_fixture_path(), {});
    ASSERT_TRUE(imported);
    const auto first = imported.value().asset->id;
    auto second = service->library().create_asset_version(first);
    ASSERT_TRUE(second);
    auto repository = SqliteCatalogRepository::open(database_path);
    auto cache = FilesystemPreviewCache::create((root / "batch-race-cache").string());
    auto recovery = FilesystemRecoveryStore::create_for_catalog(database_path);
    ASSERT_TRUE(repository);
    ASSERT_TRUE(cache);
    ASSERT_TRUE(recovery);
    CatalogService isolated(
        engine, std::move(repository).value(), std::make_unique<QtRasterDecoder>(),
        std::move(cache).value(),
        std::make_unique<ControlledRecoveryPublish>(
            std::move(recovery).value(),
            [&] { EXPECT_TRUE(service->library().set_rating(second.value().version.id, 3)); }));
    const auto revision = isolated.library().snapshot().value().revision;
    auto result = apply_asset_annotation_batch(
        isolated,
        JsonValue::Array{batch_item(first, "metadata", JsonValue::Object{{"title", "First"}}),
                         batch_item(second.value().version.id, "metadata",
                                    JsonValue::Object{{"title", "Must not write"}})},
        revision);
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().committed_count, 1U);
    ASSERT_TRUE(result.value().items[1].error);
    EXPECT_EQ(result.value().items[1].error->code, ErrorCode::kConflict);
    EXPECT_FALSE(service->library().find_asset(second.value().version.id).value().metadata.title);
    EXPECT_EQ(service->library().find_asset(second.value().version.id).value().review.rating, 3);
}

TEST_F(CatalogServiceTest, AnnotationBatchUnknownCommitStopsAndRequiresReconciliation)
{
    ASSERT_TRUE(open_service(true));
    auto imported = service->import().import_one(png_fixture_path(), {});
    ASSERT_TRUE(imported);
    const auto first = imported.value().asset->id;
    auto second = service->library().create_asset_version(first);
    ASSERT_TRUE(second);
    auto repository = SqliteCatalogRepository::open(database_path);
    auto cache = FilesystemPreviewCache::create((root / "batch-unknown-cache").string());
    auto recovery = FilesystemRecoveryStore::create_for_catalog(database_path);
    ASSERT_TRUE(repository);
    ASSERT_TRUE(cache);
    ASSERT_TRUE(recovery);
    bool injected = false;
    CatalogService isolated(engine, std::move(repository).value(),
                            std::make_unique<QtRasterDecoder>(), std::move(cache).value(),
                            std::make_unique<ControlledRecoveryPublish>(
                                std::move(recovery).value(),
                                [&]
                                {
                                    if (!injected)
                                    {
                                        injected = true;
                                        throw std::runtime_error("unknown batch publication");
                                    }
                                }));
    const auto revision = isolated.library().snapshot().value().revision;
    auto result = apply_asset_annotation_batch(
        isolated,
        JsonValue::Array{batch_item(first, "metadata", JsonValue::Object{{"title", "Durable"}}),
                         batch_item(second.value().version.id, "metadata",
                                    JsonValue::Object{{"title", "Undispatched"}})},
        revision);
    ASSERT_TRUE(result);
    EXPECT_TRUE(result.value().unknown);
    EXPECT_EQ(result.value().items[0].status, AssetAnnotationBatchStatus::kUnknown);
    EXPECT_EQ(result.value().items[1].status, AssetAnnotationBatchStatus::kNotStarted);
    EXPECT_EQ(*asset_annotation_batch_result_json(result.value()).find("outcome")->string_if(),
              "unknown");
    EXPECT_EQ(service->library().find_asset(first).value().metadata.title, "Durable");
    EXPECT_FALSE(service->library().find_asset(second.value().version.id).value().metadata.title);
}

TEST_F(CatalogServiceTest, AnnotationReadbackBoundsAggregateStateAndRejectsStaleOrCancelledReads)
{
    ASSERT_TRUE(open_service(true));
    auto imported = service->import().import_one(png_fixture_path(), {});
    ASSERT_TRUE(imported);
    const auto first = imported.value().asset->id;
    std::vector<std::string> ids{first};
    for (std::size_t index = 1; index < 32; ++index)
    {
        auto version = service->library().create_asset_version(first);
        ASSERT_TRUE(version);
        ids.push_back(version.value().version.id);
    }
    WritableMetadataPatch patch;
    patch.update_description = true;
    patch.description = std::string(2000, 'x');
    ASSERT_TRUE(service->metadata().set_writable_metadata_selection(ids, patch));
    std::vector<std::string> tags;
    for (int index = 0; index < 600; ++index)
        tags.push_back("tag" + std::to_string(index) + std::string(55, 'x'));
    ASSERT_TRUE(service->metadata().set_tags_selection(ids, tags));
    const auto revision = service->library().snapshot().value().revision;
    auto states = read_asset_annotations(*service, ids, revision);
    ASSERT_TRUE(states);
    EXPECT_LT(serialize_json(states.value()).size(), 1024U * 1024U);
    const auto &assets = *states.value().find("assets")->array_if();
    EXPECT_TRUE(*assets.front().find("available")->boolean_if());
    EXPECT_FALSE(*assets.back().find("available")->boolean_if());
    CancellationSource stop;
    ASSERT_TRUE(stop.cancel("read_cancelled"));
    EXPECT_FALSE(read_asset_annotations(*service, ids, revision, stop.token()));
    EXPECT_FALSE(read_asset_annotations(*service, {first, first}, revision));
    EXPECT_FALSE(read_asset_annotations(*service, ids, revision - 1));
    EXPECT_EQ(service->library().snapshot().value().revision, revision);
}
} // namespace ravo
