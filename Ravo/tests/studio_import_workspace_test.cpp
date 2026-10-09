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
#include "../services/src/catalog_service_test_support.h"
#include "ravo/desktop/studio_command_controller.h"
#include "studio_command_ids.h"
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
    static ImportRequest plannedRequest(StudioPresenter &presenter)
    {
        return presenter.import_workspace_->plannedImportRequest();
    }
    static void preflightActive(StudioPresenter &presenter, bool active)
    {
        presenter.import_workspace_->import_preflight_active_ = active;
    }
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
    static bool importWorkerUiFence(StudioPresenter &presenter, std::shared_ptr<bool> reached)
    {
        return presenter.import_workspace_->worker->executor().post(
            [receiver = &presenter, reached = std::move(reached)]
            {
                QMetaObject::invokeMethod(
                    receiver, [reached] { *reached = true; }, Qt::QueuedConnection);
            });
    }
    static bool beforeImportPublication(StudioPresenter &presenter, std::function<void()> callback)
    {
        auto *worker = presenter.import_workspace_->worker.get();
        return worker->executor().post(
            [worker, callback = std::move(callback)]() mutable
            {
                CatalogServiceTestControl::set_before_import_publication(*worker->service(),
                                                                         std::move(callback));
            });
    }
    static CancellationToken scanToken(StudioPresenter &presenter)
    {
        return presenter.import_workspace_->scan->token();
    }
    static bool destinationPreviewFence(StudioPresenter &presenter,
                                        std::shared_ptr<std::promise<void>> reached)
    {
        return presenter.import_workspace_->destination_preview_worker->executor().post(
            [reached = std::move(reached)] { reached->set_value(); });
    }
    static bool filesystemFence(StudioPresenter &presenter,
                                std::shared_ptr<std::promise<void>> reached)
    {
        return presenter.import_workspace_->filesystem_executor_.post([reached = std::move(reached)]
                                                                      { reached->set_value(); });
    }
    static bool blockFilesystem(StudioPresenter &presenter, std::shared_future<void> release)
    {
        return presenter.import_workspace_->filesystem_executor_.post([release]
                                                                      { release.wait(); });
    }
    static bool blockDestinationPreview(StudioPresenter &presenter,
                                        std::shared_future<void> release)
    {
        return presenter.import_workspace_->destination_preview_worker->executor().post(
            [release] { release.wait(); });
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
bool wait_for_filesystem(StudioPresenter &presenter)
{
    auto reached = std::make_shared<std::promise<void>>();
    auto finished = reached->get_future();
    if (!testing::StudioImportTestControl::filesystemFence(presenter, std::move(reached)))
        return false;
    if (!wait_until(
            [&]
            {
                return finished.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
            }))
        return false;
    QCoreApplication::processEvents();
    return true;
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

TEST(StudioImportWorkspace, MoveCleanupFailureSurfacesWarningAndKeepsCommittedAsset)
{
    ensure_qt_core();
    init_logging("ravo-import-tests");
    QTemporaryDir directory;
    const auto source = directory.filePath("source");
    const auto destination = directory.filePath("destination");
    ASSERT_TRUE(QDir().mkpath(source));
    ASSERT_TRUE(QDir().mkpath(destination));
    const auto path = source + "/move.png";
    ASSERT_TRUE(photo(path, Qt::green));
    QFile sidecar(source + "/move.xmp");
    ASSERT_TRUE(sidecar.open(QIODevice::WriteOnly));
    ASSERT_GT(sidecar.write("original-xmp"), 0);
    sidecar.close();
    const auto digest = [](const QString &file_path)
    {
        QFile file(file_path);
        return file.open(QIODevice::ReadOnly) ?
                   QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256) :
                   QByteArray{};
    };
    const auto original = digest(path);
    ASSERT_FALSE(original.isEmpty());
    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath("library.sqlite"));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    auto *imports = presenter.imports();
    imports->openImportPage();
    imports->setImportSourceRoot(source);
    imports->setImportDestination(destination);
    imports->setImportMode(QStringLiteral("move"));
    ASSERT_TRUE(wait_until([&] { return !imports->importScanActive() && imports->importReady(); }));
    // Change XMP only after the copies are verified and before catalog publication.
    ASSERT_TRUE(testing::StudioImportTestControl::beforeImportPublication(
        presenter,
        [&]
        {
            QFile changed(sidecar.fileName());
            ASSERT_TRUE(changed.open(QIODevice::Append));
            ASSERT_GT(changed.write("-changed"), 0);
        }));
    imports->startPlannedImport();
    ASSERT_TRUE(wait_until(
        [&] { return !imports->importPreflightActive() && !imports->importWorkActive(); }, 30000));
    EXPECT_EQ(presenter.lastImportCount(), 1);
    EXPECT_EQ(presenter.libraryTotal(), 1);
    EXPECT_TRUE(presenter.errorText().contains(QStringLiteral("changed before move cleanup")))
        << presenter.errorText().toStdString();
    EXPECT_EQ(digest(path), original);
    EXPECT_EQ(digest(destination + "/move.png"), original);
    EXPECT_TRUE(QFile::exists(sidecar.fileName()));
    EXPECT_NE(digest(sidecar.fileName()), digest(destination + "/move.xmp"));
}

TEST(StudioImportWorkspace, MoveCancelAndConflictPreserveSourceAndCatalog)
{
    ensure_qt_core();
    init_logging("ravo-import-tests");
    for (const bool conflict : {false, true})
    {
        SCOPED_TRACE(conflict ? "conflict" : "cancel");
        QTemporaryDir directory;
        const auto source = directory.filePath("source");
        const auto destination = directory.filePath("destination");
        ASSERT_TRUE(QDir().mkpath(source));
        ASSERT_TRUE(QDir().mkpath(destination));
        ASSERT_TRUE(photo(source + "/move.png", Qt::blue));
        QFile original(source + "/move.png");
        ASSERT_TRUE(original.open(QIODevice::ReadOnly));
        const auto bytes = original.readAll();
        original.close();
        StudioPresenter presenter;
        presenter.createCatalogFromPath(directory.filePath("library.sqlite"));
        ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
        auto *imports = presenter.imports();
        imports->openImportPage();
        imports->setImportSourceRoot(source);
        imports->setImportDestination(destination);
        imports->setImportMode(QStringLiteral("move"));
        ASSERT_TRUE(
            wait_until([&] { return !imports->importScanActive() && imports->importReady(); }));
        WorkerGate gate;
        ASSERT_TRUE(testing::StudioImportTestControl::blockImportWorker(
            presenter, gate.promise.get_future().share()));
        if (conflict)
        {
            ASSERT_TRUE(photo(destination + "/move.png", Qt::red));
        }
        imports->startPlannedImport();
        ASSERT_TRUE(imports->importPreflightActive());
        if (!conflict)
            imports->closeImportPage();
        gate.release();
        ASSERT_TRUE(wait_until(
            [&] { return !imports->importPreflightActive() && !imports->importWorkActive(); },
            30000));
        EXPECT_EQ(presenter.libraryTotal(), 0);
        ASSERT_TRUE(original.open(QIODevice::ReadOnly));
        EXPECT_EQ(original.readAll(), bytes);
        EXPECT_EQ(QFile::exists(destination + "/move.png"), conflict);
        EXPECT_EQ(presenter.errorText().isEmpty(), !conflict);
    }
}

TEST(StudioImportWorkspace, DestinationPlanningBlocksImportAndCommandsButCanBeCancelled)
{
    ensure_qt_core();
    init_logging("ravo-import-tests");
    QTemporaryDir directory;
    const auto source = directory.filePath("source");
    const auto destination = directory.filePath("destination");
    ASSERT_TRUE(QDir().mkpath(source));
    ASSERT_TRUE(QDir().mkpath(destination));
    ASSERT_TRUE(photo(source + "/photo.png", Qt::red));
    ASSERT_TRUE(StudioImportPreferences{}.rememberSource(source));
    ASSERT_TRUE(StudioImportPreferences{}.rememberDestination(destination));
    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath("library.sqlite"));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    WorkerGate gate;
    ASSERT_TRUE(testing::StudioImportTestControl::blockDestinationPreview(
        presenter, gate.promise.get_future().share()));
    StudioCommandController commands(presenter);
    presenter.imports()->openImportPage();
    EXPECT_TRUE(presenter.imports()->importInteractionBlocked());
    EXPECT_FALSE(presenter.imports()->importReady());
    ASSERT_TRUE(wait_until([&] { return presenter.imports()->importDestinationPreviewActive(); }));
    EXPECT_FALSE(presenter.imports()->importInteractionBlocked());
    EXPECT_TRUE(commands.executeCommand(QLatin1String(command::kPhotoSelectAll))
                    .value("accepted")
                    .toBool());
    EXPECT_TRUE(commands.action(QLatin1String(command::kWindowSettings)).value("enabled").toBool());
    presenter.imports()->startPlannedImport();
    EXPECT_TRUE(presenter.imports()->importPageOpen());
    EXPECT_FALSE(presenter.imports()->importPreflightActive());
    EXPECT_TRUE(commands.executeCommand(QLatin1String(command::kLibraryCancelOperation))
                    .value("accepted")
                    .toBool());
    EXPECT_TRUE(presenter.imports()->importPageOpen());
    EXPECT_TRUE(presenter.imports()->importSourceRoot().isEmpty());
    EXPECT_EQ(presenter.imports()->importDestination(), destination);
    EXPECT_EQ(presenter.imports()->importCandidates()->rowCount(), 0);
    EXPECT_TRUE(presenter.imports()->importDestinationPreview().isEmpty());
    EXPECT_FALSE(presenter.imports()->importDestinationPreviewActive());
    EXPECT_FALSE(presenter.imports()->importScanActive());
    EXPECT_FALSE(presenter.imports()->importReady());
    EXPECT_FALSE(presenter.imports()->importInteractionBlocked());
    gate.release();
    auto reached = std::make_shared<std::promise<void>>();
    auto finished = reached->get_future();
    ASSERT_TRUE(testing::StudioImportTestControl::destinationPreviewFence(presenter, reached));
    ASSERT_TRUE(wait_until(
        [&]
        { return finished.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; }));
    QCoreApplication::processEvents();
    EXPECT_TRUE(presenter.imports()->importPageOpen());
    EXPECT_TRUE(presenter.imports()->importDestinationPreview().isEmpty());
    EXPECT_FALSE(presenter.imports()->importInteractionBlocked());
    EXPECT_TRUE(presenter.errorText().isEmpty());
    const auto child = source + "/chosen";
    ASSERT_TRUE(QDir().mkpath(child));
    ASSERT_TRUE(photo(child + "/child.png", Qt::blue));
    presenter.imports()->setImportSourceRoot(child);
    ASSERT_TRUE(wait_until([&] { return presenter.imports()->importReady(); }));
    EXPECT_FALSE(presenter.imports()->importInteractionBlocked());
    EXPECT_TRUE(commands.executeCommand(QLatin1String(command::kPhotoSelectAll))
                    .value("accepted")
                    .toBool());
    EXPECT_EQ(presenter.imports()
                  ->importDestinationPreview()
                  .front()
                  .toMap()
                  .value("photoCount")
                  .toUInt(),
              1U);
    EXPECT_TRUE(QDir(destination).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
}

TEST(StudioImportWorkspace, CancelAndEscapeAbandonEnumerationWithoutLeavingImport)
{
    ensure_qt_core();
    init_logging("ravo-import-tests");
    QTemporaryDir directory;
    const auto root = directory.filePath("disk");
    const auto chosen = root + "/chosen";
    const auto destination = directory.filePath("destination");
    ASSERT_TRUE(QDir().mkpath(chosen));
    ASSERT_TRUE(QDir().mkpath(destination));
    ASSERT_TRUE(photo(root + "/unwanted.png", Qt::red));
    ASSERT_TRUE(photo(chosen + "/chosen.png", Qt::blue));
    const auto hash = [](const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return QByteArray{};
        return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
    };
    const auto unwanted_hash = hash(root + "/unwanted.png");
    const auto chosen_hash = hash(chosen + "/chosen.png");
    ASSERT_FALSE(unwanted_hash.isEmpty());
    ASSERT_FALSE(chosen_hash.isEmpty());
    for (const auto *command_id : {command::kLibraryCancelOperation, command::kWindowDismiss})
    {
        SCOPED_TRACE(command_id);
        ASSERT_TRUE(StudioImportPreferences{}.rememberSource(root));
        ASSERT_TRUE(StudioImportPreferences{}.rememberDestination(destination));
        StudioPresenter presenter;
        presenter.createCatalogFromPath(directory.filePath(
            command_id == command::kLibraryCancelOperation ? "cancel.sqlite" : "escape.sqlite"));
        ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
        WorkerGate gate;
        ASSERT_TRUE(testing::StudioImportTestControl::blockImportWorker(
            presenter, gate.promise.get_future().share()));
        StudioCommandController commands(presenter);
        auto *imports = presenter.imports();
        imports->openImportPage();
        ASSERT_TRUE(imports->importScanActive());
        ASSERT_TRUE(imports->importInteractionBlocked());
        const auto token = testing::StudioImportTestControl::scanToken(presenter);
        ASSERT_TRUE(commands.executeCommand(QLatin1String(command_id)).value("accepted").toBool());
        EXPECT_TRUE(token.is_cancellation_requested());
        EXPECT_TRUE(imports->importPageOpen());
        EXPECT_TRUE(imports->importSourceRoot().isEmpty());
        EXPECT_FALSE(imports->importScanActive());
        EXPECT_FALSE(imports->importInteractionBlocked());
        EXPECT_FALSE(imports->importReady());
        EXPECT_EQ(imports->importCandidates()->rowCount(), 0);
        EXPECT_EQ(imports->importDestination(), destination);
        EXPECT_EQ(presenter.visibleCount(), 0);
        // Choose the intended subtree while the cancelled worker is still blocked.
        imports->setImportSourceRoot(chosen);
        gate.release();
        ASSERT_TRUE(
            wait_until([&] { return imports->importReady() && !imports->importScanActive(); }));
        EXPECT_EQ(imports->importSourceRoot(), chosen);
        ASSERT_EQ(imports->importCandidates()->rowCount(), 1);
        EXPECT_EQ(imports->importCandidates()->sourcePath(0),
                  QFileInfo(chosen + "/chosen.png").canonicalFilePath());
        EXPECT_TRUE(presenter.errorText().isEmpty());
        // Source cancellation must never replace formal preflight cancellation.
        testing::StudioImportTestControl::preflightActive(presenter, true);
        imports->cancelImportSource();
        EXPECT_EQ(imports->importSourceRoot(), chosen);
        testing::StudioImportTestControl::preflightActive(presenter, false);
        ASSERT_TRUE(commands.executeCommand(QLatin1String(command::kLibraryCancelOperation))
                        .value("accepted")
                        .toBool());
        EXPECT_TRUE(imports->importPageOpen());
        EXPECT_TRUE(imports->importSourceRoot().isEmpty());
        EXPECT_TRUE(commands.executeCommand(QLatin1String(command::kWindowDismiss))
                        .value("accepted")
                        .toBool());
        EXPECT_FALSE(imports->importPageOpen());
        EXPECT_EQ(hash(root + "/unwanted.png"), unwanted_hash);
        EXPECT_EQ(hash(chosen + "/chosen.png"), chosen_hash);
        EXPECT_TRUE(QDir(destination).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    }
}

TEST(StudioImportWorkspace, ThousandPhotoPlanningAndBlockedThumbnailsKeepWindowInteractive)
{
    ensure_qt_core();
    init_logging("ravo-import-tests");
    QTemporaryDir directory;
    const auto source = directory.filePath("source");
    const auto destination = directory.filePath("destination");
    ASSERT_TRUE(QDir().mkpath(source));
    ASSERT_TRUE(QDir().mkpath(destination));
    constexpr int count = 1025;
    for (int row = 0; row < count; ++row)
        ASSERT_TRUE(
            photo(source + QString("/%1.png").arg(row), QColor::fromRgb(static_cast<QRgb>(row))));
    ASSERT_TRUE(StudioImportPreferences{}.rememberSource(source));
    ASSERT_TRUE(StudioImportPreferences{}.rememberDestination(destination));
    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath("catalog.sqlite"));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    WorkerGate planning;
    WorkerGate thumbnails;
    ASSERT_TRUE(testing::StudioImportTestControl::blockDestinationPreview(
        presenter, planning.promise.get_future().share()));
    ASSERT_TRUE(testing::StudioImportTestControl::blockThumbnails(
        presenter, thumbnails.promise.get_future().share()));
    StudioCommandController commands(presenter);
    auto *imports = presenter.imports();
    imports->openImportPage();
    ASSERT_TRUE(wait_until(
        [&]
        {
            return imports->importCandidates()->rowCount() == count &&
                   imports->importDestinationPreviewActive() && !imports->importScanActive();
        }));
    EXPECT_FALSE(imports->importInteractionBlocked());
    EXPECT_FALSE(imports->importReady());
    EXPECT_TRUE(commands.executeCommand(QLatin1String(command::kPhotoSelectAll))
                    .value("accepted")
                    .toBool());
    EXPECT_TRUE(
        commands.action(QLatin1String(command::kLibraryRevealFolder)).value("enabled").toBool());
    EXPECT_TRUE(
        commands.action(QLatin1String(command::kLibraryCopyFolderPath)).value("enabled").toBool());
    EXPECT_FALSE(commands
                     .executeCommand(QLatin1String(command::kLibraryCopyFolderPath),
                                     QStringLiteral("relative/path"))
                     .value("accepted")
                     .toBool());
    imports->startPlannedImport();
    EXPECT_FALSE(imports->importWorkActive());
    EXPECT_TRUE(imports->importPageOpen());
    const auto saw_partial = std::make_shared<bool>(false);
    const auto partial_connection = QObject::connect(
        imports, &StudioImportWorkspace::importPageChanged, &presenter,
        [imports, saw_partial]
        {
            if (!imports->importDestinationPreviewActive() ||
                imports->importDestinationPreview().isEmpty())
                return;
            const auto partial_count =
                imports->importDestinationPreview().front().toMap().value("photoCount").toInt();
            if (partial_count > 0 && partial_count < count)
            {
                *saw_partial = true;
                EXPECT_FALSE(imports->importInteractionBlocked());
                EXPECT_FALSE(imports->importReady());
            }
        });
    planning.release();
    ASSERT_TRUE(wait_until([&] { return imports->importReady(); }));
    QObject::disconnect(partial_connection);
    EXPECT_TRUE(*saw_partial);
    ASSERT_FALSE(imports->importDestinationPreview().isEmpty());
    EXPECT_EQ(imports->importDestinationPreview().front().toMap().value("photoCount").toInt(),
              count);
    EXPECT_TRUE(imports->importCandidates()->thumbnail(0).isNull());
    // A checked-set replacement keeps the existing tree visible and does not reset
    // its rows until a replacement is actually published.
    WorkerGate replacement;
    ASSERT_TRUE(testing::StudioImportTestControl::blockDestinationPreview(
        presenter, replacement.promise.get_future().share()));
    const auto published = imports->importDestinationPreview();
    const auto tree_resets = std::make_shared<int>(0);
    const auto reset_connection =
        QObject::connect(imports->importDestinationFolders(), &QAbstractItemModel::modelReset,
                         &presenter, [tree_resets] { ++*tree_resets; });
    imports->importCandidates()->toggleSelected(0);
    EXPECT_TRUE(imports->importDestinationPreviewActive());
    EXPECT_FALSE(imports->importInteractionBlocked());
    EXPECT_FALSE(imports->importReady());
    EXPECT_EQ(imports->importDestinationPreview(), published);
    EXPECT_EQ(*tree_resets, 0);
    imports->startPlannedImport();
    EXPECT_FALSE(imports->importWorkActive());
    replacement.release();
    ASSERT_TRUE(wait_until([&] { return imports->importReady(); }));
    EXPECT_EQ(imports->importDestinationPreview().front().toMap().value("photoCount").toInt(),
              count - 1);
    EXPECT_TRUE(imports->importCandidates()->thumbnail(0).isNull());
    EXPECT_TRUE(QDir(destination).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    EXPECT_EQ(presenter.visibleCount(), 0);
    EXPECT_TRUE(commands.executeCommand(QLatin1String(command::kLibraryCancelOperation))
                    .value("accepted")
                    .toBool());
    EXPECT_TRUE(imports->importPageOpen());
    EXPECT_TRUE(imports->importSourceRoot().isEmpty());
    QObject::disconnect(reset_connection);
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
    EXPECT_TRUE(model.selectedPath().isEmpty());
    model.applyChildren(directory.path(), 1, list_filesystem_folders(directory.path()));
    EXPECT_EQ(model.rowCount(), 1);
    EXPECT_TRUE(model.data(model.index(0, 0), FilesystemBrowserModel::CollapsedRole).toBool());
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

TEST(StudioImportWorkspace, SecondCopyCheckboxRequiresPathAndUncheckingOmitsCopy)
{
    ensure_qt_core();
    init_logging("ravo-import-tests");
    QTemporaryDir directory;
    const auto source = directory.filePath("source");
    const auto destination = directory.filePath("destination");
    const auto second = directory.filePath("second");
    ASSERT_TRUE(QDir().mkpath(source));
    ASSERT_TRUE(QDir().mkpath(destination));
    ASSERT_TRUE(QDir().mkpath(second));
    ASSERT_TRUE(photo(source + "/photo.png", Qt::red));
    QFile original(source + "/photo.png");
    ASSERT_TRUE(original.open(QIODevice::ReadOnly));
    const auto source_hash =
        QCryptographicHash::hash(original.readAll(), QCryptographicHash::Sha256);
    original.close();
    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath("catalog.sqlite"));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    auto *imports = presenter.imports();
    imports->openImportPage();
    imports->setImportSourceRoot(source);
    imports->setImportDestination(destination);
    ASSERT_TRUE(wait_until([&] { return imports->importReady(); }));
    EXPECT_FALSE(imports->importSecondCopyEnabled());
    imports->setImportSecondCopyEnabled(true);
    EXPECT_FALSE(imports->importReady());
    imports->startPlannedImport();
    EXPECT_TRUE(imports->importPageOpen());
    EXPECT_FALSE(imports->importPreflightActive());
    EXPECT_FALSE(presenter.errorText().isEmpty());
    imports->setImportMode(QStringLiteral("add"));
    EXPECT_TRUE(imports->importReady());
    EXPECT_TRUE(
        testing::StudioImportTestControl::plannedRequest(presenter).second_copy_directory.empty());
    imports->setImportMode(QStringLiteral("copy"));
    imports->setImportSecondCopyDestination(second);
    ASSERT_TRUE(wait_until([&] { return imports->importReady(); }));
    EXPECT_EQ(testing::StudioImportTestControl::plannedRequest(presenter).second_copy_directory,
              second.toStdString());
    testing::StudioImportTestControl::preflightActive(presenter, true);
    imports->setImportSecondCopyEnabled(false);
    EXPECT_TRUE(imports->importSecondCopyEnabled());
    testing::StudioImportTestControl::preflightActive(presenter, false);
    imports->setImportSecondCopyEnabled(false);
    EXPECT_EQ(imports->importSecondCopyDestination(), second);
    EXPECT_TRUE(
        testing::StudioImportTestControl::plannedRequest(presenter).second_copy_directory.empty());
    ASSERT_TRUE(wait_until([&] { return imports->importReady(); }));
    imports->startPlannedImport();
    ASSERT_TRUE(wait_until(
        [&]
        {
            return QFileInfo::exists(destination + "/photo.png") &&
                   !imports->importPreflightActive() && !imports->importWorkActive();
        }));
    EXPECT_FALSE(QFileInfo::exists(second + "/photo.png"));
    for (const auto &path : {source + "/photo.png", destination + "/photo.png"})
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        EXPECT_EQ(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256),
                  source_hash);
    }
}

TEST(StudioImportWorkspace, RenameComponentsPreserveOriginalNamesUnlessEnabled)
{
    ensure_qt_core();
    StudioPresenter presenter;
    auto *imports = presenter.imports();
    EXPECT_FALSE(imports->importRenameEnabled());
    EXPECT_TRUE(imports->importFilenameTemplate().isEmpty());
    EXPECT_TRUE(
        testing::StudioImportTestControl::plannedRequest(presenter).filename_template.empty());
    imports->setImportRenameEnabled(true);
    EXPECT_EQ(imports->importFilenameTemplate(), QStringLiteral("{date}_{stem}_{sequence}{ext}"));
    EXPECT_EQ(imports->importRenameExample(), QStringLiteral("20260123_IMG_1234_0001.jpg"));
    imports->setImportRenamePart(0, 1);
    imports->setImportRenamePart(1, 3);
    imports->setImportRenamePart(2, 0);
    imports->setImportRenameSeparator(1);
    EXPECT_EQ(imports->importFilenameTemplate(), QStringLiteral("{stem}-{sequence}{ext}"));
    EXPECT_EQ(imports->importRenameExample(), QStringLiteral("IMG_1234-0001.jpg"));
    EXPECT_EQ(testing::StudioImportTestControl::plannedRequest(presenter).filename_template,
              "{stem}-{sequence}{ext}");
    imports->setImportRenameEnabled(false);
    EXPECT_TRUE(imports->importFilenameTemplate().isEmpty());
    EXPECT_EQ(imports->importRenameExample(), QStringLiteral("IMG_1234.jpg"));
    EXPECT_TRUE(
        testing::StudioImportTestControl::plannedRequest(presenter).filename_template.empty());
    imports->setImportRenameEnabled(true);
    EXPECT_EQ(imports->importFilenameTemplate(), QStringLiteral("{stem}-{sequence}{ext}"));
    imports->setImportMode(QStringLiteral("add"));
    EXPECT_TRUE(
        testing::StudioImportTestControl::plannedRequest(presenter).filename_template.empty());
    imports->setImportMode(QStringLiteral("copy"));
    testing::StudioImportTestControl::preflightActive(presenter, true);
    imports->setImportRenameEnabled(false);
    imports->setImportRenamePart(0, 2);
    imports->setImportRenameSeparator(2);
    EXPECT_TRUE(imports->importRenameEnabled());
    EXPECT_EQ(imports->importFilenameTemplate(), QStringLiteral("{stem}-{sequence}{ext}"));
    testing::StudioImportTestControl::preflightActive(presenter, false);
}

TEST(StudioImportWorkspace, RenameComponentsRejectInvalidChoicesAndPreserveExtensions)
{
    ensure_qt_core();
    StudioPresenter presenter;
    auto *imports = presenter.imports();
    imports->setImportRenameEnabled(true);
    const auto original = imports->importFilenameTemplate();
    for (const auto &[position, component] :
         {std::pair{-1, 1}, std::pair{3, 1}, std::pair{0, 0}, std::pair{1, -1}, std::pair{1, 4}})
    {
        imports->setImportRenamePart(position, component);
        EXPECT_EQ(imports->importFilenameTemplate(), original);
        EXPECT_FALSE(presenter.errorText().isEmpty());
    }
    for (int separator : {-1, 3})
    {
        imports->setImportRenameSeparator(separator);
        EXPECT_EQ(imports->importFilenameTemplate(), original);
    }
    for (int first = 1; first <= 3; ++first)
        for (int second = 0; second <= 3; ++second)
            for (int third = 0; third <= 3; ++third)
                for (int separator = 0; separator <= 2; ++separator)
                {
                    imports->setImportRenamePart(0, first);
                    imports->setImportRenamePart(1, second);
                    imports->setImportRenamePart(2, third);
                    imports->setImportRenameSeparator(separator);
                    const auto expanded = expand_import_filename_template(
                        imports->importFilenameTemplate().toStdString(), "DSC_1234", "20260123", 1,
                        ".NEF");
                    ASSERT_TRUE(expanded);
                    EXPECT_TRUE(expanded.value().ends_with(".NEF"));
                    EXPECT_EQ(expanded.value().find('{'), std::string::npos);
                }
}

TEST(StudioImportWorkspace, SourceSelectionPersistsWithoutImportAndRevealsAfterRestart)
{
    ensure_qt_core();
    init_logging("ravo-import-tests");
    QTemporaryDir directory;
    const auto source = directory.filePath("Pictures/2026/09");
    ASSERT_TRUE(QDir().mkpath(source + "/child"));
    ASSERT_TRUE(photo(source + "/photo.png", Qt::red));
    const auto expect_revealed = [&](StudioPresenter &presenter, bool expanded = false)
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
                    {
                        const auto index = model->index(row, 0);
                        if (expanded &&
                            (model->data(index, FilesystemBrowserModel::CollapsedRole).toBool() ||
                             model->data(index, FilesystemBrowserModel::ListingPendingRole)
                                 .toBool()))
                            return false;
                        return model->data(model->index(row, 0), FilesystemBrowserModel::PathRole)
                                   .toString() == source;
                    }
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
        expect_revealed(presenter, true);
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
    expect_revealed(restarted, true);
    EXPECT_GT(reveal_count, 0);
    auto *tree = restarted.imports()->importSourceFolders();
    tree->toggleCollapsed(source);
    restarted.imports()->refreshImportSources();
    ASSERT_TRUE(wait_for_filesystem(restarted));
    for (int row = 0; row < tree->rowCount(); ++row)
        if (tree->data(tree->index(row, 0), FilesystemBrowserModel::SelectedRole).toBool())
            EXPECT_TRUE(
                tree->data(tree->index(row, 0), FilesystemBrowserModel::CollapsedRole).toBool());
    restarted.imports()->closeImportPage();
    ASSERT_TRUE(QFile::remove(source + "/photo.png"));
    ASSERT_TRUE(QDir().rmdir(source + "/child"));
    ASSERT_TRUE(QDir().rmdir(source));
    restarted.imports()->openImportPage();
    ASSERT_TRUE(wait_until([&] { return !restarted.imports()->importScanActive(); }));
    EXPECT_TRUE(restarted.imports()->importSourceRoot().isEmpty());
    EXPECT_FALSE(restarted.errorText().isEmpty());
    EXPECT_FALSE(restarted.imports()->importReady());
    ASSERT_TRUE(wait_for_filesystem(restarted));
    EXPECT_TRUE(tree->selectedPath().isEmpty());
    for (int row = 0; row < tree->rowCount(); ++row)
    {
        const auto index = tree->index(row, 0);
        EXPECT_EQ(tree->data(index, FilesystemBrowserModel::DepthRole).toInt(), 0);
        EXPECT_TRUE(tree->data(index, FilesystemBrowserModel::CollapsedRole).toBool());
        EXPECT_FALSE(tree->data(index, FilesystemBrowserModel::SelectedRole).toBool());
        EXPECT_FALSE(tree->data(index, FilesystemBrowserModel::ListingPendingRole).toBool());
    }
    restarted.imports()->closeImportPage();
    restarted.imports()->openImportPage();
    EXPECT_TRUE(restarted.imports()->importSourceRoot().isEmpty());
    EXPECT_TRUE(restarted.errorText().isEmpty());
    restarted.imports()->closeImportPage();
}

TEST(StudioImportWorkspace, NewSourceSelectionCancelsPendingSavedSourceRestore)
{
    ensure_qt_core();
    init_logging("ravo-import-tests");
    QTemporaryDir directory;
    const auto previous = directory.filePath("previous/2026");
    const auto chosen = directory.filePath("chosen/2026");
    ASSERT_TRUE(QDir().mkpath(previous));
    ASSERT_TRUE(QDir().mkpath(chosen));
    ASSERT_TRUE(photo(previous + "/previous.png", Qt::red));
    ASSERT_TRUE(photo(chosen + "/chosen.png", Qt::blue));
    ASSERT_TRUE(StudioImportPreferences{}.rememberSource(previous));
    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath("catalog.sqlite"));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    WorkerGate gate;
    ASSERT_TRUE(testing::StudioImportTestControl::blockFilesystem(
        presenter, gate.promise.get_future().share()));
    auto *imports = presenter.imports();
    auto *tree = imports->importSourceFolders();
    imports->openImportPage();
    EXPECT_EQ(imports->importSourceRoot(), previous);
    EXPECT_TRUE(tree->selectedPath().isEmpty());
    imports->setImportSourceRoot(chosen);
    gate.release();
    ASSERT_TRUE(wait_for_filesystem(presenter));
    ASSERT_TRUE(wait_until([&] { return !imports->importScanActive(); }));
    EXPECT_EQ(imports->importSourceRoot(), chosen);
    EXPECT_EQ(tree->selectedPath(), chosen);
    ASSERT_EQ(imports->importCandidates()->rowCount(), 1);
    EXPECT_EQ(imports->importCandidates()->sourcePath(0),
              QFileInfo(chosen + "/chosen.png").canonicalFilePath());
    imports->closeImportPage();
}

TEST(StudioImportWorkspace, ClosedSourceRestoreCannotExpandUnavailableSourceAfterReopen)
{
    ensure_qt_core();
    init_logging("ravo-import-tests");
    QTemporaryDir directory;
    const auto source = directory.filePath("source");
    ASSERT_TRUE(QDir().mkpath(source));
    ASSERT_TRUE(StudioImportPreferences{}.rememberSource(source));
    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath("catalog.sqlite"));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    WorkerGate gate;
    ASSERT_TRUE(testing::StudioImportTestControl::blockFilesystem(
        presenter, gate.promise.get_future().share()));
    auto *imports = presenter.imports();
    imports->openImportPage();
    imports->closeImportPage();
    ASSERT_TRUE(QDir().rmdir(source));
    imports->openImportPage();
    EXPECT_TRUE(imports->importSourceRoot().isEmpty());
    EXPECT_FALSE(presenter.errorText().isEmpty());
    gate.release();
    ASSERT_TRUE(wait_for_filesystem(presenter));
    auto *tree = imports->importSourceFolders();
    EXPECT_TRUE(tree->selectedPath().isEmpty());
    for (int row = 0; row < tree->rowCount(); ++row)
    {
        const auto index = tree->index(row, 0);
        EXPECT_EQ(tree->data(index, FilesystemBrowserModel::DepthRole).toInt(), 0);
        EXPECT_TRUE(tree->data(index, FilesystemBrowserModel::CollapsedRole).toBool());
        EXPECT_FALSE(tree->data(index, FilesystemBrowserModel::ListingPendingRole).toBool());
    }
    imports->closeImportPage();
}

TEST(StudioImportWorkspace, DestinationAndOrganizationPersistBeforeImportAcrossReopenAndRestart)
{
    ensure_qt_core();
    init_logging("ravo-import-tests");
    QTemporaryDir directory;
    const auto destination = directory.filePath("Pictures");
    ASSERT_TRUE(QDir().mkpath(destination));
    const auto expect_restored = [&](StudioPresenter &presenter)
    {
        EXPECT_EQ(presenter.imports()->importDestination(), destination);
        EXPECT_EQ(presenter.imports()->importOrganization(), QStringLiteral("month"));
        EXPECT_EQ(presenter.imports()->importDestinationFolderUrl().toLocalFile(), destination);
        auto *tree = presenter.imports()->importDestinationFolders();
        EXPECT_TRUE(wait_until(
            [&]
            {
                for (int row = 0; row < tree->rowCount(); ++row)
                    if (tree->data(tree->index(row, 0), FilesystemBrowserModel::SelectedRole)
                            .toBool())
                        return tree->data(tree->index(row, 0), FilesystemBrowserModel::PathRole) ==
                               destination;
                return false;
            }));
    };
    {
        StudioPresenter presenter;
        presenter.createCatalogFromPath(directory.filePath("first.sqlite"));
        ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
        presenter.imports()->openImportPage();
        presenter.imports()->setImportDestination(destination);
        presenter.imports()->setImportOrganization(QStringLiteral("month"));
        ASSERT_TRUE(wait_until(
            [&]
            { return StudioImportPreferences{}.loadLastDestination().value() == destination; }));
        EXPECT_EQ(StudioImportPreferences{}.loadLastOrganization().value(),
                  QStringLiteral("month"));
        presenter.imports()->closeImportPage();
        presenter.imports()->openImportPage();
        expect_restored(presenter);
        presenter.imports()->setImportDestination(directory.filePath("missing"));
        ASSERT_TRUE(
            wait_until([&] { return !presenter.imports()->importDestinationError().isEmpty(); }));
        presenter.imports()->closeImportPage();
    }
    StudioPresenter restarted;
    restarted.createCatalogFromPath(directory.filePath("second.sqlite"));
    ASSERT_TRUE(wait_until([&] { return restarted.catalogOpen() && !restarted.busy(); }));
    restarted.imports()->openImportPage();
    expect_restored(restarted);
    EXPECT_TRUE(QDir(destination).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
}

TEST(StudioImportWorkspace, OrganizationPreferenceRejectsMalformedValuesAndFailedWrites)
{
    ensure_qt_core();
    StudioImportPreferences preferences;
    QSettings settings;
    settings.remove(QStringLiteral("desktop/import/lastOrganization"));
    settings.sync();
    EXPECT_EQ(preferences.loadLastOrganization().value(), QStringLiteral("single"));
    for (const auto &organization : {"single", "hierarchy", "date", "month"})
    {
        ASSERT_TRUE(preferences.rememberOrganization(QLatin1String(organization)));
        EXPECT_EQ(preferences.loadLastOrganization().value(), QLatin1String(organization));
    }
    EXPECT_FALSE(preferences.rememberOrganization(QStringLiteral("unknown")));
    EXPECT_EQ(preferences.loadLastOrganization().value(), QStringLiteral("month"));
    for (const QVariant &invalid : {QVariant{42}, QVariant{QStringLiteral("unknown")}})
    {
        settings.setValue(QStringLiteral("desktop/import/lastOrganization"), invalid);
        settings.sync();
        const auto loaded = preferences.loadLastOrganization();
        ASSERT_FALSE(loaded);
        EXPECT_EQ(loaded.error().context.at("reason"), "invalid_import_organization_preference");
        EXPECT_EQ(preferences.loadLastOrganization().value(), QStringLiteral("single"));
    }
    ASSERT_TRUE(preferences.rememberOrganization(QStringLiteral("month")));
    QTemporaryDir directory;
    {
        const auto settings_directory = QFileInfo(QSettings{}.fileName()).absolutePath();
        const auto saved = directory.filePath("saved-settings");
        ASSERT_TRUE(QDir().rename(settings_directory, saved));
        BlockedSettingsDirectory restore{settings_directory, saved};
        QFile blocker(settings_directory);
        ASSERT_TRUE(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        const auto remembered = preferences.rememberOrganization(QStringLiteral("date"));
        ASSERT_FALSE(remembered);
        EXPECT_EQ(remembered.error().context.at("reason"), "import_preferences_io_failed");
    }
    EXPECT_EQ(preferences.loadLastOrganization().value(), QStringLiteral("month"));
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

TEST(StudioImportWorkspace, DestinationConflictReturnsToGalleryAndRemembersSelectedFolder)
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
    // A valid folder choice is durable even when the later import preflight
    // rejects an existing output file. Import success no longer owns this choice.
    EXPECT_EQ(StudioImportPreferences{}.loadLastDestination().value(), destination);
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
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.imports()->importDestinationPreviewActive() &&
                   !presenter.imports()->importDestinationPreview().empty();
        }))
        << presenter.imports()->importDestinationPreviewError().toStdString();
    EXPECT_TRUE(presenter.imports()->importScanActive());
    EXPECT_TRUE(QDir(destination).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).empty());
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
        SCOPED_TRACE(cancel ? "cancelled preflight" : "source disappeared during execution");
        QTemporaryDir directory;
        const auto source = directory.filePath("source");
        const auto destination = directory.filePath("destination");
        ASSERT_TRUE(QDir().mkpath(source));
        ASSERT_TRUE(QDir().mkpath(destination));
        ASSERT_TRUE(photo(source + "/a.png", Qt::red));
        bool terminal_saw_placeholder = false;
        StudioPresenter presenter;
        presenter.createCatalogFromPath(directory.filePath("catalog.sqlite"));
        ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
        QObject::connect(
            presenter.imports(), &StudioImportWorkspace::libraryWorkChanged, &presenter,
            [&]
            {
                if (!presenter.imports()->importWorkActive() && presenter.libraryTotal() != 0)
                    terminal_saw_placeholder = true;
            });
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
        WorkerGate execution;
        WorkerGate publication;
        ASSERT_TRUE(testing::StudioImportTestControl::blockImportWorker(
            presenter, catalog.promise.get_future().share()));
        presenter.imports()->startPlannedImport();
        EXPECT_FALSE(presenter.imports()->importPageOpen());
        EXPECT_TRUE(presenter.imports()->importPreflightActive());
        EXPECT_TRUE(presenter.imports()->importWorkActive());
        if (cancel)
            presenter.cancelCatalogOperation();
        else
        {
            ASSERT_TRUE(testing::StudioImportTestControl::blockImportWorker(
                presenter, execution.promise.get_future().share()));
            ASSERT_TRUE(testing::StudioImportTestControl::blockCatalog(
                presenter, publication.promise.get_future().share()));
        }
        catalog.release();
        if (!cancel)
        {
            ASSERT_TRUE(wait_until(
                [&]
                {
                    return !presenter.imports()->importPreflightActive() &&
                           presenter.imports()->galleryPlaceholders();
                }));
            ASSERT_TRUE(QFile::remove(source + "/a.png"));
            execution.release();
            auto handled = std::make_shared<bool>(false);
            ASSERT_TRUE(testing::StudioImportTestControl::importWorkerUiFence(presenter, handled));
            ASSERT_TRUE(wait_until([&] { return *handled; }));
            EXPECT_TRUE(presenter.imports()->importWorkActive());
            publication.release();
        }
        ASSERT_TRUE(wait_until(
            [&]
            {
                return !presenter.imports()->importPreflightActive() &&
                       !presenter.imports()->importWorkActive();
            },
            30000));
        EXPECT_FALSE(presenter.imports()->importPageOpen());
        EXPECT_EQ(presenter.libraryTotal(), 0);
        EXPECT_FALSE(terminal_saw_placeholder);
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
