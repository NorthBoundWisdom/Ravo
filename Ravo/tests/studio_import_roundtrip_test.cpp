#include <QColorSpace>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QImage>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include "ravo/desktop/import_candidate_list_model.h"
#include "ravo/desktop/studio_presenter.h"
#include "studio_test_support.h"
#include "ravo/foundation/log.h"

namespace ravo
{
using namespace studio_test_support;

namespace
{
bool photo(const QString &path, const QColor &color)
{
    QImage image(32, 24, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(color);
    return image.save(path, "PNG");
}

QByteArray file_sha256(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}
} // namespace

// Temp media + temp catalog only. Opt-in real corpus is not exercised here (C1/C2).
TEST(StudioImportRoundtrip, CopyPreservesSourceHashAndGalleryMembership)
{
    ensure_qt_core();
    init_logging("ravo-import-roundtrip");
    QTemporaryDir directory;
    const auto source = directory.filePath("source");
    const auto destination = directory.filePath("destination");
    ASSERT_TRUE(QDir().mkpath(source));
    ASSERT_TRUE(QDir().mkpath(destination));
    ASSERT_TRUE(photo(source + "/keep.png", Qt::green));
    ASSERT_TRUE(photo(source + "/also.png", Qt::yellow));
    const auto keep_hash = file_sha256(source + "/keep.png");
    const auto also_hash = file_sha256(source + "/also.png");
    ASSERT_FALSE(keep_hash.isEmpty());
    ASSERT_FALSE(also_hash.isEmpty());

    const auto catalog = directory.filePath("library.sqlite");
    {
        StudioPresenter presenter;
        presenter.createCatalogFromPath(catalog);
        ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));

        presenter.openImportPage();
        presenter.importSourceFolders()->resetWithRoots(
            {{directory.path(), QStringLiteral("fixture"), true}});
        presenter.importDestinationFolders()->resetWithRoots(
            {{directory.path(), QStringLiteral("fixture"), true}});
        presenter.setImportSourceRoot(source);
        presenter.setImportDestination(destination);
        ASSERT_TRUE(
            wait_until([&] { return !presenter.importScanActive() && presenter.importReady(); }));
        ASSERT_EQ(presenter.importCandidates()->rowCount(), 2);
        ASSERT_EQ(presenter.importCandidates()->selectedCount(), 2);

        // Destination preview may be empty while debounce runs; wait for inactive.
        ASSERT_TRUE(wait_until([&] { return !presenter.importDestinationPreviewActive(); }, 5000));

        presenter.startPlannedImport();
        ASSERT_TRUE(wait_until(
            [&] { return !presenter.importPreflightActive() && !presenter.importWorkActive(); },
            30000));
        ASSERT_TRUE(presenter.errorText().isEmpty()) << presenter.errorText().toStdString();
        EXPECT_EQ(presenter.lastImportCount(), 2);

        EXPECT_EQ(file_sha256(source + "/keep.png"), keep_hash);
        EXPECT_EQ(file_sha256(source + "/also.png"), also_hash);
        EXPECT_TRUE(QFile::exists(destination + "/keep.png"));
        EXPECT_TRUE(QFile::exists(destination + "/also.png"));
        EXPECT_EQ(file_sha256(destination + "/keep.png"), keep_hash);
        EXPECT_EQ(file_sha256(destination + "/also.png"), also_hash);

        presenter.closeImportPage();
        presenter.openImportPage();
        presenter.setImportSourceRoot(source);
        ASSERT_TRUE(wait_until([&] { return !presenter.importScanActive(); }));
        EXPECT_EQ(presenter.importDuplicateCount(), 2);
        EXPECT_FALSE(presenter.importReady());
        EXPECT_GE(presenter.libraryTotal(), 2);
        presenter.closeImportPage();
        // Same-catalog second Presenter reopen is covered by existing workspace reopen
        // fixtures; this tranche asserts membership on the live session after copy.
    }
}

TEST(StudioImportRoundtrip, CancelViaPageCloseLeavesSourcesUntouched)
{
    ensure_qt_core();
    init_logging("ravo-import-roundtrip");
    QTemporaryDir directory;
    const auto source = directory.filePath("source");
    const auto destination = directory.filePath("destination");
    ASSERT_TRUE(QDir().mkpath(source));
    ASSERT_TRUE(QDir().mkpath(destination));
    ASSERT_TRUE(photo(source + "/solo.png", Qt::cyan));
    const auto original = file_sha256(source + "/solo.png");
    ASSERT_FALSE(original.isEmpty());

    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath("library.sqlite"));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.openImportPage();
    presenter.importSourceFolders()->resetWithRoots(
        {{directory.path(), QStringLiteral("fixture"), true}});
    presenter.importDestinationFolders()->resetWithRoots(
        {{directory.path(), QStringLiteral("fixture"), true}});
    presenter.setImportSourceRoot(source);
    presenter.setImportDestination(destination);
    ASSERT_TRUE(
        wait_until([&] { return !presenter.importScanActive() && presenter.importReady(); }));
    presenter.startPlannedImport();
    presenter.closeImportPage();
    ASSERT_TRUE(wait_until(
        [&] { return !presenter.importPreflightActive() && !presenter.importWorkActive(); }));
    EXPECT_EQ(file_sha256(source + "/solo.png"), original);
    EXPECT_TRUE(QDir(destination).entryList(QDir::Files).isEmpty());
}
TEST(StudioImportRoundtrip, DestroyAndReopenSameCatalogPreservesMembership)
{
    ensure_qt_core();
    init_logging("ravo-import-roundtrip");
    QTemporaryDir directory;
    const auto source = directory.filePath("source");
    const auto destination = directory.filePath("destination");
    ASSERT_TRUE(QDir().mkpath(source));
    ASSERT_TRUE(QDir().mkpath(destination));
    ASSERT_TRUE(photo(source + "/keep.png", Qt::green));
    ASSERT_TRUE(photo(source + "/skip.png", Qt::yellow));
    const auto keep_hash = file_sha256(source + "/keep.png");
    const auto skip_hash = file_sha256(source + "/skip.png");
    const auto keep_size = QFileInfo(source + "/keep.png").size();
    const auto keep_mtime = QFileInfo(source + "/keep.png").lastModified();
    ASSERT_FALSE(keep_hash.isEmpty());
    ASSERT_FALSE(skip_hash.isEmpty());

    const auto catalog = directory.filePath("library.sqlite");
    QStringList imported_ids;
    {
        StudioPresenter presenter;
        presenter.createCatalogFromPath(catalog);
        ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
        presenter.openImportPage();
        presenter.importSourceFolders()->resetWithRoots(
            {{directory.path(), QStringLiteral("fixture"), true}});
        presenter.importDestinationFolders()->resetWithRoots(
            {{directory.path(), QStringLiteral("fixture"), true}});
        presenter.setImportSourceRoot(source);
        presenter.setImportDestination(destination);
        ASSERT_TRUE(
            wait_until([&] { return !presenter.importScanActive() && presenter.importReady(); }));
        ASSERT_EQ(presenter.importCandidates()->rowCount(), 2);
        // Explicit subset: uncheck skip.png (row 1 after sort may vary — match by name).
        auto *model = presenter.importCandidates();
        for (int row = 0; row < model->rowCount(); ++row)
        {
            const auto name =
                model->data(model->index(row, 0), ImportCandidateListModel::DisplayNameRole)
                    .toString();
            if (name == QStringLiteral("skip.png") &&
                model->data(model->index(row, 0), ImportCandidateListModel::SelectedRole).toBool())
                model->applyCheck(row);
        }
        ASSERT_EQ(model->selectedCount(), 1);
        ASSERT_TRUE(wait_until([&] { return !presenter.importDestinationPreviewActive(); }, 5000));
        presenter.startPlannedImport();
        ASSERT_TRUE(wait_until(
            [&] { return !presenter.importPreflightActive() && !presenter.importWorkActive(); },
            30000));
        ASSERT_TRUE(presenter.errorText().isEmpty()) << presenter.errorText().toStdString();
        EXPECT_EQ(presenter.lastImportCount(), 1);
        EXPECT_TRUE(QFile::exists(destination + "/keep.png"));
        EXPECT_FALSE(QFile::exists(destination + "/skip.png"));
        for (int row = 0; row < presenter.assets()->rowCount(); ++row)
        {
            const auto id = presenter.assets()->assetIdAt(row);
            if (!id.isEmpty())
                imported_ids.push_back(id);
        }
        EXPECT_EQ(imported_ids.size(), 1);
        presenter.closeImportPage();
    }

    // Destroy owner completely; reopen the same durable catalog path.
    {
        StudioPresenter presenter;
        presenter.openCatalogFromPath(catalog);
        ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
        EXPECT_EQ(presenter.libraryTotal(), 1);
        EXPECT_EQ(presenter.assets()->rowCount(), 1);
        const auto id = presenter.assets()->assetIdAt(0);
        ASSERT_FALSE(id.isEmpty());
        EXPECT_EQ(imported_ids.size(), 1);
        EXPECT_EQ(id, imported_ids.front());
        EXPECT_EQ(file_sha256(source + "/keep.png"), keep_hash);
        EXPECT_EQ(file_sha256(source + "/skip.png"), skip_hash);
        EXPECT_EQ(QFileInfo(source + "/keep.png").size(), keep_size);
        EXPECT_EQ(QFileInfo(source + "/keep.png").lastModified(), keep_mtime);
        EXPECT_EQ(file_sha256(destination + "/keep.png"), keep_hash);
    }
}

TEST(StudioImportRoundtrip, CancelledPreflightDoesNotLateImportAfterDrain)
{
    ensure_qt_core();
    init_logging("ravo-import-roundtrip");
    QTemporaryDir directory;
    const auto source = directory.filePath("source");
    const auto destination = directory.filePath("destination");
    ASSERT_TRUE(QDir().mkpath(source));
    ASSERT_TRUE(QDir().mkpath(destination));
    ASSERT_TRUE(photo(source + "/solo.png", Qt::cyan));
    const auto original = file_sha256(source + "/solo.png");
    ASSERT_FALSE(original.isEmpty());

    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath("library.sqlite"));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.openImportPage();
    presenter.importSourceFolders()->resetWithRoots(
        {{directory.path(), QStringLiteral("fixture"), true}});
    presenter.importDestinationFolders()->resetWithRoots(
        {{directory.path(), QStringLiteral("fixture"), true}});
    presenter.setImportSourceRoot(source);
    presenter.setImportDestination(destination);
    ASSERT_TRUE(
        wait_until([&] { return !presenter.importScanActive() && presenter.importReady(); }));
    presenter.startPlannedImport();
    presenter.closeImportPage(); // cancel / abandon while preflight may still be queued
    ASSERT_TRUE(wait_until(
        [&] { return !presenter.importPreflightActive() && !presenter.importWorkActive(); }));
    for (int i = 0; i < 50; ++i)
        QCoreApplication::processEvents();
    EXPECT_EQ(file_sha256(source + "/solo.png"), original);
    EXPECT_TRUE(QDir(destination).entryList(QDir::Files).isEmpty());
    EXPECT_EQ(presenter.libraryTotal(), 0);
}

} // namespace ravo
