#include "ravo/recipe/develop.h"

#include "develop_internal.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>

namespace ravo
{
namespace
{

[[nodiscard]] std::string make_exposure_instance_id(const DevelopParams &params)
{
    for (std::size_t n = 1;; ++n)
    {
        const std::string id = "exposure-" + std::to_string(n);
        bool used = false;
        for (const auto &instance : params.exposure_instances)
        {
            if (instance.instance_id == id)
            {
                used = true;
                break;
            }
        }
        if (!used)
        {
            return id;
        }
    }
}

[[nodiscard]] std::string make_color_balance_rgb_instance_id(const DevelopParams &params)
{
    for (std::size_t n = 1;; ++n)
    {
        const std::string id = "colorbalancergb-" + std::to_string(n);
        bool used = false;
        for (const auto &instance : params.color_balance_rgb_instances)
        {
            if (instance.instance_id == id)
            {
                used = true;
                break;
            }
        }
        if (!used)
        {
            return id;
        }
    }
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

[[nodiscard]] std::string make_duplicate_mask_id(const DevelopParams &params)
{
    for (std::size_t n = 1;; ++n)
    {
        const std::string id = "mask-dup-" + std::to_string(n);
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
            return id;
        }
    }
}

[[nodiscard]] Result<std::string> clone_mask_subgraph(DevelopParams &params,
                                                      const std::string_view source_id,
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
    const Mask *source = nullptr;
    for (const auto &mask : params.masks)
    {
        if (mask.id == source_id)
        {
            source = &mask;
            break;
        }
    }
    if (source == nullptr)
    {
        return make_error(ErrorCode::kNotFound, "Instance mask was not found",
                          {{"reason", "duplicate_instance_mask_missing"}, {"mask_id", source_key}});
    }
    Mask cloned = *source;
    cloned.id = make_duplicate_mask_id(params);
    if (auto *group = std::get_if<MaskGroup>(&cloned.payload))
    {
        for (auto &child : group->children)
        {
            auto child_clone = clone_mask_subgraph(params, child.mask_id, visiting);
            if (!child_clone)
            {
                return child_clone.error();
            }
            child.mask_id = std::move(child_clone).value();
        }
    }
    const std::string new_id = cloned.id;
    params.masks.push_back(std::move(cloned));
    visiting.erase(source_key);
    return new_id;
}

[[nodiscard]] Result<std::optional<std::string>>
duplicate_instance_mask(DevelopParams &params, const std::optional<std::string> &mask_id)
{
    if (!mask_id.has_value() || mask_id->empty())
    {
        return std::optional<std::string>{};
    }
    std::unordered_set<std::string> visiting;
    auto cloned = clone_mask_subgraph(params, *mask_id, visiting);
    if (!cloned)
    {
        return cloned.error();
    }
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
    seed.name = "Master";
    seed.params = params.color_balance_rgb;
    seed.mask_id = params.color_balance_rgb_mask_id;
    params.color_balance_rgb_instances.push_back(std::move(seed));
    return params.color_balance_rgb_instances.size();
}

Result<std::string> add_exposure_instance(DevelopParams &params)
{
    ensure_exposure_instances(params);
    DevelopExposureInstance added;
    added.instance_id = make_exposure_instance_id(params);
    added.name = "Instance " + std::to_string(params.exposure_instances.size() + 1U);
    params.exposure_instances.push_back(added);
    return added.instance_id;
}

Result<std::string> add_color_balance_rgb_instance(DevelopParams &params)
{
    ensure_color_balance_rgb_instances(params);
    DevelopColorBalanceRgbInstance added;
    added.instance_id = make_color_balance_rgb_instance_id(params);
    added.name = "Instance " + std::to_string(params.color_balance_rgb_instances.size() + 1U);
    params.color_balance_rgb_instances.push_back(added);
    return added.instance_id;
}

Result<void> delete_exposure_instance(DevelopParams &params, const std::string_view instance_id)
{
    if (params.exposure_instances.size() <= 1U)
    {
        // Collapse back to legacy singleton buffer.
        if (!params.exposure_instances.empty())
        {
            load_exposure_instance_into_legacy(params, 0);
        }
        params.exposure_instances.clear();
        return {};
    }
    return delete_instance(params.exposure_instances, instance_id);
}

Result<void> delete_color_balance_rgb_instance(DevelopParams &params,
                                               const std::string_view instance_id)
{
    if (params.color_balance_rgb_instances.size() <= 1U)
    {
        if (!params.color_balance_rgb_instances.empty())
        {
            load_color_balance_rgb_instance_into_legacy(params, 0);
        }
        params.color_balance_rgb_instances.clear();
        return {};
    }
    return delete_instance(params.color_balance_rgb_instances, instance_id);
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
    ensure_exposure_instances(params);
    const auto found = find_exposure_instance_index(params, instance_id);
    if (!found)
    {
        return make_error(ErrorCode::kNotFound, "Develop instance was not found",
                          {{"instance_id", std::string(instance_id)}});
    }
    DevelopExposureInstance copy = params.exposure_instances[*found];
    auto mask = duplicate_instance_mask(params, copy.mask_id);
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
    ensure_color_balance_rgb_instances(params);
    const auto found = find_color_balance_rgb_instance_index(params, instance_id);
    if (!found)
    {
        return make_error(ErrorCode::kNotFound, "Develop instance was not found",
                          {{"instance_id", std::string(instance_id)}});
    }
    DevelopColorBalanceRgbInstance copy = params.color_balance_rgb_instances[*found];
    auto mask = duplicate_instance_mask(params, copy.mask_id);
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
