#include "ravo/recipe/develop.h"

#include "develop_internal.h"

#include <algorithm>
#include <iterator>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>
#include <variant>

namespace ravo
{
namespace
{

// Never reuse deleted instance ids: mint max(existing numeric suffix)+1 so a
// stale command/history entry cannot address a newly created instance (COR-01).
[[nodiscard]] std::size_t max_numeric_instance_suffix(const std::string_view prefix,
                                                      const auto &instances) noexcept
{
    std::size_t max_n = 0U;
    for (const auto &instance : instances)
    {
        const std::string_view id = instance.instance_id;
        if (!id.starts_with(prefix) || id.size() == prefix.size())
            continue;
        const auto suffix = id.substr(prefix.size());
        if (suffix.empty() || suffix.front() == '0')
            continue;
        bool digits = true;
        std::size_t value = 0U;
        for (const char character : suffix)
        {
            if (character < '0' || character > '9')
            {
                digits = false;
                break;
            }
            value = value * 10U + static_cast<std::size_t>(character - '0');
        }
        if (digits)
            max_n = std::max(max_n, value);
    }
    return max_n;
}

[[nodiscard]] std::uint64_t numeric_suffix_after_prefix(const std::string_view id,
                                                        const std::string_view prefix) noexcept
{
    if (!id.starts_with(prefix) || id.size() == prefix.size())
        return 0;
    const auto suffix = id.substr(prefix.size());
    if (suffix.empty() || suffix.front() == '0')
        return 0;
    std::uint64_t value = 0;
    for (const char character : suffix)
    {
        if (character < '0' || character > '9')
            return 0;
        value = value * 10U + static_cast<std::uint64_t>(character - '0');
    }
    return value;
}

void remember_exposure_instance_id(DevelopParams &params,
                                   const std::string_view instance_id) noexcept
{
    params.exposure_instance_id_high_water =
        std::max(params.exposure_instance_id_high_water,
                 numeric_suffix_after_prefix(instance_id, "exposure-"));
}

void remember_color_balance_rgb_instance_id(DevelopParams &params,
                                            const std::string_view instance_id) noexcept
{
    params.color_balance_rgb_instance_id_high_water =
        std::max(params.color_balance_rgb_instance_id_high_water,
                 numeric_suffix_after_prefix(instance_id, "colorbalancergb-"));
}

[[nodiscard]] std::string make_exposure_instance_id(DevelopParams &params)
{
    const auto from_live = max_numeric_instance_suffix("exposure-", params.exposure_instances);
    const auto next =
        std::max(from_live, static_cast<std::size_t>(params.exposure_instance_id_high_water)) + 1U;
    params.exposure_instance_id_high_water = static_cast<std::uint64_t>(next);
    return "exposure-" + std::to_string(next);
}

[[nodiscard]] std::string make_color_balance_rgb_instance_id(DevelopParams &params)
{
    const auto from_live =
        max_numeric_instance_suffix("colorbalancergb-", params.color_balance_rgb_instances);
    const auto next =
        std::max(from_live,
                 static_cast<std::size_t>(params.color_balance_rgb_instance_id_high_water)) +
        1U;
    params.color_balance_rgb_instance_id_high_water = static_cast<std::uint64_t>(next);
    return "colorbalancergb-" + std::to_string(next);
}

template <typename T>
[[nodiscard]] Result<void> rename_instance(std::vector<T> &instances,
                                           const std::string_view instance_id,
                                           const std::string_view name)
{
    for (auto &instance : instances)
    {
        if (instance.instance_id == instance_id)
        {
            instance.name = std::string(name);
            return {};
        }
    }
    return make_error(ErrorCode::kNotFound, "Develop instance was not found",
                      {{"instance_id", std::string(instance_id)}});
}

template <typename T>
[[nodiscard]] Result<void> set_instance_bypass(std::vector<T> &instances,
                                               const std::string_view instance_id,
                                               const bool bypass)
{
    for (auto &instance : instances)
    {
        if (instance.instance_id == instance_id)
        {
            instance.bypass = bypass;
            return {};
        }
    }
    return make_error(ErrorCode::kNotFound, "Develop instance was not found",
                      {{"instance_id", std::string(instance_id)}});
}

template <typename T>
[[nodiscard]] Result<void> set_instance_enabled(std::vector<T> &instances,
                                                const std::string_view instance_id,
                                                const bool enabled)
{
    for (auto &instance : instances)
    {
        if (instance.instance_id == instance_id)
        {
            instance.enabled = enabled;
            return {};
        }
    }
    return make_error(ErrorCode::kNotFound, "Develop instance was not found",
                      {{"instance_id", std::string(instance_id)}});
}

template <typename T>
[[nodiscard]] Result<void> delete_instance(std::vector<T> &instances,
                                           const std::string_view instance_id)
{
    const auto found = std::find_if(instances.begin(), instances.end(), [&](const T &instance)
                                    { return instance.instance_id == instance_id; });
    if (found == instances.end())
    {
        return make_error(ErrorCode::kNotFound, "Develop instance was not found",
                          {{"instance_id", std::string(instance_id)}});
    }
    instances.erase(found);
    return {};
}

template <typename T>
[[nodiscard]] Result<void> reorder_instance(std::vector<T> &instances, const std::size_t from,
                                            const std::size_t to)
{
    if (instances.empty() || from >= instances.size() || to >= instances.size())
    {
        return make_error(ErrorCode::kValidation, "Develop instance reorder is out of range",
                          {{"from", std::to_string(from)}, {"to", std::to_string(to)}});
    }
    if (from == to)
    {
        return {};
    }
    T item = std::move(instances[from]);
    instances.erase(instances.begin() + static_cast<std::ptrdiff_t>(from));
    instances.insert(instances.begin() + static_cast<std::ptrdiff_t>(to), std::move(item));
    return {};
}

// Mint Studio-owned IDs so duplicated instance masks remain editable through
// the existing Exposure / Color Balance RGB authoring pipeline (ADR-0044/0145).
// Non-studio source masks stay fail-closed on the source instance; the clone
// becomes an independent authored leaf.
[[nodiscard]] std::string make_duplicate_mask_id(const DevelopParams &params,
                                                 const std::vector<Mask> &staged,
                                                 const std::string_view prefix)
{
    for (std::size_t n = 1;; ++n)
    {
        const std::string id = std::string(prefix) + std::to_string(n);
        bool used = false;
        for (const auto &mask : params.masks)
        {
            if (mask.id == id)
            {
                used = true;
                break;
            }
        }
        if (!used)
        {
            for (const auto &mask : staged)
            {
                if (mask.id == id)
                {
                    used = true;
                    break;
                }
            }
        }
        if (!used)
        {
            return id;
        }
    }
}

// Stage clones into `staged` only. `params.masks` stays unchanged until the
// caller publishes the full staged set (all-or-nothing).
[[nodiscard]] Result<std::string>
clone_mask_subgraph_staged(const DevelopParams &params, std::vector<Mask> &staged,
                           const std::string_view source_id, const std::string_view id_prefix,
                           std::unordered_set<std::string> &visiting)
{
    if (source_id.empty())
    {
        return make_error(ErrorCode::kValidation, "Mask id is empty",
                          {{"reason", "duplicate_instance_empty_mask"}});
    }
    const std::string source_key{source_id};
    if (!visiting.insert(source_key).second)
    {
        return make_error(ErrorCode::kConflict, "Mask graph cycle while cloning instance mask",
                          {{"reason", "duplicate_instance_mask_cycle"}, {"mask_id", source_key}});
    }
    const auto find_mask = [&](const std::string_view id) -> const Mask *
    {
        for (const auto &mask : params.masks)
        {
            if (mask.id == id)
                return &mask;
        }
        for (const auto &mask : staged)
        {
            if (mask.id == id)
                return &mask;
        }
        return nullptr;
    };
    const Mask *source = find_mask(source_id);
    if (source == nullptr)
    {
        return make_error(ErrorCode::kNotFound, "Instance mask was not found",
                          {{"reason", "duplicate_instance_mask_missing"}, {"mask_id", source_key}});
    }
    Mask cloned = *source;
    cloned.id = make_duplicate_mask_id(params, staged, id_prefix);
    if (auto *group = std::get_if<MaskGroup>(&cloned.payload))
    {
        for (auto &child : group->children)
        {
            auto child_clone =
                clone_mask_subgraph_staged(params, staged, child.mask_id, id_prefix, visiting);
            if (!child_clone)
            {
                return child_clone.error();
            }
            child.mask_id = std::move(child_clone).value();
        }
    }
    const std::string new_id = cloned.id;
    staged.push_back(std::move(cloned));
    visiting.erase(source_key);
    return new_id;
}

[[nodiscard]] bool is_studio_instance_mask_id(const std::string_view id,
                                              const std::string_view prefix) noexcept
{
    if (!id.starts_with(prefix) || id.size() == prefix.size())
        return false;
    const auto suffix = id.substr(prefix.size());
    if (suffix.empty() || suffix.front() == '0')
        return false;
    return std::all_of(suffix.begin(), suffix.end(),
                       [](const char character) { return character >= '0' && character <= '9'; });
}

[[nodiscard]] std::size_t mask_reference_count(const DevelopParams &params,
                                               const std::string_view id) noexcept
{
    std::size_t count = 0U;
    if (params.exposure_mask_id && *params.exposure_mask_id == id)
        ++count;
    if (params.color_balance_rgb_mask_id && *params.color_balance_rgb_mask_id == id)
        ++count;
    for (const auto &instance : params.exposure_instances)
    {
        if (instance.mask_id && *instance.mask_id == id)
            ++count;
    }
    for (const auto &instance : params.color_balance_rgb_instances)
    {
        if (instance.mask_id && *instance.mask_id == id)
            ++count;
    }
    // Other Develop mask attachments (curves, lights, etc.).
    if (params.graduated_mask_id && *params.graduated_mask_id == id)
        ++count;
    if (params.color_harmonizer_mask_id && *params.color_harmonizer_mask_id == id)
        ++count;
    if (params.rgb_curve_mask_id && *params.rgb_curve_mask_id == id)
        ++count;
    if (params.tone_curve_mask_id && *params.tone_curve_mask_id == id)
        ++count;
    if (params.highlights_mask_id && *params.highlights_mask_id == id)
        ++count;
    if (params.shadows_mask_id && *params.shadows_mask_id == id)
        ++count;
    if (params.whites_mask_id && *params.whites_mask_id == id)
        ++count;
    if (params.blacks_mask_id && *params.blacks_mask_id == id)
        ++count;
    if (params.velvia_mask_id && *params.velvia_mask_id == id)
        ++count;
    if (params.color_zones_mask_id && *params.color_zones_mask_id == id)
        ++count;
    if (params.monochrome_mask_id && *params.monochrome_mask_id == id)
        ++count;
    if (params.split_toning_mask_id && *params.split_toning_mask_id == id)
        ++count;
    for (const auto &mask : params.masks)
    {
        const auto *group = std::get_if<MaskGroup>(&mask.payload);
        if (group == nullptr)
            continue;
        count += static_cast<std::size_t>(
            std::count_if(group->children.begin(), group->children.end(),
                          [id](const MaskGroupChild &child) { return child.mask_id == id; }));
    }
    return count;
}

void erase_mask_entry(DevelopParams &params, const std::string_view id)
{
    params.masks.erase(std::remove_if(params.masks.begin(), params.masks.end(),
                                      [id](const Mask &mask) { return mask.id == id; }),
                       params.masks.end());
}

// Remove only exclusively owned Studio mask subgraphs. Shared/external masks are
// retained; callers must already have detached the deleted instance reference.
void gc_exclusively_owned_mask_subgraph(DevelopParams &params, const std::string_view root_id,
                                        const std::string_view studio_prefix)
{
    if (root_id.empty() || !is_studio_instance_mask_id(root_id, studio_prefix))
        return;
    if (mask_reference_count(params, root_id) != 0U)
        return;
    const Mask *found = nullptr;
    for (const auto &mask : params.masks)
    {
        if (mask.id == root_id)
        {
            found = &mask;
            break;
        }
    }
    if (found == nullptr)
        return;
    std::vector<std::string> child_ids;
    if (const auto *group = std::get_if<MaskGroup>(&found->payload); group != nullptr)
    {
        child_ids.reserve(group->children.size());
        for (const auto &child : group->children)
            child_ids.push_back(child.mask_id);
    }
    erase_mask_entry(params, root_id);
    for (const auto &child_id : child_ids)
        gc_exclusively_owned_mask_subgraph(params, child_id, studio_prefix);
}

void clear_legacy_mask_if_matches(std::optional<std::string> &legacy, const std::string_view id)
{
    if (legacy && *legacy == id)
        legacy.reset();
}

[[nodiscard]] Result<std::optional<std::string>>
duplicate_instance_mask(DevelopParams &params, const std::optional<std::string> &mask_id,
                        const std::string_view id_prefix)
{
    if (!mask_id.has_value() || mask_id->empty())
    {
        return std::optional<std::string>{};
    }
    std::unordered_set<std::string> visiting;
    std::vector<Mask> staged;
    auto cloned = clone_mask_subgraph_staged(params, staged, *mask_id, id_prefix, visiting);
    if (!cloned)
    {
        // Staged clones are discarded — params.masks is byte-for-byte unchanged.
        return cloned.error();
    }
    params.masks.insert(params.masks.end(), std::make_move_iterator(staged.begin()),
                        std::make_move_iterator(staged.end()));
    return std::optional<std::string>{std::move(cloned).value()};
}

} // namespace

void mirror_legacy_exposure_into_instance(DevelopParams &params, const std::size_t index) noexcept
{
    if (index >= params.exposure_instances.size())
    {
        return;
    }
    auto &instance = params.exposure_instances[index];
    instance.mode = params.exposure_mode;
    instance.black = params.exposure_black;
    instance.exposure_ev = params.exposure_ev;
    instance.deflicker_percentile = params.exposure_deflicker_percentile;
    instance.deflicker_target_ev = params.exposure_deflicker_target_ev;
    instance.compensate_exposure_bias = params.exposure_compensate_exposure_bias;
    instance.compensate_highlight_preservation = params.exposure_compensate_highlight_preservation;
    instance.mask_id = params.exposure_mask_id;
}

void load_exposure_instance_into_legacy(DevelopParams &params, const std::size_t index) noexcept
{
    if (index >= params.exposure_instances.size())
    {
        return;
    }
    const auto &instance = params.exposure_instances[index];
    params.exposure_mode = instance.mode;
    params.exposure_black = instance.black;
    params.exposure_ev = instance.exposure_ev;
    params.exposure_deflicker_percentile = instance.deflicker_percentile;
    params.exposure_deflicker_target_ev = instance.deflicker_target_ev;
    params.exposure_compensate_exposure_bias = instance.compensate_exposure_bias;
    params.exposure_compensate_highlight_preservation = instance.compensate_highlight_preservation;
    params.exposure_mask_id = instance.mask_id;
}

void mirror_legacy_color_balance_rgb_into_instance(DevelopParams &params,
                                                   const std::size_t index) noexcept
{
    if (index >= params.color_balance_rgb_instances.size())
    {
        return;
    }
    auto &instance = params.color_balance_rgb_instances[index];
    instance.params = params.color_balance_rgb;
    instance.mask_id = params.color_balance_rgb_mask_id;
}

void load_color_balance_rgb_instance_into_legacy(DevelopParams &params,
                                                 const std::size_t index) noexcept
{
    if (index >= params.color_balance_rgb_instances.size())
    {
        return;
    }
    const auto &instance = params.color_balance_rgb_instances[index];
    params.color_balance_rgb = instance.params;
    params.color_balance_rgb_mask_id = instance.mask_id;
}

std::size_t ensure_exposure_instances(DevelopParams &params)
{
    if (!params.exposure_instances.empty())
    {
        return params.exposure_instances.size();
    }
    DevelopExposureInstance seed;
    seed.instance_id = "exposure-1";
    remember_exposure_instance_id(params, seed.instance_id);
    seed.name = "Master";
    seed.mode = params.exposure_mode;
    seed.black = params.exposure_black;
    seed.exposure_ev = params.exposure_ev;
    seed.deflicker_percentile = params.exposure_deflicker_percentile;
    seed.deflicker_target_ev = params.exposure_deflicker_target_ev;
    seed.compensate_exposure_bias = params.exposure_compensate_exposure_bias;
    seed.compensate_highlight_preservation = params.exposure_compensate_highlight_preservation;
    seed.mask_id = params.exposure_mask_id;
    params.exposure_instances.push_back(std::move(seed));
    return params.exposure_instances.size();
}

std::size_t ensure_color_balance_rgb_instances(DevelopParams &params)
{
    if (!params.color_balance_rgb_instances.empty())
    {
        return params.color_balance_rgb_instances.size();
    }
    DevelopColorBalanceRgbInstance seed;
    seed.instance_id = "colorbalancergb-1";
    remember_color_balance_rgb_instance_id(params, seed.instance_id);
    seed.name = "Master";
    seed.params = params.color_balance_rgb;
    seed.mask_id = params.color_balance_rgb_mask_id;
    params.color_balance_rgb_instances.push_back(std::move(seed));
    return params.color_balance_rgb_instances.size();
}

Result<std::string> add_exposure_instance(DevelopParams &params)
{
    static_cast<void>(ensure_exposure_instances(params));
    DevelopExposureInstance added;
    added.instance_id = make_exposure_instance_id(params);
    added.name = "Instance " + std::to_string(params.exposure_instances.size() + 1U);
    params.exposure_instances.push_back(added);
    return added.instance_id;
}

Result<std::string> add_color_balance_rgb_instance(DevelopParams &params)
{
    static_cast<void>(ensure_color_balance_rgb_instances(params));
    DevelopColorBalanceRgbInstance added;
    added.instance_id = make_color_balance_rgb_instance_id(params);
    added.name = "Instance " + std::to_string(params.color_balance_rgb_instances.size() + 1U);
    params.color_balance_rgb_instances.push_back(added);
    return added.instance_id;
}

Result<void> delete_exposure_instance(DevelopParams &params, const std::string_view instance_id)
{
    if (params.exposure_instances.empty())
    {
        return make_error(ErrorCode::kNotFound, "Develop instance was not found",
                          {{"instance_id", std::string(instance_id)},
                           {"reason", "delete_exposure_instance_empty"}});
    }
    const auto found = find_exposure_instance_index(params, instance_id);
    if (!found)
    {
        return make_error(ErrorCode::kNotFound, "Develop instance was not found",
                          {{"instance_id", std::string(instance_id)},
                           {"reason", "delete_exposure_instance_id_mismatch"}});
    }
    const std::optional<std::string> owned_mask = params.exposure_instances[*found].mask_id;
    remember_exposure_instance_id(params, instance_id);
    constexpr std::string_view kExposureStudioPrefix = "ravo.studio.mask.exposure.";
    if (params.exposure_instances.size() == 1U)
    {
        // Sole-instance collapse requires a matching id (above). Disabled or
        // bypassed sole instances must not re-activate through legacy fields:
        // collapse to documented identity exposure parameters.
        const auto &sole = params.exposure_instances.front();
        const bool reset_identity = !sole.enabled || sole.bypass;
        if (reset_identity)
        {
            params.exposure_mode = std::string(kExposureModeManual);
            params.exposure_black = 0.0;
            params.exposure_ev = 0.0;
            params.exposure_deflicker_percentile = 0.0;
            params.exposure_deflicker_target_ev = 0.0;
            params.exposure_compensate_exposure_bias = false;
            params.exposure_compensate_highlight_preservation = false;
            params.exposure_mask_id.reset();
        }
        else
        {
            // Preserve params+mask on legacy singleton fields.
            load_exposure_instance_into_legacy(params, 0);
        }
        params.exposure_instances.clear();
        // Only GC when identity reset dropped the attachment; enabled collapse
        // keeps the mask on legacy exposure_mask_id.
        if (reset_identity && owned_mask)
            gc_exclusively_owned_mask_subgraph(params, *owned_mask, kExposureStudioPrefix);
        return {};
    }
    auto removed = delete_instance(params.exposure_instances, instance_id);
    if (!removed)
        return removed.error();
    if (owned_mask)
    {
        clear_legacy_mask_if_matches(params.exposure_mask_id, *owned_mask);
        gc_exclusively_owned_mask_subgraph(params, *owned_mask, kExposureStudioPrefix);
    }
    return {};
}

Result<void> delete_color_balance_rgb_instance(DevelopParams &params,
                                               const std::string_view instance_id)
{
    if (params.color_balance_rgb_instances.empty())
    {
        return make_error(ErrorCode::kNotFound, "Develop instance was not found",
                          {{"instance_id", std::string(instance_id)},
                           {"reason", "delete_color_balance_rgb_instance_empty"}});
    }
    const auto found = find_color_balance_rgb_instance_index(params, instance_id);
    if (!found)
    {
        return make_error(ErrorCode::kNotFound, "Develop instance was not found",
                          {{"instance_id", std::string(instance_id)},
                           {"reason", "delete_color_balance_rgb_instance_id_mismatch"}});
    }
    const std::optional<std::string> owned_mask =
        params.color_balance_rgb_instances[*found].mask_id;
    remember_color_balance_rgb_instance_id(params, instance_id);
    constexpr std::string_view kColorStudioPrefix = "ravo.studio.mask.color_balance_rgb.";
    if (params.color_balance_rgb_instances.size() == 1U)
    {
        const auto &sole = params.color_balance_rgb_instances.front();
        const bool reset_identity = !sole.enabled || sole.bypass;
        if (reset_identity)
        {
            params.color_balance_rgb = ColorBalanceRgbParams{};
            params.color_balance_rgb_mask_id.reset();
        }
        else
        {
            load_color_balance_rgb_instance_into_legacy(params, 0);
        }
        params.color_balance_rgb_instances.clear();
        if (reset_identity && owned_mask)
            gc_exclusively_owned_mask_subgraph(params, *owned_mask, kColorStudioPrefix);
        return {};
    }
    auto removed = delete_instance(params.color_balance_rgb_instances, instance_id);
    if (!removed)
        return removed.error();
    if (owned_mask)
    {
        clear_legacy_mask_if_matches(params.color_balance_rgb_mask_id, *owned_mask);
        gc_exclusively_owned_mask_subgraph(params, *owned_mask, kColorStudioPrefix);
    }
    return {};
}

Result<void> rename_exposure_instance(DevelopParams &params, const std::string_view instance_id,
                                      const std::string_view name)
{
    return rename_instance(params.exposure_instances, instance_id, name);
}

Result<void> rename_color_balance_rgb_instance(DevelopParams &params,
                                               const std::string_view instance_id,
                                               const std::string_view name)
{
    return rename_instance(params.color_balance_rgb_instances, instance_id, name);
}

Result<void> set_exposure_instance_bypass(DevelopParams &params, const std::string_view instance_id,
                                          const bool bypass)
{
    return set_instance_bypass(params.exposure_instances, instance_id, bypass);
}

Result<void> set_color_balance_rgb_instance_bypass(DevelopParams &params,
                                                   const std::string_view instance_id,
                                                   const bool bypass)
{
    return set_instance_bypass(params.color_balance_rgb_instances, instance_id, bypass);
}

Result<void> set_exposure_instance_enabled(DevelopParams &params,
                                           const std::string_view instance_id, const bool enabled)
{
    return set_instance_enabled(params.exposure_instances, instance_id, enabled);
}

Result<void> set_color_balance_rgb_instance_enabled(DevelopParams &params,
                                                    const std::string_view instance_id,
                                                    const bool enabled)
{
    return set_instance_enabled(params.color_balance_rgb_instances, instance_id, enabled);
}

Result<void> reorder_exposure_instance(DevelopParams &params, const std::size_t from,
                                       const std::size_t to)
{
    return reorder_instance(params.exposure_instances, from, to);
}

Result<void> reorder_color_balance_rgb_instance(DevelopParams &params, const std::size_t from,
                                                const std::size_t to)
{
    return reorder_instance(params.color_balance_rgb_instances, from, to);
}

std::optional<std::size_t> find_exposure_instance_index(const DevelopParams &params,
                                                        const std::string_view instance_id)
{
    for (std::size_t i = 0; i < params.exposure_instances.size(); ++i)
    {
        if (params.exposure_instances[i].instance_id == instance_id)
        {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> find_color_balance_rgb_instance_index(const DevelopParams &params,
                                                                 const std::string_view instance_id)
{
    for (std::size_t i = 0; i < params.color_balance_rgb_instances.size(); ++i)
    {
        if (params.color_balance_rgb_instances[i].instance_id == instance_id)
        {
            return i;
        }
    }
    return std::nullopt;
}

Result<std::string> duplicate_exposure_instance(DevelopParams &params,
                                                const std::string_view instance_id)
{
    static_cast<void>(ensure_exposure_instances(params));
    const auto found = find_exposure_instance_index(params, instance_id);
    if (!found)
    {
        return make_error(ErrorCode::kNotFound, "Develop instance was not found",
                          {{"instance_id", std::string(instance_id)}});
    }
    DevelopExposureInstance copy = params.exposure_instances[*found];
    auto mask = duplicate_instance_mask(params, copy.mask_id, "ravo.studio.mask.exposure.");
    if (!mask)
    {
        return mask.error();
    }
    copy.instance_id = make_exposure_instance_id(params);
    if (copy.name.empty())
    {
        copy.name = "Instance " + std::to_string(params.exposure_instances.size() + 1U);
    }
    else
    {
        copy.name += " copy";
    }
    copy.mask_id = std::move(mask).value();
    params.exposure_instances.push_back(copy);
    return copy.instance_id;
}

Result<std::string> duplicate_color_balance_rgb_instance(DevelopParams &params,
                                                         const std::string_view instance_id)
{
    static_cast<void>(ensure_color_balance_rgb_instances(params));
    const auto found = find_color_balance_rgb_instance_index(params, instance_id);
    if (!found)
    {
        return make_error(ErrorCode::kNotFound, "Develop instance was not found",
                          {{"instance_id", std::string(instance_id)}});
    }
    DevelopColorBalanceRgbInstance copy = params.color_balance_rgb_instances[*found];
    auto mask =
        duplicate_instance_mask(params, copy.mask_id, "ravo.studio.mask.color_balance_rgb.");
    if (!mask)
    {
        return mask.error();
    }
    copy.instance_id = make_color_balance_rgb_instance_id(params);
    if (copy.name.empty())
    {
        copy.name = "Instance " + std::to_string(params.color_balance_rgb_instances.size() + 1U);
    }
    else
    {
        copy.name += " copy";
    }
    copy.mask_id = std::move(mask).value();
    params.color_balance_rgb_instances.push_back(copy);
    return copy.instance_id;
}

} // namespace ravo
