#include <QColorSpace>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include "ravo/desktop/studio_presenter.h"
#include "ravo/foundation/log.h"
#include "studio_test_support.h"

namespace ravo
{
using namespace studio_test_support;
TEST(StudioImportWorkspace, DestinationPreviewTracksSelectionAndOrganizationWithoutCreatingFolders)
{
    ensure_qt_core();
    init_logging("ravo-import-preview-tests");
    QTemporaryDir directory;
    const auto source = directory.filePath("source");
    const auto destination = directory.filePath("destination");
    ASSERT_TRUE(QDir().mkpath(source + "/nested"));
    ASSERT_TRUE(QDir().mkpath(destination));
    QImage image(32, 24, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(Qt::red);
    ASSERT_TRUE(image.save(source + "/a.png"));
    image.fill(Qt::blue);
    ASSERT_TRUE(image.save(source + "/nested/b.png"));
    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath("library.sqlite"));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.openImportPage();
    presenter.setImportSourceRoot(source);
    presenter.setImportDestination(destination);
    presenter.setImportOrganization(QStringLiteral("hierarchy"));
    const auto ready = [&]
    {
        return !presenter.importScanActive() && !presenter.importDestinationPreviewActive() &&
               !presenter.importDestinationPreview().empty();
    };
    ASSERT_TRUE(wait_until(ready)) << presenter.importDestinationPreviewError().toStdString();
    ASSERT_TRUE(presenter.importDestinationPreviewError().isEmpty());
    auto folders = presenter.importDestinationPreview();
    ASSERT_EQ(folders.size(), 3);
    EXPECT_EQ(folders.front().toMap().value("photoCount").toUInt(), 2U);
    EXPECT_EQ(folders.back().toMap().value("photoCount").toUInt(), 1U);
    EXPECT_TRUE(folders.back().toMap().value("willCreate").toBool());
    EXPECT_TRUE(QDir(destination).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).empty());
    presenter.importCandidates()->setAllSelected(false);
    EXPECT_TRUE(presenter.importDestinationPreview().empty());
    presenter.importCandidates()->setAllSelected(true);
    presenter.setImportOrganization(QStringLiteral("date"));
    presenter.setImportOrganization(QStringLiteral("month"));
    ASSERT_TRUE(wait_until(ready)) << presenter.importDestinationPreviewError().toStdString();
    folders = presenter.importDestinationPreview();
    ASSERT_EQ(folders.size(), 3);
    EXPECT_EQ(folders.back().toMap().value("depth").toInt(), 2);
    EXPECT_EQ(folders.back().toMap().value("photoCount").toUInt(), 2U);
    presenter.setImportOrganization(QStringLiteral("hierarchy"));
    presenter.closeImportPage();
    EXPECT_FALSE(presenter.importDestinationPreviewActive());
    EXPECT_TRUE(presenter.importDestinationPreview().empty());
    EXPECT_TRUE(QDir(destination).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).empty());
    presenter.openImportPage();
    presenter.setImportDestination(destination);
    ASSERT_TRUE(wait_until(ready)) << presenter.importDestinationPreviewError().toStdString();
    presenter.startPlannedImport();
    ASSERT_TRUE(wait_until(
        [&] { return !presenter.importPreflightActive() && !presenter.importWorkActive(); },
        30000));
    EXPECT_EQ(presenter.lastImportCount(), 2U);
    EXPECT_TRUE(QFile::exists(destination + "/source/a.png"));
    EXPECT_TRUE(QFile::exists(destination + "/source/nested/b.png"));
    EXPECT_TRUE(QFile::exists(source + "/a.png"));
}
} // namespace ravo
