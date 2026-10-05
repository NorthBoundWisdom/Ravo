#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include "ravo/desktop/filesystem_browser_model.h"
#include "studio_test_support.h"

namespace ravo
{
namespace
{
using namespace studio_test_support;

TEST(FilesystemBrowserModelTest,
     PreviewMergesMonthsWithExistingFoldersWithoutCreatingOrSelectingThem)
{
    ensure_qt_core();
    QTemporaryDir directory;
    const auto root = directory.path();
    ASSERT_TRUE(QDir().mkpath(root + "/2026/09"));
    ASSERT_TRUE(QDir().mkpath(root + "/Images"));
    FilesystemBrowserModel model;
    model.resetWithRoots({{root, "Pictures", true}});
    QObject::connect(&model, &FilesystemBrowserModel::directoryListingRequested, &model,
                     [&](const QString &path, quint64 generation)
                     { model.applyChildren(path, generation, list_filesystem_folders(path)); });
    model.selectFolder(root);
    const auto plan = std::vector<ImportDestinationFolder>{
        {(root).toStdString(), "Pictures", 0, 3, false, false},
        {(root + "/2026").toStdString(), "2026", 1, 2, false, false},
        {(root + "/2026/09").toStdString(), "09", 2, 1, false, false},
        {(root + "/2026/10").toStdString(), "10", 2, 1, true, false},
        {(root + "/2027").toStdString(), "2027", 1, 1, true, false},
        {(root + "/2027/01").toStdString(), "01", 2, 1, true, false},
        {(root + "/second/2028").toStdString(), "2028", 1, 1, true, true}};
    model.setPreviewFolders(plan);
    ASSERT_EQ(model.rowCount(), 7);
    const QStringList names{"Pictures", "2026", "09", "10", "2027", "01", "Images"};
    for (int row = 0; row < names.size(); ++row)
        EXPECT_EQ(model.data(model.index(row, 0), FilesystemBrowserModel::DisplayNameRole),
                  names[row]);
    EXPECT_FALSE(model.data(model.index(1, 0), FilesystemBrowserModel::WillCreateRole).toBool());
    EXPECT_FALSE(model.data(model.index(2, 0), FilesystemBrowserModel::WillCreateRole).toBool());
    EXPECT_TRUE(model.data(model.index(3, 0), FilesystemBrowserModel::WillCreateRole).toBool());
    EXPECT_TRUE(model.data(model.index(4, 0), FilesystemBrowserModel::WillCreateRole).toBool());
    EXPECT_EQ(model.data(model.index(5, 0), FilesystemBrowserModel::DepthRole).toInt(), 2);
    model.activateFolder(root + "/2026");
    EXPECT_EQ(model.rowCount(), 7); // Choosing an expanded real year does not collapse it.
    model.activateFolder(root + "/2026/10");
    model.selectFolder(root + "/2027");
    EXPECT_EQ(model.selectedPath(), root + "/2026");
    model.toggleCollapsed(root + "/2027");
    EXPECT_EQ(model.rowCount(), 6);
    model.toggleCollapsed(root + "/2027");
    EXPECT_EQ(model.rowCount(), 7);
    model.toggleCollapsed(root + "/2026");
    EXPECT_EQ(model.rowCount(), 5);
    model.selectFolder(root + "/2026/10");
    EXPECT_EQ(model.selectedPath(), root + "/2026");
    model.toggleCollapsed(root + "/2026");
    EXPECT_EQ(model.rowCount(), 7);
    EXPECT_FALSE(QDir(root + "/2026/10").exists());
    EXPECT_FALSE(QDir(root + "/2027").exists());
    model.setPreviewFolders({});
    model.activateFolder(root);
    ASSERT_EQ(model.rowCount(), 4);
    model.activateFolder(root + "/2026");
    EXPECT_EQ(model.rowCount(), 4);
    EXPECT_EQ(model.data(model.index(2, 0), FilesystemBrowserModel::DisplayNameRole), "09");
}

TEST(FilesystemBrowserModelTest, PreviewSurvivesLateListingsAndClearsAfterReplacementOrFailure)
{
    ensure_qt_core();
    FilesystemBrowserModel model;
    const QString root = QDir::tempPath() + "/ravo-planned-folders";
    model.resetWithRoots({{root, "Pictures", true}});
    quint64 generation = 0;
    int requests = 0;
    QObject::connect(&model, &FilesystemBrowserModel::directoryListingRequested, &model,
                     [&](const QString &, quint64 value)
                     {
                         generation = value;
                         ++requests;
                     });
    const auto make_plan = [&](const QString &month)
    {
        return std::vector<ImportDestinationFolder>{
            {(root + "/2026").toStdString(), "2026", 1, 1, true, false},
            {(root + "/2026/" + month).toStdString(), month.toStdString(), 2, 1, true, false}};
    };
    model.setPreviewFolders(make_plan("09"));
    ASSERT_EQ(requests, 1);
    EXPECT_EQ(model.rowCount(), 3);
    model.setPreviewFolders(make_plan("10"));
    EXPECT_EQ(requests, 1);
    model.applyChildren(root, generation,
                        std::vector<FilesystemFolderEntry>{{root + "/Images", "Images", false}});
    ASSERT_EQ(model.rowCount(), 4);
    EXPECT_EQ(model.data(model.index(2, 0), FilesystemBrowserModel::DisplayNameRole), "10");
    model.setPreviewFolders({});
    EXPECT_EQ(model.rowCount(), 2);
    model.resetWithRoots({{root, "Pictures", true}});
    model.setPreviewFolders(make_plan("10"));
    model.applyChildren(root, generation, make_error(ErrorCode::kIo, "Disconnected"));
    ASSERT_EQ(model.rowCount(), 1);
    EXPECT_EQ(model.data(model.index(0, 0), FilesystemBrowserModel::ErrorRole), "Disconnected");
    EXPECT_EQ(requests, 2); // No retry loop for a failed filesystem owner.
    model.setPreviewFolders({});
    EXPECT_EQ(model.rowCount(), 1);
}

} // namespace
} // namespace ravo
