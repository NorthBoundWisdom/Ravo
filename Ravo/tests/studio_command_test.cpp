#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <string>

#include <QVariantMap>

#include <QColor>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QKeySequence>
#include <QThread>
#include <QTranslator>
#include <QSettings>
#include <QTemporaryDir>

#include "ravo/adapters/filesystem_preview_cache.h"
#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/engine/engine.h"
#include "ravo/foundation/log.h"
#include "ravo/recipe/develop.h"
#include "ravo/services/catalog_service.h"

#include "ravo/desktop/preview_request_owner.h"
#include "ravo/desktop/studio_command_controller.h"
#include "ravo/desktop/studio_presenter.h"
#include "ravo/recipe/color_harmonizer.h"
#include "ravo/recipe/develop_mask.h"
#include "studio_language_manager.h"

namespace ravo
{
namespace
{

void ensure_qt_core();

TEST(PreviewRequestOwnerTest, SupersededWorkIsCancelledAndLateResultsAreRejected)
{
    PreviewRequestOwner owner;
    const auto first_revision = owner.supersede("first_request");
    const auto first_token = owner.begin();
    EXPECT_FALSE(first_token.is_cancellation_requested());
    EXPECT_TRUE(owner.accepts(first_revision, "asset-a", "asset-a"));

    const auto second_revision = owner.supersede("selection_changed");
    EXPECT_TRUE(first_token.is_cancellation_requested());
    EXPECT_EQ(first_token.reason(), "selection_changed");
    EXPECT_FALSE(owner.accepts(first_revision, "asset-a", "asset-a"));

    const auto second_token = owner.begin();
    EXPECT_FALSE(second_token.is_cancellation_requested());
    EXPECT_FALSE(owner.accepts(second_revision, "asset-a", "asset-b"));
    EXPECT_TRUE(owner.accepts(second_revision, "asset-b", "asset-b"));
}

TEST(StudioSettingsTest, LanguageSettingNormalizesPersistsAndRepairsCorruption)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto previous_format = QSettings::defaultFormat();
    const QString previous_organization = QCoreApplication::organizationName();
    const QString previous_application = QCoreApplication::applicationName();
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    QCoreApplication::setOrganizationName(QStringLiteral("RavoSettingsTest"));
    QCoreApplication::setApplicationName(QStringLiteral("LanguageContract"));
    {
        QSettings settings;
        settings.setValue(QStringLiteral("desktop/language"), QStringLiteral("broken"));
        settings.sync();
        ASSERT_EQ(settings.status(), QSettings::NoError);
    }
    StudioLanguageManager manager;
    ASSERT_TRUE(manager.initialize()) << manager.lastError().toStdString();
    EXPECT_EQ(manager.language(), QStringLiteral("en_US"));
    {
        QSettings settings;
        EXPECT_FALSE(settings.contains(QStringLiteral("desktop/language")));
    }
    EXPECT_TRUE(manager.setLanguage(QStringLiteral("en-US")));
    {
        QSettings settings;
        EXPECT_EQ(settings.value(QStringLiteral("desktop/language")).toString(),
                  QStringLiteral("en_US"));
    }
    EXPECT_FALSE(manager.setLanguage(QStringLiteral("fr_FR")));
    EXPECT_EQ(manager.language(), QStringLiteral("en_US"));
    EXPECT_FALSE(manager.lastError().isEmpty());
    {
        QSettings settings;
        EXPECT_EQ(settings.value(QStringLiteral("desktop/language")).toString(),
                  QStringLiteral("en_US"));
        settings.clear();
    }
    QCoreApplication::setOrganizationName(previous_organization);
    QCoreApplication::setApplicationName(previous_application);
    QSettings::setDefaultFormat(previous_format);
}

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

[[nodiscard]] QString qml_model_entry(const QString &source, const char *field)
{
    const auto needle = QStringLiteral("\"field\": \"%1\"").arg(QString::fromLatin1(field));
    const auto field_position = source.indexOf(needle);
    if (field_position < 0)
    {
        return {};
    }
    const auto begin = source.lastIndexOf(QLatin1Char('{'), field_position);
    const auto end = source.indexOf(QLatin1Char('}'), field_position);
    if (begin < 0 || end < field_position)
    {
        return {};
    }
    return source.mid(begin, end - begin + 1);
}

TEST(StudioPresenterTest, MigratedColorPropertiesExposeCanonicalIdentity)
{
    ensure_qt_core();
    StudioPresenter presenter;
    EXPECT_DOUBLE_EQ(presenter.editChannelMixerRR(), 1.0);
    EXPECT_DOUBLE_EQ(presenter.editChannelMixerRG(), 0.0);
    EXPECT_DOUBLE_EQ(presenter.editChannelMixerRB(), 0.0);
    EXPECT_DOUBLE_EQ(presenter.editChannelMixerGR(), 0.0);
    EXPECT_DOUBLE_EQ(presenter.editChannelMixerGG(), 1.0);
    EXPECT_DOUBLE_EQ(presenter.editChannelMixerGB(), 0.0);
    EXPECT_DOUBLE_EQ(presenter.editChannelMixerBR(), 0.0);
    EXPECT_DOUBLE_EQ(presenter.editChannelMixerBG(), 0.0);
    EXPECT_DOUBLE_EQ(presenter.editChannelMixerBB(), 1.0);
    EXPECT_DOUBLE_EQ(presenter.editHotPixelsStrength(), 0.0);
    EXPECT_DOUBLE_EQ(presenter.editHotPixelsThreshold(), 0.05);
    EXPECT_FALSE(presenter.editHotPixelsPermissive());
    EXPECT_EQ(presenter.editRawCaIterations(), 0);
    EXPECT_FALSE(presenter.editRawCaAvoidShift());
    EXPECT_DOUBLE_EQ(presenter.editSharpen(), 0.0);
    EXPECT_DOUBLE_EQ(presenter.editSharpenRadius(), 2.0);
    EXPECT_DOUBLE_EQ(presenter.editSharpenThreshold(), 0.5);
    const auto retouch = presenter.editRetouch();
    EXPECT_EQ(retouch.value(QStringLiteral("regionCount")).toInt(), 0);
    EXPECT_TRUE(retouch.value(QStringLiteral("regions")).toList().isEmpty());
    EXPECT_EQ(retouch.value(QStringLiteral("numScales")).toInt(), 0);
    EXPECT_DOUBLE_EQ(presenter.editDehaze(), 0.0);
    EXPECT_DOUBLE_EQ(presenter.editDehazeDistance(), 0.2);
    EXPECT_TRUE(presenter.editDehazeAdaptive());
    EXPECT_TRUE(presenter.filterText().isEmpty());
    EXPECT_EQ(presenter.mediaFilter(), QStringLiteral("any"));
    EXPECT_EQ(presenter.editFilter(), QStringLiteral("any"));
    const auto legacy_balance = presenter.editLegacyColorBalance();
    EXPECT_EQ(legacy_balance.size(), 18);
    EXPECT_FALSE(legacy_balance.value(QStringLiteral("enabled")).toBool());
    EXPECT_EQ(legacy_balance.value(QStringLiteral("modeIndex")).toInt(), 1);
    EXPECT_DOUBLE_EQ(legacy_balance.value(QStringLiteral("liftFactor")).toDouble(), 1.0);
    EXPECT_DOUBLE_EQ(legacy_balance.value(QStringLiteral("gammaBlue")).toDouble(), 1.0);
    EXPECT_DOUBLE_EQ(legacy_balance.value(QStringLiteral("gainGreen")).toDouble(), 1.0);
    EXPECT_DOUBLE_EQ(legacy_balance.value(QStringLiteral("inputSaturation")).toDouble(), 1.0);
    EXPECT_DOUBLE_EQ(legacy_balance.value(QStringLiteral("contrast")).toDouble(), 1.0);
    EXPECT_DOUBLE_EQ(legacy_balance.value(QStringLiteral("greyFulcrum")).toDouble(), 18.0);
    EXPECT_DOUBLE_EQ(legacy_balance.value(QStringLiteral("outputSaturation")).toDouble(), 1.0);
    const auto color_checker = presenter.editColorChecker();
    EXPECT_EQ(color_checker.size(), 10);
    EXPECT_FALSE(color_checker.value(QStringLiteral("enabled")).toBool());
    EXPECT_EQ(color_checker.value(QStringLiteral("presetIndex")).toInt(), -1);
    EXPECT_EQ(color_checker.value(QStringLiteral("patchIndex")).toInt(), 0);
    EXPECT_EQ(color_checker.value(QStringLiteral("patchCount")).toInt(), 24);
    EXPECT_DOUBLE_EQ(color_checker.value(QStringLiteral("sourceL")).toDouble(), 37.990001678466797);
    EXPECT_DOUBLE_EQ(color_checker.value(QStringLiteral("sourceA")).toDouble(), 13.5600004196167);
    EXPECT_DOUBLE_EQ(color_checker.value(QStringLiteral("sourceB")).toDouble(), 14.0600004196167);
    EXPECT_DOUBLE_EQ(color_checker.value(QStringLiteral("targetL")).toDouble(), 37.990001678466797);
    EXPECT_DOUBLE_EQ(color_checker.value(QStringLiteral("targetA")).toDouble(), 13.5600004196167);
    EXPECT_DOUBLE_EQ(color_checker.value(QStringLiteral("targetB")).toDouble(), 14.0600004196167);
    const auto balance = presenter.editColorBalanceRgb();
    EXPECT_EQ(balance.size(), 33);
    EXPECT_DOUBLE_EQ(balance.value(QStringLiteral("globalY")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(balance.value(QStringLiteral("shadowsFalloff")).toDouble(), 1.0);
    EXPECT_DOUBLE_EQ(balance.value(QStringLiteral("highlightsFalloff")).toDouble(), 1.0);
    EXPECT_DOUBLE_EQ(balance.value(QStringLiteral("maskGreyFulcrum")).toDouble(), 0.1845);
    EXPECT_DOUBLE_EQ(balance.value(QStringLiteral("greyFulcrum")).toDouble(), 0.1845);
    EXPECT_EQ(balance.value(QStringLiteral("formulaIndex")).toInt(), 0);
    const auto color_correction = presenter.editColorCorrection();
    EXPECT_EQ(color_correction.size(), 6);
    EXPECT_FALSE(color_correction.value(QStringLiteral("enabled")).toBool());
    EXPECT_DOUBLE_EQ(color_correction.value(QStringLiteral("highlightA")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(color_correction.value(QStringLiteral("highlightB")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(color_correction.value(QStringLiteral("shadowA")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(color_correction.value(QStringLiteral("shadowB")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(color_correction.value(QStringLiteral("saturation")).toDouble(), 1.0);
    const auto color_contrast = presenter.editColorContrast();
    EXPECT_EQ(color_contrast.size(), 6);
    EXPECT_FALSE(color_contrast.value(QStringLiteral("enabled")).toBool());
    EXPECT_DOUBLE_EQ(color_contrast.value(QStringLiteral("aSteepness")).toDouble(), 1.0);
    EXPECT_DOUBLE_EQ(color_contrast.value(QStringLiteral("aOffset")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(color_contrast.value(QStringLiteral("bSteepness")).toDouble(), 1.0);
    EXPECT_DOUBLE_EQ(color_contrast.value(QStringLiteral("bOffset")).toDouble(), 0.0);
    EXPECT_TRUE(color_contrast.value(QStringLiteral("unbound")).toBool());
    const auto color_reconstruction = presenter.editColorReconstruction();
    EXPECT_EQ(color_reconstruction.size(), 7);
    EXPECT_FALSE(color_reconstruction.value(QStringLiteral("enabled")).toBool());
    EXPECT_DOUBLE_EQ(color_reconstruction.value(QStringLiteral("threshold")).toDouble(), 100.0);
    EXPECT_DOUBLE_EQ(color_reconstruction.value(QStringLiteral("spatial")).toDouble(), 400.0);
    EXPECT_DOUBLE_EQ(color_reconstruction.value(QStringLiteral("range")).toDouble(), 10.0);
    EXPECT_DOUBLE_EQ(color_reconstruction.value(QStringLiteral("hueDegrees")).toDouble(), 237.6);
    EXPECT_EQ(color_reconstruction.value(QStringLiteral("precedenceIndex")).toInt(), 0);
    EXPECT_EQ(color_reconstruction.value(QStringLiteral("precedenceChoices")).toStringList().size(),
              3);
    const auto color_harmonizer = presenter.editColorHarmonizer();
    EXPECT_EQ(color_harmonizer.size(), 24);
    EXPECT_FALSE(color_harmonizer.value(QStringLiteral("enabled")).toBool());
    EXPECT_EQ(color_harmonizer.value(QStringLiteral("ruleIndex")).toInt(), 3);
    EXPECT_EQ(color_harmonizer.value(QStringLiteral("ruleChoices")).toStringList().size(), 10);
    EXPECT_FALSE(color_harmonizer.value(QStringLiteral("customRule")).toBool());
    EXPECT_EQ(color_harmonizer.value(QStringLiteral("activeNodeCount")).toInt(), 2);
    EXPECT_TRUE(color_harmonizer.value(QStringLiteral("anchorVisible")).toBool());
    EXPECT_DOUBLE_EQ(color_harmonizer.value(QStringLiteral("anchorHueDegrees")).toDouble(), 36.0);
    EXPECT_DOUBLE_EQ(color_harmonizer.value(QStringLiteral("pullStrength")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(color_harmonizer.value(QStringLiteral("neutralProtection")).toDouble(), 0.5);
    EXPECT_DOUBLE_EQ(color_harmonizer.value(QStringLiteral("pullWidth")).toDouble(), 1.0);
    EXPECT_DOUBLE_EQ(color_harmonizer.value(QStringLiteral("smoothing")).toDouble(), 0.0);
    EXPECT_EQ(color_harmonizer.value(QStringLiteral("customNodeCount")).toInt(), 4);
    EXPECT_DOUBLE_EQ(color_harmonizer.value(QStringLiteral("customHue0Degrees")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(color_harmonizer.value(QStringLiteral("nodeSaturation0")).toDouble(), 1.0);
    const auto shared_controls = color_harmonizer.value(QStringLiteral("sharedControls")).toList();
    ASSERT_EQ(shared_controls.size(), 5);
    const auto anchor_control = shared_controls.front().toMap();
    EXPECT_DOUBLE_EQ(anchor_control.value(QStringLiteral("minimum")).toDouble(),
                     kColorHarmonizerHueDegreesMin);
    EXPECT_DOUBLE_EQ(anchor_control.value(QStringLiteral("maximum")).toDouble(),
                     kColorHarmonizerHueDegreesMax);
    EXPECT_DOUBLE_EQ(anchor_control.value(QStringLiteral("step")).toDouble(), 0.1);
    EXPECT_DOUBLE_EQ(anchor_control.value(QStringLiteral("reset")).toDouble(), 36.0);
    EXPECT_TRUE(anchor_control.value(QStringLiteral("visible")).toBool());
    const auto smoothing_control =
        std::find_if(shared_controls.cbegin(), shared_controls.cend(),
                     [](const QVariant &candidate)
                     {
                         return candidate.toMap().value(QStringLiteral("field")).toString() ==
                                QStringLiteral("colorHarmonizerSmoothing");
                     });
    ASSERT_NE(smoothing_control, shared_controls.cend());
    EXPECT_DOUBLE_EQ(smoothing_control->toMap().value(QStringLiteral("minimum")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(smoothing_control->toMap().value(QStringLiteral("maximum")).toDouble(), 2.0);
    EXPECT_DOUBLE_EQ(smoothing_control->toMap().value(QStringLiteral("step")).toDouble(), 0.01);
    EXPECT_FALSE(color_harmonizer.value(QStringLiteral("customNodeControl"))
                     .toMap()
                     .value(QStringLiteral("visible"))
                     .toBool());
    const auto custom_hues = color_harmonizer.value(QStringLiteral("customHueControls")).toList();
    const auto node_sats =
        color_harmonizer.value(QStringLiteral("nodeSaturationControls")).toList();
    ASSERT_EQ(custom_hues.size(), 4);
    ASSERT_EQ(node_sats.size(), 4);
    EXPECT_FALSE(custom_hues[0].toMap().value(QStringLiteral("visible")).toBool());
    EXPECT_TRUE(node_sats[0].toMap().value(QStringLiteral("visible")).toBool());
    EXPECT_TRUE(node_sats[1].toMap().value(QStringLiteral("visible")).toBool());
    EXPECT_FALSE(node_sats[2].toMap().value(QStringLiteral("visible")).toBool());
    EXPECT_FALSE(node_sats[3].toMap().value(QStringLiteral("visible")).toBool());
    const auto harmonizer_mask = presenter.editColorHarmonizerMask();
    EXPECT_FALSE(harmonizer_mask.value(QStringLiteral("attached")).toBool());
    EXPECT_TRUE(harmonizer_mask.value(QStringLiteral("editable")).toBool());
    EXPECT_EQ(harmonizer_mask.value(QStringLiteral("kindIndex")).toInt(), 0);
    EXPECT_EQ(harmonizer_mask.value(QStringLiteral("kindName")).toString(), QStringLiteral("none"));
    EXPECT_EQ(harmonizer_mask.value(QStringLiteral("statusCode")).toString(),
              QStringLiteral("no_mask"));
    EXPECT_EQ(harmonizer_mask.value(QStringLiteral("kindChoices")).toStringList().size(), 9);
    EXPECT_EQ(harmonizer_mask.value(QStringLiteral("sourceChoices")).toStringList().size(), 2);
    EXPECT_EQ(harmonizer_mask.value(QStringLiteral("channelChoices")).toStringList().size(), 4);
    const auto mask_controls = harmonizer_mask.value(QStringLiteral("numericControls")).toList();
    ASSERT_EQ(mask_controls.size(), 22);
    const auto radius = std::find_if(
        mask_controls.cbegin(), mask_controls.cend(), [](const QVariant &candidate)
        { return candidate.toMap().value(QStringLiteral("key")) == QStringLiteral("radius"); });
    ASSERT_NE(radius, mask_controls.cend());
    EXPECT_DOUBLE_EQ(radius->toMap().value(QStringLiteral("min")).toDouble(), 0.01);
    EXPECT_DOUBLE_EQ(radius->toMap().value(QStringLiteral("max")).toDouble(),
                     kCanonicalMaskUnitMax);
    const auto graduated_mask = presenter.editGraduatedMask();
    EXPECT_EQ(graduated_mask.value(QStringLiteral("target")).toString(),
              QStringLiteral("graduatednd"));
    EXPECT_EQ(graduated_mask.value(QStringLiteral("detachField")).toString(),
              QStringLiteral("graduatedMask"));
    const auto primaries = presenter.editPrimaries();
    EXPECT_EQ(primaries.size(), 8);
    EXPECT_DOUBLE_EQ(primaries.value(QStringLiteral("achromaticTintHueDegrees")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(primaries.value(QStringLiteral("achromaticTintPurity")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(primaries.value(QStringLiteral("redHueDegrees")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(primaries.value(QStringLiteral("redPurity")).toDouble(), 1.0);
    EXPECT_DOUBLE_EQ(primaries.value(QStringLiteral("greenHueDegrees")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(primaries.value(QStringLiteral("greenPurity")).toDouble(), 1.0);
    EXPECT_DOUBLE_EQ(primaries.value(QStringLiteral("blueHueDegrees")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(primaries.value(QStringLiteral("bluePurity")).toDouble(), 1.0);
    const auto white_balance = presenter.editWhiteBalance();
    EXPECT_EQ(white_balance.size(), 7);
    EXPECT_EQ(white_balance.value(QStringLiteral("modeIndex")).toInt(), 0);
    EXPECT_FALSE(white_balance.value(QStringLiteral("hasCoefficients")).toBool());
    EXPECT_FALSE(white_balance.value(QStringLiteral("canPick")).toBool());
    EXPECT_DOUBLE_EQ(white_balance.value(QStringLiteral("red")).toDouble(), 1.0);
    EXPECT_DOUBLE_EQ(white_balance.value(QStringLiteral("green")).toDouble(), 1.0);
    EXPECT_DOUBLE_EQ(white_balance.value(QStringLiteral("blue")).toDouble(), 1.0);
    EXPECT_DOUBLE_EQ(white_balance.value(QStringLiteral("fourth")).toDouble(), 1.0);
    EXPECT_EQ(presenter.editColorEqBands().size(), 8);
    const auto input_color = presenter.editInputColor();
    EXPECT_EQ(input_color.size(), 7);
    EXPECT_EQ(input_color.value(QStringLiteral("inputProfileIndex")).toInt(), 0);
    EXPECT_EQ(input_color.value(QStringLiteral("workingProfileIndex")).toInt(), 0);
    EXPECT_EQ(input_color.value(QStringLiteral("intentIndex")).toInt(), 0);
    EXPECT_EQ(input_color.value(QStringLiteral("normalizeIndex")).toInt(), 0);
    EXPECT_FALSE(input_color.value(QStringLiteral("blueMapping")).toBool());
    EXPECT_EQ(input_color.value(QStringLiteral("inputProfile")).toString(),
              QStringLiteral("source"));
    EXPECT_EQ(input_color.value(QStringLiteral("workingProfile")).toString(),
              QStringLiteral("linear_rec709"));
    const auto profile_gamma = presenter.editProfileGamma();
    EXPECT_EQ(profile_gamma.size(), 8);
    EXPECT_FALSE(profile_gamma.value(QStringLiteral("enabled")).toBool());
    EXPECT_EQ(profile_gamma.value(QStringLiteral("modeIndex")).toInt(), 0);
    EXPECT_DOUBLE_EQ(profile_gamma.value(QStringLiteral("linear")).toDouble(), 0.1);
    EXPECT_DOUBLE_EQ(profile_gamma.value(QStringLiteral("gamma")).toDouble(), 0.45);
    EXPECT_DOUBLE_EQ(profile_gamma.value(QStringLiteral("dynamicRange")).toDouble(), 10.0);
    EXPECT_DOUBLE_EQ(profile_gamma.value(QStringLiteral("greyPoint")).toDouble(), 18.0);
    EXPECT_DOUBLE_EQ(profile_gamma.value(QStringLiteral("shadowsRange")).toDouble(), -5.0);
    EXPECT_DOUBLE_EQ(profile_gamma.value(QStringLiteral("securityFactor")).toDouble(), 0.0);
    const auto output_color = presenter.editOutputColor();
    EXPECT_EQ(output_color.size(), 9);
    EXPECT_EQ(output_color.value(QStringLiteral("outputProfileIndex")).toInt(), 0);
    EXPECT_EQ(output_color.value(QStringLiteral("intentIndex")).toInt(), 0);
    EXPECT_EQ(output_color.value(QStringLiteral("proofModeIndex")).toInt(), 0);
    EXPECT_EQ(output_color.value(QStringLiteral("proofProfileIndex")).toInt(), 0);
    EXPECT_EQ(output_color.value(QStringLiteral("proofIntentIndex")).toInt(), 1);
    EXPECT_TRUE(output_color.value(QStringLiteral("blackPointCompensation")).toBool());
    EXPECT_EQ(output_color.value(QStringLiteral("outputProfile")).toString(),
              QStringLiteral("srgb"));
    EXPECT_EQ(output_color.value(QStringLiteral("proofMode")).toString(), QStringLiteral("off"));
    EXPECT_EQ(output_color.value(QStringLiteral("proofProfile")).toString(),
              QStringLiteral("srgb"));
    const auto exposure = presenter.editExposureParams();
    EXPECT_EQ(exposure.size(), 7);
    EXPECT_EQ(exposure.value(QStringLiteral("modeIndex")).toInt(), 0);
    EXPECT_DOUBLE_EQ(exposure.value(QStringLiteral("black")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(exposure.value(QStringLiteral("exposureEv")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(exposure.value(QStringLiteral("deflickerPercentile")).toDouble(), 50.0);
    EXPECT_DOUBLE_EQ(exposure.value(QStringLiteral("deflickerTargetEv")).toDouble(), -4.0);
    EXPECT_FALSE(exposure.value(QStringLiteral("compensateExposureBias")).toBool());
    EXPECT_FALSE(exposure.value(QStringLiteral("compensateHighlightPreservation")).toBool());
}

TEST(StudioPresenterTest, ZoomModesAndFactorBoundsHaveOneDeterministicOwner)
{
    ensure_qt_core();
    StudioPresenter presenter;
    EXPECT_EQ(presenter.zoomMode(), QStringLiteral("fit"));
    EXPECT_DOUBLE_EQ(presenter.zoomFactor(), 1.0);
    presenter.setZoomMode(QStringLiteral("fill"));
    EXPECT_EQ(presenter.zoomMode(), QStringLiteral("fill"));
    presenter.setZoomMode(QStringLiteral("100"));
    EXPECT_EQ(presenter.zoomMode(), QStringLiteral("actual"));
    EXPECT_DOUBLE_EQ(presenter.zoomFactor(), 1.0);
    presenter.setZoomFactor(0.0);
    EXPECT_EQ(presenter.zoomMode(), QStringLiteral("custom"));
    EXPECT_DOUBLE_EQ(presenter.zoomFactor(), 0.1);
    presenter.adjustZoom(120);
    EXPECT_DOUBLE_EQ(presenter.zoomFactor(), 0.11);
    presenter.setZoomFactor(100.0);
    EXPECT_DOUBLE_EQ(presenter.zoomFactor(), 8.0);
    presenter.setZoomMode(QStringLiteral("future"));
    EXPECT_EQ(presenter.zoomMode(), QStringLiteral("fit"));
    presenter.setZoomMode(QStringLiteral("fill"));
    presenter.toggleActualSize();
    EXPECT_EQ(presenter.zoomMode(), QStringLiteral("actual"));
    presenter.toggleActualSize();
    EXPECT_EQ(presenter.zoomMode(), QStringLiteral("fill"));
    presenter.setZoomFactor(2.0);
    presenter.toggleActualSize();
    EXPECT_EQ(presenter.zoomMode(), QStringLiteral("actual"));
    presenter.toggleActualSize();
    EXPECT_EQ(presenter.zoomMode(), QStringLiteral("custom"));
    EXPECT_DOUBLE_EQ(presenter.zoomFactor(), 2.0);
    presenter.setZoomMode(QStringLiteral("fit"));
    presenter.setZoomMode(QStringLiteral("actual"));
    presenter.toggleActualSize();
    EXPECT_EQ(presenter.zoomMode(), QStringLiteral("fit"));
}

TEST(StudioPresenterTest, CopiedEditsClipboardStartsEmptyAndIgnoresEmptySelection)
{
    ensure_qt_core();
    StudioPresenter presenter;
    EXPECT_FALSE(presenter.hasCopiedEdits());
    presenter.copyEdits();
    EXPECT_FALSE(presenter.hasCopiedEdits());
    presenter.pasteEdits();
    presenter.pasteEditsSection(QStringLiteral("light"));
    presenter.pasteEditsSection(QStringLiteral("color"));
    EXPECT_FALSE(presenter.hasCopiedEdits());
}

TEST(StudioPresenterTest, SessionUndoStartsEmptyAndHistoryRestoreWithoutSelectionIsIgnored)
{
    ensure_qt_core();
    StudioPresenter presenter;
    EXPECT_FALSE(presenter.canUndo());
    EXPECT_FALSE(presenter.canRedo());
    presenter.restoreHistory(0);
    presenter.undoEdit();
    presenter.redoEdit();
    EXPECT_FALSE(presenter.canUndo());
    EXPECT_FALSE(presenter.canRedo());
}

TEST(StudioPresenterTest, PollAppliesDevelopWrittenByAnotherCatalogClient)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString photo = directory.filePath(QStringLiteral("photo.png"));
    QImage image(32, 24, QImage::Format_RGB888);
    image.fill(QColor(120, 130, 140));
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
    ASSERT_TRUE(wait_until([&] { return !presenter.previewLoading(); }))
        << presenter.errorText().toStdString();
    {
        QElapsedTimer settle;
        settle.start();
        while (settle.elapsed() < 500)
        {
            QCoreApplication::processEvents();
            QThread::msleep(10);
        }
    }
    EXPECT_NEAR(presenter.editExposure(), 0.0, 1e-9);
    const auto asset_id = presenter.selectedAssetId().toStdString();

    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto catalog_utf8 = catalog.toUtf8().toStdString();
    auto repository = SqliteCatalogRepository::open(catalog_utf8);
    ASSERT_TRUE(repository) << repository.error().message;
    auto cache = FilesystemPreviewCache::create(catalog_utf8 + ".preview");
    ASSERT_TRUE(cache) << cache.error().message;
    CatalogService writer(engine.value(), std::move(repository).value(),
                          std::make_unique<QtRasterDecoder>(), std::move(cache).value());
    DevelopParams params;
    params.exposure_ev = 1.0;
    auto saved = writer.save_develop(asset_id, params);
    ASSERT_TRUE(saved) << saved.error().message;
    ASSERT_TRUE(writer.close());

    ASSERT_TRUE(wait_until(
        [&]
        {
            presenter.pollCatalogRevision();
            return std::abs(presenter.editExposure() - 1.0) < 1e-6;
        }))
        << presenter.errorText().toStdString() << " exposure=" << presenter.editExposure();
    EXPECT_TRUE(presenter.selectedHasEdits());
    EXPECT_FALSE(presenter.canUndo());
}

TEST(StudioPresenterTest, ScopeModeOwnsAllAcceptedDiagnosticsAndRejectsFutureState)
{
    ensure_qt_core();
    StudioPresenter presenter;
    EXPECT_EQ(presenter.scopeMode(), QStringLiteral("histogram"));
    for (const auto &mode : {QStringLiteral("waveform"), QStringLiteral("parade"),
                             QStringLiteral("vectorscope"), QStringLiteral("split")})
    {
        presenter.setScopeMode(mode);
        EXPECT_EQ(presenter.scopeMode(), mode);
    }
    presenter.setScopeMode(QStringLiteral("future"));
    EXPECT_EQ(presenter.scopeMode(), QStringLiteral("histogram"));
    EXPECT_TRUE(presenter.scopeParadeUrl().isEmpty());
    EXPECT_TRUE(presenter.scopeWaveformUrl().isEmpty());
    EXPECT_TRUE(presenter.scopeVectorscopeUrl().isEmpty());
    EXPECT_TRUE(presenter.scopeSplitUrl().isEmpty());
}

TEST(StudioQmlContract, LegacyColorBalanceSlidersExposeEverySchemaHardEndpoint)
{
    QFile panel(QStringLiteral(RAVO_STUDIO_DEVELOP_PANEL_QML));
    ASSERT_TRUE(panel.open(QIODevice::ReadOnly | QIODevice::Text))
        << panel.errorString().toStdString();
    const auto source = QString::fromUtf8(panel.readAll());

    constexpr std::array<const char *, 14> zero_to_two_fields{
        "legacyColorBalanceLiftFactor",      "legacyColorBalanceLiftRed",
        "legacyColorBalanceLiftGreen",       "legacyColorBalanceLiftBlue",
        "legacyColorBalanceGammaFactor",     "legacyColorBalanceGammaRed",
        "legacyColorBalanceGammaGreen",      "legacyColorBalanceGammaBlue",
        "legacyColorBalanceGainFactor",      "legacyColorBalanceGainRed",
        "legacyColorBalanceGainGreen",       "legacyColorBalanceGainBlue",
        "legacyColorBalanceInputSaturation", "legacyColorBalanceOutputSaturation",
    };
    for (const auto *field : zero_to_two_fields)
    {
        const auto entry = qml_model_entry(source, field);
        ASSERT_FALSE(entry.isEmpty()) << field;
        EXPECT_TRUE(entry.contains(QStringLiteral("\"minimum\": 0"))) << field;
        EXPECT_TRUE(entry.contains(QStringLiteral("\"maximum\": 2"))) << field;
    }
    const auto contrast = qml_model_entry(source, "legacyColorBalanceContrast");
    ASSERT_FALSE(contrast.isEmpty());
    EXPECT_TRUE(contrast.contains(QStringLiteral("\"minimum\": 0.01")));
    EXPECT_TRUE(contrast.contains(QStringLiteral("\"maximum\": 1.99")));
    const auto fulcrum = qml_model_entry(source, "legacyColorBalanceGreyFulcrum");
    ASSERT_FALSE(fulcrum.isEmpty());
    EXPECT_TRUE(fulcrum.contains(QStringLiteral("\"minimum\": 0.1")));
    EXPECT_TRUE(fulcrum.contains(QStringLiteral("\"maximum\": 100")));
}

TEST(StudioQmlContract, ColorCheckerExposesEveryLabFieldWithoutClampingCanonicalFloats)
{
    QFile panel(QStringLiteral(RAVO_STUDIO_DEVELOP_PANEL_QML));
    ASSERT_TRUE(panel.open(QIODevice::ReadOnly | QIODevice::Text))
        << panel.errorString().toStdString();
    const auto source = QString::fromUtf8(panel.readAll());

    constexpr std::array<const char *, 6> fields{
        "colorCheckerSourceL", "colorCheckerSourceA", "colorCheckerSourceB",
        "colorCheckerTargetL", "colorCheckerTargetA", "colorCheckerTargetB",
    };
    for (const auto *field : fields)
    {
        EXPECT_TRUE(
            source.contains(QStringLiteral("\"field\": \"%1\"").arg(QString::fromLatin1(field))))
            << field;
    }
    EXPECT_TRUE(source.contains(QStringLiteral("bottom: -3.402823466e38")));
    EXPECT_TRUE(source.contains(QStringLiteral("top: 3.402823466e38")));
    EXPECT_TRUE(source.contains(QStringLiteral("DoubleValidator.ScientificNotation")));
    EXPECT_TRUE(source.contains(QStringLiteral("colorCheckerPreset")));
    EXPECT_TRUE(source.contains(QStringLiteral("colorCheckerPatch")));
    EXPECT_TRUE(source.contains(QStringLiteral("root.presenter.editColorChecker.patchCount > 0")));
    EXPECT_FALSE(source.contains(QStringLiteral("root.hasSelection && count > 0")));
    EXPECT_TRUE(source.contains(QStringLiteral("resetControl(\"colorChecker\")")));
}

TEST(StudioQmlContract, ColorCorrectionUsesHardBoundsAndGenericDevelopIntents)
{
    QFile panel(QStringLiteral(RAVO_STUDIO_DEVELOP_PANEL_QML));
    ASSERT_TRUE(panel.open(QIODevice::ReadOnly | QIODevice::Text))
        << panel.errorString().toStdString();
    const auto source = QString::fromUtf8(panel.readAll());

    constexpr std::array<const char *, 4> endpoint_fields{
        "colorCorrectionHighlightA", "colorCorrectionHighlightB", "colorCorrectionShadowA",
        "colorCorrectionShadowB"};
    for (const auto *field : endpoint_fields)
    {
        const auto entry = qml_model_entry(source, field);
        ASSERT_FALSE(entry.isEmpty()) << field;
        EXPECT_TRUE(entry.contains(QStringLiteral("\"minimum\": -40"))) << field;
        EXPECT_TRUE(entry.contains(QStringLiteral("\"maximum\": 40"))) << field;
    }
    const auto saturation = qml_model_entry(source, "colorCorrectionSaturation");
    ASSERT_FALSE(saturation.isEmpty());
    EXPECT_TRUE(saturation.contains(QStringLiteral("\"minimum\": -3")));
    EXPECT_TRUE(saturation.contains(QStringLiteral("\"maximum\": 3")));

    const auto section_begin = source.indexOf(QStringLiteral("colorCorrectionEnabled"));
    const auto section_end = source.indexOf(QStringLiteral("colorContrast"), section_begin);
    ASSERT_GE(section_begin, 0);
    ASSERT_GT(section_end, section_begin);
    const auto section = source.mid(section_begin, section_end - section_begin);
    EXPECT_TRUE(section.contains(QStringLiteral("root.presenter.editColorCorrection")));
    EXPECT_TRUE(section.contains(
        QStringLiteral("setDevelopNumber(\"colorCorrectionEnabled\", checked ? 1 : 0)")));
    EXPECT_TRUE(section.contains(QStringLiteral("setDevelopNumber(modelData.field, value)")));
    EXPECT_TRUE(section.contains(QStringLiteral("previewDevelopNumber(modelData.field, value)")));
    EXPECT_TRUE(section.contains(QStringLiteral("resetControl(modelData.field)")));
    EXPECT_TRUE(section.contains(QStringLiteral("resetControl(\"colorCorrection\")")));
    EXPECT_FALSE(section.contains(QStringLiteral("affine_lab_v1")));

    const auto rgb_balance = source.indexOf(QStringLiteral("colorBalanceGlobalY"));
    const auto correction = source.indexOf(QStringLiteral("colorCorrectionHighlightA"));
    const auto contrast = source.indexOf(QStringLiteral("colorContrast"), correction);
    ASSERT_GE(rgb_balance, 0);
    ASSERT_GE(correction, 0);
    ASSERT_GE(contrast, 0);
    EXPECT_LT(rgb_balance, correction);
    EXPECT_LT(correction, contrast);
}

TEST(StudioQmlContract, ColorContrastExposesFullV2SurfaceThroughGenericDevelopIntents)
{
    QFile panel(QStringLiteral(RAVO_STUDIO_DEVELOP_PANEL_QML));
    ASSERT_TRUE(panel.open(QIODevice::ReadOnly | QIODevice::Text))
        << panel.errorString().toStdString();
    const auto source = QString::fromUtf8(panel.readAll());

    for (const auto *field : {"colorContrastASteepness", "colorContrastBSteepness"})
    {
        const auto entry = qml_model_entry(source, field);
        ASSERT_FALSE(entry.isEmpty()) << field;
        EXPECT_TRUE(entry.contains(QStringLiteral("\"minimum\": 0"))) << field;
        EXPECT_TRUE(entry.contains(QStringLiteral("\"maximum\": 5"))) << field;
    }
    for (const auto *field : {"colorContrastAOffset", "colorContrastBOffset"})
    {
        EXPECT_TRUE(
            source.contains(QStringLiteral("\"field\": \"%1\"").arg(QString::fromLatin1(field))))
            << field;
    }

    const auto section_begin = source.indexOf(QStringLiteral("colorContrastEnabled"));
    const auto section_end = source.indexOf(QStringLiteral("colorHarmonizerEnabled"), section_begin);
    ASSERT_GE(section_begin, 0);
    ASSERT_GT(section_end, section_begin);
    const auto section = source.mid(section_begin, section_end - section_begin);
    EXPECT_TRUE(section.contains(QStringLiteral("root.presenter.editColorContrast")));
    EXPECT_TRUE(section.contains(
        QStringLiteral("setDevelopNumber(\"colorContrastEnabled\", checked ? 1 : 0)")));
    EXPECT_TRUE(section.contains(QStringLiteral("qsTr(\"Enable Color contrast\")")));
    EXPECT_TRUE(section.contains(QStringLiteral("setDevelopNumber(modelData.field, value)")));
    EXPECT_TRUE(section.contains(QStringLiteral("previewDevelopNumber(modelData.field, value)")));
    EXPECT_TRUE(section.contains(
        QStringLiteral("setDevelopNumber(\"colorContrastUnbound\", checked ? 1 : 0)")));
    EXPECT_TRUE(section.contains(QStringLiteral("qsTr(\"Allow extended chroma\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("DoubleValidator.ScientificNotation")));
    EXPECT_TRUE(source.contains(QStringLiteral("bottom: -3.4028234663852886e38")));
    EXPECT_TRUE(source.contains(QStringLiteral("top: 3.4028234663852886e38")));
    EXPECT_TRUE(section.contains(QStringLiteral("resetControl(modelData.field)")));
    EXPECT_TRUE(section.contains(QStringLiteral("resetControl(\"colorContrast\")")));
    EXPECT_TRUE(section.contains(QStringLiteral("qsTr(\"Disable and reset Color contrast\")")));
    EXPECT_FALSE(section.contains(QStringLiteral("axis_affine_v2")));

    const auto correction = source.indexOf(QStringLiteral("colorCorrectionEnabled"));
    const auto contrast = source.indexOf(QStringLiteral("colorContrastEnabled"), correction);
    const auto a_steepness = source.indexOf(QStringLiteral("colorContrastASteepness"), contrast);
    const auto b_steepness = source.indexOf(QStringLiteral("colorContrastBSteepness"), contrast);
    const auto a_offset = source.indexOf(QStringLiteral("colorContrastAOffset"), contrast);
    const auto b_offset = source.indexOf(QStringLiteral("colorContrastBOffset"), contrast);
    const auto unbound = source.indexOf(QStringLiteral("colorContrastUnbound"), contrast);
    ASSERT_GE(correction, 0);
    ASSERT_GE(contrast, 0);
    ASSERT_GE(a_steepness, 0);
    ASSERT_GE(b_steepness, 0);
    ASSERT_GE(a_offset, 0);
    ASSERT_GE(b_offset, 0);
    ASSERT_GE(unbound, 0);
    EXPECT_LT(correction, contrast);
    EXPECT_LT(contrast, a_steepness);
    EXPECT_LT(a_steepness, b_steepness);
    EXPECT_LT(b_steepness, a_offset);
    EXPECT_LT(a_offset, b_offset);
    EXPECT_LT(b_offset, unbound);
}

TEST(StudioQmlContract, ColorHarmonizerLoadsNumericControlsWithoutForbiddenPresentation)
{
    QFile panel(QStringLiteral(RAVO_STUDIO_DEVELOP_PANEL_QML));
    ASSERT_TRUE(panel.open(QIODevice::ReadOnly | QIODevice::Text))
        << panel.errorString().toStdString();
    const auto source = QString::fromUtf8(panel.readAll());

    const auto section_begin = source.indexOf(QStringLiteral("colorHarmonizerEnabled"));
    const auto section_end =
        source.indexOf(QStringLiteral("qsTr(\"Color Reconstruction\")"), section_begin);
    ASSERT_GE(section_begin, 0);
    ASSERT_GT(section_end, section_begin);
    const auto section = source.mid(section_begin, section_end - section_begin);
    EXPECT_TRUE(section.contains(QStringLiteral("root.presenter.editColorHarmonizer")));
    EXPECT_TRUE(section.contains(
        QStringLiteral("setDevelopNumber(\"colorHarmonizerEnabled\", checked ? 1 : 0)")));
    EXPECT_TRUE(section.contains(QStringLiteral("colorHarmonizerRuleIndex")));
    EXPECT_TRUE(section.contains(QStringLiteral("editColorHarmonizer.ruleChoices")));
    EXPECT_TRUE(section.contains(
        QStringLiteral("setDevelopNumber(\"colorHarmonizerRuleIndex\", currentIndex)")));
    EXPECT_TRUE(section.contains(QStringLiteral("editColorHarmonizer.sharedControls")));
    EXPECT_TRUE(section.contains(QStringLiteral("editColorHarmonizer.customNodeControl")));
    EXPECT_TRUE(section.contains(QStringLiteral("editColorHarmonizer.customHueControls")));
    EXPECT_TRUE(section.contains(QStringLiteral("editColorHarmonizer.nodeSaturationControls")));
    EXPECT_TRUE(section.contains(QStringLiteral("modelData.minimum")));
    EXPECT_TRUE(section.contains(QStringLiteral("modelData.maximum")));
    EXPECT_TRUE(section.contains(QStringLiteral("modelData.step")));
    EXPECT_TRUE(section.contains(QStringLiteral("modelData.reset")));
    EXPECT_TRUE(section.contains(QStringLiteral("modelData.visible")));
    EXPECT_TRUE(section.contains(QStringLiteral("nodeControl.field")));
    EXPECT_TRUE(section.contains(QStringLiteral("modelData.field")));
    EXPECT_TRUE(section.contains(QStringLiteral("editColorHarmonizer.customRule")));
    EXPECT_TRUE(section.contains(QStringLiteral("editColorHarmonizer.customNodeCount")));
    EXPECT_FALSE(section.contains(QStringLiteral("\"minimum\": 0, \"maximum\": 360")));
    EXPECT_FALSE(section.contains(
        QStringLiteral("modelData.index < root.presenter.editColorHarmonizer.customNodeCount")));
    EXPECT_TRUE(section.contains(QStringLiteral("resetControl(\"colorHarmonizer\")")));
    EXPECT_FALSE(section.contains(QStringLiteral("OpenCL")));
    EXPECT_FALSE(section.contains(QStringLiteral("auto-detect")));
    EXPECT_FALSE(section.contains(QStringLiteral("histogram")));
    EXPECT_FALSE(section.contains(QStringLiteral("picker")));
    EXPECT_FALSE(section.contains(QStringLiteral("harmony guide"), Qt::CaseInsensitive));
    EXPECT_TRUE(section.contains(QStringLiteral("MaskEditor")));
    EXPECT_TRUE(section.contains(QStringLiteral("editColorHarmonizerMask")));
    EXPECT_TRUE(source.contains(QStringLiteral("component MaskEditor")));
    EXPECT_TRUE(source.contains(QStringLiteral("editGraduatedMask")));
    EXPECT_TRUE(source.contains(QStringLiteral("maskEditor.mask.numericControls")));
    EXPECT_TRUE(source.contains(QStringLiteral("maskEditor.mask.kindChoices")));
    EXPECT_TRUE(source.contains(QStringLiteral("maskEditor.mask.sourceChoices")));
    EXPECT_TRUE(source.contains(QStringLiteral("maskEditor.mask.channelChoices")));
    EXPECT_TRUE(source.contains(QStringLiteral("resetControl(maskEditor.mask.detachField)")));
    EXPECT_TRUE(
        source.contains(QStringLiteral("maskEditor.mask.editable === true && modelData.visible")));
    EXPECT_TRUE(source.contains(QStringLiteral("Show mask overlay")));
    EXPECT_TRUE(source.contains(QStringLiteral("setMaskOverlay")));
    EXPECT_FALSE(source.contains(QStringLiteral("OpenCL")));
    EXPECT_FALSE(source.contains(QStringLiteral("JSON")));

    const auto contrast = source.indexOf(QStringLiteral("colorContrastEnabled"));
    const auto harmonizer = source.indexOf(QStringLiteral("colorHarmonizerEnabled"), contrast);
    const auto reconstruction =
        source.indexOf(QStringLiteral("qsTr(\"Color Reconstruction\")"), harmonizer);
    ASSERT_GE(contrast, 0);
    ASSERT_GE(harmonizer, 0);
    ASSERT_GE(reconstruction, 0);
    EXPECT_LT(contrast, harmonizer);
    EXPECT_LT(harmonizer, reconstruction);
}

TEST(StudioQmlContract, ColorReconstructionExposesTheFrozenV3Surface)
{
    QFile panel(QStringLiteral(RAVO_STUDIO_DEVELOP_PANEL_QML));
    ASSERT_TRUE(panel.open(QIODevice::ReadOnly | QIODevice::Text))
        << panel.errorString().toStdString();
    const auto source = QString::fromUtf8(panel.readAll());

    const auto section_begin = source.indexOf(QStringLiteral("colorReconstructionEnabled"));
    const auto section_end = source.indexOf(QStringLiteral("qsTr(\"Color Zones\")"), section_begin);
    ASSERT_GE(section_begin, 0);
    ASSERT_GT(section_end, section_begin);
    const auto section = source.mid(section_begin, section_end - section_begin);
    EXPECT_TRUE(section.contains(QStringLiteral("root.presenter.editColorReconstruction")));
    EXPECT_TRUE(section.contains(QStringLiteral("colorReconstructionPrecedenceIndex")));
    EXPECT_TRUE(section.contains(QStringLiteral("colorReconstructionThreshold")));
    EXPECT_TRUE(section.contains(QStringLiteral("colorReconstructionSpatial")));
    EXPECT_TRUE(section.contains(QStringLiteral("colorReconstructionRange")));
    EXPECT_TRUE(section.contains(QStringLiteral("colorReconstructionHueDegrees")));
    EXPECT_TRUE(section.contains(QStringLiteral("\"minimum\": 50")));
    EXPECT_TRUE(section.contains(QStringLiteral("\"maximum\": 150")));
    EXPECT_TRUE(section.contains(QStringLiteral("\"maximum\": 1000")));
    EXPECT_TRUE(section.contains(QStringLiteral("\"maximum\": 50")));
    EXPECT_TRUE(section.contains(QStringLiteral("from: 0")));
    EXPECT_TRUE(section.contains(QStringLiteral("to: 360")));
    EXPECT_TRUE(section.contains(QStringLiteral("precedenceIndex === 2")));
    EXPECT_TRUE(section.contains(QStringLiteral("resetControl(\"colorReconstruction\")")));
    EXPECT_FALSE(section.contains(QStringLiteral("OpenCL")));
    EXPECT_FALSE(section.contains(QStringLiteral("picker"), Qt::CaseInsensitive));
    EXPECT_FALSE(section.contains(QStringLiteral("GTK"), Qt::CaseInsensitive));

    const auto monochrome = source.indexOf(QStringLiteral("qsTr(\"Monochrome\")"));
    const auto split_toning = source.indexOf(QStringLiteral("qsTr(\"Split Toning\")"));
    const auto advanced = source.indexOf(QStringLiteral("qsTr(\"Color · Advanced\")"));
    const auto harmonizer = source.indexOf(QStringLiteral("colorHarmonizerEnabled"));
    const auto reconstruction = source.indexOf(QStringLiteral("colorReconstructionEnabled"));
    ASSERT_GE(monochrome, 0);
    ASSERT_GE(split_toning, 0);
    ASSERT_GE(advanced, 0);
    ASSERT_GE(harmonizer, 0);
    ASSERT_GE(reconstruction, 0);
    EXPECT_LT(split_toning, monochrome);
    EXPECT_LT(monochrome, advanced);
    EXPECT_LT(advanced, harmonizer);
    EXPECT_LT(harmonizer, reconstruction);
}

TEST(StudioQmlContract, SharpenExposesAmountRadiusAndThresholdFromOnePresenter)
{
    QFile panel(QStringLiteral(RAVO_STUDIO_DEVELOP_PANEL_QML));
    ASSERT_TRUE(panel.open(QIODevice::ReadOnly | QIODevice::Text))
        << panel.errorString().toStdString();
    const auto source = QString::fromUtf8(panel.readAll());
    const auto begin = source.indexOf(QStringLiteral("title: qsTr(\"Sharpen\")"));
    const auto end = source.indexOf(QStringLiteral("title: qsTr(\"Clarity\")"), begin);
    ASSERT_GE(begin, 0);
    ASSERT_GT(end, begin);
    const auto section = source.mid(begin, end - begin);
    EXPECT_TRUE(section.contains(QStringLiteral("root.presenter.editSharpen")));
    EXPECT_TRUE(section.contains(QStringLiteral("root.presenter.editSharpenRadius")));
    EXPECT_TRUE(section.contains(QStringLiteral("root.presenter.editSharpenThreshold")));
    EXPECT_TRUE(section.contains(QStringLiteral("previewDevelopNumber(\"sharpen\"")));
    EXPECT_TRUE(section.contains(QStringLiteral("previewDevelopNumber(\"sharpenRadius\"")));
    EXPECT_TRUE(section.contains(QStringLiteral("previewDevelopNumber(\"sharpenThreshold\"")));
    EXPECT_TRUE(section.contains(QStringLiteral("to: 8")));
    EXPECT_TRUE(section.contains(QStringLiteral("to: 100")));
    EXPECT_TRUE(section.contains(QStringLiteral("resetValue: 0.5")));
    EXPECT_FALSE(section.contains(QStringLiteral("OpenCL")));
}

TEST(StudioQmlContract, DehazeExposesStrengthDistanceAndAdaptiveScale)
{
    QFile panel(QStringLiteral(RAVO_STUDIO_DEVELOP_PANEL_QML));
    ASSERT_TRUE(panel.open(QIODevice::ReadOnly | QIODevice::Text))
        << panel.errorString().toStdString();
    const auto source = QString::fromUtf8(panel.readAll());
    const auto begin = source.indexOf(QStringLiteral("title: qsTr(\"Dehaze\")"));
    const auto end = source.indexOf(QStringLiteral("sectionId: \"detail\""), begin);
    ASSERT_GE(begin, 0);
    ASSERT_GT(end, begin);
    const auto section = source.mid(begin, end - begin);
    EXPECT_TRUE(section.contains(QStringLiteral("root.presenter.editDehaze")));
    EXPECT_TRUE(section.contains(QStringLiteral("root.presenter.editDehazeDistance")));
    EXPECT_TRUE(section.contains(QStringLiteral("root.presenter.editDehazeAdaptive")));
    EXPECT_TRUE(section.contains(QStringLiteral("previewDevelopNumber(\"dehaze\"")));
    EXPECT_TRUE(section.contains(QStringLiteral("previewDevelopNumber(\"dehazeDistance\"")));
    EXPECT_TRUE(section.contains(QStringLiteral("setDevelopNumber(\"dehazeAdaptive\"")));
    EXPECT_TRUE(section.contains(QStringLiteral("qsTr(\"Adaptive window scale\")")));
    EXPECT_FALSE(section.contains(QStringLiteral("OpenCL")));
}

TEST(StudioQmlContract, OutputDitherUsesPresenterMethodsWithoutQmlPixelMath)
{
    QFile panel(QStringLiteral(RAVO_STUDIO_DEVELOP_PANEL_QML));
    ASSERT_TRUE(panel.open(QIODevice::ReadOnly | QIODevice::Text))
        << panel.errorString().toStdString();
    const auto source = QString::fromUtf8(panel.readAll());
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"outputDitherEnabled\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"outputDitherMethod\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("root.presenter.editOutputDither.methodChoices")));
    EXPECT_TRUE(source.contains(QStringLiteral("outputDitherMethodIndex")));
    EXPECT_TRUE(source.contains(QStringLiteral("outputDitherDamping")));
    EXPECT_TRUE(source.contains(QStringLiteral("resetControl(\"outputDither\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("Auto dithers integer exports")));
    EXPECT_FALSE(source.contains(QStringLiteral("7.0 / 16.0")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"canvasEnabled\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("id: canvasEnabledBox")));
    EXPECT_TRUE(source.contains(QStringLiteral("root.presenter.editCanvasEnabled")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Enlarge Canvas\")")));
    EXPECT_FALSE(source.contains(QStringLiteral("qsTr(\"Enable enlarged canvas\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("visible: canvasEnabledBox.checked")));
    EXPECT_TRUE(source.contains(QStringLiteral("root.presenter.editCanvas.colorChoices")));
    EXPECT_TRUE(source.contains(QStringLiteral("canvasColorIndex")));
    EXPECT_TRUE(source.contains(QStringLiteral("resetControl(\"canvas\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"outputFrameEnabled\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("root.presenter.editOutputFrame.basisChoices")));
    EXPECT_TRUE(source.contains(QStringLiteral("outputFrameLineOffset")));
    EXPECT_TRUE(source.contains(QStringLiteral("resetControl(\"outputFrame\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"watermarkEnabled\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"watermarkText\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("root.presenter.editWatermark.alignmentChoices")));
    EXPECT_TRUE(source.contains(QStringLiteral("setDevelopText(\"watermarkText\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("resetControl(\"watermark\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"colorZonesEnabled\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("root.presenter.editColorZones.selectByChoices")));
    EXPECT_TRUE(source.contains(QStringLiteral("colorZonesChroma")));
    EXPECT_TRUE(source.contains(QStringLiteral("colorZonesHueInterpolationIndex")));
    EXPECT_TRUE(source.contains(QStringLiteral("resetControl(\"colorZones\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"monochromeEnabled\"")));
    EXPECT_TRUE(
        source.contains(QStringLiteral("root.presenter.editMonochromeFilter[modelData.key]")));
    EXPECT_TRUE(source.contains(QStringLiteral("monochromeHighlights")));
    EXPECT_TRUE(source.contains(QStringLiteral("resetControl(\"monochrome\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"splitToningEnabled\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("root.presenter.editSplitToning.shadowSaturation")));
    EXPECT_TRUE(source.contains(QStringLiteral("splitHighlightSaturation")));
    EXPECT_TRUE(source.contains(QStringLiteral("splitCompress")));
    EXPECT_TRUE(source.contains(QStringLiteral("resetControl(\"splitToning\")")));
}

TEST(StudioQmlContract, DevelopPanelUsesDefaultGradingStackWithoutBuryingColorEq)
{
    QFile panel(QStringLiteral(RAVO_STUDIO_DEVELOP_PANEL_QML));
    ASSERT_TRUE(panel.open(QIODevice::ReadOnly | QIODevice::Text))
        << panel.errorString().toStdString();
    const auto source = QString::fromUtf8(panel.readAll());
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
    EXPECT_LT(white_balance, light);
    EXPECT_LT(light, curves);
    EXPECT_LT(curves, color_eq);
    EXPECT_LT(color_eq, color);
    EXPECT_LT(color, geometry);
    EXPECT_LT(geometry, graduated);
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Color Equalizer\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Graduated ND\")")));
    EXPECT_FALSE(source.contains(QStringLiteral("Graduated ND / Color EQ")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"colorBalanceShadowsWheel\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"colorBalanceMidtonesWheel\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"colorBalanceHighlightsWheel\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("hueField: \"colorBalanceShadowsHue\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("chromaField: \"colorBalanceShadowsChroma\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Color Balance RGB · more\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Color · Advanced\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"colorEqChannel\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"colorEqBand\" + modelData.index")));
    EXPECT_TRUE(source.contains(QStringLiteral("editColorEqBands")));
    EXPECT_TRUE(source.contains(QStringLiteral("satField")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"curveFamily\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"curveChannel\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"curveEditor\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Curves\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Monotonic\")")));
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
    EXPECT_FALSE(wheel_source.contains(QStringLiteral("OpenCL")));
}

TEST(StudioQmlContract, RetouchAuthorsOrderedRegionsThroughCommandBoundary)
{
    QFile panel(QStringLiteral(RAVO_STUDIO_DEVELOP_PANEL_QML));
    ASSERT_TRUE(panel.open(QIODevice::ReadOnly | QIODevice::Text))
        << panel.errorString().toStdString();
    const auto source = QString::fromUtf8(panel.readAll());
    EXPECT_TRUE(source.contains(QStringLiteral("id: retouchEditor")));
    EXPECT_TRUE(source.contains(QStringLiteral("root.presenter.editRetouch.regions")));
    EXPECT_TRUE(source.contains(QStringLiteral("root.commands.addRetouchRegion")));
    EXPECT_TRUE(source.contains(QStringLiteral("root.commands.removeRetouchRegion")));
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

TEST(StudioQmlContract, DevelopSectionsFollowLightroomEditOrder)
{
    QFile panel(QStringLiteral(RAVO_STUDIO_DEVELOP_PANEL_QML));
    ASSERT_TRUE(panel.open(QIODevice::ReadOnly | QIODevice::Text))
        << panel.errorString().toStdString();
    const auto source = QString::fromUtf8(panel.readAll());
    EXPECT_FALSE(source.contains(QStringLiteral("qsTr(\"Undo\")")));
    EXPECT_FALSE(source.contains(QStringLiteral("qsTr(\"Reset all\")")));
    const QStringList order{
        QStringLiteral("whiteBalance"), QStringLiteral("light"),
        QStringLiteral("curves"),       QStringLiteral("colorEqualizer"),
        QStringLiteral("color"),        QStringLiteral("primaries"),
        QStringLiteral("geometry"),     QStringLiteral("toneEqual"),
        QStringLiteral("graduated"),    QStringLiteral("effects"),
        QStringLiteral("detail"),       QStringLiteral("raw"),
        QStringLiteral("calibration"),
        QStringLiteral("inputProfile"), QStringLiteral("profileGamma"),
        QStringLiteral("outputProfile"),
    };
    qsizetype cursor = source.indexOf(QStringLiteral("component DevelopSection"));
    ASSERT_GE(cursor, 0);
    for (const auto &id : order)
    {
        const auto needle = QStringLiteral("sectionId: \"%1\"").arg(id);
        const auto found = source.indexOf(needle, cursor);
        ASSERT_GE(found, 0) << id.toStdString();
        EXPECT_GT(found, cursor) << id.toStdString();
        cursor = found + needle.size();
    }
}

TEST(StudioQmlContract, GeometryCropToolbarUsesIconsAndAspectLock)
{
    QFile panel(QStringLiteral(RAVO_STUDIO_DEVELOP_PANEL_QML));
    ASSERT_TRUE(panel.open(QIODevice::ReadOnly | QIODevice::Text))
        << panel.errorString().toStdString();
    const auto source = QString::fromUtf8(panel.readAll());
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
    EXPECT_FALSE(geometry.contains(QStringLiteral("qsTr(\"Angle\")")));
    EXPECT_FALSE(geometry.contains(QStringLiteral("previewDevelopNumber(\"straighten\"")));
    EXPECT_FALSE(geometry.contains(QStringLiteral("resetControl(\"straighten\")")));
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
    EXPECT_TRUE(library_source.contains(QStringLiteral("Layout.preferredWidth: ControlState.borderThin")));

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
    EXPECT_TRUE(history_source.contains(QStringLiteral("qsTr(\"Copy\")")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("qsTr(\"Paste\")")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("qsTr(\"Paste Light\")")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("qsTr(\"Paste Color\")")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("pasteEditsSection(\"light\")")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("pasteEditsSection(\"color\")")));
    EXPECT_LT(history_source.indexOf(QStringLiteral("qsTr(\"Snapshot\")")),
              history_source.indexOf(QStringLiteral("qsTr(\"Copy\")")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("Layout.preferredWidth: 1")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("copyEdits")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("pasteEdits")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("hasCopiedEdits")));

    QFile actions(QStringLiteral(RAVO_STUDIO_ACTIONS_QML));
    ASSERT_TRUE(actions.open(QIODevice::ReadOnly | QIODevice::Text))
        << actions.errorString().toStdString();
    const auto action_source = QString::fromUtf8(actions.readAll());
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.photoRenameSnapshot")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.editCopyEdits")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.editPasteEdits")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.editPasteEditsSection")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.editSetNumbers")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.editPickWhiteBalance")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.editSetWhiteBalancePick")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("previewDevelopNumbers(fields)")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("setDevelopNumbers(fields)")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("pasteEditsSection(section)")));
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
    EXPECT_EQ(QCoreApplication::translate("DevelopPanel", "Allow extended chroma"),
              QStringLiteral("允许扩展色度"));
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
    EXPECT_TRUE(visible_line.contains(QStringLiteral("photoPlane.width")));
    EXPECT_FALSE(visible_line.contains(QStringLiteral("cropGuideReady")));
    EXPECT_TRUE(source.contains(
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
    EXPECT_TRUE(source.contains(QStringLiteral("function beginInspectZoomAnimation()")));
    EXPECT_TRUE(source.contains(QStringLiteral("id: inspectZoomAnim")));
    EXPECT_TRUE(source.contains(QStringLiteral("inspectStageLockW")));
    EXPECT_TRUE(source.contains(QStringLiteral("inspectAnimScale")));
    EXPECT_TRUE(source.contains(QStringLiteral("transform: Scale")));
    EXPECT_TRUE(source.contains(QStringLiteral("cursorShape: studio.whiteBalancePickActive ? Qt.CrossCursor : Qt.BlankCursor")));
    EXPECT_TRUE(source.contains(QStringLiteral("id: magnifierCursor")));
    EXPECT_TRUE(source.contains(QStringLiteral("onDoubleTapped")));
    EXPECT_TRUE(source.contains(QStringLiteral("openGallery(\"grid\")")));
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
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"presetList\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("editPresets")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.presetImport")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.presetApplyPath")));
    EXPECT_FALSE(source.contains(QStringLiteral("OpenCL")));
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
    EXPECT_FALSE(source.contains(QStringLiteral("SegmentedControl")));
    EXPECT_FALSE(source.contains(QStringLiteral("srgb_to_linear")));
    EXPECT_FALSE(source.contains(QStringLiteral("rgb_to_d50_uv")));
}

} // namespace
} // namespace ravo
