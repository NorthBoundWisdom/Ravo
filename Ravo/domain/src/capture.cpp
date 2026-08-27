#include "ravo/domain/types.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace ravo
{
namespace
{

[[nodiscard]] TaskError capture_error(const std::string_view message, const std::string_view reason,
                                      const std::string_view field = {},
                                      const std::string_view path = {},
                                      const std::string_view value = {})
{
    std::map<std::string, std::string, std::less<>> context{{"reason", std::string(reason)}};
    if (!field.empty())
    {
        context.emplace("field", std::string(field));
    }
    if (!path.empty())
    {
        context.emplace("path", std::string(path));
    }
    if (!value.empty())
    {
        context.emplace("value", std::string(value));
    }
    return make_error(ErrorCode::kValidation, std::string(message), std::move(context));
}

[[nodiscard]] bool is_ascii_digit(const char ch) noexcept
{
    return ch >= '0' && ch <= '9';
}

[[nodiscard]] int digit_value(const char ch) noexcept
{
    return ch - '0';
}

[[nodiscard]] bool is_leap_year(const int year) noexcept
{
    if (year % 4 != 0)
    {
        return false;
    }
    if (year % 100 != 0)
    {
        return true;
    }
    return year % 400 == 0;
}

[[nodiscard]] int days_in_month(const int year, const int month) noexcept
{
    static constexpr int kDays[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && is_leap_year(year))
    {
        return 29;
    }
    return kDays[month];
}

[[nodiscard]] std::uint64_t gcd_u64(std::uint64_t left, std::uint64_t right) noexcept
{
    while (right != 0U)
    {
        const std::uint64_t remainder = left % right;
        left = right;
        right = remainder;
    }
    return left == 0U ? 1U : left;
}

[[nodiscard]] Result<void> parse_local_exif(const std::string_view text,
                                            const std::string_view field,
                                            const std::string_view path)
{
    if (text.size() != kCaptureLocalExifLength)
    {
        return capture_error("Capture local time must be exactly YYYY:MM:DD HH:MM:SS",
                             "invalid_capture_datetime", field, path, text);
    }
    for (const std::size_t index : {0U, 1U, 2U, 3U, 5U, 6U, 8U, 9U, 11U, 12U, 14U, 15U, 17U, 18U})
    {
        if (!is_ascii_digit(text[index]))
        {
            return capture_error("Capture local time must use ASCII digits",
                                 "invalid_capture_datetime", field, path, std::string(text));
        }
    }
    if (text[4] != ':' || text[7] != ':' || text[10] != ' ' || text[13] != ':' || text[16] != ':')
    {
        return capture_error("Capture local time must use Exif separators",
                             "invalid_capture_datetime", field, path, std::string(text));
    }
    const int year = digit_value(text[0]) * 1000 + digit_value(text[1]) * 100 +
                     digit_value(text[2]) * 10 + digit_value(text[3]);
    const int month = digit_value(text[5]) * 10 + digit_value(text[6]);
    const int day = digit_value(text[8]) * 10 + digit_value(text[9]);
    const int hour = digit_value(text[11]) * 10 + digit_value(text[12]);
    const int minute = digit_value(text[14]) * 10 + digit_value(text[15]);
    const int second = digit_value(text[17]) * 10 + digit_value(text[18]);
    if (year < 1 || year > 9999 || month < 1 || month > 12 || day < 1 ||
        day > days_in_month(year, month) || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59)
    {
        return capture_error("Capture local time is outside the Gregorian calendar",
                             "invalid_capture_datetime", field, path, std::string(text));
    }
    return {};
}

[[nodiscard]] Result<void> parse_subsecond(const std::string_view text,
                                           const std::string_view field,
                                           const std::string_view path)
{
    if (text.size() < kCaptureSubsecondDigitsMin || text.size() > kCaptureSubsecondDigitsMax)
    {
        return capture_error("Capture subsecond must be 1 to 9 ASCII digits",
                             "invalid_capture_subsecond", field, path, std::string(text));
    }
    if (!std::all_of(text.begin(), text.end(), is_ascii_digit))
    {
        return capture_error("Capture subsecond must be ASCII digits only",
                             "invalid_capture_subsecond", field, path, std::string(text));
    }
    return {};
}

[[nodiscard]] std::string two_digits(const int value)
{
    std::string text = "00";
    text[0] = static_cast<char>('0' + (value / 10));
    text[1] = static_cast<char>('0' + (value % 10));
    return text;
}

} // namespace

Result<void> validate_capture_datetime(const CaptureDateTime &value)
{
    auto local = parse_local_exif(value.local_exif, "captured_datetime", {});
    if (!local)
    {
        return local.error();
    }
    if (value.subsecond_digits)
    {
        auto fraction = parse_subsecond(*value.subsecond_digits, "captured_subsecond_digits", {});
        if (!fraction)
        {
            return fraction.error();
        }
    }
    if (value.utc_offset_minutes)
    {
        const auto minutes = *value.utc_offset_minutes;
        if (minutes < kCaptureUtcOffsetMinutesMin || minutes > kCaptureUtcOffsetMinutesMax)
        {
            return capture_error("Capture UTC offset is outside -14:00..+14:00",
                                 "invalid_capture_utc_offset", "captured_utc_offset_minutes");
        }
        const int abs_minutes = minutes < 0 ? -minutes : minutes;
        if (abs_minutes / 60 == 14 && abs_minutes % 60 != 0)
        {
            return capture_error("Capture UTC offset minutes must be zero at +/-14:00",
                                 "invalid_capture_utc_offset", "captured_utc_offset_minutes");
        }
    }
    return {};
}

Result<void> validate_capture_location(const CaptureLocation &value)
{
    if (value.latitude_e6 < kCaptureLatitudeE6Min || value.latitude_e6 > kCaptureLatitudeE6Max)
    {
        return capture_error("Capture latitude is outside +/-90 degrees",
                             "invalid_capture_latitude", "gps_latitude_e6");
    }
    if (value.longitude_e6 < kCaptureLongitudeE6Min || value.longitude_e6 > kCaptureLongitudeE6Max)
    {
        return capture_error("Capture longitude is outside +/-180 degrees",
                             "invalid_capture_longitude", "gps_longitude_e6");
    }
    if (value.altitude)
    {
        const auto maximum = value.altitude->reference == CaptureAltitudeReference::kBelowSeaLevel ?
                                 kCaptureAltitudeBelowSeaLevelMmMax :
                                 kCaptureAltitudeAboveSeaLevelMmMax;
        if (value.altitude->magnitude_mm > maximum)
        {
            return capture_error("Capture altitude is outside its legal bound",
                                 "invalid_capture_altitude", "gps_altitude_magnitude_mm");
        }
        if (value.altitude->reference != CaptureAltitudeReference::kAboveSeaLevel &&
            value.altitude->reference != CaptureAltitudeReference::kBelowSeaLevel)
        {
            return capture_error("Capture altitude reference must be above or below sea level",
                                 "invalid_capture_altitude_ref", "gps_altitude_ref");
        }
    }
    return {};
}

bool capture_metadata_has_values(const CaptureMetadata &capture) noexcept
{
    return capture.camera_make || capture.camera_model || capture.iso || capture.aperture ||
           capture.focal_length_mm || capture.shutter_s || capture.captured_unix_s ||
           capture.captured_datetime || capture.location;
}

Result<void> validate_capture_metadata(const CaptureMetadata &capture)
{
    if (capture.captured_datetime)
    {
        auto valid = validate_capture_datetime(*capture.captured_datetime);
        if (!valid)
        {
            return valid.error();
        }
    }
    if (capture.location)
    {
        auto valid = validate_capture_location(*capture.location);
        if (!valid)
        {
            return valid.error();
        }
    }
    return {};
}

std::array<ExportUnsignedRational, 3> capture_microdegrees_to_dms(const std::int32_t e6) noexcept
{
    const std::int64_t abs_e6 =
        e6 < 0 ? -static_cast<std::int64_t>(e6) : static_cast<std::int64_t>(e6);
    const auto degrees = static_cast<std::uint32_t>(abs_e6 / 1000000);
    const auto rem = static_cast<std::uint32_t>(abs_e6 % 1000000);
    const std::uint64_t minute_e6 = static_cast<std::uint64_t>(rem) * 60U;
    const auto minutes = static_cast<std::uint32_t>(minute_e6 / 1000000U);
    const std::uint64_t second_num = (minute_e6 % 1000000U) * 60U;
    const std::uint64_t second_den = 1000000U;
    const std::uint64_t divisor = gcd_u64(second_num, second_den);
    std::array<ExportUnsignedRational, 3> dms{};
    dms[0] = {degrees, 1U};
    dms[1] = {minutes, 1U};
    dms[2] = {static_cast<std::uint32_t>(second_num / divisor),
              static_cast<std::uint32_t>(second_den / divisor)};
    return dms;
}

ExportUnsignedRational capture_altitude_mm_to_rational(const std::uint32_t magnitude_mm) noexcept
{
    const std::uint64_t num = magnitude_mm;
    const std::uint64_t den = 1000U;
    const std::uint64_t divisor = gcd_u64(num, den);
    return {static_cast<std::uint32_t>(num / divisor), static_cast<std::uint32_t>(den / divisor)};
}

std::string format_capture_utc_offset(const std::int32_t utc_offset_minutes)
{
    const bool negative = utc_offset_minutes < 0;
    const int abs_minutes = negative ? -utc_offset_minutes : utc_offset_minutes;
    std::string text;
    text.push_back(negative ? '-' : '+');
    text += two_digits(abs_minutes / 60);
    text.push_back(':');
    text += two_digits(abs_minutes % 60);
    return text;
}

std::string format_capture_datetime_iso(const CaptureDateTime &value)
{
    std::string text;
    text.reserve(32U);
    text.append(value.local_exif, 0, 4);
    text.push_back('-');
    text.append(value.local_exif, 5, 2);
    text.push_back('-');
    text.append(value.local_exif, 8, 2);
    text.push_back('T');
    text.append(value.local_exif, 11, 8);
    if (value.subsecond_digits)
    {
        text.push_back('.');
        text += *value.subsecond_digits;
    }
    if (value.utc_offset_minutes)
    {
        text += format_capture_utc_offset(*value.utc_offset_minutes);
    }
    return text;
}

std::string format_scaled_decimal(const std::int64_t value, const int scale)
{
    const bool negative = value < 0;
    std::uint64_t abs_value = static_cast<std::uint64_t>(negative ? -value : value);
    std::uint64_t factor = 1;
    for (int index = 0; index < scale; ++index)
    {
        factor *= 10U;
    }
    const std::uint64_t whole = abs_value / factor;
    std::uint64_t fraction = abs_value % factor;
    std::string text = negative ? "-" : "";
    text += std::to_string(whole);
    if (fraction == 0U)
    {
        return text;
    }
    std::string digits(static_cast<std::size_t>(scale), '0');
    for (int index = scale - 1; index >= 0; --index)
    {
        digits[static_cast<std::size_t>(index)] = static_cast<char>('0' + (fraction % 10U));
        fraction /= 10U;
    }
    while (!digits.empty() && digits.back() == '0')
    {
        digits.pop_back();
    }
    text.push_back('.');
    text += digits;
    return text;
}

std::string format_gps_xmp_coordinate(const std::int32_t e6, const char positive_ref,
                                      const char negative_ref)
{
    const char ref = e6 < 0 ? negative_ref : positive_ref;
    const std::int64_t abs_e6 =
        e6 < 0 ? -static_cast<std::int64_t>(e6) : static_cast<std::int64_t>(e6);
    const auto degrees = static_cast<std::uint32_t>(abs_e6 / 1000000);
    const auto rem = static_cast<std::uint32_t>(abs_e6 % 1000000);
    const std::uint64_t minute_e6 = static_cast<std::uint64_t>(rem) * 60U;
    const auto minutes = static_cast<std::uint32_t>(minute_e6 / 1000000U);
    const auto minute_frac = static_cast<std::int64_t>(minute_e6 % 1000000U);
    std::string text = std::to_string(degrees);
    text.push_back(',');
    text += format_scaled_decimal(static_cast<std::int64_t>(minutes) * 1000000 + minute_frac, 6);
    text.push_back(ref);
    return text;
}

} // namespace ravo
