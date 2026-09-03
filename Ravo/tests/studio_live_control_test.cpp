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
    presenter.openImportPage();
    presenter.setImportSourceRoot(source_dir);
    ASSERT_TRUE(wait_until(
        [&]
        { return !presenter.importScanActive() && presenter.importCandidates()->rowCount() == 1; },
        30000));
    EXPECT_EQ(presenter.importCandidates()->selectedCount(), 1);
    presenter.setImportMode(QStringLiteral("copy"));
    presenter.setImportDestination(destination);
    presenter.setImportFilenameTemplate(QStringLiteral("shoot-{sequence}{ext}"));
    presenter.setImportSecondCopyDestination(second_copy);
    presenter.setImportPreviewPolicy(QStringLiteral("standard"));
    presenter.startPlannedImport();
    ASSERT_TRUE(wait_until([&] { return !presenter.importPageOpen(); }, 30000))
        << presenter.errorText().toStdString();
    EXPECT_GE(presenter.visibleCount(), 1);
    EXPECT_FALSE(presenter.assets()
                     ->data(presenter.assets()->index(0, 0), AssetListModel::DisplayNameRole)
                     .toString()
                     .isEmpty());
    ASSERT_TRUE(wait_until([&] { return !presenter.importWorkActive(); }, 30000))
        << presenter.errorText().toStdString();
    EXPECT_TRUE(presenter.lastImportSelected());
    EXPECT_EQ(presenter.lastImportCount(), 1);
    EXPECT_EQ(presenter.importFilenameTemplate(), QStringLiteral("shoot-{sequence}{ext}"));
    EXPECT_EQ(presenter.importSecondCopyDestination(), second_copy);
    const QString copied = QDir(destination).filePath(QStringLiteral("shoot-0001.png"));
    const QString copied_second = QDir(second_copy).filePath(QStringLiteral("shoot-0001.png"));
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

TEST(StudioQmlContract, ExportOptionsDialogExposesEveryFormatWithoutCodecParsing)
{
    QFile dialog(QStringLiteral(RAVO_STUDIO_EXPORT_OPTIONS_QML));
    ASSERT_TRUE(dialog.open(QIODevice::ReadOnly | QIODevice::Text))
        << dialog.errorString().toStdString();
    const auto source = QString::fromUtf8(dialog.readAll());

    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"ExportOptionsDialog\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Format\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Filename template\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"exportFilenameTemplate\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("presenter.selectedCount > 1")));
    EXPECT_TRUE(source.contains(QStringLiteral("{stem}-{sequence}{ext}")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Quality\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Subsampling\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Bit depth\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Compression\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Sample type\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Compression level\")")));
    EXPECT_TRUE(
        source.contains(QStringLiteral("qsTr(\"Write grayscale when the image is neutral\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Resolution (dpi)\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Metadata privacy\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Long edge\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"exportMaxEdge\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("\"maxEdge\": maxEdgeSpin.realValue")));
    EXPECT_TRUE(
        source.contains(QStringLiteral("qsTr(\"Original copy writes the exact source bytes")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Cancel\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Continue\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("presenter.exportFormatChoices()")));
    EXPECT_TRUE(source.contains(QStringLiteral("presenter.exportDefaultOptions()")));
    EXPECT_TRUE(source.contains(QStringLiteral("presenter.exportOptionBounds()")));
    EXPECT_TRUE(source.contains(QStringLiteral("resetFromPresenter()")));
    EXPECT_TRUE(source.contains(QStringLiteral("exportAccepted")));
    EXPECT_TRUE(source.contains(QStringLiteral("exportCanceled")));
    EXPECT_TRUE(source.contains(QStringLiteral("Accessible.name")));
    EXPECT_TRUE(source.contains(QStringLiteral("Keys.onEscapePressed")));
    EXPECT_TRUE(source.contains(QStringLiteral("tiffCompressionId !== \"none\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"jpegQuality\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"jpegSubsampling\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"pngBitDepth\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"pngCompression\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"tiffSampleType\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"tiffCompression\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"tiffCompressionLevel\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"tiffGrayscaleIfNeutral\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"tiffResolutionDpi\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"metadataMode\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("\"metadataMode\": metadataModeId")));
    EXPECT_FALSE(source.contains(QStringLiteral("parse_export")));
    EXPECT_FALSE(source.contains(QStringLiteral("export_format_from_ui")));
    EXPECT_FALSE(source.contains(QStringLiteral("JpegExportOptions")));
    EXPECT_FALSE(source.contains(QStringLiteral("toLowerCase()")));
    EXPECT_FALSE(source.contains(QStringLiteral("Math.round")));
    EXPECT_FALSE(source.contains(QStringLiteral("property double jpegQuality: 95")));
    EXPECT_FALSE(source.contains(QStringLiteral("property double tiffResolutionDpi: 300")));
}

TEST(StudioQmlContract, CropOverlayShowsWhenCropToolActivates)
{
    QFile main(QStringLiteral(RAVO_STUDIO_MAIN_QML));
    ASSERT_TRUE(main.open(QIODevice::ReadOnly | QIODevice::Text))
        << main.errorString().toStdString();
    const auto source = QString::fromUtf8(main.readAll());
    const auto overlay = source.indexOf(QStringLiteral("CropOverlay"));
    ASSERT_GE(overlay, 0);
    const auto visible = source.indexOf(QStringLiteral("visible:"), overlay);
    ASSERT_GE(visible, 0);
    const auto visible_line =
        source.mid(visible, source.indexOf(QLatin1Char('\n'), visible) - visible);
    EXPECT_TRUE(visible_line.contains(QStringLiteral("cropToolActive")));
    EXPECT_TRUE(visible_line.contains(QStringLiteral("studio.previewUrl")));
    EXPECT_TRUE(visible_line.contains(QStringLiteral("photoPlane.width")));
    EXPECT_FALSE(visible_line.contains(QStringLiteral("cropGuideReady")));
    EXPECT_TRUE(source.contains(QStringLiteral("rotation: 0")));
    EXPECT_TRUE(source.contains(QStringLiteral("straighten: studio.editStraighten")));
    EXPECT_FALSE(source.contains(
        QStringLiteral("cropToolActive && studio.cropGuideReady ? studio.editStraighten : 0")));
    EXPECT_TRUE(source.contains(QStringLiteral("photoItem: photoPlane")));
    EXPECT_TRUE(source.contains(QStringLiteral("sourceWidth: studio.selectedWorkingWidth")));
    EXPECT_TRUE(source.contains(QStringLiteral("sourceHeight: studio.selectedWorkingHeight")));
    EXPECT_TRUE(
        source.contains(QStringLiteral("minShortEdgePixels: studio.cropMinShortEdgePixels")));
    EXPECT_TRUE(
        source.contains(QStringLiteral("minShortEdgeFraction: studio.cropMinShortEdgeFraction")));
    EXPECT_TRUE(source.contains(QStringLiteral("onTapped: window.showPhotoMenu()")));
    EXPECT_FALSE(source.contains(QStringLiteral("onClicked: window.showPhotoMenu()")));
}

TEST(StudioQmlContract, PhotoNavigationPansClampsAndResetsOnlyOnOwnedStateChanges)
{
    QFile main(QStringLiteral(RAVO_STUDIO_MAIN_QML));
    ASSERT_TRUE(main.open(QIODevice::ReadOnly | QIODevice::Text))
        << main.errorString().toStdString();
    const auto source = QString::fromUtf8(main.readAll());
    EXPECT_TRUE(source.contains(QStringLiteral("property string viewportAssetId")));
    EXPECT_TRUE(source.contains(QStringLiteral("function centerPhotoViewport()")));
    EXPECT_TRUE(
        source.contains(QStringLiteral("window.viewportAssetId !== studio.selectedAssetId")));
    EXPECT_TRUE(source.contains(QStringLiteral("function onZoomChanged()")));
    EXPECT_TRUE(source.contains(QStringLiteral("function onEditChanged()")));
    EXPECT_TRUE(source.contains(QStringLiteral("inspectRoiDebounce.restart()")));
    EXPECT_TRUE(source.contains(QStringLiteral("function onBrowseModeChanged()")));
    EXPECT_TRUE(source.contains(QStringLiteral("scroller.contentX = maxX / 2")));
    EXPECT_TRUE(source.contains(QStringLiteral("scroller.contentY = maxY / 2")));
    EXPECT_TRUE(source.contains(QStringLiteral("boundsBehavior: Flickable.StopAtBounds")));
    EXPECT_TRUE(source.contains(QStringLiteral("function seekNavigatorViewport(nx, ny)")));
    EXPECT_TRUE(source.contains(QStringLiteral("Math.min(maxX")));
    EXPECT_TRUE(source.contains(QStringLiteral("Math.min(maxY")));
    EXPECT_TRUE(source.contains(QStringLiteral("WheelHandler")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.viewAdjustZoom")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.viewToggleActualSize")));
    EXPECT_TRUE(source.contains(QStringLiteral("photoInspectEnabled")));
    EXPECT_TRUE(source.contains(QStringLiteral("cropToolActive")));
    EXPECT_TRUE(source.contains(QStringLiteral("function togglePhotoInspectZoom(stagePos)")));
    EXPECT_TRUE(source.contains(QStringLiteral("function applyPhotoViewportAfterZoom()")));
    EXPECT_TRUE(source.contains(QStringLiteral("studio.previewViewportWidth")));
    EXPECT_TRUE(source.contains(QStringLiteral("studio.previewViewportHeight")));
    EXPECT_TRUE(source.contains(QStringLiteral("previewPlaceholderReady")));
    EXPECT_TRUE(source.contains(QStringLiteral("id: previewPlaceholderImage")));
    EXPECT_TRUE(source.contains(QStringLiteral("studio.selectedThumbnailUrl")));
    EXPECT_TRUE(source.contains(QStringLiteral("source: studio.previewUrl")));
    EXPECT_FALSE(source.contains(QStringLiteral("previewImage.implicitWidth")));
    EXPECT_FALSE(source.contains(QStringLiteral("previewImage.implicitHeight")));
    EXPECT_TRUE(source.contains(QStringLiteral("function beginInspectZoomAnimation()")));
    EXPECT_TRUE(source.contains(QStringLiteral("id: inspectZoomAnim")));
    EXPECT_TRUE(source.contains(QStringLiteral("inspectStageLockW")));
    EXPECT_TRUE(source.contains(QStringLiteral("inspectAnimScale")));
    EXPECT_TRUE(source.contains(QStringLiteral("transform: Scale")));
    EXPECT_TRUE(source.contains(QStringLiteral(
        "cursorShape: studio.whiteBalancePickActive || studio.maskPlaceActive || studio.maskParametricAssistActive ? Qt.CrossCursor : Qt.BlankCursor")));
    EXPECT_TRUE(source.contains(QStringLiteral("id: magnifierCursor")));
    EXPECT_TRUE(source.contains(QStringLiteral("onDoubleTapped")));
    EXPECT_TRUE(source.contains(QStringLiteral("openGallery(\"grid\")")));
}

TEST(StudioQmlContract, FilmstripWheelScrollsHorizontallyAndPhotoInfoSpansGridLoupeAndEdit)
{
    QFile filmstrip(
        QStringLiteral(RAVO_REPOSITORY_ROOT "/Ravo/desktop/qml/gallery/FilmStripBar.qml"));
    ASSERT_TRUE(filmstrip.open(QIODevice::ReadOnly | QIODevice::Text))
        << filmstrip.errorString().toStdString();
    const auto filmstrip_source = QString::fromUtf8(filmstrip.readAll());
    EXPECT_TRUE(filmstrip_source.contains(QStringLiteral("WheelHandler")));
    EXPECT_TRUE(filmstrip_source.contains(QStringLiteral("target: strip")));
    EXPECT_TRUE(filmstrip_source.contains(QStringLiteral("blocking: true")));
    EXPECT_TRUE(
        filmstrip_source.contains(QStringLiteral("flickableDirection: Flickable.HorizontalFlick")));
    EXPECT_TRUE(filmstrip_source.contains(QStringLiteral("event.pixelDelta.x")));
    EXPECT_TRUE(filmstrip_source.contains(QStringLiteral("event.pixelDelta.y")));
    EXPECT_TRUE(filmstrip_source.contains(QStringLiteral("event.angleDelta.y")));
    EXPECT_TRUE(filmstrip_source.contains(QStringLiteral("const delta = horizontal + vertical")));
    EXPECT_TRUE(filmstrip_source.contains(QStringLiteral("strip.contentX - delta")));
    EXPECT_TRUE(filmstrip_source.contains(QStringLiteral("strip.contentWidth - strip.width")));
    EXPECT_TRUE(filmstrip_source.contains(QStringLiteral("strip.cancelFlick()")));
    EXPECT_TRUE(filmstrip_source.contains(QStringLiteral("event.accepted = true")));

    QFile overlay(QStringLiteral(RAVO_REPOSITORY_ROOT
                                 "/Ravo/desktop/qml/gallery/PhotoInformationOverlay.qml"));
    ASSERT_TRUE(overlay.open(QIODevice::ReadOnly | QIODevice::Text))
        << overlay.errorString().toStdString();
    const auto overlay_source = QString::fromUtf8(overlay.readAll());
    EXPECT_TRUE(overlay_source.contains(QStringLiteral("objectName: \"photoInformationOverlay\"")));
    EXPECT_TRUE(overlay_source.contains(QStringLiteral("property bool compact")));
    EXPECT_TRUE(overlay_source.contains(QStringLiteral("Fonts.scaledUiSize(640)")));
    EXPECT_TRUE(overlay_source.contains(QStringLiteral("Text.ElideNone")));
    EXPECT_TRUE(overlay_source.contains(QStringLiteral("Text.Wrap")));

    QFile main(QStringLiteral(RAVO_STUDIO_MAIN_QML));
    ASSERT_TRUE(main.open(QIODevice::ReadOnly | QIODevice::Text))
        << main.errorString().toStdString();
    const auto main_source = QString::fromUtf8(main.readAll());
    EXPECT_TRUE(main_source.contains(QStringLiteral("PhotoInformationOverlay")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("studioCommands.photoInfoVisible")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("studio.browseMode === \"loupe\"")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("studio.browseMode === \"develop\"")));
    EXPECT_TRUE(main_source.contains(
        QStringLiteral("showInformationOverlay: studioCommands.photoInfoVisible")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("captureSummary: tile.captureSummary")));

    QFile thumbnail(
        QStringLiteral(RAVO_REPOSITORY_ROOT "/Ravo/desktop/qml/gallery/ThumbnailCell.qml"));
    ASSERT_TRUE(thumbnail.open(QIODevice::ReadOnly | QIODevice::Text))
        << thumbnail.errorString().toStdString();
    const auto thumbnail_source = QString::fromUtf8(thumbnail.readAll());
    EXPECT_TRUE(thumbnail_source.contains(QStringLiteral("property bool showInformationOverlay")));
    EXPECT_TRUE(thumbnail_source.contains(QStringLiteral("compact: true")));
    EXPECT_TRUE(thumbnail_source.contains(QStringLiteral("captureSummary: root.captureSummary")));
    EXPECT_TRUE(thumbnail_source.contains(QStringLiteral("objectName: \"placeholderName\"")));
    EXPECT_TRUE(thumbnail_source.contains(QStringLiteral("objectName: \"rejectedPreviewWash\"")));
    EXPECT_TRUE(thumbnail_source.contains(QStringLiteral("root.rejected ? 0.38 : 1")));
    EXPECT_TRUE(
        thumbnail_source.contains(QStringLiteral("Qt.lighter(Theme.imageSurroundColor, 1.5)")));
    EXPECT_TRUE(
        thumbnail_source.contains(QStringLiteral("root.current ? 3 : (root.selected ? 2 : 1)")));
    EXPECT_TRUE(thumbnail_source.contains(QStringLiteral("Theme.selectedSecondaryBorderColor")));
}

TEST(StudioQmlContract, MainExportUsesTwoStepExplicitFormatPayload)
{
    QFile main(QStringLiteral(RAVO_STUDIO_MAIN_QML));
    ASSERT_TRUE(main.open(QIODevice::ReadOnly | QIODevice::Text))
        << main.errorString().toStdString();
    const auto source = QString::fromUtf8(main.readAll());
    EXPECT_TRUE(source.contains(QStringLiteral("ExportOptionsDialog")));
    EXPECT_TRUE(source.contains(QStringLiteral("pendingExportFormat")));
    EXPECT_TRUE(source.contains(QStringLiteral("pendingExportOptions")));
    EXPECT_TRUE(source.contains(QStringLiteral("pendingExportFilenameTemplate")));
    EXPECT_TRUE(source.contains(QStringLiteral("libraryExportBatchWrite")));
    EXPECT_TRUE(source.contains(QStringLiteral("Select Batch Export Folder")));
    EXPECT_TRUE(source.contains(QStringLiteral("\"directory\": folderPath")));
    EXPECT_TRUE(source.contains(QStringLiteral("\"filenameTemplate\": filenameTemplate")));
    EXPECT_TRUE(source.contains(QStringLiteral("\"format\": format")));
    EXPECT_TRUE(source.contains(QStringLiteral("\"options\": options")));
    EXPECT_TRUE(source.contains(QStringLiteral("onFileRejected: window.clearPendingExport()")));
    EXPECT_TRUE(source.contains(QStringLiteral("exportOptionsDialog.visible")));
    EXPECT_FALSE(source.contains(QStringLiteral("\"filter\": selectedFilter")));
    EXPECT_FALSE(source.contains(QStringLiteral("JPEG (*.jpg *.jpeg)\", \"PNG (*.png)\"")));
}

TEST(StudioQmlContract, MainRestoresTypedWindowGeometryAndLeavesSmokeHidden)
{
    QFile main(QStringLiteral(RAVO_STUDIO_MAIN_QML));
    ASSERT_TRUE(main.open(QIODevice::ReadOnly | QIODevice::Text))
        << main.errorString().toStdString();
    const auto source = QString::fromUtf8(main.readAll());
    EXPECT_TRUE(source.contains(QStringLiteral("width: studioWindow.startupWidth")));
    EXPECT_TRUE(source.contains(QStringLiteral("height: studioWindow.startupHeight")));
    EXPECT_TRUE(source.contains(QStringLiteral("studioWindow.restore(window)")));
    EXPECT_TRUE(source.contains(QStringLiteral("visible = true")));
    EXPECT_TRUE(source.contains(QStringLiteral("if (!studioSmoke)")));
    EXPECT_FALSE(source.contains(QStringLiteral("Qt.labs.settings")));
    EXPECT_FALSE(source.contains(QStringLiteral("visible: !studioSmoke")));
}

TEST(StudioQmlContract, CatalogRecoveryUsesCommandOwnedDialogsProgressAndCancellation)
{
    QFile main(QStringLiteral(RAVO_STUDIO_MAIN_QML));
    ASSERT_TRUE(main.open(QIODevice::ReadOnly | QIODevice::Text))
        << main.errorString().toStdString();
    const auto main_source = QString::fromUtf8(main.readAll());
    EXPECT_TRUE(main_source.contains(QStringLiteral("id: backupCreateDialog")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("id: backupVerifyDialog")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("id: backupRestoreSourceDialog")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("id: backupRestoreDestinationDialog")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("ids.libraryBackupCreatePath")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("ids.libraryBackupVerifyPath")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("ids.libraryBackupRestorePaths")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("id: backupScheduleDialog")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("id: backupScheduleFolderDialog")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("ids.libraryBackupSchedulePath")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("id: folderRelinkDialog")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("ids.libraryFolderRelinkPath")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("id: removeFolderDialog")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("ids.libraryRemoveFolderConfirmed")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("\"backup\": backup")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("\"catalog\": filePath")));

    QFile library(QStringLiteral(RAVO_STUDIO_LIBRARY_SIDE_PANEL_QML));
    ASSERT_TRUE(library.open(QIODevice::ReadOnly | QIODevice::Text))
        << library.errorString().toStdString();
    const auto library_source = QString::fromUtf8(library.readAll());
    EXPECT_TRUE(library_source.contains(QStringLiteral("catalogOperationActive")));
    EXPECT_TRUE(library_source.contains(QStringLiteral("catalogOperationStage")));
    EXPECT_TRUE(library_source.contains(QStringLiteral("catalogOperationCompleted")));
    EXPECT_TRUE(library_source.contains(QStringLiteral("libraryCancelOperation")));
    EXPECT_TRUE(library_source.contains(QStringLiteral("qrc:/GeoControls/icons/Close.svg")));
    EXPECT_TRUE(library_source.contains(QStringLiteral("tooltipText: qsTr(\"Cancel\")")));
    EXPECT_FALSE(library_source.contains(QStringLiteral("text: qsTr(\"Cancel\")")));
    EXPECT_TRUE(library_source.contains(QStringLiteral("backupScheduleStatus")));
    EXPECT_TRUE(library_source.contains(QStringLiteral("Last verified: %1 · %2")));
    EXPECT_TRUE(library_source.contains(QStringLiteral("libraryFolderRelink")));
    EXPECT_TRUE(library_source.contains(QStringLiteral("missing — click to locate")));
    EXPECT_TRUE(
        library_source.contains(QStringLiteral("acceptedButtons: Qt.LeftButton | Qt.RightButton")));
    EXPECT_TRUE(library_source.contains(QStringLiteral("root.showFolderMenu(folderRow)")));
    EXPECT_TRUE(library_source.contains(QStringLiteral("FolderContextMenu")));

    QFile folder_menu(
        QStringLiteral(RAVO_REPOSITORY_ROOT "/Ravo/desktop/qml/chrome/FolderContextMenu.qml"));
    ASSERT_TRUE(folder_menu.open(QIODevice::ReadOnly | QIODevice::Text))
        << folder_menu.errorString().toStdString();
    const auto folder_menu_source = QString::fromUtf8(folder_menu.readAll());
    EXPECT_TRUE(folder_menu_source.contains(QStringLiteral("ids.libraryRevealFolder")));
    EXPECT_TRUE(folder_menu_source.contains(QStringLiteral("ids.libraryRemoveFolder")));
    EXPECT_TRUE(folder_menu_source.contains(QStringLiteral("ids.libraryImportFolderPath")));
    EXPECT_TRUE(folder_menu_source.contains(QStringLiteral("ids.libraryFolderRelink")));
    EXPECT_TRUE(folder_menu_source.contains(QStringLiteral("qsTr(\"Remove from Catalog...\")")));

    QFile schedule(QStringLiteral(RAVO_STUDIO_BACKUP_SCHEDULE_QML));
    ASSERT_TRUE(schedule.open(QIODevice::ReadOnly | QIODevice::Text))
        << schedule.errorString().toStdString();
    const auto schedule_source = QString::fromUtf8(schedule.readAll());
    EXPECT_TRUE(schedule_source.contains(QStringLiteral("backupScheduleInterval")));
    EXPECT_TRUE(schedule_source.contains(QStringLiteral("backupScheduleRetention")));
    EXPECT_TRUE(schedule_source.contains(QStringLiteral("Choose Folder…")));
}

TEST(StudioQmlContract, GalleryRequestsSparsePagesFromVisibleDelegates)
{
    QFile main(QStringLiteral(RAVO_STUDIO_MAIN_QML));
    ASSERT_TRUE(main.open(QIODevice::ReadOnly | QIODevice::Text))
        << main.errorString().toStdString();
    const auto source = QString::fromUtf8(main.readAll());
    EXPECT_TRUE(source.contains(QStringLiteral("studio.libraryHasMore")));
    EXPECT_TRUE(source.contains(QStringLiteral("studio.loadNextLibraryPage()")));
    EXPECT_TRUE(source.contains(QStringLiteral("studio.ensureLibraryRow(tile.index)")));
    EXPECT_TRUE(source.contains(QStringLiteral("onAssetIdChanged")));
    EXPECT_TRUE(source.contains(QStringLiteral("cacheBuffer: cellHeight")));
    EXPECT_FALSE(source.contains(QStringLiteral("cacheBuffer: cellHeight * 8")));
}

TEST(StudioQmlContract, LibrarySidePanelExposesNamedLibrarySets)
{
    QFile library(QStringLiteral(RAVO_STUDIO_LIBRARY_SIDE_PANEL_QML));
    ASSERT_TRUE(library.open(QIODevice::ReadOnly | QIODevice::Text))
        << library.errorString().toStdString();
    const auto source = QString::fromUtf8(library.readAll());
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"librarySetsPanel\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("presenter.librarySets")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.libraryCreateManualSet")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.libraryCreateSmartSet")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.librarySelectSet")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.libraryAddSelectionToSet")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.libraryDeleteSet")));
    EXPECT_FALSE(source.contains(QStringLiteral("library_set_member")));
    EXPECT_TRUE(source.contains(QStringLiteral("id: navigator")));
    EXPECT_TRUE(source.contains(QStringLiteral("Layout.preferredHeight: Fonts.scaledUiSize(200)")));
    EXPECT_TRUE(source.contains(QStringLiteral("Layout.minimumHeight: Fonts.scaledUiSize(200)")));
    EXPECT_TRUE(source.contains(QStringLiteral("Layout.maximumHeight: Fonts.scaledUiSize(200)")));
    EXPECT_TRUE(source.contains(QStringLiteral("id: navHeldImage")));
    EXPECT_FALSE(source.contains(QStringLiteral("navImage.implicitWidth")));
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
    presenter.importFilePaths({photo});
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.visibleCount() == 1 && !presenter.selectedAssetId().isEmpty() &&
                   !presenter.busy() && !presenter.importWorkActive();
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

TEST(StudioQmlContract, SurveyAndVersionBadgesStayInPresenterOwnedQml)
{
    QFile main(QStringLiteral(RAVO_STUDIO_MAIN_QML));
    ASSERT_TRUE(main.open(QIODevice::ReadOnly | QIODevice::Text))
        << main.errorString().toStdString();
    const auto main_source = QString::fromUtf8(main.readAll());
    EXPECT_TRUE(main_source.contains(QStringLiteral("id: surveyStage")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("studio.browseMode === \"survey\"")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("studio.surveySlots")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("studio.selectSurveySlot")));
    EXPECT_FALSE(main_source.contains(QStringLiteral("version_ordinal")));
    EXPECT_FALSE(main_source.contains(QStringLiteral("library_stack_member")));

    QFile thumbnail(
        QStringLiteral(RAVO_REPOSITORY_ROOT "/Ravo/desktop/qml/gallery/ThumbnailCell.qml"));
    ASSERT_TRUE(thumbnail.open(QIODevice::ReadOnly | QIODevice::Text))
        << thumbnail.errorString().toStdString();
    const auto thumbnail_source = QString::fromUtf8(thumbnail.readAll());
    EXPECT_TRUE(thumbnail_source.contains(QStringLiteral("property int versionOrdinal")));
    EXPECT_TRUE(thumbnail_source.contains(QStringLiteral("property int stackCount")));
    EXPECT_TRUE(thumbnail_source.contains(QStringLiteral("property bool stackPick")));

    QFile bar(
        QStringLiteral(RAVO_REPOSITORY_ROOT "/Ravo/desktop/qml/gallery/GalleryReviewBar.qml"));
    ASSERT_TRUE(bar.open(QIODevice::ReadOnly | QIODevice::Text)) << bar.errorString().toStdString();
    const auto bar_source = QString::fromUtf8(bar.readAll());
    EXPECT_TRUE(bar_source.contains(QStringLiteral("ids.viewSurvey")));
    EXPECT_TRUE(bar_source.contains(QStringLiteral("ids.photoCreateVersion")));
    EXPECT_TRUE(bar_source.contains(QStringLiteral("ids.photoStackSelection")));
    EXPECT_TRUE(bar_source.contains(QStringLiteral("id: leadingTools")));
    EXPECT_TRUE(bar_source.contains(QStringLiteral("id: reviewTools")));
    EXPECT_TRUE(bar_source.contains(QStringLiteral("id: overflowButton")));
    EXPECT_TRUE(bar_source.contains(QStringLiteral("id: overflowMenu")));
    EXPECT_TRUE(bar_source.contains(QStringLiteral("qsTr(\"More\")")));
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
    presenter.importFilePaths({first, second});
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.visibleCount() == 2 && !presenter.selectedAssetId().isEmpty() &&
                   !presenter.busy() && !presenter.importWorkActive();
        }))
        << presenter.errorText().toStdString();
    const QString primary = presenter.assets()->assetIdAt(0);
    const QString other = presenter.assets()->assetIdAt(1);
    presenter.selectAsset(primary);
    presenter.toggleAssetSelected(other);
    EXPECT_EQ(presenter.selectedCount(), 2);
    commands.executeCommand(QStringLiteral("studio.view.show_survey"));
    ASSERT_TRUE(wait_until([&] { return presenter.browseMode() == QLatin1String("survey"); }))
        << presenter.errorText().toStdString();
    EXPECT_NE(presenter.browseMode(), QLatin1String("develop"));
    EXPECT_EQ(presenter.surveySlotCount(), 2);
    EXPECT_EQ(presenter.surveySlots().size(), 2);
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
}

TEST(StudioQmlContract, LibrarySidePanelShowsCommandOwnedLastImportGroup)
{
    QFile library(QStringLiteral(RAVO_STUDIO_LIBRARY_SIDE_PANEL_QML));
    ASSERT_TRUE(library.open(QIODevice::ReadOnly | QIODevice::Text))
        << library.errorString().toStdString();
    const auto source = QString::fromUtf8(library.readAll());
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"lastImportGroup\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Last Imported Photos\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("presenter.lastImportAvailable")));
    EXPECT_TRUE(source.contains(QStringLiteral("presenter.lastImportSelected")));
    EXPECT_TRUE(source.contains(QStringLiteral("presenter.lastImportCount")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.librarySelectLastImport")));
    EXPECT_FALSE(source.contains(QStringLiteral("imported_after_unix_ms")));
}

TEST(StudioQmlContract, ImportUsesOneWorkspaceForSelectionTransferAndPreviewPolicy)
{
    QFile page(QStringLiteral(RAVO_STUDIO_IMPORT_PAGE_QML));
    ASSERT_TRUE(page.open(QIODevice::ReadOnly | QIODevice::Text))
        << page.errorString().toStdString();
    const auto source = QString::fromUtf8(page.readAll());
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Add\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Copy\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Move\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("importCandidates.applyCheck")));
    EXPECT_TRUE(source.contains(QStringLiteral("importCandidates.setAllSelected")));
    EXPECT_TRUE(source.contains(QStringLiteral("importCandidates.highlightExclusive")));
    EXPECT_TRUE(source.contains(QStringLiteral("importCandidates.highlightToggle")));
    EXPECT_TRUE(source.contains(QStringLiteral("importCandidates.highlightRange")));
    EXPECT_TRUE(source.contains(QStringLiteral("importCandidates.highlightAll")));
    EXPECT_TRUE(source.contains(QStringLiteral("StandardKey.SelectAll")));
    EXPECT_TRUE(source.contains(QStringLiteral("fittedGridCell")));
    EXPECT_TRUE(source.contains(QStringLiteral("required property bool highlighted")));
    EXPECT_TRUE(source.contains(QStringLiteral("visible: count > 0")));
    EXPECT_TRUE(source.contains(
        QStringLiteral("onIndexChanged: root.presenter.ensureImportThumbnail(index)")));
    EXPECT_TRUE(source.contains(QStringLiteral("required property bool inspected")));
    EXPECT_TRUE(source.contains(QStringLiteral("setImportOrganization")));
    EXPECT_TRUE(source.contains(QStringLiteral("By month (YYYY/MM)")));
    EXPECT_TRUE(source.contains(QStringLiteral("ImportFolderTree")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"importSourceFolderTree\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"importDestinationFolderTree\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("importSourceFolders")));
    EXPECT_TRUE(source.contains(QStringLiteral("importDestinationFolders")));
    EXPECT_TRUE(source.contains(QStringLiteral("setImportPreviewPolicy")));
    EXPECT_TRUE(source.contains(QStringLiteral("setImportFilenameTemplate")));
    EXPECT_TRUE(source.contains(QStringLiteral("setImportSecondCopyDestination")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"importFilenameTemplate\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"importChooseSecondCopy\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("{date}, {stem}, {sequence}, {ext}")));
    EXPECT_TRUE(source.contains(QStringLiteral("Minimal (320)")));
    EXPECT_TRUE(source.contains(QStringLiteral("Standard (1600)")));
    EXPECT_TRUE(source.contains(QStringLiteral("startPlannedImport")));

    QFile main(QStringLiteral(RAVO_STUDIO_MAIN_QML));
    ASSERT_TRUE(main.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto main_source = QString::fromUtf8(main.readAll());
    EXPECT_TRUE(main_source.contains(QStringLiteral("ImportPage")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("studio.openImportPage()")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("importSecondCopyDialog")));
    EXPECT_FALSE(main_source.contains(QStringLiteral("id: importDialog")));
    EXPECT_FALSE(main_source.contains(QStringLiteral("id: importFolderDialog")));
}

TEST(StudioQmlContract, LibraryFilterBarUsesCanonicalQueryCommands)
{
    QFile main(QStringLiteral(RAVO_STUDIO_MAIN_QML));
    ASSERT_TRUE(main.open(QIODevice::ReadOnly | QIODevice::Text))
        << main.errorString().toStdString();
    const auto main_source = QString::fromUtf8(main.readAll());
    EXPECT_TRUE(main_source.contains(QStringLiteral("LibraryFilterBar")));
    EXPECT_FALSE(main_source.contains(QStringLiteral("qsTr(\"Search photos\")")));
    EXPECT_FALSE(main_source.contains(QStringLiteral("qsTr(\"Clear filters\")")));

    QFile bar(QStringLiteral(RAVO_STUDIO_LIBRARY_FILTER_BAR_QML));
    ASSERT_TRUE(bar.open(QIODevice::ReadOnly | QIODevice::Text)) << bar.errorString().toStdString();
    const auto source = QString::fromUtf8(bar.readAll());
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Search photos\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("setTextFilter")));
    EXPECT_TRUE(source.contains(QStringLiteral("setMediaFilter")));
    EXPECT_TRUE(source.contains(QStringLiteral("setEditFilter")));
    EXPECT_TRUE(source.contains(QStringLiteral("presenter.mediaFilter")));
    EXPECT_TRUE(source.contains(QStringLiteral("presenter.editFilter")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Capture time\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"File size\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("setRatingExact")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Unrated\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Add filter\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("function extraOpen(id)")));
    EXPECT_TRUE(source.contains(QStringLiteral("function addExtra(id)")));
    EXPECT_TRUE(source.contains(QStringLiteral("function removeExtra(id)")));
    EXPECT_TRUE(source.contains(QStringLiteral("qrc:/GeoControls/icons/Plus.svg")));
    EXPECT_TRUE(source.contains(QStringLiteral("qrc:/GeoControls/icons/Close.svg")));

    QFile actions(QStringLiteral(RAVO_STUDIO_ACTIONS_QML));
    ASSERT_TRUE(actions.open(QIODevice::ReadOnly | QIODevice::Text))
        << actions.errorString().toStdString();
    const auto action_source = QString::fromUtf8(actions.readAll());
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.librarySetTextFilter")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.librarySetMediaFilter")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.librarySetEditFilter")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.librarySetCameraFilter")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.librarySetLensFilter")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.librarySetCaptureDateFilter")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.librarySetLocationFilter")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Camera\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Lens\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Capture date\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Location\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("setCameraFacetFilter")));
    EXPECT_TRUE(source.contains(QStringLiteral("setLensFacetFilter")));
    EXPECT_TRUE(source.contains(QStringLiteral("setCaptureDateFacetFilter")));
    EXPECT_TRUE(source.contains(QStringLiteral("setLocationFacetFilter")));
    EXPECT_TRUE(source.contains(QStringLiteral("presenter.cameraFacets")));
    EXPECT_TRUE(source.contains(QStringLiteral("presenter.locationFacets")) ||
                source.contains(QStringLiteral("presenter.countryFacets")));
    EXPECT_TRUE(source.contains(QStringLiteral("matchingFacetCount")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"%1 photos\")")));
}

TEST(StudioQmlContract, RecipeStyleUsesExplicitSaveAndApplyFileCommands)
{
    QFile main(QStringLiteral(RAVO_STUDIO_MAIN_QML));
    ASSERT_TRUE(main.open(QIODevice::ReadOnly | QIODevice::Text))
        << main.errorString().toStdString();
    const auto source = QString::fromUtf8(main.readAll());
    EXPECT_TRUE(source.contains(QStringLiteral("id: styleSaveDialog")));
    EXPECT_TRUE(source.contains(QStringLiteral("id: styleApplyDialog")));
    EXPECT_TRUE(source.contains(QStringLiteral("*.rstyle.json")));
    EXPECT_TRUE(source.contains(QStringLiteral("*.xmp")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.styleSavePath")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.styleApplyPath")));
    EXPECT_TRUE(source.contains(QStringLiteral("id === ids.styleSave")));
    EXPECT_TRUE(source.contains(QStringLiteral("id === ids.styleApply")));
    EXPECT_TRUE(source.contains(QStringLiteral("id: presetImportDialog")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.presetImport")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.presetImportPath")));
    EXPECT_TRUE(source.contains(QStringLiteral("id: parameterSelectionDialog")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.presetSave")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.presetSaveSelected")));
    EXPECT_TRUE(source.contains(QStringLiteral("id === ids.editCopyParameters")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.editCopyParametersSelected")));
    EXPECT_TRUE(source.contains(QStringLiteral("id: presetRenameDialog")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.presetRenamePath")));
    EXPECT_TRUE(source.contains(QStringLiteral("id: presetDeleteDialog")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.presetDeleteConfirmed")));
    EXPECT_TRUE(source.contains(QStringLiteral("cancelPendingConfirmation(token)")));
    EXPECT_FALSE(source.contains(QStringLiteral("darktable_style")));
}

TEST(StudioQmlContract, DevelopPresetPanelSitsAboveHistoryAndImportsThroughCommands)
{
    QFile panel(QStringLiteral(RAVO_STUDIO_DEVELOP_PRESET_PANEL_QML));
    ASSERT_TRUE(panel.open(QIODevice::ReadOnly | QIODevice::Text))
        << panel.errorString().toStdString();
    const auto source = QString::fromUtf8(panel.readAll());
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Presets\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"presetImportButton\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"presetSaveButton\"")));
    EXPECT_LT(source.indexOf(QStringLiteral("objectName: \"presetImportButton\"")),
              source.indexOf(QStringLiteral("objectName: \"presetSaveButton\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"presetList\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("editPresets")));
    EXPECT_TRUE(source.contains(QStringLiteral("modifiedParameterChoices")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.presetImport")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.presetSave")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.presetApplyPath")));
    EXPECT_TRUE(source.contains(QStringLiteral("acceptedButtons: Qt.LeftButton | Qt.RightButton")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.presetCopyInfo")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.presetRename")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.presetDelete")));
    EXPECT_TRUE(source.contains(QStringLiteral("Chrome.StudioContextMenu")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Copy Preset Info\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Rename…\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Delete…\")")));
    EXPECT_FALSE(source.contains(QStringLiteral("ravo.debug.preset")));
    EXPECT_FALSE(source.contains(QStringLiteral("OpenCL")));

    QFile rename_dialog(QStringLiteral(RAVO_STUDIO_PRESET_RENAME_DIALOG_QML));
    ASSERT_TRUE(rename_dialog.open(QIODevice::ReadOnly | QIODevice::Text))
        << rename_dialog.errorString().toStdString();
    const auto rename_source = QString::fromUtf8(rename_dialog.readAll());
    EXPECT_TRUE(rename_source.contains(QStringLiteral("DialogShell")));
    EXPECT_TRUE(rename_source.contains(QStringLiteral("objectName: \"presetRenameField\"")));
    EXPECT_TRUE(rename_source.contains(QStringLiteral("signal renameAccepted")));

    QFile save_dialog(QStringLiteral(RAVO_STUDIO_PRESET_SAVE_DIALOG_QML));
    ASSERT_TRUE(save_dialog.open(QIODevice::ReadOnly | QIODevice::Text))
        << save_dialog.errorString().toStdString();
    const auto save_source = QString::fromUtf8(save_dialog.readAll());
    EXPECT_TRUE(save_source.contains(QStringLiteral("objectName: \"ParameterSelectionDialog\"")));
    EXPECT_TRUE(save_source.contains(QStringLiteral("objectName: \"presetParameterList\"")));
    EXPECT_TRUE(save_source.contains(QStringLiteral("signal saveAccepted")));
    EXPECT_TRUE(save_source.contains(QStringLiteral("signal copyAccepted")));
    EXPECT_TRUE(save_source.contains(QStringLiteral("openForCopy")));
    EXPECT_TRUE(save_source.contains(QStringLiteral("qsTr(\"Copy Parameters\")")));
    EXPECT_TRUE(save_source.contains(QStringLiteral("\"included\": false")));
    EXPECT_TRUE(save_source.contains(QStringLiteral("selectedCount > 0")));
}

TEST(StudioQmlContract, PhotoContextMenuCopiesPresenterOwnedDebugText)
{
    QFile menu(QStringLiteral(RAVO_STUDIO_PHOTO_CONTEXT_MENU_QML));
    ASSERT_TRUE(menu.open(QIODevice::ReadOnly | QIODevice::Text))
        << menu.errorString().toStdString();
    const auto source = QString::fromUtf8(menu.readAll());
    EXPECT_TRUE(source.contains(QStringLiteral("copyParameters")));
    EXPECT_TRUE(source.contains(QStringLiteral("pasteParameters")));
    EXPECT_TRUE(source.contains(QStringLiteral("pasteParametersToSelection")));
    EXPECT_TRUE(source.contains(QStringLiteral("copyPhotoInfo")));
    EXPECT_TRUE(source.contains(QStringLiteral("copyPhotoParameters")));
    EXPECT_TRUE(source.contains(QStringLiteral("revealInFileManager")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"revealInFileManagerMenuItem\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"viewPhotoMenuItem\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"editPhotoMenuItem\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("action: root.commands.loupe")));
    EXPECT_TRUE(source.contains(QStringLiteral("action: root.commands.develop")));
    EXPECT_TRUE(source.contains(QStringLiteral("StudioContextMenu")));
    EXPECT_TRUE(source.contains(QStringLiteral("StudioContextMenuItem")));
    EXPECT_LT(source.indexOf(QStringLiteral("copyParameters")),
              source.indexOf(QStringLiteral("copyPhotoInfo")));
    EXPECT_LT(source.indexOf(QStringLiteral("copyPhotoInfo")),
              source.indexOf(QStringLiteral("copyPhotoParameters")));
    EXPECT_LT(source.indexOf(QStringLiteral("copyPhotoParameters")),
              source.indexOf(QStringLiteral("revealInFileManager")));
    EXPECT_FALSE(source.contains(QStringLiteral("ravo.debug.photo")));
    EXPECT_FALSE(source.contains(QStringLiteral("ravo.debug.parameters")));

    QFile shared_item(QStringLiteral(RAVO_STUDIO_CONTEXT_MENU_ITEM_QML));
    ASSERT_TRUE(shared_item.open(QIODevice::ReadOnly | QIODevice::Text))
        << shared_item.errorString().toStdString();
    const auto shared_source = QString::fromUtf8(shared_item.readAll());
    EXPECT_TRUE(shared_source.contains(QStringLiteral("id: checkmark")));
    EXPECT_TRUE(shared_source.contains(QStringLiteral("reserveCheckColumn: checkable")));
    EXPECT_TRUE(shared_source.contains(QStringLiteral("root.checkable && root.checked")));
    EXPECT_TRUE(shared_source.contains(QStringLiteral("indicator: Item")));

    QFile actions(QStringLiteral(RAVO_STUDIO_ACTIONS_QML));
    ASSERT_TRUE(actions.open(QIODevice::ReadOnly | QIODevice::Text))
        << actions.errorString().toStdString();
    const auto action_source = QString::fromUtf8(actions.readAll());
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.photoCopyInfo")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("copyPhotoInfo")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.photoCopyParameters")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("copyPhotoParameters")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.photoRevealInFileManager")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("revealInFileManager")));
}

TEST(StudioQmlContract, ScopePanelExposesFiveEngineOwnedModesWithoutPixelMath)
{
    QFile panel(QStringLiteral(RAVO_STUDIO_SCOPE_PANEL_QML));
    ASSERT_TRUE(panel.open(QIODevice::ReadOnly | QIODevice::Text))
        << panel.errorString().toStdString();
    const auto source = QString::fromUtf8(panel.readAll());
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Histogram\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Waveform\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Parade\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Vectorscope\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Split\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("scopeWaveformUrl")));
    EXPECT_TRUE(source.contains(QStringLiteral("scopeParadeUrl")));
    EXPECT_TRUE(source.contains(QStringLiteral("scopeVectorscopeUrl")));
    EXPECT_TRUE(source.contains(QStringLiteral("scopeSplitUrl")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.viewSetScopeMode")));
    EXPECT_TRUE(source.contains(QStringLiteral("id: scopeModeButton")));
    EXPECT_TRUE(source.contains(QStringLiteral("anchors.left: plot.left")));
    EXPECT_TRUE(source.contains(QStringLiteral("anchors.top: plot.top")));
    EXPECT_TRUE(source.contains(QStringLiteral("id: scopeModeMenu")));
    EXPECT_TRUE(source.contains(QStringLiteral("modeId: \"histogram\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("modeId === \"parade\"")));
    EXPECT_FALSE(source.contains(QStringLiteral("SegmentedControl")));
    EXPECT_FALSE(source.contains(QStringLiteral("srgb_to_linear")));
    EXPECT_FALSE(source.contains(QStringLiteral("rgb_to_d50_uv")));
}

} // namespace
} // namespace ravo
