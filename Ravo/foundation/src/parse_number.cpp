#include "ravo/foundation/parse_number.h"

#include <cmath>
#include <locale>
#include <sstream>
#include <string>

namespace ravo
{
namespace
{

[[nodiscard]] bool is_allowed_ascii_double_char(const unsigned char character) noexcept
{
    return character == '+' || character == '-' || character == '.' || character == 'e' ||
           character == 'E' || (character >= '0' && character <= '9');
}

} // namespace

bool parse_ascii_double(const std::string_view text, double &out) noexcept
{
    if (text.empty() || text.size() > kMaxAsciiDoubleTokenBytes)
        return false;

    // Reject whitespace, non-ASCII, and non-decimal forms (hex floats, locale
    // commas) before classic-locale extraction.
    for (const char raw : text)
    {
        if (!is_allowed_ascii_double_char(static_cast<unsigned char>(raw)))
            return false;
    }

    try
    {
        std::istringstream stream{std::string(text)};
        stream.imbue(std::locale::classic());
        stream >> std::noskipws;
        double parsed = 0.0;
        if (!(stream >> parsed) || stream.peek() != std::char_traits<char>::eof() ||
            !std::isfinite(parsed))
            return false;
        out = parsed;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

} // namespace ravo
