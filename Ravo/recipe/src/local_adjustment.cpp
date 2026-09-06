#include "ravo/recipe/local_adjustment.h"

#include <algorithm>
#include <array>
#include <set>

#include "ravo/recipe/develop_mask.h"
#include "ravo/recipe/operation.h"

namespace ravo
{
namespace
{
constexpr std::string_view kMaskPrefix = "ravo.studio.mask.local.";

TaskError local_error(const std::string_view reason)
{
    return make_error(ErrorCode::kValidation, "Local adjustment was rejected",
                      {{"reason", std::string(reason)}});
}

auto find_local(DevelopParams &params, const std::string_view id)
{
    return std::find_if(params.local_adjustments.begin(), params.local_adjustments.end(),
                        [id](const auto &local) { return local.operation.instance_id == id; });
}

bool final_stage(const OperationInstance &operation)
{
    return operation.id == "ravo.geometry.rotate" || operation.id == "ravo.geometry.flip" ||
           operation.id == "ravo.geometry.perspective" || operation.id == "ravo.geometry.crop" ||
           operation.id == "ravo.geometry.straighten" ||
           operation.id.starts_with("ravo.display.") ||
           operation.id == "ravo.core.rapidraw-tone-controls" ||
           operation.id == "ravo.color.output";
}
} // namespace

bool local_adjustment_operation_allowed(const std::string_view id) noexcept
{
    constexpr std::array allowed{
        "ravo.core.exposure",       "ravo.core.contrast",         "ravo.core.highlights",
        "ravo.core.shadows",        "ravo.core.whites",           "ravo.core.blacks",
        "ravo.core.gamma",          "ravo.core.tonecurve",        "ravo.core.toneequal",
        "ravo.color.temperature",   "ravo.color.rgblevels",       "ravo.color.rgbcurve",
        "ravo.color.colorbalance",  "ravo.color.colorbalancergb", "ravo.color.colorcorrection",
        "ravo.color.colorcontrast", "ravo.color.colorharmonizer", "ravo.color.colorchecker",
        "ravo.color.colorequal",    "ravo.color.colorzones",      "ravo.color.vibrance",
        "ravo.color.saturation",    "ravo.color.velvia",          "ravo.color.monochrome",
        "ravo.color.splittoning",   "ravo.color.lut3d",           "ravo.effect.graduatednd",
        "ravo.effect.vignette",     "ravo.effect.grain",          "ravo.effect.bloom",
        "ravo.effect.soften",       "ravo.effect.dehaze",         "ravo.detail.texture",
        "ravo.detail.sharpen",      "ravo.detail.clarity",        "ravo.detail.denoiseprofile"};
    return std::find(allowed.begin(), allowed.end(), id) != allowed.end();
}

Result<void> validate_local_adjustment(const OperationInstance &operation,
                                       const OperationRegistry &registry)
{
    if (operation.id != kLocalAdjustmentOperationId || operation.schema_version != 1 ||
        !operation.mask_id || operation.mask_id->empty() ||
        operation.children.size() > kLocalAdjustmentMaxOperations)
        return local_error("invalid_local_adjustment");
    std::set<std::string> kinds;
    for (const auto &child : operation.children)
    {
        if (!local_adjustment_operation_allowed(child.id) || child.mask_id ||
            !child.children.empty() || !kinds.insert(child.id).second)
            return local_error("unsupported_local_adjustment_operation");
        if (child.id == "ravo.color.temperature")
        {
            auto white = temperature_from_parameters(child.parameters);
            if (!white)
                return white.error();
            if (!white.value().coefficients)
                return local_error("local_white_balance_requires_rgb_coefficients");
        }
    }
    Recipe nested;
    nested.asset = {"local-validation", "local-validation", std::nullopt};
    nested.operations = operation.children;
    return validate_recipe(nested, registry);
}

Result<DevelopParams> local_adjustment_develop(const DevelopParams &params,
                                               const std::string_view id)
{
    const auto found =
        std::find_if(params.local_adjustments.begin(), params.local_adjustments.end(),
                     [id](const auto &local) { return local.operation.instance_id == id; });
    if (found == params.local_adjustments.end())
        return local_error("local_adjustment_not_found");
    Recipe recipe;
    recipe.operations = found->operation.children;
    recipe.masks = params.masks;
    auto local = develop_from_recipe(recipe);
    if (!local)
        return local.error();
    local.value().local_mask_id = found->operation.mask_id;
    auto complete =
        recipe_from_develop({"local-ownership", "local-ownership", std::nullopt}, params);
    if (!complete)
        return complete.error();
    for (const auto &operation : complete.value().operations)
        if (operation.mask_id && operation.instance_id != id)
            local.value().mask_read_only_roots.push_back(*operation.mask_id);
    for (const auto &region : params.retouch.regions)
        local.value().mask_read_only_roots.push_back(region.mask_id);
    if (!local.value().temperature.coefficients)
    {
        local.value().temperature.mode = kTemperatureModeManual;
        local.value().temperature.coefficients =
            std::array<double, kTemperatureChannelCount>{1, 1, 1, 1};
    }
    return local;
}

Result<void> set_local_adjustment_develop(DevelopParams &params, const std::string_view id,
                                          const DevelopParams &local)
{
    auto found = find_local(params, id);
    if (found == params.local_adjustments.end())
        return local_error("local_adjustment_not_found");
    if (!local.local_mask_id || !local.local_adjustments.empty())
        return local_error("invalid_local_adjustment_scope");
    auto built = recipe_from_develop({"local", "local", std::nullopt}, local);
    if (!built)
        return built.error();
    auto previous = local_adjustment_develop(params, id);
    if (!previous)
        return previous.error();
    auto normalized_previous =
        recipe_from_develop({"local", "local", std::nullopt}, previous.value());
    if (!normalized_previous)
        return normalized_previous.error();
    OperationInstance group = found->operation;
    group.children.clear();
    group.mask_id = local.local_mask_id;
    for (auto &operation : built.value().operations)
    {
        if (operation.id == "ravo.color.input" || operation.id == "ravo.color.output")
            continue;
        if (!local_adjustment_operation_allowed(operation.id) || operation.mask_id)
            return local_error("unsupported_local_adjustment_operation");
        const auto old =
            std::find_if(found->operation.children.begin(), found->operation.children.end(),
                         [&](const auto &child) { return child.id == operation.id; });
        if (old != found->operation.children.end())
        {
            operation.instance_id = old->instance_id;
            operation.name = old->name;
            operation.bypass = old->bypass;
            const auto normalized =
                std::find_if(normalized_previous.value().operations.begin(),
                             normalized_previous.value().operations.end(),
                             [&](const auto &child) { return child.id == operation.id; });
            if (normalized != normalized_previous.value().operations.end() &&
                normalized->enabled == operation.enabled)
                operation.enabled = old->enabled;
        }
        group.children.push_back(std::move(operation));
    }
    auto registry = make_phase1_registry();
    if (!registry)
        return registry.error();
    auto valid = validate_local_adjustment(group, registry.value());
    if (!valid)
        return valid.error();
    auto graph = validate_mask_graph(local.masks);
    if (!graph)
        return graph.error();
    if (std::none_of(local.masks.begin(), local.masks.end(),
                     [&](const auto &mask) { return mask.id == *group.mask_id; }))
        return local_error("local_adjustment_mask_not_found");
    DevelopParams staged = params;
    find_local(staged, id)->operation = std::move(group);
    staged.masks = local.masks;
    auto complete =
        recipe_from_develop({"local-validation", "local-validation", std::nullopt}, staged);
    if (!complete)
        return complete.error();
    auto valid_complete = validate_recipe(complete.value(), registry.value());
    if (!valid_complete)
        return valid_complete.error();
    params = std::move(staged);
    return {};
}

Result<std::string> create_local_adjustment(DevelopParams &params, const std::string_view id,
                                            const std::int64_t mask_kind)
{
    if (id.empty() || id.size() > 128 ||
        params.local_adjustments.size() >= kLocalAdjustmentMaxCount ||
        find_local(params, id) != params.local_adjustments.end())
        return local_error("invalid_local_adjustment_id_or_limit");
    DevelopParams local;
    local.masks = params.masks;
    auto authored =
        apply_develop_mask_field_strict(local, "localMaskKind", static_cast<double>(mask_kind));
    if (!authored)
        return authored.error();
    if (!local.local_mask_id)
        return local_error("local_adjustment_requires_mask");
    LocalAdjustment adjustment;
    adjustment.operation.id = kLocalAdjustmentOperationId;
    adjustment.operation.instance_id = id;
    adjustment.operation.name = std::string(id);
    adjustment.operation.mask_id = local.local_mask_id;
    params.local_adjustments.push_back(std::move(adjustment));
    params.masks = std::move(local.masks);
    return std::string(id);
}

Result<void> duplicate_local_adjustment(DevelopParams &params, const std::string_view source_id,
                                        const std::string_view new_id)
{
    auto source = find_local(params, source_id);
    if (source == params.local_adjustments.end())
        return local_error("local_adjustment_not_found");
    if (new_id.empty() || new_id.size() > 128 ||
        find_local(params, new_id) != params.local_adjustments.end() ||
        params.local_adjustments.size() >= kLocalAdjustmentMaxCount)
        return local_error("invalid_local_adjustment_id_or_limit");
    DevelopParams next = params;
    LocalAdjustment duplicate = *source;
    auto cloned = clone_develop_mask_subgraph(next, duplicate.operation.mask_id, kMaskPrefix);
    if (!cloned)
        return cloned.error();
    duplicate.operation.mask_id = std::move(cloned).value();
    duplicate.operation.instance_id = new_id;
    duplicate.operation.name = duplicate.operation.name.value_or(std::string(source_id)) + " copy";
    next.local_adjustments.push_back(std::move(duplicate));
    auto graph = validate_mask_graph(next.masks);
    if (!graph)
        return graph.error();
    params = std::move(next);
    return {};
}

Result<void> delete_local_adjustment(DevelopParams &params, const std::string_view id)
{
    auto found = find_local(params, id);
    if (found == params.local_adjustments.end())
        return local_error("local_adjustment_not_found");
    const auto root = found->operation.mask_id;
    params.local_adjustments.erase(found);
    if (root)
        collect_unreferenced_develop_mask(params, *root, kMaskPrefix);
    return {};
}

Result<void> insert_local_adjustments(Recipe &recipe, const DevelopParams &params)
{
    if (params.local_adjustments.size() > kLocalAdjustmentMaxCount)
        return local_error("local_adjustment_limit");
    for (const auto &local : params.local_adjustments)
    {
        auto insertion = recipe.operations.end();
        for (const auto &following : local.following_instances)
        {
            insertion = std::find_if(recipe.operations.begin(), recipe.operations.end(),
                                     [&](const auto &operation)
                                     { return operation.instance_id == following; });
            if (insertion != recipe.operations.end())
                break;
        }
        if (local.following_instances.empty())
            insertion =
                std::find_if(recipe.operations.begin(), recipe.operations.end(), final_stage);
        recipe.operations.insert(insertion, local.operation);
    }
    return {};
}

Result<void> promote_legacy_local_adjustments(DevelopParams &params)
{
    auto recipe = recipe_from_develop({"local-migration", "local-migration", std::nullopt}, params);
    if (!recipe)
        return recipe.error();
    DevelopParams staged = params;
    bool changed = false;
    std::vector<std::string> retired_roots;
    for (auto &operation : recipe.value().operations)
    {
        if (!operation.mask_id || operation.id == kLocalAdjustmentOperationId ||
            !local_adjustment_operation_allowed(operation.id))
            continue;
        auto root = clone_develop_mask_subgraph(staged, operation.mask_id, kMaskPrefix);
        if (!root)
            return root.error();
        OperationInstance group;
        group.id = kLocalAdjustmentOperationId;
        group.instance_id = "local-" + operation.instance_id;
        std::size_t suffix = 1;
        while (std::any_of(recipe.value().operations.begin(), recipe.value().operations.end(),
                           [&](const auto &existing)
                           { return existing.instance_id == group.instance_id; }))
            group.instance_id = "local-" + operation.instance_id + "-" + std::to_string(suffix++);
        group.name = operation.name.value_or(operation.instance_id);
        group.mask_id = std::move(root).value();
        group.enabled = operation.enabled;
        group.bypass = operation.bypass;
        retired_roots.push_back(*operation.mask_id);
        operation.mask_id.reset();
        operation.enabled = true;
        operation.bypass = false;
        group.children.push_back(std::move(operation));
        operation = std::move(group);
        changed = true;
    }
    if (!changed)
        return {};
    recipe.value().masks = std::move(staged.masks);
    auto upgraded = develop_from_recipe(recipe.value());
    if (!upgraded)
        return upgraded.error();
    for (const auto &root : retired_roots)
        if (root.starts_with("ravo.studio.mask."))
            collect_unreferenced_develop_mask(upgraded.value(), root,
                                              root.substr(0, root.find_last_of('.') + 1));
    params = std::move(upgraded).value();
    return {};
}

} // namespace ravo
