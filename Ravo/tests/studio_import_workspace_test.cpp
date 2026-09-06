#include <QColorSpace>
#include <QCryptographicHash>
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
    presenter.openImportPage();
    presenter.setImportRecursive(false);
    presenter.setImportSourceRoot(first);
    ASSERT_TRUE(wait_until([&] { return !presenter.importScanActive(); }));
    EXPECT_FALSE(presenter.importRecursive());
    EXPECT_EQ(presenter.importCandidates()->rowCount(), 1);
    presenter.setImportSourceRoot(second);
    ASSERT_TRUE(wait_until([&] { return !presenter.importScanActive(); }));
    EXPECT_FALSE(presenter.importRecursive());
    EXPECT_EQ(presenter.importCandidates()->rowCount(), 1);
    presenter.closeImportPage();
    presenter.openImportPage();
    ASSERT_TRUE(wait_until([&] { return !presenter.importScanActive(); }));
    EXPECT_FALSE(presenter.importRecursive());
    EXPECT_EQ(presenter.importCandidates()->rowCount(), 1);
    presenter.setImportRecursive(true);
    ASSERT_TRUE(wait_until([&] { return !presenter.importScanActive(); }));
    EXPECT_EQ(presenter.importCandidates()->rowCount(), 2);
    // Rapidly replacing a recursive scan must not republish its nested candidates.
    presenter.setImportSourceRoot(first);
    presenter.setImportRecursive(false);
    presenter.setImportSourceRoot(second);
    ASSERT_TRUE(wait_until([&] { return !presenter.importScanActive(); }));
    EXPECT_FALSE(presenter.importRecursive());
    EXPECT_EQ(presenter.importCandidates()->rowCount(), 1);
    presenter.closeImportPage();
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
    QTemporaryDir external;
    ASSERT_TRUE(external.isValid());
    model.revealFolder(external.path());
    EXPECT_EQ(model.selectedPath(), external.path());
    EXPECT_EQ(model.data(model.index(0, 0), FilesystemBrowserModel::PathRole).toString(),
              user_root);
    EXPECT_EQ(model.data(model.index(model.rowCount() - 1, 0), FilesystemBrowserModel::PathRole)
                  .toString(),
              external.path());
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
    presenter.openImportPage();
    for (auto *model : {presenter.importSourceFolders(), presenter.importDestinationFolders()})
        EXPECT_EQ(model->data(model->index(0, 0), FilesystemBrowserModel::PathRole).toString(),
                  QDir::cleanPath(QDir::homePath()));
    auto *model = presenter.importDestinationFolders();
    presenter.setImportDestination(destination);
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
    ASSERT_TRUE(presenter.importDestinationError().isEmpty());
    presenter.setImportDestination(destination);
    EXPECT_TRUE(child_visible());
    EXPECT_EQ(model->selectedPath(), destination);
    presenter.closeImportPage();
}

TEST(StudioImportWorkspace, SourcePreferenceValidatesPathsAndKeepsUnavailableFolders)
{
    ensure_qt_core();
    QTemporaryDir directory;
    const auto source = directory.filePath("unavailable");
    StudioImportPreferences preferences;
    ASSERT_TRUE(preferences.rememberSource(source));
    EXPECT_EQ(StudioImportPreferences{}.loadLastSource().value(), source);
    EXPECT_FALSE(preferences.rememberSource(QStringLiteral("relative/path")));
    EXPECT_EQ(preferences.loadLastSource().value(), source);
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
        auto *model = presenter.importSourceFolders();
        EXPECT_TRUE(wait_until(
            [&]
            {
                if (presenter.importScanActive())
                    return false;
                for (int row = 0; row < model->rowCount(); ++row)
                    if (model->data(model->index(row, 0), FilesystemBrowserModel::SelectedRole)
                            .toBool())
                        return model->data(model->index(row, 0), FilesystemBrowserModel::PathRole)
                                   .toString() == source;
                return false;
            }));
        EXPECT_EQ(presenter.importSourceRoot(), source);
        EXPECT_EQ(presenter.importSourceFolderUrl().toLocalFile(), source);
        EXPECT_EQ(presenter.importCandidates()->rowCount(), 1);
    };
    {
        StudioPresenter presenter;
        presenter.createCatalogFromPath(directory.filePath("first.sqlite"));
        ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
        presenter.openImportPage();
        presenter.setImportSourceRoot(source);
        expect_revealed(presenter);
        EXPECT_EQ(StudioImportPreferences{}.loadLastSource().value(), source);
        presenter.closeImportPage();
        presenter.openImportPage();
        expect_revealed(presenter);
        presenter.closeImportPage();
    }
    StudioPresenter restarted;
    restarted.createCatalogFromPath(directory.filePath("second.sqlite"));
    ASSERT_TRUE(wait_until([&] { return restarted.catalogOpen() && !restarted.busy(); }));
    int reveal_count = 0;
    QObject::connect(restarted.importSourceFolders(), &FilesystemBrowserModel::folderRevealed,
                     &restarted, [&](int) { ++reveal_count; });
    restarted.openImportPage();
    expect_revealed(restarted);
    EXPECT_GT(reveal_count, 0);
    restarted.closeImportPage();
    ASSERT_TRUE(QFile::remove(source + "/photo.png"));
    ASSERT_TRUE(QDir().rmdir(source));
    restarted.openImportPage();
    ASSERT_TRUE(wait_until([&] { return !restarted.importScanActive(); }));
    EXPECT_EQ(restarted.importSourceRoot(), source);
    EXPECT_FALSE(restarted.errorText().isEmpty());
    EXPECT_FALSE(restarted.importReady());
    restarted.closeImportPage();
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
        owner.openImportPage();
        // Keep this behavioral test independent of the host's temporary-directory size.
        owner.importSourceFolders()->resetWithRoots(
            {{directory.path(), QStringLiteral("fixture"), true}});
        owner.importDestinationFolders()->resetWithRoots(
            {{directory.path(), QStringLiteral("fixture"), true}});
        if (!owner.importSourceRoot().isEmpty())
            owner.importSourceFolders()->revealFolder(owner.importSourceRoot());
        if (!owner.importDestination().isEmpty())
            owner.importDestinationFolders()->revealFolder(owner.importDestination());
    };
    {
        StudioPresenter presenter;
        presenter.createCatalogFromPath(catalog);
        ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
        open_fixture_import(presenter);
        EXPECT_EQ(presenter.importMode(), QStringLiteral("copy"));
        presenter.setImportSourceRoot(source);
        presenter.setImportDestination(destination);
        ASSERT_TRUE(
            wait_until([&] { return !presenter.importScanActive() && presenter.importReady(); }));
        EXPECT_EQ(presenter.importCandidates()->rowCount(), 3);
        EXPECT_EQ(presenter.importCandidates()->selectedCount(), 2);
        EXPECT_EQ(presenter.importDuplicateCount(), 1);
        const auto complete_thumbnails = [&]
        {
            auto *model = presenter.importCandidates();
            const auto generation = model->generation();
            for (int row = 0; row < model->rowCount(); ++row)
                presenter.ensureImportThumbnail(row);
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
        EXPECT_EQ(presenter.importCandidates()->selectedCount(), 2);
        presenter.startPlannedImport();
        ASSERT_TRUE(wait_until(
            [&] { return !presenter.importPreflightActive() && !presenter.importWorkActive(); },
            30000));
        ASSERT_TRUE(presenter.errorText().isEmpty()) << presenter.errorText().toStdString();
        EXPECT_EQ(presenter.lastImportCount(), 2);
        EXPECT_EQ(StudioImportPreferences{}.loadLastDestination().value(), destination);
        EXPECT_TRUE(QFile::exists(source + "/a.png"));
        EXPECT_TRUE(QFile::exists(source + "/renamed.png"));
        open_fixture_import(presenter);
        ASSERT_TRUE(wait_until([&] { return !presenter.importScanActive(); }));
        EXPECT_EQ(presenter.importCandidates()->rowCount(), 3);
        EXPECT_EQ(presenter.importDuplicateCount(), 3);
        complete_thumbnails();
        auto *candidates = presenter.importCandidates();
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
        EXPECT_FALSE(presenter.importReady());
        // Direct catalog-URI duplicates must also load thumbnails without rescanning forever.
        presenter.setImportSourceRoot(destination);
        ASSERT_TRUE(wait_until([&] { return !presenter.importScanActive(); }));
        EXPECT_EQ(candidates->rowCount(), 2);
        EXPECT_EQ(presenter.importDuplicateCount(), 2);
        complete_thumbnails();
        EXPECT_EQ(candidates->selectedCount(), 0);
        EXPECT_FALSE(presenter.importReady());
        EXPECT_EQ(source_hash(), original_hash);
        presenter.setImportMode(QStringLiteral("add"));
        presenter.closeImportPage();
        open_fixture_import(presenter);
        EXPECT_EQ(presenter.importMode(), QStringLiteral("copy"));
    }
    StudioPresenter restarted;
    restarted.createCatalogFromPath(directory.filePath("another.sqlite"));
    ASSERT_TRUE(wait_until([&] { return restarted.catalogOpen() && !restarted.busy(); }));
    open_fixture_import(restarted);
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
