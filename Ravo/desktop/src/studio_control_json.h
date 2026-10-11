#pragma once
#include <algorithm>
#include <charconv>
#include <initializer_list>
#include <string_view>
#include "ravo/foundation/json.h"
namespace ravo::studio_control_internal
{
[[nodiscard]] Result<const JsonValue::Object *> require_object(const JsonValue &, std::string_view);
[[nodiscard]] Result<void> reject_unknown(const JsonValue::Object &,
                                          std::initializer_list<std::string_view>,
                                          std::string_view);
[[nodiscard]] Result<std::string> string_field(const JsonValue::Object &, std::string_view,
                                               std::size_t maximum = 4096U);
[[nodiscard]] Result<double> number_value(const JsonValue &, std::string_view);
template <typename Integer>
[[nodiscard]] Result<Integer> integer_field(const JsonValue::Object &object,
                                            const std::string_view key)
{
    const auto found = object.find(key);
    if (found == object.end() || found->second.number_if() == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Live Studio integer field is missing",
                          {{"field", std::string(key)}});
    }
    const auto &text = found->second.number_if()->text;
    Integer result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    {
        return make_error(ErrorCode::kValidation, "Live Studio integer field is invalid",
                          {{"field", std::string(key)}});
    }
    return result;
}

} // namespace ravo::studio_control_internal
