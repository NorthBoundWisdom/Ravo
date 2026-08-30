#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/recipe/recipe.h"

namespace ravo
{

inline constexpr std::int64_t kRecipeStyleCompleteSchemaVersion = 1;
inline constexpr std::int64_t kRecipeStyleSelectedSchemaVersion = 2;
inline constexpr std::int64_t kRecipeStyleSchemaVersion = kRecipeStyleSelectedSchemaVersion;
inline constexpr std::string_view kRecipeStyleAssetId = "style-template";
inline constexpr std::string_view kRecipeStyleInputUri = "ravo-style://template";
inline constexpr std::size_t kRecipeStyleNameMaxBytes = 128U;
inline constexpr std::size_t kRecipeStyleDescriptionMaxBytes = 2048U;
inline constexpr std::size_t kRecipeStyleFileMaxBytes = 8U * 1024U * 1024U;

struct RecipeStyle
{
    std::int64_t schema_version = kRecipeStyleSchemaVersion;
    std::string name;
    std::string description;
    Recipe recipe;
    // Empty only for schema v1 complete-replacement styles. Schema v2 stores a
    // sorted, unique selection of stable Develop preset-field identifiers.
    std::vector<std::string> selected_fields;
};

[[nodiscard]] Result<RecipeStyle> recipe_style_from_recipe(std::string name,
                                                           std::string description,
                                                           Recipe recipe);
[[nodiscard]] Result<RecipeStyle>
recipe_style_from_selected_fields(std::string name, std::string description, Recipe recipe,
                                  std::vector<std::string> selected_fields);
[[nodiscard]] Result<RecipeStyle> parse_recipe_style_json(std::string_view text);
[[nodiscard]] Result<std::string> serialize_recipe_style(const RecipeStyle &style);
[[nodiscard]] Result<Recipe> apply_recipe_style(const RecipeStyle &style, AssetDescriptor asset);
[[nodiscard]] Result<Recipe> apply_recipe_style(const RecipeStyle &style, Recipe target);

} // namespace ravo
