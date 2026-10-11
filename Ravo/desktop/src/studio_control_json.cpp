#include "studio_control_json.h"
#include <cmath>
#include "ravo/foundation/parse_number.h"
namespace ravo::studio_control_internal
{
[[nodiscard]] Result<const JsonValue::Object *> require_object(const JsonValue &value,
                                                               const std::string_view location)
{
    const auto *object = value.object_if();
    if (object == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Live Studio value must be an object",
                          {{"location", std::string(location)}});
    }
    return object;
}

[[nodiscard]] Result<void> reject_unknown(const JsonValue::Object &object,
                                          const std::initializer_list<std::string_view> allowed,
                                          const std::string_view location)
{
    for (const auto &[key, value] : object)
    {
        static_cast<void>(value);
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
        {
            return make_error(ErrorCode::kValidation, "Unknown live Studio request field",
                              {{"field", key}, {"location", std::string(location)}});
        }
    }
    return {};
}

[[nodiscard]] Result<std::string>
string_field(const JsonValue::Object &object, const std::string_view key, const std::size_t maximum)
{
    const auto found = object.find(key);
    if (found == object.end() || found->second.string_if() == nullptr ||
        found->second.string_if()->empty() || found->second.string_if()->size() > maximum)
    {
        return make_error(ErrorCode::kValidation, "Live Studio string field is invalid",
                          {{"field", std::string(key)}});
    }
    return *found->second.string_if();
}

[[nodiscard]] Result<double> number_value(const JsonValue &value, const std::string_view name)
{
    const auto *number = value.number_if();
    if (number == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Live Develop value must be numeric",
                          {{"name", std::string(name)}});
    }
    double parsed = 0.;
    if (!parse_ascii_double(number->text, parsed))
    {
        return make_error(ErrorCode::kValidation, "Live Develop value must be finite",
                          {{"name", std::string(name)}});
    }
    return parsed;
}

} // namespace ravo::studio_control_internal
