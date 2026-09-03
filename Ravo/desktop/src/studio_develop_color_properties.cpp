#include "ravo/desktop/studio_presenter.h"

#include "studio_develop_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <numbers>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
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
using studio_develop_internal::develop_mask_field_prefix;
using studio_develop_internal::develop_mask_target_from_name;
using studio_develop_internal::map_mask_place_preview;
using studio_develop_internal::mask_place_geometry_allowed;

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
    else if (target == QLatin1String("rgb_curve"))
        normalized = QStringLiteral("rgb_curve");
    else if (target == QLatin1String("tone_curve"))
        normalized = QStringLiteral("tone_curve");
    else if (target == QLatin1String("highlights"))
        normalized = QStringLiteral("highlights");
    else if (target == QLatin1String("shadows"))
        normalized = QStringLiteral("shadows");
    else if (target == QLatin1String("whites"))
        normalized = QStringLiteral("whites");
    else if (target == QLatin1String("blacks"))
        normalized = QStringLiteral("blacks");
    const bool comparison_changed = visible && clear_comparison();
    const bool changed = mask_overlay_visible_ != visible || mask_overlay_target_ != normalized;
    mask_overlay_visible_ = visible;
    mask_overlay_target_ = normalized;
    const bool place_cleared = !visible && mask_place_active_;
    if (place_cleared)
        mask_place_active_ = false;
    const bool assist_cleared = !visible && mask_parametric_assist_active_;
    if (assist_cleared)
        mask_parametric_assist_active_ = false;
    if (!changed && !comparison_changed && !place_cleared && !assist_cleared)
    {
        return;
    }
    if (comparison_changed || place_cleared || assist_cleared)
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

bool StudioPresenter::maskPlaceActive() const noexcept
{
    return mask_place_active_;
}

bool StudioPresenter::maskPlaceGeometryAllowed() const noexcept
{
    return mask_place_geometry_allowed(develop_);
}

void StudioPresenter::setMaskPlaceActive(const bool active)
{
    const bool enabled = active && mask_overlay_visible_ && mask_place_geometry_allowed(develop_);
    if (mask_place_active_ == enabled)
        return;
    if (enabled)
    {
        static_cast<void>(clear_comparison());
        if (crop_tool_active_)
            setCropToolActive(false);
        if (white_balance_pick_active_)
            setWhiteBalancePickActive(false);
        if (mask_parametric_assist_active_)
            mask_parametric_assist_active_ = false;
    }
    mask_place_active_ = enabled;
    emit editChanged();
    if (enabled)
        emit previewChanged();
}

void StudioPresenter::placeMask(const double preview_x, const double preview_y)
{
    if (!mask_place_active_ || !mask_overlay_visible_)
        return;
    const auto target = develop_mask_target_from_name(utf8_from_qstring(mask_overlay_target_));
    if (!target)
    {
        setError(QCoreApplication::translate("DevelopPanel",
                                             "Mask click placement has no overlay target"));
        setMaskPlaceActive(false);
        return;
    }
    auto mapped = map_mask_place_preview(develop_, preview_x, preview_y);
    if (!mapped)
    {
        const auto reason = mapped.error().context.find("reason");
        const auto reason_text = reason == mapped.error().context.end() ?
                                     QStringLiteral("unknown") :
                                     qstring_from_utf8(reason->second);
        setError(QCoreApplication::translate("DevelopPanel", "Mask click placement was rejected") +
                 QStringLiteral(" [") + reason_text + QStringLiteral("]"));
        setMaskPlaceActive(false);
        return;
    }
    const auto state = develop_mask_editor_state(develop_, *target);
    if (!state.attached || !state.editable)
    {
        setError(QCoreApplication::translate(
            "DevelopPanel", "Mask click placement requires an editable attached mask"));
        setMaskPlaceActive(false);
        return;
    }
    const std::string_view kind = state.kind_name == "group" ?
                                      std::string_view(state.child_kind_name) :
                                      std::string_view(state.kind_name);
    const QString prefix = develop_mask_field_prefix(*target);
    QString x_field;
    QString y_field;
    if (kind == "circle" || kind == "ellipse")
    {
        x_field = prefix + QStringLiteral("CenterX");
        y_field = prefix + QStringLiteral("CenterY");
    }
    else if (kind == "linear_gradient")
    {
        x_field = prefix + QStringLiteral("AnchorX");
        y_field = prefix + QStringLiteral("AnchorY");
    }
    else
    {
        setError(QCoreApplication::translate(
            "DevelopPanel", "Mask click placement supports circle, ellipse, or linear gradient"));
        setMaskPlaceActive(false);
        return;
    }
    DevelopParams next = develop_;
    auto applied_x =
        apply_develop_mask_field_strict(next, utf8_from_qstring(x_field), mapped.value().first);
    if (!applied_x)
    {
        const auto reason = applied_x.error().context.find("reason");
        const auto reason_text = reason == applied_x.error().context.end() ?
                                     QStringLiteral("unknown") :
                                     qstring_from_utf8(reason->second);
        setError(QCoreApplication::translate("DevelopPanel", "Mask edit was rejected") +
                 QStringLiteral(" [") + reason_text + QStringLiteral("]"));
        return;
    }
    auto applied_y =
        apply_develop_mask_field_strict(next, utf8_from_qstring(y_field), mapped.value().second);
    if (!applied_y)
    {
        const auto reason = applied_y.error().context.find("reason");
        const auto reason_text = reason == applied_y.error().context.end() ?
                                     QStringLiteral("unknown") :
                                     qstring_from_utf8(reason->second);
        setError(QCoreApplication::translate("DevelopPanel", "Mask edit was rejected") +
                 QStringLiteral(" [") + reason_text + QStringLiteral("]"));
        return;
    }
    mutate_develop(std::move(next), DevelopEdit::Commit, true, utf8_from_qstring(x_field));
}

bool StudioPresenter::maskParametricAssistActive() const noexcept
{
    return mask_parametric_assist_active_;
}

bool StudioPresenter::maskParametricAssistAllowed() const noexcept
{
    return mask_place_geometry_allowed(develop_);
}

void StudioPresenter::setMaskParametricAssistActive(const bool active)
{
    const bool enabled = active && mask_overlay_visible_ && mask_place_geometry_allowed(develop_);
    if (mask_parametric_assist_active_ == enabled)
        return;
    if (enabled)
    {
        static_cast<void>(clear_comparison());
        if (crop_tool_active_)
            setCropToolActive(false);
        if (white_balance_pick_active_)
            setWhiteBalancePickActive(false);
        if (mask_place_active_)
            mask_place_active_ = false;
    }
    mask_parametric_assist_active_ = enabled;
    emit editChanged();
    if (enabled)
        emit previewChanged();
}

void StudioPresenter::assistParametricMask(const double preview_x, const double preview_y)
{
    if (!mask_parametric_assist_active_ || !mask_overlay_visible_)
        return;
    const auto target = develop_mask_target_from_name(utf8_from_qstring(mask_overlay_target_));
    if (!target)
    {
        setError(
            QCoreApplication::translate("DevelopPanel", "Parametric assist has no overlay target"));
        setMaskParametricAssistActive(false);
        return;
    }
    if (!develop_mask_parametric_assist_allowed(*target))
    {
        setError(QCoreApplication::translate(
                     "DevelopPanel", "Parametric assist is not available for this operation") +
                 QStringLiteral(" [mask_parametric_assist_target_unsupported]"));
        setMaskParametricAssistActive(false);
        return;
    }
    if (!mask_place_geometry_allowed(develop_))
    {
        setError(QCoreApplication::translate(
                     "DevelopPanel", "Parametric assist is unavailable with Canvas, Perspective, "
                                     "straighten, rotate, or flip") +
                 QStringLiteral(" [mask_place_geometry_unavailable]"));
        setMaskParametricAssistActive(false);
        return;
    }
    if (!std::isfinite(preview_x) || !std::isfinite(preview_y) || preview_x < 0.0 ||
        preview_x > 1.0 || preview_y < 0.0 || preview_y > 1.0)
    {
        setError(QCoreApplication::translate("DevelopPanel", "Parametric assist was rejected") +
                 QStringLiteral(" [invalid_parametric_assist_preview]"));
        setMaskParametricAssistActive(false);
        return;
    }
    const auto state = develop_mask_editor_state(develop_, *target);
    if (!state.attached || !state.editable || state.kind_name != "parametric")
    {
        setError(QCoreApplication::translate(
            "DevelopPanel", "Parametric assist requires an editable attached parametric mask"));
        setMaskParametricAssistActive(false);
        return;
    }

    QImage preview;
    {
        const QMutexLocker lock(&preview_image_mutex_);
        preview = preview_base_image_;
    }
    if (preview.isNull() || preview.width() <= 0 || preview.height() <= 0)
    {
        setError(QCoreApplication::translate("DevelopPanel", "Parametric assist needs a preview") +
                 QStringLiteral(" [mask_parametric_assist_preview_unavailable]"));
        setMaskParametricAssistActive(false);
        return;
    }
    const QImage rgb = preview.format() == QImage::Format_RGB888 ?
                           preview :
                           preview.convertToFormat(QImage::Format_RGB888);
    if (rgb.isNull() || rgb.format() != QImage::Format_RGB888)
    {
        setError(QCoreApplication::translate("DevelopPanel", "Parametric assist needs a preview") +
                 QStringLiteral(" [mask_parametric_assist_preview_unavailable]"));
        setMaskParametricAssistActive(false);
        return;
    }
    const int width = rgb.width();
    const int height = rgb.height();
    const int column = static_cast<int>(
        std::clamp(std::lround(preview_x * (width - 1)), 0L, static_cast<long>(width - 1)));
    const int row = static_cast<int>(
        std::clamp(std::lround(preview_y * (height - 1)), 0L, static_cast<long>(height - 1)));
    const uchar *line = rgb.constScanLine(row);
    const std::uint8_t red = line[column * 3];
    const std::uint8_t green = line[column * 3 + 1];
    const std::uint8_t blue = line[column * 3 + 2];
    const double sample = normalized_display_mask_channel(red, green, blue, state.channel_index);

    const std::array<std::uint32_t, 256> *bins = nullptr;
    if (scope_histogram_.max_count > 0U)
    {
        switch (state.channel_index)
        {
        case 1:
            bins = &scope_histogram_.red;
            break;
        case 2:
            bins = &scope_histogram_.green;
            break;
        case 3:
            bins = &scope_histogram_.blue;
            break;
        default:
            bins = &scope_histogram_.luma;
            break;
        }
    }
    auto thresholds = parametric_thresholds_from_histogram_assist(sample, bins);
    if (!thresholds)
    {
        const auto reason = thresholds.error().context.find("reason");
        const auto reason_text = reason == thresholds.error().context.end() ?
                                     QStringLiteral("unknown") :
                                     qstring_from_utf8(reason->second);
        setError(QCoreApplication::translate("DevelopPanel", "Parametric assist was rejected") +
                 QStringLiteral(" [") + reason_text + QStringLiteral("]"));
        setMaskParametricAssistActive(false);
        return;
    }

    const QString prefix = develop_mask_field_prefix(*target);
    DevelopParams next = develop_;
    // Apply mid keys before outer keys so each single-field write stays
    // monotonic against the previous Threshold0..3 snapshot.
    const std::array<std::pair<QString, double>, 4> fields{{
        {prefix + QStringLiteral("Threshold2"), thresholds.value()[2]},
        {prefix + QStringLiteral("Threshold1"), thresholds.value()[1]},
        {prefix + QStringLiteral("Threshold0"), thresholds.value()[0]},
        {prefix + QStringLiteral("Threshold3"), thresholds.value()[3]},
    }};
    for (const auto &field : fields)
    {
        auto applied =
            apply_develop_mask_field_strict(next, utf8_from_qstring(field.first), field.second);
        if (!applied)
        {
            const auto reason = applied.error().context.find("reason");
            const auto reason_text = reason == applied.error().context.end() ?
                                         QStringLiteral("unknown") :
                                         qstring_from_utf8(reason->second);
            setError(QCoreApplication::translate("DevelopPanel", "Mask edit was rejected") +
                     QStringLiteral(" [") + reason_text + QStringLiteral("]"));
            return;
        }
    }
    mutate_develop(std::move(next), DevelopEdit::Commit, true, utf8_from_qstring(fields[0].first));
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
