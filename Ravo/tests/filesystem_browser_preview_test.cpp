#include <array>

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
    const std::array<qulonglong, 7> counts{3, 2, 1, 1, 1, 1, 0};
    for (int row = 0; row < names.size(); ++row)
        EXPECT_EQ(model.data(model.index(row, 0), FilesystemBrowserModel::PlannedPhotoCountRole)
                      .toULongLong(),
                  counts[static_cast<std::size_t>(row)]);
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
    for (int row = 0; row < model.rowCount(); ++row)
        EXPECT_EQ(model.data(model.index(row, 0), FilesystemBrowserModel::PlannedPhotoCountRole)
                      .toULongLong(),
                  0U);
    model.activateFolder(root);
    ASSERT_EQ(model.rowCount(), 4);
    model.activateFolder(root + "/2026");
    EXPECT_EQ(model.rowCount(), 4);
    EXPECT_EQ(model.data(model.index(2, 0), FilesystemBrowserModel::DisplayNameRole), "09");
}

TEST(FilesystemBrowserModelTest, PreviewRevealsNestedDestinationAfterDeferredAncestorListings)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto root = directory.path();
    const auto destination = root + "/Pictures";
    ASSERT_TRUE(QDir().mkpath(destination + "/2026/03"));
    FilesystemBrowserModel model;
    model.resetWithRoots({{root, "Home", true}});
    std::vector<std::pair<QString, quint64>> requests;
    QObject::connect(&model, &FilesystemBrowserModel::directoryListingRequested, &model,
                     [&](const QString &path, const quint64 generation)
                     { requests.emplace_back(path, generation); });
    QString revealed;
    bool preview_revealed_after_last_reset = false;
    QObject::connect(&model, &QAbstractItemModel::modelReset, &model,
                     [&] { preview_revealed_after_last_reset = false; });
    QObject::connect(
        &model, &FilesystemBrowserModel::folderRevealed, &model,
        [&](const int row)
        {
            revealed = model.data(model.index(row, 0), FilesystemBrowserModel::PathRole).toString();
            preview_revealed_after_last_reset =
                model.data(model.index(row, 0), FilesystemBrowserModel::WillCreateRole).toBool();
        });
    const auto month = destination + "/2026/10";
    model.setPreviewFolders({{destination.toStdString(), "Pictures", 0, 2, false, false},
                             {(destination + "/2026").toStdString(), "2026", 1, 2, false, false},
                             {month.toStdString(), "10", 2, 2, true, false}},
                            destination);
    // Publishing the plan must itself reveal its destination through the Home
    // tree. Neither the QML view nor a synchronous preloaded root is an oracle.
    for (std::size_t request = 0; request < 3; ++request)
    {
        ASSERT_GT(requests.size(), request);
        const auto [path, generation] = requests[request];
        model.applyChildren(path, generation, list_filesystem_folders(path));
    }
    ASSERT_EQ(requests.size(), 3U);
    EXPECT_EQ(revealed, month);
    EXPECT_TRUE(preview_revealed_after_last_reset);
    EXPECT_EQ(model.selectedPath(), destination);
    ASSERT_EQ(model.rowCount(), 5);
    EXPECT_EQ(model.data(model.index(4, 0), FilesystemBrowserModel::PathRole), month);
    EXPECT_TRUE(model.data(model.index(4, 0), FilesystemBrowserModel::WillCreateRole).toBool());
    EXPECT_EQ(
        model.data(model.index(4, 0), FilesystemBrowserModel::PlannedPhotoCountRole).toULongLong(),
        2U);
    EXPECT_FALSE(QDir(month).exists());
    revealed.clear();
    model.toggleCollapsed(destination + "/2026");
    EXPECT_TRUE(revealed.isEmpty()); // A completed reveal never fights later user scrolling.
    const auto collapsed_rows = model.rowCount();
    int resets = 0;
    const auto reset_connection =
        QObject::connect(&model, &QAbstractItemModel::modelReset, &model, [&] { ++resets; });
    model.setPreviewFolders({{destination.toStdString(), "Pictures", 0, 1000, false, false},
                             {(destination + "/2026").toStdString(), "2026", 1, 1000, false, false},
                             {month.toStdString(), "10", 2, 1000, true, false}},
                            destination);
    EXPECT_EQ(resets, 0);
    EXPECT_EQ(model.rowCount(), collapsed_rows);
    EXPECT_TRUE(revealed.isEmpty());
    EXPECT_EQ(model.selectedPath(), destination);
    model.toggleCollapsed(destination + "/2026");
    EXPECT_EQ(
        model.data(model.index(4, 0), FilesystemBrowserModel::PlannedPhotoCountRole).toULongLong(),
        1000U);
    QObject::disconnect(reset_connection);
    model.setPreviewFolders({});
    for (int row = 0; row < model.rowCount(); ++row)
        EXPECT_FALSE(
            model.data(model.index(row, 0), FilesystemBrowserModel::WillCreateRole).toBool());
    EXPECT_FALSE(QDir(month).exists());
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
