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
#include "ravo/domain/uri.h"
#include "studio_test_support.h"
#include "ravo/foundation/log.h"
#include "ravo/desktop/studio_command_controller.h"
#include "ravo/desktop/studio_live_session_controller.h"

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

TEST(StudioImportRoundtrip, ProgressIsObservableBeforeItemsCompleteAndThroughRealCli)
{
    ensure_qt_core();
    init_logging("ravo-import-progress-tests");
    QTemporaryDir directory;
    const auto source = directory.filePath("source");
    const auto destination = directory.filePath("destination");
    ASSERT_TRUE(QDir().mkpath(source));
    ASSERT_TRUE(QDir().mkpath(destination));
    ASSERT_TRUE(photo(source + "/progress.png", Qt::red));
    StudioPresenter presenter;
    StudioCommandController commands(presenter);
    presenter.createCatalogFromPath(directory.filePath("library.sqlite"));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    auto live = StudioLiveSessionController::create(presenter, commands);
    ASSERT_TRUE(live);
    bool checking_before_items = false;
    QObject::connect(presenter.imports(), &StudioImportWorkspace::importProgressChanged, &presenter,
                     [&]
                     {
                         const auto state = presenter.imports()->jsonSnapshot();
                         if (*state.find("active")->boolean_if() &&
                             *state.find("progress")->find("phase")->string_if() == "checking")
                         {
                             checking_before_items = true;
                             EXPECT_EQ(presenter.imports()->importWorkCompleted(), 0);
                             EXPECT_FALSE(presenter.imports()->importWorkTitle().isEmpty());
                             EXPECT_FALSE(presenter.imports()->importWorkCountText().isEmpty());
                         }
                     });
    presenter.imports()->openImportPage();
    presenter.imports()->setImportSourceRoot(source);
    presenter.imports()->setImportDestination(destination);
    ASSERT_TRUE(wait_until([&] { return presenter.imports()->importReady(); }, 30000));
    presenter.imports()->startPlannedImport();
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.imports()->importWorkActive() && presenter.visibleCount() == 1 &&
                   !presenter.busy();
        },
        30000));
    EXPECT_TRUE(checking_before_items);
    auto response = run_cli_process({"studio", "state", "--session-id",
                                     QString::fromStdString(live.value()->descriptor().session_id),
                                     "--timeout-ms", "3000", "--json"});
    ASSERT_EQ(response.exit_code, 0) << response.standard_error.toStdString();
    auto data = cli_data(response.standard_output);
    ASSERT_TRUE(data);
    const auto *import = data.value().find("import");
    ASSERT_NE(import, nullptr);
    EXPECT_EQ(*import->find("schema")->string_if(), "ravo.studio.import/v1");
    EXPECT_FALSE(*import->find("active")->boolean_if());
    EXPECT_EQ(import->find("items_completed")->number_if()->text, "1");
    EXPECT_EQ(*import->find("progress")->find("phase")->string_if(), "importing");
    EXPECT_EQ(import->find("progress")->find("completed")->number_if()->text, "1");
    EXPECT_EQ(import->find("candidates")->find("photos")->number_if()->text, "1");
    EXPECT_EQ(file_sha256(source + "/progress.png"), file_sha256(destination + "/progress.png"));
}

// Temp media + temp catalog only. Opt-in real corpus is not exercised here (C1/C2).
TEST(StudioImportRoundtrip, FolderMovePreservesDestinationBytesAndReopensCatalog)
{
    ensure_qt_core();
    init_logging("ravo-import-roundtrip");
    QTemporaryDir directory;
    const auto source = directory.filePath("New Volume/iphone26");
    const auto destination = directory.filePath("destination");
    const auto second = directory.filePath("second");
    ASSERT_TRUE(QDir().mkpath(source));
    ASSERT_TRUE(QDir().mkpath(destination));
    ASSERT_TRUE(QDir().mkpath(second));
    ASSERT_TRUE(photo(source + "/move.png", Qt::green));
    QFile sidecar(source + "/move.xmp");
    ASSERT_TRUE(sidecar.open(QIODevice::WriteOnly));
    ASSERT_GT(sidecar.write("<x:xmpmeta>preserve me</x:xmpmeta>"), 0);
    sidecar.close();
    const auto image_hash = file_sha256(source + "/move.png");
    const auto sidecar_hash = file_sha256(sidecar.fileName());
    ASSERT_FALSE(image_hash.isEmpty());
    ASSERT_FALSE(sidecar_hash.isEmpty());
    const auto catalog = directory.filePath("library.sqlite");
    QString asset_id;
    {
        StudioPresenter presenter;
        presenter.createCatalogFromPath(catalog);
        ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
        auto *imports = presenter.imports();
        imports->openImportPage();
        EXPECT_EQ(imports->importIngestTransport(), QStringLiteral("folder"));
        EXPECT_TRUE(imports->importMoveUnavailableReason().isEmpty());
        imports->setImportSourceRoot(source);
        EXPECT_TRUE(imports->importIngestSourceUri().isEmpty());
        imports->setImportDestination(destination);
        imports->setImportSecondCopyDestination(second);
        imports->setImportSecondCopyEnabled(true);
        imports->setImportMode(QStringLiteral("move"));
        ASSERT_EQ(imports->importMode(), QStringLiteral("move"));
        ASSERT_TRUE(
            wait_until([&] { return !imports->importScanActive() && imports->importReady(); }));
        imports->startPlannedImport();
        ASSERT_TRUE(wait_until(
            [&] { return !imports->importPreflightActive() && !imports->importWorkActive(); },
            30000));
        ASSERT_TRUE(presenter.errorText().isEmpty()) << presenter.errorText().toStdString();
        EXPECT_EQ(presenter.lastImportCount(), 1);
        EXPECT_TRUE(imports->importIngestReport().isEmpty());
        EXPECT_FALSE(QFile::exists(source + "/move.png"));
        EXPECT_FALSE(QFile::exists(sidecar.fileName()));
        EXPECT_EQ(file_sha256(destination + "/move.png"), image_hash);
        EXPECT_EQ(file_sha256(destination + "/move.xmp"), sidecar_hash);
        EXPECT_EQ(file_sha256(second + "/move.png"), image_hash);
        EXPECT_EQ(file_sha256(second + "/move.xmp"), sidecar_hash);
        ASSERT_EQ(presenter.assets()->rowCount(), 1);
        asset_id = presenter.assets()->assetIdAt(0);
        const auto asset = presenter.assets()->assetById(asset_id);
        ASSERT_TRUE(asset);
        const auto normalized = normalize_local_input((destination + "/move.png").toStdString());
        ASSERT_TRUE(normalized);
        EXPECT_EQ(asset->normalized_uri, normalized.value().uri);
    }
    StudioPresenter reopened;
    reopened.openCatalogFromPath(catalog);
    ASSERT_TRUE(wait_until([&] { return reopened.catalogOpen() && !reopened.busy(); }));
    ASSERT_EQ(reopened.assets()->rowCount(), 1);
    EXPECT_EQ(reopened.assets()->assetIdAt(0), asset_id);
    EXPECT_EQ(file_sha256(destination + "/move.png"), image_hash);
}

TEST(StudioImportRoundtrip, IngestMoveRejectionExplainsReasonAndCanReturnToFolder)
{
    ensure_qt_core();
    init_logging("ravo-import-roundtrip");
    StudioPresenter presenter;
    auto *imports = presenter.imports();
    EXPECT_EQ(imports->importIngestTransport(), QStringLiteral("folder"));
    for (const auto *transport : {"filesystem-card", "ptp-stub", "ptp-usb", "mtp"})
    {
        imports->setImportMode(QStringLiteral("move"));
        ASSERT_EQ(imports->importMode(), QStringLiteral("move"));
        imports->setImportResumeBatchId(QStringLiteral("old-checkpoint"));
        imports->setImportIngestTransport(QString::fromLatin1(transport));
        EXPECT_EQ(imports->importMode(), QStringLiteral("copy"));
        EXPECT_TRUE(imports->importResumeBatchId().isEmpty());
        ASSERT_FALSE(imports->importMoveUnavailableReason().isEmpty());
        imports->setImportMode(QStringLiteral("move"));
        EXPECT_EQ(imports->importMode(), QStringLiteral("copy"));
        EXPECT_EQ(presenter.errorText(), imports->importMoveUnavailableReason());
        imports->setImportIngestTransport(QStringLiteral("folder"));
        EXPECT_TRUE(imports->importMoveUnavailableReason().isEmpty());
    }
}

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

        presenter.imports()->openImportPage();
        presenter.imports()->importSourceFolders()->resetWithRoots(
            {{directory.path(), QStringLiteral("fixture"), true}});
        presenter.imports()->importDestinationFolders()->resetWithRoots(
            {{directory.path(), QStringLiteral("fixture"), true}});
        presenter.imports()->setImportSourceRoot(source);
        presenter.imports()->setImportDestination(destination);
        ASSERT_TRUE(wait_until(
            [&]
            {
                return !presenter.imports()->importScanActive() &&
                       presenter.imports()->importReady();
            }));
        ASSERT_EQ(presenter.imports()->importCandidates()->rowCount(), 2);
        ASSERT_EQ(presenter.imports()->importCandidates()->selectedCount(), 2);

        // Destination preview may be empty while debounce runs; wait for inactive.
        ASSERT_TRUE(wait_until(
            [&] { return !presenter.imports()->importDestinationPreviewActive(); }, 5000));

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

        EXPECT_EQ(file_sha256(source + "/keep.png"), keep_hash);
        EXPECT_EQ(file_sha256(source + "/also.png"), also_hash);
        EXPECT_TRUE(QFile::exists(destination + "/keep.png"));
        EXPECT_TRUE(QFile::exists(destination + "/also.png"));
        EXPECT_EQ(file_sha256(destination + "/keep.png"), keep_hash);
        EXPECT_EQ(file_sha256(destination + "/also.png"), also_hash);

        presenter.imports()->closeImportPage();
        presenter.imports()->openImportPage();
        presenter.imports()->setImportSourceRoot(source);
        ASSERT_TRUE(wait_until([&] { return !presenter.imports()->importScanActive(); }));
        EXPECT_EQ(presenter.imports()->importDuplicateCount(), 2);
        EXPECT_FALSE(presenter.imports()->importReady());
        EXPECT_GE(presenter.libraryTotal(), 2);
        presenter.imports()->closeImportPage();
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
    presenter.imports()->openImportPage();
    presenter.imports()->importSourceFolders()->resetWithRoots(
        {{directory.path(), QStringLiteral("fixture"), true}});
    presenter.imports()->importDestinationFolders()->resetWithRoots(
        {{directory.path(), QStringLiteral("fixture"), true}});
    presenter.imports()->setImportSourceRoot(source);
    presenter.imports()->setImportDestination(destination);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.imports()->importScanActive() && presenter.imports()->importReady();
        }));
    presenter.imports()->startPlannedImport();
    presenter.imports()->closeImportPage();
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.imports()->importPreflightActive() &&
                   !presenter.imports()->importWorkActive();
        }));
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
        presenter.imports()->openImportPage();
        presenter.imports()->importSourceFolders()->resetWithRoots(
            {{directory.path(), QStringLiteral("fixture"), true}});
        presenter.imports()->importDestinationFolders()->resetWithRoots(
            {{directory.path(), QStringLiteral("fixture"), true}});
        presenter.imports()->setImportSourceRoot(source);
        presenter.imports()->setImportDestination(destination);
        ASSERT_TRUE(wait_until(
            [&]
            {
                return !presenter.imports()->importScanActive() &&
                       presenter.imports()->importReady();
            }));
        ASSERT_EQ(presenter.imports()->importCandidates()->rowCount(), 2);
        // Explicit subset: uncheck skip.png (row 1 after sort may vary — match by name).
        auto *model = presenter.imports()->importCandidates();
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
        ASSERT_TRUE(wait_until(
            [&] { return !presenter.imports()->importDestinationPreviewActive(); }, 5000));
        presenter.imports()->startPlannedImport();
        ASSERT_TRUE(wait_until(
            [&]
            {
                return !presenter.imports()->importPreflightActive() &&
                       !presenter.imports()->importWorkActive();
            },
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
        presenter.imports()->closeImportPage();
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
    presenter.imports()->openImportPage();
    presenter.imports()->importSourceFolders()->resetWithRoots(
        {{directory.path(), QStringLiteral("fixture"), true}});
    presenter.imports()->importDestinationFolders()->resetWithRoots(
        {{directory.path(), QStringLiteral("fixture"), true}});
    presenter.imports()->setImportSourceRoot(source);
    presenter.imports()->setImportDestination(destination);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.imports()->importScanActive() && presenter.imports()->importReady();
        }));
    presenter.imports()->startPlannedImport();
    presenter.imports()->closeImportPage(); // cancel / abandon while preflight may still be queued
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.imports()->importPreflightActive() &&
                   !presenter.imports()->importWorkActive();
        }));
    for (int i = 0; i < 50; ++i)
        QCoreApplication::processEvents();
    EXPECT_EQ(file_sha256(source + "/solo.png"), original);
    EXPECT_TRUE(QDir(destination).entryList(QDir::Files).isEmpty());
    EXPECT_EQ(presenter.libraryTotal(), 0);
}

TEST(StudioImportRoundtrip, RepeatedIntentsRemainQuiescentWithBoundedDiagnostics)
{
    ensure_qt_core();
    init_logging("ravo-import-roundtrip");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto source_a = directory.filePath("source-a");
    const auto source_b = directory.filePath("source-b");
    const auto destination = directory.filePath("destination");
    ASSERT_TRUE(QDir().mkpath(source_a));
    ASSERT_TRUE(QDir().mkpath(source_b));
    ASSERT_TRUE(QDir().mkpath(destination));
    ASSERT_TRUE(photo(source_a + "/a1.png", Qt::red));
    ASSERT_TRUE(photo(source_a + "/a2.png", Qt::green));
    ASSERT_TRUE(photo(source_b + "/b1.png", Qt::blue));
    ASSERT_TRUE(photo(source_b + "/b2.png", Qt::yellow));

    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath("library.sqlite"));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.imports()->openImportPage();
    presenter.imports()->setImportMode(QStringLiteral("add"));
    presenter.imports()->setImportSourceRoot(source_a);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.imports()->importCandidates()->rowCount() >= 2 &&
                   !presenter.imports()->importScanActive();
        },
        30000));
    auto *model = presenter.imports()->importCandidates();
    model->resetSelectionRowTouchCount();
    model->applyCheck(0);
    EXPECT_LE(model->selectionRowTouchCount(), 8U);

    // Source change must stop old generation work and admit the new scan.
    presenter.imports()->setImportSourceRoot(source_b);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.imports()->importCandidates()->rowCount() >= 2 &&
                   !presenter.imports()->importScanActive();
        },
        30000));
    EXPECT_FALSE(presenter.imports()->importWorkActive());
    EXPECT_FALSE(presenter.imports()->importPreflightActive());

    // Close/reopen same catalog — reuse DestroyAndReopen coverage for membership;
    // here assert the page returns to a quiescent owner.
    presenter.imports()->closeImportPage();
    EXPECT_FALSE(presenter.imports()->importPageOpen());
    presenter.imports()->openImportPage();
    presenter.imports()->setImportSourceRoot(source_a);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.imports()->importCandidates()->rowCount() >= 2 &&
                   !presenter.imports()->importScanActive();
        },
        30000));
    EXPECT_FALSE(presenter.busy());
    EXPECT_TRUE(presenter.errorText().isEmpty()) << presenter.errorText().toStdString();

    // Preflight cancel path: start then close before commit.
    presenter.imports()->setImportDestination(destination);
    presenter.imports()->setImportMode(QStringLiteral("copy"));
    ASSERT_TRUE(
        wait_until([&] { return !presenter.imports()->importDestinationPreviewActive(); }, 5000));
    presenter.imports()->startPlannedImport();
    presenter.imports()->closeImportPage();
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.imports()->importPreflightActive() &&
                   !presenter.imports()->importWorkActive();
        },
        30000));
    EXPECT_FALSE(presenter.imports()->importPageOpen());
}

TEST(StudioImportRoundtrip, ImportRecoveryWorkflowCoversSourceSwitchEvictionAndReopen)
{
    ensure_qt_core();
    init_logging("ravo-import-roundtrip");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto source_a = directory.filePath("source-a");
    const auto source_b = directory.filePath("source-b");
    const auto destination = directory.filePath("destination");
    const auto catalog = directory.filePath("library.sqlite");
    ASSERT_TRUE(QDir().mkpath(source_a));
    ASSERT_TRUE(QDir().mkpath(source_b));
    ASSERT_TRUE(QDir().mkpath(destination));
    for (int i = 0; i < 4; ++i)
    {
        ASSERT_TRUE(
            photo(source_a + QStringLiteral("/a%1.png").arg(i), QColor(20 + i * 40, 10, 10)));
        ASSERT_TRUE(
            photo(source_b + QStringLiteral("/b%1.png").arg(i), QColor(10, 20 + i * 40, 10)));
    }
    const auto hash_a0 = file_sha256(source_a + "/a0.png");
    ASSERT_FALSE(hash_a0.isEmpty());

    int library_total = 0;
    {
        StudioPresenter presenter;
        presenter.createCatalogFromPath(catalog);
        ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
        presenter.imports()->openImportPage();
        presenter.imports()->importSourceFolders()->resetWithRoots(
            {{directory.path(), QStringLiteral("fixture"), true}});
        presenter.imports()->importDestinationFolders()->resetWithRoots(
            {{directory.path(), QStringLiteral("fixture"), true}});
        presenter.imports()->setImportMode(QStringLiteral("copy"));
        presenter.imports()->setImportSourceRoot(source_a);
        presenter.imports()->setImportDestination(destination);
        ASSERT_TRUE(wait_until(
            [&]
            {
                return !presenter.imports()->importScanActive() &&
                       presenter.imports()->importReady();
            },
            30000));
        auto *model = presenter.imports()->importCandidates();
        ASSERT_EQ(model->rowCount(), 4);
        for (int row = 0; row < model->rowCount(); ++row)
            presenter.imports()->ensureImportThumbnail(row);
        ASSERT_TRUE(wait_until(
            [&]
            {
                for (int row = 0; row < model->rowCount(); ++row)
                    if (!model->inspected(row) || model->thumbnail(row).isNull())
                        return false;
                return true;
            },
            30000));

        presenter.imports()->setImportSourceRoot(source_b);
        ASSERT_TRUE(wait_until(
            [&] { return model->rowCount() == 4 && !presenter.imports()->importScanActive(); },
            30000));
        for (int row = 0; row < model->rowCount(); ++row)
            presenter.imports()->ensureImportThumbnail(row);
        ASSERT_TRUE(wait_until(
            [&]
            {
                for (int row = 0; row < model->rowCount(); ++row)
                    if (!model->inspected(row) || model->thumbnail(row).isNull())
                        return false;
                return true;
            },
            30000));

        presenter.imports()->setImportSourceRoot(source_a);
        ASSERT_TRUE(wait_until(
            [&]
            {
                return model->rowCount() == 4 && !presenter.imports()->importScanActive() &&
                       presenter.imports()->importReady();
            },
            30000));
        for (int row = 0; row < model->rowCount(); ++row)
            presenter.imports()->ensureImportThumbnail(row);
        ASSERT_TRUE(wait_until(
            [&]
            {
                for (int row = 0; row < model->rowCount(); ++row)
                    if (!model->inspected(row) || model->thumbnail(row).isNull())
                        return false;
                return true;
            },
            30000));

        presenter.imports()->startPlannedImport();
        presenter.imports()->closeImportPage();
        ASSERT_TRUE(wait_until(
            [&]
            {
                return !presenter.imports()->importPreflightActive() &&
                       !presenter.imports()->importWorkActive();
            },
            30000));
        EXPECT_EQ(file_sha256(source_a + "/a0.png"), hash_a0);

        presenter.imports()->openImportPage();
        presenter.imports()->setImportSourceRoot(source_a);
        presenter.imports()->setImportDestination(destination);
        ASSERT_TRUE(wait_until(
            [&]
            {
                return !presenter.imports()->importScanActive() &&
                       presenter.imports()->importReady();
            },
            30000));
        presenter.imports()->startPlannedImport();
        ASSERT_TRUE(wait_until(
            [&]
            {
                return !presenter.imports()->importPreflightActive() &&
                       !presenter.imports()->importWorkActive();
            },
            30000));
        ASSERT_TRUE(presenter.errorText().isEmpty()) << presenter.errorText().toStdString();
        EXPECT_GE(presenter.lastImportCount(), 1);
        EXPECT_EQ(file_sha256(source_a + "/a0.png"), hash_a0);

        library_total = presenter.libraryTotal();
        EXPECT_GE(library_total, 1);
        presenter.imports()->closeImportPage();
    }
    StudioPresenter reopened;
    reopened.openCatalogFromPath(catalog);
    ASSERT_TRUE(wait_until([&] { return reopened.catalogOpen() && !reopened.busy(); }));
    EXPECT_EQ(reopened.libraryTotal(), library_total);
}

} // namespace ravo
