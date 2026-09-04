#include "ravo/desktop/studio_presenter.h"

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

void StudioPresenter::retarget_instance_edit_after_field(DevelopParams &params,
                                                         const std::string_view field)
{
    if (is_exposure_edit_field(field) && !params.exposure_instances.empty())
    {
        const std::size_t selected =
            std::min(selected_exposure_instance_index_, params.exposure_instances.size() - 1U);
        if (selected > 0U)
        {
            // apply_develop_field mirrored into front(); restore and write selected.
            if (exposure_front_restore_.has_value())
            {
                params.exposure_instances.front() = *exposure_front_restore_;
                exposure_front_restore_.reset();
            }
        }
        mirror_legacy_exposure_into_instance(params, selected);
        load_exposure_instance_into_legacy(params, selected);
        selected_exposure_instance_index_ = selected;
    }
    if (is_color_balance_rgb_edit_field(field) && !params.color_balance_rgb_instances.empty())
    {
        const std::size_t selected = std::min(selected_color_balance_rgb_instance_index_,
                                              params.color_balance_rgb_instances.size() - 1U);
        if (selected > 0U && color_balance_rgb_front_restore_.has_value())
        {
            params.color_balance_rgb_instances.front() = *color_balance_rgb_front_restore_;
            color_balance_rgb_front_restore_.reset();
        }
        mirror_legacy_color_balance_rgb_into_instance(params, selected);
        load_color_balance_rgb_instance_into_legacy(params, selected);
        selected_color_balance_rgb_instance_index_ = selected;
    }
}

void StudioPresenter::capture_instance_front_for_field(const DevelopParams &params,
                                                       const std::string_view field)
{
    exposure_front_restore_.reset();
    color_balance_rgb_front_restore_.reset();
    if (is_exposure_edit_field(field) && selected_exposure_instance_index_ > 0U &&
        !params.exposure_instances.empty())
    {
        exposure_front_restore_ = params.exposure_instances.front();
    }
    if (is_color_balance_rgb_edit_field(field) && selected_color_balance_rgb_instance_index_ > 0U &&
        !params.color_balance_rgb_instances.empty())
    {
        color_balance_rgb_front_restore_ = params.color_balance_rgb_instances.front();
    }
}

void StudioPresenter::sync_selected_instance_edit_buffers(DevelopParams &params)
{
    if (!params.exposure_instances.empty())
    {
        selected_exposure_instance_index_ =
            std::min(selected_exposure_instance_index_, params.exposure_instances.size() - 1U);
        load_exposure_instance_into_legacy(params, selected_exposure_instance_index_);
    }
    else
    {
        selected_exposure_instance_index_ = 0U;
    }
    if (!params.color_balance_rgb_instances.empty())
    {
        selected_color_balance_rgb_instance_index_ =
            std::min(selected_color_balance_rgb_instance_index_,
                     params.color_balance_rgb_instances.size() - 1U);
        load_color_balance_rgb_instance_into_legacy(params,
                                                    selected_color_balance_rgb_instance_index_);
    }
    else
    {
        selected_color_balance_rgb_instance_index_ = 0U;
    }
}

QVariantList StudioPresenter::exposureInstances() const
{
    QVariantList rows;
    if (develop_.exposure_instances.empty())
    {
        // Synthetic singleton row so chrome can still add a second instance.
        rows.push_back(
            QVariantMap{{QStringLiteral("id"), QStringLiteral("exposure-1")},
                        {QStringLiteral("name"), QStringLiteral("Master")},
                        {QStringLiteral("enabled"), true},
                        {QStringLiteral("bypass"), false},
                        {QStringLiteral("selected"), true},
                        {QStringLiteral("hasMask"), develop_.exposure_mask_id.has_value()},
                        {QStringLiteral("synthetic"), true}});
        return rows;
    }
    rows.reserve(static_cast<qsizetype>(develop_.exposure_instances.size()));
    for (std::size_t i = 0; i < develop_.exposure_instances.size(); ++i)
    {
        rows.push_back(exposure_instance_map(develop_.exposure_instances[i],
                                             i == selected_exposure_instance_index_));
    }
    return rows;
}

QVariantList StudioPresenter::colorBalanceRgbInstances() const
{
    QVariantList rows;
    if (develop_.color_balance_rgb_instances.empty())
    {
        rows.push_back(
            QVariantMap{{QStringLiteral("id"), QStringLiteral("colorbalancergb-1")},
                        {QStringLiteral("name"), QStringLiteral("Master")},
                        {QStringLiteral("enabled"), true},
                        {QStringLiteral("bypass"), false},
                        {QStringLiteral("selected"), true},
                        {QStringLiteral("hasMask"), develop_.color_balance_rgb_mask_id.has_value()},
                        {QStringLiteral("synthetic"), true}});
        return rows;
    }
    rows.reserve(static_cast<qsizetype>(develop_.color_balance_rgb_instances.size()));
    for (std::size_t i = 0; i < develop_.color_balance_rgb_instances.size(); ++i)
    {
        rows.push_back(
            color_balance_rgb_instance_map(develop_.color_balance_rgb_instances[i],
                                           i == selected_color_balance_rgb_instance_index_));
    }
    return rows;
}

QString StudioPresenter::selectedExposureInstanceId() const
{
    if (develop_.exposure_instances.empty())
    {
        return QStringLiteral("exposure-1");
    }
    const auto index =
        std::min(selected_exposure_instance_index_, develop_.exposure_instances.size() - 1U);
    return qstring_from_utf8(develop_.exposure_instances[index].instance_id);
}

QString StudioPresenter::selectedColorBalanceRgbInstanceId() const
{
    if (develop_.color_balance_rgb_instances.empty())
    {
        return QStringLiteral("colorbalancergb-1");
    }
    const auto index = std::min(selected_color_balance_rgb_instance_index_,
                                develop_.color_balance_rgb_instances.size() - 1U);
    return qstring_from_utf8(develop_.color_balance_rgb_instances[index].instance_id);
}

void StudioPresenter::selectExposureInstance(const QString &instance_id)
{
    DevelopParams next = develop_;
    static_cast<void>(ensure_exposure_instances(next));
    const auto id = utf8_from_qstring(instance_id);
    const auto found = find_exposure_instance_index(next, id);
    if (!found)
    {
        setError(QCoreApplication::translate("DevelopPanel", "Exposure instance was not found."));
        return;
    }
    // Persist current edit buffer into the previously selected instance before switching.
    mirror_legacy_exposure_into_instance(next, selected_exposure_instance_index_);
    selected_exposure_instance_index_ = *found;
    load_exposure_instance_into_legacy(next, selected_exposure_instance_index_);
    mutate_develop(std::move(next), DevelopEdit::Overlay, false, std::nullopt);
}

void StudioPresenter::selectColorBalanceRgbInstance(const QString &instance_id)
{
    DevelopParams next = develop_;
    static_cast<void>(ensure_color_balance_rgb_instances(next));
    const auto id = utf8_from_qstring(instance_id);
    const auto found = find_color_balance_rgb_instance_index(next, id);
    if (!found)
    {
        setError(QCoreApplication::translate("DevelopPanel",
                                             "Color Balance RGB instance was not found."));
        return;
    }
    mirror_legacy_color_balance_rgb_into_instance(next, selected_color_balance_rgb_instance_index_);
    selected_color_balance_rgb_instance_index_ = *found;
    load_color_balance_rgb_instance_into_legacy(next, selected_color_balance_rgb_instance_index_);
    mutate_develop(std::move(next), DevelopEdit::Overlay, false, std::nullopt);
}

void StudioPresenter::addExposureInstance()
{
    DevelopParams next = develop_;
    mirror_legacy_exposure_into_instance(next, selected_exposure_instance_index_);
    auto added = add_exposure_instance(next);
    if (!added)
    {
        setError(qstring_from_utf8(added.error().message));
        return;
    }
    selected_exposure_instance_index_ = next.exposure_instances.size() - 1U;
    load_exposure_instance_into_legacy(next, selected_exposure_instance_index_);
    mutate_develop(std::move(next), DevelopEdit::Commit, true,
                   std::string("exposure.instance.add"));
}

void StudioPresenter::addColorBalanceRgbInstance()
{
    DevelopParams next = develop_;
    mirror_legacy_color_balance_rgb_into_instance(next, selected_color_balance_rgb_instance_index_);
    auto added = add_color_balance_rgb_instance(next);
    if (!added)
    {
        setError(qstring_from_utf8(added.error().message));
        return;
    }
    selected_color_balance_rgb_instance_index_ = next.color_balance_rgb_instances.size() - 1U;
    load_color_balance_rgb_instance_into_legacy(next, selected_color_balance_rgb_instance_index_);
    mutate_develop(std::move(next), DevelopEdit::Commit, true,
                   std::string("colorbalancergb.instance.add"));
}

void StudioPresenter::duplicateExposureInstance()
{
    DevelopParams next = develop_;
    static_cast<void>(ensure_exposure_instances(next));
    mirror_legacy_exposure_into_instance(next, selected_exposure_instance_index_);
    const auto source_id = next.exposure_instances[selected_exposure_instance_index_].instance_id;
    auto duplicated = duplicate_exposure_instance(next, source_id);
    if (!duplicated)
    {
        setError(qstring_from_utf8(duplicated.error().message));
        return;
    }
    selected_exposure_instance_index_ = next.exposure_instances.size() - 1U;
    load_exposure_instance_into_legacy(next, selected_exposure_instance_index_);
    mutate_develop(std::move(next), DevelopEdit::Commit, true,
                   std::string("exposure.instance.duplicate"));
}

void StudioPresenter::duplicateColorBalanceRgbInstance()
{
    DevelopParams next = develop_;
    static_cast<void>(ensure_color_balance_rgb_instances(next));
    mirror_legacy_color_balance_rgb_into_instance(next, selected_color_balance_rgb_instance_index_);
    const auto source_id =
        next.color_balance_rgb_instances[selected_color_balance_rgb_instance_index_].instance_id;
    auto duplicated = duplicate_color_balance_rgb_instance(next, source_id);
    if (!duplicated)
    {
        setError(qstring_from_utf8(duplicated.error().message));
        return;
    }
    selected_color_balance_rgb_instance_index_ = next.color_balance_rgb_instances.size() - 1U;
    load_color_balance_rgb_instance_into_legacy(next, selected_color_balance_rgb_instance_index_);
    mutate_develop(std::move(next), DevelopEdit::Commit, true,
                   std::string("colorbalancergb.instance.duplicate"));
}

void StudioPresenter::deleteExposureInstance(const QString &instance_id)
{
    DevelopParams next = develop_;
    static_cast<void>(ensure_exposure_instances(next));
    mirror_legacy_exposure_into_instance(next, selected_exposure_instance_index_);
    auto deleted = delete_exposure_instance(next, utf8_from_qstring(instance_id));
    if (!deleted)
    {
        setError(qstring_from_utf8(deleted.error().message));
        return;
    }
    if (selected_exposure_instance_index_ >= next.exposure_instances.size())
    {
        selected_exposure_instance_index_ =
            next.exposure_instances.empty() ? 0U : next.exposure_instances.size() - 1U;
    }
    if (!next.exposure_instances.empty())
    {
        load_exposure_instance_into_legacy(next, selected_exposure_instance_index_);
    }
    mutate_develop(std::move(next), DevelopEdit::Commit, true,
                   std::string("exposure.instance.delete"));
}

void StudioPresenter::deleteColorBalanceRgbInstance(const QString &instance_id)
{
    DevelopParams next = develop_;
    static_cast<void>(ensure_color_balance_rgb_instances(next));
    mirror_legacy_color_balance_rgb_into_instance(next, selected_color_balance_rgb_instance_index_);
    auto deleted = delete_color_balance_rgb_instance(next, utf8_from_qstring(instance_id));
    if (!deleted)
    {
        setError(qstring_from_utf8(deleted.error().message));
        return;
    }
    if (selected_color_balance_rgb_instance_index_ >= next.color_balance_rgb_instances.size())
    {
        selected_color_balance_rgb_instance_index_ =
            next.color_balance_rgb_instances.empty() ? 0U :
                                                       next.color_balance_rgb_instances.size() - 1U;
    }
    if (!next.color_balance_rgb_instances.empty())
    {
        load_color_balance_rgb_instance_into_legacy(next,
                                                    selected_color_balance_rgb_instance_index_);
    }
    mutate_develop(std::move(next), DevelopEdit::Commit, true,
                   std::string("colorbalancergb.instance.delete"));
}

void StudioPresenter::renameExposureInstance(const QString &instance_id, const QString &name)
{
    DevelopParams next = develop_;
    static_cast<void>(ensure_exposure_instances(next));
    auto renamed =
        rename_exposure_instance(next, utf8_from_qstring(instance_id), utf8_from_qstring(name));
    if (!renamed)
    {
        setError(qstring_from_utf8(renamed.error().message));
        return;
    }
    mutate_develop(std::move(next), DevelopEdit::Commit, false,
                   std::string("exposure.instance.rename"));
}

void StudioPresenter::renameColorBalanceRgbInstance(const QString &instance_id, const QString &name)
{
    DevelopParams next = develop_;
    static_cast<void>(ensure_color_balance_rgb_instances(next));
    auto renamed = rename_color_balance_rgb_instance(next, utf8_from_qstring(instance_id),
                                                     utf8_from_qstring(name));
    if (!renamed)
    {
        setError(qstring_from_utf8(renamed.error().message));
        return;
    }
    mutate_develop(std::move(next), DevelopEdit::Commit, false,
                   std::string("colorbalancergb.instance.rename"));
}

void StudioPresenter::setExposureInstanceBypass(const QString &instance_id, const bool bypass)
{
    DevelopParams next = develop_;
    static_cast<void>(ensure_exposure_instances(next));
    auto updated = set_exposure_instance_bypass(next, utf8_from_qstring(instance_id), bypass);
    if (!updated)
    {
        setError(qstring_from_utf8(updated.error().message));
        return;
    }
    mutate_develop(std::move(next), DevelopEdit::Commit, true,
                   std::string("exposure.instance.bypass"));
}

void StudioPresenter::setColorBalanceRgbInstanceBypass(const QString &instance_id,
                                                       const bool bypass)
{
    DevelopParams next = develop_;
    static_cast<void>(ensure_color_balance_rgb_instances(next));
    auto updated =
        set_color_balance_rgb_instance_bypass(next, utf8_from_qstring(instance_id), bypass);
    if (!updated)
    {
        setError(qstring_from_utf8(updated.error().message));
        return;
    }
    mutate_develop(std::move(next), DevelopEdit::Commit, true,
                   std::string("colorbalancergb.instance.bypass"));
}

void StudioPresenter::setExposureInstanceEnabled(const QString &instance_id, const bool enabled)
{
    DevelopParams next = develop_;
    static_cast<void>(ensure_exposure_instances(next));
    auto updated = set_exposure_instance_enabled(next, utf8_from_qstring(instance_id), enabled);
    if (!updated)
    {
        setError(qstring_from_utf8(updated.error().message));
        return;
    }
    mutate_develop(std::move(next), DevelopEdit::Commit, true,
                   std::string("exposure.instance.enabled"));
}

void StudioPresenter::setColorBalanceRgbInstanceEnabled(const QString &instance_id,
                                                        const bool enabled)
{
    DevelopParams next = develop_;
    static_cast<void>(ensure_color_balance_rgb_instances(next));
    auto updated =
        set_color_balance_rgb_instance_enabled(next, utf8_from_qstring(instance_id), enabled);
    if (!updated)
    {
        setError(qstring_from_utf8(updated.error().message));
        return;
    }
    mutate_develop(std::move(next), DevelopEdit::Commit, true,
                   std::string("colorbalancergb.instance.enabled"));
}

void StudioPresenter::reorderExposureInstance(const int from, const int to)
{
    if (from < 0 || to < 0)
    {
        return;
    }
    DevelopParams next = develop_;
    static_cast<void>(ensure_exposure_instances(next));
    mirror_legacy_exposure_into_instance(next, selected_exposure_instance_index_);
    const auto selected_id = next.exposure_instances[selected_exposure_instance_index_].instance_id;
    auto reordered = reorder_exposure_instance(next, static_cast<std::size_t>(from),
                                               static_cast<std::size_t>(to));
    if (!reordered)
    {
        setError(qstring_from_utf8(reordered.error().message));
        return;
    }
    if (const auto found = find_exposure_instance_index(next, selected_id))
    {
        selected_exposure_instance_index_ = *found;
    }
    load_exposure_instance_into_legacy(next, selected_exposure_instance_index_);
    mutate_develop(std::move(next), DevelopEdit::Commit, true,
                   std::string("exposure.instance.reorder"));
}

void StudioPresenter::reorderColorBalanceRgbInstance(const int from, const int to)
{
    if (from < 0 || to < 0)
    {
        return;
    }
    DevelopParams next = develop_;
    static_cast<void>(ensure_color_balance_rgb_instances(next));
    mirror_legacy_color_balance_rgb_into_instance(next, selected_color_balance_rgb_instance_index_);
    const auto selected_id =
        next.color_balance_rgb_instances[selected_color_balance_rgb_instance_index_].instance_id;
    auto reordered = reorder_color_balance_rgb_instance(next, static_cast<std::size_t>(from),
                                                        static_cast<std::size_t>(to));
    if (!reordered)
    {
        setError(qstring_from_utf8(reordered.error().message));
        return;
    }
    if (const auto found = find_color_balance_rgb_instance_index(next, selected_id))
    {
        selected_color_balance_rgb_instance_index_ = *found;
    }
    load_color_balance_rgb_instance_into_legacy(next, selected_color_balance_rgb_instance_index_);
    mutate_develop(std::move(next), DevelopEdit::Commit, true,
                   std::string("colorbalancergb.instance.reorder"));
}

} // namespace ravo
