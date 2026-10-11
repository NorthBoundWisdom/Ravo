#pragma once

#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ravo/foundation/error.h"

namespace ravo
{

struct JsonNumber
{
    std::string text;

    [[nodiscard]] bool operator==(const JsonNumber &) const = default;
};

class JsonValue
{
public:
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue, std::less<>>;

    enum class Kind
    {
        kNull,
        kBoolean,
        kNumber,
        kString,
        kArray,
        kObject,
    };

    JsonValue();
    JsonValue(std::nullptr_t);
    JsonValue(bool value);
    JsonValue(JsonNumber value);
    JsonValue(std::string value);
    JsonValue(const char *value);
    JsonValue(Array value);
    JsonValue(Object value);

    [[nodiscard]] static JsonValue number(std::string text);

    [[nodiscard]] Kind kind() const noexcept;
    [[nodiscard]] bool is_null() const noexcept;
    [[nodiscard]] const bool *boolean_if() const noexcept;
    [[nodiscard]] const JsonNumber *number_if() const noexcept;
    [[nodiscard]] const std::string *string_if() const noexcept;
    [[nodiscard]] const Array *array_if() const noexcept;
    [[nodiscard]] const Object *object_if() const noexcept;
    [[nodiscard]] const JsonValue *find(std::string_view key) const;

private:
    std::variant<std::nullptr_t, bool, JsonNumber, std::string, Array, Object> value_;
};

[[nodiscard]] Result<JsonValue> parse_json(std::string_view text);
[[nodiscard]] std::string serialize_json(const JsonValue &value);
[[nodiscard]] Result<JsonValue> json_real(double value);

} // namespace ravo
