#include <QColorSpace>
#include <QImage>
#include <QTemporaryDir>
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
namespace
{
using namespace studio_test_support;
// Opening 205 on-disk fixtures competes with parallel CI processes on Windows.
// This is setup, not the paging latency contract: selection waits below retain
// their ordinary timeout and all unloaded-row/selection assertions stay intact.
constexpr int kCatalogOpenTimeoutMs = 60000;
void make_paged_catalog(const QTemporaryDir &root)
{
    auto repository =
        SqliteCatalogRepository::create(root.filePath("library.sqlite").toStdString());
    ASSERT_TRUE(repository);
    QImage image(16, 12, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    for (int i = 0; i < 205; ++i)
    {
        const auto file = root.filePath(QString("photo-%1.png").arg(i));
        image.fill(QColor(40 + i % 100, 100, 160));
        ASSERT_TRUE(image.save(file, "PNG"));
        auto location = normalize_local_input(file.toStdString());
        ASSERT_TRUE(location);
        auto identity = read_file_identity(location.value().path);
        ASSERT_TRUE(identity);
        AssetRecord asset;
        asset.id = "ast_page_" + std::to_string(i);
        asset.normalized_uri = location.value().uri;
        asset.media_type = "image/png";
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
} // namespace ravo
