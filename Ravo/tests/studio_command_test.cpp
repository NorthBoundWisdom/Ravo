#include <gtest/gtest.h>

#include <algorithm>
#include <array>

#include <QVariantMap>

#include <QCoreApplication>
#include <QFile>
#include <QKeySequence>
#include <QTranslator>

#include "ravo/desktop/preview_request_owner.h"
#include "ravo/desktop/studio_command_controller.h"
#include "ravo/desktop/studio_presenter.h"
#include "ravo/recipe/color_harmonizer.h"
#include "ravo/recipe/develop_mask.h"

namespace ravo
{
namespace
{

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
    EXPECT_EQ(harmonizer_mask.value(QStringLiteral("kindChoices")).toStringList().size(), 6);
    EXPECT_EQ(harmonizer_mask.value(QStringLiteral("sourceChoices")).toStringList().size(), 2);
    EXPECT_EQ(harmonizer_mask.value(QStringLiteral("channelChoices")).toStringList().size(), 4);
    const auto mask_controls = harmonizer_mask.value(QStringLiteral("numericControls")).toList();
    ASSERT_EQ(mask_controls.size(), 15);
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
    EXPECT_EQ(white_balance.size(), 6);
    EXPECT_EQ(white_balance.value(QStringLiteral("modeIndex")).toInt(), 0);
    EXPECT_FALSE(white_balance.value(QStringLiteral("hasCoefficients")).toBool());
    EXPECT_DOUBLE_EQ(white_balance.value(QStringLiteral("red")).toDouble(), 1.0);
    EXPECT_DOUBLE_EQ(white_balance.value(QStringLiteral("green")).toDouble(), 1.0);
    EXPECT_DOUBLE_EQ(white_balance.value(QStringLiteral("blue")).toDouble(), 1.0);
    EXPECT_DOUBLE_EQ(white_balance.value(QStringLiteral("fourth")).toDouble(), 1.0);
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
    const auto section_end = source.indexOf(QStringLiteral("monochrome"), section_begin);
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
    const auto section_end = source.indexOf(QStringLiteral("qsTr(\"Monochrome\")"), section_begin);
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
    const auto monochrome = source.indexOf(QStringLiteral("qsTr(\"Monochrome\")"), harmonizer);
    ASSERT_GE(contrast, 0);
    ASSERT_GE(harmonizer, 0);
    ASSERT_GE(monochrome, 0);
    EXPECT_LT(contrast, harmonizer);
    EXPECT_LT(harmonizer, monochrome);
}

TEST(StudioCommands, BuiltinRegistryIsCompleteAndConflictFree)
{
    EXPECT_TRUE(StudioCommandController::validateBuiltinDefinitions().isEmpty());
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
    const auto export_open = ids.value(QStringLiteral("libraryExport")).toString();

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
    const auto bounds = presenter.exportOptionBounds();
    EXPECT_EQ(bounds.value(QStringLiteral("jpegQualityMin")).toInt(), 5);
    EXPECT_EQ(bounds.value(QStringLiteral("tiffResolutionDpiMax")).toInt(), 9600);
}

TEST(StudioQmlContract, ExportOptionsDialogExposesEveryFormatWithoutCodecParsing)
{
    QFile dialog(QStringLiteral(RAVO_STUDIO_EXPORT_OPTIONS_QML));
    ASSERT_TRUE(dialog.open(QIODevice::ReadOnly | QIODevice::Text))
        << dialog.errorString().toStdString();
    const auto source = QString::fromUtf8(dialog.readAll());

    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"ExportOptionsDialog\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Format\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Quality\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Subsampling\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Bit depth\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Compression\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Sample type\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Compression level\")")));
    EXPECT_TRUE(
        source.contains(QStringLiteral("qsTr(\"Write grayscale when the image is neutral\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Resolution (dpi)\")")));
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
    EXPECT_FALSE(source.contains(QStringLiteral("parse_export")));
    EXPECT_FALSE(source.contains(QStringLiteral("export_format_from_ui")));
    EXPECT_FALSE(source.contains(QStringLiteral("JpegExportOptions")));
    EXPECT_FALSE(source.contains(QStringLiteral("toLowerCase()")));
    EXPECT_FALSE(source.contains(QStringLiteral("Math.round")));
    EXPECT_FALSE(source.contains(QStringLiteral("property double jpegQuality: 95")));
    EXPECT_FALSE(source.contains(QStringLiteral("property double tiffResolutionDpi: 300")));
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
    EXPECT_TRUE(source.contains(QStringLiteral("\"format\": format")));
    EXPECT_TRUE(source.contains(QStringLiteral("\"options\": options")));
    EXPECT_TRUE(source.contains(QStringLiteral("onFileRejected: window.clearPendingExport()")));
    EXPECT_TRUE(source.contains(QStringLiteral("exportOptionsDialog.visible")));
    EXPECT_FALSE(source.contains(QStringLiteral("\"filter\": selectedFilter")));
    EXPECT_FALSE(source.contains(QStringLiteral("JPEG (*.jpg *.jpeg)\", \"PNG (*.png)\"")));
}

} // namespace
} // namespace ravo
