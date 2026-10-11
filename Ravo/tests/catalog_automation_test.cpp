#include "catalog_test_support.h"
#include <gtest/gtest.h>
#include <future>
#include <thread>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QScopeGuard>
#include "ravo/foundation/executor.h"

namespace ravo
{
namespace
{
JsonValue patch()
{
    return JsonValue::Array{
        JsonValue::Object{{"name", "exposure"}, {"value", JsonValue::number("0.625")}}};
}
} // namespace
TEST_F(CatalogServiceTest, AutomationBatchCancellationRetainsPerPhotoCommitEvidence)
{
    ASSERT_TRUE(open_service(true));
    const auto first_path = root / "one.png", second_path = root / "two.png";
    std::filesystem::copy_file(png_fixture_path(), first_path);
    std::filesystem::copy_file(png_fixture_path(), second_path);
    const auto original = file_sha256(first_path.string());
    auto first = service->import().import_one(first_path.string(), {});
    ASSERT_TRUE(first);
    auto second = service->import().import_one(second_path.string(), {});
    ASSERT_TRUE(second);
    const auto first_id = first.value().asset->id, second_id = second.value().asset->id;
    auto before = sqlite_repository->snapshot();
    ASSERT_TRUE(before);
    CancellationSource cancellation;
    auto result = service->develop().apply_patch_selection(
        {first_id, second_id}, patch(), before.value().revision, cancellation.token(),
        [&](std::size_t done, std::size_t, const DevelopApplyItemResult *item)
        {
            if (done == 1)
            {
                ASSERT_NE(item, nullptr);
                ASSERT_EQ(item->status, DevelopApplyItemStatus::kApplied);
                EXPECT_TRUE(item->history_id);
                EXPECT_TRUE(item->revision);
                static_cast<void>(cancellation.cancel("test_after_first_commit"));
            }
        });
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result.value().applied, 1U);
    EXPECT_EQ(result.value().skipped, 1U);
    ASSERT_EQ(result.value().items.size(), 2U);
    ASSERT_TRUE(result.value().items[1].error);
    EXPECT_EQ(result.value().items[1].error->code, ErrorCode::kCancelled);
    auto saved = service->develop().load_recipe(first_id);
    ASSERT_TRUE(saved);
    auto params = develop_from_recipe(saved.value());
    ASSERT_TRUE(params);
    EXPECT_DOUBLE_EQ(params.value().exposure_ev, 0.625);
    auto untouched = sqlite_repository->load_recipe_json(second_id);
    ASSERT_TRUE(untouched);
    EXPECT_FALSE(untouched.value());
    EXPECT_EQ(file_sha256(first_path.string()), original);
}
TEST_F(CatalogServiceTest, AutomationBatchStopsAtRevisionConflictAndPreservesEarlierCommit)
{
    ASSERT_TRUE(open_service(true));
    std::vector<std::string> ids;
    for (int index = 0; index < 3; ++index)
    {
        const auto path = root / (std::to_string(index) + ".png");
        std::filesystem::copy_file(png_fixture_path(), path);
        auto imported = service->import().import_one(path.string(), {});
        ASSERT_TRUE(imported);
        ids.push_back(imported.value().asset->id);
    }
    auto before = sqlite_repository->snapshot();
    ASSERT_TRUE(before);
    auto result = service->develop().apply_patch_selection(
        ids, patch(), before.value().revision, {},
        [&](std::size_t done, std::size_t, const DevelopApplyItemResult *)
        {
            if (done == 1)
            {
                DevelopParams competitor;
                competitor.exposure_ev = -0.5;
                ASSERT_TRUE(service->develop().save_develop(ids[1], competitor));
            }
        });
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().applied, 1U);
    EXPECT_EQ(result.value().failed, 1U);
    EXPECT_EQ(result.value().skipped, 1U);
    ASSERT_TRUE(result.value().items[1].error);
    EXPECT_EQ(result.value().items[1].error->code, ErrorCode::kConflict);
    auto competitor = service->develop().load_recipe(ids[1]);
    ASSERT_TRUE(competitor);
    auto params = develop_from_recipe(competitor.value());
    ASSERT_TRUE(params);
    EXPECT_DOUBLE_EQ(params.value().exposure_ev, -0.5);
    auto untouched = sqlite_repository->load_recipe_json(ids[2]);
    ASSERT_TRUE(untouched);
    EXPECT_FALSE(untouched.value());
}
TEST_F(CatalogServiceTest, CancelledSaveNeverPublishesRecipeOrHistory)
{
    ASSERT_TRUE(open_service(true));
    auto imported = service->import().import_one(png_fixture_path(), {});
    ASSERT_TRUE(imported);
    const auto id = imported.value().asset->id;
    auto before = sqlite_repository->snapshot();
    ASSERT_TRUE(before);
    auto history = sqlite_repository->list_recipe_history(id);
    ASSERT_TRUE(history);
    CancellationSource cancellation;
    ASSERT_TRUE(cancellation.cancel("before_commit"));
    RecipeSaveOptions options;
    options.cancellation = cancellation.token();
    DevelopParams params;
    params.exposure_ev = 1.25;
    auto saved = service->develop().save_develop_with_history(id, params, options);
    ASSERT_FALSE(saved);
    EXPECT_EQ(saved.error().code, ErrorCode::kCancelled);
    auto after = sqlite_repository->snapshot();
    ASSERT_TRUE(after);
    EXPECT_EQ(before.value().revision, after.value().revision);
    auto after_history = sqlite_repository->list_recipe_history(id);
    ASSERT_TRUE(after_history);
    EXPECT_EQ(history.value().size(), after_history.value().size());
}
TEST_F(CatalogServiceTest, AutomationRecipeCancellationWhileWaitingForWriterLockCannotCommit)
{
    ASSERT_TRUE(open_service(true));
    auto imported = service->import().import_one(png_fixture_path(), {});
    ASSERT_TRUE(imported);
    const auto id = imported.value().asset->id;
    auto before = sqlite_repository->snapshot();
    ASSERT_TRUE(before);
    auto history = sqlite_repository->list_recipe_history(id);
    ASSERT_TRUE(history);
    CancellationSource cancellation;
    SerialExecutor worker;
    std::promise<void> opened, start;
    auto start_future = start.get_future().share();
    std::promise<Result<RecipeCommitResult>> finished;
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
            RecipeCommitPrecondition condition;
            condition.cancellation = cancellation.token();
            finished.set_value(repository.value()->commit_recipe(
                id, 1, std::nullopt, "", RecipeHistoryWrite::kAppendIfNew, {}, {}, condition));
        }));
    bool released = false;
    auto unblock = qScopeGuard(
        [&]
        {
            static_cast<void>(cancellation.cancel("test_cleanup"));
            if (!released)
                start.set_value();
            worker.wait_idle();
        });
    ASSERT_EQ(opened.get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
    const auto connection = QStringLiteral("ravo_automation_write_lock");
    auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
    auto remove_connection = qScopeGuard(
        [&]
        {
            database.close();
            database = {};
            QSqlDatabase::removeDatabase(connection);
        });
    database.setDatabaseName(QString::fromStdString(database_path));
    ASSERT_TRUE(database.open());
    ASSERT_TRUE(database.transaction());
    QSqlQuery lock(database);
    ASSERT_TRUE(lock.exec(QStringLiteral("UPDATE schema_info SET revision=revision WHERE id=1")));
    lock.finish();
    start.set_value();
    released = true;
    EXPECT_EQ(result_future.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout);
    ASSERT_TRUE(cancellation.cancel("cancelled_during_writer_lock"));
    ASSERT_TRUE(database.rollback());
    ASSERT_EQ(result_future.wait_for(std::chrono::seconds(10)), std::future_status::ready);
    auto result = result_future.get();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::kCancelled);
    worker.wait_idle();
    lock = QSqlQuery{};
    auto after = sqlite_repository->snapshot();
    ASSERT_TRUE(after);
    EXPECT_EQ(before.value().revision, after.value().revision);
    auto after_history = sqlite_repository->list_recipe_history(id);
    ASSERT_TRUE(after_history);
    EXPECT_EQ(history.value().size(), after_history.value().size());
    auto recipe = sqlite_repository->load_recipe_json(id);
    ASSERT_TRUE(recipe);
    EXPECT_FALSE(recipe.value());
}
} // namespace ravo
