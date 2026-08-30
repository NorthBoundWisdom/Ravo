#include "ravo/recipe/recipe.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <ios>
#include <limits>
#include <locale>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "ravo/recipe/color_contrast.h"
#include "ravo/recipe/canvas_frame.h"
#include "ravo/recipe/color_correction.h"
#include "ravo/recipe/color_harmonizer.h"
#include "ravo/recipe/color_input.h"
#include "ravo/recipe/color_output.h"
#include "ravo/recipe/color_reconstruction.h"
#include "ravo/recipe/color_zones.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/dehaze.h"
#include "ravo/recipe/lut3d.h"
#include "ravo/recipe/monochrome.h"
#include "ravo/recipe/operation.h"
#include "ravo/recipe/output_dither.h"
#include "ravo/recipe/perspective.h"
#include "ravo/recipe/profile_gamma.h"
#include "ravo/recipe/primaries.h"
#include "ravo/recipe/retouch.h"
#include "ravo/recipe/sharpen.h"
#include "ravo/recipe/split_toning.h"
#include "ravo/recipe/texture.h"
#include "ravo/recipe/watermark.h"
#include "ravo/recipe/velvia.h"

namespace ravo
{

ParameterValue::ParameterValue(const std::nullptr_t value)
    : value(value)
{
}
ParameterValue::ParameterValue(const bool value)
    : value(value)
{
}
ParameterValue::ParameterValue(const std::int64_t value)
    : value(value)
{
}
ParameterValue::ParameterValue(const double value)
    : value(value)
{
}
ParameterValue::ParameterValue(std::string value)
    : value(std::move(value))
{
}
ParameterValue::ParameterValue(const char *value)
    : value(std::string(value))
{
}
ParameterValue::ParameterValue(Array value)
    : value(std::move(value))
{
}
ParameterValue::ParameterValue(Object value)
    : value(std::move(value))
{
}

namespace
{

using JsonObject = JsonValue::Object;

[[nodiscard]] TaskError field_error(const std::string_view message, const std::string_view path)
{
    return make_error(ErrorCode::kValidation, std::string(message), {{"path", std::string(path)}});
}

[[nodiscard]] Result<void> reject_unknown_fields(const JsonObject &object,
                                                 const std::set<std::string, std::less<>> &allowed,
                                                 const std::string_view path)
{
    for (const auto &[name, ignored] : object)
    {
        static_cast<void>(ignored);
        if (!allowed.contains(name))
        {
            return field_error("Unknown JSON field", std::string(path) + "." + name);
        }
    }
    return {};
}

[[nodiscard]] Result<const JsonObject *> object_at(const JsonValue &value,
                                                   const std::string_view path)
{
    const auto *object = value.object_if();
    if (object == nullptr)
    {
        return field_error("Expected a JSON object", path);
    }
    return object;
}

[[nodiscard]] Result<const JsonValue *>
required_field(const JsonObject &object, const std::string_view name, const std::string_view path)
{
    const auto iterator = object.find(std::string(name));
    if (iterator == object.end())
    {
        return field_error("Required JSON field is missing",
                           std::string(path) + "." + std::string(name));
    }
    return &iterator->second;
}

[[nodiscard]] Result<std::string> string_at(const JsonValue &value, const std::string_view path)
{
    const auto *string = value.string_if();
    if (string == nullptr)
    {
        return field_error("Expected a JSON string", path);
    }
    return *string;
}

[[nodiscard]] Result<bool> boolean_at(const JsonValue &value, const std::string_view path)
{
    const auto *boolean = value.boolean_if();
    if (boolean == nullptr)
    {
        return field_error("Expected a JSON boolean", path);
    }
    return *boolean;
}

[[nodiscard]] Result<std::int64_t> integer_at(const JsonValue &value, const std::string_view path)
{
    const auto *number = value.number_if();
    if (number == nullptr)
    {
        return field_error("Expected an integer JSON number", path);
    }
    std::int64_t result = 0;
    const auto [position, error] =
        std::from_chars(number->text.data(), number->text.data() + number->text.size(), result);
    if (error != std::errc{} || position != number->text.data() + number->text.size())
    {
        return field_error("Expected an integer JSON number", path);
    }
    return result;
}

[[nodiscard]] bool parse_json_double(const std::string_view text, double &result)
{
    if (text.empty())
    {
        return false;
    }
    // Apple libc++ still lacks floating std::from_chars. Parse the JSON number
    // token with the classic locale so the decimal point stays '.' on every host.
    std::istringstream stream{std::string(text)};
    stream.imbue(std::locale::classic());
    stream >> std::noskipws;
    double value = 0.0;
    if (!(stream >> value) || stream.peek() != std::char_traits<char>::eof() ||
        !std::isfinite(value))
    {
        return false;
    }
    result = value;
    return true;
}

[[nodiscard]] Result<std::string> format_json_double(const double value)
{
    if (!std::isfinite(value))
    {
        return make_error(ErrorCode::kValidation, "Recipe numeric values must be finite");
    }
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream.precision(std::numeric_limits<double>::max_digits10);
    stream << value;
    if (!stream)
    {
        return make_error(ErrorCode::kInternal, "Unable to serialize a recipe numeric value");
    }
    return stream.str();
}

[[nodiscard]] Result<double> double_at(const JsonValue &value, const std::string_view path)
{
    const auto *number = value.number_if();
    if (number == nullptr)
    {
        return field_error("Expected a numeric JSON value", path);
    }
    double result = 0.0;
    if (!parse_json_double(number->text, result))
    {
        return field_error("Expected a finite numeric JSON value", path);
    }
    return result;
}

[[nodiscard]] JsonValue integer_json(const std::int64_t value)
{
    return JsonValue::number(std::to_string(value));
}

[[nodiscard]] Result<JsonValue> double_json(const double value)
{
    auto text = format_json_double(value);
    if (!text)
    {
        return text.error();
    }
    return JsonValue::number(std::move(text.value()));
}

[[nodiscard]] Result<AssetDescriptor> parse_asset(const JsonValue &value)
{
    auto object = object_at(value, "asset");
    if (!object)
    {
        return object.error();
    }
    auto fields =
        reject_unknown_fields(*object.value(), {"content_hash", "id", "input_uri"}, "asset");
    if (!fields)
    {
        return fields.error();
    }
    auto id = required_field(*object.value(), "id", "asset");
    auto input_uri = required_field(*object.value(), "input_uri", "asset");
    if (!id)
    {
        return id.error();
    }
    if (!input_uri)
    {
        return input_uri.error();
    }
    auto parsed_id = string_at(*id.value(), "asset.id");
    auto parsed_uri = string_at(*input_uri.value(), "asset.input_uri");
    if (!parsed_id)
    {
        return parsed_id.error();
    }
    if (!parsed_uri)
    {
        return parsed_uri.error();
    }

    AssetDescriptor asset{std::move(parsed_id).value(), std::move(parsed_uri).value(),
                          std::nullopt};
    const auto hash = object.value()->find("content_hash");
    if (hash != object.value()->end())
    {
        auto content_hash = string_at(hash->second, "asset.content_hash");
        if (!content_hash)
        {
            return content_hash.error();
        }
        asset.content_hash = std::move(content_hash).value();
    }
    return asset;
}

[[nodiscard]] Result<OperationInstance> parse_operation(const JsonValue &value,
                                                        const std::string_view path)
{
    auto object = object_at(value, path);
    if (!object)
    {
        return object.error();
    }
    auto fields = reject_unknown_fields(
        *object.value(),
        {"enabled", "id", "instance_id", "mask_id", "parameters", "schema_version"}, path);
    if (!fields)
    {
        return fields.error();
    }
    auto id = required_field(*object.value(), "id", path);
    auto schema_version = required_field(*object.value(), "schema_version", path);
    auto instance_id = required_field(*object.value(), "instance_id", path);
    auto enabled = required_field(*object.value(), "enabled", path);
    auto parameters = required_field(*object.value(), "parameters", path);
    if (!id || !schema_version || !instance_id || !enabled || !parameters)
    {
        return !id             ? id.error() :
               !schema_version ? schema_version.error() :
               !instance_id    ? instance_id.error() :
               !enabled        ? enabled.error() :
                                 parameters.error();
    }

    auto parsed_id = string_at(*id.value(), std::string(path) + ".id");
    auto parsed_schema = integer_at(*schema_version.value(), std::string(path) + ".schema_version");
    auto parsed_instance = string_at(*instance_id.value(), std::string(path) + ".instance_id");
    auto parsed_enabled = boolean_at(*enabled.value(), std::string(path) + ".enabled");
    auto parameter_object = object_at(*parameters.value(), std::string(path) + ".parameters");
    if (!parsed_id || !parsed_schema || !parsed_instance || !parsed_enabled || !parameter_object)
    {
        return !parsed_id       ? parsed_id.error() :
               !parsed_schema   ? parsed_schema.error() :
               !parsed_instance ? parsed_instance.error() :
               !parsed_enabled  ? parsed_enabled.error() :
                                  parameter_object.error();
    }

    OperationInstance operation{std::move(parsed_id).value(),
                                parsed_schema.value(),
                                std::move(parsed_instance).value(),
                                parsed_enabled.value(),
                                {},
                                std::nullopt};
    for (const auto &[name, parameter] : *parameter_object.value())
    {
        auto parsed_parameter = parameter_value_from_json(parameter);
        if (!parsed_parameter)
        {
            auto error = parsed_parameter.error();
            error.context.emplace("path", std::string(path) + ".parameters." + name);
            return error;
        }
        operation.parameters.emplace(name, std::move(parsed_parameter).value());
    }
    const auto mask = object.value()->find("mask_id");
    if (mask != object.value()->end())
    {
        auto parsed_mask = string_at(mask->second, std::string(path) + ".mask_id");
        if (!parsed_mask)
        {
            return parsed_mask.error();
        }
        operation.mask_id = std::move(parsed_mask).value();
    }
    return operation;
}

[[nodiscard]] Result<void> validate_parameter_type(const ParameterValue &value,
                                                   const ParameterRule &rule,
                                                   const std::string &operation_id)
{
    const auto type_matches = [&]
    {
        switch (rule.type)
        {
        case ParameterType::kBoolean:
            return std::holds_alternative<bool>(value.value);
        case ParameterType::kInteger:
            return std::holds_alternative<std::int64_t>(value.value);
        case ParameterType::kNumber:
            return std::holds_alternative<std::int64_t>(value.value) ||
                   std::holds_alternative<double>(value.value);
        case ParameterType::kString:
            return std::holds_alternative<std::string>(value.value);
        case ParameterType::kArray:
            return std::holds_alternative<ParameterValue::Array>(value.value);
        case ParameterType::kObject:
            return std::holds_alternative<ParameterValue::Object>(value.value);
        }
        return false;
    };
    if (!type_matches())
    {
        return make_error(ErrorCode::kValidation, "Recipe parameter has the wrong type",
                          {{"expected_type", std::string(parameter_type_name(rule.type))},
                           {"operation_id", operation_id},
                           {"parameter", rule.name}});
    }

    if (rule.type == ParameterType::kInteger || rule.type == ParameterType::kNumber)
    {
        const double numeric = std::holds_alternative<std::int64_t>(value.value) ?
                                   static_cast<double>(std::get<std::int64_t>(value.value)) :
                                   std::get<double>(value.value);
        if (!std::isfinite(numeric))
        {
            return make_error(ErrorCode::kValidation, "Recipe numeric parameter must be finite",
                              {{"operation_id", operation_id}, {"parameter", rule.name}});
        }
        if ((rule.minimum.has_value() && numeric < *rule.minimum) ||
            (rule.maximum.has_value() && numeric > *rule.maximum))
        {
            return make_error(ErrorCode::kValidation,
                              "Recipe parameter is outside the permitted range",
                              {{"operation_id", operation_id}, {"parameter", rule.name}});
        }
    }
    return {};
}

} // namespace

Result<ParameterValue> parameter_value_from_json(const JsonValue &value)
{
    if (value.is_null())
    {
        return ParameterValue{nullptr};
    }
    if (const auto *boolean = value.boolean_if(); boolean != nullptr)
    {
        return ParameterValue{*boolean};
    }
    if (const auto *number = value.number_if(); number != nullptr)
    {
        std::int64_t integer = 0;
        const auto [integer_position, integer_error] = std::from_chars(
            number->text.data(), number->text.data() + number->text.size(), integer);
        if (integer_error == std::errc{} &&
            integer_position == number->text.data() + number->text.size())
        {
            return ParameterValue{integer};
        }
        auto floating = double_at(value, "parameter");
        if (!floating)
        {
            return floating.error();
        }
        return ParameterValue{floating.value()};
    }
    if (const auto *string = value.string_if(); string != nullptr)
    {
        return ParameterValue{*string};
    }
    if (const auto *array = value.array_if(); array != nullptr)
    {
        ParameterValue::Array result;
        result.reserve(array->size());
        for (const auto &child : *array)
        {
            auto parameter = parameter_value_from_json(child);
            if (!parameter)
            {
                return parameter.error();
            }
            result.push_back(std::move(parameter).value());
        }
        return ParameterValue{std::move(result)};
    }
    if (const auto *object = value.object_if(); object != nullptr)
    {
        ParameterValue::Object result;
        for (const auto &[name, child] : *object)
        {
            auto parameter = parameter_value_from_json(child);
            if (!parameter)
            {
                return parameter.error();
            }
            result.emplace(name, std::move(parameter).value());
        }
        return ParameterValue{std::move(result)};
    }
    return make_error(ErrorCode::kInternal, "Unreachable JSON value kind while parsing parameter");
}

Result<JsonValue> parameter_value_to_json(const ParameterValue &value)
{
    if (std::holds_alternative<std::nullptr_t>(value.value))
    {
        return JsonValue{};
    }
    if (const auto *boolean = std::get_if<bool>(&value.value); boolean != nullptr)
    {
        return JsonValue{*boolean};
    }
    if (const auto *integer = std::get_if<std::int64_t>(&value.value); integer != nullptr)
    {
        return integer_json(*integer);
    }
    if (const auto *floating = std::get_if<double>(&value.value); floating != nullptr)
    {
        return double_json(*floating);
    }
    if (const auto *string = std::get_if<std::string>(&value.value); string != nullptr)
    {
        return JsonValue{*string};
    }
    if (const auto *array = std::get_if<ParameterValue::Array>(&value.value); array != nullptr)
    {
        JsonValue::Array result;
        result.reserve(array->size());
        for (const auto &child : *array)
        {
            auto json = parameter_value_to_json(child);
            if (!json)
            {
                return json.error();
            }
            result.push_back(std::move(json).value());
        }
        return JsonValue{std::move(result)};
    }
    if (const auto *object = std::get_if<ParameterValue::Object>(&value.value); object != nullptr)
    {
        JsonValue::Object result;
        for (const auto &[name, child] : *object)
        {
            auto json = parameter_value_to_json(child);
            if (!json)
            {
                return json.error();
            }
            result.emplace(name, std::move(json).value());
        }
        return JsonValue{std::move(result)};
    }
    return make_error(ErrorCode::kInternal, "Unreachable parameter value kind while serializing");
}

Result<Recipe> parse_recipe_json(const std::string_view text)
{
    auto json = parse_json(text);
    if (!json)
    {
        return json.error();
    }
    auto object = object_at(json.value(), "recipe");
    if (!object)
    {
        return object.error();
    }
    auto fields = reject_unknown_fields(
        *object.value(), {"asset", "masks", "operations", "schema_version"}, "recipe");
    if (!fields)
    {
        return fields.error();
    }

    auto schema_version = required_field(*object.value(), "schema_version", "recipe");
    auto asset = required_field(*object.value(), "asset", "recipe");
    auto operations = required_field(*object.value(), "operations", "recipe");
    auto masks = required_field(*object.value(), "masks", "recipe");
    if (!schema_version || !asset || !operations || !masks)
    {
        return !schema_version ? schema_version.error() :
               !asset          ? asset.error() :
               !operations     ? operations.error() :
                                 masks.error();
    }
    auto parsed_schema = integer_at(*schema_version.value(), "recipe.schema_version");
    auto parsed_asset = parse_asset(*asset.value());
    const auto *operation_array = operations.value()->array_if();
    const auto *mask_array = masks.value()->array_if();
    if (!parsed_schema || !parsed_asset || operation_array == nullptr || mask_array == nullptr)
    {
        return !parsed_schema ? parsed_schema.error() :
               !parsed_asset  ? parsed_asset.error() :
               operation_array == nullptr ?
                               field_error("Expected a JSON array", "recipe.operations") :
                               field_error("Expected a JSON array", "recipe.masks");
    }
    if (mask_array->size() > kCanonicalMaskMaxNodes)
    {
        return make_error(
            ErrorCode::kValidation, "Mask graph exceeds the canonical node limit",
            {{"reason", "mask_graph_too_large"}, {"count", std::to_string(mask_array->size())}});
    }

    Recipe recipe{parsed_schema.value(), std::move(parsed_asset).value(), {}, {}};
    recipe.operations.reserve(operation_array->size());
    for (std::size_t index = 0; index < operation_array->size(); ++index)
    {
        auto operation = parse_operation((*operation_array)[index],
                                         "recipe.operations[" + std::to_string(index) + "]");
        if (!operation)
        {
            return operation.error();
        }
        recipe.operations.push_back(std::move(operation).value());
    }
    recipe.masks.reserve(mask_array->size());
    for (std::size_t index = 0; index < mask_array->size(); ++index)
    {
        auto mask = parse_canonical_mask((*mask_array)[index],
                                         "recipe.masks[" + std::to_string(index) + "]");
        if (!mask)
        {
            return mask.error();
        }
        recipe.masks.push_back(std::move(mask).value());
    }
    return upgrade_recipe(std::move(recipe));
}

Result<Recipe> upgrade_recipe(Recipe recipe)
{
    auto masks = upgrade_mask_graph(recipe.masks);
    if (!masks)
    {
        return masks.error();
    }
    for (auto &operation : recipe.operations)
    {
        if (operation.id == kExposureOperationId)
        {
            auto upgraded = upgrade_exposure_operation(operation);
            if (!upgraded)
            {
                return upgraded.error();
            }
        }
        else if (operation.id == kColorContrastOperationId)
        {
            auto upgraded = upgrade_color_contrast_operation(operation);
            if (!upgraded)
            {
                return upgraded.error();
            }
        }
        else if (operation.id == kSharpenOperationId)
        {
            auto upgraded = upgrade_sharpen_operation(operation);
            if (!upgraded)
            {
                return upgraded.error();
            }
        }
        else if (operation.id == kDehazeOperationId)
        {
            auto upgraded = upgrade_dehaze_operation(operation);
            if (!upgraded)
            {
                return upgraded.error();
            }
        }
        else if (operation.id == kMonochromeOperationId)
        {
            auto upgraded = upgrade_monochrome_operation(operation);
            if (!upgraded)
                return upgraded.error();
        }
        else if (operation.id == kSplitToningOperationId)
        {
            auto upgraded = upgrade_split_toning_operation(operation);
            if (!upgraded)
                return upgraded.error();
        }
        else if (operation.id == kVelviaOperationId)
        {
            auto upgraded = upgrade_velvia_operation(operation);
            if (!upgraded)
                return upgraded.error();
        }
    }
    if (recipe.schema_version == 1)
    {
        for (auto &operation : recipe.operations)
        {
            if (operation.id == "ravo.color.input" && operation.parameters.empty())
            {
                operation.parameters = input_color_to_parameters(InputColorParams{});
            }
        }
        auto input =
            std::find_if(recipe.operations.begin(), recipe.operations.end(),
                         [](const OperationInstance &operation)
                         { return operation.enabled && operation.id == "ravo.color.input"; });
        if (input == recipe.operations.end())
        {
            std::size_t suffix = 1;
            std::string instance_id;
            do
            {
                instance_id = "color-input-" + std::to_string(suffix++);
            } while (std::any_of(recipe.operations.begin(), recipe.operations.end(),
                                 [&instance_id](const OperationInstance &operation)
                                 { return operation.instance_id == instance_id; }));
            const auto insertion =
                std::find_if(recipe.operations.begin(), recipe.operations.end(),
                             [](const OperationInstance &operation)
                             { return operation.id != "ravo.color.temperature"; });
            recipe.operations.insert(insertion,
                                     {"ravo.color.input", 1, std::move(instance_id), true,
                                      input_color_to_parameters(InputColorParams{}), std::nullopt});
        }
        recipe.schema_version = 2;
    }
    if (recipe.schema_version == 2)
    {
        for (auto &operation : recipe.operations)
        {
            if (operation.id == "ravo.color.output" && operation.parameters.empty())
            {
                operation.parameters = output_color_to_parameters(OutputColorParams{});
            }
        }
        const auto output =
            std::find_if(recipe.operations.begin(), recipe.operations.end(),
                         [](const OperationInstance &operation)
                         { return operation.enabled && operation.id == "ravo.color.output"; });
        if (output == recipe.operations.end())
        {
            std::size_t suffix = 1;
            std::string instance_id;
            do
            {
                instance_id = "color-output-" + std::to_string(suffix++);
            } while (std::any_of(recipe.operations.begin(), recipe.operations.end(),
                                 [&instance_id](const OperationInstance &operation)
                                 { return operation.instance_id == instance_id; }));
            recipe.operations.push_back({"ravo.color.output", 1, std::move(instance_id), true,
                                         output_color_to_parameters(OutputColorParams{}),
                                         std::nullopt});
        }
        recipe.schema_version = 3;
    }
    if (recipe.schema_version == 3)
    {
        return recipe;
    }
    if (recipe.schema_version > 3)
    {
        return make_error(ErrorCode::kUnsupported, "Recipe schema version is newer than Ravo",
                          {{"schema_version", std::to_string(recipe.schema_version)}});
    }
    return make_error(ErrorCode::kUnsupported,
                      "Recipe schema version has no registered upgrade path",
                      {{"schema_version", std::to_string(recipe.schema_version)}});
}

Result<JsonValue> recipe_to_json(const Recipe &recipe)
{
    auto mask_graph = validate_mask_graph(recipe.masks);
    if (!mask_graph)
    {
        return mask_graph.error();
    }
    JsonValue::Object asset{{"id", recipe.asset.id}, {"input_uri", recipe.asset.input_uri}};
    if (recipe.asset.content_hash.has_value())
    {
        asset.emplace("content_hash", *recipe.asset.content_hash);
    }

    JsonValue::Array operations;
    operations.reserve(recipe.operations.size());
    for (const auto &operation : recipe.operations)
    {
        JsonValue::Object parameters;
        for (const auto &[name, value] : operation.parameters)
        {
            auto json = parameter_value_to_json(value);
            if (!json)
            {
                return json.error();
            }
            parameters.emplace(name, std::move(json).value());
        }
        JsonValue::Object entry{
            {"enabled", operation.enabled},
            {"id", operation.id},
            {"instance_id", operation.instance_id},
            {"parameters", std::move(parameters)},
            {"schema_version", integer_json(operation.schema_version)},
        };
        if (operation.mask_id.has_value())
        {
            entry.emplace("mask_id", *operation.mask_id);
        }
        operations.emplace_back(std::move(entry));
    }

    JsonValue::Array masks;
    masks.reserve(recipe.masks.size());
    for (const auto &mask : recipe.masks)
    {
        auto encoded = canonical_mask_to_json(mask);
        if (!encoded)
        {
            return encoded.error();
        }
        masks.emplace_back(std::move(encoded).value());
    }

    return JsonValue{JsonValue::Object{{"asset", std::move(asset)},
                                       {"masks", std::move(masks)},
                                       {"operations", std::move(operations)},
                                       {"schema_version", integer_json(recipe.schema_version)}}};
}

Result<std::string> serialize_recipe(const Recipe &recipe)
{
    auto json = recipe_to_json(recipe);
    if (!json)
    {
        return json.error();
    }
    return serialize_json(json.value());
}

Result<void> validate_recipe(const Recipe &recipe, const OperationRegistry &registry)
{
    if (recipe.schema_version != 3)
    {
        return make_error(ErrorCode::kUnsupported, "Unsupported recipe schema version",
                          {{"schema_version", std::to_string(recipe.schema_version)}});
    }
    if (recipe.asset.id.empty() || recipe.asset.input_uri.empty())
    {
        return make_error(ErrorCode::kValidation,
                          "Recipe asset ID and input URI must not be empty");
    }

    auto mask_graph = validate_mask_graph(recipe.masks);
    if (!mask_graph)
    {
        return mask_graph.error();
    }
    std::set<std::string, std::less<>> masks;
    for (const auto &mask : recipe.masks)
    {
        masks.insert(mask.id);
    }

    std::set<std::string, std::less<>> instances;
    for (const auto &stored_operation : recipe.operations)
    {
        OperationInstance upgraded_operation;
        const OperationInstance *operation_pointer = &stored_operation;
        if (stored_operation.id == kExposureOperationId && stored_operation.schema_version == 1)
        {
            upgraded_operation = stored_operation;
            auto upgraded = upgrade_exposure_operation(upgraded_operation);
            if (!upgraded)
            {
                return upgraded.error();
            }
            operation_pointer = &upgraded_operation;
        }
        else if (stored_operation.id == kColorContrastOperationId &&
                 stored_operation.schema_version == 1)
        {
            upgraded_operation = stored_operation;
            auto upgraded = upgrade_color_contrast_operation(upgraded_operation);
            if (!upgraded)
            {
                return upgraded.error();
            }
            operation_pointer = &upgraded_operation;
        }
        else if (stored_operation.id == kSharpenOperationId && stored_operation.schema_version == 1)
        {
            upgraded_operation = stored_operation;
            auto upgraded = upgrade_sharpen_operation(upgraded_operation);
            if (!upgraded)
            {
                return upgraded.error();
            }
            operation_pointer = &upgraded_operation;
        }
        else if (stored_operation.id == kDehazeOperationId && stored_operation.schema_version == 1)
        {
            upgraded_operation = stored_operation;
            auto upgraded = upgrade_dehaze_operation(upgraded_operation);
            if (!upgraded)
            {
                return upgraded.error();
            }
            operation_pointer = &upgraded_operation;
        }
        else if (stored_operation.id == kMonochromeOperationId &&
                 stored_operation.schema_version == 1)
        {
            upgraded_operation = stored_operation;
            auto upgraded = upgrade_monochrome_operation(upgraded_operation);
            if (!upgraded)
                return upgraded.error();
            operation_pointer = &upgraded_operation;
        }
        else if (stored_operation.id == kSplitToningOperationId &&
                 stored_operation.schema_version == 1)
        {
            upgraded_operation = stored_operation;
            auto upgraded = upgrade_split_toning_operation(upgraded_operation);
            if (!upgraded)
                return upgraded.error();
            operation_pointer = &upgraded_operation;
        }
        else if (stored_operation.id == kVelviaOperationId && stored_operation.schema_version == 1)
        {
            upgraded_operation = stored_operation;
            auto upgraded = upgrade_velvia_operation(upgraded_operation);
            if (!upgraded)
                return upgraded.error();
            operation_pointer = &upgraded_operation;
        }
        const auto &operation = *operation_pointer;
        if (operation.id.empty() || operation.instance_id.empty())
        {
            return make_error(ErrorCode::kValidation,
                              "Recipe operation ID and instance ID must not be empty");
        }
        if (!instances.insert(operation.instance_id).second)
        {
            return make_error(ErrorCode::kConflict,
                              "Recipe contains duplicate operation instance IDs",
                              {{"instance_id", operation.instance_id}});
        }
        const auto *descriptor = registry.find(operation.id);
        if (descriptor == nullptr)
        {
            return make_error(ErrorCode::kUnsupported, "Recipe references an unknown operation",
                              {{"operation_id", operation.id}});
        }
        if (operation.schema_version != descriptor->parameter_schema_version)
        {
            return make_error(ErrorCode::kUnsupported,
                              "Recipe operation schema version is unsupported",
                              {{"operation_id", operation.id},
                               {"schema_version", std::to_string(operation.schema_version)}});
        }
        if (operation.mask_id.has_value() &&
            (!descriptor->supports_mask || !masks.contains(*operation.mask_id)))
        {
            return make_error(ErrorCode::kUnsupported,
                              "Recipe operation cannot use the requested mask",
                              {{"operation_id", operation.id}, {"mask_id", *operation.mask_id}});
        }

        std::map<std::string, const ParameterRule *, std::less<>> rules;
        for (const auto &rule : descriptor->parameters)
        {
            rules.emplace(rule.name, &rule);
        }
        for (const auto &[name, parameter] : operation.parameters)
        {
            const auto rule = rules.find(name);
            if (rule == rules.end())
            {
                return make_error(ErrorCode::kValidation, "Recipe operation parameter is unknown",
                                  {{"operation_id", operation.id}, {"parameter", name}});
            }
            auto type = validate_parameter_type(parameter, *rule->second, operation.id);
            if (!type)
            {
                return type.error();
            }
        }
        for (const auto &rule : descriptor->parameters)
        {
            if (rule.required && !operation.parameters.contains(rule.name))
            {
                return make_error(ErrorCode::kValidation, "Recipe operation parameter is required",
                                  {{"operation_id", operation.id}, {"parameter", rule.name}});
            }
        }
        if (operation.id == kDemosaicOperationId)
        {
            auto demosaic = validate_demosaic_parameters(operation.parameters);
            if (!demosaic)
            {
                auto error = demosaic.error();
                error.context.emplace("operation_id", operation.id);
                return error;
            }
        }
        if (operation.id == "ravo.core.tonecurve")
        {
            auto curve = validate_tone_curve_parameters(operation.parameters);
            if (!curve)
            {
                auto error = curve.error();
                error.context.emplace("operation_id", operation.id);
                return error;
            }
        }
        if (operation.id == "ravo.color.rgbcurve")
        {
            auto curve = validate_rgb_curve_parameters(operation.parameters);
            if (!curve)
            {
                auto error = curve.error();
                error.context.emplace("operation_id", operation.id);
                return error;
            }
        }
        if (operation.id == "ravo.color.input")
        {
            auto input_color = validate_input_color_parameters(operation.parameters);
            if (!input_color)
            {
                auto error = input_color.error();
                error.context.emplace("operation_id", operation.id);
                return error;
            }
        }
        if (operation.id == "ravo.color.output")
        {
            auto output_color = validate_output_color_parameters(operation.parameters);
            if (!output_color)
            {
                auto error = output_color.error();
                error.context.emplace("operation_id", operation.id);
                return error;
            }
        }
        if (operation.id == kProfileGammaOperationId)
        {
            auto profile_gamma = validate_profile_gamma_parameters(operation.parameters);
            if (!profile_gamma)
            {
                auto error = profile_gamma.error();
                error.context.emplace("operation_id", operation.id);
                return error;
            }
        }
        if (operation.id == kPrimariesOperationId)
        {
            auto primaries = validate_primaries_parameters(operation.parameters);
            if (!primaries)
            {
                auto error = primaries.error();
                error.context.emplace("operation_id", operation.id);
                return error;
            }
        }
        if (operation.id == kExposureOperationId)
        {
            auto exposure = validate_exposure_parameters(operation.parameters);
            if (!exposure)
            {
                auto error = exposure.error();
                error.context.emplace("operation_id", operation.id);
                return error;
            }
        }
        if (operation.id == "ravo.display.sigmoid")
        {
            auto sigmoid = validate_sigmoid_parameters(operation.parameters);
            if (!sigmoid)
            {
                auto error = sigmoid.error();
                error.context.emplace("operation_id", operation.id);
                return error;
            }
        }
        if (operation.id == "ravo.color.channelmixerrgb")
        {
            auto mixer = validate_channel_mixer_parameters(operation.parameters);
            if (!mixer)
            {
                auto error = mixer.error();
                error.context.emplace("operation_id", operation.id);
                return error;
            }
        }
        if (operation.id == "ravo.color.temperature")
        {
            auto temperature = validate_temperature_parameters(operation.parameters);
            if (!temperature)
            {
                auto error = temperature.error();
                error.context.emplace("operation_id", operation.id);
                return error;
            }
        }
        if (operation.id == "ravo.color.colorbalancergb")
        {
            auto color_balance = validate_color_balance_rgb_parameters(operation.parameters);
            if (!color_balance)
            {
                auto error = color_balance.error();
                error.context.emplace("operation_id", operation.id);
                return error;
            }
        }
        if (operation.id == kColorBalanceOperationId)
        {
            auto color_balance = validate_color_balance_parameters(operation.parameters);
            if (!color_balance)
            {
                auto error = color_balance.error();
                error.context.emplace("operation_id", operation.id);
                return error;
            }
        }
        if (operation.id == kColorCheckerOperationId)
        {
            auto color_checker = validate_color_checker_parameters(operation.parameters);
            if (!color_checker)
            {
                auto error = color_checker.error();
                error.context.emplace("operation_id", operation.id);
                return error;
            }
        }
        if (operation.id == kColorHarmonizerOperationId)
        {
            auto color_harmonizer = validate_color_harmonizer_parameters(operation.parameters);
            if (!color_harmonizer)
            {
                auto error = color_harmonizer.error();
                error.context.emplace("operation_id", operation.id);
                return error;
            }
        }
        if (operation.id == kColorCorrectionOperationId)
        {
            auto color_correction = validate_color_correction_parameters(operation.parameters);
            if (!color_correction)
            {
                auto error = color_correction.error();
                error.context.emplace("operation_id", operation.id);
                return error;
            }
        }
        if (operation.id == kColorContrastOperationId)
        {
            auto color_contrast = validate_color_contrast_parameters(operation.parameters);
            if (!color_contrast)
            {
                auto error = color_contrast.error();
                error.context.emplace("operation_id", operation.id);
                return error;
            }
        }
        if (operation.id == kColorReconstructionOperationId)
        {
            auto color_reconstruction =
                validate_color_reconstruction_parameters(operation.parameters);
            if (!color_reconstruction)
            {
                auto error = color_reconstruction.error();
                error.context.emplace("operation_id", operation.id);
                return error;
            }
        }
        if (operation.id == kSharpenOperationId)
        {
            auto sharpen = validate_sharpen_parameters(operation.parameters);
            if (!sharpen)
            {
                auto error = sharpen.error();
                error.context.emplace("operation_id", operation.id);
                return error;
            }
        }
        if (operation.id == kTextureOperationId)
        {
            auto texture = validate_texture_parameters(operation.parameters);
            if (!texture)
            {
                auto error = texture.error();
                error.context.emplace("operation_id", operation.id);
                return error;
            }
        }
        if (operation.id == kDehazeOperationId)
        {
            auto dehaze = validate_dehaze_parameters(operation.parameters);
            if (!dehaze)
            {
                auto error = dehaze.error();
                error.context.emplace("operation_id", operation.id);
                return error;
            }
        }
        if (operation.id == kRetouchOperationId)
        {
            auto retouch = validate_retouch_operation(operation, recipe.masks);
            if (!retouch)
            {
                return retouch.error();
            }
        }
        if (operation.id == kOutputDitherOperationId)
        {
            auto dither = validate_output_dither_parameters(operation.parameters);
            if (!dither)
            {
                auto error = dither.error();
                error.context.emplace("operation_id", operation.id);
                return error;
            }
        }
        if (operation.id == kCanvasOperationId)
        {
            auto canvas = canvas_from_parameters(operation.parameters);
            if (!canvas)
                return canvas.error();
        }
        if (operation.id == kFrameOperationId)
        {
            auto frame = frame_from_parameters(operation.parameters);
            if (!frame)
                return frame.error();
        }
        if (operation.id == kWatermarkOperationId)
        {
            auto watermark = watermark_from_parameters(operation.parameters);
            if (!watermark)
                return watermark.error();
        }
        if (operation.id == kColorZonesOperationId)
        {
            auto zones = color_zones_from_parameters(operation.parameters);
            if (!zones)
                return zones.error();
        }
        if (operation.id == kMonochromeOperationId)
        {
            auto monochrome = monochrome_from_parameters(operation.parameters);
            if (!monochrome)
                return monochrome.error();
        }
        if (operation.id == kSplitToningOperationId)
        {
            auto split = split_toning_from_parameters(operation.parameters);
            if (!split)
                return split.error();
        }
        if (operation.id == kVelviaOperationId)
        {
            auto velvia = velvia_from_parameters(operation.parameters);
            if (!velvia)
                return velvia.error();
        }
        if (operation.id == kLut3dOperationId)
        {
            auto lut = lut3d_from_parameters(operation.parameters);
            if (!lut)
                return lut.error();
        }
        if (operation.id == "ravo.raw.highlights")
        {
            if (const auto found = operation.parameters.find("mode");
                found != operation.parameters.end())
            {
                const auto *text = std::get_if<std::string>(&found->second.value);
                if (text == nullptr ||
                    (*text != kRawHighlightsModeClip && *text != kRawHighlightsModeInpaint &&
                     *text != kRawHighlightsModeOpposed && *text != kRawHighlightsModeLch))
                {
                    return make_error(ErrorCode::kValidation,
                                      "RAW highlight reconstruction mode is unsupported",
                                      {{"operation_id", operation.id}});
                }
            }
        }
        if (operation.id == "ravo.geometry.lens")
        {
            if (const auto found = operation.parameters.find("mode");
                found != operation.parameters.end())
            {
                const auto *text = std::get_if<std::string>(&found->second.value);
                if (text == nullptr || (*text != kLensModeManual && *text != kLensModeLookup))
                {
                    return make_error(ErrorCode::kValidation, "Lens correction mode is unsupported",
                                      {{"operation_id", operation.id}});
                }
            }
        }
    }

    std::optional<std::size_t> input_color_index;
    std::optional<std::size_t> any_input_color_index;
    for (std::size_t index = 0; index < recipe.operations.size(); ++index)
    {
        const auto &operation = recipe.operations[index];
        if (operation.id != "ravo.color.input")
        {
            continue;
        }
        if (!any_input_color_index)
        {
            any_input_color_index = index;
        }
        if (!operation.enabled)
        {
            continue;
        }
        if (input_color_index)
        {
            return make_error(ErrorCode::kConflict,
                              "Recipe contains more than one enabled input colour operation",
                              {{"operation_id", operation.id}});
        }
        input_color_index = index;
    }

    std::optional<std::size_t> profile_gamma_index;
    for (std::size_t index = 0; index < recipe.operations.size(); ++index)
    {
        const auto &operation = recipe.operations[index];
        if (!operation.enabled || operation.id != kProfileGammaOperationId)
        {
            continue;
        }
        if (profile_gamma_index)
        {
            return make_error(ErrorCode::kConflict,
                              "Recipe contains more than one enabled profile gamma operation",
                              {{"operation_id", operation.id}});
        }
        profile_gamma_index = index;
    }
    if (profile_gamma_index)
    {
        if (!any_input_color_index)
        {
            return make_error(ErrorCode::kValidation,
                              "Profile gamma requires an input colour operation",
                              {{"operation_id", std::string(kProfileGammaOperationId)}});
        }
        if (*profile_gamma_index + 1U != *any_input_color_index)
        {
            return make_error(ErrorCode::kValidation,
                              "Profile gamma must immediately precede input colour",
                              {{"operation_id", std::string(kProfileGammaOperationId)}});
        }
    }

    std::optional<std::size_t> output_color_index;
    std::optional<std::size_t> any_output_color_index;
    for (std::size_t index = 0; index < recipe.operations.size(); ++index)
    {
        const auto &operation = recipe.operations[index];
        if (operation.id != "ravo.color.output")
        {
            continue;
        }
        if (!any_output_color_index)
            any_output_color_index = index;
        if (!operation.enabled)
            continue;
        if (output_color_index)
        {
            return make_error(ErrorCode::kConflict,
                              "Recipe contains more than one enabled output colour operation",
                              {{"operation_id", operation.id}});
        }
        output_color_index = index;
    }

    std::optional<std::size_t> output_dither_index;
    std::optional<std::size_t> frame_index;
    std::optional<std::size_t> watermark_index;
    std::optional<std::size_t> canvas_index;
    for (std::size_t index = 0; index < recipe.operations.size(); ++index)
    {
        const auto &operation = recipe.operations[index];
        if (operation.id == kOutputDitherOperationId)
        {
            if (output_dither_index)
                return make_error(
                    ErrorCode::kConflict, "Recipe contains more than one Output Dither operation",
                    {{"operation_id", operation.id}, {"reason", "duplicate_output_dither"}});
            output_dither_index = index;
        }
        else if (operation.id == kFrameOperationId)
        {
            if (frame_index)
                return make_error(
                    ErrorCode::kConflict, "Recipe contains more than one Frame",
                    {{"operation_id", operation.id}, {"reason", "duplicate_output_frame"}});
            frame_index = index;
        }
        else if (operation.id == kWatermarkOperationId)
        {
            if (watermark_index)
                return make_error(
                    ErrorCode::kConflict, "Recipe contains more than one Watermark",
                    {{"operation_id", operation.id}, {"reason", "duplicate_watermark"}});
            watermark_index = index;
        }
        else if (operation.id == kCanvasOperationId)
        {
            if (canvas_index)
                return make_error(ErrorCode::kConflict, "Recipe contains more than one Canvas",
                                  {{"operation_id", operation.id}, {"reason", "duplicate_canvas"}});
            canvas_index = index;
        }
    }
    if (output_dither_index)
    {
        std::size_t next = *output_dither_index + 1U;
        if (frame_index)
            next = *frame_index == next ? next + 1U : recipe.operations.size() + 1U;
        if (watermark_index)
            next = *watermark_index == next ? next + 1U : recipe.operations.size() + 1U;
        if (!any_output_color_index || *output_dither_index != *any_output_color_index + 1U ||
            next != recipe.operations.size())
        {
            return make_error(
                ErrorCode::kValidation,
                "Output Dither must follow Output Color and precede only Frame and Watermark",
                {{"operation_id", std::string(kOutputDitherOperationId)},
                 {"reason", "invalid_output_dither_order"}});
        }
    }
    if (frame_index)
    {
        const std::size_t expected =
            output_dither_index ? *output_dither_index + 1U :
                                  any_output_color_index.value_or(recipe.operations.size()) + 1U;
        const std::size_t next = *frame_index + 1U;
        if (!any_output_color_index || *frame_index != expected ||
            (watermark_index ? *watermark_index != next || next + 1U != recipe.operations.size() :
                               next != recipe.operations.size()))
        {
            return make_error(ErrorCode::kValidation,
                              "Frame must follow Output Color or Dither and precede only Watermark",
                              {{"operation_id", std::string(kFrameOperationId)},
                               {"reason", "invalid_output_frame_order"}});
        }
    }
    if (watermark_index)
    {
        const std::size_t expected =
            frame_index         ? *frame_index + 1U :
            output_dither_index ? *output_dither_index + 1U :
                                  any_output_color_index.value_or(recipe.operations.size()) + 1U;
        if (!any_output_color_index || *watermark_index != expected ||
            *watermark_index + 1U != recipe.operations.size())
        {
            return make_error(ErrorCode::kValidation,
                              "Watermark must be final after Output Color, Dither, or Frame",
                              {{"operation_id", std::string(kWatermarkOperationId)},
                               {"reason", "invalid_watermark_order"}});
        }
    }
    if (canvas_index && recipe.operations[*canvas_index].enabled)
    {
        bool composed_geometry_seen = false;
        for (std::size_t index = *canvas_index + 1U; index < recipe.operations.size(); ++index)
        {
            const auto &operation = recipe.operations[index];
            if (!operation.enabled)
                continue;
            if (operation.id == "ravo.geometry.rotate" || operation.id == "ravo.geometry.flip" ||
                operation.id == "ravo.geometry.lens")
            {
                return make_error(ErrorCode::kUnsupported,
                                  "Canvas does not yet compose with later geometry",
                                  {{"operation_id", operation.id},
                                   {"reason", "canvas_later_geometry_unsupported"}});
            }
            if (operation.id == "ravo.geometry.crop" ||
                operation.id == "ravo.geometry.straighten" ||
                operation.id == kPerspectiveOperationId)
            {
                composed_geometry_seen = true;
                continue;
            }
            if (composed_geometry_seen &&
                (operation.mask_id.has_value() || operation.id == kRetouchOperationId))
            {
                return make_error(ErrorCode::kUnsupported,
                                  "Canvas-composed geometry cannot precede another mask consumer",
                                  {{"operation_id", operation.id},
                                   {"reason", "canvas_geometry_later_mask_unsupported"}});
            }
        }
    }

    std::optional<std::size_t> primaries_index;
    for (std::size_t index = 0; index < recipe.operations.size(); ++index)
    {
        const auto &operation = recipe.operations[index];
        if (!operation.enabled || operation.id != kPrimariesOperationId)
        {
            continue;
        }
        if (primaries_index)
        {
            return make_error(ErrorCode::kConflict,
                              "Recipe contains more than one enabled RGB primaries operation",
                              {{"operation_id", operation.id}});
        }
        primaries_index = index;
    }
    if (primaries_index)
    {
        if (!any_input_color_index)
        {
            return make_error(ErrorCode::kValidation,
                              "RGB primaries requires an input colour operation",
                              {{"operation_id", std::string(kPrimariesOperationId)}});
        }
        if (*primaries_index != *any_input_color_index + 1U)
        {
            return make_error(ErrorCode::kValidation,
                              "RGB primaries must immediately follow input colour",
                              {{"operation_id", std::string(kPrimariesOperationId)}});
        }
        const std::size_t expected_end =
            watermark_index     ? *watermark_index + 1U :
            frame_index         ? *frame_index + 1U :
            output_dither_index ? *output_dither_index + 1U :
                                  any_output_color_index.value_or(recipe.operations.size()) + 1U;
        const bool valid_output_tail =
            any_output_color_index && expected_end == recipe.operations.size() &&
            (!output_dither_index || *output_dither_index == *any_output_color_index + 1U) &&
            (!frame_index ||
             *frame_index == (output_dither_index ? *output_dither_index + 1U :
                                                    *any_output_color_index + 1U)) &&
            (!watermark_index ||
             *watermark_index == (frame_index         ? *frame_index + 1U :
                                  output_dither_index ? *output_dither_index + 1U :
                                                        *any_output_color_index + 1U));
        if (!valid_output_tail)
        {
            return make_error(ErrorCode::kValidation,
                              "RGB primaries recipes require output colour as the final operation",
                              {{"operation_id", std::string(kPrimariesOperationId)}});
        }
    }

    std::optional<std::size_t> temperature_index;
    std::optional<TemperatureParams> temperature_params;
    for (std::size_t index = 0; index < recipe.operations.size(); ++index)
    {
        const auto &operation = recipe.operations[index];
        if (!operation.enabled || operation.id != "ravo.color.temperature")
        {
            continue;
        }
        if (temperature_index)
        {
            return make_error(ErrorCode::kConflict,
                              "Recipe contains more than one enabled temperature operation",
                              {{"operation_id", operation.id}});
        }
        auto parsed = temperature_from_parameters(operation.parameters);
        if (!parsed)
        {
            return parsed.error();
        }
        temperature_index = index;
        temperature_params = std::move(parsed).value();
    }
    if (temperature_index)
    {
        for (std::size_t index = 0; index < *temperature_index; ++index)
        {
            const auto &operation = recipe.operations[index];
            if (operation.enabled &&
                (operation.id == "ravo.raw.highlights" || operation.id == "ravo.raw.cacorrect" ||
                 operation.id == "ravo.raw.hotpixels" || operation.id == "ravo.raw.denoise"))
            {
                return make_error(ErrorCode::kValidation,
                                  "Temperature must precede every CFA preprocessing operation",
                                  {{"operation_id", operation.id}});
            }
        }
    }
    if (temperature_params && temperature_params->mode == kTemperatureModeAsShotToReference)
    {
        bool has_explicit_late_adaptation = false;
        for (std::size_t index = *temperature_index + 1U; index < recipe.operations.size(); ++index)
        {
            const auto &operation = recipe.operations[index];
            if (!operation.enabled || operation.id != "ravo.color.channelmixerrgb")
            {
                continue;
            }
            auto mixer = channel_mixer_from_parameters(operation.parameters);
            if (!mixer)
            {
                return mixer.error();
            }
            has_explicit_late_adaptation = mixer.value().adaptation != kChannelMixerAdaptationRgb;
            if (has_explicit_late_adaptation)
            {
                break;
            }
        }
        if (!has_explicit_late_adaptation)
        {
            return make_error(
                ErrorCode::kValidation,
                "As-shot-to-reference temperature requires a later explicit chromatic adaptation",
                {{"operation_id", "ravo.color.temperature"}});
        }
    }
    return {};
}

} // namespace ravo
