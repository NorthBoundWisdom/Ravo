#include "ravo/foundation/json.h"

#include <charconv>
#include <cctype>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace ravo
{

JsonValue::JsonValue()
    : value_(nullptr)
{
}
JsonValue::JsonValue(const std::nullptr_t value)
    : value_(value)
{
}
JsonValue::JsonValue(const bool value)
    : value_(value)
{
}
JsonValue::JsonValue(JsonNumber value)
    : value_(std::move(value))
{
}
JsonValue::JsonValue(std::string value)
    : value_(std::move(value))
{
}
JsonValue::JsonValue(const char *value)
    : value_(std::string(value))
{
}
JsonValue::JsonValue(Array value)
    : value_(std::move(value))
{
}
JsonValue::JsonValue(Object value)
    : value_(std::move(value))
{
}

JsonValue JsonValue::number(std::string text)
{
    return JsonValue{JsonNumber{std::move(text)}};
}

JsonValue::Kind JsonValue::kind() const noexcept
{
    switch (value_.index())
    {
    case 0:
        return Kind::kNull;
    case 1:
        return Kind::kBoolean;
    case 2:
        return Kind::kNumber;
    case 3:
        return Kind::kString;
    case 4:
        return Kind::kArray;
    case 5:
        return Kind::kObject;
    default:
        return Kind::kNull;
    }
}

bool JsonValue::is_null() const noexcept
{
    return std::holds_alternative<std::nullptr_t>(value_);
}

const bool *JsonValue::boolean_if() const noexcept
{
    return std::get_if<bool>(&value_);
}

const JsonNumber *JsonValue::number_if() const noexcept
{
    return std::get_if<JsonNumber>(&value_);
}

const std::string *JsonValue::string_if() const noexcept
{
    return std::get_if<std::string>(&value_);
}

const JsonValue::Array *JsonValue::array_if() const noexcept
{
    return std::get_if<Array>(&value_);
}

const JsonValue::Object *JsonValue::object_if() const noexcept
{
    return std::get_if<Object>(&value_);
}

const JsonValue *JsonValue::find(const std::string_view key) const
{
    const auto *object = object_if();
    if (object == nullptr)
    {
        return nullptr;
    }
    const auto iterator = object->find(std::string(key));
    return iterator == object->end() ? nullptr : &iterator->second;
}

namespace
{

class Parser
{
public:
    explicit Parser(const std::string_view text)
        : text_(text)
    {
    }

    [[nodiscard]] Result<JsonValue> parse()
    {
        skip_whitespace();
        auto value = parse_value();
        if (!value)
        {
            return value.error();
        }
        skip_whitespace();
        if (!at_end())
        {
            return error("Unexpected trailing data after JSON value");
        }
        return std::move(value).value();
    }

private:
    [[nodiscard]] Result<JsonValue> parse_value()
    {
        if (at_end())
        {
            return error("Expected a JSON value");
        }

        switch (peek())
        {
        case 'n':
            return parse_literal("null", JsonValue{});
        case 't':
            return parse_literal("true", JsonValue{true});
        case 'f':
            return parse_literal("false", JsonValue{false});
        case '"':
            return parse_string_value();
        case '[':
            return parse_array();
        case '{':
            return parse_object();
        default:
            if (peek() == '-' || is_digit(peek()))
            {
                return parse_number();
            }
            return error("Expected a JSON value");
        }
    }

    [[nodiscard]] Result<JsonValue> parse_literal(const std::string_view literal, JsonValue value)
    {
        if (text_.substr(position_, literal.size()) != literal)
        {
            return error("Invalid JSON literal");
        }
        advance(literal.size());
        return value;
    }

    [[nodiscard]] Result<JsonValue> parse_string_value()
    {
        auto value = parse_string();
        if (!value)
        {
            return value.error();
        }
        return JsonValue{std::move(value).value()};
    }

    [[nodiscard]] Result<std::string> parse_string()
    {
        if (!consume('"'))
        {
            return error("Expected string opening quote");
        }

        std::string result;
        while (!at_end())
        {
            const char character = take();
            if (character == '"')
            {
                return result;
            }
            if (static_cast<unsigned char>(character) < 0x20U)
            {
                return error("Control character is not allowed in a JSON string");
            }
            if (character != '\\')
            {
                result.push_back(character);
                continue;
            }
            if (at_end())
            {
                return error("Unterminated JSON escape sequence");
            }
            switch (take())
            {
            case '"':
                result.push_back('"');
                break;
            case '\\':
                result.push_back('\\');
                break;
            case '/':
                result.push_back('/');
                break;
            case 'b':
                result.push_back('\b');
                break;
            case 'f':
                result.push_back('\f');
                break;
            case 'n':
                result.push_back('\n');
                break;
            case 'r':
                result.push_back('\r');
                break;
            case 't':
                result.push_back('\t');
                break;
            case 'u':
            {
                auto scalar = parse_unicode_escape();
                if (!scalar)
                {
                    return scalar.error();
                }
                append_utf8(result, scalar.value());
                break;
            }
            default:
                return error("Unknown JSON escape sequence");
            }
        }
        return error("Unterminated JSON string");
    }

    [[nodiscard]] Result<std::uint32_t> parse_unicode_escape()
    {
        auto unit = parse_hex_quad();
        if (!unit)
        {
            return unit.error();
        }
        const std::uint32_t first = unit.value();
        if (first < 0xD800U || first > 0xDFFFU)
        {
            return first;
        }
        if (first > 0xDBFFU)
        {
            return error("JSON string contains an unpaired low surrogate");
        }
        if (!consume('\\') || !consume('u'))
        {
            return error("JSON string contains an unpaired high surrogate");
        }
        auto low = parse_hex_quad();
        if (!low)
        {
            return low.error();
        }
        if (low.value() < 0xDC00U || low.value() > 0xDFFFU)
        {
            return error("JSON string contains an invalid surrogate pair");
        }
        return 0x10000U + ((first - 0xD800U) << 10U) + (low.value() - 0xDC00U);
    }

    [[nodiscard]] Result<std::uint32_t> parse_hex_quad()
    {
        if (remaining() < 4U)
        {
            return error("Incomplete JSON unicode escape");
        }
        std::uint32_t value = 0;
        for (int index = 0; index < 4; ++index)
        {
            const char character = take();
            value <<= 4U;
            if (character >= '0' && character <= '9')
            {
                value += static_cast<std::uint32_t>(character - '0');
            }
            else if (character >= 'a' && character <= 'f')
            {
                value += static_cast<std::uint32_t>(character - 'a' + 10);
            }
            else if (character >= 'A' && character <= 'F')
            {
                value += static_cast<std::uint32_t>(character - 'A' + 10);
            }
            else
            {
                return error("Invalid hexadecimal digit in JSON unicode escape");
            }
        }
        return value;
    }

    [[nodiscard]] Result<JsonValue> parse_number()
    {
        const std::size_t start = position_;
        consume('-');
        if (consume('0'))
        {
            if (!at_end() && is_digit(peek()))
            {
                return error("JSON numbers cannot contain a leading zero");
            }
        }
        else
        {
            if (at_end() || !is_digit(peek()))
            {
                return error("JSON number is missing an integer part");
            }
            while (!at_end() && is_digit(peek()))
            {
                take();
            }
        }
        if (consume('.'))
        {
            if (at_end() || !is_digit(peek()))
            {
                return error("JSON number is missing digits after decimal point");
            }
            while (!at_end() && is_digit(peek()))
            {
                take();
            }
        }
        if (consume('e') || consume('E'))
        {
            consume('+');
            consume('-');
            if (at_end() || !is_digit(peek()))
            {
                return error("JSON number is missing exponent digits");
            }
            while (!at_end() && is_digit(peek()))
            {
                take();
            }
        }
        return JsonValue::number(std::string(text_.substr(start, position_ - start)));
    }

    [[nodiscard]] Result<JsonValue> parse_array()
    {
        consume('[');
        skip_whitespace();
        JsonValue::Array array;
        if (consume(']'))
        {
            return JsonValue{std::move(array)};
        }
        while (true)
        {
            auto value = parse_value();
            if (!value)
            {
                return value.error();
            }
            array.push_back(std::move(value).value());
            skip_whitespace();
            if (consume(']'))
            {
                return JsonValue{std::move(array)};
            }
            if (!consume(','))
            {
                return error("Expected ',' or ']' in JSON array");
            }
            skip_whitespace();
        }
    }

    [[nodiscard]] Result<JsonValue> parse_object()
    {
        consume('{');
        skip_whitespace();
        JsonValue::Object object;
        if (consume('}'))
        {
            return JsonValue{std::move(object)};
        }
        while (true)
        {
            if (at_end() || peek() != '"')
            {
                return error("Expected a JSON object key");
            }
            auto key = parse_string();
            if (!key)
            {
                return key.error();
            }
            skip_whitespace();
            if (!consume(':'))
            {
                return error("Expected ':' after JSON object key");
            }
            skip_whitespace();
            auto value = parse_value();
            if (!value)
            {
                return value.error();
            }
            const auto [iterator, inserted] =
                object.emplace(std::move(key).value(), std::move(value).value());
            static_cast<void>(iterator);
            if (!inserted)
            {
                return error("Duplicate JSON object key");
            }
            skip_whitespace();
            if (consume('}'))
            {
                return JsonValue{std::move(object)};
            }
            if (!consume(','))
            {
                return error("Expected ',' or '}' in JSON object");
            }
            skip_whitespace();
        }
    }

    [[nodiscard]] TaskError error(const std::string_view message) const
    {
        return make_error(ErrorCode::kValidation, std::string(message),
                          {{"column", std::to_string(column_)},
                           {"line", std::to_string(line_)},
                           {"offset", std::to_string(position_)}});
    }

    [[nodiscard]] bool at_end() const noexcept
    {
        return position_ >= text_.size();
    }
    [[nodiscard]] std::size_t remaining() const noexcept
    {
        return text_.size() - position_;
    }
    [[nodiscard]] char peek() const noexcept
    {
        return text_[position_];
    }

    char take()
    {
        const char character = text_[position_++];
        if (character == '\n')
        {
            ++line_;
            column_ = 1;
        }
        else
        {
            ++column_;
        }
        return character;
    }

    void advance(const std::size_t length)
    {
        for (std::size_t index = 0; index < length; ++index)
        {
            take();
        }
    }

    bool consume(const char expected)
    {
        if (at_end() || peek() != expected)
        {
            return false;
        }
        take();
        return true;
    }

    void skip_whitespace()
    {
        while (!at_end())
        {
            const char character = peek();
            if (character != ' ' && character != '\n' && character != '\r' && character != '\t')
            {
                return;
            }
            take();
        }
    }

    [[nodiscard]] static bool is_digit(const char character) noexcept
    {
        return character >= '0' && character <= '9';
    }

    static void append_utf8(std::string &destination, const std::uint32_t scalar)
    {
        if (scalar <= 0x7FU)
        {
            destination.push_back(static_cast<char>(scalar));
        }
        else if (scalar <= 0x7FFU)
        {
            destination.push_back(static_cast<char>(0xC0U | (scalar >> 6U)));
            destination.push_back(static_cast<char>(0x80U | (scalar & 0x3FU)));
        }
        else if (scalar <= 0xFFFFU)
        {
            destination.push_back(static_cast<char>(0xE0U | (scalar >> 12U)));
            destination.push_back(static_cast<char>(0x80U | ((scalar >> 6U) & 0x3FU)));
            destination.push_back(static_cast<char>(0x80U | (scalar & 0x3FU)));
        }
        else
        {
            destination.push_back(static_cast<char>(0xF0U | (scalar >> 18U)));
            destination.push_back(static_cast<char>(0x80U | ((scalar >> 12U) & 0x3FU)));
            destination.push_back(static_cast<char>(0x80U | ((scalar >> 6U) & 0x3FU)));
            destination.push_back(static_cast<char>(0x80U | (scalar & 0x3FU)));
        }
    }

    std::string_view text_;
    std::size_t position_ = 0;
    std::size_t line_ = 1;
    std::size_t column_ = 1;
};

void append_escaped_string(std::string &destination, const std::string_view value)
{
    destination.push_back('"');
    constexpr char hex[] = "0123456789abcdef";
    for (const char raw : value)
    {
        const auto character = static_cast<unsigned char>(raw);
        switch (character)
        {
        case '"':
            destination += "\\\"";
            break;
        case '\\':
            destination += "\\\\";
            break;
        case '\b':
            destination += "\\b";
            break;
        case '\f':
            destination += "\\f";
            break;
        case '\n':
            destination += "\\n";
            break;
        case '\r':
            destination += "\\r";
            break;
        case '\t':
            destination += "\\t";
            break;
        default:
            if (character < 0x20U)
            {
                destination += "\\u00";
                destination.push_back(hex[(character >> 4U) & 0x0FU]);
                destination.push_back(hex[character & 0x0FU]);
            }
            else
            {
                destination.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    destination.push_back('"');
}

void append_serialized(std::string &destination, const JsonValue &value)
{
    switch (value.kind())
    {
    case JsonValue::Kind::kNull:
        destination += "null";
        return;
    case JsonValue::Kind::kBoolean:
        destination += *value.boolean_if() ? "true" : "false";
        return;
    case JsonValue::Kind::kNumber:
        destination += value.number_if()->text;
        return;
    case JsonValue::Kind::kString:
        append_escaped_string(destination, *value.string_if());
        return;
    case JsonValue::Kind::kArray:
    {
        destination.push_back('[');
        bool first = true;
        for (const auto &element : *value.array_if())
        {
            if (!first)
            {
                destination.push_back(',');
            }
            append_serialized(destination, element);
            first = false;
        }
        destination.push_back(']');
        return;
    }
    case JsonValue::Kind::kObject:
    {
        destination.push_back('{');
        bool first = true;
        for (const auto &[key, child] : *value.object_if())
        {
            if (!first)
            {
                destination.push_back(',');
            }
            append_escaped_string(destination, key);
            destination.push_back(':');
            append_serialized(destination, child);
            first = false;
        }
        destination.push_back('}');
        return;
    }
    }
}

} // namespace

Result<JsonValue> parse_json(const std::string_view text)
{
    return Parser{text}.parse();
}

std::string serialize_json(const JsonValue &value)
{
    std::string result;
    append_serialized(result, value);
    return result;
}

} // namespace ravo
