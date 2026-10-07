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
    presenter.imports()->openImportPage();
    presenter.imports()->setImportSourceRoot(source);
    presenter.imports()->setImportDestination(destination);
    presenter.imports()->setImportOrganization(QStringLiteral("hierarchy"));
    const auto ready = [&]
    {
        return !presenter.imports()->importScanActive() &&
               !presenter.imports()->importDestinationPreviewActive() &&
               !presenter.imports()->importDestinationPreview().empty();
    };
    ASSERT_TRUE(wait_until(ready))
        << presenter.imports()->importDestinationPreviewError().toStdString();
    ASSERT_TRUE(presenter.imports()->importDestinationPreviewError().isEmpty());
    auto folders = presenter.imports()->importDestinationPreview();
    ASSERT_EQ(folders.size(), 3);
    EXPECT_EQ(folders.front().toMap().value("photoCount").toUInt(), 2U);
    EXPECT_EQ(folders.back().toMap().value("photoCount").toUInt(), 1U);
    EXPECT_TRUE(folders.back().toMap().value("willCreate").toBool());
    EXPECT_TRUE(QDir(destination).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).empty());
    presenter.imports()->importCandidates()->setAllSelected(false);
    EXPECT_TRUE(presenter.imports()->importDestinationPreview().empty());
    presenter.imports()->importCandidates()->setAllSelected(true);
    presenter.imports()->setImportOrganization(QStringLiteral("date"));
    presenter.imports()->setImportOrganization(QStringLiteral("month"));
    ASSERT_TRUE(wait_until(ready))
        << presenter.imports()->importDestinationPreviewError().toStdString();
    folders = presenter.imports()->importDestinationPreview();
    ASSERT_EQ(folders.size(), 3);
    EXPECT_EQ(folders.back().toMap().value("depth").toInt(), 2);
    EXPECT_EQ(folders.back().toMap().value("photoCount").toUInt(), 2U);
    auto *tree = presenter.imports()->importDestinationFolders();
    const auto month_path = destination + "/" + folders[1].toMap().value("name").toString() + "/" +
                            folders.back().toMap().value("name").toString();
    ASSERT_TRUE(wait_until(
        [&]
        {
            for (int row = 0; row < tree->rowCount(); ++row)
                if (tree->data(tree->index(row, 0), FilesystemBrowserModel::PathRole) == month_path)
                    return tree->data(tree->index(row, 0), FilesystemBrowserModel::WillCreateRole)
                        .toBool();
            return false;
        }));
    EXPECT_FALSE(QDir(month_path).exists());
    presenter.imports()->setImportOrganization(QStringLiteral("hierarchy"));
    presenter.imports()->closeImportPage();
    EXPECT_FALSE(presenter.imports()->importDestinationPreviewActive());
    EXPECT_TRUE(presenter.imports()->importDestinationPreview().empty());
    for (int row = 0; row < tree->rowCount(); ++row)
        EXPECT_FALSE(
            tree->data(tree->index(row, 0), FilesystemBrowserModel::WillCreateRole).toBool());
    EXPECT_TRUE(QDir(destination).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).empty());
    presenter.imports()->openImportPage();
    presenter.imports()->setImportDestination(destination);
    ASSERT_TRUE(wait_until(ready))
        << presenter.imports()->importDestinationPreviewError().toStdString();
    presenter.imports()->startPlannedImport();
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.imports()->importPreflightActive() &&
                   !presenter.imports()->importWorkActive();
        },
        30000));
    EXPECT_EQ(presenter.lastImportCount(), 2U);
    EXPECT_TRUE(QFile::exists(destination + "/source/a.png"));
    EXPECT_TRUE(QFile::exists(destination + "/source/nested/b.png"));
    EXPECT_TRUE(QFile::exists(source + "/a.png"));
}

TEST(StudioImportWorkspace, DestinationPreviewKeyUsesRevisionsNotPathSnapshots)
{
    ensure_qt_core();
    // Contract: destination-preview invalidation keys must use candidate
    // generation + selectionRevision (and related draft/catalog revisions),
    // never selectedPaths() snapshots. Anchor on the controller wiring rather
    // than a single-line "destination_preview = std::make_unique" spell so
    // clang-format line breaks cannot false-fail the check.
    QFile presenter_file(QString::fromUtf8(RAVO_REPOSITORY_ROOT) +
                         QStringLiteral("/Ravo/desktop/src/studio_import_destination_preview.cpp"));
    ASSERT_TRUE(presenter_file.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto source = QString::fromUtf8(presenter_file.readAll());
    const auto key_begin =
        source.indexOf(QStringLiteral("make_unique<StudioImportDestinationPreviewController>"));
    ASSERT_GE(key_begin, 0);
    const auto request_builder =
        source.indexOf(QStringLiteral("return plannedImportRequest();"), key_begin);
    ASSERT_GT(request_builder, key_begin);
    const auto key_region = source.mid(key_begin, request_builder - key_begin + 30);
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
