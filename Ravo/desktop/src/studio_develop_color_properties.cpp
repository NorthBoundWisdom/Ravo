#include "ravo/desktop/studio_presenter.h"

#include "studio_develop_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include <numbers>
#include <set>
#include <string_view>
#include <utility>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUrl>
#include <QMetaObject>
#include <QMutexLocker>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include "ravo/recipe/develop.h"
#include "ravo/recipe/develop_mask.h"
#include "ravo/recipe/recipe.h"
#include "ravo/recipe/style.h"
#include "ravo/adapters/crs_xmp.h"
#include "ravo/adapters/text_file.h"
#include "studio_debug_info.h"
#include "studio_qt.h"

namespace ravo
{
using studio_develop_internal::develop_mask_editor_map;

namespace
{

constexpr double kColorHarmonizerHueStepDegrees = 0.1;
constexpr double kColorHarmonizerLinearStep = 0.01;
constexpr double kColorHarmonizerCustomNodesStep = 1.0;
constexpr int kColorHarmonizerHueDecimals = 1;
constexpr int kColorHarmonizerLinearDecimals = 2;
constexpr int kColorHarmonizerCustomNodesDecimals = 0;
// This is an interaction minimum only. Canonical recipes retain the exact
// strictly-positive hard lower bound exported by mask.h.

[[nodiscard]] QString color_harmonizer_rule_label(const std::string_view name)
{
    if (name == "monochromatic")
    {
        return QCoreApplication::translate("DevelopPanel", "Monochromatic");
    }
    if (name == "analogous")
    {
        return QCoreApplication::translate("DevelopPanel", "Analogous");
    }
    if (name == "analogous_complementary")
    {
        return QCoreApplication::translate("DevelopPanel", "Analogous complementary");
    }
    if (name == "complementary")
    {
        return QCoreApplication::translate("DevelopPanel", "Complementary");
    }
    if (name == "split_complementary")
    {
        return QCoreApplication::translate("DevelopPanel", "Split complementary");
    }
    if (name == "dyad")
    {
        return QCoreApplication::translate("DevelopPanel", "Dyad");
    }
    if (name == "triad")
    {
        return QCoreApplication::translate("DevelopPanel", "Triad");
    }
    if (name == "tetrad")
    {
        return QCoreApplication::translate("DevelopPanel", "Tetrad");
    }
    if (name == "square")
    {
        return QCoreApplication::translate("DevelopPanel", "Square");
    }
    if (name == "custom")
    {
        return QCoreApplication::translate("DevelopPanel", "Custom");
    }
    return {};
}

[[nodiscard]] QVariantMap color_harmonizer_control(const QString &title, const QString &key,
                                                   const QString &field, const double minimum,
                                                   const double maximum, const double step,
                                                   const double reset, const int decimals,
                                                   const bool visible)
{
    return {{QStringLiteral("title"), title},     {QStringLiteral("key"), key},
            {QStringLiteral("field"), field},     {QStringLiteral("minimum"), minimum},
            {QStringLiteral("maximum"), maximum}, {QStringLiteral("step"), step},
            {QStringLiteral("reset"), reset},     {QStringLiteral("decimals"), decimals},
            {QStringLiteral("visible"), visible}};
}


} // namespace

QVariantMap StudioPresenter::editColorHarmonizer() const
{
    const auto &params = develop_.color_harmonizer;
    const ColorHarmonizerParams defaults;
    const bool custom_rule = params.rule == ColorHarmonizerRule::kCustom;
    const int active_node_count = static_cast<int>(color_harmonizer_active_node_count(params));
    const bool anchor_visible = color_harmonizer_uses_anchor_hue(params.rule);

    QStringList rule_choices;
    rule_choices.reserve(static_cast<int>(kColorHarmonizerRuleCount));
    for (std::size_t index = 0U; index < kColorHarmonizerRuleCount; ++index)
    {
        const auto rule = color_harmonizer_rule_from_index(static_cast<std::int64_t>(index));
        const auto name = rule ? color_harmonizer_rule_name(rule.value()) : std::string_view{};
        rule_choices.push_back(color_harmonizer_rule_label(name));
    }

    const QVariantList shared_controls{
        color_harmonizer_control(QCoreApplication::translate("DevelopPanel", "Anchor hue"),
                                 QStringLiteral("anchorHueDegrees"),
                                 QStringLiteral("colorHarmonizerAnchorHueDegrees"),
                                 kColorHarmonizerHueDegreesMin, kColorHarmonizerHueDegreesMax,
                                 kColorHarmonizerHueStepDegrees,
                                 color_harmonizer_hue_turns_to_degrees(defaults.anchor_hue),
                                 kColorHarmonizerHueDecimals, anchor_visible),
        color_harmonizer_control(QCoreApplication::translate("DevelopPanel", "Pull strength"),
                                 QStringLiteral("pullStrength"),
                                 QStringLiteral("colorHarmonizerPullStrength"),
                                 kColorHarmonizerPullStrengthMin, kColorHarmonizerPullStrengthMax,
                                 kColorHarmonizerLinearStep, defaults.pull_strength,
                                 kColorHarmonizerLinearDecimals, true),
        color_harmonizer_control(QCoreApplication::translate("DevelopPanel", "Neutral protection"),
                                 QStringLiteral("neutralProtection"),
                                 QStringLiteral("colorHarmonizerNeutralProtection"),
                                 kColorHarmonizerNeutralProtectionMin,
                                 kColorHarmonizerNeutralProtectionMax, kColorHarmonizerLinearStep,
                                 defaults.neutral_protection, kColorHarmonizerLinearDecimals, true),
        color_harmonizer_control(
            QCoreApplication::translate("DevelopPanel", "Pull width"), QStringLiteral("pullWidth"),
            QStringLiteral("colorHarmonizerPullWidth"), kColorHarmonizerPullWidthMin,
            kColorHarmonizerPullWidthMax, kColorHarmonizerLinearStep, defaults.pull_width,
            kColorHarmonizerLinearDecimals, true),
        color_harmonizer_control(
            QCoreApplication::translate("DevelopPanel", "Smoothing"), QStringLiteral("smoothing"),
            QStringLiteral("colorHarmonizerSmoothing"), kColorHarmonizerSmoothingMin,
            kColorHarmonizerSmoothingMax, kColorHarmonizerLinearStep, defaults.smoothing,
            kColorHarmonizerLinearDecimals, true),
    };

    const QVariantMap custom_node_control = color_harmonizer_control(
        QCoreApplication::translate("DevelopPanel", "Custom nodes"),
        QStringLiteral("customNodeCount"), QStringLiteral("colorHarmonizerCustomNodeCount"),
        static_cast<double>(kColorHarmonizerCustomNodesMin),
        static_cast<double>(kColorHarmonizerCustomNodesMax), kColorHarmonizerCustomNodesStep,
        static_cast<double>(defaults.num_custom_nodes), kColorHarmonizerCustomNodesDecimals,
        custom_rule);

    QVariantList custom_hue_controls;
    QVariantList node_saturation_controls;
    custom_hue_controls.reserve(static_cast<int>(kColorHarmonizerNodeSlotCount));
    node_saturation_controls.reserve(static_cast<int>(kColorHarmonizerNodeSlotCount));
    static const std::array<const char *, 4> hue_titles{
        QT_TRANSLATE_NOOP("DevelopPanel", "Custom hue 1"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Custom hue 2"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Custom hue 3"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Custom hue 4")};
    static const std::array<const char *, 4> sat_titles{
        QT_TRANSLATE_NOOP("DevelopPanel", "Node saturation 1"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Node saturation 2"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Node saturation 3"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Node saturation 4")};
    for (std::size_t index = 0U; index < kColorHarmonizerNodeSlotCount; ++index)
    {
        const auto suffix = QString::number(static_cast<int>(index));
        custom_hue_controls.push_back(color_harmonizer_control(
            QCoreApplication::translate("DevelopPanel", hue_titles[index]),
            QStringLiteral("customHue%1Degrees").arg(suffix),
            QStringLiteral("colorHarmonizerCustomHue%1Degrees").arg(suffix),
            kColorHarmonizerHueDegreesMin, kColorHarmonizerHueDegreesMax,
            kColorHarmonizerHueStepDegrees,
            color_harmonizer_hue_turns_to_degrees(defaults.custom_hue[index]),
            kColorHarmonizerHueDecimals, color_harmonizer_uses_custom_hue(params, index)));
        node_saturation_controls.push_back(color_harmonizer_control(
            QCoreApplication::translate("DevelopPanel", sat_titles[index]),
            QStringLiteral("nodeSaturation%1").arg(suffix),
            QStringLiteral("colorHarmonizerNodeSaturation%1").arg(suffix),
            kColorHarmonizerNodeSaturationMin, kColorHarmonizerNodeSaturationMax,
            kColorHarmonizerLinearStep, defaults.node_saturation[index],
            kColorHarmonizerLinearDecimals, color_harmonizer_uses_node_saturation(params, index)));
    }

    return {
        {QStringLiteral("enabled"), develop_.color_harmonizer_enabled},
        {QStringLiteral("ruleIndex"), static_cast<int>(color_harmonizer_rule_index(params.rule))},
        {QStringLiteral("ruleChoices"), rule_choices},
        {QStringLiteral("customRule"), custom_rule},
        {QStringLiteral("activeNodeCount"), active_node_count},
        {QStringLiteral("anchorVisible"), anchor_visible},
        {QStringLiteral("anchorHueDegrees"),
         color_harmonizer_hue_turns_to_degrees(params.anchor_hue)},
        {QStringLiteral("pullStrength"), params.pull_strength},
        {QStringLiteral("neutralProtection"), params.neutral_protection},
        {QStringLiteral("pullWidth"), params.pull_width},
        {QStringLiteral("smoothing"), params.smoothing},
        {QStringLiteral("customNodeCount"), static_cast<int>(params.num_custom_nodes)},
        {QStringLiteral("customHue0Degrees"),
         color_harmonizer_hue_turns_to_degrees(params.custom_hue[0])},
        {QStringLiteral("customHue1Degrees"),
         color_harmonizer_hue_turns_to_degrees(params.custom_hue[1])},
        {QStringLiteral("customHue2Degrees"),
         color_harmonizer_hue_turns_to_degrees(params.custom_hue[2])},
        {QStringLiteral("customHue3Degrees"),
         color_harmonizer_hue_turns_to_degrees(params.custom_hue[3])},
        {QStringLiteral("nodeSaturation0"), params.node_saturation[0]},
        {QStringLiteral("nodeSaturation1"), params.node_saturation[1]},
        {QStringLiteral("nodeSaturation2"), params.node_saturation[2]},
        {QStringLiteral("nodeSaturation3"), params.node_saturation[3]},
        {QStringLiteral("sharedControls"), shared_controls},
        {QStringLiteral("customNodeControl"), custom_node_control},
        {QStringLiteral("customHueControls"), custom_hue_controls},
        {QStringLiteral("nodeSaturationControls"), node_saturation_controls}};
}

QVariantMap StudioPresenter::editColorHarmonizerMask() const
{
    return develop_mask_editor_map(
        develop_mask_editor_state(develop_, DevelopMaskTarget::kColorHarmonizer),
        DevelopMaskTarget::kColorHarmonizer);
}

QVariantMap StudioPresenter::editColorBalanceRgbMask() const
{
    return develop_mask_editor_map(
        develop_mask_editor_state(develop_, DevelopMaskTarget::kColorBalanceRgb),
        DevelopMaskTarget::kColorBalanceRgb);
}

bool StudioPresenter::maskOverlayVisible() const noexcept
{
    return mask_overlay_visible_;
}

QString StudioPresenter::maskOverlayTarget() const
{
    return mask_overlay_target_;
}

void StudioPresenter::setMaskOverlay(const QString &target, const bool visible)
{
    QString normalized = QStringLiteral("color_harmonizer");
    if (target == QLatin1String("graduatednd"))
        normalized = QStringLiteral("graduatednd");
    else if (target == QLatin1String("color_balance_rgb"))
        normalized = QStringLiteral("color_balance_rgb");
    else if (target == QLatin1String("exposure"))
        normalized = QStringLiteral("exposure");
    const bool comparison_changed = visible && clear_comparison();
    const bool changed = mask_overlay_visible_ != visible || mask_overlay_target_ != normalized;
    mask_overlay_visible_ = visible;
    mask_overlay_target_ = normalized;
    if (!changed && !comparison_changed)
    {
        return;
    }
    if (comparison_changed)
    {
        emit editChanged();
    }
    emit previewChanged();
    if (!visible)
    {
        if (!preview_base_image_.isNull())
        {
            const QMutexLocker lock(&preview_image_mutex_);
            preview_image_ = preview_base_image_;
        }
        emit previewChanged();
        return;
    }
    enqueue_preview();
}

void StudioPresenter::retranslate()
{
    emit editChanged();
}

double StudioPresenter::editMonochrome() const noexcept
{
    return develop_.monochrome.mix;
}

QVariantMap StudioPresenter::editMonochromeFilter() const
{
    const auto &params = develop_.monochrome;
    return {{QStringLiteral("present"), develop_.monochrome_present},
            {QStringLiteral("enabled"), develop_.monochrome_enabled},
            {QStringLiteral("masked"), develop_.monochrome_mask_id.has_value()},
            {QStringLiteral("filterA"), params.filter_a},
            {QStringLiteral("filterB"), params.filter_b},
            {QStringLiteral("size"), params.size},
            {QStringLiteral("highlights"), params.highlights},
            {QStringLiteral("mix"), params.mix}};
}

double StudioPresenter::editSplitShadowsHue() const noexcept
{
    return develop_.split_toning.shadow_hue;
}

double StudioPresenter::editSplitHighlightsHue() const noexcept
{
    return develop_.split_toning.highlight_hue;
}

double StudioPresenter::editSplitBalance() const noexcept
{
    return develop_.split_toning.balance;
}

double StudioPresenter::editSplitAmount() const noexcept
{
    return develop_.split_toning.mix;
}

QVariantMap StudioPresenter::editSplitToning() const
{
    const auto &params = develop_.split_toning;
    return {{QStringLiteral("present"), develop_.split_toning_present},
            {QStringLiteral("enabled"), develop_.split_toning_enabled},
            {QStringLiteral("masked"), develop_.split_toning_mask_id.has_value()},
            {QStringLiteral("shadowSaturation"), params.shadow_saturation},
            {QStringLiteral("highlightSaturation"), params.highlight_saturation},
            {QStringLiteral("compress"), params.compress},
            {QStringLiteral("mix"), params.mix}};
}

double StudioPresenter::editGamma() const noexcept
{
    return develop_.gamma;
}

QVariantMap StudioPresenter::editRgbLevels() const
{
    const auto &params = develop_.rgb_levels;
    int preserve_index = 1;
    const std::array<std::string_view, 7> names{
        kToneCurvePreserveColorsNone, kToneCurvePreserveColorsLuminance,
        kToneCurvePreserveColorsMax,  kToneCurvePreserveColorsAverage,
        kToneCurvePreserveColorsSum,  kToneCurvePreserveColorsNorm,
        kToneCurvePreserveColorsPower};
    for (int index = 0; index < static_cast<int>(names.size()); ++index)
    {
        if (params.preserve_colors == names[static_cast<std::size_t>(index)])
        {
            preserve_index = index;
            break;
        }
    }
    return {{QStringLiteral("modeIndex"), params.mode == kRgbLevelsModeIndependent ? 1 : 0},
            {QStringLiteral("preserveIndex"), preserve_index},
            {QStringLiteral("black"), params.levels[0][0]},
            {QStringLiteral("grey"), params.levels[0][1]},
            {QStringLiteral("white"), params.levels[0][2]},
            {QStringLiteral("blackG"), params.levels[1][0]},
            {QStringLiteral("greyG"), params.levels[1][1]},
            {QStringLiteral("whiteG"), params.levels[1][2]},
            {QStringLiteral("blackB"), params.levels[2][0]},
            {QStringLiteral("greyB"), params.levels[2][1]},
            {QStringLiteral("whiteB"), params.levels[2][2]}};
}

QVariantList StudioPresenter::editToneCurve() const
{
    return tone_curve_to_variant(develop_.tone_curve);
}

QVariantList StudioPresenter::editToneCurveSamples() const
{
    return tone_curve_sample_list(develop_.tone_curve, develop_.tone_curve_interpolation);
}

} // namespace ravo
