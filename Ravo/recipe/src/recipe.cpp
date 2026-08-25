#include "ravo/recipe/recipe.h"

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

#include "ravo/recipe/develop.h"
#include "ravo/recipe/operation.h"

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

[[nodiscard]] Result<Mask> parse_mask(const JsonValue &value, const std::string_view path)
{
    auto object = object_at(value, path);
    if (!object)
    {
        return object.error();
    }
    auto fields = reject_unknown_fields(*object.value(), {"id", "kind", "schema_version"}, path);
    if (!fields)
    {
        return fields.error();
    }
    auto id = required_field(*object.value(), "id", path);
    auto schema_version = required_field(*object.value(), "schema_version", path);
    auto kind = required_field(*object.value(), "kind", path);
    if (!id)
    {
        return id.error();
    }
    if (!schema_version)
    {
        return schema_version.error();
    }
    if (!kind)
    {
        return kind.error();
    }
    auto parsed_id = string_at(*id.value(), std::string(path) + ".id");
    auto parsed_version =
        integer_at(*schema_version.value(), std::string(path) + ".schema_version");
    auto parsed_kind = string_at(*kind.value(), std::string(path) + ".kind");
    if (!parsed_id)
    {
        return parsed_id.error();
    }
    if (!parsed_version)
    {
        return parsed_version.error();
    }
    if (!parsed_kind)
    {
        return parsed_kind.error();
    }
    if (parsed_kind.value() != "all")
    {
        return make_error(ErrorCode::kUnsupported, "Unsupported canonical mask kind",
                          {{"kind", parsed_kind.value()}, {"path", std::string(path)}});
    }
    return Mask{std::move(parsed_id).value(), parsed_version.value(), MaskKind::kAll};
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
        auto mask = parse_mask((*mask_array)[index], "recipe.masks[" + std::to_string(index) + "]");
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
    if (recipe.schema_version == 1)
    {
        return recipe;
    }
    if (recipe.schema_version > 1)
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
        masks.emplace_back(JsonValue::Object{
            {"id", mask.id},
            {"kind", "all"},
            {"schema_version", integer_json(mask.schema_version)},
        });
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
    if (recipe.schema_version != 1)
    {
        return make_error(ErrorCode::kUnsupported, "Unsupported recipe schema version",
                          {{"schema_version", std::to_string(recipe.schema_version)}});
    }
    if (recipe.asset.id.empty() || recipe.asset.input_uri.empty())
    {
        return make_error(ErrorCode::kValidation,
                          "Recipe asset ID and input URI must not be empty");
    }

    std::set<std::string, std::less<>> masks;
    for (const auto &mask : recipe.masks)
    {
        if (mask.id.empty() || mask.schema_version != 1)
        {
            return make_error(ErrorCode::kValidation, "Recipe mask is invalid",
                              {{"mask_id", mask.id}});
        }
        if (!masks.insert(mask.id).second)
        {
            return make_error(ErrorCode::kConflict, "Recipe contains duplicate mask IDs",
                              {{"mask_id", mask.id}});
        }
    }

    std::set<std::string, std::less<>> instances;
    for (const auto &operation : recipe.operations)
    {
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
    return {};
}

} // namespace ravo
