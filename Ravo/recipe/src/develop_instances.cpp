#include "ravo/recipe/develop.h"

#include "develop_internal.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

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

} // namespace ravo
