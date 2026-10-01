#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include "ravo/desktop/filesystem_browser_model.h"
#include "ravo/desktop/studio_import_preferences.h"
#include "ravo/desktop/studio_presenter.h"
#include "ravo/foundation/log.h"
#include "studio_test_support.h"

namespace ravo
{
namespace
{
using namespace studio_test_support;

TEST(FilesystemBrowserModelTest, MountedRootsRefreshPreservesExpansionAndRejectsRemovedListing)
{
    ensure_qt_core();
    FilesystemBrowserModel model;
    model.resetWithRoots(
        {{"/home-fixture", "Home", true}, {"/picker-fixture", "Chosen folder", true}});
    quint64 generation = 0;
    QObject::connect(&model, &FilesystemBrowserModel::directoryListingRequested, &model,
                     [&](const QString &, quint64 value) { generation = value; });
    model.updateMountedRoots({{"/card-fixture", "Camera card", true}});
    model.activateFolder("/card-fixture");
    ASSERT_GT(generation, 0U);
    model.applyChildren("/card-fixture", generation,
                        std::vector<FilesystemFolderEntry>{{"/card-fixture/DCIM", "DCIM", true}});
    model.selectFolder("/card-fixture/DCIM");
    const auto count = model.rowCount();
    int resets = 0;
    QObject::connect(&model, &QAbstractItemModel::modelReset, &model, [&] { ++resets; });
    model.updateMountedRoots(
        {{"/card-fixture", "Camera card", true}, {"/second-card-fixture", "Second card", true}});
    EXPECT_EQ(model.rowCount(), count + 1);
    EXPECT_EQ(model.selectedPath(), QStringLiteral("/card-fixture/DCIM"));
    EXPECT_FALSE(model.data(model.index(2, 0), FilesystemBrowserModel::CollapsedRole).toBool());
    const auto reset_count = resets;
    model.updateMountedRoots(
        {{"/card-fixture", "Camera card", true}, {"/second-card-fixture", "Second card", true}});
    EXPECT_EQ(resets, reset_count);

    model.toggleCollapsed("/second-card-fixture");
    const auto stale = generation;
    model.updateMountedRoots({});
    EXPECT_EQ(model.rowCount(), 2); // Home and explicit picker roots survive removal.
    model.updateMountedRoots({{"/second-card-fixture", "Reinserted card", true}});
    model.toggleCollapsed("/second-card-fixture");
    ASSERT_GT(generation, stale);
    model.applyChildren(
        "/second-card-fixture", stale,
        std::vector<FilesystemFolderEntry>{{"/second-card-fixture/stale", "stale", true}});
    EXPECT_EQ(model.rowCount(), 3);
    EXPECT_TRUE(model.data(model.index(2, 0), FilesystemBrowserModel::ListingPendingRole).toBool());
    model.applyChildren(
        "/second-card-fixture", generation,
        std::vector<FilesystemFolderEntry>{{"/second-card-fixture/DCIM", "DCIM", true}});
    EXPECT_EQ(model.rowCount(), 4);
}

TEST(FilesystemBrowserModelTest, MountedRootDiscoveryReturnsAccessibleAbsoluteDirectories)
{
    ensure_qt_core();
    const auto roots = list_mounted_filesystem_roots();
    ASSERT_FALSE(roots.empty());
    QStringList seen;
    for (const auto &root : roots)
    {
        EXPECT_TRUE(QDir::isAbsolutePath(root.path));
        EXPECT_TRUE(QDir(root.path).exists());
        EXPECT_FALSE(root.display_name.isEmpty());
        EXPECT_FALSE(seen.contains(root.path));
        seen.push_back(root.path);
    }
    // An opt-in hardware assertion can bind discovery to a mount independently
    // identified by the host (for example diskutil), without scanning its photos.
    const auto expected = qEnvironmentVariable("RAVO_TEST_EXPECT_MOUNTED_ROOT");
    if (!expected.isEmpty())
        EXPECT_TRUE(seen.contains(QDir::cleanPath(expected))) << expected.toStdString();
}

TEST(FilesystemBrowserModelTest, OverlappingRootsKeepUniqueFolderIdentities)
{
    ensure_qt_core();
    FilesystemBrowserModel model;
    model.resetWithRoots({{"/disk/Home", "Home", true}});
    model.updateMountedRoots({{"/disk", "Disk", true}});
    quint64 generation = 0;
    QObject::connect(&model, &FilesystemBrowserModel::directoryListingRequested, &model,
                     [&](const QString &, quint64 value) { generation = value; });
    model.toggleCollapsed("/disk");
    model.applyChildren("/disk", generation,
                        std::vector<FilesystemFolderEntry>{{"/disk/Home", "Home", true},
                                                           {"/disk/DCIM", "DCIM", true}});
    ASSERT_EQ(model.rowCount(), 3);
    EXPECT_EQ(model.data(model.index(2, 0), FilesystemBrowserModel::PathRole).toString(),
              QStringLiteral("/disk/DCIM"));
    model.activateFolder("/disk/Home");
    model.applyChildren(
        "/disk/Home", generation,
        std::vector<FilesystemFolderEntry>{{"/disk/Home/Pictures", "Pictures", true}});
    ASSERT_EQ(model.rowCount(), 4);
    EXPECT_EQ(model.selectedPath(), QStringLiteral("/disk/Home"));
    EXPECT_EQ(model.data(model.index(1, 0), FilesystemBrowserModel::PathRole).toString(),
              QStringLiteral("/disk/Home/Pictures"));
}

TEST(StudioImportWorkspace, MountedVolumesAppearInBothTreesWithoutChangingSource)
{
    ensure_qt_core();
    init_logging("ravo-import-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(StudioImportPreferences{}.rememberSource(directory.path()));
    const auto roots = list_mounted_filesystem_roots();
    ASSERT_FALSE(roots.empty());
    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath("catalog.sqlite"));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.openImportPage();
    const auto volumes_visible = [&]
    {
        for (auto *model : {presenter.importSourceFolders(), presenter.importDestinationFolders()})
            for (const auto &root : roots)
            {
                bool found = false;
                for (int row = 0; row < model->rowCount(); ++row)
                    if (model->data(model->index(row, 0), FilesystemBrowserModel::PathRole)
                            .toString() == root.path)
                        found = true;
                if (!found)
                    return false;
            }
        return true;
    };
    ASSERT_TRUE(wait_until(volumes_visible));
    EXPECT_EQ(presenter.importSourceRoot(), directory.path());
    EXPECT_EQ(presenter.importSourceFolders()->selectedPath(), directory.path());
    presenter.closeImportPage();
    presenter.openImportPage();
    ASSERT_TRUE(wait_until(volumes_visible));
    EXPECT_EQ(presenter.importSourceRoot(), directory.path());
    presenter.closeImportPage();
}

} // namespace
} // namespace ravo
