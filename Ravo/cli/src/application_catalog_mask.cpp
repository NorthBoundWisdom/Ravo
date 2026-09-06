#include "application_internal.h"

#include <algorithm>
#include <map>
#include <set>
#include <QUuid>
#include "ravo/recipe/local_adjustment.h"
#include "ravo/recipe/develop_mask.h"
#include "ravo/services/catalog_service.h"

namespace ravo::cli_internal
{
Result<JsonValue> run_catalog_mask_command(const EngineFacade &engine,
                                           const std::span<const std::string_view> positional)
{
    std::map<std::string, std::string, std::less<>> flags;
    std::vector<std::pair<std::string, double>> fields;
    for (std::size_t index = 2; index < positional.size(); ++index)
    {
        const auto option = positional[index];
        if (++index >= positional.size())
            return make_error(ErrorCode::kInvalidArgument, "Mask option requires a value");
        const auto value = positional[index];
        if (option == "--set")
        {
            const auto split = value.find('=');
            if (split == std::string_view::npos || split == 0)
                return make_error(ErrorCode::kInvalidArgument, "--set requires name=value");
            auto number = parse_double_flag(value.substr(split + 1), option);
            if (!number)
                return number.error();
            const auto name = std::string(value.substr(0, split));
            if (std::any_of(fields.begin(), fields.end(),
                            [&](const auto &field) { return field.first == name; }))
                return make_error(ErrorCode::kInvalidArgument, "Mask field specified twice");
            fields.emplace_back(name, number.value());
            continue;
        }
        if (option != "--catalog" && option != "--asset-id" && option != "--action" &&
            option != "--id" && option != "--new-id" && option != "--kind" && option != "--name" &&
            option != "--enabled" && option != "--expect-revision")
            return make_error(ErrorCode::kInvalidArgument, "Unknown mask option",
                              {{"option", std::string(option)}});
        if (!flags.emplace(std::string(option), std::string(value)).second)
            return make_error(ErrorCode::kInvalidArgument, "Mask option specified twice");
    }
    if (flags["--catalog"].empty() || flags["--asset-id"].empty() || flags["--action"].empty())
        return make_error(ErrorCode::kInvalidArgument,
                          "catalog mask requires --catalog, --asset-id and --action");
    const auto &action = flags["--action"];
    std::set<std::string, std::less<>> allowed{"--catalog", "--asset-id", "--action"};
    if (action != "list")
        allowed.insert("--expect-revision");
    if (action == "create")
        allowed.insert({"--id", "--kind", "--name"});
    else if (action == "set" || action == "delete" || action == "invert")
        allowed.insert("--id");
    else if (action == "rename")
        allowed.insert({"--id", "--name"});
    else if (action == "enable")
        allowed.insert({"--id", "--enabled"});
    else if (action == "duplicate")
        allowed.insert({"--id", "--new-id"});
    else if (action != "list")
        return make_error(ErrorCode::kInvalidArgument, "Unknown mask action");
    if (!fields.empty() && action != "set")
        return make_error(ErrorCode::kInvalidArgument, "--set requires the set mask action");
    for (const auto &[option, value] : flags)
    {
        (void)value;
        if (!allowed.contains(option))
            return make_error(ErrorCode::kInvalidArgument,
                              "Mask option is not valid for this action", {{"option", option}});
    }
    auto session = open_catalog_session(engine, flags["--catalog"], false);
    if (!session)
        return session.error();
    auto &service = *session.value();
    auto revision = service.snapshot();
    if (!revision)
        return revision.error();
    auto loaded = service.load_recipe(flags["--asset-id"]);
    if (!loaded)
        return loaded.error();
    auto params = develop_from_recipe(loaded.value());
    if (!params)
        return params.error();
    auto promoted = promote_legacy_local_adjustments(params.value());
    if (!promoted)
        return promoted.error();
    if (action != "list")
    {
        auto expected = parse_uint64_flag(flags["--expect-revision"], "--expect-revision");
        if (!expected)
            return expected.error();
        if (expected.value() != static_cast<std::uint64_t>(revision.value().revision))
            return make_error(ErrorCode::kConflict, "Catalog revision is stale",
                              {{"reason", "stale_catalog_revision"}});
        const auto &id = flags["--id"];
        if (action == "create")
        {
            auto kind = parse_int_flag(flags["--kind"], "--kind");
            if (!kind)
                return kind.error();
            const auto fresh =
                id.empty() ?
                    "local-" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString() :
                    id;
            auto created = create_local_adjustment(params.value(), fresh, kind.value());
            if (!created)
                return created.error();
            if (const auto name = flags.find("--name"); name != flags.end())
            {
                if (name->second.empty() || name->second.size() > 200)
                    return make_error(ErrorCode::kInvalidArgument,
                                      "Mask name must contain 1 through 200 bytes");
                params.value().local_adjustments.back().operation.name = name->second;
            }
        }
        else if (action == "delete")
        {
            auto removed = delete_local_adjustment(params.value(), id);
            if (!removed)
                return removed.error();
        }
        else if (action == "duplicate")
        {
            const auto fresh =
                flags["--new-id"].empty() ?
                    "local-" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString() :
                    flags["--new-id"];
            auto duplicated = duplicate_local_adjustment(params.value(), id, fresh);
            if (!duplicated)
                return duplicated.error();
        }
        else if (action == "set" || action == "invert")
        {
            auto local = local_adjustment_develop(params.value(), id);
            if (!local)
                return local.error();
            if (action == "invert")
                fields.emplace_back(
                    "localMaskInverted",
                    develop_mask_editor_state(local.value(), DevelopMaskTarget::kLocal).inverted ?
                        0 :
                        1);
            if (fields.empty())
                return make_error(ErrorCode::kInvalidArgument, "Mask set requires --set fields");
            for (const auto &[name, value] : fields)
            {
                auto applied = apply_develop_field_strict(local.value(), name, value);
                if (!applied)
                    return applied.error();
            }
            auto applied = set_local_adjustment_develop(params.value(), id, local.value());
            if (!applied)
                return applied.error();
        }
        else if (action == "rename" || action == "enable")
        {
            auto local = std::find_if(params.value().local_adjustments.begin(),
                                      params.value().local_adjustments.end(), [&](const auto &item)
                                      { return item.operation.instance_id == id; });
            if (local == params.value().local_adjustments.end())
                return make_error(ErrorCode::kNotFound, "Local adjustment does not exist");
            if (action == "rename")
            {
                if (flags["--name"].empty() || flags["--name"].size() > 200)
                    return make_error(ErrorCode::kInvalidArgument,
                                      "Mask name must contain 1 through 200 bytes");
                local->operation.name = flags["--name"];
            }
            else
            {
                if (flags["--enabled"] != "true" && flags["--enabled"] != "false")
                    return make_error(ErrorCode::kInvalidArgument,
                                      "--enabled requires true or false");
                local->operation.enabled = flags["--enabled"] == "true";
                local->operation.bypass = false;
            }
        }
        else
            return make_error(ErrorCode::kInvalidArgument, "Unknown mask action");
        auto recipe = recipe_from_develop(loaded.value().asset, params.value());
        if (!recipe)
            return recipe.error();
        RecipeSaveOptions options;
        options.expected_revision = revision.value().revision;
        auto saved = service.save_recipe(flags["--asset-id"], recipe.value(), options);
        if (!saved)
            return saved.error();
        loaded = service.load_recipe(flags["--asset-id"]);
        if (!loaded)
            return loaded.error();
        params = develop_from_recipe(loaded.value());
        if (!params)
            return params.error();
        revision = service.snapshot();
        if (!revision)
            return revision.error();
    }
    auto recipe = recipe_from_develop(loaded.value().asset, params.value());
    if (!recipe)
        return recipe.error();
    auto encoded = recipe_to_json(recipe.value());
    if (!encoded)
        return encoded.error();
    return JsonValue{JsonValue::Object{
        {"schema_version", JsonValue::number("1")},
        {"asset_id", flags["--asset-id"]},
        {"revision", JsonValue::number(std::to_string(revision.value().revision))},
        {"recipe", std::move(encoded).value()}}};
}
} // namespace ravo::cli_internal
