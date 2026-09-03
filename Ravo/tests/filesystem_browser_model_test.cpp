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

TEST(FilesystemBrowserModelTest, ListsVisibleChildrenAndSkipsHiddenFolders)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(QDir(directory.path()).mkdir(QStringLiteral("Pictures")));
    ASSERT_TRUE(QDir(directory.path()).mkdir(QStringLiteral("Documents")));
    ASSERT_TRUE(QDir(directory.path()).mkdir(QStringLiteral(".hidden")));

    auto listed = list_filesystem_folders(directory.path());
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed.value().size(), 2U);
    EXPECT_EQ(listed.value()[0].display_name, QStringLiteral("Documents"));
    EXPECT_EQ(listed.value()[1].display_name, QStringLiteral("Pictures"));
}

TEST(FilesystemBrowserModelTest, ExpandSelectsWithoutLosingRoots)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString pictures = QDir(directory.path()).filePath(QStringLiteral("Pictures"));
    ASSERT_TRUE(QDir().mkpath(QDir(pictures).filePath(QStringLiteral("2024"))));

    FilesystemFolderEntry root;
    root.path = directory.path();
    root.display_name = QStringLiteral("root");
    FilesystemBrowserModel model;
    model.resetWithRoots({root});
    ASSERT_EQ(model.rowCount(), 1);
    model.selectFolder(directory.path());
    EXPECT_TRUE(model.data(model.index(0, 0), FilesystemBrowserModel::SelectedRole).toBool());

    const auto generation = 1U;
    model.toggleCollapsed(directory.path());
    auto listed = list_filesystem_folders(directory.path());
    ASSERT_TRUE(listed) << listed.error().message;
    model.applyChildren(directory.path(), generation, std::move(listed));
    EXPECT_GE(model.rowCount(), 2);
    bool found_pictures = false;
    for (int row = 0; row < model.rowCount(); ++row)
    {
        if (model.data(model.index(row, 0), FilesystemBrowserModel::DisplayNameRole).toString() ==
            QStringLiteral("Pictures"))
            found_pictures = true;
    }
    EXPECT_TRUE(found_pictures);
    model.selectFolder(pictures);
    EXPECT_EQ(model.selectedPath(), QDir::cleanPath(QDir::fromNativeSeparators(pictures)));
}

} // namespace
} // namespace ravo
