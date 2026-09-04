#pragma once

#include <cstddef>
#include <string_view>

namespace ravo
{

inline constexpr std::size_t kMaxAsciiDoubleTokenBytes = 64U;

// Locale-independent ASCII decimal parser for CLI / JSON-like tokens.
// Requires complete consumption of `text` (bounded length), a '.' decimal separator,
// and a finite value. Independent of the process LC_NUMERIC locale (comma locales must not
// reinterpret '.' or accept ','). Prefer this over floating std::from_chars
// (unavailable on Apple libc++) and bare std::strtod (locale-sensitive).
[[nodiscard]] bool parse_ascii_double(std::string_view text, double &out) noexcept;

} // namespace ravo
