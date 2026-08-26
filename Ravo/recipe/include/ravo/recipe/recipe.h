#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "ravo/foundation/error.h"
#include "ravo/foundation/json.h"

namespace ravo
{

class OperationRegistry;

struct ParameterValue
{
    using Array = std::vector<ParameterValue>;
    using Object = std::map<std::string, ParameterValue, std::less<>>;
    using Storage =
        std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Array, Object>;

    Storage value = nullptr;

    ParameterValue() = default;
    ParameterValue(std::nullptr_t);
    ParameterValue(bool value);
    ParameterValue(std::int64_t value);
    ParameterValue(double value);
    ParameterValue(std::string value);
    ParameterValue(const char *value);
    ParameterValue(Array value);
    ParameterValue(Object value);
};

enum class MaskKind
{
    kAll,
};

struct Mask
{
    std::string id;
    std::int64_t schema_version = 1;
    MaskKind kind = MaskKind::kAll;
};

struct AssetDescriptor
{
    std::string id;
    std::string input_uri;
    std::optional<std::string> content_hash;
};

struct OperationInstance
{
    std::string id;
    std::int64_t schema_version = 1;
    std::string instance_id;
    bool enabled = true;
    std::map<std::string, ParameterValue, std::less<>> parameters;
    std::optional<std::string> mask_id;
};

struct Recipe
{
    std::int64_t schema_version = 3;
    AssetDescriptor asset;
    std::vector<OperationInstance> operations;
    std::vector<Mask> masks;
};

[[nodiscard]] Result<ParameterValue> parameter_value_from_json(const JsonValue &value);
[[nodiscard]] Result<JsonValue> parameter_value_to_json(const ParameterValue &value);

[[nodiscard]] Result<Recipe> parse_recipe_json(std::string_view text);
[[nodiscard]] Result<Recipe> upgrade_recipe(Recipe recipe);
[[nodiscard]] Result<JsonValue> recipe_to_json(const Recipe &recipe);
[[nodiscard]] Result<std::string> serialize_recipe(const Recipe &recipe);
[[nodiscard]] Result<void> validate_recipe(const Recipe &recipe, const OperationRegistry &registry);

} // namespace ravo
