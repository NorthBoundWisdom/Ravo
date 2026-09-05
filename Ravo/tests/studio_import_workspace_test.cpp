#include <QColorSpace>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QSettings>
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include "ravo/desktop/import_candidate_list_model.h"
#include "ravo/desktop/filesystem_browser_model.h"
#include "ravo/desktop/studio_import_preferences.h"
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
struct BlockedSettingsDirectory
{
    QString original;
    QString saved;
    ~BlockedSettingsDirectory()
    {
        if (original.isEmpty())
            return;
        QFile::remove(original);
        QDir().rename(saved, original);
    }
};
} // namespace

TEST(FilesystemBrowserModelTest, RevealExpandsAncestorsAndRejectsOldListings)
{
    ensure_qt_core();
    QTemporaryDir directory;
    const auto target = directory.filePath(QStringLiteral("Photos/2026/09"));
    ASSERT_TRUE(QDir().mkpath(target));
    FilesystemBrowserModel model;
    model.resetWithRoots({{directory.path(), QStringLiteral("root"), true}});
    int revealed = -1;
    QObject::connect(&model, &FilesystemBrowserModel::folderRevealed, &model,
                     [&](int row) { revealed = row; });
    QObject::connect(
        &model, &FilesystemBrowserModel::directoryListingRequested, &model,
        [&](const QString &path, quint64 generation)
        { model.applyChildren(path, generation, list_filesystem_folders(path)); },
        Qt::QueuedConnection);
    model.revealFolder(target);
    ASSERT_TRUE(wait_until([&] { return revealed >= 0; }));
    EXPECT_EQ(model.data(model.index(revealed, 0), FilesystemBrowserModel::PathRole).toString(),
              target);
    EXPECT_TRUE(
        model.data(model.index(revealed, 0), FilesystemBrowserModel::SelectedRole).toBool());
    model.resetWithRoots({{directory.path(), QStringLiteral("root"), true}});
    model.applyChildren(directory.path(), 1, list_filesystem_folders(directory.path()));
    EXPECT_EQ(model.rowCount(), 1);
}

TEST(StudioImportWorkspace, TypedDestinationPersistsAndRejectsMalformedValues)
{
    ensure_qt_core();
    QTemporaryDir directory;
    StudioImportPreferences preferences;
    ASSERT_TRUE(preferences.rememberDestination(directory.path()));
    EXPECT_EQ(StudioImportPreferences{}.loadLastDestination().value(), directory.path());
    EXPECT_FALSE(preferences.rememberDestination(QStringLiteral("relative/path")));
    EXPECT_EQ(preferences.loadLastDestination().value(), directory.path());
    QSettings settings;
    settings.setValue(QStringLiteral("desktop/import/lastDestination"), 42);
    settings.sync();
    EXPECT_FALSE(preferences.loadLastDestination());
    EXPECT_TRUE(preferences.loadLastDestination().value().isEmpty());
}

TEST(StudioImportWorkspace, SettingsWriteFailureKeepsCommittedPhotosAndPreviousDestination)
{
    ensure_qt_core();
    init_logging("ravo-import-tests");
    QTemporaryDir directory;
    const auto previous = directory.filePath("previous");
    const auto source = directory.filePath("source");
    const auto destination = directory.filePath("destination");
    ASSERT_TRUE(QDir().mkpath(previous));
    ASSERT_TRUE(QDir().mkpath(source));
    ASSERT_TRUE(QDir().mkpath(destination));
    ASSERT_TRUE(photo(source + "/photo.png", Qt::red));
    ASSERT_TRUE(StudioImportPreferences{}.rememberDestination(previous));
    {
        const auto settings_directory = QFileInfo(QSettings{}.fileName()).absolutePath();
        const auto saved = directory.filePath("saved-settings");
        ASSERT_TRUE(QDir().rename(settings_directory, saved));
        BlockedSettingsDirectory restore{settings_directory, saved};
        QFile blocker(settings_directory);
        ASSERT_TRUE(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        StudioPresenter presenter;
        presenter.createCatalogFromPath(directory.filePath("library.sqlite"));
        ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
        presenter.openImportPage();
        presenter.setImportSourceRoot(source);
        presenter.setImportDestination(destination);
        ASSERT_TRUE(wait_until([&] { return presenter.importReady(); }));
        presenter.startPlannedImport();
        ASSERT_TRUE(wait_until(
            [&] { return !presenter.importPreflightActive() && !presenter.importWorkActive(); },
            30000));
        EXPECT_EQ(presenter.lastImportCount(), 1);
        EXPECT_TRUE(QFile::exists(destination + "/photo.png"));
        EXPECT_TRUE(presenter.errorText().contains(QStringLiteral("could not be remembered")));
    }
    auto restored = StudioImportPreferences{}.loadLastDestination();
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_EQ(restored.value(), previous);
}

TEST(StudioImportWorkspace, StreamingCandidatesKeepUncheckedIntentAndExcludeDuplicates)
{
    ensure_qt_core();
    ImportCandidateListModel model;
    model.setCandidates({});
    model.setAllSelected(false);
    ImportCandidate candidate;
    candidate.source_path = "/photo.png";
    candidate.size_bytes = 128;
    candidate.content_sha256 = std::string(64, 'a');
    model.appendCandidate(candidate);
    EXPECT_EQ(model.selectedCount(), 0);
    model.setAllSelected(true);
    EXPECT_EQ(model.selectedBytes(), 128U);
    candidate.source_path = "/duplicate.png";
    candidate.duplicate = true;
    model.appendCandidate(candidate);
    EXPECT_EQ(model.rowCount(), 1);
    EXPECT_EQ(model.selectedContentHashes().front().second, std::string(64, 'a'));
    const auto generation = model.generation();
    model.setCandidates({});
    EXPECT_GT(model.generation(), generation);
}

TEST(StudioImportWorkspace, CopyDefaultRemembersCommittedRootAndHidesAllDuplicatesOnReopen)
{
    ensure_qt_core();
    init_logging("ravo-import-tests");
    QTemporaryDir directory;
    const auto source = directory.filePath("source");
    const auto destination = directory.filePath("destination");
    ASSERT_TRUE(QDir().mkpath(source));
    ASSERT_TRUE(QDir().mkpath(destination));
    ASSERT_TRUE(photo(source + "/a.png", Qt::red));
    ASSERT_TRUE(QFile::copy(source + "/a.png", source + "/renamed.png"));
    ASSERT_TRUE(photo(source + "/b.png", Qt::blue));
    const auto catalog = directory.filePath("library.sqlite");
    {
        StudioPresenter presenter;
        presenter.createCatalogFromPath(catalog);
        ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
        presenter.openImportPage();
        EXPECT_EQ(presenter.importMode(), QStringLiteral("copy"));
        presenter.setImportSourceRoot(source);
        presenter.setImportDestination(destination);
        ASSERT_TRUE(
            wait_until([&] { return !presenter.importScanActive() && presenter.importReady(); }));
        EXPECT_EQ(presenter.importCandidates()->rowCount(), 2);
        EXPECT_EQ(presenter.importDuplicateCount(), 1);
        presenter.startPlannedImport();
        ASSERT_TRUE(wait_until(
            [&] { return !presenter.importPreflightActive() && !presenter.importWorkActive(); },
            30000));
        ASSERT_TRUE(presenter.errorText().isEmpty()) << presenter.errorText().toStdString();
        EXPECT_EQ(presenter.lastImportCount(), 2);
        EXPECT_EQ(StudioImportPreferences{}.loadLastDestination().value(), destination);
        EXPECT_TRUE(QFile::exists(source + "/a.png"));
        EXPECT_TRUE(QFile::exists(source + "/renamed.png"));
        presenter.openImportPage();
        ASSERT_TRUE(wait_until([&] { return !presenter.importScanActive(); }));
        EXPECT_EQ(presenter.importCandidates()->rowCount(), 0);
        EXPECT_EQ(presenter.importDuplicateCount(), 3);
        EXPECT_FALSE(presenter.importReady());
        presenter.setImportMode(QStringLiteral("add"));
        presenter.closeImportPage();
        presenter.openImportPage();
        EXPECT_EQ(presenter.importMode(), QStringLiteral("copy"));
    }
    StudioPresenter restarted;
    restarted.createCatalogFromPath(directory.filePath("another.sqlite"));
    ASSERT_TRUE(wait_until([&] { return restarted.catalogOpen() && !restarted.busy(); }));
    restarted.openImportPage();
    EXPECT_EQ(restarted.importDestination(), destination);
    EXPECT_EQ(restarted.importDestinationFolderUrl().toLocalFile(), destination);
    restarted.setImportDestination(directory.filePath("unavailable"));
    ASSERT_TRUE(wait_until([&] { return !restarted.importDestinationError().isEmpty(); }));
    EXPECT_FALSE(restarted.importReady());
    restarted.closeImportPage();
    EXPECT_EQ(StudioImportPreferences{}.loadLastDestination().value(), destination);
}

TEST(StudioImportWorkspace, CancelPreflightAndReplaceCatalogRejectLateResults)
{
    ensure_qt_core();
    init_logging("ravo-import-tests");
    QTemporaryDir directory;
    const auto source = directory.filePath("source");
    const auto destination = directory.filePath("destination");
    ASSERT_TRUE(QDir().mkpath(source));
    ASSERT_TRUE(QDir().mkpath(destination));
    ASSERT_TRUE(photo(source + "/photo.png", Qt::red));
    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath("first.sqlite"));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.openImportPage();
    presenter.setImportSourceRoot(source);
    presenter.setImportDestination(destination);
    ASSERT_TRUE(wait_until([&] { return presenter.importReady(); }));
    presenter.startPlannedImport();
    presenter.closeImportPage();
    presenter.createCatalogFromPath(directory.filePath("second.sqlite"));
    ASSERT_TRUE(wait_until(
        [&] { return !presenter.busy() && presenter.catalogPath().endsWith("second.sqlite"); }));
    EXPECT_FALSE(presenter.importPageOpen());
    EXPECT_FALSE(presenter.importPreflightActive());
    EXPECT_FALSE(presenter.importWorkActive());
    EXPECT_EQ(presenter.importCandidates()->rowCount(), 0);
    EXPECT_TRUE(QDir(destination).entryList(QDir::Files).isEmpty());
    EXPECT_EQ(presenter.visibleCount(), 0);
}

TEST(StudioImportWorkspace, DestinationConflictKeepsWorkspaceAndDoesNotRememberDraft)
{
    ensure_qt_core();
    init_logging("ravo-import-tests");
    QTemporaryDir directory;
    const auto previous = directory.filePath("previous");
    ASSERT_TRUE(QDir().mkpath(previous));
    ASSERT_TRUE(StudioImportPreferences{}.rememberDestination(previous));
    const auto source = directory.filePath("source");
    const auto destination = directory.filePath("destination");
    ASSERT_TRUE(QDir().mkpath(source));
    ASSERT_TRUE(QDir().mkpath(destination));
    ASSERT_TRUE(photo(source + "/photo.png", Qt::red));
    ASSERT_TRUE(photo(destination + "/photo.png", Qt::blue));
    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath("library.sqlite"));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.openImportPage();
    presenter.setImportSourceRoot(source);
    presenter.setImportDestination(destination);
    ASSERT_TRUE(
        wait_until([&] { return !presenter.importScanActive() && presenter.importReady(); }));
    presenter.startPlannedImport();
    ASSERT_TRUE(wait_until([&] { return !presenter.importPreflightActive(); }));
    EXPECT_TRUE(presenter.importPageOpen());
    EXPECT_FALSE(presenter.importWorkActive());
    EXPECT_FALSE(presenter.errorText().isEmpty());
    EXPECT_EQ(presenter.visibleCount(), 0);
    EXPECT_EQ(StudioImportPreferences{}.loadLastDestination().value(), previous);
}
} // namespace ravo
