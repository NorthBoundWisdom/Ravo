#include <gtest/gtest.h>

#include <array>

#include <QCoreApplication>
#include <QFile>
#include <QKeySequence>
#include <QTranslator>

#include "ravo/desktop/preview_request_owner.h"
#include "ravo/desktop/studio_command_controller.h"
#include "ravo/desktop/studio_presenter.h"

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
        const auto expected = QStringLiteral("\"field\": \"%1\", \"minimum\": 0, \"maximum\": 2")
                                  .arg(QString::fromLatin1(field));
        EXPECT_TRUE(source.contains(expected)) << field;
    }
    EXPECT_TRUE(source.contains(QStringLiteral(
        "\"field\": \"legacyColorBalanceContrast\", \"minimum\": 0.01, \"maximum\": 1.99")));
    EXPECT_TRUE(source.contains(QStringLiteral(
        "\"field\": \"legacyColorBalanceGreyFulcrum\", \"minimum\": 0.1, \"maximum\": 100")));
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
        const auto expected = QStringLiteral("\"field\": \"%1\", \"minimum\": -40, \"maximum\": 40")
                                  .arg(QString::fromLatin1(field));
        EXPECT_TRUE(source.contains(expected)) << field;
    }
    EXPECT_TRUE(source.contains(QStringLiteral(
        "\"field\": \"colorCorrectionSaturation\", \"minimum\": -3, \"maximum\": 3")));

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
        const auto expected = QStringLiteral("\"field\": \"%1\", \"minimum\": 0, \"maximum\": 5")
                                  .arg(QString::fromLatin1(field));
        EXPECT_TRUE(source.contains(expected)) << field;
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

} // namespace
} // namespace ravo
