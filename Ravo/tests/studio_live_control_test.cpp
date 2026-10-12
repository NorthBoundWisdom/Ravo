#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QVariantMap>

#include <QColor>
#include <QColorSpace>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QKeySequence>
#include <QMetaType>
#include <QProcess>
#include <QSize>
#include <QThread>
#include <QTranslator>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrlQuery>

#include "ravo/adapters/filesystem_preview_cache.h"
#include "ravo/adapters/filesystem_recovery_store.h"
#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/engine/engine.h"
#include "ravo/control/live_control.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/log.h"
#include "ravo/foundation/json.h"
#include "ravo/recipe/develop.h"
#include "ravo/services/catalog_service.h"

#include "ravo/desktop/preview_request_owner.h"
#include "ravo/desktop/library_set_list_model.h"
#include "ravo/desktop/studio_command_controller.h"
#include "ravo/desktop/studio_live_session_controller.h"
#include "ravo/desktop/asset_list_model.h"
#include "ravo/desktop/studio_presenter.h"
#include "ravo/recipe/color_harmonizer.h"
#include "ravo/recipe/develop_mask.h"
#include "ravo/recipe/style.h"
#include "studio_debug_info.h"
#include "studio_language_manager.h"

#include "studio_test_support.h"

namespace ravo
{
namespace
{
using namespace studio_test_support;

TEST(StudioPresenterTest, ImportWorkspaceScansSelectsCopiesAndBuildsPreviewInBackground)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString source_dir = QDir(directory.path()).filePath(QStringLiteral("source"));
    const QString destination = QDir(directory.path()).filePath(QStringLiteral("destination"));
    const QString second_copy = QDir(directory.path()).filePath(QStringLiteral("second-copy"));
    ASSERT_TRUE(QDir().mkpath(source_dir));
    ASSERT_TRUE(QDir().mkpath(destination));
    ASSERT_TRUE(QDir().mkpath(second_copy));
    const QString photo = QDir(source_dir).filePath(QStringLiteral("workspace.png"));
    QImage image(80, 60, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(50, 100, 150));
    ASSERT_TRUE(image.save(photo, "PNG"));
    const QByteArray source_hash = [&]
    {
        QFile file(photo);
        EXPECT_TRUE(file.open(QIODevice::ReadOnly));
        return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
    }();

    StudioPresenter presenter;
    presenter.createCatalogFromPath(
        QDir(directory.path()).filePath(QStringLiteral("library.sqlite")));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.imports()->openImportPage();
    presenter.imports()->setImportSourceRoot(source_dir);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.imports()->importScanActive() &&
                   presenter.imports()->importCandidates()->rowCount() == 1;
        },
        30000));
    EXPECT_EQ(presenter.imports()->importCandidates()->selectedCount(), 1);
    presenter.imports()->setImportMode(QStringLiteral("copy"));
    presenter.imports()->setImportDestination(destination);
    presenter.imports()->setImportRenamePart(0, 1);
    presenter.imports()->setImportRenamePart(1, 3);
    presenter.imports()->setImportRenamePart(2, 0);
    presenter.imports()->setImportRenameSeparator(1);
    presenter.imports()->setImportRenameEnabled(true);
    presenter.imports()->setImportSecondCopyDestination(second_copy);
    presenter.imports()->setImportSecondCopyEnabled(true);
    presenter.imports()->setImportPreviewPolicy(QStringLiteral("standard"));
    ASSERT_TRUE(wait_until([&] { return presenter.imports()->importReady(); }, 30000))
        << presenter.errorText().toStdString();
    presenter.imports()->startPlannedImport();
    ASSERT_TRUE(wait_until([&] { return !presenter.imports()->importPageOpen(); }, 30000))
        << presenter.errorText().toStdString();
    // Gallery handoff precedes asynchronous preflight. Wait for placeholder
    // publication independently; closing the Import page is not a commit fence.
    ASSERT_TRUE(wait_until([&] { return presenter.visibleCount() >= 1; }, 30000))
        << presenter.errorText().toStdString();
    EXPECT_FALSE(presenter.assets()
                     ->data(presenter.assets()->index(0, 0), AssetListModel::DisplayNameRole)
                     .toString()
                     .isEmpty());
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.imports()->importPreflightActive() &&
                   !presenter.imports()->importWorkActive();
        },
        30000))
        << presenter.errorText().toStdString();
    EXPECT_TRUE(presenter.lastImportSelected());
    EXPECT_EQ(presenter.lastImportCount(), 1);
    EXPECT_EQ(presenter.imports()->importFilenameTemplate(),
              QStringLiteral("{stem}-{sequence}{ext}"));
    EXPECT_EQ(presenter.imports()->importSecondCopyDestination(), second_copy);
    const QString copied = QDir(destination).filePath(QStringLiteral("workspace-0001.png"));
    const QString copied_second = QDir(second_copy).filePath(QStringLiteral("workspace-0001.png"));
    ASSERT_TRUE(QFileInfo::exists(copied));
    QFile copied_file(copied);
    ASSERT_TRUE(copied_file.open(QIODevice::ReadOnly));
    EXPECT_EQ(QCryptographicHash::hash(copied_file.readAll(), QCryptographicHash::Sha256),
              source_hash);
    QFile second_file(copied_second);
    ASSERT_TRUE(second_file.open(QIODevice::ReadOnly));
    EXPECT_EQ(QCryptographicHash::hash(second_file.readAll(), QCryptographicHash::Sha256),
              source_hash);
    QFile source_file(photo);
    ASSERT_TRUE(source_file.open(QIODevice::ReadOnly));
    EXPECT_EQ(QCryptographicHash::hash(source_file.readAll(), QCryptographicHash::Sha256),
              source_hash);
    ASSERT_TRUE(wait_until([&] { return !presenter.previewWorkActive(); }, 30000));
}

TEST(StudioPresenterTest, NamedLibrarySetsSurviveReloadAndFilterListing)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString photo = directory.filePath(QStringLiteral("photo.png"));
    QImage image(32, 24, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(40, 90, 130));
    ASSERT_TRUE(image.save(photo, "PNG"));
    StudioPresenter presenter;
    StudioCommandController commands(presenter);
    presenter.createCatalogFromPath(directory.filePath(QStringLiteral("library.sqlite")));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }))
        << presenter.errorText().toStdString();
    presenter.imports()->importFilePaths({photo});
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.visibleCount() == 1 && !presenter.selectedAssetId().isEmpty() &&
                   !presenter.busy() && !presenter.imports()->importWorkActive();
        }))
        << presenter.errorText().toStdString();
    commands.executeCommand(QStringLiteral("studio.library.create_manual_set"),
                            QStringLiteral("Job"));
    ASSERT_TRUE(wait_until([&] { return presenter.librarySets()->rowCount() == 1; }))
        << presenter.errorText().toStdString();
    EXPECT_EQ(presenter.librarySets()->data(presenter.librarySets()->index(0, 0),
                                            LibrarySetListModel::NameRole),
              QStringLiteral("Job"));
    EXPECT_FALSE(presenter.selectedLibrarySetId().isEmpty());
    EXPECT_EQ(presenter.visibleCount(), 1);
}

TEST(StudioPresenterTest, VersionsStacksAndSurveyUseSerialBrowsePreviews)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString first = directory.filePath(QStringLiteral("one.png"));
    const QString second = directory.filePath(QStringLiteral("two.png"));
    QImage image(24, 16, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(40, 90, 130));
    ASSERT_TRUE(image.save(first, "PNG"));
    image.fill(QColor(130, 90, 40));
    ASSERT_TRUE(image.save(second, "PNG"));
    StudioPresenter presenter;
    StudioCommandController commands(presenter);
    presenter.createCatalogFromPath(directory.filePath(QStringLiteral("library.sqlite")));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }))
        << presenter.errorText().toStdString();
    presenter.imports()->importFilePaths({first, second});
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.visibleCount() == 2 && !presenter.selectedAssetId().isEmpty() &&
                   !presenter.busy() && !presenter.imports()->importWorkActive();
        }))
        << presenter.errorText().toStdString();
    const QString primary = presenter.assets()->assetIdAt(0);
    const QString other = presenter.assets()->assetIdAt(1);
    presenter.selectAsset(primary);
    const QString burst = QStringLiteral("studio.view.burst_compare");
    const QString previous = QStringLiteral("studio.view.burst_compare_previous");
    const QString next = QStringLiteral("studio.view.burst_compare_next");
    const auto unavailable = [&](const QString &id)
    {
        const auto before_selection = presenter.selectedAssetId();
        const auto before_mode = presenter.browseMode();
        const auto before_error = presenter.errorText();
        EXPECT_FALSE(commands.action(id).value(QStringLiteral("enabled")).toBool());
        const auto keyboard = commands.executeAction(id, QStringLiteral("keyboard"));
        EXPECT_FALSE(keyboard.value(QStringLiteral("accepted")).toBool());
        EXPECT_EQ(keyboard.value(QStringLiteral("code")).toString(), QStringLiteral("unavailable"));
        EXPECT_FALSE(commands.executeCommand(id).value(QStringLiteral("accepted")).toBool());
        for (const auto &entry : commands.shortcutEntries())
            if (entry.toMap().value(QStringLiteral("actionId")).toString() == id)
                EXPECT_FALSE(entry.toMap().value(QStringLiteral("enabled")).toBool());
        EXPECT_EQ(presenter.selectedAssetId(), before_selection);
        EXPECT_EQ(presenter.browseMode(), before_mode);
        EXPECT_EQ(presenter.errorText(), before_error);
    };
    unavailable(burst);
    unavailable(previous);
    unavailable(next);
    presenter.toggleAssetSelected(other);
    EXPECT_EQ(presenter.selectedCount(), 2);
    commands.executeCommand(QStringLiteral("studio.view.show_survey"));
    ASSERT_TRUE(wait_until([&] { return presenter.browseMode() == QLatin1String("survey"); }))
        << presenter.errorText().toStdString();
    EXPECT_NE(presenter.browseMode(), QLatin1String("develop"));
    EXPECT_EQ(presenter.surveySlotCount(), 2);
    EXPECT_EQ(presenter.surveySlots().size(), 2);
    unavailable(previous);
    unavailable(next);
    commands.executeCommand(QStringLiteral("studio.view.show_grid"));
    ASSERT_TRUE(wait_until([&] { return presenter.browseMode() == QLatin1String("grid"); }));
    presenter.selectFolder(QString{});
    ASSERT_TRUE(wait_until(
        [&] { return presenter.visibleCount() == 2 && !presenter.lastImportSelected(); }))
        << presenter.errorText().toStdString();
    presenter.selectAsset(primary);
    commands.executeCommand(QStringLiteral("studio.photo.create_version"));
    ASSERT_TRUE(wait_until([&] { return presenter.visibleCount() == 3; }))
        << presenter.errorText().toStdString();
    bool saw_version = false;
    for (int row = 0; row < presenter.assets()->rowCount(); ++row)
    {
        if (presenter.assets()
                ->data(presenter.assets()->index(row, 0), AssetListModel::VersionOrdinalRole)
                .toInt() > 0)
            saw_version = true;
    }
    EXPECT_TRUE(saw_version);
    presenter.selectAsset(primary);
    presenter.toggleAssetSelected(other);
    commands.executeCommand(QStringLiteral("studio.photo.stack_selection"));
    ASSERT_TRUE(
        wait_until([&] { return presenter.visibleCount() == 2 && presenter.collapseStacks(); }))
        << presenter.errorText().toStdString();
    const auto stack_focus = presenter.selectedAssetId();
    ASSERT_TRUE(commands.action(burst).value(QStringLiteral("enabled")).toBool());
    unavailable(previous);
    unavailable(next);
    // A queued compare must not reopen Survey after the user changes mode.
    ASSERT_TRUE(commands.executeAction(burst, QStringLiteral("keyboard"))
                    .value(QStringLiteral("accepted"))
                    .toBool());
    EXPECT_TRUE(presenter.burstComparePending());
    unavailable(previous);
    unavailable(next);
    presenter.openLoupe();
    presenter.returnToGrid();
    ASSERT_TRUE(wait_until([&] { return !presenter.burstComparePending(); }));
    EXPECT_EQ(presenter.browseMode(), QLatin1String("grid"));
    EXPECT_FALSE(presenter.burstCompareActive());
    presenter.returnToGrid();
    ASSERT_TRUE(commands.executeAction(burst, QStringLiteral("keyboard"))
                    .value(QStringLiteral("accepted"))
                    .toBool());
    ASSERT_TRUE(wait_until(
        [&] { return !presenter.burstComparePending() && presenter.burstCompareActive(); }));
    EXPECT_EQ(presenter.surveySlots().size(), 2);
    for (const auto &id : {previous, next, next, previous})
    {
        ASSERT_TRUE(commands.action(id).value(QStringLiteral("enabled")).toBool());
        ASSERT_TRUE(commands.executeAction(id, QStringLiteral("keyboard"))
                        .value(QStringLiteral("accepted"))
                        .toBool());
        ASSERT_TRUE(wait_until([&] { return !presenter.burstComparePending(); }));
        EXPECT_TRUE(presenter.burstCompareActive());
        EXPECT_TRUE(presenter.errorText().isEmpty()) << presenter.errorText().toStdString();
    }
    presenter.openSurvey();
    EXPECT_FALSE(presenter.burstCompareActive());
    unavailable(previous);
    unavailable(next);
    presenter.returnToGrid();
    unavailable(previous);
    unavailable(next);
    presenter.selectAsset(stack_focus);
    presenter.unstackSelection();
    ASSERT_TRUE(wait_until([&] { return presenter.visibleCount() == 3; }));
    unavailable(burst);
    unavailable(previous);
    unavailable(next);
    EXPECT_TRUE(presenter.errorText().isEmpty()) << presenter.errorText().toStdString();
}

} // namespace
} // namespace ravo
