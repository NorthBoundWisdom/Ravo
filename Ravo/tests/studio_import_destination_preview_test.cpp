#include <cstdint>
#include <string>
#include <utility>
#include <vector>

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
namespace
{

void expect_candidate_keyboard_batch_check_skips_duplicates()
{
    ImportCandidateListModel model;
    std::vector<ImportCandidate> candidates(4);
    for (int row = 0; row < static_cast<int>(candidates.size()); ++row)
    {
        candidates[static_cast<std::size_t>(row)].source_path =
            "/candidate-" + std::to_string(row) + ".png";
        candidates[static_cast<std::size_t>(row)].size_bytes =
            static_cast<std::uint64_t>((row + 1) * 10);
    }
    candidates[1].duplicate = true;
    candidates[1].duplicate_reason = "catalog_content";
    model.setCandidates(std::move(candidates));

    ASSERT_EQ(model.selectedCount(), 3);
    model.highlightRange(0, 2, false);
    EXPECT_TRUE(model.highlighted(0));
    EXPECT_FALSE(model.highlighted(1));
    EXPECT_TRUE(model.highlighted(2));

    model.applyCheck(2);
    EXPECT_EQ(model.selectedCount(), 1);
    EXPECT_FALSE(
        model.data(model.index(0, 0), ImportCandidateListModel::SelectedRole).toBool());
    EXPECT_FALSE(
        model.data(model.index(1, 0), ImportCandidateListModel::SelectedRole).toBool());
    EXPECT_FALSE(
        model.data(model.index(2, 0), ImportCandidateListModel::SelectedRole).toBool());
    EXPECT_TRUE(model.data(model.index(3, 0), ImportCandidateListModel::SelectedRole).toBool());

    model.highlightAll();
    model.applyCheck(3);
    EXPECT_EQ(model.selectedCount(), 0);
    model.applyCheck(0);
    EXPECT_EQ(model.selectedCount(), 3);
    EXPECT_FALSE(model.highlighted(1));
}

void expect_candidate_grid_keyboard_contract()
{
    QFile file(QString::fromUtf8(RAVO_REPOSITORY_ROOT) +
               QStringLiteral("/Ravo/desktop/qml/chrome/ImportPhotoGrid.qml"));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto source = QString::fromUtf8(file.readAll());

    EXPECT_TRUE(source.contains(QStringLiteral("activeFocusOnTab: true")));
    EXPECT_TRUE(source.contains(QStringLiteral("Keys.priority: Keys.BeforeItem")));
    EXPECT_TRUE(source.contains(QStringLiteral("function moveKeyboardFocus")));
    EXPECT_TRUE(source.contains(QStringLiteral("event.key === Qt.Key_Left")));
    EXPECT_TRUE(source.contains(QStringLiteral("event.key === Qt.Key_PageDown")));
    EXPECT_FALSE(source.contains(QStringLiteral("importCandidates.highlightAll")));
    EXPECT_TRUE(source.contains(
        QStringLiteral("highlightRange(root.selectionAnchor, bounded, additive)")));
    EXPECT_TRUE(
        source.contains(QStringLiteral("applyCheck(candidateGrid.currentIndex)")));
    EXPECT_TRUE(source.contains(QStringLiteral("candidateGrid.currentIndex = index")));
    EXPECT_TRUE(source.contains(QStringLiteral("Accessible.description")));

    QFile page_file(QString::fromUtf8(RAVO_REPOSITORY_ROOT) +
                    QStringLiteral("/Ravo/desktop/qml/chrome/ImportPage.qml"));
    ASSERT_TRUE(page_file.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto page_source = QString::fromUtf8(page_file.readAll());
    EXPECT_TRUE(page_source.contains(QStringLiteral("candidateKeyboardHelp")));
    EXPECT_TRUE(page_source.contains(QStringLiteral("selectionArea.width >= 720")));
}

} // namespace

TEST(StudioImportWorkspace, DestinationPreviewTracksSelectionAndOrganizationWithoutCreatingFolders)
{
    ensure_qt_core();
    expect_candidate_keyboard_batch_check_skips_duplicates();
    expect_candidate_grid_keyboard_contract();

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
