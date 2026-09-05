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
#include "ravo/desktop/folder_list_model.h"
#include "ravo/desktop/library_set_list_model.h"
#include "ravo/desktop/studio_command_controller.h"
#include "ravo/desktop/studio_live_session_controller.h"
#include "ravo/desktop/asset_list_model.h"
#include "ravo/desktop/import_candidate_list_model.h"
#include "ravo/desktop/studio_presenter.h"
#include "ravo/recipe/color_harmonizer.h"
#include "ravo/recipe/develop_mask.h"
#include "ravo/recipe/style.h"
#include "studio_debug_info.h"
#include "studio_develop_internal.h"
#include "studio_language_manager.h"
#include "studio_qml_test_support.h"

#include "studio_test_support.h"
#include "interactive_perf_report.h"

namespace ravo
{
namespace
{
using namespace studio_test_support;
TEST(StudioPresenterTest, MultiInstanceStructuralEditsCreateHistoryAndUndo)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString photo = directory.filePath(QStringLiteral("multi-instance-history.png"));
    QImage image(48, 32, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(90, 110, 130));
    ASSERT_TRUE(image.save(photo, "PNG"));
    const QString catalog = directory.filePath(QStringLiteral("library.sqlite"));

    StudioPresenter presenter;
    presenter.createCatalogFromPath(catalog);
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }))
        << presenter.errorText().toStdString();
    presenter.importFilePaths({photo});
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.visibleCount() == 1 && !presenter.selectedAssetId().isEmpty() &&
                   !presenter.busy();
        }))
        << presenter.errorText().toStdString();
    presenter.openDevelop();
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.previewLoading() && !presenter.previewUrl().isEmpty() &&
                   presenter.selectedPhotoParametersDebugInfo().contains(
                       QStringLiteral("recipe_state=saved\n"));
        }))
        << presenter.errorText().toStdString();

    const auto history_size = [&] { return presenter.recipeHistory().size(); };

    presenter.addExposureInstance();
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.previewLoading() && presenter.exposureInstances().size() == 2 &&
                   history_size() == 1;
        }))
        << presenter.errorText().toStdString()
        << " instances=" << presenter.exposureInstances().size() << " history=" << history_size();
    EXPECT_TRUE(presenter.canUndo());

    const auto local_id =
        presenter.exposureInstances().back().toMap().value(QStringLiteral("id")).toString();
    presenter.selectExposureInstance(local_id);
    presenter.setDevelopNumber(QStringLiteral("exposureMaskKind"), 3.0); // circle
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.previewLoading() && history_size() == 2 &&
                   presenter.exposureInstances()
                       .back()
                       .toMap()
                       .value(QStringLiteral("hasMask"))
                       .toBool();
        }))
        << presenter.errorText().toStdString() << " history=" << history_size();

    presenter.duplicateExposureInstance();
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.previewLoading() && presenter.exposureInstances().size() == 3 &&
                   history_size() == 3 &&
                   presenter.exposureInstances()
                       .back()
                       .toMap()
                       .value(QStringLiteral("hasMask"))
                       .toBool();
        }))
        << presenter.errorText().toStdString() << " history=" << history_size();

    presenter.setExposureInstanceBypass(
        presenter.exposureInstances().front().toMap().value(QStringLiteral("id")).toString(), true);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.previewLoading() && history_size() == 4 &&
                   presenter.exposureInstances()
                       .front()
                       .toMap()
                       .value(QStringLiteral("bypass"))
                       .toBool();
        }))
        << presenter.errorText().toStdString() << " history=" << history_size();

    presenter.reorderExposureInstance(0, 2);
    ASSERT_TRUE(wait_until([&] { return !presenter.previewLoading() && history_size() == 5; }))
        << presenter.errorText().toStdString() << " history=" << history_size();

    // Undo restores instance vector + masks step by step.
    presenter.undoEdit();
    ASSERT_TRUE(wait_until([&] { return !presenter.previewLoading() && history_size() >= 4; }))
        << presenter.errorText().toStdString();
    EXPECT_EQ(presenter.exposureInstances().size(), 3);

    presenter.undoEdit(); // bypass
    ASSERT_TRUE(wait_until([&] { return !presenter.previewLoading(); }))
        << presenter.errorText().toStdString();
    presenter.undoEdit(); // duplicate
    ASSERT_TRUE(wait_until(
        [&] { return !presenter.previewLoading() && presenter.exposureInstances().size() == 2; }))
        << presenter.errorText().toStdString();
    EXPECT_TRUE(
        presenter.exposureInstances().back().toMap().value(QStringLiteral("hasMask")).toBool());

    presenter.undoEdit(); // mask
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.previewLoading() && presenter.exposureInstances().size() == 2 &&
                   !presenter.exposureInstances()
                        .back()
                        .toMap()
                        .value(QStringLiteral("hasMask"))
                        .toBool();
        }))
        << presenter.errorText().toStdString();

    presenter.undoEdit(); // add
    ASSERT_TRUE(wait_until(
        [&] { return !presenter.previewLoading() && presenter.exposureInstances().size() <= 1; }))
        << presenter.errorText().toStdString();
    EXPECT_TRUE(presenter.canRedo());

    presenter.redoEdit();
    ASSERT_TRUE(wait_until(
        [&] { return !presenter.previewLoading() && presenter.exposureInstances().size() == 2; }))
        << presenter.errorText().toStdString();

    const auto before_delete = presenter.exposureInstances().size();
    const auto delete_id =
        presenter.exposureInstances().back().toMap().value(QStringLiteral("id")).toString();
    presenter.deleteExposureInstance(delete_id);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.previewLoading() &&
                   presenter.exposureInstances().size() == before_delete - 1;
        }))
        << presenter.errorText().toStdString();
    presenter.undoEdit();
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.previewLoading() &&
                   presenter.exposureInstances().size() == before_delete;
        }))
        << presenter.errorText().toStdString();
}

TEST(StudioPresenterTest, Local01PreviewScaleAndOneToOneRoiMaskAuthoring)
{
    // Live Studio path: QML normalizes clicks to the photoPlane (fit/fill/1:1
    // scroll only change plane size; authoring coords stay 0..1). map_mask_place
    // then maps through crop — including 1:1 ROI aspect — onto the selected
    // instance mask. Preview scale itself is request-time and must not alter
    // the normalized authoring contract.
    using studio_develop_internal::map_mask_place_preview;
    using studio_develop_internal::mask_place_geometry_allowed;

    DevelopParams params;
    ASSERT_TRUE(add_exposure_instance(params));
    ASSERT_EQ(params.exposure_instances.size(), 2U);
    load_exposure_instance_into_legacy(params, 1);
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "exposureMaskKind", 3.0)); // circle
    mirror_legacy_exposure_into_instance(params, 1);
    ASSERT_TRUE(params.exposure_instances[1].mask_id.has_value());
    EXPECT_EQ(params.exposure_mask_id, params.exposure_instances[1].mask_id);

    // 1:1 ROI crop on the develop recipe (Studio Actual Size / locked aspect).
    params.crop_x = 0.10;
    params.crop_y = 0.05;
    params.crop_width = 0.80;
    params.crop_height = 0.70;
    ASSERT_TRUE(apply_crop_aspect(params, "1:1"));
    EXPECT_NEAR(params.crop_width, params.crop_height, 1e-9);
    EXPECT_TRUE(mask_place_geometry_allowed(params));

    // Same normalized photoPlane click under any preview scale maps identically.
    constexpr double preview_x = 0.25;
    constexpr double preview_y = 0.75;
    auto mapped_fit = map_mask_place_preview(params, preview_x, preview_y);
    ASSERT_TRUE(mapped_fit) << mapped_fit.error().message;
    auto mapped_fill = map_mask_place_preview(params, preview_x, preview_y);
    ASSERT_TRUE(mapped_fill) << mapped_fill.error().message;
    auto mapped_actual = map_mask_place_preview(params, preview_x, preview_y);
    ASSERT_TRUE(mapped_actual) << mapped_actual.error().message;
    EXPECT_DOUBLE_EQ(mapped_fit.value().first, mapped_fill.value().first);
    EXPECT_DOUBLE_EQ(mapped_fit.value().second, mapped_fill.value().second);
    EXPECT_DOUBLE_EQ(mapped_fit.value().first, mapped_actual.value().first);
    EXPECT_DOUBLE_EQ(mapped_fit.value().second, mapped_actual.value().second);
    EXPECT_NEAR(mapped_fit.value().first, params.crop_x + preview_x * params.crop_width, 1e-12);
    EXPECT_NEAR(mapped_fit.value().second, params.crop_y + preview_y * params.crop_height, 1e-12);

    // Apply onto selected instance mask fields (mirrors StudioPresenter::placeMask).
    ASSERT_TRUE(
        apply_develop_mask_field_strict(params, "exposureMaskCenterX", mapped_fit.value().first));
    ASSERT_TRUE(
        apply_develop_mask_field_strict(params, "exposureMaskCenterY", mapped_fit.value().second));
    mirror_legacy_exposure_into_instance(params, 1);
    load_exposure_instance_into_legacy(params, 1);
    auto state = develop_mask_editor_state(params, DevelopMaskTarget::kExposure);
    EXPECT_TRUE(state.editable);
    EXPECT_EQ(state.kind_name, "circle");
    EXPECT_NEAR(state.center_x, mapped_fit.value().first, 1e-9);
    EXPECT_NEAR(state.center_y, mapped_fit.value().second, 1e-9);
    EXPECT_EQ(params.exposure_mask_id, params.exposure_instances[1].mask_id);

    // Inspect-ROI normalized rect (1:1 viewport window into photoPlane) does not
    // redefine authoring space: a click expressed in full-plane coords still maps
    // through crop only. ROI origin (0.2,0.3) size (0.4,0.4) of the plane.
    const double roi_x = 0.2;
    const double roi_y = 0.3;
    const double roi_w = 0.4;
    const double roi_h = 0.4;
    const double click_in_roi_u = 0.5; // center of visible ROI
    const double click_in_roi_v = 0.5;
    const double plane_x = roi_x + click_in_roi_u * roi_w;
    const double plane_y = roi_y + click_in_roi_v * roi_h;
    auto through_roi = map_mask_place_preview(params, plane_x, plane_y);
    ASSERT_TRUE(through_roi) << through_roi.error().message;
    EXPECT_NEAR(through_roi.value().first, params.crop_x + plane_x * params.crop_width, 1e-12);
    EXPECT_NEAR(through_roi.value().second, params.crop_y + plane_y * params.crop_height, 1e-12);
}

} // namespace

TEST(StudioPresenterTest, EditIn01PrepareReturnReopenAbandonAcrossRestart)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString photo = directory.filePath(QStringLiteral("editin-source.png"));
    QImage image(64, 48, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(40, 90, 140));
    ASSERT_TRUE(image.save(photo, "PNG"));
    const QString catalog = directory.filePath(QStringLiteral("library.sqlite"));

    StudioPresenter presenter;
    presenter.createCatalogFromPath(catalog);
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }))
        << presenter.errorText().toStdString();
    presenter.importFilePaths({photo});
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.visibleCount() == 1 && !presenter.selectedAssetId().isEmpty() &&
                   !presenter.busy();
        }))
        << presenter.errorText().toStdString();
    const auto source_id = presenter.selectedAssetId();

    presenter.prepareExternalEditorWorkingCopy(
        QVariantMap{{QStringLiteral("editorId"), QStringLiteral("photoshop")},
                    {QStringLiteral("tiffSampleType"), QStringLiteral("uint8")},
                    {QStringLiteral("profile"), QStringLiteral("srgb")},
                    {QStringLiteral("maxEdge"), 48},
                    {QStringLiteral("autoStack"), true},
                    {QStringLiteral("openAfterCreate"), false}});
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.busy() &&
                   presenter.externalEditorSession()
                       .value(QStringLiteral("workingCopyId"))
                       .isValid() &&
                   !presenter.externalEditorSession()
                        .value(QStringLiteral("workingCopyId"))
                        .toString()
                        .isEmpty();
        }))
        << presenter.errorText().toStdString()
        << " session-keys=" << presenter.externalEditorSession().keys().join(",").toStdString();

    const auto working_id =
        presenter.externalEditorSession().value(QStringLiteral("workingCopyId")).toString();
    const auto working_path =
        presenter.externalEditorSession().value(QStringLiteral("workingPath")).toString();
    ASSERT_FALSE(working_id.isEmpty());
    ASSERT_TRUE(QFileInfo::exists(working_path)) << working_path.toStdString();
    EXPECT_TRUE(working_path.endsWith(QStringLiteral("/working.tif")));
    EXPECT_EQ(presenter.externalEditorSession().value(QStringLiteral("machineState")).toString(),
              QStringLiteral("pending"));

    // Simulate Studio restart: clear in-memory session, reopen durable session.
    presenter.clearExternalEditorSession();
    EXPECT_TRUE(presenter.externalEditorSession().isEmpty());
    presenter.reopenExternalEditorWorkingCopy(QString(), false);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.busy() && presenter.externalEditorSession()
                                                .value(QStringLiteral("workingCopyId"))
                                                .toString() == working_id;
        }))
        << presenter.errorText().toStdString();
    EXPECT_EQ(presenter.externalEditorSession().value(QStringLiteral("sourceAssetId")).toString(),
              source_id);

    // Unchanged return is visible as conflict reason.
    presenter.checkExternalEditorReturned(working_id, QString());
    ASSERT_TRUE(wait_until([&] { return !presenter.busy(); }))
        << presenter.errorText().toStdString();
    EXPECT_FALSE(presenter.errorText().isEmpty());
    EXPECT_TRUE(presenter.statusText().contains(QStringLiteral("unchanged")) ||
                presenter.externalEditorSession()
                    .value(QStringLiteral("reason"))
                    .toString()
                    .contains(QStringLiteral("unchanged")) ||
                presenter.errorText().contains(QStringLiteral("unchanged")));

    // Modify working copy and register/auto-stack.
    QImage edited(32, 24, QImage::Format_RGB888);
    edited.fill(QColor(220, 40, 40));
    ASSERT_TRUE(edited.save(working_path, "JPEG", 90));
    presenter.refreshExternalEditorWorkingCopyStatus(working_id);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.busy() && presenter.externalEditorSession()
                                                .value(QStringLiteral("machineState"))
                                                .toString() == QStringLiteral("modified");
        }))
        << presenter.errorText().toStdString() << " reason="
        << presenter.externalEditorSession()
               .value(QStringLiteral("reason"))
               .toString()
               .toStdString();

    presenter.checkExternalEditorReturned(working_id, QString());
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.busy() &&
                   presenter.externalEditorSession().value(QStringLiteral("registered")).toBool();
        }))
        << presenter.errorText().toStdString();
    EXPECT_TRUE(presenter.externalEditorSession().value(QStringLiteral("autoStacked")).toBool());
    const auto derived_id =
        presenter.externalEditorSession().value(QStringLiteral("derivedAssetId")).toString();
    ASSERT_FALSE(derived_id.isEmpty());
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.selectedAssetId() == derived_id ||
                   presenter.externalEditorSession()
                           .value(QStringLiteral("stackPickAssetId"))
                           .toString() == derived_id;
        }))
        << "selected=" << presenter.selectedAssetId().toStdString()
        << " derived=" << derived_id.toStdString();

    // Fresh session then abandon across clear/reopen.
    presenter.clearExternalEditorSession();
    ASSERT_TRUE(wait_until([&] { return !presenter.busy(); }));
    presenter.selectAsset(source_id);
    ASSERT_TRUE(
        wait_until([&] { return presenter.selectedAssetId() == source_id && !presenter.busy(); }));
    presenter.prepareExternalEditorWorkingCopy(
        QVariantMap{{QStringLiteral("editorId"), QStringLiteral("affinity")},
                    {QStringLiteral("tiffSampleType"), QStringLiteral("uint16")},
                    {QStringLiteral("maxEdge"), 40},
                    {QStringLiteral("autoStack"), false},
                    {QStringLiteral("openAfterCreate"), false}});
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.busy() &&
                   !presenter.externalEditorSession()
                        .value(QStringLiteral("workingCopyId"))
                        .toString()
                        .isEmpty() &&
                   !presenter.externalEditorSession().value(QStringLiteral("registered")).toBool();
        }))
        << presenter.errorText().toStdString();
    const auto abandon_id =
        presenter.externalEditorSession().value(QStringLiteral("workingCopyId")).toString();
    const auto abandon_path =
        presenter.externalEditorSession().value(QStringLiteral("workingPath")).toString();
    presenter.clearExternalEditorSession();
    presenter.reopenExternalEditorWorkingCopy(abandon_id, false);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.busy() && presenter.externalEditorSession()
                                                .value(QStringLiteral("workingCopyId"))
                                                .toString() == abandon_id;
        }))
        << presenter.errorText().toStdString();
    presenter.abandonExternalEditorWorkingCopy(abandon_id);
    ASSERT_TRUE(wait_until(
        [&] { return !presenter.busy() && presenter.externalEditorSession().isEmpty(); }))
        << presenter.errorText().toStdString();
    EXPECT_FALSE(QFileInfo::exists(abandon_path)) << abandon_path.toStdString();
}

TEST(StudioPresenterTest, ImportIngestTransportCopyReportsFilesystemCard)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString source_dir = QDir(directory.path()).filePath(QStringLiteral("card/DCIM"));
    ASSERT_TRUE(QDir().mkpath(source_dir));
    const QString dest_dir = QDir(directory.path()).filePath(QStringLiteral("dest"));
    ASSERT_TRUE(QDir().mkpath(dest_dir));
    for (const auto &name : {QStringLiteral("one.png"), QStringLiteral("two.png")})
    {
        QImage image(32, 24, QImage::Format_RGB888);
        image.setColorSpace(QColorSpace(QColorSpace::SRgb));
        image.fill(QColor(name == QStringLiteral("one.png") ? 12 : 90, 34, 56));
        ASSERT_TRUE(image.save(QDir(source_dir).filePath(name), "PNG"));
    }

    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath(QStringLiteral("library.sqlite")));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }))
        << presenter.errorText().toStdString();
    presenter.openImportPage();
    presenter.setImportIngestTransport(QStringLiteral("filesystem-card"));
    presenter.setImportMode(QStringLiteral("copy"));
    presenter.setImportSourceRoot(QDir(directory.path()).filePath(QStringLiteral("card")));
    presenter.setImportDestination(dest_dir);
    ASSERT_TRUE(wait_until(
        [&]
        { return !presenter.importScanActive() && presenter.importCandidates()->rowCount() == 2; },
        30000))
        << presenter.errorText().toStdString();
    EXPECT_TRUE(presenter.importIngestSourceUri().startsWith(
        QStringLiteral("ravo-ingest:filesystem-card:")));
    EXPECT_FALSE(presenter.importNativeSupport().value(QStringLiteral("adapterPackaged")).toBool());
    EXPECT_EQ(presenter.importNativeSupport().value(QStringLiteral("ptpUsb")).toString(),
              QStringLiteral("unsupported"));

    presenter.startPlannedImport();
    ASSERT_TRUE(wait_until(
        [&] { return !presenter.importPreflightActive() && !presenter.importWorkActive(); }, 30000))
        << presenter.errorText().toStdString();
    EXPECT_TRUE(presenter.errorText().isEmpty()) << presenter.errorText().toStdString();
    const auto report = presenter.importIngestReport();
    EXPECT_EQ(report.value(QStringLiteral("transport")).toString(),
              QStringLiteral("filesystem-card"));
    EXPECT_EQ(report.value(QStringLiteral("imported")).toInt(), 2);
    EXPECT_EQ(report.value(QStringLiteral("duplicates")).toInt(), 0);
    EXPECT_EQ(report.value(QStringLiteral("failed")).toInt(), 0);
    EXPECT_TRUE(report.value(QStringLiteral("resumeCheckpointCleared")).toBool());
    EXPECT_EQ(presenter.visibleCount(), 2);

    // Reopening the same card hides every cataloged photo before thumbnail demand.
    presenter.openImportPage();
    ASSERT_TRUE(wait_until([&] { return !presenter.importScanActive(); }, 30000));
    EXPECT_EQ(presenter.importCandidates()->rowCount(), 0);
    EXPECT_EQ(presenter.importDuplicateCount(), 2);
    EXPECT_FALSE(presenter.importReady());
    EXPECT_EQ(presenter.visibleCount(), 2);
}

TEST(StudioPresenterTest, ImportNativeTransportFailsClosedWithoutPackagedAdapter)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString source_dir = QDir(directory.path()).filePath(QStringLiteral("card"));
    ASSERT_TRUE(QDir().mkpath(source_dir));
    QImage image(16, 16, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(1, 2, 3));
    ASSERT_TRUE(image.save(QDir(source_dir).filePath(QStringLiteral("x.png")), "PNG"));

    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath(QStringLiteral("library.sqlite")));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }))
        << presenter.errorText().toStdString();
    presenter.openImportPage();
    presenter.setImportIngestTransport(QStringLiteral("ptp-usb"));
    presenter.setImportMode(QStringLiteral("copy"));
    presenter.setImportSourceRoot(source_dir);
    presenter.setImportDestination(QDir(directory.path()).filePath(QStringLiteral("dest")));
    ASSERT_TRUE(wait_until([&] { return !presenter.importScanActive(); }, 30000));
    presenter.importCandidates()->setAllSelected(true);
    presenter.startPlannedImport();
    ASSERT_TRUE(wait_until(
        [&] { return !presenter.importPreflightActive() && !presenter.importWorkActive(); }, 5000));
    EXPECT_FALSE(presenter.errorText().isEmpty());
    EXPECT_TRUE(presenter.errorText().contains(QStringLiteral("not packaged")) ||
                presenter.errorText().contains(QStringLiteral("native")));
    EXPECT_TRUE(presenter.importIngestReport().isEmpty());
}

} // namespace ravo
