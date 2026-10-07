#include <QColorSpace>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QSettings>
#include <QTemporaryDir>
#include <future>
#include <algorithm>
#include <QElapsedTimer>
#include <gtest/gtest.h>
#include "ravo/desktop/import_candidate_list_model.h"
#include "ravo/desktop/filesystem_browser_model.h"
#include "ravo/desktop/studio_import_preferences.h"
#include "ravo/desktop/studio_presenter.h"
#include "studio_import_thumbnail_controller.h"
#include "studio_import_scan_controller.h"
#include "ravo/desktop/studio_import_workspace.h"
#include "studio_import_worker.h"
#include "ravo/desktop/studio_command_controller.h"
#include "studio_test_support.h"
#include "ravo/foundation/log.h"

namespace ravo
{
using namespace studio_test_support;
namespace testing
{
class StudioImportTestControl
{
public:
    static void pendingClassification(StudioPresenter &presenter)
    {
        const auto snapshot =
            presenter.executor_.submit([&] { return presenter.service_->library().snapshot(); });
        ASSERT_TRUE(snapshot);
        static_cast<void>(presenter.import_workspace_->scan->begin("test_classification_pending"));
        presenter.import_workspace_->scan->setCatalogRevision(snapshot.value().revision);
    }
    static bool blockThumbnails(StudioPresenter &presenter, std::shared_future<void> release)
    {
        return presenter.import_workspace_ && presenter.import_workspace_->thumbnails &&
               presenter.import_workspace_->thumbnails->executor().post([release]
                                                                        { release.wait(); });
    }
    static bool blockCatalog(StudioPresenter &presenter, std::shared_future<void> release)
    {
        return presenter.executor_.post([release] { release.wait(); });
    }
    static bool blockImportWorker(StudioPresenter &presenter, std::shared_future<void> release)
    {
        return presenter.import_workspace_->worker->executor().post([release] { release.wait(); });
    }
};
} // namespace testing
namespace
{
struct WorkerGate
{
    std::promise<void> promise;
    bool released = false;
    void release()
    {
        if (!released)
        {
            released = true;
            promise.set_value();
        }
    }
    ~WorkerGate()
    {
        release();
    }
};
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

TEST(StudioImportWorkspace, RejectedDestinationDispatchReportsClosedFilesystemOwner)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    StudioPresenter presenter;
    auto *imports = presenter.imports();
    imports->shutdown();
    imports->setImportDestination(directory.path());
    EXPECT_FALSE(imports->importDestinationError().isEmpty());
    EXPECT_EQ(presenter.errorText(), imports->importDestinationError());
    EXPECT_FALSE(imports->importReady());
    EXPECT_FALSE(imports->importDestinationPreviewActive());
}

TEST(StudioImportWorkspace, RejectedFolderDispatchClearsPendingAndReportsClosedOwner)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    StudioPresenter presenter;
    auto *imports = presenter.imports();
    auto *browser = imports->importSourceFolders();
    browser->resetWithRoots({{directory.path(), QStringLiteral("root"), true}});
    imports->shutdown();
    browser->toggleCollapsed(directory.path());
    const auto root = browser->index(0, 0);
    EXPECT_FALSE(browser->data(root, FilesystemBrowserModel::ListingPendingRole).toBool());
    EXPECT_FALSE(browser->data(root, FilesystemBrowserModel::ErrorRole).toString().isEmpty());
    EXPECT_FALSE(presenter.errorText().isEmpty());
    EXPECT_EQ(browser->rowCount(), 1);
}

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

TEST(FilesystemBrowserModelTest, NewSelectionCancelsPendingRevealAndListingErrorsStayVisible)
{
    ensure_qt_core();
    FilesystemBrowserModel model;
    model.resetWithRoots({{"/photos", "photos", true}, {"/other", "other", true}});
    quint64 generation = 0;
    QObject::connect(&model, &FilesystemBrowserModel::directoryListingRequested, &model,
                     [&](const QString &, quint64 requested) { generation = requested; });
    int revealed = 0;
    QObject::connect(&model, &FilesystemBrowserModel::folderRevealed, &model,
                     [&](int) { ++revealed; });
    model.revealFolder("/photos/2026");
    ASSERT_GT(generation, 0U);
    model.selectFolder("/other");
    model.applyChildren("/photos", generation,
                        std::vector<FilesystemFolderEntry>{{"/photos/2026", "2026", true}});
    EXPECT_EQ(model.selectedPath(), QStringLiteral("/other"));
    EXPECT_EQ(revealed, 0);
    model.toggleCollapsed("/photos");
    EXPECT_EQ(model.rowCount(), 2);
    model.toggleCollapsed("/photos");
    EXPECT_EQ(model.rowCount(), 3);
    model.revealFolder("/other/missing");
    model.applyChildren("/other", generation, make_error(ErrorCode::kIo, "unavailable"));
    EXPECT_EQ(model.data(model.index(2, 0), FilesystemBrowserModel::ErrorRole).toString(),
              QStringLiteral("unavailable"));
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

TEST(StudioImportWorkspace, RecursiveChoiceSurvivesFolderChangesAndPageReopen)
{
    ensure_qt_core();
    init_logging("ravo-import-tests");
    QTemporaryDir directory;
    const auto first = directory.filePath("first");
    const auto second = directory.filePath("second");
    for (const auto &source : {first, second})
    {
        ASSERT_TRUE(QDir().mkpath(source + "/nested"));
        ASSERT_TRUE(photo(source + "/top.png", Qt::red));
        ASSERT_TRUE(photo(source + "/nested/child.png", Qt::blue));
    }
    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath("catalog.sqlite"));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.imports()->openImportPage();
    presenter.imports()->setImportRecursive(false);
    presenter.imports()->setImportSourceRoot(first);
    ASSERT_TRUE(wait_until([&] { return !presenter.imports()->importScanActive(); }));
    EXPECT_FALSE(presenter.imports()->importRecursive());
    EXPECT_EQ(presenter.imports()->importCandidates()->rowCount(), 1);
    presenter.imports()->setImportSourceRoot(second);
    ASSERT_TRUE(wait_until([&] { return !presenter.imports()->importScanActive(); }));
    EXPECT_FALSE(presenter.imports()->importRecursive());
    EXPECT_EQ(presenter.imports()->importCandidates()->rowCount(), 1);
    presenter.imports()->closeImportPage();
    presenter.imports()->openImportPage();
    ASSERT_TRUE(wait_until([&] { return !presenter.imports()->importScanActive(); }));
    EXPECT_FALSE(presenter.imports()->importRecursive());
    EXPECT_EQ(presenter.imports()->importCandidates()->rowCount(), 1);
    presenter.imports()->setImportRecursive(true);
    ASSERT_TRUE(wait_until([&] { return !presenter.imports()->importScanActive(); }));
    EXPECT_EQ(presenter.imports()->importCandidates()->rowCount(), 2);
    // Rapidly replacing a recursive scan must not republish its nested candidates.
    presenter.imports()->setImportSourceRoot(first);
    presenter.imports()->setImportRecursive(false);
    presenter.imports()->setImportSourceRoot(second);
    ASSERT_TRUE(wait_until([&] { return !presenter.imports()->importScanActive(); }));
    EXPECT_FALSE(presenter.imports()->importRecursive());
    EXPECT_EQ(presenter.imports()->importCandidates()->rowCount(), 1);
    presenter.imports()->closeImportPage();
}

TEST(StudioImportWorkspace, HomeRecursionGuardIncludesNormalizedAndSymlinkPaths)
{
    ensure_qt_core();
    QTemporaryDir directory;
    const auto user_root = directory.filePath("user");
    const auto pictures = user_root + "/Pictures";
    ASSERT_TRUE(QDir().mkpath(pictures));
    EXPECT_FALSE(import_source_recursion(user_root, user_root, true));
    EXPECT_FALSE(import_source_recursion(user_root + "/.", user_root, true));
    EXPECT_FALSE(import_source_recursion(pictures + "/..", user_root, true));
    EXPECT_FALSE(import_source_recursion(user_root, user_root, false));
    EXPECT_FALSE(import_source_recursion(pictures, user_root, false));
    EXPECT_TRUE(import_source_recursion(pictures, user_root, true));
    EXPECT_TRUE(import_source_recursion(user_root + "-other", user_root, true));
#if defined(Q_OS_UNIX)
    const auto alias = directory.filePath("home-link");
    ASSERT_TRUE(QFile::link(user_root, alias));
    EXPECT_FALSE(import_source_recursion(alias, user_root, true));
    EXPECT_FALSE(import_source_recursion(user_root, alias, true));
    EXPECT_TRUE(import_source_recursion(alias + "/Pictures", user_root, true));
#endif
}

TEST(FilesystemBrowserModelTest, HomeRootExpandsOnceAndExternalPickerPathsRemainReachable)
{
    ensure_qt_core();
    FilesystemBrowserModel model;
    int requests = 0;
    quint64 generation = 0;
    const auto user_root = QDir::cleanPath(QDir::homePath());
    QObject::connect(&model, &FilesystemBrowserModel::directoryListingRequested, &model,
                     [&](const QString &path, quint64 next)
                     {
                         EXPECT_EQ(path, user_root);
                         ++requests;
                         generation = next;
                     });
    model.loadUserDirectory();
    ASSERT_EQ(model.rowCount(), 1);
    EXPECT_EQ(model.data(model.index(0, 0), FilesystemBrowserModel::PathRole).toString(),
              user_root);
    EXPECT_EQ(requests, 1);
    model.activateFolder(user_root);
    model.toggleCollapsed(user_root);
    EXPECT_EQ(requests, 1);
    model.applyChildren(
        user_root, generation,
        std::vector<FilesystemFolderEntry>{{user_root + "/Pictures", "Pictures", true}});
    EXPECT_EQ(model.rowCount(), 2);
    model.toggleCollapsed(user_root);
    EXPECT_EQ(model.rowCount(), 1);
    model.activateFolder(user_root);
    EXPECT_EQ(model.rowCount(), 2);
    EXPECT_EQ(model.selectedPath(), user_root);
    EXPECT_EQ(requests, 1);
    // This model test has no filesystem listing worker. Use an explicitly
    // out-of-root path: Windows' real temp directory lives inside Home.
    const auto external = user_root + QStringLiteral("-external-picker-fixture");
    model.revealFolder(external);
    EXPECT_EQ(model.selectedPath(), external);
    EXPECT_EQ(model.data(model.index(0, 0), FilesystemBrowserModel::PathRole).toString(),
              user_root);
    EXPECT_EQ(model.data(model.index(model.rowCount() - 1, 0), FilesystemBrowserModel::PathRole)
                  .toString(),
              external);
}

TEST(StudioImportWorkspace, BothTreesStartAtHomeAndRepeatedDestinationSelectionKeepsExpansion)
{
    ensure_qt_core();
    init_logging("ravo-import-tests");
    QTemporaryDir directory;
    const auto destination = directory.filePath("destination");
    ASSERT_TRUE(QDir().mkpath(destination + "/child"));
    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath("catalog.sqlite"));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.imports()->openImportPage();
    for (auto *model : {presenter.imports()->importSourceFolders(),
                        presenter.imports()->importDestinationFolders()})
        EXPECT_EQ(model->data(model->index(0, 0), FilesystemBrowserModel::PathRole).toString(),
                  QDir::cleanPath(QDir::homePath()));
    auto *model = presenter.imports()->importDestinationFolders();
    presenter.imports()->setImportDestination(destination);
    // A picker path inside Home needs asynchronous ancestor discovery before
    // its row can be activated, just as it does in the real folder tree.
    ASSERT_TRUE(wait_until(
        [&]
        {
            for (int row = 0; row < model->rowCount(); ++row)
                if (model->data(model->index(row, 0), FilesystemBrowserModel::PathRole)
                        .toString() == destination)
                    return true;
            return false;
        }));
    model->activateFolder(destination);
    const auto child_visible = [&]
    {
        for (int row = 0; row < model->rowCount(); ++row)
            if (model->data(model->index(row, 0), FilesystemBrowserModel::PathRole).toString() ==
                destination + "/child")
                return true;
        return false;
    };
    ASSERT_TRUE(wait_until(child_visible));
    ASSERT_TRUE(presenter.imports()->importDestinationError().isEmpty());
    presenter.imports()->setImportDestination(destination);
    EXPECT_TRUE(child_visible());
    EXPECT_EQ(model->selectedPath(), destination);
    presenter.imports()->closeImportPage();
}

TEST(StudioImportWorkspace, SourcePreferenceValidatesPathsAndRemovesUnavailableFolders)
{
    ensure_qt_core();
    QTemporaryDir directory;
    const auto source = directory.filePath("unavailable");
    ASSERT_TRUE(QDir().mkpath(source));
    StudioImportPreferences preferences;
    ASSERT_TRUE(preferences.rememberSource(source));
    EXPECT_EQ(StudioImportPreferences{}.loadLastSource().value(), source);
    EXPECT_FALSE(preferences.rememberSource(QStringLiteral("relative/path")));
    EXPECT_EQ(preferences.loadLastSource().value(), source);
    ASSERT_TRUE(QDir().rmdir(source));
    const auto unavailable = preferences.loadLastSource();
    ASSERT_FALSE(unavailable);
    EXPECT_EQ(unavailable.error().context.at("reason"), "unavailable_import_source_preference");
    EXPECT_TRUE(preferences.loadLastSource().value().isEmpty());
    QSettings settings;
    settings.setValue(QStringLiteral("desktop/import/lastSource"), 42);
    settings.sync();
    auto invalid = preferences.loadLastSource();
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, ErrorCode::kValidation);
    EXPECT_TRUE(preferences.loadLastSource().value().isEmpty());
}

TEST(StudioImportWorkspace, SourceSelectionPersistsWithoutImportAndRevealsAfterRestart)
{
    ensure_qt_core();
    init_logging("ravo-import-tests");
    QTemporaryDir directory;
    const auto source = directory.filePath("Pictures/2026/09");
    ASSERT_TRUE(QDir().mkpath(source));
    ASSERT_TRUE(photo(source + "/photo.png", Qt::red));
    const auto expect_revealed = [&](StudioPresenter &presenter)
    {
        auto *model = presenter.imports()->importSourceFolders();
        EXPECT_TRUE(wait_until(
            [&]
            {
                if (presenter.imports()->importScanActive())
                    return false;
                for (int row = 0; row < model->rowCount(); ++row)
                    if (model->data(model->index(row, 0), FilesystemBrowserModel::SelectedRole)
                            .toBool())
                        return model->data(model->index(row, 0), FilesystemBrowserModel::PathRole)
                                   .toString() == source;
                return false;
            }));
        EXPECT_EQ(presenter.imports()->importSourceRoot(), source);
        EXPECT_EQ(presenter.imports()->importSourceFolderUrl().toLocalFile(), source);
        EXPECT_EQ(presenter.imports()->importCandidates()->rowCount(), 1);
    };
    {
        StudioPresenter presenter;
        presenter.createCatalogFromPath(directory.filePath("first.sqlite"));
        ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
        presenter.imports()->openImportPage();
        presenter.imports()->setImportSourceRoot(source);
        expect_revealed(presenter);
        EXPECT_EQ(StudioImportPreferences{}.loadLastSource().value(), source);
        presenter.imports()->closeImportPage();
        presenter.imports()->openImportPage();
        expect_revealed(presenter);
        presenter.imports()->closeImportPage();
    }
    StudioPresenter restarted;
    restarted.createCatalogFromPath(directory.filePath("second.sqlite"));
    ASSERT_TRUE(wait_until([&] { return restarted.catalogOpen() && !restarted.busy(); }));
    int reveal_count = 0;
    QObject::connect(restarted.imports()->importSourceFolders(),
                     &FilesystemBrowserModel::folderRevealed, &restarted,
                     [&](int) { ++reveal_count; });
    restarted.imports()->openImportPage();
    expect_revealed(restarted);
    EXPECT_GT(reveal_count, 0);
    restarted.imports()->closeImportPage();
    ASSERT_TRUE(QFile::remove(source + "/photo.png"));
    ASSERT_TRUE(QDir().rmdir(source));
    restarted.imports()->openImportPage();
    ASSERT_TRUE(wait_until([&] { return !restarted.imports()->importScanActive(); }));
    EXPECT_TRUE(restarted.imports()->importSourceRoot().isEmpty());
    EXPECT_FALSE(restarted.errorText().isEmpty());
    EXPECT_FALSE(restarted.imports()->importReady());
    restarted.imports()->closeImportPage();
    restarted.imports()->openImportPage();
    EXPECT_TRUE(restarted.imports()->importSourceRoot().isEmpty());
    EXPECT_TRUE(restarted.errorText().isEmpty());
    restarted.imports()->closeImportPage();
}

TEST(StudioImportWorkspace, SourcePreferenceWriteFailureRetainsPreviousPath)
{
    ensure_qt_core();
    QTemporaryDir directory;
    StudioImportPreferences preferences;
    ASSERT_TRUE(preferences.rememberSource(directory.path()));
    {
        const auto settings_directory = QFileInfo(QSettings{}.fileName()).absolutePath();
        const auto saved = directory.filePath("saved-settings");
        ASSERT_TRUE(QDir().rename(settings_directory, saved));
        BlockedSettingsDirectory restore{settings_directory, saved};
        QFile blocker(settings_directory);
        ASSERT_TRUE(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        EXPECT_FALSE(preferences.rememberSource(directory.filePath("next")));
    }
    auto restored = preferences.loadLastSource();
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_EQ(restored.value(), directory.path());
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
        presenter.imports()->openImportPage();
        presenter.imports()->setImportSourceRoot(source);
        presenter.imports()->setImportDestination(destination);
        ASSERT_TRUE(wait_until([&] { return presenter.imports()->importReady(); }));
        presenter.imports()->startPlannedImport();
        ASSERT_TRUE(wait_until(
            [&]
            {
                return !presenter.imports()->importPreflightActive() &&
                       !presenter.imports()->importWorkActive();
            },
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
    candidate.duplicate_reason = "batch_content";
    model.appendCandidate(candidate);
    EXPECT_EQ(model.rowCount(), 2);
    EXPECT_FALSE(model.data(model.index(1, 0), ImportCandidateListModel::EligibleRole).toBool());
    EXPECT_FALSE(model.data(model.index(1, 0), ImportCandidateListModel::SelectedRole).toBool());
    model.toggleSelected(1);
    model.applyCheck(1);
    model.highlightExclusive(1);
    model.highlightToggle(1);
    EXPECT_FALSE(model.highlighted(1));
    model.highlightRange(0, 1, false);
    EXPECT_TRUE(model.highlighted(0));
    EXPECT_FALSE(model.highlighted(1));
    model.highlightAll();
    EXPECT_FALSE(model.highlighted(1));
    model.selectRange(0, 1, false);
    model.setAllSelected(true);
    EXPECT_EQ(model.selectedCount(), 1);
    EXPECT_EQ(model.selectedPaths(), QStringList{QStringLiteral("/photo.png")});
    EXPECT_EQ(model.selectedBytes(), 128U);
    // A single-path inspection has no batch/content duplicate context.
    candidate.duplicate = false;
    candidate.duplicate_reason.clear();
    model.updateCandidate(1, candidate);
    EXPECT_TRUE(model.data(model.index(1, 0), ImportCandidateListModel::DuplicateRole).toBool());
    EXPECT_FALSE(model.data(model.index(1, 0), ImportCandidateListModel::EligibleRole).toBool());
    model.setAllSelected(true);
    EXPECT_EQ(model.selectedCount(), 1);
    model.highlightAll();
    candidate.source_path = "/photo.png";
    candidate.duplicate = true;
    model.updateCandidate(0, candidate);
    EXPECT_FALSE(model.highlighted(0));
    EXPECT_EQ(model.selectedCount(), 0);
    model.setCandidates({});
    candidate.source_path = "/photo.png";
    candidate.duplicate = false;
    model.appendCandidate(candidate);
    EXPECT_EQ(model.selectedContentHashes().front().second, std::string(64, 'a'));
    const auto generation = model.generation();
    model.setCandidates({});
    EXPECT_GT(model.generation(), generation);
}

TEST(StudioImportWorkspace, CopyDefaultRemembersCommittedRootAndHidesAllDuplicatesOnReopen)
{
    // Preserve the established test identity; duplicate rows now stay visible and disabled.
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
    const auto source_hash = [&]
    {
        QByteArray bytes;
        for (const auto *name : {"/a.png", "/b.png", "/renamed.png"})
        {
            QFile file(source + QString::fromLatin1(name));
            if (!file.open(QIODevice::ReadOnly))
                return QByteArray{};
            bytes.append(file.readAll());
        }
        return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    };
    const auto original_hash = source_hash();
    ASSERT_FALSE(original_hash.isEmpty());
    const auto catalog = directory.filePath("library.sqlite");
    const auto open_fixture_import = [&](StudioPresenter &owner)
    {
        owner.imports()->openImportPage();
        // Keep this behavioral test independent of the host's temporary-directory size.
        owner.imports()->importSourceFolders()->resetWithRoots(
            {{directory.path(), QStringLiteral("fixture"), true}});
        owner.imports()->importDestinationFolders()->resetWithRoots(
            {{directory.path(), QStringLiteral("fixture"), true}});
        if (!owner.imports()->importSourceRoot().isEmpty())
            owner.imports()->importSourceFolders()->revealFolder(
                owner.imports()->importSourceRoot());
        if (!owner.imports()->importDestination().isEmpty())
            owner.imports()->importDestinationFolders()->revealFolder(
                owner.imports()->importDestination());
    };
    {
        StudioPresenter presenter;
        presenter.createCatalogFromPath(catalog);
        ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
        open_fixture_import(presenter);
        EXPECT_EQ(presenter.imports()->importMode(), QStringLiteral("copy"));
        presenter.imports()->setImportSourceRoot(source);
        presenter.imports()->setImportDestination(destination);
        ASSERT_TRUE(wait_until(
            [&]
            {
                return !presenter.imports()->importScanActive() &&
                       presenter.imports()->importReady();
            }));
        EXPECT_EQ(presenter.imports()->importCandidates()->rowCount(), 3);
        EXPECT_EQ(presenter.imports()->importCandidates()->selectedCount(), 2);
        EXPECT_EQ(presenter.imports()->importDuplicateCount(), 1);
        const auto complete_thumbnails = [&]
        {
            auto *model = presenter.imports()->importCandidates();
            const auto generation = model->generation();
            for (int row = 0; row < model->rowCount(); ++row)
                presenter.imports()->ensureImportThumbnail(row);
            EXPECT_TRUE(wait_until(
                [&]
                {
                    for (int row = 0; row < model->rowCount(); ++row)
                        if (!model->inspected(row) || model->thumbnail(row).isNull())
                            return false;
                    return true;
                }));
            EXPECT_EQ(model->generation(), generation);
        };
        complete_thumbnails();
        EXPECT_EQ(presenter.imports()->importCandidates()->selectedCount(), 2);
        presenter.imports()->startPlannedImport();
        ASSERT_TRUE(wait_until(
            [&]
            {
                return !presenter.imports()->importPreflightActive() &&
                       !presenter.imports()->importWorkActive();
            },
            30000));
        ASSERT_TRUE(presenter.errorText().isEmpty()) << presenter.errorText().toStdString();
        EXPECT_EQ(presenter.lastImportCount(), 2);
        EXPECT_EQ(StudioImportPreferences{}.loadLastDestination().value(), destination);
        EXPECT_TRUE(QFile::exists(source + "/a.png"));
        EXPECT_TRUE(QFile::exists(source + "/renamed.png"));
        open_fixture_import(presenter);
        ASSERT_TRUE(wait_until([&] { return !presenter.imports()->importScanActive(); }));
        EXPECT_EQ(presenter.imports()->importCandidates()->rowCount(), 3);
        EXPECT_EQ(presenter.imports()->importDuplicateCount(), 3);
        complete_thumbnails();
        auto *candidates = presenter.imports()->importCandidates();
        candidates->setAllSelected(true);
        candidates->highlightAll();
        EXPECT_EQ(candidates->selectedCount(), 0);
        for (int row = 0; row < candidates->rowCount(); ++row)
        {
            EXPECT_TRUE(
                candidates->data(candidates->index(row, 0), ImportCandidateListModel::DuplicateRole)
                    .toBool());
            EXPECT_FALSE(candidates->highlighted(row));
        }
        EXPECT_FALSE(presenter.imports()->importReady());
        // Direct catalog-URI duplicates must also load thumbnails without rescanning forever.
        presenter.imports()->setImportSourceRoot(destination);
        ASSERT_TRUE(wait_until([&] { return !presenter.imports()->importScanActive(); }));
        EXPECT_EQ(candidates->rowCount(), 2);
        EXPECT_EQ(presenter.imports()->importDuplicateCount(), 2);
        complete_thumbnails();
        EXPECT_EQ(candidates->selectedCount(), 0);
        EXPECT_FALSE(presenter.imports()->importReady());
        EXPECT_EQ(source_hash(), original_hash);
        presenter.imports()->setImportMode(QStringLiteral("add"));
        presenter.imports()->closeImportPage();
        open_fixture_import(presenter);
        EXPECT_EQ(presenter.imports()->importMode(), QStringLiteral("copy"));
    }
    StudioPresenter restarted;
    restarted.createCatalogFromPath(directory.filePath("another.sqlite"));
    ASSERT_TRUE(wait_until([&] { return restarted.catalogOpen() && !restarted.busy(); }));
    open_fixture_import(restarted);
    EXPECT_EQ(restarted.imports()->importDestination(), destination);
    EXPECT_EQ(restarted.imports()->importDestinationFolderUrl().toLocalFile(), destination);
    restarted.imports()->setImportDestination(directory.filePath("unavailable"));
    ASSERT_TRUE(
        wait_until([&] { return !restarted.imports()->importDestinationError().isEmpty(); }));
    EXPECT_FALSE(restarted.imports()->importReady());
    restarted.imports()->closeImportPage();
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
    presenter.imports()->openImportPage();
    presenter.imports()->setImportSourceRoot(source);
    presenter.imports()->setImportDestination(destination);
    ASSERT_TRUE(wait_until([&] { return presenter.imports()->importReady(); }));
    presenter.imports()->startPlannedImport();
    presenter.imports()->closeImportPage();
    presenter.createCatalogFromPath(directory.filePath("second.sqlite"));
    ASSERT_TRUE(wait_until(
        [&] { return !presenter.busy() && presenter.catalogPath().endsWith("second.sqlite"); }));
    EXPECT_FALSE(presenter.imports()->importPageOpen());
    EXPECT_FALSE(presenter.imports()->importPreflightActive());
    EXPECT_FALSE(presenter.imports()->importWorkActive());
    EXPECT_EQ(presenter.imports()->importCandidates()->rowCount(), 0);
    EXPECT_TRUE(QDir(destination).entryList(QDir::Files).isEmpty());
    EXPECT_EQ(presenter.visibleCount(), 0);
}

TEST(StudioImportWorkspace, DestinationConflictReturnsToGalleryAndDoesNotRememberDraft)
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
    presenter.imports()->openImportPage();
    presenter.imports()->setImportSourceRoot(source);
    presenter.imports()->setImportDestination(destination);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.imports()->importScanActive() && presenter.imports()->importReady();
        }));
    presenter.imports()->startPlannedImport();
    ASSERT_TRUE(wait_until([&] { return !presenter.imports()->importPreflightActive(); }));
    EXPECT_FALSE(presenter.imports()->importPageOpen());
    EXPECT_FALSE(presenter.imports()->importWorkActive());
    EXPECT_FALSE(presenter.errorText().isEmpty());
    EXPECT_EQ(presenter.visibleCount(), 0);
    EXPECT_EQ(StudioImportPreferences{}.loadLastDestination().value(), previous);
}
TEST(StudioImportWorkspace, PublishesCompletePlaceholdersAndDecodesInRowOrderOutsideCatalogQueue)
{
    ensure_qt_core();
    init_logging("ravo-import-tests");
    QTemporaryDir directory;
    const auto source = directory.filePath("source");
    ASSERT_TRUE(QDir().mkpath(source));
    constexpr int count = 96; // More than the old queue's silently dropped 64 rows.
    for (int row = 0; row < count; ++row)
        ASSERT_TRUE(photo(source + QStringLiteral("/%1.png").arg(row, 3, 10, QLatin1Char('0')),
                          QColor(row, 80, 120)));
    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath("catalog.sqlite"));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.imports()->openImportPage();
    presenter.imports()->setImportMode(QStringLiteral("add"));
    WorkerGate thumbnails;
    ASSERT_TRUE(testing::StudioImportTestControl::blockThumbnails(
        presenter, thumbnails.promise.get_future().share()));
    auto *model = presenter.imports()->importCandidates();
    bool placeholders_seen = false;
    QObject::connect(model, &QAbstractItemModel::modelReset, &presenter,
                     [&]
                     {
                         if (model->rowCount() != count)
                             return;
                         placeholders_seen = true;
                         EXPECT_TRUE(presenter.imports()->importScanActive());
                         EXPECT_EQ(presenter.imports()->importScanCompleted(), 0);
                         for (int row = 0; row < count; ++row)
                         {
                             EXPECT_FALSE(model->sourcePath(row).isEmpty());
                             EXPECT_TRUE(model->thumbnail(row).isNull());
                         }
                         // Classification must not undo an Uncheck All made on placeholders.
                         model->setAllSelected(false);
                     });
    presenter.imports()->setImportSourceRoot(source);
    ASSERT_TRUE(wait_until([&] { return !presenter.imports()->importScanActive(); }));
    EXPECT_TRUE(placeholders_seen);
    EXPECT_EQ(model->selectedCount(), 0);
    model->setAllSelected(true);
    ASSERT_TRUE(presenter.imports()->importReady());
    WorkerGate catalog;
    ASSERT_TRUE(testing::StudioImportTestControl::blockCatalog(
        presenter, catalog.promise.get_future().share()));
    std::vector<int> completed;
    QObject::connect(model, &QAbstractItemModel::dataChanged, &presenter,
                     [&](const QModelIndex &first, const QModelIndex &, const QList<int> &roles)
                     {
                         if (roles.contains(ImportCandidateListModel::ThumbnailUrlRole) &&
                             !model->thumbnail(first.row()).isNull())
                             completed.push_back(first.row());
                     });
    // Delegate creation order is not a priority policy.
    for (int row = count - 1; row >= 0; --row)
        presenter.imports()->ensureImportThumbnail(row);
    thumbnails.release();
    ASSERT_TRUE(wait_until([&] { return completed.size() == count; }, 30000));
    for (int row = 0; row < count; ++row)
        EXPECT_EQ(completed[static_cast<std::size_t>(row)], row);
    EXPECT_TRUE(presenter.imports()->importReady());
    catalog.release();
}

TEST(StudioImportWorkspace, ImportStartsWithPendingClassificationAndNoThumbnails)
{
    ensure_qt_core();
    init_logging("ravo-import-tests");
    QTemporaryDir directory;
    const auto source = directory.filePath("source");
    ASSERT_TRUE(QDir().mkpath(source));
    ASSERT_TRUE(photo(source + "/a.png", Qt::red));
    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath("catalog.sqlite"));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.imports()->openImportPage();
    presenter.imports()->setImportMode("add");
    presenter.imports()->setImportSourceRoot(source);
    ASSERT_TRUE(wait_until([&] { return !presenter.imports()->importScanActive(); }));
    WorkerGate thumbnails;
    ASSERT_TRUE(testing::StudioImportTestControl::blockThumbnails(
        presenter, thumbnails.promise.get_future().share()));
    ImportCandidate candidate;
    candidate.source_path = (source + "/a.png").toStdString();
    candidate.display_name = "a.png";
    presenter.imports()->importCandidates()->setCandidates({candidate});
    testing::StudioImportTestControl::pendingClassification(presenter);
    ASSERT_TRUE(presenter.imports()->importScanActive());
    ASSERT_TRUE(presenter.imports()->importReady());
    EXPECT_TRUE(presenter.imports()->importCandidates()->selectedContentHashes().empty());
    const auto destination = directory.filePath("destination");
    ASSERT_TRUE(QDir().mkpath(destination));
    WorkerGate catalog;
    ASSERT_TRUE(testing::StudioImportTestControl::blockCatalog(
        presenter, catalog.promise.get_future().share()));
    presenter.imports()->setImportMode("copy");
    presenter.imports()->setImportDestination(destination);
    ASSERT_TRUE(wait_until([&] { return presenter.imports()->importReady(); }));
    EXPECT_TRUE(presenter.imports()->importScanActive());
    presenter.imports()->startPlannedImport();
    EXPECT_FALSE(presenter.imports()->importPageOpen());
    EXPECT_EQ(presenter.browseMode(), QStringLiteral("grid"));
    EXPECT_TRUE(presenter.imports()->importPreflightActive());
    EXPECT_TRUE(presenter.imports()->importWorkActive());
    catalog.release();
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.imports()->importPreflightActive() &&
                   !presenter.imports()->importWorkActive();
        },
        30000));
    EXPECT_EQ(presenter.lastImportCount(), 1);
    EXPECT_TRUE(presenter.imports()->importCandidates()->thumbnail(0).isNull());
    thumbnails.release();
}

TEST(StudioImportWorkspace, GalleryPreflightCancelAndFailureReleaseImportState)
{
    ensure_qt_core();
    init_logging("ravo-import-tests");
    for (const bool cancel : {true, false})
    {
        QTemporaryDir directory;
        const auto source = directory.filePath("source");
        const auto destination = directory.filePath("destination");
        ASSERT_TRUE(QDir().mkpath(source));
        ASSERT_TRUE(QDir().mkpath(destination));
        ASSERT_TRUE(photo(source + "/a.png", Qt::red));
        StudioPresenter presenter;
        presenter.createCatalogFromPath(directory.filePath("catalog.sqlite"));
        ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
        presenter.imports()->openImportPage();
        presenter.imports()->setImportSourceRoot(source);
        presenter.imports()->setImportDestination(destination);
        ASSERT_TRUE(wait_until(
            [&]
            {
                return presenter.imports()->importReady() &&
                       !presenter.imports()->importScanActive();
            }));
        WorkerGate catalog;
        ASSERT_TRUE(testing::StudioImportTestControl::blockImportWorker(
            presenter, catalog.promise.get_future().share()));
        presenter.imports()->startPlannedImport();
        EXPECT_FALSE(presenter.imports()->importPageOpen());
        EXPECT_TRUE(presenter.imports()->importPreflightActive());
        EXPECT_TRUE(presenter.imports()->importWorkActive());
        if (cancel)
            presenter.cancelCatalogOperation();
        else
            ASSERT_TRUE(QFile::remove(source + "/a.png"));
        catalog.release();
        ASSERT_TRUE(wait_until(
            [&]
            {
                return !presenter.imports()->importPreflightActive() &&
                       !presenter.imports()->importWorkActive();
            },
            30000));
        EXPECT_FALSE(presenter.imports()->importPageOpen());
        EXPECT_EQ(presenter.libraryTotal(), 0);
        EXPECT_TRUE(QDir(destination).entryList(QDir::Files).isEmpty());
        EXPECT_EQ(presenter.errorText().isEmpty(), cancel);
        presenter.imports()->openImportPage();
        EXPECT_TRUE(presenter.imports()->importPageOpen());
    }
}

TEST(StudioImportWorkspace, ImportAndSelectAllDoNotWaitForWorkspaceThumbnails)
{
    ensure_qt_core();
    init_logging("ravo-import-tests");
    QTemporaryDir directory;
    const auto source = directory.filePath("source");
    ASSERT_TRUE(QDir().mkpath(source));
    ASSERT_TRUE(photo(source + "/a.png", Qt::red));
    ASSERT_TRUE(QFile::copy(source + "/a.png", source + "/duplicate.png"));
    ASSERT_TRUE(photo(source + "/b.png", Qt::blue));
    StudioPresenter presenter;
    StudioCommandController controller(presenter);
    presenter.createCatalogFromPath(directory.filePath("catalog.sqlite"));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.imports()->openImportPage();
    presenter.imports()->setImportMode(QStringLiteral("add"));
    WorkerGate thumbnails;
    ASSERT_TRUE(testing::StudioImportTestControl::blockThumbnails(
        presenter, thumbnails.promise.get_future().share()));
    presenter.imports()->setImportSourceRoot(source);
    ASSERT_TRUE(wait_until([&] { return presenter.imports()->importReady(); }));
    auto *model = presenter.imports()->importCandidates();
    ASSERT_EQ(model->rowCount(), 3);
    ASSERT_TRUE(wait_until([&] { return !presenter.imports()->importScanActive(); }));
    presenter.imports()->ensureImportThumbnail(0);
    const QString action = QStringLiteral("studio.photo.select_all");
    int select_all_shortcuts = 0;
    for (const auto &value : controller.shortcutEntries())
        if (value.toMap().value("actionId").toString() == action)
        {
            ++select_all_shortcuts;
            EXPECT_TRUE(value.toMap().value("enabled").toBool());
        }
    EXPECT_EQ(select_all_shortcuts, 1);
    EXPECT_TRUE(
        controller.executeAction(action, QStringLiteral("keyboard")).value("accepted").toBool());
    EXPECT_TRUE(model->highlighted(0));
    EXPECT_TRUE(model->highlighted(1));
    EXPECT_FALSE(model->highlighted(2));
    EXPECT_EQ(presenter.selectedCount(), 0); // No Gallery selection was changed.
    controller.setTextInputActive(true);
    for (const auto &value : controller.shortcutEntries())
        if (value.toMap().value("actionId").toString() == action)
            EXPECT_FALSE(value.toMap().value("enabled").toBool());
    controller.setTextInputActive(false);
    model->applyCheck(0);
    EXPECT_EQ(model->selectedCount(), 0);
    model->applyCheck(0);
    EXPECT_EQ(model->selectedCount(), 2);
    presenter.imports()->startPlannedImport();
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.imports()->importPreflightActive() &&
                   !presenter.imports()->importWorkActive();
        },
        30000));
    EXPECT_EQ(presenter.lastImportCount(), 2U);
    EXPECT_TRUE(model->thumbnail(0).isNull());
    thumbnails.release();
}
TEST(StudioImportWorkspace, ThumbnailCacheIsBoundedAndClassificationPreservesCompletion)
{
    ensure_qt_core();
    ImportCandidateListModel model;
    std::vector<ImportCandidate> candidates(300);
    for (int row = 0; row < 300; ++row)
        candidates[static_cast<std::size_t>(row)].source_path = std::to_string(row);
    model.setCandidates(candidates);
    QObject::connect(
        &model, &QAbstractItemModel::dataChanged, &model,
        [&](const QModelIndex &, const QModelIndex &, const QList<int> &)
        {
            int checked = 0;
            for (int row = 0; row < model.rowCount(); ++row)
                checked += model.data(model.index(row, 0), ImportCandidateListModel::SelectedRole)
                               .toBool();
            EXPECT_EQ(model.selectedCount(), checked);
        });
    QImage image(16, 16, QImage::Format_RGB888);
    image.fill(Qt::red);
    for (int row = 0; row < 300; ++row)
        model.finishThumbnail(row, image);
    int retained = 0;
    for (int row = 0; row < 300; ++row)
        retained += !model.thumbnail(row).isNull();
    EXPECT_EQ(retained, 256);
    EXPECT_FALSE(model.inspected(0));
    candidates.back().duplicate = true;
    model.applyScanBatch(0, candidates);
    EXPECT_TRUE(model.inspected(299));
    EXPECT_FALSE(model.thumbnail(299).isNull());
    EXPECT_EQ(model.selectedCount(), 299);
    model.highlightAll();
    model.applyCheck(0);
    EXPECT_EQ(model.selectedCount(), 0);
    model.applyCheck(0);
    EXPECT_EQ(model.selectedCount(), 299);
    model.finishThumbnail(298, {}, make_error(ErrorCode::kIo, "thumbnail failed"));
    EXPECT_EQ(model.selectedCount(), 299); // Preview failure is not import eligibility.
    EXPECT_EQ(model.data(model.index(298, 0), ImportCandidateListModel::ErrorRole).toString(),
              QStringLiteral("thumbnail failed"));
    model.setCandidates({});
    EXPECT_EQ(model.selectedCount(), 0);
    EXPECT_EQ(model.selectedBytes(), 0U);
    model.setAllSelected(false);
    model.setCandidates(candidates, true);
    EXPECT_EQ(model.selectedCount(), 0);
}

TEST(StudioImportWorkspace, ThumbnailResultsCannotCrossSourceReplacementOrPageClose)
{
    ensure_qt_core();
    init_logging("ravo-import-tests");
    QTemporaryDir directory;
    const auto first = directory.filePath("first");
    const auto second = directory.filePath("second");
    ASSERT_TRUE(QDir().mkpath(first));
    ASSERT_TRUE(QDir().mkpath(second));
    ASSERT_TRUE(photo(first + "/same.png", Qt::red));
    ASSERT_TRUE(photo(second + "/same.png", Qt::blue));
    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath("catalog.sqlite"));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.imports()->openImportPage();
    WorkerGate thumbnails;
    ASSERT_TRUE(testing::StudioImportTestControl::blockThumbnails(
        presenter, thumbnails.promise.get_future().share()));
    presenter.imports()->setImportSourceRoot(first);
    ASSERT_TRUE(wait_until([&] { return !presenter.imports()->importScanActive(); }));
    presenter.imports()->ensureImportThumbnail(0);
    QCoreApplication::processEvents();
    presenter.imports()->setImportSourceRoot(second);
    ASSERT_TRUE(wait_until([&] { return !presenter.imports()->importScanActive(); }));
    presenter.imports()->ensureImportThumbnail(0);
    thumbnails.release();
    auto *model = presenter.imports()->importCandidates();
    ASSERT_TRUE(wait_until([&] { return model->inspected(0); }));
    ASSERT_FALSE(model->thumbnail(0).isNull());
    EXPECT_EQ(model->thumbnail(0).pixelColor(0, 0), QColor(Qt::blue));
    EXPECT_EQ(model->sourcePath(0), QFileInfo(second + "/same.png").canonicalFilePath());
    WorkerGate closing;
    ASSERT_TRUE(testing::StudioImportTestControl::blockThumbnails(
        presenter, closing.promise.get_future().share()));
    presenter.imports()->setImportSourceRoot(second);
    ASSERT_TRUE(wait_until([&] { return !presenter.imports()->importScanActive(); }));
    presenter.imports()->ensureImportThumbnail(0);
    QCoreApplication::processEvents();
    presenter.imports()->closeImportPage();
    closing.release();
    QCoreApplication::processEvents();
    EXPECT_EQ(model->rowCount(), 0);
    EXPECT_FALSE(presenter.imports()->importPageOpen());
}

TEST(StudioImportWorkspace, RealSourceProgressProbe)
{
    const auto source = qEnvironmentVariable("RAVO_IMPORT_SCAN_SOURCE");
    if (source.isEmpty())
        GTEST_SKIP() << "Set RAVO_IMPORT_SCAN_SOURCE to an explicit read-only source directory";
    ensure_qt_core();
    init_logging("ravo-import-probe");
    ASSERT_TRUE(QFileInfo(source).isDir());
    QTemporaryDir directory;
    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath("probe.sqlite"));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    // Avoid scanning a preference restored by another test.
    ASSERT_TRUE(StudioImportPreferences{}.rememberSource(directory.path()));
    presenter.imports()->openImportPage();
    presenter.imports()->setImportMode(QStringLiteral("add"));
    QElapsedTimer elapsed;
    elapsed.start();
    qint64 placeholders_ms = -1;
    qint64 first_image_ms = -1;
    qint64 scan_ms = -1;
    int requested = 0;
    auto *model = presenter.imports()->importCandidates();
    QObject::connect(model, &QAbstractItemModel::modelReset, &presenter,
                     [&]
                     {
                         if (!model->rowCount())
                             return;
                         placeholders_ms = elapsed.elapsed();
                         requested = std::min(model->rowCount(), 32);
                         for (int row = requested - 1; row >= 0; --row)
                             presenter.imports()->ensureImportThumbnail(row);
                     });
    std::vector<int> images;
    QObject::connect(model, &QAbstractItemModel::dataChanged, &presenter,
                     [&](const QModelIndex &first, const QModelIndex &, const QList<int> &roles)
                     {
                         if (roles.contains(ImportCandidateListModel::ThumbnailUrlRole) &&
                             !model->thumbnail(first.row()).isNull())
                         {
                             if (first_image_ms < 0)
                                 first_image_ms = elapsed.elapsed();
                             images.push_back(first.row());
                         }
                     });
    QObject::connect(presenter.imports(), &StudioImportWorkspace::importPageChanged, &presenter,
                     [&]
                     {
                         if (!presenter.imports()->importScanActive() && placeholders_ms >= 0 &&
                             scan_ms < 0)
                             scan_ms = elapsed.elapsed();
                     });
    presenter.imports()->setImportSourceRoot(source);
    const bool viewport_ready = wait_until(
        [&]
        {
            if (requested == 0)
                return false;
            for (int row = 0; row < requested; ++row)
                if (!model->inspected(row))
                    return false;
            return true;
        },
        30000);
    RecordProperty("candidates", model->rowCount());
    RecordProperty("placeholders_ms", std::to_string(placeholders_ms));
    RecordProperty("first_image_ms", std::to_string(first_image_ms));
    RecordProperty("scan_ms", std::to_string(scan_ms));
    RecordProperty("scan_completed", presenter.imports()->importScanCompleted());
    RecordProperty("scan_still_active", presenter.imports()->importScanActive() ? 1 : 0);
    RecordProperty("decoded_images", static_cast<int>(images.size()));
    EXPECT_TRUE(viewport_ready);
    EXPECT_TRUE(presenter.errorText().isEmpty()) << presenter.errorText().toStdString();
    EXPECT_TRUE(std::is_sorted(images.begin(), images.end()));
    EXPECT_GE(first_image_ms, 0);
    EXPECT_EQ(presenter.visibleCount(), 0); // No import is executed by this probe.
    presenter.imports()->closeImportPage();
}
} // namespace ravo
