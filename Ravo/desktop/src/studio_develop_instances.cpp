#include "ravo/desktop/studio_develop_presenter.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <QCoreApplication>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include "ravo/recipe/develop.h"
#include "studio_qt.h"

namespace ravo
{
namespace
{

[[nodiscard]] bool is_exposure_edit_field(const std::string_view field) noexcept
{
    return field == "exposure" || field == "exposureMode" || field == "exposureBlack" ||
           field == "exposureDeflickerPercentile" || field == "exposureDeflickerTarget" ||
           field == "exposureDeflickerTargetEv" || field == "exposureCompensateBias" ||
           field == "exposureCompensateHighlight" || field.starts_with("exposureMask");
}

[[nodiscard]] bool is_color_balance_rgb_edit_field(const std::string_view field) noexcept
{
    // RGB instance fields and mask prefix; exclude legacy Color Balance IOP names if any.
    return (field.starts_with("colorBalance") && !field.starts_with("colorBalanceLegacy")) ||
           field.starts_with("colorBalanceRgbMask");
}

[[nodiscard]] QVariantMap exposure_instance_map(const DevelopExposureInstance &instance,
                                                const bool selected)
{
    return {{QStringLiteral("id"), qstring_from_utf8(instance.instance_id)},
            {QStringLiteral("name"), qstring_from_utf8(instance.name)},
            {QStringLiteral("enabled"), instance.enabled},
            {QStringLiteral("bypass"), instance.bypass},
            {QStringLiteral("selected"), selected},
            {QStringLiteral("hasMask"), instance.mask_id.has_value()}};
}

[[nodiscard]] QVariantMap
color_balance_rgb_instance_map(const DevelopColorBalanceRgbInstance &instance, const bool selected)
{
    return {{QStringLiteral("id"), qstring_from_utf8(instance.instance_id)},
            {QStringLiteral("name"), qstring_from_utf8(instance.name)},
            {QStringLiteral("enabled"), instance.enabled},
            {QStringLiteral("bypass"), instance.bypass},
            {QStringLiteral("selected"), selected},
            {QStringLiteral("hasMask"), instance.mask_id.has_value()}};
}

} // namespace

void StudioDevelopPresenter::retarget_instance_edit_after_field(DevelopParams &params,
                                                                const std::string_view field)
{
    if (is_exposure_edit_field(field) && !params.exposure_instances.empty())
    {
        const std::size_t selected = std::min(state_.selected_exposure_instance_index_,
                                              params.exposure_instances.size() - 1U);
        if (selected > 0U)
        {
            // apply_develop_field mirrored into front(); restore and write selected.
            if (state_.exposure_front_restore_.has_value())
            {
                params.exposure_instances.front() = *state_.exposure_front_restore_;
                state_.exposure_front_restore_.reset();
            }
        }
        mirror_legacy_exposure_into_instance(params, selected);
        load_exposure_instance_into_legacy(params, selected);
        state_.selected_exposure_instance_index_ = selected;
    }
    if (is_color_balance_rgb_edit_field(field) && !params.color_balance_rgb_instances.empty())
    {
        const std::size_t selected = std::min(state_.selected_color_balance_rgb_instance_index_,
                                              params.color_balance_rgb_instances.size() - 1U);
        if (selected > 0U && state_.color_balance_rgb_front_restore_.has_value())
        {
            params.color_balance_rgb_instances.front() = *state_.color_balance_rgb_front_restore_;
            state_.color_balance_rgb_front_restore_.reset();
        }
        mirror_legacy_color_balance_rgb_into_instance(params, selected);
        load_color_balance_rgb_instance_into_legacy(params, selected);
        state_.selected_color_balance_rgb_instance_index_ = selected;
    }
}

void StudioDevelopPresenter::capture_instance_front_for_field(const DevelopParams &params,
                                                              const std::string_view field)
{
    state_.exposure_front_restore_.reset();
    state_.color_balance_rgb_front_restore_.reset();
    if (is_exposure_edit_field(field) && state_.selected_exposure_instance_index_ > 0U &&
        !params.exposure_instances.empty())
    {
        state_.exposure_front_restore_ = params.exposure_instances.front();
    }
    if (is_color_balance_rgb_edit_field(field) &&
        state_.selected_color_balance_rgb_instance_index_ > 0U &&
        !params.color_balance_rgb_instances.empty())
    {
        state_.color_balance_rgb_front_restore_ = params.color_balance_rgb_instances.front();
    }
}

void StudioDevelopPresenter::sync_selected_instance_edit_buffers(DevelopParams &params)
{
    if (!params.exposure_instances.empty())
    {
        state_.selected_exposure_instance_index_ = std::min(
            state_.selected_exposure_instance_index_, params.exposure_instances.size() - 1U);
        load_exposure_instance_into_legacy(params, state_.selected_exposure_instance_index_);
    }
    else
    {
        state_.selected_exposure_instance_index_ = 0U;
    }
    if (!params.color_balance_rgb_instances.empty())
    {
        state_.selected_color_balance_rgb_instance_index_ =
            std::min(state_.selected_color_balance_rgb_instance_index_,
                     params.color_balance_rgb_instances.size() - 1U);
        load_color_balance_rgb_instance_into_legacy(
            params, state_.selected_color_balance_rgb_instance_index_);
    }
    else
    {
        state_.selected_color_balance_rgb_instance_index_ = 0U;
    }
}

QVariantList StudioDevelopPresenter::exposureInstances() const
{
    QVariantList rows;
    if (state_.develop_.exposure_instances.empty())
    {
        // Synthetic singleton row so chrome can still add a second instance.
        rows.push_back(
            QVariantMap{{QStringLiteral("id"), QStringLiteral("exposure-1")},
                        {QStringLiteral("name"), QStringLiteral("Master")},
                        {QStringLiteral("enabled"), true},
                        {QStringLiteral("bypass"), false},
                        {QStringLiteral("selected"), true},
                        {QStringLiteral("hasMask"), state_.develop_.exposure_mask_id.has_value()},
                        {QStringLiteral("synthetic"), true}});
        return rows;
    }
    rows.reserve(static_cast<qsizetype>(state_.develop_.exposure_instances.size()));
    for (std::size_t i = 0; i < state_.develop_.exposure_instances.size(); ++i)
    {
        rows.push_back(exposure_instance_map(state_.develop_.exposure_instances[i],
                                             i == state_.selected_exposure_instance_index_));
    }
    return rows;
}

QVariantList StudioDevelopPresenter::colorBalanceRgbInstances() const
{
    QVariantList rows;
    if (state_.develop_.color_balance_rgb_instances.empty())
    {
        rows.push_back(QVariantMap{
            {QStringLiteral("id"), QStringLiteral("colorbalancergb-1")},
            {QStringLiteral("name"), QStringLiteral("Master")},
            {QStringLiteral("enabled"), true},
            {QStringLiteral("bypass"), false},
            {QStringLiteral("selected"), true},
            {QStringLiteral("hasMask"), state_.develop_.color_balance_rgb_mask_id.has_value()},
            {QStringLiteral("synthetic"), true}});
        return rows;
    }
    rows.reserve(static_cast<qsizetype>(state_.develop_.color_balance_rgb_instances.size()));
    for (std::size_t i = 0; i < state_.develop_.color_balance_rgb_instances.size(); ++i)
    {
        rows.push_back(
            color_balance_rgb_instance_map(state_.develop_.color_balance_rgb_instances[i],
                                           i == state_.selected_color_balance_rgb_instance_index_));
    }
    return rows;
}

QString StudioDevelopPresenter::selectedExposureInstanceId() const
{
    if (state_.develop_.exposure_instances.empty())
    {
        return QStringLiteral("exposure-1");
    }
    const auto index = std::min(state_.selected_exposure_instance_index_,
                                state_.develop_.exposure_instances.size() - 1U);
    return qstring_from_utf8(state_.develop_.exposure_instances[index].instance_id);
}

QString StudioDevelopPresenter::selectedColorBalanceRgbInstanceId() const
{
    if (state_.develop_.color_balance_rgb_instances.empty())
    {
        return QStringLiteral("colorbalancergb-1");
    }
    const auto index = std::min(state_.selected_color_balance_rgb_instance_index_,
                                state_.develop_.color_balance_rgb_instances.size() - 1U);
    return qstring_from_utf8(state_.develop_.color_balance_rgb_instances[index].instance_id);
}

void StudioDevelopPresenter::selectExposureInstance(const QString &instance_id)
{
    DevelopParams next = state_.develop_;
    static_cast<void>(ensure_exposure_instances(next));
    const auto id = utf8_from_qstring(instance_id);
    const auto found = find_exposure_instance_index(next, id);
    if (!found)
    {
        emit errorOccurred(
            QCoreApplication::translate("DevelopPanel", "Exposure instance was not found."));
        return;
    }
    // Persist current edit buffer into the previously selected instance before switching.
    mirror_legacy_exposure_into_instance(next, state_.selected_exposure_instance_index_);
    state_.selected_exposure_instance_index_ = *found;
    load_exposure_instance_into_legacy(next, state_.selected_exposure_instance_index_);
    mutate_develop(std::move(next), DevelopEdit::Overlay, false, std::nullopt);
}

void StudioDevelopPresenter::selectColorBalanceRgbInstance(const QString &instance_id)
{
    DevelopParams next = state_.develop_;
    static_cast<void>(ensure_color_balance_rgb_instances(next));
    const auto id = utf8_from_qstring(instance_id);
    const auto found = find_color_balance_rgb_instance_index(next, id);
    if (!found)
    {
        emit errorOccurred(QCoreApplication::translate(
            "DevelopPanel", "Color Balance RGB instance was not found."));
        return;
    }
    mirror_legacy_color_balance_rgb_into_instance(
        next, state_.selected_color_balance_rgb_instance_index_);
    state_.selected_color_balance_rgb_instance_index_ = *found;
    load_color_balance_rgb_instance_into_legacy(next,
                                                state_.selected_color_balance_rgb_instance_index_);
    mutate_develop(std::move(next), DevelopEdit::Overlay, false, std::nullopt);
}

void StudioDevelopPresenter::addExposureInstance()
{
    DevelopParams next = state_.develop_;
    mirror_legacy_exposure_into_instance(next, state_.selected_exposure_instance_index_);
    auto added = add_exposure_instance(next);
    if (!added)
    {
        emit errorOccurred(qstring_from_utf8(added.error().message));
        return;
    }
    state_.selected_exposure_instance_index_ = next.exposure_instances.size() - 1U;
    load_exposure_instance_into_legacy(next, state_.selected_exposure_instance_index_);
    mutate_develop(std::move(next), DevelopEdit::Commit, true, std::nullopt);
}

void StudioDevelopPresenter::addColorBalanceRgbInstance()
{
    DevelopParams next = state_.develop_;
    mirror_legacy_color_balance_rgb_into_instance(
        next, state_.selected_color_balance_rgb_instance_index_);
    auto added = add_color_balance_rgb_instance(next);
    if (!added)
    {
        emit errorOccurred(qstring_from_utf8(added.error().message));
        return;
    }
    state_.selected_color_balance_rgb_instance_index_ =
        next.color_balance_rgb_instances.size() - 1U;
    load_color_balance_rgb_instance_into_legacy(next,
                                                state_.selected_color_balance_rgb_instance_index_);
    mutate_develop(std::move(next), DevelopEdit::Commit, true, std::nullopt);
}

void StudioDevelopPresenter::duplicateExposureInstance()
{
    DevelopParams next = state_.develop_;
    static_cast<void>(ensure_exposure_instances(next));
    mirror_legacy_exposure_into_instance(next, state_.selected_exposure_instance_index_);
    const auto source_id =
        next.exposure_instances[state_.selected_exposure_instance_index_].instance_id;
    auto duplicated = duplicate_exposure_instance(next, source_id);
    if (!duplicated)
    {
        emit errorOccurred(qstring_from_utf8(duplicated.error().message));
        return;
    }
    state_.selected_exposure_instance_index_ = next.exposure_instances.size() - 1U;
    load_exposure_instance_into_legacy(next, state_.selected_exposure_instance_index_);
    mutate_develop(std::move(next), DevelopEdit::Commit, true, std::nullopt);
}

void StudioDevelopPresenter::duplicateColorBalanceRgbInstance()
{
    DevelopParams next = state_.develop_;
    static_cast<void>(ensure_color_balance_rgb_instances(next));
    mirror_legacy_color_balance_rgb_into_instance(
        next, state_.selected_color_balance_rgb_instance_index_);
    const auto source_id =
        next.color_balance_rgb_instances[state_.selected_color_balance_rgb_instance_index_]
            .instance_id;
    auto duplicated = duplicate_color_balance_rgb_instance(next, source_id);
    if (!duplicated)
    {
        emit errorOccurred(qstring_from_utf8(duplicated.error().message));
        return;
    }
    state_.selected_color_balance_rgb_instance_index_ =
        next.color_balance_rgb_instances.size() - 1U;
    load_color_balance_rgb_instance_into_legacy(next,
                                                state_.selected_color_balance_rgb_instance_index_);
    mutate_develop(std::move(next), DevelopEdit::Commit, true, std::nullopt);
}

void StudioDevelopPresenter::deleteExposureInstance(const QString &instance_id)
{
    DevelopParams next = state_.develop_;
    static_cast<void>(ensure_exposure_instances(next));
    mirror_legacy_exposure_into_instance(next, state_.selected_exposure_instance_index_);
    auto deleted = delete_exposure_instance(next, utf8_from_qstring(instance_id));
    if (!deleted)
    {
        emit errorOccurred(qstring_from_utf8(deleted.error().message));
        return;
    }
    if (state_.selected_exposure_instance_index_ >= next.exposure_instances.size())
    {
        state_.selected_exposure_instance_index_ =
            next.exposure_instances.empty() ? 0U : next.exposure_instances.size() - 1U;
    }
    if (!next.exposure_instances.empty())
    {
        load_exposure_instance_into_legacy(next, state_.selected_exposure_instance_index_);
    }
    mutate_develop(std::move(next), DevelopEdit::Commit, true, std::nullopt);
}

void StudioDevelopPresenter::deleteColorBalanceRgbInstance(const QString &instance_id)
{
    DevelopParams next = state_.develop_;
    static_cast<void>(ensure_color_balance_rgb_instances(next));
    mirror_legacy_color_balance_rgb_into_instance(
        next, state_.selected_color_balance_rgb_instance_index_);
    auto deleted = delete_color_balance_rgb_instance(next, utf8_from_qstring(instance_id));
    if (!deleted)
    {
        emit errorOccurred(qstring_from_utf8(deleted.error().message));
        return;
    }
    if (state_.selected_color_balance_rgb_instance_index_ >=
        next.color_balance_rgb_instances.size())
    {
        state_.selected_color_balance_rgb_instance_index_ =
            next.color_balance_rgb_instances.empty() ? 0U :
                                                       next.color_balance_rgb_instances.size() - 1U;
    }
    if (!next.color_balance_rgb_instances.empty())
    {
        load_color_balance_rgb_instance_into_legacy(
            next, state_.selected_color_balance_rgb_instance_index_);
    }
    mutate_develop(std::move(next), DevelopEdit::Commit, true, std::nullopt);
}

void StudioDevelopPresenter::renameExposureInstance(const QString &instance_id, const QString &name)
{
    DevelopParams next = state_.develop_;
    static_cast<void>(ensure_exposure_instances(next));
    auto renamed =
        rename_exposure_instance(next, utf8_from_qstring(instance_id), utf8_from_qstring(name));
    if (!renamed)
    {
        emit errorOccurred(qstring_from_utf8(renamed.error().message));
        return;
    }
    mutate_develop(std::move(next), DevelopEdit::Commit, false,
                   std::string("exposure.instance.rename"));
}

void StudioDevelopPresenter::renameColorBalanceRgbInstance(const QString &instance_id,
                                                           const QString &name)
{
    DevelopParams next = state_.develop_;
    static_cast<void>(ensure_color_balance_rgb_instances(next));
    auto renamed = rename_color_balance_rgb_instance(next, utf8_from_qstring(instance_id),
                                                     utf8_from_qstring(name));
    if (!renamed)
    {
        emit errorOccurred(qstring_from_utf8(renamed.error().message));
        return;
    }
    mutate_develop(std::move(next), DevelopEdit::Commit, false,
                   std::string("colorbalancergb.instance.rename"));
}

void StudioDevelopPresenter::setExposureInstanceBypass(const QString &instance_id,
                                                       const bool bypass)
{
    DevelopParams next = state_.develop_;
    static_cast<void>(ensure_exposure_instances(next));
    auto updated = set_exposure_instance_bypass(next, utf8_from_qstring(instance_id), bypass);
    if (!updated)
    {
        emit errorOccurred(qstring_from_utf8(updated.error().message));
        return;
    }
    mutate_develop(std::move(next), DevelopEdit::Commit, true, std::nullopt);
}

void StudioDevelopPresenter::setColorBalanceRgbInstanceBypass(const QString &instance_id,
                                                              const bool bypass)
{
    DevelopParams next = state_.develop_;
    static_cast<void>(ensure_color_balance_rgb_instances(next));
    auto updated =
        set_color_balance_rgb_instance_bypass(next, utf8_from_qstring(instance_id), bypass);
    if (!updated)
    {
        emit errorOccurred(qstring_from_utf8(updated.error().message));
        return;
    }
    mutate_develop(std::move(next), DevelopEdit::Commit, true, std::nullopt);
}

void StudioDevelopPresenter::setExposureInstanceEnabled(const QString &instance_id,
                                                        const bool enabled)
{
    DevelopParams next = state_.develop_;
    static_cast<void>(ensure_exposure_instances(next));
    auto updated = set_exposure_instance_enabled(next, utf8_from_qstring(instance_id), enabled);
    if (!updated)
    {
        emit errorOccurred(qstring_from_utf8(updated.error().message));
        return;
    }
    mutate_develop(std::move(next), DevelopEdit::Commit, true, std::nullopt);
}

void StudioDevelopPresenter::setColorBalanceRgbInstanceEnabled(const QString &instance_id,
                                                               const bool enabled)
{
    DevelopParams next = state_.develop_;
    static_cast<void>(ensure_color_balance_rgb_instances(next));
    auto updated =
        set_color_balance_rgb_instance_enabled(next, utf8_from_qstring(instance_id), enabled);
    if (!updated)
    {
        emit errorOccurred(qstring_from_utf8(updated.error().message));
        return;
    }
    mutate_develop(std::move(next), DevelopEdit::Commit, true, std::nullopt);
}

void StudioDevelopPresenter::reorderExposureInstance(const int from, const int to)
{
    if (from < 0 || to < 0)
    {
        return;
    }
    DevelopParams next = state_.develop_;
    static_cast<void>(ensure_exposure_instances(next));
    mirror_legacy_exposure_into_instance(next, state_.selected_exposure_instance_index_);
    const auto selected_id =
        next.exposure_instances[state_.selected_exposure_instance_index_].instance_id;
    auto reordered = reorder_exposure_instance(next, static_cast<std::size_t>(from),
                                               static_cast<std::size_t>(to));
    if (!reordered)
    {
        emit errorOccurred(qstring_from_utf8(reordered.error().message));
        return;
    }
    if (const auto found = find_exposure_instance_index(next, selected_id))
    {
        state_.selected_exposure_instance_index_ = *found;
    }
    load_exposure_instance_into_legacy(next, state_.selected_exposure_instance_index_);
    mutate_develop(std::move(next), DevelopEdit::Commit, true, std::nullopt);
}

void StudioDevelopPresenter::reorderColorBalanceRgbInstance(const int from, const int to)
{
    if (from < 0 || to < 0)
    {
        return;
    }
    DevelopParams next = state_.develop_;
    static_cast<void>(ensure_color_balance_rgb_instances(next));
    mirror_legacy_color_balance_rgb_into_instance(
        next, state_.selected_color_balance_rgb_instance_index_);
    const auto selected_id =
        next.color_balance_rgb_instances[state_.selected_color_balance_rgb_instance_index_]
            .instance_id;
    auto reordered = reorder_color_balance_rgb_instance(next, static_cast<std::size_t>(from),
                                                        static_cast<std::size_t>(to));
    if (!reordered)
    {
        emit errorOccurred(qstring_from_utf8(reordered.error().message));
        return;
    }
    if (const auto found = find_color_balance_rgb_instance_index(next, selected_id))
    {
        state_.selected_color_balance_rgb_instance_index_ = *found;
    }
    load_color_balance_rgb_instance_into_legacy(next,
                                                state_.selected_color_balance_rgb_instance_index_);
    mutate_develop(std::move(next), DevelopEdit::Commit, true, std::nullopt);
}

} // namespace ravo
