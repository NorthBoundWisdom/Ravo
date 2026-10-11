#include "studio_test_support.h"
#include "studio_library_test_support.h"
#include "studio_automation_test_control.h"
#include "ravo/desktop/studio_live_session_controller.h"
#include "ravo/desktop/studio_command_controller.h"
#include "ravo/foundation/log.h"
#include "ravo/recipe/recipe.h"
#include <gtest/gtest.h>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <QFileInfo>
#include <QCoreApplication>
namespace ravo
{
using namespace studio_test_support;
TEST(StudioAutomationRegression, SelectionPendingIsBusyInMachineStateAndRejectsDevelop)
{
    ensure_qt_core();
    init_logging("ravo-review-probes");
    QTemporaryDir root;
    ScopedEnvironmentVariable registry("RAVO_LIVE_CONTROL_DIR", root.filePath("control").toUtf8());
    make_paged_catalog(root);
    StudioPresenter p;
    StudioCommandController commands(p);
    auto live = StudioLiveSessionController::create(p, commands);
    ASSERT_TRUE(live);
    p.openCatalogFromPath(root.filePath("library.sqlite"));
    ASSERT_TRUE(
        wait_until([&] { return p.catalogOpen() && !p.busy() && p.visibleCount() == 205; }));
    p.openDevelop();
    ASSERT_TRUE(wait_until(
        [&]
        {
            auto s = live.value()->snapshot();
            return s.find("recipe")->find("current")->object_if() &&
                   !*s.find("busy")->boolean_if() && !p.inspect()->previewLoading();
        }));
    auto entered = std::make_shared<std::promise<void>>();
    std::promise<void> release;
    ASSERT_TRUE(
        testing::StudioAutomationTestControl::block(p, entered, release.get_future().share()));
    auto unblock = qScopeGuard([&] { release.set_value(); });
    ASSERT_EQ(entered->get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
    p.selectAllVisible();
    ASSERT_TRUE(p.selectionResolving());
    ASSERT_TRUE(p.busy());
    auto state = live.value()->snapshot();
    EXPECT_TRUE(*state.find("busy")->boolean_if()) << "Machine state hides pending selection";
    auto applied = commands.applyDevelopFields({{"exposure", 0.25}});
    EXPECT_FALSE(applied && applied.value()) << "Develop accepted during selection resolution";
}

TEST(StudioAutomationRegression, PrimaryAfterDeselectHasMetadataWithoutResidentPage)
{
    ensure_qt_core();
    init_logging("ravo-review-probes");
    QTemporaryDir root;
    ScopedEnvironmentVariable registry("RAVO_LIVE_CONTROL_DIR", root.filePath("control").toUtf8());
    make_paged_catalog(root, 1000);
    StudioPresenter p;
    StudioCommandController commands(p);
    auto live = StudioLiveSessionController::create(p, commands);
    ASSERT_TRUE(live);
    p.openCatalogFromPath(root.filePath("library.sqlite"));
    ASSERT_TRUE(
        wait_until([&] { return p.catalogOpen() && !p.busy() && p.visibleCount() == 1000; }));
    p.selectAsset("ast_page_0");
    p.selectAllVisible();
    ASSERT_TRUE(wait_until([&] { return !p.busy() && p.selectedCount() == 1000; }));
    ASSERT_FALSE(p.assets()->rowLoaded(999));
    p.toggleAssetSelected("ast_page_0");
    EXPECT_EQ(p.selectedAssetId(), "ast_page_999");
    EXPECT_EQ(p.selectedCount(), 999);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !p.busy() && p.develop()->state().develop_loaded_ &&
                   !p.inspect()->previewLoading();
        }));
    EXPECT_EQ(p.selectedMediaType(), "image/png");
    auto state = live.value()->snapshot();
    EXPECT_TRUE(state.find("selection")->find("media_type") != nullptr);
    EXPECT_TRUE(state.find("recipe")->find("current")->object_if() != nullptr);
}

TEST(StudioAutomationRegression, NavigationAfterQueryReplacementUsesCurrentRank)
{
    ensure_qt_core();
    init_logging("ravo-review-probes");
    QTemporaryDir root;
    make_paged_catalog(root, 1000);
    StudioPresenter p;
    p.openCatalogFromPath(root.filePath("library.sqlite"));
    ASSERT_TRUE(
        wait_until([&] { return p.catalogOpen() && !p.busy() && p.visibleCount() == 1000; }));
    p.selectLibraryRow(850);
    ASSERT_TRUE(wait_until([&] { return p.selectedAssetId() == "ast_page_850"; }));
    auto generation = testing::StudioAutomationTestControl::generation(p);
    p.library()->setSort("name", "asc");
    ASSERT_TRUE(wait_until(
        [&]
        {
            return testing::StudioAutomationTestControl::generation(p) > generation + 1 &&
                   !p.assets()->rowLoaded(850) && !p.busy() && p.selectedIndex() >= 0;
        }));
    LibraryPageRequest locate;
    locate.query.sort_field = AssetSortField::kDisplayName;
    locate.query.sort_direction = SortDirection::kAscending;
    locate.around_asset_id = "ast_page_850";
    locate.limit = 1;
    auto found = testing::StudioAutomationTestControl::query(p, locate);
    ASSERT_TRUE(found);
    auto rank = found.value().offset;
    EXPECT_EQ(p.selectedIndex(), static_cast<int>(rank));
    locate.around_asset_id.reset();
    locate.offset = rank + 1;
    auto next = testing::StudioAutomationTestControl::query(p, locate);
    ASSERT_TRUE(next);
    ASSERT_EQ(next.value().assets.size(), 1U);
    const auto expected = QString::fromStdString(next.value().assets.front().id);
    p.selectNext();
    ASSERT_TRUE(wait_until([&] { return p.selectedAssetId() != "ast_page_850"; }));
    EXPECT_EQ(p.selectedAssetId(), expected);
}

TEST(StudioAutomationRegression, StrictPreviewDoesNotAdoptChangedRecipeWhileWaiting)
{
    ensure_qt_core();
    init_logging("ravo-review-probes");
    QTemporaryDir root;
    ScopedEnvironmentVariable registry("RAVO_LIVE_CONTROL_DIR", root.filePath("control").toUtf8());
    make_paged_catalog(root, 1);
    StudioPresenter p;
    StudioCommandController commands(p);
    auto live = StudioLiveSessionController::create(p, commands);
    ASSERT_TRUE(live);
    p.openCatalogFromPath(root.filePath("library.sqlite"));
    ASSERT_TRUE(wait_until([&] { return p.catalogOpen() && !p.busy(); }));
    ASSERT_TRUE(wait_until(
        [&]
        {
            return live.value()->snapshot().find("recipe")->find("current")->object_if() != nullptr;
        }));
    auto first = live.value()->snapshot();
    auto second = first;
    auto first_object = *first.object_if();
    first_object["revision"] = JsonValue::number("100");
    first_object["busy"] = true;
    first_object["session_id"] = "review-preview";
    auto first_recipe = *first_object.at("recipe").object_if();
    first_recipe["revision"] = JsonValue::number("10");
    first_object["recipe"] = JsonValue{first_recipe};
    first = JsonValue{first_object};
    auto second_object = *second.object_if();
    second_object["revision"] = JsonValue::number("101");
    second_object["busy"] = false;
    second_object["session_id"] = "review-preview";
    auto second_recipe = *second_object.at("recipe").object_if();
    auto recipe = parse_recipe_json(serialize_json(second_recipe.at("current")));
    ASSERT_TRUE(recipe);
    auto develop = develop_from_recipe(recipe.value());
    ASSERT_TRUE(develop);
    ASSERT_TRUE(apply_develop_field_strict(develop.value(), "exposure", 1.0));
    auto updated = recipe_from_develop(recipe.value().asset, develop.value());
    ASSERT_TRUE(updated);
    auto encoded = serialize_recipe(updated.value());
    ASSERT_TRUE(encoded);
    auto json = parse_json(encoded.value());
    ASSERT_TRUE(json);
    second_recipe["current"] = std::move(json).value();
    second_recipe["revision"] = JsonValue::number("11");
    second_recipe["state"] = "pending";
    second_object["recipe"] = JsonValue{second_recipe};
    second = JsonValue{second_object};
    LiveSessionDescriptor descriptor;
    descriptor.session_id = "review-preview";
    descriptor.server_name = "ravo-review-preview";
    descriptor.process_id = static_cast<std::uint64_t>(QCoreApplication::applicationPid());
    descriptor.executable_path = RAVO_CLI_EXECUTABLE;
    descriptor.workspace_root = RAVO_REPOSITORY_ROOT;
    int calls = 0;
    auto server =
        LocalControlServer::start(descriptor, [&](const LiveControlRequest &) -> Result<JsonValue>
                                  { return calls++ == 0 ? first : second; });
    ASSERT_TRUE(server);
    auto selection =
        QString::fromStdString(first.find("selection")->find("revision")->number_if()->text);
    const auto output = root.filePath("strict-preview.png");
    auto result = run_cli_process({"studio", "preview", "--session-id", "review-preview",
                                   "--asset-id", "ast_page_0", "--expect-session-revision", "100",
                                   "--expect-selection-revision", selection, "--output", output,
                                   "--max-edge", "32", "--timeout-ms", "5000", "--json"});
    EXPECT_NE(result.exit_code, 0) << result.standard_output.toStdString();
    EXPECT_FALSE(QFileInfo::exists(output)) << "Published recipe 11 after recipe 10 was observed";
}

TEST(StudioAutomationRegression, PostDispatchTimeoutReportsWhetherMutationCommitted)
{
    ensure_qt_core();
    init_logging("ravo-review-probes");
    QTemporaryDir root;
    ScopedEnvironmentVariable registry("RAVO_LIVE_CONTROL_DIR", root.filePath("control").toUtf8());
    make_paged_catalog(root, 1);
    StudioPresenter p;
    StudioCommandController commands(p);
    auto live = StudioLiveSessionController::create(p, commands);
    ASSERT_TRUE(live);
    p.openCatalogFromPath(root.filePath("library.sqlite"));
    ASSERT_TRUE(wait_until([&] { return p.catalogOpen() && !p.busy(); }));
    p.openDevelop();
    ASSERT_TRUE(wait_until(
        [&]
        {
            auto s = live.value()->snapshot();
            return s.find("recipe")->find("current")->object_if() &&
                   !*s.find("busy")->boolean_if() && !p.inspect()->previewLoading();
        }));
    LiveSessionDescriptor descriptor;
    descriptor.session_id = "review-timeout";
    descriptor.server_name = "ravo-review-timeout";
    descriptor.process_id = static_cast<std::uint64_t>(QCoreApplication::applicationPid());
    descriptor.executable_path = RAVO_CLI_EXECUTABLE;
    descriptor.workspace_root = RAVO_REPOSITORY_ROOT;
    bool dispatched = false;
    auto server = LocalControlServer::start(
        descriptor,
        [&](const LiveControlRequest &request) -> Result<JsonValue>
        {
            if (request.method == "operation")
            {
                const auto *action = request.params.find("action");
                if (action && action->string_if() && *action->string_if() == "reserve")
                    return JsonValue{JsonValue::Object{{"operation_id", "review-operation"},
                                                       {"phase", "reserved"}}};
                auto state = live.value()->snapshot();
                auto stored = testing::StudioAutomationTestControl::saved(p);
                if (!stored)
                    return stored.error();
                auto params = develop_from_recipe(stored.value());
                if (!params)
                    return params.error();
                const bool committed = params.value().exposure_ev == 0.5;
                return JsonValue{JsonValue::Object{
                    {"operation_id", "review-operation"},
                    {"phase", committed ? "committed" : "pending"},
                    {"mutation_applied", committed ? JsonValue{true} : JsonValue{nullptr}},
                    {"catalog_revision", *state.find("catalog")->find("revision")}}};
            }
            if (request.method == "develop")
            {
                auto applied = commands.applyDevelopFields({{"exposure", 0.5}});
                if (!applied)
                    return applied.error();
                dispatched = true;
            }
            auto state = live.value()->snapshot();
            if (dispatched)
            {
                auto object = *state.object_if();
                object["busy"] = true;
                state = JsonValue{object};
            } // hold post-commit settlement only
            return state;
        });
    ASSERT_TRUE(server);
    auto result =
        run_cli_process({"studio", "develop", "--session-id", "review-timeout", "--asset-id",
                         "ast_page_0", "--set", "exposure=0.5", "--timeout-ms", "100", "--json"},
                        5000);
    ASSERT_TRUE(dispatched);
    EXPECT_NE(result.exit_code, 0);
    ASSERT_TRUE(
        wait_until([&] { return !p.busy() && !p.develop()->state().pending_save_.has_value(); }));
    auto saved = testing::StudioAutomationTestControl::saved(p);
    ASSERT_TRUE(saved);
    auto params = develop_from_recipe(saved.value());
    ASSERT_TRUE(params);
    EXPECT_DOUBLE_EQ(params.value().exposure_ev, 0.5);
    EXPECT_NE(result.standard_output.indexOf("mutation_applied"), -1)
        << result.standard_output.toStdString();
    auto envelope = parse_json(result.standard_output.toStdString());
    ASSERT_TRUE(envelope);
    const auto *context = envelope.value().find("error")->find("context");
    ASSERT_NE(context, nullptr);
    ASSERT_NE(context->find("mutation_applied"), nullptr);
    EXPECT_EQ(*context->find("mutation_applied")->string_if(), "true");
}

} // namespace ravo
