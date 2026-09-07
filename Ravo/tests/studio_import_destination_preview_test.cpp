#include <QColorSpace>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include "ravo/desktop/import_candidate_list_model.h"
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

TEST(StudioImportWorkspace, DestinationPreviewKeyUsesRevisionsNotPathSnapshots)
{
    ensure_qt_core();
    QFile presenter_file(QString::fromUtf8(RAVO_REPOSITORY_ROOT) +
                         QStringLiteral("/Ravo/desktop/src/studio_presenter.cpp"));
    ASSERT_TRUE(presenter_file.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto source = QString::fromUtf8(presenter_file.readAll());
    const auto key_begin = source.indexOf(QStringLiteral("destination_preview ="));
    ASSERT_GE(key_begin, 0);
    const auto key_region = source.mid(key_begin, 4000);
    EXPECT_TRUE(key_region.contains(QStringLiteral("std::make_unique")));
    EXPECT_TRUE(key_region.contains(QStringLiteral("selectionRevision")));
    EXPECT_TRUE(key_region.contains(QStringLiteral("generation")));
    EXPECT_FALSE(key_region.contains(QStringLiteral("selectedPaths()")));
    EXPECT_TRUE(key_region.contains(QStringLiteral("plannedImportRequest")));

    ImportCandidateListModel model;
    std::vector<ImportCandidate> candidates(1000);
    for (int row = 0; row < 1000; ++row)
    {
        candidates[static_cast<std::size_t>(row)].source_path = "/p" + std::to_string(row) + ".png";
        candidates[static_cast<std::size_t>(row)].size_bytes = 1;
    }
    model.setCandidates(std::move(candidates));
    model.resetSelectedPathsCallCount();
    for (int row = 0; row < 100; ++row)
        model.toggleSelected(row);
    EXPECT_EQ(model.selectedPathsCallCount(), 0U)
        << "check mutations must not enumerate paths before debounce snapshot";
}

} // namespace ravo
