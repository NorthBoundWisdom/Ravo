#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "ravo/foundation/error.h"
#include "ravo/foundation/json.h"
#include "ravo/recipe/mask.h"

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
    [[nodiscard]] bool operator==(const ParameterValue &other) const noexcept;
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
    // ADR-0145: optional display name + bypass (skip eval, keep serialized).
    std::optional<std::string> name = std::nullopt;
    bool bypass = false;
    // Only ravo.local.adjustment/v1 owns children. Local groups cannot nest.
    std::vector<OperationInstance> children = {};
    [[nodiscard]] bool operator==(const OperationInstance &) const noexcept = default;
};

inline constexpr std::string_view kLocalAdjustmentOperationId = "ravo.local.adjustment";
inline constexpr std::size_t kLocalAdjustmentMaxCount = 64;
inline constexpr std::size_t kLocalAdjustmentMaxOperations = 64;

struct LocalAdjustment
{
    OperationInstance operation;
    // Reconstructed from the ordered recipe, never serialized as UI state.
    // Surviving following instances preserve an imported group's pipeline slot.
    std::vector<std::string> following_instances;
    [[nodiscard]] bool operator==(const LocalAdjustment &) const noexcept = default;
};

struct Recipe
{
    std::int64_t schema_version = 4;
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
