#include "ravo/recipe/style.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>

namespace ravo
{
namespace
{

[[nodiscard]] TaskError style_error(const std::string_view message, const std::string_view reason,
                                    const std::string_view field = {})
{
    std::map<std::string, std::string, std::less<>> context{{"reason", std::string(reason)}};
    if (!field.empty())
        context.emplace("field", std::string(field));
    return make_error(ErrorCode::kValidation, std::string(message), std::move(context));
}

[[nodiscard]] Result<void> validate_text(const std::string &value, const std::string_view field,
                                         const std::size_t maximum, const bool empty_allowed)
{
    if ((!empty_allowed && value.empty()) || value.size() > maximum ||
        value.find('\0') != std::string::npos || value.find('\r') != std::string::npos ||
        (!empty_allowed && value.find('\n') != std::string::npos))
        return style_error("Recipe style text field is invalid", "invalid_recipe_style_text",
                           field);
    return {};
}

[[nodiscard]] Result<void> validate_style(const RecipeStyle &style)
{
    if (style.schema_version != kRecipeStyleSchemaVersion)
        return make_error(ErrorCode::kUnsupported, "Recipe style schema is unsupported",
                          {{"schema_version", std::to_string(style.schema_version)},
                           {"reason", "unsupported_recipe_style_schema"}});
    auto name = validate_text(style.name, "name", kRecipeStyleNameMaxBytes, false);
    auto description =
        validate_text(style.description, "description", kRecipeStyleDescriptionMaxBytes, true);
    if (!name || !description)
        return !name ? name.error() : description.error();
    if (style.recipe.asset.id != kRecipeStyleAssetId ||
        style.recipe.asset.input_uri != kRecipeStyleInputUri ||
        style.recipe.asset.content_hash.has_value())
        return style_error("Recipe style asset placeholder is invalid",
                           "invalid_recipe_style_asset", "recipe.asset");
    return {};
}

[[nodiscard]] std::string_view trim_left(const std::string_view text) noexcept
{
    std::size_t offset = 0U;
    while (offset < text.size() &&
           (text[offset] == ' ' || text[offset] == '\t' || text[offset] == '\r' ||
            text[offset] == '\n'))
        ++offset;
    return text.substr(offset);
}

} // namespace

Result<RecipeStyle> recipe_style_from_recipe(std::string name, std::string description,
                                             Recipe recipe)
{
    recipe.asset = {std::string(kRecipeStyleAssetId), std::string(kRecipeStyleInputUri),
                    std::nullopt};
    RecipeStyle style{kRecipeStyleSchemaVersion, std::move(name), std::move(description),
                      std::move(recipe)};
    auto valid = validate_style(style);
    if (!valid)
        return valid.error();
    return style;
}

Result<RecipeStyle> parse_recipe_style_json(const std::string_view text)
{
    if (text.size() > kRecipeStyleFileMaxBytes)
        return style_error("Recipe style exceeds the file-size limit", "recipe_style_too_large");
    const auto trimmed = trim_left(text);
    if (trimmed.starts_with("<darktable_style") ||
        (trimmed.starts_with("<?xml") &&
         trimmed.substr(0U, std::min<std::size_t>(trimmed.size(), 4096U))
             .find("<darktable_style") != std::string_view::npos))
        return make_error(ErrorCode::kUnsupported, "Legacy dtstyle is not a Ravo recipe style",
                          {{"reason", "unsupported_legacy_dtstyle"}});
    auto json = parse_json(text);
    if (!json)
        return json.error();
    const auto *object = json.value().object_if();
    if (object == nullptr)
        return style_error("Recipe style must be a JSON object", "invalid_recipe_style_object");
    constexpr std::array<std::string_view, 4> names{"schema_version", "name", "description",
                                                    "recipe"};
    for (const auto &[name, ignored] : *object)
    {
        (void)ignored;
        if (std::find(names.begin(), names.end(), name) == names.end())
            return style_error("Recipe style field is unknown", "unknown_recipe_style_field",
                               name);
    }
    const auto schema = object->find("schema_version");
    const auto name = object->find("name");
    const auto description = object->find("description");
    const auto recipe = object->find("recipe");
    if (schema == object->end() || name == object->end() || description == object->end() ||
        recipe == object->end())
        return style_error("Recipe style field is required", "missing_recipe_style_field");
    const auto *schema_number = schema->second.number_if();
    const auto *name_text = name->second.string_if();
    const auto *description_text = description->second.string_if();
    if (schema_number == nullptr || name_text == nullptr || description_text == nullptr ||
        recipe->second.object_if() == nullptr)
        return style_error("Recipe style field has the wrong type", "invalid_recipe_style_type");
    std::int64_t schema_version = 0;
    const auto [position, error] =
        std::from_chars(schema_number->text.data(),
                        schema_number->text.data() + schema_number->text.size(), schema_version);
    if (error != std::errc{} || position != schema_number->text.data() + schema_number->text.size())
        return style_error("Recipe style schema must be an integer",
                           "invalid_recipe_style_schema");
    auto recipe_text = serialize_json(recipe->second);
    auto parsed_recipe = parse_recipe_json(recipe_text);
    if (!parsed_recipe)
        return parsed_recipe.error();
    RecipeStyle style{schema_version, *name_text, *description_text,
                      std::move(parsed_recipe).value()};
    auto valid = validate_style(style);
    if (!valid)
        return valid.error();
    return style;
}

Result<std::string> serialize_recipe_style(const RecipeStyle &style)
{
    auto valid = validate_style(style);
    if (!valid)
        return valid.error();
    auto recipe = recipe_to_json(style.recipe);
    if (!recipe)
        return recipe.error();
    return serialize_json(JsonValue{JsonValue::Object{
        {"description", style.description},
        {"name", style.name},
        {"recipe", std::move(recipe).value()},
        {"schema_version", JsonValue::number(std::to_string(style.schema_version))},
    }});
}

Result<Recipe> apply_recipe_style(const RecipeStyle &style, AssetDescriptor asset)
{
    auto valid = validate_style(style);
    if (!valid)
        return valid.error();
    if (asset.id.empty() || asset.input_uri.empty())
        return style_error("Style target asset is invalid", "invalid_recipe_style_target");
    Recipe result = style.recipe;
    result.asset = std::move(asset);
    return result;
}

} // namespace ravo
