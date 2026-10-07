#include <QColorSpace>
#include <QImage>
#include <QFileInfo>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include "studio_test_support.h"
#include "ravo/desktop/studio_presenter.h"
#include "ravo/desktop/studio_command_controller.h"
#include "ravo/desktop/asset_list_model.h"
#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/foundation/log.h"

namespace ravo
{
namespace
{
using namespace studio_test_support;
void import_merge_pair(StudioPresenter &presenter, const QTemporaryDir &directory)
{
    QImage image(96, 64, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    QStringList photos;
    for (int i = 0; i < 2; ++i)
    {
        const auto path = directory.filePath(QString("merge-input-%1.png").arg(i));
        photos.push_back(path);
        image.fill(QColor(80 + i * 30, 80 + i * 30, 80 + i * 30));
        ASSERT_TRUE(image.save(path, "PNG"));
    }
    presenter.createCatalogFromPath(directory.filePath("library.sqlite"));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.imports()->importFilePaths(photos);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.visibleCount() == 2 && !presenter.busy() &&
                   !presenter.imports()->importWorkActive() &&
                   !presenter.inspect()->previewLoading();
        }));
}
} // namespace
TEST(StudioPhotoMerge, SelectionBoundDialogAndSharedServicePublishDerivedAsset)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    StudioPresenter presenter;
    StudioCommandController commands(presenter);
    EXPECT_FALSE(commands.action("studio.photo.merge_hdr").value("enabled").toBool());
    import_merge_pair(presenter, root);
    EXPECT_FALSE(commands.action("studio.photo.merge_hdr").value("enabled").toBool());
    presenter.selectAllVisible();
    ASSERT_EQ(presenter.selectedCount(), 2);
    EXPECT_TRUE(commands.action("studio.photo.merge_hdr").value("enabled").toBool());
    EXPECT_TRUE(commands.action("studio.photo.panorama").value("enabled").toBool());
    QVariantMap context;
    QObject::connect(&presenter, &StudioPresenter::photoMergeDialogRequested, &presenter,
                     [&](const QVariantMap &value) { context = value; });
    presenter.preparePhotoMerge("hdr");
    ASSERT_FALSE(context.value("token").toString().isEmpty());
    EXPECT_EQ(context.value("sources").toStringList().size(), 2);
    presenter.applyPhotoMerge(
        {{"token", context.value("token")}, {"autoAlign", false}, {"exposureStops", "0, 1"}});
    EXPECT_TRUE(presenter.catalogOperationActive());
    ASSERT_TRUE(
        wait_until([&] { return !presenter.busy() && !presenter.catalogOperationActive(); }));
    ASSERT_TRUE(presenter.errorText().isEmpty()) << presenter.errorText().toStdString();
    ASSERT_TRUE(wait_until([&] { return presenter.visibleCount() == 3; }))
        << presenter.statusText().toStdString();
    EXPECT_EQ(presenter.selectedCount(), 1);
    EXPECT_TRUE(presenter.statusText().contains("TIFF"));
}
TEST(StudioPhotoMerge, StaleSelectionAndMalformedOptionsDoNotStartWork)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    StudioPresenter presenter;
    import_merge_pair(presenter, root);
    presenter.selectAllVisible();
    QVariantMap context;
    QObject::connect(&presenter, &StudioPresenter::photoMergeDialogRequested, &presenter,
                     [&](const QVariantMap &value) { context = value; });
    presenter.preparePhotoMerge("hdr");
    presenter.selectAsset(presenter.assets()->assetIdAt(0));
    presenter.applyPhotoMerge(
        {{"token", context.value("token")}, {"autoAlign", false}, {"exposureStops", "0, 1"}});
    EXPECT_FALSE(presenter.catalogOperationActive());
    EXPECT_FALSE(presenter.errorText().isEmpty());
    presenter.selectAllVisible();
    presenter.preparePhotoMerge("hdr");
    presenter.applyPhotoMerge({{"token", context.value("token")}, {"maxEdge", "-1"}});
    EXPECT_FALSE(presenter.catalogOperationActive());
    EXPECT_EQ(presenter.visibleCount(), 2);
}
TEST(StudioPhotoMerge, WindowDestructionCancelsOwnedMergeWithoutChangingOriginals)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    {
        StudioPresenter presenter;
        import_merge_pair(presenter, root);
        presenter.selectAllVisible();
        QVariantMap context;
        QObject::connect(&presenter, &StudioPresenter::photoMergeDialogRequested, &presenter,
                         [&](const QVariantMap &value) { context = value; });
        presenter.preparePhotoMerge("hdr");
        presenter.applyPhotoMerge(
            {{"token", context.value("token")}, {"autoAlign", false}, {"exposureStops", "0, 1"}});
        presenter.cancelCatalogOperation();
    }
    auto repository = SqliteCatalogRepository::open(root.filePath("library.sqlite").toStdString());
    ASSERT_TRUE(repository);
    auto assets = repository.value()->list_assets();
    ASSERT_TRUE(assets);
    EXPECT_EQ(assets.value().size(), 2);
    EXPECT_TRUE(QFileInfo::exists(root.filePath("merge-input-0.png")));
    EXPECT_TRUE(QFileInfo::exists(root.filePath("merge-input-1.png")));
}
} // namespace ravo
