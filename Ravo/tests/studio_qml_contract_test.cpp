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
#include "ravo/desktop/library_set_list_model.h"
#include "ravo/desktop/studio_command_controller.h"
#include "ravo/desktop/studio_live_session_controller.h"
#include "ravo/desktop/studio_presenter.h"
#include "ravo/recipe/color_harmonizer.h"
#include "ravo/recipe/develop_mask.h"
#include "ravo/recipe/style.h"
#include "studio_debug_info.h"
#include "studio_language_manager.h"
#include "studio_qml_test_support.h"

namespace ravo
{
namespace
{

void ensure_qt_core();

void ensure_qt_core()
{
    if (QCoreApplication::instance() != nullptr)
        return;
    static int argc = 1;
    static char executable[] = "ravo-desktop-command-tests";
    static char *argv[] = {executable, nullptr};
    static auto *application = new QCoreApplication(argc, argv);
    static_cast<void>(application);
}

[[nodiscard]] bool wait_until(const std::function<bool()> &ready, const int timeout_ms = 15000)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeout_ms)
    {
        if (ready())
        {
            return true;
        }
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
    return ready();
}

class ScopedEnvironmentVariable
{
public:
    ScopedEnvironmentVariable(const char *name, const QByteArray &value)
        : name_(name)
        , old_value_(qgetenv(name))
        , was_set_(qEnvironmentVariableIsSet(name))
    {
        qputenv(name, value);
    }

    ~ScopedEnvironmentVariable()
    {
        if (was_set_)
            qputenv(name_.constData(), old_value_);
        else
            qunsetenv(name_.constData());
    }

private:
    QByteArray name_;
    QByteArray old_value_;
    bool was_set_ = false;
};

TEST(StudioQmlContract, LightPresentsCommonControlsBeforeSpecializedSettings)
{
    const auto source = combined_develop_qml_source();
    ASSERT_FALSE(source.isEmpty());
    const auto light_begin = source.indexOf(QStringLiteral("sectionId: \"light\""));
    const auto light_end = source.indexOf(QStringLiteral("sectionId: \"curves\""), light_begin);
    ASSERT_GE(light_begin, 0);
    ASSERT_GT(light_end, light_begin);
    const auto light = source.mid(light_begin, light_end - light_begin);

    const auto exposure = light.indexOf(QStringLiteral("previewDevelopNumber(\"exposure\""));
    const auto sigmoid_contrast =
        light.indexOf(QStringLiteral("previewDevelopNumber(\"sigmoidContrast\""));
    const auto raster_contrast = light.indexOf(QStringLiteral("previewDevelopNumber(\"contrast\""));
    const auto highlights = light.indexOf(QStringLiteral("previewDevelopNumber(\"highlights\""));
    const auto shadows = light.indexOf(QStringLiteral("previewDevelopNumber(\"shadows\""));
    const auto whites = light.indexOf(QStringLiteral("previewDevelopNumber(\"whites\""));
    const auto blacks = light.indexOf(QStringLiteral("previewDevelopNumber(\"blacks\""));
    const auto exposure_mode = light.indexOf(QStringLiteral("setDevelopNumber(\"exposureMode\""));
    const auto exposure_black =
        light.indexOf(QStringLiteral("previewDevelopNumber(\"exposureBlack\""));
    const auto deflicker =
        light.indexOf(QStringLiteral("previewDevelopNumber(\"exposureDeflickerPercentile\""));
    const auto sigmoid_heading =
        light.indexOf(QStringLiteral("qsTr(\"Sigmoid Display · Standard SDR\")"));
    const auto sigmoid_skew = light.indexOf(QStringLiteral("previewDevelopNumber(\"sigmoidSkew\""));
    const auto preserve_hue =
        light.indexOf(QStringLiteral("previewDevelopNumber(\"sigmoidHuePreservation\""));
    const auto gamma = light.indexOf(QStringLiteral("previewDevelopNumber(\"gamma\""));
    const auto rgb_levels = light.indexOf(QStringLiteral("qsTr(\"RGB levels\")"));

    ASSERT_GE(exposure, 0);
    ASSERT_GE(sigmoid_contrast, 0);
    ASSERT_GE(raster_contrast, 0);
    ASSERT_GE(highlights, 0);
    ASSERT_GE(shadows, 0);
    ASSERT_GE(whites, 0);
    ASSERT_GE(blacks, 0);
    ASSERT_GE(exposure_mode, 0);
    ASSERT_GE(exposure_black, 0);
    ASSERT_GE(deflicker, 0);
    ASSERT_GE(sigmoid_heading, 0);
    ASSERT_GE(sigmoid_skew, 0);
    ASSERT_GE(preserve_hue, 0);
    ASSERT_GE(gamma, 0);
    ASSERT_GE(rgb_levels, 0);

    EXPECT_LT(exposure, sigmoid_contrast);
    EXPECT_LT(exposure, raster_contrast);
    EXPECT_LT(sigmoid_contrast, highlights);
    EXPECT_LT(raster_contrast, highlights);
    EXPECT_LT(highlights, shadows);
    EXPECT_LT(shadows, whites);
    EXPECT_LT(whites, blacks);
    EXPECT_LT(blacks, exposure_mode);
    EXPECT_LT(exposure_mode, exposure_black);
    EXPECT_LT(exposure_black, deflicker);
    EXPECT_LT(deflicker, sigmoid_heading);
    EXPECT_LT(sigmoid_heading, sigmoid_skew);
    EXPECT_LT(sigmoid_skew, preserve_hue);
    EXPECT_LT(preserve_hue, gamma);
    EXPECT_LT(gamma, rgb_levels);
}

TEST(StudioQmlContract, DevelopPanelUsesDefaultGradingStackWithoutBuryingColorEq)
{
    const auto source = combined_develop_qml_source();
    ASSERT_FALSE(source.isEmpty());
    const auto white_balance = source.indexOf(QStringLiteral("sectionId: \"whiteBalance\""));
    const auto light = source.indexOf(QStringLiteral("sectionId: \"light\""));
    const auto curves = source.indexOf(QStringLiteral("sectionId: \"curves\""));
    const auto color_eq = source.indexOf(QStringLiteral("sectionId: \"colorEqualizer\""));
    const auto color = source.indexOf(QStringLiteral("sectionId: \"color\""));
    const auto geometry = source.indexOf(QStringLiteral("sectionId: \"geometry\""));
    const auto graduated = source.indexOf(QStringLiteral("sectionId: \"graduated\""));
    ASSERT_GE(white_balance, 0);
    ASSERT_GE(light, 0);
    ASSERT_GE(curves, 0);
    ASSERT_GE(color_eq, 0);
    ASSERT_GE(color, 0);
    ASSERT_GE(geometry, 0);
    ASSERT_GE(graduated, 0);
    EXPECT_LT(light, white_balance);
    EXPECT_LT(light, curves);
    EXPECT_LT(curves, color);
    EXPECT_LT(color, white_balance);
    EXPECT_LT(white_balance, color_eq);
    EXPECT_LT(color, geometry);
    EXPECT_LT(geometry, graduated);
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Color Mixer\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Graduated ND\")")));
    EXPECT_FALSE(source.contains(QStringLiteral("Graduated ND / Color EQ")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"colorBalanceShadowsWheel\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"colorBalanceMidtonesWheel\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"colorBalanceHighlightsWheel\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("hueField: \"colorBalanceShadowsHue\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("chromaField: \"colorBalanceShadowsChroma\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"colorBalanceGlobalWheel\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"colorGradingThreeWay\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"colorGradingGlobal\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"colorGradingDetails\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Color Balance RGB · more\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Color · Advanced\")")));
    EXPECT_TRUE(source.contains(
        QStringLiteral("objectName: \"colorEqBand\" + colorMixer.activeBand + \"Hue\"")));
    EXPECT_TRUE(source.contains(
        QStringLiteral("objectName: \"colorEqBand\" + colorMixer.activeBand + \"Saturation\"")));
    EXPECT_TRUE(source.contains(
        QStringLiteral("objectName: \"colorEqBand\" + colorMixer.activeBand + \"Luminance\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("editColorEqBands")));
    EXPECT_TRUE(source.contains(QStringLiteral("satField")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"curveFamily\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"curveChannel\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"curveEditor\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Curves\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Monotonic\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"curveFamilyRgb\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"curveFamilyTone\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"curvePointMode\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"curveParametricMode\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"resetActiveCurve\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Channel\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Parametric regions\")")));
    const auto curve_settings = source.indexOf(QStringLiteral("qsTr(\"Curve settings\")"));
    const auto curve_settings_owner =
        source.lastIndexOf(QStringLiteral("CustomEditPanel {"), curve_settings);
    ASSERT_GE(curve_settings, 0);
    ASSERT_GE(curve_settings_owner, 0);
    const auto curve_settings_section =
        source.mid(curve_settings_owner, curve_settings - curve_settings_owner + 600);
    EXPECT_TRUE(curve_settings_section.contains(QStringLiteral("initialExpanded: false")));
    EXPECT_TRUE(curve_settings_section.contains(
        QStringLiteral("titleBarColor: Theme.contentSurfaceColor")));
    EXPECT_TRUE(curve_settings_section.contains(QStringLiteral("borderColor: Theme.lightColor")));
    EXPECT_TRUE(
        curve_settings_section.contains(QStringLiteral("borderWidth: ControlState.borderFocus")));
    EXPECT_TRUE(
        source.contains(QStringLiteral("curveControls.rgbFamily ? \"rgbCurve\" : \"toneCurve\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("histogramMode")));
    EXPECT_TRUE(source.contains(QStringLiteral("previewCurve")));
    EXPECT_TRUE(source.contains(QStringLiteral("rgbCurveShadows")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Camera Calibration\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("Aqua")));
    EXPECT_TRUE(source.contains(QStringLiteral("Purple")));
    EXPECT_TRUE(source.contains(QStringLiteral("vignetteMidpoint")));
    EXPECT_TRUE(source.contains(QStringLiteral("vignetteCenterX")));
    EXPECT_TRUE(source.contains(QStringLiteral("vignetteCenterY")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Luminance denoise\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Color denoise\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("denoiseChroma")));
    const auto detail_section = source.indexOf(QStringLiteral("sectionId: \"detail\""));
    const auto raw_section = source.indexOf(QStringLiteral("sectionId: \"raw\""));
    const auto luma_denoise = source.indexOf(QStringLiteral("qsTr(\"Luminance denoise\")"));
    ASSERT_GE(detail_section, 0);
    ASSERT_GE(raw_section, 0);
    ASSERT_GE(luma_denoise, 0);
    EXPECT_GT(luma_denoise, detail_section);
    EXPECT_LT(luma_denoise, raw_section);
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"whiteBalancePickActive\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("setWhiteBalancePickActive")));
    QFile main(QStringLiteral(RAVO_STUDIO_MAIN_QML));
    ASSERT_TRUE(main.open(QIODevice::ReadOnly | QIODevice::Text))
        << main.errorString().toStdString();
    const auto main_source = QString::fromUtf8(main.readAll());
    EXPECT_TRUE(main_source.contains(QStringLiteral("pickWhiteBalance")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("whiteBalancePickActive")));
    QFile wheel(QStringLiteral(RAVO_STUDIO_COLOR_GRADE_WHEEL_QML));
    ASSERT_TRUE(wheel.open(QIODevice::ReadOnly | QIODevice::Text))
        << wheel.errorString().toStdString();
    const auto wheel_source = QString::fromUtf8(wheel.readAll());
    EXPECT_TRUE(wheel_source.contains(QStringLiteral("previewDevelopNumbers")));
    EXPECT_TRUE(wheel_source.contains(QStringLiteral("setDevelopNumbers")));
    EXPECT_TRUE(wheel_source.contains(QStringLiteral("DevelopColorSlider")));
    EXPECT_TRUE(wheel_source.contains(QStringLiteral("const hue = (360 - angle) % 360")));
    EXPECT_FALSE(wheel_source.contains(QStringLiteral("OpenCL")));

    QFile color_slider(QStringLiteral(RAVO_STUDIO_DEVELOP_COLOR_SLIDER_QML));
    ASSERT_TRUE(color_slider.open(QIODevice::ReadOnly | QIODevice::Text))
        << color_slider.errorString().toStdString();
    const auto color_slider_source = QString::fromUtf8(color_slider.readAll());
    EXPECT_TRUE(color_slider_source.contains(QStringLiteral("signal valueEdited(double value)")));
    EXPECT_TRUE(
        color_slider_source.contains(QStringLiteral("signal valueCommitted(double value)")));
    EXPECT_TRUE(color_slider_source.contains(QStringLiteral("property Gradient trackGradient")));
    EXPECT_TRUE(color_slider_source.contains(QStringLiteral("pauseAncestorFlickable")));

    auto curve_editor_path = QStringLiteral(RAVO_STUDIO_DEVELOP_PANEL_QML);
    curve_editor_path.replace(QStringLiteral("DevelopPanel.qml"),
                              QStringLiteral("ToneCurveEditor.qml"));
    QFile curve_editor(curve_editor_path);
    ASSERT_TRUE(curve_editor.open(QIODevice::ReadOnly | QIODevice::Text))
        << curve_editor.errorString().toStdString();
    const auto curve_source = QString::fromUtf8(curve_editor.readAll());
    EXPECT_TRUE(curve_source.contains(QStringLiteral("qsTr(\"Input\")")));
    EXPECT_TRUE(curve_source.contains(QStringLiteral("qsTr(\"Output\")")));
    EXPECT_TRUE(curve_source.contains(QStringLiteral("property color curveColor")));
    EXPECT_TRUE(curve_source.contains(QStringLiteral("property bool showRegionSplits")));
    EXPECT_TRUE(curve_source.contains(QStringLiteral("Qt.CrossCursor")));
    EXPECT_TRUE(curve_source.contains(QStringLiteral("root.displayPoints")));
}

TEST(StudioQmlContract, RetouchAuthorsOrderedRegionsThroughCommandBoundary)
{
    const auto source = combined_develop_qml_source();
    ASSERT_FALSE(source.isEmpty());
    EXPECT_TRUE(source.contains(QStringLiteral("id: retouchEditor")));
    EXPECT_TRUE(source.contains(QStringLiteral("panel.presenter.editRetouch.regions")));
    EXPECT_TRUE(source.contains(QStringLiteral("panel.commands.addRetouchRegion")));
    EXPECT_TRUE(source.contains(QStringLiteral("panel.commands.removeRetouchRegion")));
    EXPECT_TRUE(source.contains(QStringLiteral("[\"clone\", \"heal\", \"blur\", \"fill\"]")));
    EXPECT_TRUE(source.contains(QStringLiteral("\"blurType\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("\"fillMode\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("\"sourceX\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Add retouch region\")")));
    EXPECT_FALSE(source.contains(QStringLiteral("apply_retouch")));

    QFile actions(QStringLiteral(RAVO_STUDIO_ACTIONS_QML));
    ASSERT_TRUE(actions.open(QIODevice::ReadOnly | QIODevice::Text))
        << actions.errorString().toStdString();
    const auto action_source = QString::fromUtf8(actions.readAll());
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.editAddRetouchRegion")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.editRemoveRetouchRegion")));
}

TEST(StudioQmlContract, DevelopSlidersPublishUserEditsBeforeRelease)
{
    const auto panel_source = combined_develop_qml_source();
    ASSERT_FALSE(panel_source.isEmpty());
    EXPECT_TRUE(panel_source.contains(QStringLiteral("onValueEdited: function (value)")));
    EXPECT_FALSE(panel_source.contains(QStringLiteral("onValueEdited: if (")));
    EXPECT_FALSE(panel_source.contains(
        QStringLiteral("onValueChanged: if (panel.liveReady && panel.commands)")));
    EXPECT_TRUE(panel_source.contains(QStringLiteral("onValueCommitted: function (value)")));
    EXPECT_TRUE(panel_source.contains(QStringLiteral("previewDevelopNumber")));
    EXPECT_FALSE(panel_source.contains(QStringLiteral("onValueChanged: retouchEditor.")));

    QFile wheel(QStringLiteral(RAVO_STUDIO_COLOR_GRADE_WHEEL_QML));
    ASSERT_TRUE(wheel.open(QIODevice::ReadOnly | QIODevice::Text))
        << wheel.errorString().toStdString();
    const auto wheel_source = QString::fromUtf8(wheel.readAll());
    EXPECT_TRUE(wheel_source.contains(QStringLiteral("onValueEdited: function (value)")));
    EXPECT_FALSE(wheel_source.contains(QStringLiteral("onValueEdited: if (")));
    EXPECT_FALSE(wheel_source.contains(
        QStringLiteral("onValueChanged: if (panel.liveReady && panel.commands)")));
    EXPECT_TRUE(wheel_source.contains(QStringLiteral("onValueCommitted: function (value)")));
}

TEST(StudioQmlContract, DevelopSectionsFollowLightroomEditOrder)
{
    const auto source = combined_develop_qml_source();
    ASSERT_FALSE(source.isEmpty());
    EXPECT_FALSE(source.contains(QStringLiteral("qsTr(\"Undo\")")));
    EXPECT_FALSE(source.contains(QStringLiteral("qsTr(\"Reset all\")")));
    const QStringList order{
        QStringLiteral("light"),
        QStringLiteral("curves"),
        QStringLiteral("color"),
        QStringLiteral("whiteBalance"),
        QStringLiteral("colorEqualizer"),
        QStringLiteral("color"),
        QStringLiteral("primaries"),
        QStringLiteral("geometry"),
        QStringLiteral("toneEqual"),
        QStringLiteral("graduated"),
        QStringLiteral("effects"),
        QStringLiteral("detail"),
        QStringLiteral("raw"),
        QStringLiteral("calibration"),
        QStringLiteral("inputProfile"),
        QStringLiteral("profileGamma"),
        QStringLiteral("outputProfile"),
    };
    qsizetype cursor = 0;
    for (const auto &id : order)
    {
        const auto needle = QStringLiteral("sectionId: \"%1\"").arg(id);
        const auto found = source.indexOf(needle, cursor);
        ASSERT_GE(found, 0) << id.toStdString();
        EXPECT_GT(found, cursor) << id.toStdString();
        cursor = found + needle.size();
    }
    const auto light = source.indexOf(QStringLiteral("sectionId: \"light\""));
    const auto white_balance = source.indexOf(QStringLiteral("sectionId: \"whiteBalance\""), light);
    const auto curves = source.indexOf(QStringLiteral("sectionId: \"curves\""), light);
    ASSERT_GE(light, 0);
    ASSERT_GT(white_balance, light);
    ASSERT_GT(curves, light);
    ASSERT_GT(white_balance, curves);
}

TEST(StudioQmlContract, GeometryCropToolbarUsesIconsAndAspectLock)
{
    const auto source = combined_develop_qml_source();
    ASSERT_FALSE(source.isEmpty());
    EXPECT_FALSE(source.contains(QStringLiteral("qsTr(\"Rotate L\")")));
    EXPECT_FALSE(source.contains(QStringLiteral("qsTr(\"Flip H\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("RotateCcw.svg")));
    EXPECT_TRUE(source.contains(QStringLiteral("RotateCw.svg")));
    EXPECT_TRUE(source.contains(QStringLiteral("FlipHorizontal.svg")));
    EXPECT_TRUE(source.contains(QStringLiteral("FlipVertical.svg")));
    EXPECT_TRUE(source.contains(QStringLiteral("Lock.svg")));
    EXPECT_TRUE(source.contains(QStringLiteral("Unlock.svg")));
    EXPECT_TRUE(source.contains(QStringLiteral("AbstractButton.IconOnly")));
    EXPECT_TRUE(source.contains(QStringLiteral("setCropAspect(checked ? \"locked\" : \"free\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Lock aspect ratio\")")));
    const auto geometry_begin = source.indexOf(QStringLiteral("sectionId: \"geometry\""));
    const auto geometry_end =
        source.indexOf(QStringLiteral("sectionId: \"toneEqual\""), geometry_begin);
    ASSERT_GE(geometry_begin, 0);
    ASSERT_GT(geometry_end, geometry_begin);
    const auto geometry = source.mid(geometry_begin, geometry_end - geometry_begin);
    EXPECT_TRUE(geometry.contains(QStringLiteral("Layout.fillWidth: true")));
    EXPECT_TRUE(geometry.contains(QStringLiteral("qsTr(\"Angle\")")));
    EXPECT_TRUE(geometry.contains(QStringLiteral("\"field\": \"straighten\"")));
    EXPECT_TRUE(geometry.contains(QStringLiteral("\"field\": \"perspectiveVertical\"")));
    EXPECT_TRUE(geometry.contains(QStringLiteral("\"field\": \"perspectiveHorizontal\"")));
    EXPECT_TRUE(geometry.contains(QStringLiteral("\"field\": \"perspectiveShear\"")));
    EXPECT_TRUE(
        geometry.contains(QStringLiteral("panel.commands.autoPerspective(modelData.mode)")));
    EXPECT_TRUE(geometry.contains(QStringLiteral("perspectiveConstrainCrop")));
    EXPECT_TRUE(geometry.contains(QStringLiteral("perspectiveInterpolationIndex")));
}

TEST(StudioQmlContract, DevelopReviewToolbarOffersSynchronizedBeforeAfterComparison)
{
    QFile review(
        QStringLiteral(RAVO_REPOSITORY_ROOT "/Ravo/desktop/qml/gallery/GalleryReviewBar.qml"));
    ASSERT_TRUE(review.open(QIODevice::ReadOnly | QIODevice::Text))
        << review.errorString().toStdString();
    const auto review_source = QString::fromUtf8(review.readAll());
    const auto comparison_button = review_source.indexOf(QStringLiteral("id: comparisonButton"));
    const auto rating_control = review_source.indexOf(QStringLiteral("RatingControl"));
    ASSERT_GE(comparison_button, 0);
    ASSERT_GT(rating_control, comparison_button);
    EXPECT_TRUE(
        review_source.contains(QStringLiteral("objectName: \"beforeAfterComparisonButton\"")));
    EXPECT_TRUE(review_source.contains(QStringLiteral("visible: root.developOpen")));
    EXPECT_TRUE(review_source.contains(
        QStringLiteral("action: root.commands ? root.commands.comparison : null")));
    EXPECT_TRUE(review_source.contains(QStringLiteral("text: qsTr(\"Y|Y\")")));
    EXPECT_TRUE(review_source.contains(QStringLiteral("tooltipText: action ? action.text : \"\"")));

    QFile main_qml(QStringLiteral(RAVO_STUDIO_MAIN_QML));
    ASSERT_TRUE(main_qml.open(QIODevice::ReadOnly | QIODevice::Text))
        << main_qml.errorString().toStdString();
    const auto main_source = QString::fromUtf8(main_qml.readAll());
    EXPECT_TRUE(main_source.contains(QStringLiteral("comparisonReady")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("studio.comparisonBeforeUrl")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("id: comparisonBeforeImage")));
    EXPECT_TRUE(
        main_source.contains(QStringLiteral("return window.comparisonReady ? width * 2 : width")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("text: qsTr(\"Before\")")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("text: qsTr(\"After\")")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("!studio.comparisonActive")));

    QFile actions(QStringLiteral(RAVO_STUDIO_ACTIONS_QML));
    ASSERT_TRUE(actions.open(QIODevice::ReadOnly | QIODevice::Text))
        << actions.errorString().toStdString();
    const auto action_source = QString::fromUtf8(actions.readAll());
    EXPECT_TRUE(action_source.contains(QStringLiteral("property alias comparison")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("root.ids.editComparison")));
}

TEST(StudioQmlContract, EditLeftRailShowsHistoryInsteadOfLibraryFolders)
{
    QFile library(QStringLiteral(RAVO_STUDIO_LIBRARY_SIDE_PANEL_QML));
    ASSERT_TRUE(library.open(QIODevice::ReadOnly | QIODevice::Text))
        << library.errorString().toStdString();
    const auto library_source = QString::fromUtf8(library.readAll());
    EXPECT_TRUE(library_source.contains(QStringLiteral("DevelopPresetPanel")));
    EXPECT_TRUE(library_source.contains(QStringLiteral("DevelopHistoryPanel")));
    const auto preset_panel = library_source.indexOf(QStringLiteral("DevelopPresetPanel"));
    const auto history_panel = library_source.indexOf(QStringLiteral("DevelopHistoryPanel"));
    ASSERT_GE(preset_panel, 0);
    ASSERT_GE(history_panel, 0);
    EXPECT_LT(preset_panel, history_panel);
    EXPECT_TRUE(library_source.contains(QStringLiteral("developOpen")));
    EXPECT_TRUE(library_source.contains(QStringLiteral("visible: !root.developOpen")));
    EXPECT_TRUE(library_source.contains(QStringLiteral("id: zoomModeBar")));
    EXPECT_TRUE(library_source.contains(QStringLiteral("Layout.preferredWidth: 1")));
    EXPECT_TRUE(library_source.contains(QStringLiteral("qsTr(\"Fit\")")));
    EXPECT_TRUE(library_source.contains(QStringLiteral("qsTr(\"Fill\")")));
    EXPECT_TRUE(library_source.contains(QStringLiteral("qsTr(\"1:1\")")));
    EXPECT_TRUE(
        library_source.contains(QStringLiteral("Layout.preferredWidth: ControlState.borderThin")));

    QFile history(QStringLiteral(RAVO_STUDIO_DEVELOP_HISTORY_PANEL_QML));
    ASSERT_TRUE(history.open(QIODevice::ReadOnly | QIODevice::Text))
        << history.errorString().toStdString();
    const auto history_source = QString::fromUtf8(history.readAll());
    EXPECT_TRUE(history_source.contains(QStringLiteral("recipeHistory")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("activeHistoryId")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("activeHistorySeq")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("restoreHistory")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("createSnapshot")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("createSnapshot(\"\")")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("renameSnapshot")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("onDoubleClicked")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("commitRename")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("historyEntries")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("qsTr(\"Original\")")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("\"id\": 0")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("snapshotEntries")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("id: snapshotList")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("qsTr(\"Snapshots\")")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("kind === \"snapshot\"")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("qsTr(\"Undo\")")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("qsTr(\"Redo\")")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("qsTr(\"Reset all\")")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("beforeAfter")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("qsTr(\"Copy Parameters\")")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("qsTr(\"Paste Parameters\")")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("objectName: \"copyParametersButton\"")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("objectName: \"pasteParametersButton\"")));
    EXPECT_FALSE(history_source.contains(QStringLiteral("qsTr(\"Paste Light\")")));
    EXPECT_FALSE(history_source.contains(QStringLiteral("qsTr(\"Paste Color\")")));
    EXPECT_FALSE(history_source.contains(QStringLiteral("pasteEditsSection")));
    EXPECT_LT(history_source.indexOf(QStringLiteral("qsTr(\"Snapshot\")")),
              history_source.indexOf(QStringLiteral("qsTr(\"Copy Parameters\")")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("Layout.preferredWidth: 1")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("copyParameters")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("pasteParameters")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("hasCopiedParameters")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("modifiedParameterChoices")));

    QFile actions(QStringLiteral(RAVO_STUDIO_ACTIONS_QML));
    ASSERT_TRUE(actions.open(QIODevice::ReadOnly | QIODevice::Text))
        << actions.errorString().toStdString();
    const auto action_source = QString::fromUtf8(actions.readAll());
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.photoRenameSnapshot")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.editCopyParameters")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.editCopyParametersSelected")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.editPasteParameters")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.editSetNumbers")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.editPickWhiteBalance")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.editSetWhiteBalancePick")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("previewDevelopNumbers(fields)")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("setDevelopNumbers(fields)")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("copySelectedParameters(fields)")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.editPickWhiteBalance")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("pickWhiteBalance(x, y)")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("maximumLineCount: 1")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("entryText")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("inactive")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("textColor")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("disabledTextColor")));
}

TEST(StudioCommands, LockingCropAspectKeepsCurrentRatio)
{
    ensure_qt_core();
    StudioPresenter presenter;
    EXPECT_EQ(presenter.cropAspect(), QStringLiteral("free"));
    EXPECT_NEAR(presenter.cropAspectRatio(), 0.0, 1e-12);
    presenter.setCropAspect(QStringLiteral("locked"));
    EXPECT_EQ(presenter.cropAspect(), QStringLiteral("locked"));
    EXPECT_NEAR(presenter.cropAspectRatio(), 1.0, 1e-6);
    presenter.setCropAspect(QStringLiteral("free"));
    EXPECT_EQ(presenter.cropAspect(), QStringLiteral("free"));
    EXPECT_NEAR(presenter.cropAspectRatio(), 0.0, 1e-12);
}

TEST(StudioCommands, BuiltinRegistryIsCompleteAndConflictFree)
{
    EXPECT_TRUE(StudioCommandController::validateBuiltinDefinitions().isEmpty());
}

TEST(StudioCommands, CopyDebugTextRequiresSelectionOrPresetPath)
{
    ensure_qt_core();
    StudioPresenter presenter;
    StudioCommandController controller(presenter);
    const auto ids = controller.ids();
    const auto photo_copy = ids.value(QStringLiteral("photoCopyInfo")).toString();
    const auto parameters_copy = ids.value(QStringLiteral("photoCopyParameters")).toString();
    const auto preset_copy = ids.value(QStringLiteral("presetCopyInfo")).toString();
    ASSERT_EQ(photo_copy, QStringLiteral("studio.photo.copy_info"));
    ASSERT_EQ(parameters_copy, QStringLiteral("studio.photo.copy_parameters"));
    ASSERT_EQ(preset_copy, QStringLiteral("studio.preset.copy_info"));

    const auto photo_action = controller.action(photo_copy);
    EXPECT_FALSE(photo_action.value(QStringLiteral("enabled")).toBool());
    EXPECT_FALSE(photo_action.value(QStringLiteral("disabledReason")).toString().isEmpty());
    const auto photo_rejected = controller.executeAction(photo_copy, QStringLiteral("control"));
    EXPECT_FALSE(photo_rejected.value(QStringLiteral("accepted")).toBool());
    EXPECT_EQ(photo_rejected.value(QStringLiteral("code")).toString(),
              QStringLiteral("unavailable"));

    const auto parameters_action = controller.action(parameters_copy);
    EXPECT_FALSE(parameters_action.value(QStringLiteral("enabled")).toBool());
    EXPECT_FALSE(parameters_action.value(QStringLiteral("disabledReason")).toString().isEmpty());
    const auto parameters_rejected =
        controller.executeAction(parameters_copy, QStringLiteral("control"));
    EXPECT_FALSE(parameters_rejected.value(QStringLiteral("accepted")).toBool());
    EXPECT_EQ(parameters_rejected.value(QStringLiteral("code")).toString(),
              QStringLiteral("unavailable"));

    const auto preset_rejected =
        controller.executeCommand(preset_copy, QString{}, QStringLiteral("control"));
    EXPECT_FALSE(preset_rejected.value(QStringLiteral("accepted")).toBool());
    EXPECT_EQ(preset_rejected.value(QStringLiteral("code")).toString(),
              QStringLiteral("unavailable"));
}

TEST(StudioCommands, PresetDeleteRequiresCurrentPathBoundConfirmation)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath(QStringLiteral("library.sqlite")));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }))
        << presenter.errorText().toStdString();
    StudioCommandController controller(presenter);
    const auto ids = controller.ids();
    const QString request_id = ids.value(QStringLiteral("presetDelete")).toString();
    const QString confirmed_id = ids.value(QStringLiteral("presetDeleteConfirmed")).toString();
    ASSERT_EQ(request_id, QStringLiteral("studio.preset.request_delete"));
    ASSERT_EQ(confirmed_id, QStringLiteral("studio.preset.delete"));

    QString presentation_id;
    QVariant presentation_argument;
    QObject::connect(&controller, &StudioCommandController::presentationCommandRequested,
                     &controller,
                     [&](const QString &id, const QVariant &argument)
                     {
                         presentation_id = id;
                         presentation_argument = argument;
                     });
    const QVariantMap preset{{QStringLiteral("path"), QStringLiteral("/tmp/Warm.xmp")},
                             {QStringLiteral("name"), QStringLiteral("Warm")}};
    const auto requested = controller.executeCommand(request_id, preset, QStringLiteral("control"));
    ASSERT_TRUE(requested.value(QStringLiteral("accepted")).toBool());
    ASSERT_EQ(presentation_id, request_id);
    const auto presented = presentation_argument.toMap();
    const QString token = presented.value(QStringLiteral("token")).toString();
    ASSERT_FALSE(token.isEmpty());
    EXPECT_EQ(presented.value(QStringLiteral("path")).toString(),
              preset.value(QStringLiteral("path")).toString());

    const QVariantMap wrong_path{{QStringLiteral("token"), token},
                                 {QStringLiteral("path"), QStringLiteral("/tmp/Other.xmp")}};
    const auto changed =
        controller.executeCommand(confirmed_id, wrong_path, QStringLiteral("control"));
    EXPECT_FALSE(changed.value(QStringLiteral("accepted")).toBool());
    EXPECT_EQ(changed.value(QStringLiteral("code")).toString(), QStringLiteral("invalid_argument"));

    controller.cancelPendingConfirmation(token);
    const QVariantMap canceled{
        {QStringLiteral("token"), token},
        {QStringLiteral("path"), preset.value(QStringLiteral("path")).toString()}};
    const auto after_cancel =
        controller.executeCommand(confirmed_id, canceled, QStringLiteral("control"));
    EXPECT_FALSE(after_cancel.value(QStringLiteral("accepted")).toBool());
    EXPECT_EQ(after_cancel.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_argument"));

    const auto invalid_rename = controller.executeCommand(
        ids.value(QStringLiteral("presetRename")).toString(),
        QVariantMap{{QStringLiteral("path"), QStringLiteral("/tmp/Warm.xmp")}},
        QStringLiteral("control"));
    EXPECT_FALSE(invalid_rename.value(QStringLiteral("accepted")).toBool());
    EXPECT_EQ(invalid_rename.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_argument"));
}

TEST(StudioCommands, CropToolShortcutIsRAndDoesNotRequireEditMode)
{
    ensure_qt_core();
    StudioPresenter presenter;
    StudioCommandController controller(presenter);
    const auto crop = controller.ids().value(QStringLiteral("editCropTool")).toString();
    ASSERT_FALSE(crop.isEmpty());
    bool found_r = false;
    for (const auto &entry_value : controller.shortcutEntries())
    {
        const auto entry = entry_value.toMap();
        if (entry.value(QStringLiteral("actionId")).toString() != crop)
            continue;
        found_r = true;
        EXPECT_EQ(entry.value(QStringLiteral("sequence")).toString(), QStringLiteral("R"));
    }
    EXPECT_TRUE(found_r);
    const auto spec = controller.action(crop);
    EXPECT_FALSE(spec.value(QStringLiteral("enabled")).toBool());
    EXPECT_EQ(spec.value(QStringLiteral("disabledReason")).toString(),
              QCoreApplication::translate("StudioCommands", "Open a library first."));
}

TEST(StudioCommands, PhotoInformationShortcutTogglesSessionOwnedOverlay)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath(QStringLiteral("library.sqlite")));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }))
        << presenter.errorText().toStdString();
    StudioCommandController controller(presenter);
    const auto action_id = controller.ids().value(QStringLiteral("viewPhotoInfo")).toString();
    ASSERT_EQ(action_id, QStringLiteral("studio.view.toggle_photo_info"));

    bool found_i = false;
    for (const auto &entry_value : controller.shortcutEntries())
    {
        const auto entry = entry_value.toMap();
        if (entry.value(QStringLiteral("actionId")).toString() != action_id)
            continue;
        found_i = true;
        EXPECT_EQ(entry.value(QStringLiteral("sequence")).toString(), QStringLiteral("I"));
        EXPECT_TRUE(entry.value(QStringLiteral("enabled")).toBool());
    }
    ASSERT_TRUE(found_i);
    EXPECT_FALSE(controller.photoInfoVisible());
    EXPECT_TRUE(controller.action(action_id).value(QStringLiteral("checkable")).toBool());
    EXPECT_FALSE(controller.action(action_id).value(QStringLiteral("checked")).toBool());

    const auto shown = controller.executeAction(action_id, QStringLiteral("keyboard"));
    EXPECT_TRUE(shown.value(QStringLiteral("accepted")).toBool());
    EXPECT_TRUE(controller.photoInfoVisible());
    EXPECT_TRUE(controller.action(action_id).value(QStringLiteral("checked")).toBool());

    const auto hidden = controller.executeAction(action_id, QStringLiteral("keyboard"));
    EXPECT_TRUE(hidden.value(QStringLiteral("accepted")).toBool());
    EXPECT_FALSE(controller.photoInfoVisible());

    controller.setTextInputActive(true);
    for (const auto &entry_value : controller.shortcutEntries())
    {
        const auto entry = entry_value.toMap();
        if (entry.value(QStringLiteral("actionId")).toString() == action_id)
            EXPECT_FALSE(entry.value(QStringLiteral("enabled")).toBool());
    }
}

TEST(StudioLocalization, CompiledChineseCatalogTranslatesDesktopContexts)
{
    ensure_qt_core();
    QTranslator translator;
    ASSERT_TRUE(
        translator.load(QStringLiteral(RAVO_STUDIO_TRANSLATION_DIR "/RavoStudio_zh_CN.qm")));
    ASSERT_TRUE(QCoreApplication::installTranslator(&translator));

    EXPECT_EQ(QCoreApplication::translate("SettingsPage", "Language"), QStringLiteral("语言"));
    EXPECT_EQ(QCoreApplication::translate("StudioCommands", "Open a library first."),
              QStringLiteral("请先打开图库。"));
    EXPECT_EQ(QCoreApplication::translate("StudioCommands", "Photo Information"),
              QStringLiteral("照片信息"));
    EXPECT_EQ(QCoreApplication::translate("StudioPresenter", "Library opened."),
              QStringLiteral("图库已打开。"));
    EXPECT_EQ(QCoreApplication::translate("DevelopPanel", "RGB Primaries"),
              QStringLiteral("RGB 原色"));
    EXPECT_EQ(QCoreApplication::translate("DevelopPanel", "Unbreak input profile"),
              QStringLiteral("修正输入配置文件"));
    EXPECT_EQ(QCoreApplication::translate("DevelopPanel", "Color look-up table · D50 Lab"),
              QStringLiteral("颜色查找表 · D50 Lab"));
    EXPECT_EQ(QCoreApplication::translate("DevelopPanel", "Color Correction · D50 Lab"),
              QStringLiteral("色彩校正 · D50 Lab"));
    EXPECT_EQ(QCoreApplication::translate("DevelopPanel", "Presence"), QStringLiteral("鲜艳度"));
    EXPECT_EQ(QCoreApplication::translate("DevelopPanel", "Color Grading"),
              QStringLiteral("颜色分级"));
    EXPECT_EQ(QCoreApplication::translate("DevelopPanel", "Color Mixer"),
              QStringLiteral("颜色混合器"));
    EXPECT_EQ(QCoreApplication::translate("LibrarySidePanel", "Last Imported Photos"),
              QStringLiteral("上次导入的照片"));
    EXPECT_EQ(QCoreApplication::translate("ImportPage", "Build Previews"),
              QStringLiteral("构建预览"));
    EXPECT_EQ(QCoreApplication::translate("ImportPage", "Preserve hierarchy"),
              QStringLiteral("保留目录层级"));
    EXPECT_EQ(QCoreApplication::translate("DevelopPanel", "Allow extended chroma"),
              QStringLiteral("允许扩展色度"));
    EXPECT_EQ(QCoreApplication::translate("DevelopHistoryPanel", "Copy Parameters"),
              QStringLiteral("复制参数"));
    EXPECT_EQ(QCoreApplication::translate("DevelopHistoryPanel", "Paste Parameters"),
              QStringLiteral("粘贴参数"));
    EXPECT_EQ(QCoreApplication::translate("ExportOptionsDialog", "Format"), QStringLiteral("格式"));
    EXPECT_EQ(QCoreApplication::translate("ExportOptionsDialog", "Continue"),
              QStringLiteral("继续"));
    EXPECT_EQ(QCoreApplication::translate("ExportOptionsDialog", "Automatic"),
              QStringLiteral("自动"));
    EXPECT_EQ(QCoreApplication::translate("ExportOptionsDialog",
                                          "Write grayscale when the image is neutral"),
              QStringLiteral("图像为中性时写入灰度"));
    EXPECT_EQ(QCoreApplication::translate("StudioCommands", "Export path must be a string."),
              QStringLiteral("导出路径必须是字符串。"));
    EXPECT_EQ(QCoreApplication::translate("StudioExport",
                                          "Export path suffix does not match the selected format"),
              QStringLiteral("文件扩展名与所选导出格式不匹配。"));
    EXPECT_EQ(QCoreApplication::translate("StudioExport", "JPEG quality must be between 5 and 100"),
              QStringLiteral("JPEG 质量必须在 5 到 100 之间"));

    QCoreApplication::removeTranslator(&translator);
}

TEST(StudioLocalization, EveryManifestCatalogActivates)
{
    ensure_qt_core();
    StudioLanguageManager manager(QStringList{QStringLiteral(RAVO_STUDIO_TRANSLATION_DIR)});
    ASSERT_TRUE(manager.initialize(QStringLiteral("en_US"))) << manager.lastError().toStdString();
    for (const auto &language : manager.supportedLanguages())
    {
        ASSERT_TRUE(manager.initialize(language))
            << language.toStdString() << ": " << manager.lastError().toStdString();
        EXPECT_EQ(manager.language(), language);
    }
}

TEST(StudioCommands, CommandPaletteUsesQtPortablePrimaryModifierPolicy)
{
    EXPECT_EQ(StudioCommandController::paletteShortcutForPlatform(QStringLiteral("macos")),
              QStringLiteral("Ctrl+Shift+P"));
    EXPECT_EQ(StudioCommandController::paletteShortcutForPlatform(QStringLiteral("windows")),
              QStringLiteral("Ctrl+Shift+P"));
    EXPECT_EQ(StudioCommandController::paletteShortcutForPlatform(QStringLiteral("linux")),
              QStringLiteral("Ctrl+Shift+P"));

#ifdef Q_OS_MACOS
    const auto native =
        QKeySequence::fromString(
            StudioCommandController::paletteShortcutForPlatform(QStringLiteral("macos")),
            QKeySequence::PortableText)
            .toString(QKeySequence::NativeText);
    EXPECT_TRUE(native.contains(QChar(0x2318))) << native.toStdString();
#endif
}

TEST(StudioCommands, FuzzySearchSupportsPrefixesSubsequencesAndMultipleTokens)
{
    const auto exact = StudioCommandController::fuzzyScore(
        QStringLiteral("Show Command Palette"), QStringLiteral("View"),
        {QStringLiteral("commands"), QStringLiteral("search")},
        QStringLiteral("studio.window.show_command_palette"), QStringLiteral("show command"));
    const auto subsequence = StudioCommandController::fuzzyScore(
        QStringLiteral("Show Command Palette"), QStringLiteral("View"),
        {QStringLiteral("commands"), QStringLiteral("search")},
        QStringLiteral("studio.window.show_command_palette"), QStringLiteral("scpal"));
    const auto missing = StudioCommandController::fuzzyScore(
        QStringLiteral("Show Command Palette"), QStringLiteral("View"),
        {QStringLiteral("commands"), QStringLiteral("search")},
        QStringLiteral("studio.window.show_command_palette"), QStringLiteral("export raw"));

    EXPECT_GT(exact, subsequence);
    EXPECT_GE(subsequence, 0);
    EXPECT_EQ(missing, -1);
}

TEST(StudioCommands, FuzzySearchNormalizesCaseWidthAndDiacritics)
{
    EXPECT_GE(StudioCommandController::fuzzyScore(
                  QStringLiteral("Réglages"), QStringLiteral("Window"),
                  {QStringLiteral("preferences")}, QStringLiteral("studio.window.show_settings"),
                  QStringLiteral("REGLAGES")),
              0);
}

TEST(StudioCommands, ControllerRevalidatesStateAndRejectsInvalidDispatch)
{
    ensure_qt_core();
    StudioPresenter presenter;
    StudioCommandController controller(presenter);
    const auto ids = controller.ids();

    const auto import_action =
        controller.action(ids.value(QStringLiteral("libraryImportFiles")).toString());
    EXPECT_FALSE(import_action.value(QStringLiteral("enabled")).toBool());
    EXPECT_FALSE(import_action.value(QStringLiteral("disabledReason")).toString().isEmpty());

    const auto invalid_path =
        controller.executeCommand(ids.value(QStringLiteral("libraryCreatePath")).toString(),
                                  QString{}, QStringLiteral("control"));
    EXPECT_FALSE(invalid_path.value(QStringLiteral("accepted")).toBool());
    EXPECT_EQ(invalid_path.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_argument"));

    const auto unexpected_argument =
        controller.executeCommand(ids.value(QStringLiteral("windowCommandPalette")).toString(), 1,
                                  QStringLiteral("keyboard"));
    EXPECT_FALSE(unexpected_argument.value(QStringLiteral("accepted")).toBool());
    EXPECT_EQ(unexpected_argument.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_argument"));

    const auto opened = controller.executeAction(
        ids.value(QStringLiteral("windowCommandPalette")).toString(), QStringLiteral("keyboard"));
    EXPECT_TRUE(opened.value(QStringLiteral("accepted")).toBool());
    EXPECT_TRUE(controller.paletteOpen());

    controller.setPaletteOpen(false);
    bool found_palette_shortcut = false;
    for (const auto &entry_value : controller.shortcutEntries())
    {
        const auto entry = entry_value.toMap();
        if (entry.value(QStringLiteral("actionId")).toString() !=
            ids.value(QStringLiteral("windowCommandPalette")).toString())
            continue;
        found_palette_shortcut = true;
        EXPECT_EQ(entry.value(QStringLiteral("sequence")).toString(),
                  QStringLiteral("Ctrl+Shift+P"));
        EXPECT_TRUE(entry.value(QStringLiteral("enabled")).toBool());
    }
    EXPECT_TRUE(found_palette_shortcut);
}

TEST(StudioCommands, ExportWriteRevalidatesCatalogAndRejectsLegacyFilterPayload)
{
    ensure_qt_core();
    StudioPresenter presenter;
    StudioCommandController controller(presenter);
    const auto ids = controller.ids();
    const auto export_write = ids.value(QStringLiteral("libraryExportWrite")).toString();
    const auto export_batch_write = ids.value(QStringLiteral("libraryExportBatchWrite")).toString();
    const auto export_open = ids.value(QStringLiteral("libraryExport")).toString();
    EXPECT_FALSE(export_batch_write.isEmpty());

    const auto open_action = controller.action(export_open);
    EXPECT_FALSE(open_action.value(QStringLiteral("enabled")).toBool());
    EXPECT_FALSE(open_action.value(QStringLiteral("disabledReason")).toString().isEmpty());

    const auto unavailable = controller.executeCommand(
        export_write,
        QVariantMap{{QStringLiteral("path"), QStringLiteral("/tmp/out.jpg")},
                    {QStringLiteral("format"), QStringLiteral("jpeg")},
                    {QStringLiteral("options"),
                     QVariantMap{{QStringLiteral("quality"), 95},
                                 {QStringLiteral("jpegSubsampling"), QStringLiteral("auto")}}}},
        QStringLiteral("control"));
    EXPECT_FALSE(unavailable.value(QStringLiteral("accepted")).toBool());
    EXPECT_EQ(unavailable.value(QStringLiteral("code")).toString(), QStringLiteral("unavailable"));

    const auto legacy_filter = controller.executeCommand(
        export_write,
        QVariantMap{{QStringLiteral("path"), QStringLiteral("/tmp/out.jpg")},
                    {QStringLiteral("filter"), QStringLiteral("JPEG (*.jpg *.jpeg)")}},
        QStringLiteral("control"));
    EXPECT_FALSE(legacy_filter.value(QStringLiteral("accepted")).toBool());
    EXPECT_EQ(legacy_filter.value(QStringLiteral("code")).toString(),
              QStringLiteral("unavailable"));

    const auto unavailable_batch = controller.executeCommand(
        export_batch_write,
        QVariantMap{{QStringLiteral("directory"), QStringLiteral("/tmp")},
                    {QStringLiteral("filenameTemplate"), QStringLiteral("{stem}-{sequence}{ext}")},
                    {QStringLiteral("format"), QStringLiteral("png")},
                    {QStringLiteral("options"), QVariantMap{}}},
        QStringLiteral("control"));
    EXPECT_FALSE(unavailable_batch.value(QStringLiteral("accepted")).toBool());
    EXPECT_EQ(unavailable_batch.value(QStringLiteral("code")).toString(),
              QStringLiteral("unavailable"));
}

TEST(StudioPresenterTest, ExportPresentationCatalogExposesCanonicalDefaults)
{
    ensure_qt_core();
    StudioPresenter presenter;
    const auto formats = presenter.exportFormatChoices();
    ASSERT_EQ(formats.size(), 4);
    EXPECT_EQ(formats.at(0).toMap().value(QStringLiteral("id")).toString(), QStringLiteral("jpeg"));
    EXPECT_EQ(formats.at(3).toMap().value(QStringLiteral("id")).toString(),
              QStringLiteral("original"));
    const auto defaults = presenter.exportDefaultOptions();
    EXPECT_EQ(defaults.value(QStringLiteral("format")).toString(), QStringLiteral("jpeg"));
    EXPECT_EQ(defaults.value(QStringLiteral("quality")).toInt(), 95);
    EXPECT_EQ(defaults.value(QStringLiteral("jpegSubsampling")).toString(), QStringLiteral("auto"));
    EXPECT_EQ(defaults.value(QStringLiteral("pngBitDepth")).toString(), QStringLiteral("8"));
    EXPECT_EQ(defaults.value(QStringLiteral("pngCompression")).toInt(), 5);
    EXPECT_EQ(defaults.value(QStringLiteral("tiffSampleType")).toString(), QStringLiteral("uint8"));
    EXPECT_EQ(defaults.value(QStringLiteral("tiffCompression")).toString(),
              QStringLiteral("deflate_predictor"));
    EXPECT_EQ(defaults.value(QStringLiteral("tiffCompressionLevel")).toInt(), 6);
    EXPECT_FALSE(defaults.value(QStringLiteral("tiffGrayscaleIfNeutral")).toBool());
    EXPECT_EQ(defaults.value(QStringLiteral("tiffResolutionDpi")).toInt(), 300);
    EXPECT_EQ(defaults.value(QStringLiteral("metadataMode")).toString(), QStringLiteral("full"));
    const auto metadata_modes = presenter.exportMetadataModeChoices();
    ASSERT_EQ(metadata_modes.size(), 3);
    EXPECT_EQ(metadata_modes.at(1).toMap().value(QStringLiteral("id")).toString(),
              QStringLiteral("no-location"));
    const auto bounds = presenter.exportOptionBounds();
    EXPECT_EQ(bounds.value(QStringLiteral("jpegQualityMin")).toInt(), 5);
    EXPECT_EQ(bounds.value(QStringLiteral("tiffResolutionDpiMax")).toInt(), 9600);
}

TEST(StudioPresenterTest, OutputDitherPresentationOwnsAllFrozenMethods)
{
    ensure_qt_core();
    StudioPresenter presenter;
    const auto dither = presenter.editOutputDither();
    EXPECT_FALSE(dither.value(QStringLiteral("present")).toBool());
    EXPECT_FALSE(dither.value(QStringLiteral("enabled")).toBool());
    EXPECT_EQ(dither.value(QStringLiteral("methodIndex")).toInt(), 10);
    EXPECT_FALSE(dither.value(QStringLiteral("dampingVisible")).toBool());
    const auto choices = dither.value(QStringLiteral("methodChoices")).toList();
    ASSERT_EQ(choices.size(), static_cast<qsizetype>(kOutputDitherMethodCount));
    EXPECT_EQ(choices.front().toMap().value(QStringLiteral("id")).toString(),
              QStringLiteral("random"));
    EXPECT_EQ(choices.back().toMap().value(QStringLiteral("id")).toString(),
              QStringLiteral("posterize_8"));
    const auto canvas = presenter.editCanvas();
    EXPECT_FALSE(presenter.editCanvasEnabled());
    EXPECT_FALSE(canvas.value(QStringLiteral("enabled")).toBool());
    EXPECT_EQ(canvas.value(QStringLiteral("colorChoices")).toList().size(), 5);
    const auto frame = presenter.editOutputFrame();
    EXPECT_FALSE(frame.value(QStringLiteral("enabled")).toBool());
    EXPECT_EQ(frame.value(QStringLiteral("orientationChoices")).toList().size(), 3);
    EXPECT_EQ(frame.value(QStringLiteral("basisChoices")).toList().size(), 5);
    const auto watermark = presenter.editWatermark();
    EXPECT_FALSE(watermark.value(QStringLiteral("enabled")).toBool());
    EXPECT_EQ(watermark.value(QStringLiteral("text")).toString(), QStringLiteral("RAVO"));
    EXPECT_EQ(watermark.value(QStringLiteral("alignmentChoices")).toList().size(), 9);
    const auto zones = presenter.editColorZones();
    EXPECT_FALSE(zones.value(QStringLiteral("enabled")).toBool());
    EXPECT_FALSE(zones.value(QStringLiteral("editable")).toBool());
    EXPECT_EQ(zones.value(QStringLiteral("selectByChoices")).toList().size(), 3);
    EXPECT_EQ(zones.value(QStringLiteral("interpolationChoices")).toList().size(), 3);
    const auto monochrome = presenter.editMonochromeFilter();
    EXPECT_FALSE(monochrome.value(QStringLiteral("enabled")).toBool());
    EXPECT_DOUBLE_EQ(monochrome.value(QStringLiteral("size")).toDouble(), 2.0);
    EXPECT_DOUBLE_EQ(monochrome.value(QStringLiteral("mix")).toDouble(), 1.0);
    const auto split = presenter.editSplitToning();
    EXPECT_FALSE(split.value(QStringLiteral("enabled")).toBool());
    EXPECT_DOUBLE_EQ(split.value(QStringLiteral("shadowSaturation")).toDouble(), 0.5);
    EXPECT_DOUBLE_EQ(split.value(QStringLiteral("compress")).toDouble(), 33.0);
}

TEST(StudioPresenterTest, CatalogRecoveryCommandsBackupVerifyRestoreAndRebuild)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString photo = directory.filePath(QStringLiteral("recovery-photo.png"));
    QImage image(48, 32, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(70, 120, 180));
    ASSERT_TRUE(image.save(photo, "PNG"));
    const QString catalog = directory.filePath(QStringLiteral("library.sqlite"));
    const QString backup = directory.filePath(QStringLiteral("library.ravobackup"));
    const QString restored = directory.filePath(QStringLiteral("restored.sqlite"));
    const QString scheduled_backups = directory.filePath(QStringLiteral("scheduled-backups"));
    ASSERT_TRUE(QDir().mkdir(scheduled_backups));

    StudioPresenter presenter;
    StudioCommandController controller(presenter);
    presenter.createCatalogFromPath(catalog);
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }))
        << presenter.errorText().toStdString();
    presenter.importFilePaths({photo});
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.visibleCount() == 1 && !presenter.selectedAssetId().isEmpty() &&
                   !presenter.busy() && !presenter.importWorkActive() &&
                   !presenter.previewLoading();
        }))
        << presenter.errorText().toStdString();

    const auto ids = controller.ids();
    EXPECT_EQ(ids.value(QStringLiteral("libraryRecoveryStatus")).toString(),
              QStringLiteral("studio.library.recovery_status"));
    EXPECT_EQ(ids.value(QStringLiteral("libraryBackupRestorePaths")).toString(),
              QStringLiteral("studio.library.backup_restore_paths"));
    EXPECT_EQ(ids.value(QStringLiteral("libraryPreviewRebuildSelected")).toString(),
              QStringLiteral("studio.library.preview_rebuild_selected"));
    EXPECT_EQ(ids.value(QStringLiteral("libraryBackupSchedulePath")).toString(),
              QStringLiteral("studio.library.backup_schedule_path"));

    auto status =
        controller.executeCommand(ids.value(QStringLiteral("libraryRecoveryStatus")).toString(), {},
                                  QStringLiteral("control"));
    ASSERT_TRUE(status.value(QStringLiteral("accepted")).toBool());
    ASSERT_TRUE(
        wait_until([&] { return !presenter.catalogOperationActive() && !presenter.busy(); }))
        << presenter.errorText().toStdString();
    EXPECT_EQ(presenter.recoveryPendingCount(), 0);

    auto created =
        controller.executeCommand(ids.value(QStringLiteral("libraryBackupCreatePath")).toString(),
                                  backup, QStringLiteral("control"));
    ASSERT_TRUE(created.value(QStringLiteral("accepted")).toBool());
    ASSERT_TRUE(
        wait_until([&] { return !presenter.catalogOperationActive() && !presenter.busy(); }, 30000))
        << presenter.errorText().toStdString();
    EXPECT_TRUE(QFileInfo::exists(backup + QStringLiteral("/manifest.json")));

    auto verified =
        controller.executeCommand(ids.value(QStringLiteral("libraryBackupVerifyPath")).toString(),
                                  backup, QStringLiteral("control"));
    ASSERT_TRUE(verified.value(QStringLiteral("accepted")).toBool());
    ASSERT_TRUE(
        wait_until([&] { return !presenter.catalogOperationActive() && !presenter.busy(); }, 30000))
        << presenter.errorText().toStdString();
    EXPECT_TRUE(presenter.errorText().isEmpty());
    const QVariantMap schedule{{QStringLiteral("directory"), scheduled_backups},
                               {QStringLiteral("intervalMinutes"), 15},
                               {QStringLiteral("retentionCount"), 2}};
    auto scheduled =
        controller.executeCommand(ids.value(QStringLiteral("libraryBackupSchedulePath")).toString(),
                                  schedule, QStringLiteral("control"));
    ASSERT_TRUE(scheduled.value(QStringLiteral("accepted")).toBool());
    ASSERT_TRUE(
        wait_until([&] { return !presenter.catalogOperationActive() && !presenter.busy(); }))
        << presenter.errorText().toStdString();
    auto schedule_status = presenter.backupScheduleStatus();
    EXPECT_TRUE(schedule_status.value(QStringLiteral("loaded")).toBool());
    EXPECT_TRUE(schedule_status.value(QStringLiteral("enabled")).toBool());
    EXPECT_EQ(schedule_status.value(QStringLiteral("intervalMinutes")).typeId(),
              QMetaType::LongLong);
    EXPECT_EQ(schedule_status.value(QStringLiteral("lastSuccessUnixMs")).typeId(),
              QMetaType::LongLong);
    EXPECT_EQ(schedule_status.value(QStringLiteral("nextRunUnixMs")).typeId(), QMetaType::LongLong);
    EXPECT_EQ(schedule_status.value(QStringLiteral("retentionCount")).toInt(), 2);

    auto run_schedule =
        controller.executeCommand(ids.value(QStringLiteral("libraryBackupScheduleRun")).toString(),
                                  {}, QStringLiteral("control"));
    ASSERT_TRUE(run_schedule.value(QStringLiteral("accepted")).toBool());
    ASSERT_TRUE(
        wait_until([&] { return !presenter.catalogOperationActive() && !presenter.busy(); }, 30000))
        << presenter.errorText().toStdString();
    schedule_status = presenter.backupScheduleStatus();
    EXPECT_GT(schedule_status.value(QStringLiteral("lastSuccessUnixMs")).toLongLong(), 0);
    EXPECT_GT(schedule_status.value(QStringLiteral("lastBackupBytes")).toULongLong(), 0U);

    auto disable_schedule = controller.executeCommand(
        ids.value(QStringLiteral("libraryBackupScheduleDisable")).toString(), {},
        QStringLiteral("control"));
    ASSERT_TRUE(disable_schedule.value(QStringLiteral("accepted")).toBool());
    ASSERT_TRUE(
        wait_until([&] { return !presenter.catalogOperationActive() && !presenter.busy(); }))
        << presenter.errorText().toStdString();
    EXPECT_FALSE(presenter.backupScheduleStatus().value(QStringLiteral("enabled")).toBool());

    const QVariantMap restore_paths{{QStringLiteral("backup"), backup},
                                    {QStringLiteral("catalog"), restored}};
    auto restored_command =
        controller.executeCommand(ids.value(QStringLiteral("libraryBackupRestorePaths")).toString(),
                                  restore_paths, QStringLiteral("control"));
    ASSERT_TRUE(restored_command.value(QStringLiteral("accepted")).toBool());
    ASSERT_TRUE(
        wait_until([&] { return !presenter.catalogOperationActive() && !presenter.busy(); }, 30000))
        << presenter.errorText().toStdString();
    EXPECT_TRUE(QFileInfo::exists(restored));
    EXPECT_TRUE(QFileInfo(restored + QStringLiteral(".ravo/sidecars")).isDir());

    auto rebuilt = controller.executeCommand(
        ids.value(QStringLiteral("libraryPreviewRebuildSelected")).toString(), {},
        QStringLiteral("control"));
    ASSERT_TRUE(rebuilt.value(QStringLiteral("accepted")).toBool());
    ASSERT_TRUE(
        wait_until([&] { return !presenter.catalogOperationActive() && !presenter.busy(); }, 30000))
        << presenter.errorText().toStdString();
    EXPECT_TRUE(presenter.errorText().isEmpty());
    const auto cancel_action =
        controller.action(ids.value(QStringLiteral("libraryCancelOperation")).toString());
    EXPECT_FALSE(cancel_action.value(QStringLiteral("enabled")).toBool());
}

TEST(StudioPresenterTest, MissingFolderRelinkUsesStableIdentityAndCommandOwnedDialog)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString original = directory.filePath(QStringLiteral("original-root"));
    const QString replacement = directory.filePath(QStringLiteral("replacement-root"));
    ASSERT_TRUE(QDir().mkdir(original));
    const QString photo = QDir(original).filePath(QStringLiteral("photo.png"));
    QImage image(32, 24, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(60, 110, 160));
    ASSERT_TRUE(image.save(photo, "PNG"));
    QFile source(photo);
    ASSERT_TRUE(source.open(QIODevice::ReadOnly));
    const auto source_hash = QCryptographicHash::hash(source.readAll(), QCryptographicHash::Sha256);
    source.close();
    const QString catalog = directory.filePath(QStringLiteral("library.sqlite"));
    QString folder_id;
    {
        StudioPresenter presenter;
        presenter.createCatalogFromPath(catalog);
        ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
        presenter.importFilePaths({photo});
        ASSERT_TRUE(wait_until(
            [&]
            {
                return presenter.visibleCount() == 1 && !presenter.importWorkActive() &&
                       !presenter.busy();
            }));
        for (int row = 0; row < presenter.folders()->rowCount(); ++row)
        {
            const auto index = presenter.folders()->index(row, 0);
            if (presenter.folders()->data(index, FolderListModel::DisplayNameRole).toString() ==
                QStringLiteral("original-root"))
                folder_id =
                    presenter.folders()->data(index, FolderListModel::FolderIdRole).toString();
        }
        ASSERT_FALSE(folder_id.isEmpty());
    }
    ASSERT_TRUE(QDir().rename(original, replacement));

    StudioPresenter presenter;
    StudioCommandController controller(presenter);
    presenter.openCatalogFromPath(catalog);
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }))
        << presenter.errorText().toStdString();
    bool missing = false;
    for (int row = 0; row < presenter.folders()->rowCount(); ++row)
    {
        const auto index = presenter.folders()->index(row, 0);
        if (presenter.folders()->data(index, FolderListModel::FolderIdRole).toString() == folder_id)
            missing = presenter.folders()->data(index, FolderListModel::MissingRole).toBool();
    }
    EXPECT_TRUE(missing);
    const auto ids = controller.ids();
    EXPECT_EQ(ids.value(QStringLiteral("libraryFolderRelinkPath")).toString(),
              QStringLiteral("studio.library.folder_relink_path"));
    const QVariantMap relink{{QStringLiteral("folderId"), folder_id},
                             {QStringLiteral("directory"), replacement}};
    const auto command =
        controller.executeCommand(ids.value(QStringLiteral("libraryFolderRelinkPath")).toString(),
                                  relink, QStringLiteral("control"));
    ASSERT_TRUE(command.value(QStringLiteral("accepted")).toBool());
    ASSERT_TRUE(
        wait_until([&] { return !presenter.catalogOperationActive() && !presenter.busy(); }, 30000))
        << presenter.errorText().toStdString();
    ASSERT_TRUE(wait_until([&] { return presenter.visibleCount() == 1; }));
    bool found = false;
    for (int row = 0; row < presenter.folders()->rowCount(); ++row)
    {
        const auto index = presenter.folders()->index(row, 0);
        if (presenter.folders()->data(index, FolderListModel::FolderIdRole).toString() != folder_id)
            continue;
        found = true;
        EXPECT_FALSE(presenter.folders()->data(index, FolderListModel::MissingRole).toBool());
        EXPECT_EQ(presenter.folders()->data(index, FolderListModel::DisplayNameRole).toString(),
                  QStringLiteral("replacement-root"));
    }
    EXPECT_TRUE(found);
    QFile moved(QDir(replacement).filePath(QStringLiteral("photo.png")));
    ASSERT_TRUE(moved.open(QIODevice::ReadOnly));
    EXPECT_EQ(QCryptographicHash::hash(moved.readAll(), QCryptographicHash::Sha256), source_hash);
}

TEST(StudioPresenterTest, ImportCancellationStopsUndispatchedItemsAtItemBoundary)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QStringList photos;
    std::vector<QByteArray> hashes;
    for (int index = 0; index < 3; ++index)
    {
        const auto path = directory.filePath(QStringLiteral("import-%1.png").arg(index));
        QImage image(64, 48, QImage::Format_RGB888);
        image.setColorSpace(QColorSpace(QColorSpace::SRgb));
        image.fill(QColor(40 + index * 30, 90, 150));
        ASSERT_TRUE(image.save(path, "PNG"));
        photos.push_back(path);
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        hashes.push_back(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256));
    }
    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath(QStringLiteral("library.sqlite")));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    bool cancelled = false;
    QObject::connect(&presenter, &StudioPresenter::libraryWorkChanged, &presenter,
                     [&]
                     {
                         if (!cancelled && presenter.importWorkActive() &&
                             presenter.importWorkCompleted() == 1)
                         {
                             cancelled = true;
                             presenter.cancelCatalogOperation();
                         }
                     });
    presenter.importFilePaths(photos);
    ASSERT_TRUE(wait_until([&] { return cancelled && !presenter.importWorkActive(); }, 30000))
        << presenter.errorText().toStdString();
    EXPECT_EQ(presenter.visibleCount(), 1);
    EXPECT_TRUE(presenter.lastImportAvailable());
    EXPECT_TRUE(presenter.lastImportSelected());
    EXPECT_EQ(presenter.lastImportCount(), 1);
    EXPECT_TRUE(presenter.statusText().contains(QStringLiteral("cancel"), Qt::CaseInsensitive));
    for (int index = 0; index < photos.size(); ++index)
    {
        QFile file(photos[index]);
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        EXPECT_EQ(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256),
                  hashes[static_cast<std::size_t>(index)]);
    }
}

TEST(StudioPresenterTest, ImportKeepsGalleryStableThenPublishesOneLastImportCollection)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto make_photo = [&](const QString &name, const QColor &color)
    {
        const QString path = directory.filePath(name);
        QImage image(64, 48, QImage::Format_RGB888);
        image.setColorSpace(QColorSpace(QColorSpace::SRgb));
        image.fill(color);
        EXPECT_TRUE(image.save(path, "PNG"));
        return path;
    };
    const QString baseline = make_photo(QStringLiteral("baseline.png"), QColor(20, 40, 60));
    const QString corrupt = directory.filePath(QStringLiteral("batch-corrupt.png"));
    QFile corrupt_file(corrupt);
    ASSERT_TRUE(corrupt_file.open(QIODevice::WriteOnly));
    ASSERT_EQ(corrupt_file.write("not a png"), 9);
    corrupt_file.close();
    const QStringList batch{make_photo(QStringLiteral("batch-a.png"), QColor(80, 20, 30)),
                            make_photo(QStringLiteral("batch-b.png"), QColor(20, 90, 40)),
                            make_photo(QStringLiteral("batch-c.png"), QColor(30, 40, 120)),
                            corrupt};

    StudioPresenter presenter;
    StudioCommandController commands(presenter);
    presenter.createCatalogFromPath(directory.filePath(QStringLiteral("library.sqlite")));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.importFilePaths({baseline});
    ASSERT_TRUE(wait_until([&] { return !presenter.importWorkActive(); }, 30000))
        << presenter.errorText().toStdString();
    ASSERT_TRUE(presenter.lastImportSelected());
    presenter.selectFolder(QString{});
    ASSERT_TRUE(wait_until(
        [&] { return !presenter.lastImportSelected() && presenter.visibleCount() == 1; }));

    int resets_while_importing = 0;
    int inserts_while_importing = 0;
    bool observed_intermediate_progress = false;
    QObject::connect(presenter.assets(), &QAbstractItemModel::modelReset, &presenter,
                     [&]
                     {
                         if (presenter.importWorkActive())
                             ++resets_while_importing;
                     });
    QObject::connect(presenter.assets(), &QAbstractItemModel::rowsInserted, &presenter,
                     [&]
                     {
                         if (presenter.importWorkActive())
                             ++inserts_while_importing;
                     });
    QObject::connect(&presenter, &StudioPresenter::libraryWorkChanged, &presenter,
                     [&]
                     {
                         if (presenter.importWorkActive() && presenter.importWorkCompleted() > 0 &&
                             presenter.importWorkCompleted() < presenter.importWorkTotal())
                         {
                             observed_intermediate_progress = true;
                             EXPECT_EQ(presenter.assets()->rowCount(), 1);
                         }
                     });

    presenter.importFilePaths(batch);
    ASSERT_TRUE(wait_until([&] { return !presenter.importWorkActive(); }, 30000))
        << presenter.errorText().toStdString();
    EXPECT_TRUE(observed_intermediate_progress);
    EXPECT_EQ(resets_while_importing, 0);
    EXPECT_EQ(inserts_while_importing, 0);
    EXPECT_TRUE(presenter.lastImportAvailable());
    EXPECT_TRUE(presenter.lastImportSelected());
    EXPECT_EQ(presenter.lastImportCount(), 3);
    ASSERT_EQ(presenter.visibleCount(), 3);

    std::vector<QString> names;
    for (int row = 0; row < presenter.assets()->rowCount(); ++row)
    {
        names.push_back(
            presenter.assets()
                ->data(presenter.assets()->index(row, 0), AssetListModel::DisplayNameRole)
                .toString());
    }
    std::ranges::sort(names);
    EXPECT_EQ(names,
              std::vector<QString>({QStringLiteral("batch-a.png"), QStringLiteral("batch-b.png"),
                                    QStringLiteral("batch-c.png")}));

    presenter.selectFolder(QString{});
    ASSERT_TRUE(wait_until(
        [&] { return !presenter.lastImportSelected() && presenter.visibleCount() == 4; }));
    const auto selected_last = commands.executeCommand(
        commands.ids().value(QStringLiteral("librarySelectLastImport")).toString(), {},
        QStringLiteral("control"));
    ASSERT_TRUE(selected_last.value(QStringLiteral("accepted")).toBool());
    ASSERT_TRUE(wait_until(
        [&] { return presenter.lastImportSelected() && presenter.visibleCount() == 3; }));

    const QString next_catalog = directory.filePath(QStringLiteral("next-library.sqlite"));
    presenter.createCatalogFromPath(next_catalog);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.catalogPath() == next_catalog && !presenter.busy() &&
                   presenter.visibleCount() == 0;
        }));
    EXPECT_FALSE(presenter.lastImportAvailable());
    EXPECT_FALSE(presenter.lastImportSelected());
    EXPECT_EQ(presenter.lastImportCount(), 0);
}

} // namespace
} // namespace ravo
