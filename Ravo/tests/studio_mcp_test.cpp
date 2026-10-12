#include "mcp_test_support.h"
#include "studio_library_test_support.h"
#include "studio_automation_test_control.h"
#include "ravo/desktop/studio_command_controller.h"
#include "ravo/desktop/studio_live_session_controller.h"
#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/adapters/text_file.h"
#include "ravo/adapters/filesystem_recovery_store.h"
#include "ravo/foundation/log.h"
#include "ravo/recipe/local_adjustment.h"
#include <gtest/gtest.h>
#include <QCryptographicHash>
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <fstream>

namespace ravo
{
using namespace studio_test_support;
namespace
{
class StudioMcpTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensure_qt_core();
        init_logging("ravo-studio-mcp-test");
        registry = std::make_unique<ScopedEnvironmentVariable>("RAVO_LIVE_CONTROL_DIR",
                                                               root.filePath("control").toUtf8());
        make_paged_catalog(root, catalogSize());
        presenter = std::make_unique<StudioPresenter>();
        commands = std::make_unique<StudioCommandController>(*presenter);
        auto created = StudioLiveSessionController::create(*presenter, *commands);
        ASSERT_TRUE(created);
        live = std::move(created).value();
        presenter->openCatalogFromPath(root.filePath("library.sqlite"));
        ASSERT_TRUE(wait_until([&] { return presenter->catalogOpen() && !presenter->busy(); }));
        presenter->openDevelop();
        ASSERT_TRUE(ready());
        session = live->descriptor().session_id;
        ASSERT_TRUE(mcp.start());
    }
    void TearDown() override
    {
        EXPECT_TRUE(mcp.close());
    }
    virtual int catalogSize() const
    {
        return 3;
    }
    bool ready()
    {
        return wait_until(
            [&]
            {
                const auto state = live->snapshot();
                return state.find("recipe")->find("current")->object_if() &&
                       !*state.find("busy")->boolean_if() &&
                       !presenter->inspect()->previewLoading();
            });
    }
    Result<JsonValue> call(std::string_view name, JsonValue args)
    {
        auto result = mcp.tool(name, std::move(args));
        if (!result)
            return result.error();
        return mcp_data(result.value());
    }
    Result<JsonValue> state()
    {
        return call("ravo.studio.state", JsonValue::Object{{"session_id", session}});
    }
    Result<JsonValue> reserve()
    {
        return call("ravo.operations.reserve", JsonValue::Object{{"session_id", session}});
    }
    Result<JsonValue> evaluate(const JsonValue &binding, double exposure, std::string local = {})
    {
        JsonValue::Object args{{"binding", binding},
                               {"fields", exposure_patch(exposure)},
                               {"max_edge", JsonValue::number("32")}};
        if (!local.empty())
            args.emplace("local_id", std::move(local));
        return call("ravo.develop.evaluate", JsonValue{std::move(args)});
    }
    Result<JsonValue> mask(std::string action, JsonValue args)
    {
        if (!ready())
            return make_error(ErrorCode::kIo, "Studio did not become ready");
        auto current = state();
        if (!current)
            return current.error();
        return call("ravo.mask.apply",
                    JsonValue::Object{{"binding", *current.value().find("binding")},
                                      {"action", std::move(action)},
                                      {"arguments", std::move(args)}});
    }
    QTemporaryDir root;
    std::unique_ptr<ScopedEnvironmentVariable> registry;
    std::unique_ptr<StudioPresenter> presenter;
    std::unique_ptr<StudioCommandController> commands;
    std::unique_ptr<StudioLiveSessionController> live;
    std::string session;
    McpProcess mcp;
};
} // namespace

TEST(StudioMcpProtocolTest, SupportsLegacyAndModernContractsAndRejectsUnknownInputs)
{
    ensure_qt_core();
    McpProcess mcp;
    ASSERT_TRUE(mcp.start());
    mcp.send(JsonValue::Object{
        {"jsonrpc", "2.0"},
        {"id", JsonValue::number("100")},
        {"method", "initialize"},
        {"params",
         JsonValue::Object{{"protocolVersion", "2025-11-25"},
                           {"capabilities", JsonValue::Object{}},
                           {"clientInfo", JsonValue::Object{{"name", "Ravo contract test"},
                                                            {"version", "1"}}}}}});
    auto initialized = mcp.response(100);
    ASSERT_TRUE(initialized);
    ASSERT_NE(initialized.value().find("result"), nullptr);
    EXPECT_EQ(json_string(*initialized.value().find("result"), "protocolVersion"), "2025-11-25");
    EXPECT_EQ(initialized.value().find("result")->find("resultType"), nullptr);
    mcp.send(JsonValue::Object{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
    mcp.send(JsonValue::Object{
        {"jsonrpc", "2.0"}, {"id", JsonValue::number("101")}, {"method", "tools/list"}});
    auto legacy = mcp.response(101);
    ASSERT_TRUE(legacy);
    ASSERT_NE(legacy.value().find("result"), nullptr);
    const auto *tools = legacy.value().find("result")->find("tools")->array_if();
    ASSERT_NE(tools, nullptr);
    ASSERT_EQ(tools->size(), 18U);
    std::string previous;
    for (const auto &tool : *tools)
    {
        const auto name = json_string(tool, "name");
        EXPECT_GT(name, previous);
        previous = name;
        ASSERT_NE(tool.find("inputSchema"), nullptr);
        ASSERT_NE(tool.find("outputSchema"), nullptr);
    }
    auto capabilities = mcp.tool("ravo.develop.fields");
    ASSERT_TRUE(capabilities);
    auto capabilities_data = mcp_data(capabilities.value());
    ASSERT_TRUE(capabilities_data);
    const auto *annotations = capabilities_data.value().find("annotations");
    ASSERT_NE(annotations, nullptr);
    const auto *kinds = annotations->find("kinds");
    ASSERT_NE(kinds, nullptr);
    for (const auto &tool : *tools)
    {
        const auto name = json_string(tool, "name");
        const char *kind = name == "ravo.review.apply"     ? "review" :
                           name == "ravo.metadata.patch"   ? "metadata" :
                           name == "ravo.keywords.replace" ? "keywords" :
                                                             nullptr;
        if (!kind)
            continue;
        const auto *properties = tool.find("inputSchema")->find("properties");
        ASSERT_NE(properties, nullptr);
        const auto *schema =
            properties->find(name == "ravo.keywords.replace" ? "keywords" : "fields");
        ASSERT_NE(schema, nullptr);
        ASSERT_NE(kinds->find(kind), nullptr);
        EXPECT_EQ(serialize_json(*schema), serialize_json(*kinds->find(kind))) << name;
    }
    auto modern = mcp.request("server/discover");
    ASSERT_TRUE(modern);
    ASSERT_NE(modern.value().find("result"), nullptr);
    EXPECT_EQ(json_string(*modern.value().find("result"), "resultType"), "complete");
    mcp.send(JsonValue::Object{
        {"jsonrpc", "2.0"},
        {"id", JsonValue::number("102")},
        {"method", "tools/list"},
        {"params",
         JsonValue::Object{{"_meta", JsonValue::Object{{"io.modelcontextprotocol/protocolVersion",
                                                        "2099-01-01"}}}}}});
    auto unsupported = mcp.response(102);
    ASSERT_TRUE(unsupported);
    ASSERT_NE(unsupported.value().find("error"), nullptr);
    EXPECT_EQ(unsupported.value().find("error")->find("code")->number_if()->text, "-32022");
    auto forbidden =
        mcp.request("resources/read", JsonValue::Object{{"uri", "file:///etc/passwd"}});
    ASSERT_TRUE(forbidden);
    EXPECT_NE(forbidden.value().find("error"), nullptr);
    auto unknown = mcp.tool("ravo.develop.fields", JsonValue::Object{{"unknown", true}});
    ASSERT_TRUE(unknown);
    EXPECT_TRUE(*unknown.value().find("result")->find("isError")->boolean_if());
    mcp.raw("{broken\n");
    mcp.raw(QByteArray(8 * 1024 * 1024 + 1, 'x') + '\n');
    auto ping = mcp.request("ping");
    ASSERT_TRUE(ping);
    EXPECT_NE(ping.value().find("result"), nullptr);
    auto fields = mcp.tool("ravo.develop.fields");
    ASSERT_TRUE(fields);
    auto data = mcp_data(fields.value());
    ASSERT_TRUE(data) << data.error().message;
    ASSERT_NE(data.value().find("mask_kinds"), nullptr);
    EXPECT_EQ(data.value().find("mask_kinds")->array_if()->size(), 8U);
    EXPECT_TRUE(mcp.close());
}

TEST_F(StudioMcpTest, CatalogQueryThenExplicitSelectionCanEvaluateAndCommit)
{
    QFile original_source(root.filePath("photo-1.png"));
    ASSERT_TRUE(original_source.open(QIODevice::ReadOnly));
    const auto source_hash =
        QCryptographicHash::hash(original_source.readAll(), QCryptographicHash::Sha256);
    original_source.close();
    auto current = state();
    ASSERT_TRUE(current);
    auto page = call("ravo.catalog.query",
                     JsonValue::Object{{"catalog", root.filePath("library.sqlite").toStdString()},
                                       {"limit", JsonValue::number("2")}});
    ASSERT_TRUE(page);
    EXPECT_EQ(json_string(page.value(), "schema"), "ravo.library.page/v1");
    ASSERT_EQ(page.value().find("assets")->array_if()->size(), 2U);
    EXPECT_TRUE(*page.value().find("has_more")->boolean_if());
    auto second = call("ravo.catalog.query",
                       JsonValue::Object{{"catalog", root.filePath("library.sqlite").toStdString()},
                                         {"limit", JsonValue::number("2")},
                                         {"offset", *page.value().find("next_offset")},
                                         {"expected_revision", *page.value().find("revision")}});
    ASSERT_TRUE(second);
    ASSERT_EQ(second.value().find("assets")->array_if()->size(), 1U);
    EXPECT_FALSE(call("ravo.catalog.query",
                      JsonValue::Object{{"catalog", root.filePath("library.sqlite").toStdString()},
                                        {"set_id", "missing"}}));
    auto reserved = reserve();
    ASSERT_TRUE(reserved);
    const auto operation = json_string(reserved.value(), "operation_id");
    JsonValue arguments = JsonValue::Object{{"binding", *current.value().find("binding")},
                                            {"target_asset_id", "ast_page_1"},
                                            {"operation_id", operation}};
    auto selected = call("ravo.studio.select", arguments);
    ASSERT_TRUE(selected) << selected.error().message;
    EXPECT_EQ(json_string(selected.value(), "phase"), "staged");
    EXPECT_FALSE(*selected.value().find("mutation_applied")->boolean_if());
    EXPECT_EQ(json_string(*selected.value().find("result"), "target_asset_id"), "ast_page_1");
    ASSERT_TRUE(ready());
    EXPECT_EQ(presenter->selectedAssetId(), "ast_page_1");
    auto repository = SqliteCatalogRepository::open(root.filePath("library.sqlite").toStdString());
    ASSERT_TRUE(repository);
    for (const auto *asset : {"ast_page_0", "ast_page_1"})
    {
        auto stored = repository.value()->load_recipe_json(asset);
        ASSERT_TRUE(stored);
        EXPECT_FALSE(stored.value());
        auto history = repository.value()->list_recipe_history(asset);
        ASSERT_TRUE(history);
        EXPECT_TRUE(history.value().empty());
    }
    auto bound = state();
    ASSERT_TRUE(bound);
    auto trial = evaluate(*bound.value().find("binding"), 0.375);
    ASSERT_TRUE(trial);
    auto committed = call("ravo.develop.commit",
                          JsonValue::Object{{"session_id", session},
                                            {"candidate", *trial.value().find("candidate")}});
    ASSERT_TRUE(committed);
    EXPECT_EQ(json_string(committed.value(), "phase"), "committed");
    ASSERT_TRUE(ready());
    presenter->selectAsset("ast_page_2");
    ASSERT_TRUE(ready());
    auto replay = call("ravo.studio.select", arguments);
    ASSERT_TRUE(replay);
    EXPECT_EQ(serialize_json(replay.value()), serialize_json(selected.value()));
    EXPECT_EQ(presenter->selectedAssetId(), "ast_page_2");
    auto changed_request = *arguments.object_if();
    changed_request["target_asset_id"] = "ast_page_0";
    EXPECT_FALSE(call("ravo.studio.select", std::move(changed_request)));
    EXPECT_EQ(presenter->selectedAssetId(), "ast_page_2");
    ASSERT_TRUE(original_source.open(QIODevice::ReadOnly));
    EXPECT_EQ(QCryptographicHash::hash(original_source.readAll(), QCryptographicHash::Sha256),
              source_hash);
}

TEST_F(StudioMcpTest, ExplicitSelectionRejectsUnknownStaleAndDraftWithoutRetargeting)
{
    auto current = state();
    ASSERT_TRUE(current);
    const auto original = presenter->selectedAssetId();
    const auto binding = *current.value().find("binding");
    auto unknown = call("ravo.studio.select",
                        JsonValue::Object{{"binding", binding}, {"target_asset_id", "missing"}});
    EXPECT_FALSE(unknown);
    EXPECT_EQ(presenter->selectedAssetId(), original);
    ASSERT_TRUE(ready());
    current = state();
    ASSERT_TRUE(current);
    auto stale = *current.value().find("binding")->object_if();
    stale["expected_catalog_revision"] = JsonValue::number("0");
    EXPECT_FALSE(call("ravo.studio.select", JsonValue::Object{{"binding", std::move(stale)},
                                                              {"target_asset_id", "ast_page_1"}}));
    EXPECT_EQ(presenter->selectedAssetId(), original);
    presenter->develop()->previewDevelopNumber("exposure", 0.4);
    ASSERT_TRUE(ready());
    current = state();
    ASSERT_TRUE(current);
    auto draft =
        call("ravo.studio.select", JsonValue::Object{{"binding", *current.value().find("binding")},
                                                     {"target_asset_id", "ast_page_1"}});
    EXPECT_FALSE(draft);
    EXPECT_EQ(presenter->selectedAssetId(), original);
    EXPECT_DOUBLE_EQ(presenter->develop()->state().develop_.exposure_ev, 0.4);
}

TEST_F(StudioMcpTest, QueuedExplicitSelectionCanCancelOrRejectConcurrentNavigation)
{
    for (const int action : {0, 1, 2})
    {
        const bool cancel = action == 0;
        ASSERT_TRUE(ready());
        auto current = state();
        ASSERT_TRUE(current);
        auto reserved = reserve();
        ASSERT_TRUE(reserved);
        const auto operation = json_string(reserved.value(), "operation_id");
        const auto original = presenter->selectedAssetId();
        auto entered = std::make_shared<std::promise<void>>();
        std::promise<void> release;
        ASSERT_TRUE(testing::StudioAutomationTestControl::block(*presenter, entered,
                                                                release.get_future().share()));
        auto unblock = qScopeGuard([&] { release.set_value(); });
        ASSERT_EQ(entered->get_future().wait_for(std::chrono::seconds(5)),
                  std::future_status::ready);
        auto pending = call("ravo.studio.select",
                            JsonValue::Object{{"binding", *current.value().find("binding")},
                                              {"target_asset_id", "ast_page_1"},
                                              {"operation_id", operation},
                                              {"wait", false}});
        ASSERT_TRUE(pending);
        EXPECT_EQ(json_string(pending.value(), "phase"), "pending");
        if (cancel)
            ASSERT_TRUE(
                call("ravo.operations.cancel",
                     JsonValue::Object{{"session_id", session}, {"operation_id", operation}}));
        else if (action == 1)
            presenter->selectAsset("ast_page_2");
        else
            presenter->library()->setSort("name", "asc");
        release.set_value();
        unblock.dismiss();
        auto receipt = call("ravo.operations.get", JsonValue::Object{{"session_id", session},
                                                                     {"operation_id", operation},
                                                                     {"wait", true}});
        ASSERT_TRUE(receipt);
        EXPECT_EQ(json_string(receipt.value(), "phase"), cancel ? "cancelled" : "failed");
        EXPECT_FALSE(*receipt.value().find("mutation_applied")->boolean_if());
        EXPECT_EQ(presenter->selectedAssetId(),
                  action == 1 ? QStringLiteral("ast_page_2") : original);
    }
}

namespace
{
class StudioMcpPagedTest : public StudioMcpTest
{
protected:
    int catalogSize() const override
    {
        return 250;
    }
};
} // namespace

TEST_F(StudioMcpPagedTest, ExplicitSelectionResolvesAnUnloadedCatalogAsset)
{
    ASSERT_FALSE(presenter->assets()->rowLoaded(249));
    auto current = state();
    ASSERT_TRUE(current);
    auto selected =
        call("ravo.studio.select", JsonValue::Object{{"binding", *current.value().find("binding")},
                                                     {"target_asset_id", "ast_page_249"}});
    ASSERT_TRUE(selected) << selected.error().message;
    ASSERT_TRUE(ready());
    EXPECT_EQ(presenter->selectedAssetId(), "ast_page_249");
    EXPECT_EQ(presenter->selectedCount(), 1);
    EXPECT_EQ(presenter->selectedMediaType(), "image/png");
    EXPECT_EQ(presenter->selectedIndex(), 249);
    auto entered = std::make_shared<std::promise<void>>();
    std::promise<void> release;
    ASSERT_TRUE(testing::StudioAutomationTestControl::block(*presenter, entered,
                                                            release.get_future().share()));
    auto unblock = qScopeGuard([&] { release.set_value(); });
    ASSERT_EQ(entered->get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
    presenter->selectPrevious();
    EXPECT_TRUE(presenter->selectionResolving());
    EXPECT_TRUE(presenter->busy());
    auto pending = state();
    ASSERT_TRUE(pending);
    EXPECT_TRUE(*pending.value().find("busy")->boolean_if());
    EXPECT_TRUE(*pending.value().find("selection")->find("resolving")->boolean_if());
    EXPECT_FALSE(
        call("ravo.studio.select", JsonValue::Object{{"binding", *pending.value().find("binding")},
                                                     {"target_asset_id", "ast_page_0"}}));
    auto edit = commands->applyDevelopFields({{"exposure", 0.25}});
    EXPECT_FALSE(edit && edit.value());
    presenter->setAssetTags("Must not tag old selection");
    presenter->setMetadataFields({{"title", "Must not annotate old selection"}}, {});
    presenter->applyCullReview("pick", 5, {}, false);
    EXPECT_EQ(presenter->selectedAssetId(), "ast_page_249");
    release.set_value();
    unblock.dismiss();
    ASSERT_TRUE(wait_until([&] { return presenter->selectedAssetId() == "ast_page_248"; }));
    ASSERT_TRUE(ready());
    EXPECT_EQ(presenter->selectedAssetId(), "ast_page_248");
    auto repository = SqliteCatalogRepository::open(root.filePath("library.sqlite").toStdString());
    ASSERT_TRUE(repository);
    auto previous = repository.value()->find_asset_by_id("ast_page_249");
    ASSERT_TRUE(previous && previous.value());
    EXPECT_TRUE(previous.value()->tags.empty());
    EXPECT_FALSE(previous.value()->metadata.title);
    EXPECT_EQ(previous.value()->review.rating, 0);
    EXPECT_FALSE(previous.value()->review.picked);
}

TEST_F(StudioMcpTest, ExplicitSelectionCanReturnFromVideoWithoutADevelopRecipe)
{
    presenter->imports()->importFilePaths(
        {QStringLiteral(RAVO_REPOSITORY_ROOT "/Ravo/tests/fixtures/video/hlg.mp4")});
    ASSERT_TRUE(wait_until(
        [&] { return !presenter->imports()->importWorkActive() && !presenter->busy(); }));
    presenter->selectAsset("ast_page_0");
    ASSERT_TRUE(ready());
    auto page = call("ravo.catalog.query",
                     JsonValue::Object{{"catalog", root.filePath("library.sqlite").toStdString()}});
    ASSERT_TRUE(page);
    std::string video;
    for (const auto &asset : *page.value().find("assets")->array_if())
        if (json_string(asset, "media_type").starts_with("video/"))
            video = json_string(asset, "id");
    ASSERT_FALSE(video.empty());
    auto current = state();
    ASSERT_TRUE(current);
    auto selected =
        call("ravo.studio.select", JsonValue::Object{{"binding", *current.value().find("binding")},
                                                     {"target_asset_id", video}});
    ASSERT_TRUE(selected);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter->video()->state() == "paused" &&
                   !presenter->inspect()->previewLoading();
        }));
    current = state();
    ASSERT_TRUE(current);
    EXPECT_EQ(json_string(*current.value().find("binding"), "asset_id"), video);
    EXPECT_FALSE(current.value().find("recipe")->find("current")->object_if());
    auto video_review =
        call("ravo.review.apply",
             JsonValue::Object{{"binding", *current.value().find("binding")},
                               {"fields", JsonValue::Object{{"rating", JsonValue::number("3")}}}});
    ASSERT_TRUE(video_review);
    ASSERT_TRUE(wait_until([&] { return !presenter->busy(); }));
    current = state();
    ASSERT_TRUE(current);
    EXPECT_EQ(current.value()
                  .find("selection")
                  ->find("annotations")
                  ->find("review")
                  ->find("rating")
                  ->number_if()
                  ->text,
              "3");
    auto photo =
        call("ravo.studio.select", JsonValue::Object{{"binding", *current.value().find("binding")},
                                                     {"target_asset_id", "ast_page_0"}});
    ASSERT_TRUE(photo) << photo.error().message;
    ASSERT_TRUE(ready());
    EXPECT_EQ(presenter->selectedAssetId(), "ast_page_0");
    EXPECT_FALSE(presenter->video()->available());
}

TEST_F(StudioMcpTest, EofCancelsInflightExplicitSelection)
{
    auto current = state();
    ASSERT_TRUE(current);
    const auto original = presenter->selectedAssetId();
    auto reserved = reserve();
    ASSERT_TRUE(reserved);
    const auto operation = json_string(reserved.value(), "operation_id");
    auto entered = std::make_shared<std::promise<void>>();
    std::promise<void> release;
    ASSERT_TRUE(testing::StudioAutomationTestControl::block(*presenter, entered,
                                                            release.get_future().share()));
    auto unblock = qScopeGuard([&] { release.set_value(); });
    ASSERT_EQ(entered->get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
    mcp.send(JsonValue::Object{
        {"jsonrpc", "2.0"},
        {"id", JsonValue::number("902")},
        {"method", "tools/call"},
        {"params",
         JsonValue::Object{
             {"name", "ravo.studio.select"},
             {"arguments", JsonValue::Object{{"binding", *current.value().find("binding")},
                                             {"target_asset_id", "ast_page_1"},
                                             {"operation_id", operation}}},
             {"_meta", JsonValue::Object{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                         {"io.modelcontextprotocol/clientCapabilities",
                                          JsonValue::Object{}}}}}}});
    ASSERT_TRUE(wait_until([&] { return presenter->busy(); }));
    EXPECT_TRUE(mcp.close());
    release.set_value();
    unblock.dismiss();
    ASSERT_TRUE(ready());
    EXPECT_EQ(presenter->selectedAssetId(), original);
    ASSERT_TRUE(mcp.start());
    auto receipt = call(
        "ravo.operations.get",
        JsonValue::Object{{"session_id", session}, {"operation_id", operation}, {"wait", true}});
    ASSERT_TRUE(receipt);
    EXPECT_EQ(json_string(receipt.value(), "phase"), "cancelled");
}

TEST_F(StudioMcpTest, StudioCloseCancelsQueuedSelectionBeforeDestroyingBorrowedOwner)
{
    auto current = state();
    ASSERT_TRUE(current);
    auto entered = std::make_shared<std::promise<void>>();
    std::promise<void> release;
    ASSERT_TRUE(testing::StudioAutomationTestControl::block(*presenter, entered,
                                                            release.get_future().share()));
    bool released = false;
    auto unblock = qScopeGuard(
        [&]
        {
            if (!released)
                release.set_value();
        });
    ASSERT_EQ(entered->get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
    auto pending =
        call("ravo.studio.select", JsonValue::Object{{"binding", *current.value().find("binding")},
                                                     {"target_asset_id", "ast_page_1"},
                                                     {"wait", false}});
    ASSERT_TRUE(pending);
    EXPECT_EQ(json_string(pending.value(), "phase"), "pending");
    QObject::connect(presenter.get(), &StudioPresenter::aboutToShutdown, presenter.get(),
                     [&]
                     {
                         released = true;
                         release.set_value();
                     });
    presenter.reset();
    live.reset();
    commands.reset();
    auto ping = mcp.request("ping");
    ASSERT_TRUE(ping);
    EXPECT_NE(ping.value().find("result"), nullptr);
    auto repository = SqliteCatalogRepository::open(root.filePath("library.sqlite").toStdString());
    ASSERT_TRUE(repository);
    for (const auto *asset : {"ast_page_0", "ast_page_1"})
    {
        auto recipe = repository.value()->load_recipe_json(asset);
        ASSERT_TRUE(recipe);
        EXPECT_FALSE(recipe.value());
        auto history = repository.value()->list_recipe_history(asset);
        ASSERT_TRUE(history);
        EXPECT_TRUE(history.value().empty());
    }
}

TEST_F(StudioMcpTest, AnnotationsCommitReadBackAndReplayWithoutChangingPhotoOrHistory)
{
    auto current = state();
    ASSERT_TRUE(current);
    const auto asset = json_string(*current.value().find("binding"), "asset_id");
    const auto before_annotations = current.value().find("selection")->find("annotations");
    ASSERT_NE(before_annotations, nullptr);
    EXPECT_TRUE(*before_annotations->find("available")->boolean_if());
    auto repository = SqliteCatalogRepository::open(root.filePath("library.sqlite").toStdString());
    ASSERT_TRUE(repository);
    const auto recipe = repository.value()->load_recipe_json(asset).value();
    const auto history = repository.value()->list_recipe_history(asset).value().size();
    QFile source(root.filePath("photo-0.png"));
    ASSERT_TRUE(source.open(QIODevice::ReadOnly));
    const auto source_hash = QCryptographicHash::hash(source.readAll(), QCryptographicHash::Sha256);
    source.close();
    auto reserved = reserve();
    ASSERT_TRUE(reserved);
    JsonValue args =
        JsonValue::Object{{"binding", *current.value().find("binding")},
                          {"fields", JsonValue::Object{{"rating", JsonValue::number("5")},
                                                       {"flag", "pick"},
                                                       {"color_label", "blue"}}},
                          {"operation_id", json_string(reserved.value(), "operation_id")}};
    auto applied = call("ravo.review.apply", args);
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_EQ(json_string(applied.value(), "phase"), "committed");
    EXPECT_TRUE(*applied.value().find("mutation_applied")->boolean_if());
    EXPECT_TRUE(applied.value().find("result")->find("warnings")->array_if()->empty());
    const auto revision = repository.value()->snapshot().value().revision;
    auto replay = call("ravo.review.apply", args);
    ASSERT_TRUE(replay);
    EXPECT_EQ(serialize_json(replay.value()), serialize_json(applied.value()));
    EXPECT_EQ(repository.value()->snapshot().value().revision, revision);
    auto conflicting = *args.object_if();
    conflicting["fields"] = JsonValue::Object{{"rating", JsonValue::number("4")}};
    EXPECT_FALSE(call("ravo.review.apply", conflicting));
    EXPECT_EQ(repository.value()->snapshot().value().revision, revision);

    const auto annotate = [&](const char *tool, JsonValue fields)
    {
        if (!ready())
            return Result<JsonValue>{make_error(ErrorCode::kIo, "Studio did not become ready")};
        auto snapshot = state();
        if (!snapshot)
            return Result<JsonValue>{snapshot.error()};
        return call(tool,
                    JsonValue::Object{
                        {"binding", *snapshot.value().find("binding")},
                        {std::string_view(tool) == "ravo.keywords.replace" ? "keywords" : "fields",
                         std::move(fields)}});
    };
    ASSERT_TRUE(annotate("ravo.metadata.patch", JsonValue::Object{{"title", "AI chosen"},
                                                                  {"description", "Temporary note"},
                                                                  {"creator", "Photographer"}}));
    ASSERT_TRUE(
        annotate("ravo.keywords.replace", JsonValue::Array{"People|Alice", "Travel|Japan"}));
    ASSERT_TRUE(
        annotate("ravo.metadata.patch", JsonValue::Object{{"description", JsonValue{nullptr}}}));
    ASSERT_TRUE(ready());
    current = state();
    ASSERT_TRUE(current);
    const auto &annotations = *current.value().find("selection")->find("annotations");
    EXPECT_EQ(annotations.find("review")->find("rating")->number_if()->text, "5");
    EXPECT_TRUE(*annotations.find("review")->find("picked")->boolean_if());
    EXPECT_FALSE(*annotations.find("review")->find("rejected")->boolean_if());
    EXPECT_EQ(json_string(*annotations.find("metadata"), "title"), "AI chosen");
    EXPECT_TRUE(annotations.find("metadata")->find("description")->is_null());
    EXPECT_EQ(annotations.find("keywords")->array_if()->size(), 2U);
    ASSERT_TRUE(annotate("ravo.review.apply", JsonValue::Object{{"flag", "reject"}}));
    ASSERT_TRUE(annotate("ravo.review.apply", JsonValue::Object{{"flag", "unflag"}}));
    ASSERT_TRUE(annotate("ravo.keywords.replace", JsonValue::Array{}));
    auto stored = repository.value()->find_asset_by_id(asset);
    ASSERT_TRUE(stored && stored.value());
    EXPECT_FALSE(stored.value()->review.picked);
    EXPECT_FALSE(stored.value()->review.rejected);
    EXPECT_TRUE(stored.value()->tags.empty());
    EXPECT_FALSE(repository.value()->list_keywords().value().empty());
    EXPECT_EQ(repository.value()->load_recipe_json(asset).value(), recipe);
    EXPECT_EQ(repository.value()->list_recipe_history(asset).value().size(), history);
    ASSERT_TRUE(source.open(QIODevice::ReadOnly));
    EXPECT_EQ(QCryptographicHash::hash(source.readAll(), QCryptographicHash::Sha256), source_hash);
}

TEST_F(StudioMcpTest, AnnotationTargetsOnlyTheExplicitPrimaryWithinMultiSelection)
{
    presenter->selectAllVisible();
    ASSERT_TRUE(ready());
    ASSERT_EQ(presenter->selectedCount(), 3);
    auto current = state();
    ASSERT_TRUE(current);
    const auto primary = json_string(*current.value().find("binding"), "asset_id");
    auto result = call("ravo.metadata.patch",
                       JsonValue::Object{{"binding", *current.value().find("binding")},
                                         {"fields", JsonValue::Object{{"title", "Primary only"}}}});
    ASSERT_TRUE(result);
    auto repository = SqliteCatalogRepository::open(root.filePath("library.sqlite").toStdString());
    ASSERT_TRUE(repository);
    for (const auto *id : {"ast_page_0", "ast_page_1", "ast_page_2"})
    {
        auto asset = repository.value()->find_asset_by_id(id);
        ASSERT_TRUE(asset && asset.value());
        if (primary == id)
            EXPECT_EQ(asset.value()->metadata.title, "Primary only");
        else
            EXPECT_FALSE(asset.value()->metadata.title);
    }
}

TEST_F(StudioMcpTest, AnnotationSchemasAndStaleBindingsRejectWithoutWrites)
{
    auto current = state();
    ASSERT_TRUE(current);
    const auto binding = *current.value().find("binding");
    auto repository = SqliteCatalogRepository::open(root.filePath("library.sqlite").toStdString());
    ASSERT_TRUE(repository);
    const auto revision = repository.value()->snapshot().value().revision;
    for (const auto &fields : std::vector<JsonValue>{
             JsonValue::Object{}, JsonValue::Object{{"rating", JsonValue::number("6")}},
             JsonValue::Object{{"flag", "toggle"}}, JsonValue::Object{{"picked", true}},
             JsonValue::Object{{"rating", true}}})
        EXPECT_FALSE(
            call("ravo.review.apply", JsonValue::Object{{"binding", binding}, {"fields", fields}}));
    for (const auto &fields : std::vector<JsonValue>{
             JsonValue::Object{}, JsonValue::Object{{"title", true}},
             JsonValue::Object{{"capture_time", "2026"}},
             JsonValue::Object{{"title", "Valid"}, {"unknown", JsonValue{nullptr}}}})
        EXPECT_FALSE(call("ravo.metadata.patch",
                          JsonValue::Object{{"binding", binding}, {"fields", fields}}));
    EXPECT_FALSE(
        call("ravo.keywords.replace",
             JsonValue::Object{{"binding", binding}, {"keywords", JsonValue::Array{"A", "A"}}}));
    auto stale = *binding.object_if();
    stale["expected_catalog_revision"] = JsonValue::number("0");
    EXPECT_FALSE(call(
        "ravo.metadata.patch",
        JsonValue::Object{{"binding", stale}, {"fields", JsonValue::Object{{"title", "Wrong"}}}}));
    EXPECT_EQ(repository.value()->snapshot().value().revision, revision);
    presenter->develop()->previewDevelopNumber("exposure", .4);
    ASSERT_TRUE(ready());
    current = state();
    ASSERT_TRUE(current);
    EXPECT_FALSE(
        call("ravo.review.apply",
             JsonValue::Object{{"binding", *current.value().find("binding")},
                               {"fields", JsonValue::Object{{"rating", JsonValue::number("5")}}}}));
    EXPECT_EQ(repository.value()->snapshot().value().revision, revision);
}

TEST_F(StudioMcpTest, QueuedAnnotationsCancelOnRequestAndChangedSelection)
{
    for (const bool cancel : {true, false})
    {
        ASSERT_TRUE(ready());
        auto current = state();
        ASSERT_TRUE(current);
        auto entered = std::make_shared<std::promise<void>>();
        std::promise<void> release;
        ASSERT_TRUE(testing::StudioAutomationTestControl::block(*presenter, entered,
                                                                release.get_future().share()));
        auto unblock = qScopeGuard([&] { release.set_value(); });
        ASSERT_EQ(entered->get_future().wait_for(std::chrono::seconds(5)),
                  std::future_status::ready);
        auto pending =
            call("ravo.metadata.patch",
                 JsonValue::Object{{"binding", *current.value().find("binding")},
                                   {"fields", JsonValue::Object{{"title", "Must not write"}}},
                                   {"wait", false}});
        ASSERT_TRUE(pending);
        EXPECT_EQ(json_string(pending.value(), "phase"), "pending");
        const auto id = json_string(pending.value(), "operation_id");
        if (cancel)
            ASSERT_TRUE(call("ravo.operations.cancel",
                             JsonValue::Object{{"session_id", session}, {"operation_id", id}}));
        else
            presenter->selectAsset("ast_page_1");
        release.set_value();
        unblock.dismiss();
        auto receipt =
            call("ravo.operations.get",
                 JsonValue::Object{{"session_id", session}, {"operation_id", id}, {"wait", true}});
        ASSERT_TRUE(receipt);
        EXPECT_EQ(json_string(receipt.value(), "phase"), "cancelled");
        EXPECT_FALSE(*receipt.value().find("mutation_applied")->boolean_if());
    }
    auto repository = SqliteCatalogRepository::open(root.filePath("library.sqlite").toStdString());
    ASSERT_TRUE(repository);
    for (const auto *asset : {"ast_page_0", "ast_page_1"})
        EXPECT_FALSE(repository.value()->find_asset_by_id(asset).value()->metadata.title);
}

TEST_F(StudioMcpTest, AnnotationRecoveryWarningIsCommittedAndNeverReplayed)
{
    auto current = state();
    ASSERT_TRUE(current);
    const auto asset = json_string(*current.value().find("binding"), "asset_id");
    auto initial =
        call("ravo.review.apply",
             JsonValue::Object{{"binding", *current.value().find("binding")},
                               {"fields", JsonValue::Object{{"rating", JsonValue::number("1")}}}});
    ASSERT_TRUE(initial);
    ASSERT_TRUE(ready());
    auto repository = SqliteCatalogRepository::open(root.filePath("library.sqlite").toStdString());
    ASSERT_TRUE(repository);
    const auto generation = repository.value()->recovery_state(asset).value().generation;
    const auto obsolete = std::filesystem::path(FilesystemRecoveryStore::default_root_for_catalog(
                              root.filePath("library.sqlite").toStdString())) /
                          (asset + "." + std::to_string(generation) + ".ravo.json");
    ASSERT_TRUE(std::filesystem::remove(obsolete));
    ASSERT_TRUE(std::filesystem::create_directory(obsolete));
    std::ofstream(obsolete / "blocker") << "retain recovery warning";
    current = state();
    ASSERT_TRUE(current);
    auto reserved = reserve();
    ASSERT_TRUE(reserved);
    JsonValue args = JsonValue::Object{
        {"binding", *current.value().find("binding")},
        {"fields", JsonValue::Object{{"title", "Committed despite recovery warning"}}},
        {"operation_id", json_string(reserved.value(), "operation_id")}};
    auto result = call("ravo.metadata.patch", args);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(json_string(result.value(), "phase"), "committed");
    EXPECT_TRUE(*result.value().find("mutation_applied")->boolean_if());
    ASSERT_EQ(result.value().find("result")->find("warnings")->array_if()->size(), 1U);
    const auto revision = repository.value()->snapshot().value().revision;
    auto replay = call("ravo.metadata.patch", args);
    ASSERT_TRUE(replay);
    EXPECT_EQ(serialize_json(replay.value()), serialize_json(result.value()));
    EXPECT_EQ(repository.value()->snapshot().value().revision, revision);
    EXPECT_EQ(repository.value()->find_asset_by_id(asset).value()->metadata.title,
              "Committed despite recovery warning");
}

TEST_F(StudioMcpTest, CompetingAnnotationClientsCannotBothCommitTheSameBinding)
{
    auto current = state();
    ASSERT_TRUE(current);
    auto repository = SqliteCatalogRepository::open(root.filePath("library.sqlite").toStdString());
    ASSERT_TRUE(repository);
    const auto revision = repository.value()->snapshot().value().revision;
    McpProcess other;
    ASSERT_TRUE(other.start());
    auto entered = std::make_shared<std::promise<void>>();
    std::promise<void> release;
    ASSERT_TRUE(testing::StudioAutomationTestControl::block(*presenter, entered,
                                                            release.get_future().share()));
    auto unblock = qScopeGuard([&] { release.set_value(); });
    ASSERT_EQ(entered->get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
    const auto binding = *current.value().find("binding");
    auto first = call("ravo.metadata.patch",
                      JsonValue::Object{{"binding", binding},
                                        {"fields", JsonValue::Object{{"title", "First client"}}},
                                        {"wait", false}});
    ASSERT_TRUE(first);
    const auto id = json_string(first.value(), "operation_id");
    auto second =
        other.tool("ravo.metadata.patch",
                   JsonValue::Object{{"binding", binding},
                                     {"fields", JsonValue::Object{{"title", "Second client"}}}});
    ASSERT_TRUE(second);
    EXPECT_FALSE(mcp_data(second.value()));
    release.set_value();
    unblock.dismiss();
    auto receipt =
        call("ravo.operations.get",
             JsonValue::Object{{"session_id", session}, {"operation_id", id}, {"wait", true}});
    ASSERT_TRUE(receipt);
    EXPECT_EQ(json_string(receipt.value(), "phase"), "committed");
    EXPECT_EQ(repository.value()->snapshot().value().revision, revision + 1);
    EXPECT_EQ(repository.value()
                  ->find_asset_by_id(json_string(binding, "asset_id"))
                  .value()
                  ->metadata.title,
              "First client");
    EXPECT_TRUE(other.close());
}

TEST_F(StudioMcpTest, CliAnnotationUsesTheSameReservedOperationAndVersionedPayload)
{
    auto current = state();
    ASSERT_TRUE(current);
    auto reserved = reserve();
    ASSERT_TRUE(reserved);
    auto body = *current.value().find("binding")->object_if();
    body["kind"] = "annotation";
    body["annotation"] =
        JsonValue::Object{{"version", JsonValue::number("1")},
                          {"kind", "metadata"},
                          {"fields", JsonValue::Object{{"headline", "CLI metadata"},
                                                       {"description", JsonValue{nullptr}}}}};
    const auto path = root.filePath("annotation.json");
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    const auto bytes = QByteArray::fromStdString(serialize_json(body));
    ASSERT_EQ(file.write(bytes), bytes.size());
    file.close();
    const QStringList arguments{
        "studio",         "commit",
        "--session-id",   QString::fromStdString(session),
        "--request",      path,
        "--operation-id", QString::fromStdString(json_string(reserved.value(), "operation_id")),
        "--wait",         "true",
        "--json"};
    auto result = run_cli_process(arguments);
    ASSERT_EQ(result.exit_code, 0) << result.standard_output.toStdString();
    auto data = cli_data(result.standard_output);
    ASSERT_TRUE(data);
    EXPECT_EQ(json_string(data.value(), "phase"), "committed");
    auto replay = run_cli_process(arguments);
    ASSERT_EQ(replay.exit_code, 0);
    EXPECT_EQ(serialize_json(cli_data(replay.standard_output).value()),
              serialize_json(data.value()));
    ASSERT_TRUE(ready());
    current = state();
    ASSERT_TRUE(current);
    EXPECT_EQ(json_string(*current.value().find("selection")->find("annotations")->find("metadata"),
                          "headline"),
              "CLI metadata");
}

TEST_F(StudioMcpTest, EofCancelsInflightAnnotationBeforeCommit)
{
    auto current = state();
    ASSERT_TRUE(current);
    auto reserved = reserve();
    ASSERT_TRUE(reserved);
    const auto id = json_string(reserved.value(), "operation_id");
    auto entered = std::make_shared<std::promise<void>>();
    std::promise<void> release;
    ASSERT_TRUE(testing::StudioAutomationTestControl::block(*presenter, entered,
                                                            release.get_future().share()));
    auto unblock = qScopeGuard([&] { release.set_value(); });
    ASSERT_EQ(entered->get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
    mcp.send(JsonValue::Object{
        {"jsonrpc", "2.0"},
        {"id", JsonValue::number("903")},
        {"method", "tools/call"},
        {"params",
         JsonValue::Object{
             {"name", "ravo.review.apply"},
             {"arguments",
              JsonValue::Object{{"binding", *current.value().find("binding")},
                                {"fields", JsonValue::Object{{"rating", JsonValue::number("5")}}},
                                {"operation_id", id}}},
             {"_meta", JsonValue::Object{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                         {"io.modelcontextprotocol/clientCapabilities",
                                          JsonValue::Object{}}}}}}});
    ASSERT_TRUE(wait_until([&] { return presenter->busy(); }));
    EXPECT_TRUE(mcp.close());
    release.set_value();
    unblock.dismiss();
    ASSERT_TRUE(ready());
    ASSERT_TRUE(mcp.start());
    auto receipt =
        call("ravo.operations.get",
             JsonValue::Object{{"session_id", session}, {"operation_id", id}, {"wait", true}});
    ASSERT_TRUE(receipt);
    EXPECT_EQ(json_string(receipt.value(), "phase"), "cancelled");
    auto readback = state();
    ASSERT_TRUE(readback);
    EXPECT_EQ(readback.value()
                  .find("selection")
                  ->find("annotations")
                  ->find("review")
                  ->find("rating")
                  ->number_if()
                  ->text,
              "0");
}

TEST_F(StudioMcpTest, CloseJoinsQueuedAnnotationWithoutWritingCatalog)
{
    auto current = state();
    ASSERT_TRUE(current);
    auto entered = std::make_shared<std::promise<void>>();
    std::promise<void> release;
    ASSERT_TRUE(testing::StudioAutomationTestControl::block(*presenter, entered,
                                                            release.get_future().share()));
    bool released = false;
    auto unblock = qScopeGuard(
        [&]
        {
            if (!released)
                release.set_value();
        });
    ASSERT_EQ(entered->get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
    auto pending = call("ravo.keywords.replace",
                        JsonValue::Object{{"binding", *current.value().find("binding")},
                                          {"keywords", JsonValue::Array{"Must not write"}},
                                          {"wait", false}});
    ASSERT_TRUE(pending);
    QObject::connect(presenter.get(), &StudioPresenter::aboutToShutdown, presenter.get(),
                     [&]
                     {
                         released = true;
                         release.set_value();
                     });
    presenter.reset();
    live.reset();
    commands.reset();
    auto repository = SqliteCatalogRepository::open(root.filePath("library.sqlite").toStdString());
    ASSERT_TRUE(repository);
    EXPECT_TRUE(repository.value()->find_asset_by_id("ast_page_0").value()->tags.empty());
    EXPECT_TRUE(mcp.request("ping"));
}

TEST_F(StudioMcpTest, EvaluateIsNeutralAndCommitReplayIsExactlyOnce)
{
    auto current = state();
    ASSERT_TRUE(current) << current.error().context.at("response");
    const auto binding = *current.value().find("binding");
    const auto asset = json_string(binding, "asset_id");
    auto repository = SqliteCatalogRepository::open(root.filePath("library.sqlite").toStdString());
    ASSERT_TRUE(repository);
    auto before = repository.value()->snapshot();
    ASSERT_TRUE(before);
    auto recipe = repository.value()->load_recipe_json(asset);
    ASSERT_TRUE(recipe);
    auto history = repository.value()->list_recipe_history(asset);
    ASSERT_TRUE(history);
    auto previews = repository.value()->list_previews();
    ASSERT_TRUE(previews);
    QFile source(root.filePath("photo-0.png"));
    ASSERT_TRUE(source.open(QIODevice::ReadOnly));
    const auto original = source.readAll();
    source.close();
    auto trial = evaluate(binding, 0.375);
    ASSERT_TRUE(trial) << trial.error().context.at("response");
    EXPECT_TRUE(*trial.value().find("recipe_unchanged")->boolean_if());
    EXPECT_TRUE(*trial.value().find("preview_records_unchanged")->boolean_if());
    auto after = repository.value()->snapshot();
    ASSERT_TRUE(after);
    EXPECT_EQ(before.value().revision, after.value().revision);
    auto after_recipe = repository.value()->load_recipe_json(asset);
    ASSERT_TRUE(after_recipe);
    EXPECT_EQ(recipe.value(), after_recipe.value());
    auto after_history = repository.value()->list_recipe_history(asset);
    ASSERT_TRUE(after_history);
    EXPECT_EQ(history.value().size(), after_history.value().size());
    auto after_previews = repository.value()->list_previews();
    ASSERT_TRUE(after_previews);
    EXPECT_EQ(previews.value(), after_previews.value());
    ASSERT_TRUE(source.open(QIODevice::ReadOnly));
    EXPECT_EQ(original, source.readAll());
    const auto &artifact = *trial.value().find("artifact");
    EXPECT_EQ(artifact.find("path"), nullptr);
    const auto uri = json_string(artifact, "resource_uri");
    ASSERT_FALSE(uri.empty());
    auto pixels = mcp.request("resources/read", JsonValue::Object{{"uri", uri}});
    ASSERT_TRUE(pixels);
    const auto &image = pixels.value().find("result")->find("contents")->array_if()->front();
    const auto bytes =
        QByteArray::fromBase64(QByteArray::fromStdString(json_string(image, "blob")));
    EXPECT_EQ(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString(),
              json_string(artifact, "content_sha256"));
    auto reserved = reserve();
    ASSERT_TRUE(reserved);
    const auto id = json_string(reserved.value(), "operation_id");
    const JsonValue commit = JsonValue::Object{{"session_id", session},
                                               {"operation_id", id},
                                               {"candidate", *trial.value().find("candidate")}};
    auto saved = call("ravo.develop.commit", commit);
    ASSERT_TRUE(saved) << saved.error().context.at("response");
    EXPECT_EQ(json_string(saved.value(), "phase"), "committed");
    ASSERT_TRUE(ready());
    auto persisted = testing::StudioAutomationTestControl::saved(*presenter);
    ASSERT_TRUE(persisted);
    auto params = develop_from_recipe(persisted.value());
    ASSERT_TRUE(params);
    EXPECT_DOUBLE_EQ(params.value().exposure_ev, 0.375);
    auto saved_history = repository.value()->list_recipe_history(asset);
    ASSERT_TRUE(saved_history);
    auto replay = call("ravo.develop.commit", commit);
    ASSERT_TRUE(replay) << replay.error().context.at("response");
    EXPECT_EQ(serialize_json(replay.value()), serialize_json(saved.value()));
    auto replay_history = repository.value()->list_recipe_history(asset);
    ASSERT_TRUE(replay_history);
    EXPECT_EQ(saved_history.value().size(), replay_history.value().size());
    auto cancelled = call("ravo.operations.cancel",
                          JsonValue::Object{{"session_id", session}, {"operation_id", id}});
    ASSERT_TRUE(cancelled);
    EXPECT_EQ(json_string(cancelled.value(), "phase"), "committed");
    auto stale_reservation = reserve();
    ASSERT_TRUE(stale_reservation);
    auto stale_args = *commit.object_if();
    stale_args["operation_id"] = *stale_reservation.value().find("operation_id");
    auto stale = mcp.tool("ravo.develop.commit", JsonValue{stale_args});
    ASSERT_TRUE(stale);
    EXPECT_TRUE(*stale.value().find("result")->find("isError")->boolean_if());
    presenter->selectAsset("ast_page_1");
    ASSERT_TRUE(ready());
    replay = call("ravo.develop.commit", commit);
    ASSERT_TRUE(replay);
    EXPECT_EQ(serialize_json(replay.value()), serialize_json(saved.value()));
    auto other = testing::StudioAutomationTestControl::saved(*presenter);
    ASSERT_TRUE(other);
    params = develop_from_recipe(other.value());
    ASSERT_TRUE(params);
    EXPECT_DOUBLE_EQ(params.value().exposure_ev, 0);
}

TEST_F(StudioMcpTest, CliEvaluationRejectsOutputConflictAndWrongBindingWithoutMutation)
{
    auto current = state();
    ASSERT_TRUE(current);
    auto request = *current.value().find("binding")->object_if();
    request.emplace("fields", exposure_patch(0.5));
    const auto request_path = root.filePath("request.json");
    QFile file(request_path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    const auto bytes = QByteArray::fromStdString(serialize_json(JsonValue{request}));
    ASSERT_EQ(file.write(bytes), bytes.size());
    file.close();
    const auto output = root.filePath("trial.png");
    const QStringList args{
        "studio",     "evaluate",   "--session-id", QString::fromStdString(session),
        "--request",  request_path, "--output",     output,
        "--max-edge", "32",         "--json"};
    auto first = run_cli_process(args);
    ASSERT_EQ(first.exit_code, 0) << first.standard_output.toStdString();
    QFile image(output);
    ASSERT_TRUE(image.open(QIODevice::ReadOnly));
    const auto original = image.readAll();
    image.close();
    auto conflict = run_cli_process(args);
    EXPECT_NE(conflict.exit_code, 0);
    ASSERT_TRUE(image.open(QIODevice::ReadOnly));
    EXPECT_EQ(original, image.readAll());
    image.close();
    request["asset_id"] = "ast_page_1";
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QByteArray::fromStdString(serialize_json(JsonValue{request})));
    file.close();
    auto wrong_args = args;
    wrong_args[wrong_args.indexOf("--output") + 1] = root.filePath("wrong.png");
    auto wrong = run_cli_process(wrong_args);
    EXPECT_NE(wrong.exit_code, 0);
    EXPECT_FALSE(QFile::exists(root.filePath("wrong.png")));
    auto persisted = testing::StudioAutomationTestControl::saved(*presenter);
    ASSERT_TRUE(persisted);
    auto params = develop_from_recipe(persisted.value());
    ASSERT_TRUE(params);
    EXPECT_DOUBLE_EQ(params.value().exposure_ev, 0);
}

TEST_F(StudioMcpTest, CancelledRequestDoesNotWriteOrEmitAFinalResponse)
{
    auto current = state();
    ASSERT_TRUE(current);
    auto trial = evaluate(*current.value().find("binding"), 0.75);
    ASSERT_TRUE(trial);
    auto reserved = reserve();
    ASSERT_TRUE(reserved);
    const auto id = json_string(reserved.value(), "operation_id");
    auto entered = std::make_shared<std::promise<void>>();
    std::promise<void> release;
    ASSERT_TRUE(testing::StudioAutomationTestControl::block(*presenter, entered,
                                                            release.get_future().share()));
    auto unblock = qScopeGuard([&] { release.set_value(); });
    ASSERT_EQ(entered->get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
    mcp.send(JsonValue::Object{
        {"jsonrpc", "2.0"},
        {"id", JsonValue::number("900")},
        {"method", "tools/call"},
        {"params",
         JsonValue::Object{
             {"name", "ravo.develop.commit"},
             {"arguments", JsonValue::Object{{"session_id", session},
                                             {"operation_id", id},
                                             {"candidate", *trial.value().find("candidate")}}},
             {"_meta", JsonValue::Object{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                         {"io.modelcontextprotocol/clientCapabilities",
                                          JsonValue::Object{}}}}}}});
    ASSERT_TRUE(wait_until([&] { return presenter->busy(); }));
    mcp.send(
        JsonValue::Object{{"jsonrpc", "2.0"},
                          {"method", "notifications/cancelled"},
                          {"params", JsonValue::Object{{"requestId", JsonValue::number("900")}}}});
    bool cancellation_requested = false;
    ASSERT_TRUE(wait_until(
        [&]
        {
            auto receipt = call("ravo.operations.get",
                                JsonValue::Object{{"session_id", session}, {"operation_id", id}});
            if (!receipt)
                return false;
            const auto *flag = receipt.value().find("cancel_requested");
            cancellation_requested = flag && flag->boolean_if() && *flag->boolean_if();
            return cancellation_requested;
        },
        5000));
    unblock.dismiss();
    release.set_value();
    ASSERT_TRUE(ready());
    auto receipt =
        call("ravo.operations.get",
             JsonValue::Object{{"session_id", session}, {"operation_id", id}, {"wait", true}});
    ASSERT_TRUE(receipt);
    EXPECT_EQ(json_string(receipt.value(), "phase"), "cancelled");
    EXPECT_FALSE(*receipt.value().find("mutation_applied")->boolean_if());
    auto persisted = testing::StudioAutomationTestControl::saved(*presenter);
    ASSERT_TRUE(persisted);
    auto params = develop_from_recipe(persisted.value());
    ASSERT_TRUE(params);
    EXPECT_DOUBLE_EQ(params.value().exposure_ev, 0);
    EXPECT_FALSE(mcp.received(900));
    auto cancelled_reservation = reserve();
    ASSERT_TRUE(cancelled_reservation);
    const auto cancelled_id = json_string(cancelled_reservation.value(), "operation_id");
    auto cancel_reserved =
        call("ravo.operations.cancel",
             JsonValue::Object{{"session_id", session}, {"operation_id", cancelled_id}});
    ASSERT_TRUE(cancel_reserved);
    auto rejected = mcp.tool("ravo.develop.commit",
                             JsonValue::Object{{"session_id", session},
                                               {"operation_id", cancelled_id},
                                               {"candidate", *trial.value().find("candidate")}});
    ASSERT_TRUE(rejected);
    const auto *error = rejected.value().find("result")->find("structuredContent")->find("error");
    ASSERT_NE(error, nullptr);
    ASSERT_NE(error->find("context"), nullptr);
    EXPECT_EQ(json_string(*error->find("context"), "mutation_applied"), "false");
    auto ping = mcp.request("ping");
    ASSERT_TRUE(ping);
    EXPECT_NE(ping.value().find("result"), nullptr);
}

TEST_F(StudioMcpTest, LocalMaskTrialGestureAndHistoryRestoreUseSharedOwners)
{
    auto unsupported_kind = mask("create", JsonValue::Object{{"kind", JsonValue::number("1")}});
    EXPECT_FALSE(unsupported_kind);
    auto created = mask("create", JsonValue::Object{{"kind", JsonValue::number("5")}});
    ASSERT_TRUE(created) << created.error().context.at("response");
    EXPECT_EQ(json_string(created.value(), "phase"), "committed");
    ASSERT_TRUE(ready());
    auto current = state();
    ASSERT_TRUE(current);
    const auto local = json_string(*current.value().find("editing_scope"), "local_id");
    ASSERT_FALSE(local.empty());
    auto trial = evaluate(*current.value().find("binding"), 0.625, local);
    ASSERT_TRUE(trial) << trial.error().context.at("response");
    auto committed = call("ravo.develop.commit",
                          JsonValue::Object{{"session_id", session},
                                            {"candidate", *trial.value().find("candidate")}});
    ASSERT_TRUE(committed) << committed.error().context.at("response");
    ASSERT_TRUE(ready());
    auto saved = testing::StudioAutomationTestControl::saved(*presenter);
    ASSERT_TRUE(saved);
    auto params = develop_from_recipe(saved.value());
    ASSERT_TRUE(params);
    EXPECT_DOUBLE_EQ(params.value().exposure_ev, 0);
    auto scoped = local_adjustment_develop(params.value(), local);
    ASSERT_TRUE(scoped);
    EXPECT_DOUBLE_EQ(scoped.value().exposure_ev, 0.625);
    auto done = mask("done", JsonValue::Object{});
    ASSERT_TRUE(done);
    ASSERT_TRUE(ready());
    created = mask("create", JsonValue::Object{{"kind", JsonValue::number("3")}});
    ASSERT_TRUE(created);
    EXPECT_EQ(json_string(created.value(), "phase"), "staged");
    ASSERT_TRUE(ready());
    current = state();
    ASSERT_TRUE(current);
    const auto circle = json_string(*current.value().find("editing_scope"), "local_id");
    const auto asset = json_string(*current.value().find("binding"), "asset_id");
    auto begun = mask("gesture_begin", JsonValue::Object{{"id", circle},
                                                         {"asset", asset},
                                                         {"handle", "draw"},
                                                         {"x", JsonValue::number("0.2")},
                                                         {"y", JsonValue::number("0.2")}});
    ASSERT_TRUE(begun) << begun.error().context.at("response");
    ASSERT_TRUE(ready());
    current = state();
    ASSERT_TRUE(current);
    const auto token = json_string(*current.value().find("editing_scope"), "gesture_token");
    ASSERT_FALSE(token.empty());
    auto ended = mask("gesture_end", JsonValue::Object{{"id", circle},
                                                       {"asset", asset},
                                                       {"token", token},
                                                       {"x", JsonValue::number("0.8")},
                                                       {"y", JsonValue::number("0.8")}});
    ASSERT_TRUE(ended) << ended.error().context.at("response");
    EXPECT_EQ(json_string(ended.value(), "phase"), "committed");
    ASSERT_TRUE(ready());
    current = state();
    ASSERT_TRUE(current);
    EXPECT_FALSE(current.value().find("editing_scope")->find("handles")->array_if()->empty());
    auto history =
        call("ravo.history.list", JsonValue::Object{{"binding", *current.value().find("binding")}});
    ASSERT_TRUE(history) << history.error().context.at("response");
    EXPECT_FALSE(history.value().find("entries")->array_if()->empty());
    done = mask("done", JsonValue::Object{});
    ASSERT_TRUE(done);
    ASSERT_TRUE(ready());
    current = state();
    ASSERT_TRUE(current);
    auto restored = call("ravo.history.restore",
                         JsonValue::Object{{"binding", *current.value().find("binding")},
                                           {"history_id", JsonValue::number("0")}});
    ASSERT_TRUE(restored) << restored.error().context.at("response");
    EXPECT_EQ(json_string(restored.value(), "phase"), "committed");
    ASSERT_TRUE(ready());
    saved = testing::StudioAutomationTestControl::saved(*presenter);
    ASSERT_TRUE(saved);
    params = develop_from_recipe(saved.value());
    ASSERT_TRUE(params);
    EXPECT_TRUE(params.value().local_adjustments.empty());
}

TEST_F(StudioMcpTest, BatchCommitsOnlyExplicitSelectedPhotosWithIndependentReceipts)
{
    presenter->selectAllVisible();
    ASSERT_TRUE(ready());
    auto current = state();
    ASSERT_TRUE(current);
    auto result =
        call("ravo.develop.batch",
             JsonValue::Object{{"binding", *current.value().find("binding")},
                               {"asset_ids", JsonValue::Array{"ast_page_0", "ast_page_1"}},
                               {"fields", exposure_patch(0.25)}});
    ASSERT_TRUE(result) << result.error().context.at("response");
    EXPECT_EQ(json_string(result.value(), "phase"), "committed");
    const auto *items = result.value().find("result")->find("items")->array_if();
    ASSERT_EQ(items->size(), 2U);
    for (const auto &item : *items)
    {
        EXPECT_EQ(json_string(item, "status"), "applied");
        EXPECT_NE(item.find("history_id"), nullptr);
        EXPECT_NE(item.find("catalog_revision"), nullptr);
    }
    ASSERT_TRUE(ready());
    auto repository = SqliteCatalogRepository::open(root.filePath("library.sqlite").toStdString());
    ASSERT_TRUE(repository);
    auto untouched = repository.value()->load_recipe_json("ast_page_2");
    ASSERT_TRUE(untouched);
    EXPECT_FALSE(untouched.value().has_value());
    auto history = repository.value()->list_recipe_history("ast_page_0");
    ASSERT_TRUE(history);
    EXPECT_FALSE(history.value().empty());
}

TEST_F(StudioMcpTest, EofCancelsInflightWriteAndSessionDestructionJoinsWork)
{
    auto current = state();
    ASSERT_TRUE(current);
    auto trial = evaluate(*current.value().find("binding"), 1.25);
    ASSERT_TRUE(trial);
    auto reserved = reserve();
    ASSERT_TRUE(reserved);
    const auto id = json_string(reserved.value(), "operation_id");
    auto entered = std::make_shared<std::promise<void>>();
    std::promise<void> release;
    ASSERT_TRUE(testing::StudioAutomationTestControl::block(*presenter, entered,
                                                            release.get_future().share()));
    auto unblock = qScopeGuard([&] { release.set_value(); });
    ASSERT_EQ(entered->get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
    mcp.send(JsonValue::Object{
        {"jsonrpc", "2.0"},
        {"id", JsonValue::number("901")},
        {"method", "tools/call"},
        {"params",
         JsonValue::Object{
             {"name", "ravo.develop.commit"},
             {"arguments", JsonValue::Object{{"session_id", session},
                                             {"operation_id", id},
                                             {"candidate", *trial.value().find("candidate")}}},
             {"_meta", JsonValue::Object{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                         {"io.modelcontextprotocol/clientCapabilities",
                                          JsonValue::Object{}}}}}}});
    ASSERT_TRUE(wait_until([&] { return presenter->busy(); }));
    EXPECT_TRUE(mcp.close());
    unblock.dismiss();
    release.set_value();
    ASSERT_TRUE(ready());
    auto persisted = testing::StudioAutomationTestControl::saved(*presenter);
    ASSERT_TRUE(persisted);
    auto params = develop_from_recipe(persisted.value());
    ASSERT_TRUE(params);
    EXPECT_DOUBLE_EQ(params.value().exposure_ev, 0);
    live.reset();
    commands.reset();
    presenter.reset();
}
TEST_F(StudioMcpTest, StudioCloseCancelsQueuedBatchBeforeDestroyingBorrowedOwners)
{
    presenter->selectAllVisible();
    ASSERT_TRUE(ready());
    auto current = state();
    ASSERT_TRUE(current);
    auto entered = std::make_shared<std::promise<void>>();
    std::promise<void> release;
    ASSERT_TRUE(testing::StudioAutomationTestControl::block(*presenter, entered,
                                                            release.get_future().share()));
    bool released = false;
    auto unblock = qScopeGuard(
        [&]
        {
            if (!released)
                release.set_value();
        });
    ASSERT_EQ(entered->get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
    auto result =
        call("ravo.develop.batch",
             JsonValue::Object{{"binding", *current.value().find("binding")},
                               {"asset_ids", JsonValue::Array{"ast_page_0", "ast_page_1"}},
                               {"fields", exposure_patch(0.5)},
                               {"wait", false}});
    ASSERT_TRUE(result) << result.error().context.at("response");
    EXPECT_EQ(json_string(result.value(), "phase"), "pending");
    QObject::connect(presenter.get(), &StudioPresenter::aboutToShutdown, presenter.get(),
                     [&]
                     {
                         released = true;
                         release.set_value();
                     });
    presenter.reset();
    auto ping = mcp.request("ping");
    ASSERT_TRUE(ping);
    EXPECT_NE(ping.value().find("result"), nullptr);
    live.reset();
    commands.reset();
    auto repository = SqliteCatalogRepository::open(root.filePath("library.sqlite").toStdString());
    ASSERT_TRUE(repository);
    for (const auto *id : {"ast_page_0", "ast_page_1"})
    {
        auto stored = repository.value()->load_recipe_json(id);
        ASSERT_TRUE(stored);
        EXPECT_FALSE(stored.value());
        auto history = repository.value()->list_recipe_history(id);
        ASSERT_TRUE(history);
        EXPECT_TRUE(history.value().empty());
    }
}
} // namespace ravo
