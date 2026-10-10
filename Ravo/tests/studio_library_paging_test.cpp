#include <QColorSpace>
#include <QImage>
#include <QTemporaryDir>
#include <QFile>
#include <QCoreApplication>
#include <QEvent>
#include <QScopeGuard>
#include <future>
#include <set>
#include <gtest/gtest.h>

#include "studio_test_support.h"
#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/desktop/studio_presenter.h"
#include "ravo/desktop/studio_command_controller.h"
#include "ravo/desktop/asset_list_model.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/log.h"

namespace ravo
{
namespace testing
{
class StudioLibraryTestControl
{
public:
    static std::vector<std::string> selection(const StudioPresenter &presenter)
    {
        return presenter.selected_asset_ids();
    }
    static void drain(StudioPresenter &presenter)
    {
        presenter.executor_.wait_idle();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    }
    static void waitWorker(StudioPresenter &presenter)
    {
        presenter.executor_.wait_idle();
    }
    static bool changeRevision(StudioPresenter &presenter)
    {
        return presenter.executor_.submit(
            [&] { return bool(presenter.service_->library().set_rating("ast_page_0", 5)); });
    }
    static bool block(StudioPresenter &presenter, std::shared_ptr<std::promise<void>> entered,
                      std::shared_future<void> release)
    {
        return presenter.executor_.post(
            [entered, release]
            {
                entered->set_value();
                release.wait();
            });
    }
};
} // namespace testing
namespace
{
using namespace studio_test_support;
// Opening 205 on-disk fixtures competes with parallel CI processes on Windows.
// This is setup, not the paging latency contract: selection waits below retain
// their ordinary timeout and all unloaded-row/selection assertions stay intact.
constexpr int kCatalogOpenTimeoutMs = 60000;
void make_paged_catalog(const QTemporaryDir &root, const int count = 205,
                        const bool include_unloaded_video = false)
{
    auto repository =
        SqliteCatalogRepository::create(root.filePath("library.sqlite").toStdString());
    ASSERT_TRUE(repository);
    QImage image(16, 12, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    for (int i = 0; i < count; ++i)
    {
        const auto file = root.filePath(QString("photo-%1.png").arg(i));
        image.fill(QColor(i % 256, i / 256, 160));
        ASSERT_TRUE(image.save(file, "PNG"));
        auto location = normalize_local_input(file.toStdString());
        ASSERT_TRUE(location);
        auto identity = read_file_identity(location.value().path);
        ASSERT_TRUE(identity);
        AssetRecord asset;
        asset.id = "ast_page_" + std::to_string(i);
        asset.normalized_uri = location.value().uri;
        asset.media_type = "image/png";
        if (include_unloaded_video && i == 500)
        {
            asset.media_type = "video/mp4";
            VideoInfo video;
            video.container = "mp4";
            video.codec = "h264";
            video.width = 16;
            video.height = 12;
            asset.video = video;
        }
        asset.width = 16;
        asset.height = 12;
        asset.size_bytes = identity.value().size_bytes;
        asset.mtime_unix_ms = identity.value().mtime_unix_ms;
        asset.content_fingerprint = make_content_fingerprint(identity.value());
        asset.created_unix_ms = 1000 - i;
        ASSERT_TRUE(repository.value()->commit_imported_asset(asset));
    }
}
} // namespace
TEST(StudioLibraryPaging, NextAcrossUnloadedPagePreservesSelectionUntilResolved)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    make_paged_catalog(root);
    StudioPresenter presenter;
    presenter.openCatalogFromPath(root.filePath("library.sqlite"));
    ASSERT_TRUE(wait_until(
        [&]
        { return presenter.catalogOpen() && !presenter.busy() && presenter.visibleCount() == 205; },
        kCatalogOpenTimeoutMs))
        << "open=" << presenter.catalogOpen() << " busy=" << presenter.busy()
        << " visible=" << presenter.visibleCount()
        << " error=" << presenter.errorText().toStdString();
    ASSERT_FALSE(presenter.assets()->rowLoaded(200));
    presenter.selectAsset(presenter.assets()->assetIdAt(199));
    const auto previous = presenter.selectedAssetId();
    ASSERT_EQ(previous, "ast_page_199");
    presenter.selectNext();
    EXPECT_EQ(presenter.selectedAssetId(), previous);
    ASSERT_TRUE(
        wait_until([&] { return presenter.selectedAssetId() == QStringLiteral("ast_page_200"); }));
    EXPECT_TRUE(presenter.errorText().isEmpty()) << presenter.errorText().toStdString();
    presenter.selectPrevious();
    EXPECT_EQ(presenter.selectedAssetId(), previous);
}
TEST(StudioLibraryPaging, PlaceholderClickAndRepeatedNextUseLatestBoundRow)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    make_paged_catalog(root);
    StudioPresenter presenter;
    StudioCommandController commands(presenter);
    presenter.openCatalogFromPath(root.filePath("library.sqlite"));
    ASSERT_TRUE(wait_until(
        [&]
        { return presenter.catalogOpen() && !presenter.busy() && presenter.visibleCount() == 205; },
        kCatalogOpenTimeoutMs))
        << "open=" << presenter.catalogOpen() << " busy=" << presenter.busy()
        << " visible=" << presenter.visibleCount()
        << " error=" << presenter.errorText().toStdString();
    presenter.selectAsset(presenter.assets()->assetIdAt(199));
    presenter.selectNext();
    presenter.selectNext();
    ASSERT_TRUE(
        wait_until([&] { return presenter.selectedAssetId() == QStringLiteral("ast_page_201"); }));
    EXPECT_TRUE(presenter.errorText().isEmpty());
    auto rejected = commands.executeCommand("studio.photo.select", QString{});
    EXPECT_FALSE(rejected.value("accepted").toBool());
    EXPECT_FALSE(presenter.errorText().isEmpty());
    auto selected = commands.executeCommand("studio.photo.select",
                                            QVariantMap{{"row", 204}, {"openLoupe", true}});
    EXPECT_TRUE(selected.value("accepted").toBool());
    EXPECT_EQ(presenter.selectedAssetId(), "ast_page_204");
    EXPECT_TRUE(presenter.errorText().isEmpty());
    EXPECT_EQ(presenter.browseMode(), "loupe");
}
TEST(StudioLibraryPaging, PlaceholderCommandWaitsForMetadataBeforeSelecting)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    make_paged_catalog(root);
    StudioPresenter presenter;
    StudioCommandController commands(presenter);
    presenter.openCatalogFromPath(root.filePath("library.sqlite"));
    ASSERT_TRUE(wait_until(
        [&]
        { return presenter.catalogOpen() && !presenter.busy() && presenter.visibleCount() == 205; },
        kCatalogOpenTimeoutMs))
        << "open=" << presenter.catalogOpen() << " busy=" << presenter.busy()
        << " visible=" << presenter.visibleCount()
        << " error=" << presenter.errorText().toStdString();
    ASSERT_FALSE(presenter.assets()->rowLoaded(204));
    const auto previous = presenter.selectedAssetId();
    const auto result = commands.executeCommand("studio.photo.select",
                                                QVariantMap{{"row", 204}, {"openLoupe", true}});
    EXPECT_TRUE(result.value("accepted").toBool());
    EXPECT_EQ(presenter.selectedAssetId(), previous);
    ASSERT_TRUE(
        wait_until([&] { return presenter.selectedAssetId() == QStringLiteral("ast_page_204"); }));
    EXPECT_TRUE(presenter.errorText().isEmpty());
    EXPECT_EQ(presenter.browseMode(), "loupe");
}

TEST(StudioLibraryPaging, NewSelectionAndQueryInvalidateQueuedRowSelection)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    make_paged_catalog(root);
    StudioPresenter presenter;
    presenter.openCatalogFromPath(root.filePath("library.sqlite"));
    ASSERT_TRUE(wait_until(
        [&]
        { return presenter.catalogOpen() && !presenter.busy() && presenter.visibleCount() == 205; },
        kCatalogOpenTimeoutMs))
        << "open=" << presenter.catalogOpen() << " busy=" << presenter.busy()
        << " visible=" << presenter.visibleCount()
        << " error=" << presenter.errorText().toStdString();
    presenter.selectLibraryRow(204);
    presenter.selectAsset(presenter.assets()->assetIdAt(0));
    ASSERT_TRUE(wait_until([&] { return presenter.assets()->rowLoaded(204); }));
    EXPECT_EQ(presenter.selectedAssetId(), "ast_page_0");
    presenter.library()->setSort("name", "asc");
    ASSERT_TRUE(wait_until([&] { return !presenter.assets()->rowLoaded(204); }));
    presenter.selectLibraryRow(204);
    presenter.library()->setFilterText("photo-0");
    ASSERT_TRUE(wait_until([&] { return presenter.visibleCount() == 1; }));
    EXPECT_EQ(presenter.selectedAssetId(), "ast_page_0");
}
TEST(StudioLibraryPaging, RangeAndAllSelectionResolveUnloadedRowsAndBatchUsesSnapshot)
{
    ensure_qt_core();
    QTemporaryDir root;
    make_paged_catalog(root, 1000);
    StudioPresenter presenter;
    presenter.openCatalogFromPath(root.filePath("library.sqlite"));
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.catalogOpen() && !presenter.busy() && presenter.visibleCount() == 1000;
        },
        kCatalogOpenTimeoutMs));
    presenter.selectAsset("ast_page_50");
    presenter.ensureLibraryRow(850);
    ASSERT_TRUE(wait_until([&] { return presenter.assets()->rowLoaded(850); }));
    ASSERT_FALSE(presenter.assets()->rowLoaded(400));
    presenter.selectAssetRange("ast_page_850");
    ASSERT_TRUE(wait_until([&] { return !presenter.busy() && presenter.selectedCount() == 801; }));
    auto ids = testing::StudioLibraryTestControl::selection(presenter);
    ASSERT_EQ(ids.size(), 801U);
    EXPECT_EQ(std::set<std::string>(ids.begin(), ids.end()).size(), 801U);
    for (int row = 50; row <= 850; ++row)
        EXPECT_EQ(ids[static_cast<std::size_t>(row - 50)], "ast_page_" + std::to_string(row));
    presenter.setAssetTags("complete-selection");
    testing::StudioLibraryTestControl::drain(presenter);
    auto repository = SqliteCatalogRepository::open(root.filePath("library.sqlite").toStdString());
    ASSERT_TRUE(repository);
    auto tagged = repository.value()->list_assets();
    ASSERT_TRUE(tagged);
    std::size_t tagged_count = 0;
    for (const auto &asset : tagged.value())
        if (std::find(asset.tags.begin(), asset.tags.end(), "complete-selection") !=
            asset.tags.end())
            ++tagged_count;
    EXPECT_EQ(tagged_count, 801U);
    presenter.ensureLibraryRow(400);
    ASSERT_TRUE(wait_until([&] { return presenter.assets()->rowLoaded(400); }));
    presenter.ensureLibraryRow(600);
    ASSERT_TRUE(wait_until([&] { return presenter.assets()->rowLoaded(600); }));
    EXPECT_EQ(testing::StudioLibraryTestControl::selection(presenter), ids);
    EXPECT_LE(presenter.assets()->loadedCount(), 600);
    presenter.selectAllVisible();
    ASSERT_TRUE(wait_until([&] { return !presenter.busy() && presenter.selectedCount() == 1000; }));
    EXPECT_EQ(testing::StudioLibraryTestControl::selection(presenter).size(), 1000U);
}

TEST(StudioLibraryPaging, SelectionResolutionRejectsChangedQueryAndNewSelection)
{
    ensure_qt_core();
    QTemporaryDir root;
    make_paged_catalog(root);
    StudioPresenter presenter;
    presenter.openCatalogFromPath(root.filePath("library.sqlite"));
    ASSERT_TRUE(wait_until(
        [&]
        { return presenter.catalogOpen() && !presenter.busy() && presenter.visibleCount() == 205; },
        kCatalogOpenTimeoutMs));
    auto entered = std::make_shared<std::promise<void>>();
    std::promise<void> release;
    ASSERT_TRUE(
        testing::StudioLibraryTestControl::block(presenter, entered, release.get_future().share()));
    auto unblock = qScopeGuard([&] { release.set_value(); });
    ASSERT_EQ(entered->get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
    presenter.selectAllVisible();
    EXPECT_TRUE(presenter.busy());
    StudioCommandController commands(presenter);
    EXPECT_FALSE(commands.executeCommand("studio.photo.set_rating", 5).value("accepted").toBool());
    presenter.library()->setFilterText("photo-0");
    presenter.selectAsset("ast_page_0");
    // Release before draining without a second set_value in the scope guard.
    unblock.dismiss();
    release.set_value();
    ASSERT_TRUE(wait_until([&] { return presenter.visibleCount() == 1 && !presenter.busy(); }));
    testing::StudioLibraryTestControl::drain(presenter);
    EXPECT_EQ(presenter.selectedCount(), 1);
    EXPECT_EQ(testing::StudioLibraryTestControl::selection(presenter),
              std::vector<std::string>{"ast_page_0"});
    presenter.clearFilters();
    ASSERT_TRUE(wait_until([&] { return presenter.visibleCount() == 205; }));
    presenter.selectAllVisible();
    testing::StudioLibraryTestControl::waitWorker(presenter);
    presenter.library()->setFilterText("photo-0");
    presenter.selectAsset("ast_page_0");
    ASSERT_TRUE(wait_until([&] { return presenter.visibleCount() == 1 && !presenter.busy(); }));
    testing::StudioLibraryTestControl::drain(presenter);
    EXPECT_EQ(presenter.selectedCount(), 1);
}

TEST(StudioLibraryPaging, SelectionResolutionRejectsChangedCatalogRevision)
{
    ensure_qt_core();
    QTemporaryDir root;
    make_paged_catalog(root);
    StudioPresenter presenter;
    presenter.openCatalogFromPath(root.filePath("library.sqlite"));
    ASSERT_TRUE(wait_until(
        [&]
        { return presenter.catalogOpen() && !presenter.busy() && presenter.visibleCount() == 205; },
        kCatalogOpenTimeoutMs));
    presenter.selectAsset("ast_page_0");
    ASSERT_TRUE(testing::StudioLibraryTestControl::changeRevision(presenter));
    presenter.selectAllVisible();
    ASSERT_TRUE(wait_until([&] { return !presenter.busy(); }));
    EXPECT_EQ(presenter.selectedCount(), 1);
    EXPECT_TRUE(presenter.errorText().contains("changed during selection"))
        << presenter.errorText().toStdString();
    EXPECT_EQ(testing::StudioLibraryTestControl::selection(presenter),
              std::vector<std::string>{"ast_page_0"});
}

TEST(StudioLibraryPaging, CullOwnerThreadFindsLatePageAndClearRejectsSupersededResult)
{
    ensure_qt_core();
    init_logging("ravo-desktop-command-tests");
    QTemporaryDir root;
    make_paged_catalog(root, 1000);
    ASSERT_TRUE(QFile::remove(root.filePath("photo-802.png")));
    ASSERT_TRUE(QFile::copy(root.filePath("photo-801.png"), root.filePath("photo-802.png")));
    StudioPresenter presenter;
    presenter.openCatalogFromPath(root.filePath("library.sqlite"));
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.catalogOpen() && !presenter.busy() && presenter.visibleCount() == 1000;
        },
        kCatalogOpenTimeoutMs));
    presenter.setCullSuggestionFilter("exact_duplicate");
    ASSERT_TRUE(wait_until([&] { return presenter.visibleCount() == 2; }));
    EXPECT_EQ(presenter.assets()->assetIdAt(0), "ast_page_801");
    EXPECT_EQ(presenter.assets()->assetIdAt(1), "ast_page_802");
    EXPECT_TRUE(presenter.errorText().isEmpty()) << presenter.errorText().toStdString();
    auto entered = std::make_shared<std::promise<void>>();
    std::promise<void> release;
    ASSERT_TRUE(
        testing::StudioLibraryTestControl::block(presenter, entered, release.get_future().share()));
    auto unblock = qScopeGuard([&] { release.set_value(); });
    ASSERT_EQ(entered->get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
    presenter.setCullSuggestionFilter("exact_duplicate");
    presenter.setCullSuggestionFilter("burst");
    unblock.dismiss();
    release.set_value();
    ASSERT_TRUE(wait_until([&] { return presenter.visibleCount() == 0; }));
    testing::StudioLibraryTestControl::drain(presenter);
    EXPECT_EQ(presenter.cullSuggestionFilter(), "burst");
    presenter.library()->setCullFlagFilter("picked");
    presenter.library()->setPickFilter("only");
    presenter.setCullSuggestionFilter("exact_duplicate");
    presenter.clearFilters();
    ASSERT_TRUE(wait_until([&] { return presenter.visibleCount() == 1000; }));
    testing::StudioLibraryTestControl::drain(presenter);
    EXPECT_EQ(presenter.cullSuggestionFilter(), "none");
    EXPECT_FALSE(presenter.filtersActive());
    // Analysis has completed and its callback is queued before the mode changes.
    presenter.setCullSuggestionFilter("exact_duplicate");
    testing::StudioLibraryTestControl::waitWorker(presenter);
    presenter.setCullSuggestionFilter("burst");
    ASSERT_TRUE(wait_until([&] { return presenter.visibleCount() == 0; }));
    testing::StudioLibraryTestControl::drain(presenter);
    EXPECT_EQ(presenter.cullSuggestionFilter(), "burst");
    presenter.setCullSuggestionFilter("exact_duplicate");
    testing::StudioLibraryTestControl::waitWorker(presenter);
    // Reopening even the same path is a distinct catalog session.
    presenter.openCatalogFromPath(root.filePath("library.sqlite"));
    ASSERT_TRUE(wait_until([&] { return presenter.visibleCount() == 1000 && !presenter.busy(); },
                           kCatalogOpenTimeoutMs));
    testing::StudioLibraryTestControl::drain(presenter);
    EXPECT_EQ(presenter.cullSuggestionFilter(), "none");
    presenter.setCullSuggestionFilter("exact_duplicate");
    ASSERT_TRUE(wait_until([&] { return presenter.visibleCount() == 2; }));
    presenter.openCatalogFromPath(root.filePath("absent.sqlite"));
    ASSERT_TRUE(wait_until([&] { return !presenter.busy() && presenter.visibleCount() == 1000; }));
    EXPECT_FALSE(presenter.errorText().isEmpty());
    EXPECT_EQ(presenter.cullSuggestionFilter(), "none");
}

TEST(StudioLibraryPaging, CompleteSelectionIncludesUnloadedVideoEligibility)
{
    ensure_qt_core();
    QTemporaryDir root;
    make_paged_catalog(root, 805, true);
    StudioPresenter presenter;
    StudioCommandController commands(presenter);
    presenter.openCatalogFromPath(root.filePath("library.sqlite"));
    ASSERT_TRUE(wait_until(
        [&]
        { return presenter.catalogOpen() && !presenter.busy() && presenter.visibleCount() == 805; },
        kCatalogOpenTimeoutMs));
    ASSERT_FALSE(presenter.assets()->rowLoaded(500));
    presenter.selectAllVisible();
    ASSERT_TRUE(wait_until([&] { return !presenter.busy() && presenter.selectedCount() == 805; }));
    EXPECT_TRUE(presenter.selectionHasVideo());
    EXPECT_FALSE(commands.action("studio.photo.merge_hdr").value("enabled").toBool());
    EXPECT_EQ(testing::StudioLibraryTestControl::selection(presenter).size(), 805U);
    EXPECT_FALSE(presenter.assets()->rowLoaded(500));
}

TEST(StudioLibraryPaging, PendingFourthPageSelectionCompletesAndKeepsEvictedIds)
{
    ensure_qt_core();
    QTemporaryDir root;
    make_paged_catalog(root, 805);
    StudioPresenter presenter;
    presenter.openCatalogFromPath(root.filePath("library.sqlite"));
    ASSERT_TRUE(wait_until(
        [&]
        { return presenter.catalogOpen() && !presenter.busy() && presenter.visibleCount() == 805; },
        kCatalogOpenTimeoutMs));
    presenter.selectAsset("ast_page_0");
    presenter.selectLibraryRow(200, "toggle");
    ASSERT_TRUE(wait_until([&] { return presenter.selectedCount() == 2; }));
    presenter.selectLibraryRow(400, "toggle");
    ASSERT_TRUE(wait_until([&] { return presenter.selectedCount() == 3; }));
    presenter.selectLibraryRow(600, "toggle");
    ASSERT_TRUE(wait_until([&] { return presenter.selectedCount() == 4; }));
    EXPECT_TRUE(presenter.assets()->rowLoaded(600));
    EXPECT_FALSE(presenter.assets()->rowLoaded(0));
    EXPECT_EQ(
        testing::StudioLibraryTestControl::selection(presenter),
        (std::vector<std::string>{"ast_page_0", "ast_page_200", "ast_page_400", "ast_page_600"}));
    EXPECT_EQ(presenter.selectedAssetId(), "ast_page_600");
    EXPECT_EQ(presenter.assets()->loadedCount(), 600);
    presenter.assets()->markOriginalMissing("ast_page_0");
    EXPECT_FALSE(presenter.canDeleteFromDisk());
    EXPECT_EQ(presenter.selectedCount(), 4);
}

TEST(StudioLibraryPaging, FourthPageSurvivesSelectedPagesAndRestoresHighlight)
{
    ensure_qt_core();
    AssetListModel model;
    const auto page = [](int first)
    {
        std::vector<AssetRecord> records(200);
        for (int row = 0; row < 200; ++row)
            records[static_cast<std::size_t>(row)].id = "ast_" + std::to_string(first + row);
        return records;
    };
    model.setAssets(page(0), {}, {}, 1000);
    model.setPage(200, page(200), {}, {}, 1000);
    model.setPage(400, page(400), {}, {}, 1000);
    model.setSelectedIds({"ast_0", "ast_200", "ast_400"});
    model.setPage(600, page(600), {}, {}, 1000);
    EXPECT_TRUE(model.rowLoaded(600));
    EXPECT_FALSE(model.rowLoaded(0));
    EXPECT_TRUE(model.isSelected("ast_0"));
    EXPECT_EQ(model.loadedCount(), 600);
    model.setPage(0, page(0), {}, {}, 1000);
    EXPECT_TRUE(model.data(model.index(0, 0), AssetListModel::SelectedRole).toBool());
    EXPECT_EQ(model.loadedCount(), 600);
}
} // namespace ravo
