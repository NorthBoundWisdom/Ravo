#include "ravo/foundation/json.h"
#include <array>
#include <charconv>
#include <cmath>
#include <limits>

namespace ravo
{
Result<JsonValue> json_real(const double value)
{
    if (!std::isfinite(value))
        return make_error(ErrorCode::kInvalidArgument, "JSON number must be finite");
    std::array<char, 64> buffer{};
    const auto result =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                      std::chars_format::general, std::numeric_limits<double>::max_digits10);
    if (result.ec != std::errc{})
        return make_error(ErrorCode::kInternal, "Cannot serialize JSON number");
    return JsonValue::number(std::string(buffer.data(), result.ptr));
}
} // namespace ravo
