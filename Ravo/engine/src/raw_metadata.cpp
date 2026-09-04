#include "raw_pipeline.h"

#include "image_ops.h"
#include "canvas_frame.h"
#include "color_zones.h"
#include "mask_evaluator.h"
#include "monochrome.h"
#include "perspective_transform.h"
#include "recursive_gaussian.h"
#include "retouch.h"

#include <algorithm>
#include <cctype>
#include <array>
#include <cmath>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QString>
#include <QUrl>

#include <exception>
#include <exiv2/exiv2.hpp>
#include <zlib.h>
#include <libraw/libraw.h>

#include "ravo/recipe/color_checker.h"
#include "ravo/recipe/color_harmonizer.h"
#include "ravo/recipe/primaries.h"
#include "ravo/recipe/profile_gamma.h"

#include "color_reconstruction.h"
#include "dehaze.h"
#include "dng_opcodes.h"
#include "bayer_demosaic.h"
#include "sharpen.h"
#include "split_toning.h"
#include "texture.h"
#include "xtrans_demosaic.h"

#include "raw_pipeline_internal.h"

namespace ravo
{
using namespace raw_pipeline_internal;

namespace
{

[[nodiscard]] TaskError capture_read_error(const std::string_view message,
                                           const std::string_view reason,
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

[[nodiscard]] std::string sanitize_error_value(const std::string_view raw,
                                               const std::size_t maximum = 64U)
{
    if (raw.empty() || raw.size() > maximum)
    {
        return {};
    }
    for (const char raw_character : raw)
    {
        const auto ch = static_cast<unsigned char>(raw_character);
        if (ch < 0x20U || ch > 0x7EU)
        {
            return {};
        }
    }
    return std::string(raw);
}

[[nodiscard]] std::map<std::string, std::string, std::less<>>
capture_source_context(const std::string_view input_uri, const std::string_view reason = {},
                       const std::string_view detail = {})
{
    std::map<std::string, std::string, std::less<>> context;
    if (!reason.empty())
    {
        context.emplace("reason", std::string(reason));
    }
    const auto safe_uri = sanitize_error_value(input_uri, 512U);
    if (!safe_uri.empty())
    {
        context.emplace("input_uri", safe_uri);
    }
    const auto safe_detail = sanitize_error_value(detail, 256U);
    if (!safe_detail.empty())
    {
        context.emplace("detail", safe_detail);
    }
    return context;
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

struct EngineUnsignedRational
{
    std::uint32_t numerator = 0;
    std::uint32_t denominator = 1;
};

struct UInt128
{
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;
};

[[nodiscard]] UInt128 mul_u64_u64(const std::uint64_t left, const std::uint64_t right) noexcept
{
    const std::uint64_t left_lo = left & 0xffffffffU;
    const std::uint64_t left_hi = left >> 32U;
    const std::uint64_t right_lo = right & 0xffffffffU;
    const std::uint64_t right_hi = right >> 32U;
    const std::uint64_t p0 = left_lo * right_lo;
    const std::uint64_t p1 = left_lo * right_hi;
    const std::uint64_t p2 = left_hi * right_lo;
    const std::uint64_t p3 = left_hi * right_hi;
    const std::uint64_t mid = (p0 >> 32U) + (p1 & 0xffffffffU) + (p2 & 0xffffffffU);
    UInt128 result;
    result.lo = (p0 & 0xffffffffU) | (mid << 32U);
    result.hi = p3 + (p1 >> 32U) + (p2 >> 32U) + (mid >> 32U);
    return result;
}

[[nodiscard]] bool add_u128_checked(const UInt128 left, const UInt128 right, UInt128 &out) noexcept
{
    out.lo = left.lo + right.lo;
    const std::uint64_t carry = out.lo < left.lo ? 1U : 0U;
    if (right.hi > std::numeric_limits<std::uint64_t>::max() - left.hi)
    {
        return false;
    }
    const std::uint64_t hi = left.hi + right.hi;
    if (carry != 0U && hi == std::numeric_limits<std::uint64_t>::max())
    {
        return false;
    }
    out.hi = hi + carry;
    return true;
}

[[nodiscard]] bool mul_u128_u64(const UInt128 left, const std::uint64_t right,
                                UInt128 &out) noexcept
{
    const UInt128 lo = mul_u64_u64(left.lo, right);
    const UInt128 hi = mul_u64_u64(left.hi, right);
    if (hi.hi != 0U)
    {
        return false;
    }
    out.lo = lo.lo;
    if (lo.hi > std::numeric_limits<std::uint64_t>::max() - hi.lo)
    {
        return false;
    }
    out.hi = lo.hi + hi.lo;
    return true;
}

[[nodiscard]] int cmp_u128(const UInt128 left, const UInt128 right) noexcept
{
    if (left.hi != right.hi)
    {
        return left.hi > right.hi ? 1 : -1;
    }
    if (left.lo != right.lo)
    {
        return left.lo > right.lo ? 1 : -1;
    }
    return 0;
}

[[nodiscard]] int bit_of128(const UInt128 value, const int index) noexcept
{
    if (index >= 64)
    {
        return static_cast<int>((value.hi >> (index - 64)) & 1U);
    }
    return static_cast<int>((value.lo >> index) & 1U);
}

void set_bit128(UInt128 &value, const int index) noexcept
{
    if (index >= 64)
    {
        value.hi |= 1ULL << (index - 64);
        return;
    }
    value.lo |= 1ULL << index;
}

[[nodiscard]] bool is_zero(const UInt128 value) noexcept
{
    return value.lo == 0U && value.hi == 0U;
}

[[nodiscard]] UInt128 subtract_u128(const UInt128 left, const UInt128 right) noexcept
{
    UInt128 result;
    result.lo = left.lo - right.lo;
    result.hi = left.hi - right.hi - (left.lo < right.lo ? 1U : 0U);
    return result;
}

[[nodiscard]] bool divmod_u128(const UInt128 numerator, const UInt128 denominator,
                               UInt128 &quotient, UInt128 &remainder) noexcept
{
    if (is_zero(denominator))
    {
        return false;
    }
    quotient = {};
    remainder = {};
    for (int index = 127; index >= 0; --index)
    {
        const bool overflow = (remainder.hi >> 63U) != 0U;
        remainder.hi = (remainder.hi << 1U) | (remainder.lo >> 63U);
        remainder.lo =
            (remainder.lo << 1U) | static_cast<std::uint64_t>(bit_of128(numerator, index));
        if (overflow || cmp_u128(remainder, denominator) >= 0)
        {
            remainder = subtract_u128(remainder, denominator);
            set_bit128(quotient, index);
        }
    }
    return true;
}

[[nodiscard]] Result<std::uint64_t> round_div_ties_away(const UInt128 numerator,
                                                        const UInt128 denominator)
{
    UInt128 quotient{};
    UInt128 remainder{};
    if (!divmod_u128(numerator, denominator, quotient, remainder))
    {
        return capture_read_error("Capture rational overflowed the conversion range",
                                  "capture_rational_overflow");
    }
    if (quotient.hi != 0U)
    {
        return capture_read_error("Capture rational overflowed the conversion range",
                                  "capture_rational_overflow");
    }
    std::uint64_t rounded = quotient.lo;
    if (!is_zero(remainder) && cmp_u128(remainder, subtract_u128(denominator, remainder)) >= 0)
    {
        if (rounded == std::numeric_limits<std::uint64_t>::max())
        {
            return capture_read_error("Capture rational overflowed the conversion range",
                                      "capture_rational_overflow");
        }
        ++rounded;
    }
    return rounded;
}

[[nodiscard]] Result<void> parse_local_exif(const std::string_view text,
                                            const std::string_view field,
                                            const std::string_view path)
{
    if (text.size() != 19U)
    {
        return capture_read_error("Capture local time must be exactly YYYY:MM:DD HH:MM:SS",
                                  "invalid_capture_datetime", field, path,
                                  sanitize_error_value(text));
    }
    for (const std::size_t index : {0U, 1U, 2U, 3U, 5U, 6U, 8U, 9U, 11U, 12U, 14U, 15U, 17U, 18U})
    {
        if (!is_ascii_digit(text[index]))
        {
            return capture_read_error("Capture local time must use ASCII digits",
                                      "invalid_capture_datetime", field, path,
                                      sanitize_error_value(text));
        }
    }
    if (text[4] != ':' || text[7] != ':' || text[10] != ' ' || text[13] != ':' || text[16] != ':')
    {
        return capture_read_error("Capture local time must use Exif separators",
                                  "invalid_capture_datetime", field, path,
                                  sanitize_error_value(text));
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
        return capture_read_error("Capture local time is outside the Gregorian calendar",
                                  "invalid_capture_datetime", field, path,
                                  sanitize_error_value(text));
    }
    return {};
}

[[nodiscard]] Result<void> parse_subsecond(const std::string_view text,
                                           const std::string_view field,
                                           const std::string_view path)
{
    if (text.empty() || text.size() > 9U || !std::all_of(text.begin(), text.end(), is_ascii_digit))
    {
        return capture_read_error("Capture subsecond must be 1 to 9 ASCII digits",
                                  "invalid_capture_subsecond", field, path,
                                  sanitize_error_value(text));
    }
    return {};
}

[[nodiscard]] Result<std::int32_t> parse_utc_offset(const std::string_view text,
                                                    const std::string_view field,
                                                    const std::string_view path)
{
    if (text.size() != 6U || (text[0] != '+' && text[0] != '-') || text[3] != ':' ||
        !is_ascii_digit(text[1]) || !is_ascii_digit(text[2]) || !is_ascii_digit(text[4]) ||
        !is_ascii_digit(text[5]))
    {
        return capture_read_error("Capture UTC offset must be +HH:MM or -HH:MM",
                                  "invalid_capture_utc_offset", field, path,
                                  sanitize_error_value(text));
    }
    const int hours = digit_value(text[1]) * 10 + digit_value(text[2]);
    const int minutes = digit_value(text[4]) * 10 + digit_value(text[5]);
    if (minutes > 59 || hours > 14 || (hours == 14 && minutes != 0))
    {
        return capture_read_error("Capture UTC offset is outside -14:00..+14:00",
                                  "invalid_capture_utc_offset", field, path,
                                  sanitize_error_value(text));
    }
    if (text[0] == '-' && hours == 0 && minutes == 0)
    {
        return capture_read_error("Capture UTC offset rejects -00:00", "invalid_capture_utc_offset",
                                  field, path, sanitize_error_value(text));
    }
    const std::int32_t value = static_cast<std::int32_t>(hours * 60 + minutes);
    return text[0] == '-' ? -value : value;
}

[[nodiscard]] Result<std::int32_t> engine_dms_to_microdegrees(
    const EngineUnsignedRational degrees, const EngineUnsignedRational minutes,
    const EngineUnsignedRational seconds, const bool negative, const std::int32_t minimum,
    const std::int32_t maximum, const std::string_view field, const std::string_view path)
{
    if (degrees.denominator == 0U || minutes.denominator == 0U || seconds.denominator == 0U)
    {
        return capture_read_error("Capture GPS rational has a zero denominator",
                                  "invalid_capture_gps_rational", field, path);
    }
    const auto below_60 = [](const EngineUnsignedRational value) noexcept
    {
        return static_cast<std::uint64_t>(value.numerator) <
               60ULL * static_cast<std::uint64_t>(value.denominator);
    };
    if (!below_60(minutes) || !below_60(seconds))
    {
        return capture_read_error("Capture GPS minutes or seconds are not below 60",
                                  "invalid_capture_gps_dms", field, path);
    }
    const std::uint64_t deg_n = degrees.numerator;
    const std::uint64_t deg_d = degrees.denominator;
    const std::uint64_t min_n = minutes.numerator;
    const std::uint64_t min_d = minutes.denominator;
    const std::uint64_t sec_n = seconds.numerator;
    const std::uint64_t sec_d = seconds.denominator;
    UInt128 term0{};
    UInt128 term1{};
    UInt128 term2{};
    if (!mul_u128_u64(mul_u64_u64(deg_n, min_d), sec_d, term0) ||
        !mul_u128_u64(term0, 3600U, term0) ||
        !mul_u128_u64(mul_u64_u64(min_n, deg_d), sec_d, term1) ||
        !mul_u128_u64(term1, 60U, term1) || !mul_u128_u64(mul_u64_u64(sec_n, deg_d), min_d, term2))
    {
        return capture_read_error("Capture GPS rational overflowed the conversion range",
                                  "capture_rational_overflow", field, path);
    }
    UInt128 exact_num{};
    if (!add_u128_checked(term0, term1, exact_num) ||
        !add_u128_checked(exact_num, term2, exact_num))
    {
        return capture_read_error("Capture GPS rational overflowed the conversion range",
                                  "capture_rational_overflow", field, path);
    }
    UInt128 exact_den{};
    if (!mul_u128_u64(mul_u64_u64(deg_d, min_d), sec_d, exact_den) ||
        !mul_u128_u64(exact_den, 3600U, exact_den))
    {
        return capture_read_error("Capture GPS rational overflowed the conversion range",
                                  "capture_rational_overflow", field, path);
    }
    const std::uint64_t bound_degrees =
        static_cast<std::uint64_t>(maximum < 0 ? 0 : maximum) / 1000000ULL;
    UInt128 bound_num{};
    if (!mul_u128_u64(exact_den, bound_degrees, bound_num))
    {
        return capture_read_error("Capture GPS rational overflowed the conversion range",
                                  "capture_rational_overflow", field, path);
    }
    if (cmp_u128(exact_num, bound_num) > 0)
    {
        return capture_read_error("Capture coordinate is outside its legal bound",
                                  "invalid_capture_gps_bounds", field, path);
    }
    UInt128 whole{};
    UInt128 remainder{};
    if (!divmod_u128(exact_num, exact_den, whole, remainder) || whole.hi != 0U)
    {
        return capture_read_error("Capture GPS rational overflowed the conversion range",
                                  "capture_rational_overflow", field, path);
    }
    UInt128 scaled_remainder{};
    if (!mul_u128_u64(remainder, 1000000U, scaled_remainder))
    {
        return capture_read_error("Capture GPS rational overflowed the conversion range",
                                  "capture_rational_overflow", field, path);
    }
    auto fraction = round_div_ties_away(scaled_remainder, exact_den);
    if (!fraction || whole.lo > std::numeric_limits<std::uint64_t>::max() / 1000000U ||
        fraction.value() > std::numeric_limits<std::uint64_t>::max() - whole.lo * 1000000U)
    {
        return fraction ? capture_read_error("Capture GPS rational overflowed the conversion range",
                                             "capture_rational_overflow", field, path) :
                          fraction.error();
    }
    const std::uint64_t magnitude = whole.lo * 1000000U + fraction.value();
    const std::uint64_t allowed =
        negative ? static_cast<std::uint64_t>(-static_cast<std::int64_t>(minimum)) :
                   static_cast<std::uint64_t>(maximum);
    if (magnitude > allowed)
    {
        return capture_read_error("Capture coordinate is outside its legal bound",
                                  "invalid_capture_gps_bounds", field, path);
    }
    const std::int64_t signed_value =
        negative ? -static_cast<std::int64_t>(magnitude) : static_cast<std::int64_t>(magnitude);
    if (signed_value < minimum || signed_value > maximum)
    {
        return capture_read_error("Capture coordinate is outside its legal bound",
                                  "invalid_capture_gps_bounds", field, path);
    }
    return signed_value == 0 ? 0 : static_cast<std::int32_t>(signed_value);
}

[[nodiscard]] Result<std::uint32_t> engine_altitude_to_mm(const EngineUnsignedRational value,
                                                          const std::uint32_t maximum_mm,
                                                          const std::string_view field,
                                                          const std::string_view path)
{
    if (value.denominator == 0U)
    {
        return capture_read_error("Capture altitude rational has a zero denominator",
                                  "invalid_capture_gps_rational", field, path);
    }
    UInt128 exact_mm{};
    if (!mul_u128_u64(mul_u64_u64(value.numerator, 1000U), 1U, exact_mm))
    {
        return capture_read_error("Capture altitude overflowed the conversion range",
                                  "capture_rational_overflow", field, path);
    }
    const UInt128 bound = mul_u64_u64(maximum_mm, value.denominator);
    if (cmp_u128(exact_mm, bound) > 0)
    {
        return capture_read_error("Capture altitude is outside its legal bound",
                                  "invalid_capture_altitude", field, path);
    }
    auto mm = round_div_ties_away(exact_mm, UInt128{value.denominator, 0U});
    if (!mm)
    {
        return mm.error();
    }
    if (mm.value() > maximum_mm)
    {
        return capture_read_error("Capture altitude is outside its legal bound",
                                  "invalid_capture_altitude", field, path);
    }
    return static_cast<std::uint32_t>(mm.value());
}

enum class AsciiTagStatus
{
    kAbsent,
    kPresent,
    kDuplicate,
    kWrongType,
    kMultiValue,
    kOversized,
    kContainsNul,
};

struct AsciiTag
{
    AsciiTagStatus status = AsciiTagStatus::kAbsent;
    std::string value;
    std::string path;
};

struct Rational3Tag
{
    AsciiTagStatus status = AsciiTagStatus::kAbsent;
    std::array<EngineUnsignedRational, 3> values{};
    std::string path;
};

struct ByteTag
{
    AsciiTagStatus status = AsciiTagStatus::kAbsent;
    std::uint8_t value = 0;
    std::string path;
};

struct RationalTag
{
    AsciiTagStatus status = AsciiTagStatus::kAbsent;
    EngineUnsignedRational value;
    std::string path;
};

[[nodiscard]] Result<void> require_present_ok(const AsciiTag &tag, const std::string_view field)
{
    switch (tag.status)
    {
    case AsciiTagStatus::kAbsent:
    case AsciiTagStatus::kPresent:
        return {};
    case AsciiTagStatus::kDuplicate:
        return capture_read_error("Embedded capture tag is duplicated", "duplicate_capture_tag",
                                  field, tag.path, sanitize_error_value(tag.value));
    case AsciiTagStatus::kWrongType:
        return capture_read_error("Embedded capture tag has the wrong type", "wrong_type", field,
                                  tag.path, sanitize_error_value(tag.value));
    case AsciiTagStatus::kMultiValue:
        return capture_read_error("Embedded capture tag has multiple values", "multi_value", field,
                                  tag.path, sanitize_error_value(tag.value));
    case AsciiTagStatus::kOversized:
        return capture_read_error("Embedded capture tag exceeds its accepted byte count",
                                  "invalid_capture_tag_count", field, tag.path);
    case AsciiTagStatus::kContainsNul:
        return capture_read_error("Embedded capture tag contains a NUL", "contains_nul", field,
                                  tag.path);
    }
    return capture_read_error("Embedded capture tag is malformed", "malformed_capture_tag", field,
                              tag.path);
}

[[nodiscard]] Result<void> require_rational_ok(const AsciiTagStatus status,
                                               const std::string_view field,
                                               const std::string_view path)
{
    switch (status)
    {
    case AsciiTagStatus::kAbsent:
    case AsciiTagStatus::kPresent:
        return {};
    case AsciiTagStatus::kDuplicate:
        return capture_read_error("Embedded GPS tag is duplicated", "duplicate_capture_tag", field,
                                  path);
    case AsciiTagStatus::kWrongType:
        return capture_read_error("Embedded GPS tag has the wrong type", "wrong_type", field, path);
    case AsciiTagStatus::kMultiValue:
        return capture_read_error("Embedded GPS tag has the wrong count", "multi_value", field,
                                  path);
    case AsciiTagStatus::kOversized:
        return capture_read_error("Embedded GPS tag exceeds its accepted byte count",
                                  "invalid_capture_tag_count", field, path);
    case AsciiTagStatus::kContainsNul:
        return capture_read_error("Embedded GPS tag contains a NUL", "contains_nul", field, path);
    }
    return capture_read_error("Embedded GPS tag is malformed", "malformed_capture_tag", field,
                              path);
}

[[nodiscard]] Result<std::optional<EngineCaptureDateTime>>
resolve_engine_datetime(const AsciiTag &photo, const AsciiTag &image, const AsciiTag &fraction,
                        const AsciiTag &offset)
{
    const std::array<std::pair<const AsciiTag *, std::string_view>, 4> fields{{
        {&photo, "captured_datetime"},
        {&image, "captured_datetime"},
        {&fraction, "captured_subsecond_digits"},
        {&offset, "captured_utc_offset_minutes"},
    }};
    for (const auto &[tag, field] : fields)
    {
        auto ok = require_present_ok(*tag, field);
        if (!ok)
        {
            return ok.error();
        }
    }
    const bool has_photo = photo.status == AsciiTagStatus::kPresent;
    const bool has_image = image.status == AsciiTagStatus::kPresent;
    const bool has_fraction = fraction.status == AsciiTagStatus::kPresent;
    const bool has_offset = offset.status == AsciiTagStatus::kPresent;
    if (!has_photo && !has_image)
    {
        if (has_fraction || has_offset)
        {
            return capture_read_error("Capture fraction or offset is present without a base time",
                                      "orphan_capture_datetime_component", "captured_datetime",
                                      has_fraction ? fraction.path : offset.path);
        }
        return std::optional<EngineCaptureDateTime>{};
    }
    EngineCaptureDateTime value;
    if (has_photo)
    {
        auto parsed = parse_local_exif(photo.value, "captured_datetime", photo.path);
        if (!parsed)
        {
            return parsed.error();
        }
        value.local_exif = photo.value;
        if (has_image)
        {
            auto other = parse_local_exif(image.value, "captured_datetime", image.path);
            if (!other)
            {
                return other.error();
            }
            if (image.value != photo.value)
            {
                return capture_read_error("Photo and Image DateTimeOriginal conflict",
                                          "conflicting_capture_datetime", "captured_datetime",
                                          photo.path, sanitize_error_value(photo.value));
            }
        }
    }
    else
    {
        auto parsed = parse_local_exif(image.value, "captured_datetime", image.path);
        if (!parsed)
        {
            return parsed.error();
        }
        value.local_exif = image.value;
    }
    if (has_fraction)
    {
        auto parsed = parse_subsecond(fraction.value, "captured_subsecond_digits", fraction.path);
        if (!parsed)
        {
            return parsed.error();
        }
        value.subsecond_digits = fraction.value;
    }
    if (has_offset)
    {
        auto parsed = parse_utc_offset(offset.value, "captured_utc_offset_minutes", offset.path);
        if (!parsed)
        {
            return parsed.error();
        }
        value.utc_offset_minutes = parsed.value();
    }
    return std::optional<EngineCaptureDateTime>{std::move(value)};
}

[[nodiscard]] Result<std::optional<EngineCaptureLocation>>
resolve_engine_location(const AsciiTag &lat_ref, const Rational3Tag &lat, const AsciiTag &lon_ref,
                        const Rational3Tag &lon, const ByteTag &alt_ref, const RationalTag &alt)
{
    auto lat_ref_ok = require_present_ok(lat_ref, "gps_latitude_ref");
    if (!lat_ref_ok)
    {
        return lat_ref_ok.error();
    }
    auto lon_ref_ok = require_present_ok(lon_ref, "gps_longitude_ref");
    if (!lon_ref_ok)
    {
        return lon_ref_ok.error();
    }
    auto lat_ok = require_rational_ok(lat.status, "gps_latitude_e6", lat.path);
    if (!lat_ok)
    {
        return lat_ok.error();
    }
    auto lon_ok = require_rational_ok(lon.status, "gps_longitude_e6", lon.path);
    if (!lon_ok)
    {
        return lon_ok.error();
    }
    auto alt_ref_ok = require_rational_ok(alt_ref.status, "gps_altitude_ref", alt_ref.path);
    if (!alt_ref_ok)
    {
        return alt_ref_ok.error();
    }
    auto alt_ok = require_rational_ok(alt.status, "gps_altitude", alt.path);
    if (!alt_ok)
    {
        return alt_ok.error();
    }
    const bool any_pair =
        lat_ref.status == AsciiTagStatus::kPresent || lat.status == AsciiTagStatus::kPresent ||
        lon_ref.status == AsciiTagStatus::kPresent || lon.status == AsciiTagStatus::kPresent;
    const bool all_pair =
        lat_ref.status == AsciiTagStatus::kPresent && lat.status == AsciiTagStatus::kPresent &&
        lon_ref.status == AsciiTagStatus::kPresent && lon.status == AsciiTagStatus::kPresent;
    const bool any_alt =
        alt_ref.status == AsciiTagStatus::kPresent || alt.status == AsciiTagStatus::kPresent;
    const bool all_alt =
        alt_ref.status == AsciiTagStatus::kPresent && alt.status == AsciiTagStatus::kPresent;
    if (!any_pair && !any_alt)
    {
        return std::optional<EngineCaptureLocation>{};
    }
    if (any_pair && !all_pair)
    {
        return capture_read_error(
            "Capture location requires latitude, longitude, and both references",
            "incomplete_capture_location", "gps");
    }
    if (any_alt && !all_alt)
    {
        return capture_read_error("Capture altitude requires both the value and reference 0 or 1",
                                  "incomplete_capture_altitude", "gps_altitude");
    }
    if (any_alt && !all_pair)
    {
        return capture_read_error("Capture altitude cannot exist without a complete location",
                                  "orphan_capture_altitude", "gps_altitude");
    }
    if (lat_ref.value != "N" && lat_ref.value != "S")
    {
        return capture_read_error("Capture latitude reference must be N or S",
                                  "invalid_capture_gps_ref", "gps_latitude_ref", lat_ref.path,
                                  sanitize_error_value(lat_ref.value));
    }
    if (lon_ref.value != "E" && lon_ref.value != "W")
    {
        return capture_read_error("Capture longitude reference must be E or W",
                                  "invalid_capture_gps_ref", "gps_longitude_ref", lon_ref.path,
                                  sanitize_error_value(lon_ref.value));
    }
    auto latitude = engine_dms_to_microdegrees(lat.values[0], lat.values[1], lat.values[2],
                                               lat_ref.value == "S", -90000000, 90000000,
                                               "gps_latitude_e6", lat.path);
    if (!latitude)
    {
        return latitude.error();
    }
    auto longitude = engine_dms_to_microdegrees(lon.values[0], lon.values[1], lon.values[2],
                                                lon_ref.value == "W", -180000000, 180000000,
                                                "gps_longitude_e6", lon.path);
    if (!longitude)
    {
        return longitude.error();
    }
    EngineCaptureLocation location;
    location.latitude_e6 = latitude.value();
    location.longitude_e6 = longitude.value();
    if (all_alt)
    {
        if (alt_ref.value != 0U && alt_ref.value != 1U)
        {
            return capture_read_error("Capture altitude reference must be 0 or 1",
                                      "invalid_capture_altitude_ref", "gps_altitude_ref",
                                      alt_ref.path);
        }
        const std::uint32_t maximum = alt_ref.value == 1U ? 12000000U : 100000000U;
        auto mm = engine_altitude_to_mm(alt.value, maximum, "gps_altitude", alt.path);
        if (!mm)
        {
            return mm.error();
        }
        EngineCaptureAltitude altitude;
        altitude.magnitude_mm = mm.value();
        altitude.reference = alt_ref.value == 1U ? EngineCaptureAltitudeReference::kBelowSeaLevel :
                                                   EngineCaptureAltitudeReference::kAboveSeaLevel;
        location.altitude = altitude;
    }
    return std::optional<EngineCaptureLocation>{location};
}

[[nodiscard]] std::size_t count_exif_key(const Exiv2::ExifData &exif, const char *key)
{
    std::size_t count = 0;
    for (auto it = exif.begin(); it != exif.end(); ++it)
    {
        if (it->key() == key)
        {
            ++count;
        }
    }
    return count;
}

void extract_ascii(const Exiv2::ExifData &exif, AsciiTag &tag, const char *key,
                   const std::size_t maximum_bytes)
{
    tag.path = key;
    const std::size_t count = count_exif_key(exif, key);
    if (count == 0U)
    {
        tag.status = AsciiTagStatus::kAbsent;
        return;
    }
    if (count > 1U)
    {
        tag.status = AsciiTagStatus::kDuplicate;
        return;
    }
    const auto position = exif.findKey(Exiv2::ExifKey(key));
    if (position == exif.end())
    {
        tag.status = AsciiTagStatus::kAbsent;
        return;
    }
    if (position->typeId() != Exiv2::asciiString)
    {
        tag.status = AsciiTagStatus::kWrongType;
        return;
    }
    const auto &value = position->value();
    if (value.size() > maximum_bytes)
    {
        tag.status = AsciiTagStatus::kOversized;
        return;
    }
    std::string raw(static_cast<std::size_t>(value.size()), '\0');
    if (!raw.empty())
    {
        value.copy(reinterpret_cast<Exiv2::byte *>(raw.data()), Exiv2::littleEndian);
    }
    if (!raw.empty() && raw.back() == '\0')
    {
        raw.pop_back();
    }
    if (raw.find('\0') != std::string::npos)
    {
        tag.status = AsciiTagStatus::kContainsNul;
        tag.value = raw.substr(0, raw.find('\0'));
        return;
    }
    tag.status = AsciiTagStatus::kPresent;
    tag.value = std::move(raw);
}

void extract_urational3(const Exiv2::ExifData &exif, Rational3Tag &tag, const char *key)
{
    tag.path = key;
    const std::size_t count = count_exif_key(exif, key);
    if (count == 0U)
    {
        tag.status = AsciiTagStatus::kAbsent;
        return;
    }
    if (count > 1U)
    {
        tag.status = AsciiTagStatus::kDuplicate;
        return;
    }
    const auto position = exif.findKey(Exiv2::ExifKey(key));
    if (position == exif.end())
    {
        tag.status = AsciiTagStatus::kAbsent;
        return;
    }
    if (position->typeId() != Exiv2::unsignedRational)
    {
        tag.status = AsciiTagStatus::kWrongType;
        return;
    }
    const auto *urational = dynamic_cast<const Exiv2::URationalValue *>(&position->value());
    if (urational == nullptr)
    {
        tag.status = AsciiTagStatus::kWrongType;
        return;
    }
    if (urational->count() != 3U)
    {
        tag.status = AsciiTagStatus::kMultiValue;
        return;
    }
    for (std::size_t index = 0; index < 3U; ++index)
    {
        const Exiv2::URational rational = urational->value_.at(index);
        tag.values[index].numerator = rational.first;
        tag.values[index].denominator = rational.second;
    }
    tag.status = AsciiTagStatus::kPresent;
}

void extract_byte(const Exiv2::ExifData &exif, ByteTag &tag, const char *key)
{
    tag.path = key;
    const std::size_t count = count_exif_key(exif, key);
    if (count == 0U)
    {
        tag.status = AsciiTagStatus::kAbsent;
        return;
    }
    if (count > 1U)
    {
        tag.status = AsciiTagStatus::kDuplicate;
        return;
    }
    const auto position = exif.findKey(Exiv2::ExifKey(key));
    if (position == exif.end())
    {
        tag.status = AsciiTagStatus::kAbsent;
        return;
    }
    if (position->typeId() != Exiv2::unsignedByte)
    {
        tag.status = AsciiTagStatus::kWrongType;
        return;
    }
    if (position->count() != 1U)
    {
        tag.status = AsciiTagStatus::kMultiValue;
        return;
    }
    tag.status = AsciiTagStatus::kPresent;
    tag.value = static_cast<std::uint8_t>(position->toInt64(0U));
}

void extract_urational(const Exiv2::ExifData &exif, RationalTag &tag, const char *key)
{
    tag.path = key;
    const std::size_t count = count_exif_key(exif, key);
    if (count == 0U)
    {
        tag.status = AsciiTagStatus::kAbsent;
        return;
    }
    if (count > 1U)
    {
        tag.status = AsciiTagStatus::kDuplicate;
        return;
    }
    const auto position = exif.findKey(Exiv2::ExifKey(key));
    if (position == exif.end())
    {
        tag.status = AsciiTagStatus::kAbsent;
        return;
    }
    if (position->typeId() != Exiv2::unsignedRational)
    {
        tag.status = AsciiTagStatus::kWrongType;
        return;
    }
    const auto *urational = dynamic_cast<const Exiv2::URationalValue *>(&position->value());
    if (urational == nullptr || urational->count() != 1U)
    {
        tag.status =
            urational == nullptr ? AsciiTagStatus::kWrongType : AsciiTagStatus::kMultiValue;
        return;
    }
    const Exiv2::URational rational = urational->value_.at(0U);
    tag.status = AsciiTagStatus::kPresent;
    tag.value.numerator = rational.first;
    tag.value.denominator = rational.second;
}

[[nodiscard]] Result<std::optional<double>>
extract_capture_number(const Exiv2::ExifData &exif, const char *key, const std::string_view field)
{
    const std::size_t count = count_exif_key(exif, key);
    if (count == 0U)
        return std::optional<double>{};
    if (count > 1U)
        return capture_read_error("Embedded capture tag is duplicated", "duplicate_capture_tag",
                                  field, key);
    const auto position = exif.findKey(Exiv2::ExifKey(key));
    if (position == exif.end())
        return std::optional<double>{};
    if (position->count() != 1U)
        return capture_read_error("Embedded capture tag has multiple values", "multi_value", field,
                                  key);
    const auto type = position->typeId();
    if (type != Exiv2::unsignedShort && type != Exiv2::unsignedLong &&
        type != Exiv2::unsignedRational && type != Exiv2::signedShort &&
        type != Exiv2::signedLong && type != Exiv2::signedRational)
        return capture_read_error("Embedded capture tag has the wrong numeric type", "wrong_type",
                                  field, key);
    const double value = static_cast<double>(position->toFloat(0U));
    if (!std::isfinite(value) || value <= 0.0)
        return capture_read_error("Embedded capture numeric value must be finite and positive",
                                  "invalid_capture_numeric", field, key);
    return std::optional<double>{value};
}

[[nodiscard]] Result<std::optional<std::string>> resolve_capture_text(const AsciiTag &tag,
                                                                      const std::string_view field)
{
    auto valid = require_present_ok(tag, field);
    if (!valid)
        return valid.error();
    if (tag.status != AsciiTagStatus::kPresent)
        return std::optional<std::string>{};
    std::size_t begin = 0U;
    while (begin < tag.value.size() && (tag.value[begin] == ' ' || tag.value[begin] == '\t'))
        ++begin;
    std::size_t end = tag.value.size();
    while (end > begin && (tag.value[end - 1U] == ' ' || tag.value[end - 1U] == '\t'))
        --end;
    if (begin == end)
        return std::optional<std::string>{};
    return std::optional<std::string>{tag.value.substr(begin, end - begin)};
}

[[nodiscard]] Result<EngineCaptureMetadata> extract_from_exif(const Exiv2::ExifData &exif)
{
    AsciiTag make{};
    AsciiTag model{};
    AsciiTag lens_make{};
    AsciiTag lens_model{};
    AsciiTag photo{};
    AsciiTag image{};
    AsciiTag fraction{};
    AsciiTag offset{};
    AsciiTag lat_ref{};
    AsciiTag lon_ref{};
    Rational3Tag lat{};
    Rational3Tag lon{};
    ByteTag alt_ref{};
    RationalTag alt{};
    extract_ascii(exif, make, "Exif.Image.Make", 128U);
    extract_ascii(exif, model, "Exif.Image.Model", 128U);
    extract_ascii(exif, lens_make, "Exif.Photo.LensMake", 128U);
    extract_ascii(exif, lens_model, "Exif.Photo.LensModel", 128U);
    extract_ascii(exif, photo, "Exif.Photo.DateTimeOriginal", 20U);
    extract_ascii(exif, image, "Exif.Image.DateTimeOriginal", 20U);
    extract_ascii(exif, fraction, "Exif.Photo.SubSecTimeOriginal", 10U);
    extract_ascii(exif, offset, "Exif.Photo.OffsetTimeOriginal", 7U);
    extract_ascii(exif, lat_ref, "Exif.GPSInfo.GPSLatitudeRef", 2U);
    extract_urational3(exif, lat, "Exif.GPSInfo.GPSLatitude");
    extract_ascii(exif, lon_ref, "Exif.GPSInfo.GPSLongitudeRef", 2U);
    extract_urational3(exif, lon, "Exif.GPSInfo.GPSLongitude");
    extract_byte(exif, alt_ref, "Exif.GPSInfo.GPSAltitudeRef");
    extract_urational(exif, alt, "Exif.GPSInfo.GPSAltitude");
    auto camera_make = resolve_capture_text(make, "camera_make");
    auto camera_model = resolve_capture_text(model, "camera_model");
    auto lens_make_text = resolve_capture_text(lens_make, "lens_make");
    auto lens_model_text = resolve_capture_text(lens_model, "lens_model");
    auto iso_new = extract_capture_number(exif, "Exif.Photo.PhotographicSensitivity", "iso");
    auto iso_old = extract_capture_number(exif, "Exif.Photo.ISOSpeedRatings", "iso");
    auto aperture = extract_capture_number(exif, "Exif.Photo.FNumber", "aperture");
    auto focal = extract_capture_number(exif, "Exif.Photo.FocalLength", "focal_length_mm");
    auto shutter = extract_capture_number(exif, "Exif.Photo.ExposureTime", "shutter_s");
    if (!camera_make || !camera_model || !lens_make_text || !lens_model_text || !iso_new ||
        !iso_old || !aperture || !focal || !shutter)
    {
        return !camera_make     ? camera_make.error() :
               !camera_model    ? camera_model.error() :
               !lens_make_text  ? lens_make_text.error() :
               !lens_model_text ? lens_model_text.error() :
               !iso_new         ? iso_new.error() :
               !iso_old         ? iso_old.error() :
               !aperture        ? aperture.error() :
               !focal           ? focal.error() :
                                  shutter.error();
    }
    if (iso_new.value() && iso_old.value() &&
        std::abs(*iso_new.value() - *iso_old.value()) > 1.0e-9)
    {
        return capture_read_error("Embedded ISO tags conflict", "conflicting_capture_numeric",
                                  "iso", "Exif.Photo.PhotographicSensitivity");
    }
    auto datetime = resolve_engine_datetime(photo, image, fraction, offset);
    if (!datetime)
    {
        return datetime.error();
    }
    auto location = resolve_engine_location(lat_ref, lat, lon_ref, lon, alt_ref, alt);
    if (!location)
    {
        return location.error();
    }
    EngineCaptureMetadata extracted;
    extracted.camera_make = std::move(camera_make).value();
    extracted.camera_model = std::move(camera_model).value();
    extracted.lens_make = std::move(lens_make_text).value();
    extracted.lens_model = std::move(lens_model_text).value();
    extracted.iso = iso_new.value() ? iso_new.value() : iso_old.value();
    extracted.aperture = aperture.value();
    extracted.focal_length_mm = focal.value();
    extracted.shutter_s = shutter.value();
    extracted.captured_datetime = std::move(datetime).value();
    extracted.location = std::move(location).value();
    return extracted;
}

[[nodiscard]] std::uint32_t read_be32(const unsigned char *bytes) noexcept
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
           (static_cast<std::uint32_t>(bytes[1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[2]) << 8U) | static_cast<std::uint32_t>(bytes[3]);
}

[[nodiscard]] Result<bool> read_exact(QFile &file, char *buffer, const qint64 size,
                                      const char *truncated_reason, const char *read_reason)
{
    qint64 total = 0;
    while (total < size)
    {
        const qint64 got = file.read(buffer + total, size - total);
        if (got > 0)
        {
            total += got;
            continue;
        }
        if (got < 0 || file.error() != QFile::NoError)
        {
            std::map<std::string, std::string, std::less<>> context{{"reason", read_reason}};
            const auto detail = sanitize_error_value(file.errorString().toStdString());
            if (!detail.empty())
            {
                context.emplace("qt_error", detail);
            }
            return make_error(ErrorCode::kIo, "Unable to read the PNG container",
                              std::move(context));
        }
        return make_error(ErrorCode::kValidation, "PNG container is truncated",
                          {{"reason", truncated_reason}});
    }
    return true;
}

[[nodiscard]] bool is_png_chunk_letter(const unsigned char value) noexcept
{
    return (value >= static_cast<unsigned char>('A') && value <= static_cast<unsigned char>('Z')) ||
           (value >= static_cast<unsigned char>('a') && value <= static_cast<unsigned char>('z'));
}

[[nodiscard]] Result<std::optional<std::vector<std::uint8_t>>>
extract_png_exif_payload(const QString &path)
{
    static constexpr unsigned char kSignature[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    static constexpr std::uint32_t kMaxExifPayload = 16U * 1024U * 1024U;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        std::map<std::string, std::string, std::less<>> context;
        const auto detail = sanitize_error_value(file.errorString().toStdString());
        if (!detail.empty())
        {
            context.emplace("qt_error", detail);
        }
        return make_error(ErrorCode::kIo,
                          "Unable to read the PNG source for embedded capture metadata",
                          std::move(context));
    }
    unsigned char signature[8];
    auto header = read_exact(file, reinterpret_cast<char *>(signature), 8,
                             "truncated_png_container", "png_read_failed");
    if (!header)
    {
        return header.error();
    }
    if (std::memcmp(signature, kSignature, sizeof(kSignature)) != 0)
    {
        return std::optional<std::vector<std::uint8_t>>{};
    }
    bool saw_ihdr = false;
    bool saw_idat = false;
    bool idat_ended = false;
    bool saw_iend = false;
    std::optional<std::vector<std::uint8_t>> payload;
    while (!saw_iend)
    {
        unsigned char prefix[8];
        auto prefix_ok = read_exact(file, reinterpret_cast<char *>(prefix), 8,
                                    "truncated_png_chunk_header", "png_read_failed");
        if (!prefix_ok)
        {
            return prefix_ok.error();
        }
        const std::uint32_t length = read_be32(prefix);
        const unsigned char *type = prefix + 4;
        if (!std::all_of(type, type + 4, is_png_chunk_letter) || (type[2] & 0x20U) != 0U)
        {
            return make_error(ErrorCode::kValidation, "PNG chunk type is invalid",
                              {{"reason", "invalid_png_chunk_type"}});
        }
        const bool is_ihdr = std::memcmp(type, "IHDR", 4) == 0;
        const bool is_exif = std::memcmp(type, "eXIf", 4) == 0;
        const bool is_idat = std::memcmp(type, "IDAT", 4) == 0;
        const bool is_iend = std::memcmp(type, "IEND", 4) == 0;
        if (!saw_ihdr && !is_ihdr)
        {
            return make_error(ErrorCode::kValidation, "PNG IHDR must be the first chunk",
                              {{"reason", "missing_png_ihdr"}});
        }
        if (is_ihdr && (saw_ihdr || length != 13U))
        {
            return make_error(
                ErrorCode::kValidation,
                saw_ihdr ? "PNG contains more than one IHDR" : "PNG IHDR has the wrong length",
                {{"reason", saw_ihdr ? "duplicate_png_ihdr" : "invalid_png_ihdr_length"}});
        }
        if (is_exif && length > kMaxExifPayload)
        {
            return make_error(ErrorCode::kValidation, "PNG eXIf payload exceeds the accepted cap",
                              {{"reason", "png_exif_payload_too_large"}});
        }
        if (is_exif && payload)
        {
            return make_error(ErrorCode::kValidation, "PNG contains a duplicate eXIf chunk",
                              {{"reason", "duplicate_png_exif_chunk"}});
        }
        if (is_exif && saw_idat)
        {
            return make_error(ErrorCode::kValidation, "PNG eXIf must appear before IDAT",
                              {{"reason", "png_exif_after_idat"}});
        }
        if (is_exif && length == 0U)
        {
            return make_error(ErrorCode::kValidation, "PNG eXIf chunk is empty",
                              {{"reason", "empty_png_exif_chunk"}});
        }
        if (is_idat && idat_ended)
        {
            return make_error(ErrorCode::kValidation, "PNG IDAT chunks must be consecutive",
                              {{"reason", "nonconsecutive_png_idat"}});
        }
        if (is_iend && (length != 0U || !saw_idat))
        {
            return make_error(
                ErrorCode::kValidation,
                length != 0U ? "PNG IEND has the wrong length" :
                               "PNG IEND appears before image data",
                {{"reason", length != 0U ? "invalid_png_iend_length" : "png_iend_before_idat"}});
        }

        std::vector<std::uint8_t> exif_data;
        if (is_exif)
        {
            exif_data.resize(static_cast<std::size_t>(length));
        }
        std::array<unsigned char, 64U * 1024U> buffer{};
        std::uint32_t consumed = 0U;
        uLong calculated = crc32(0L, Z_NULL, 0);
        calculated = crc32(calculated, reinterpret_cast<const Bytef *>(type), 4U);
        while (consumed < length)
        {
            const std::uint32_t remaining = length - consumed;
            const std::uint32_t step =
                std::min<std::uint32_t>(remaining, static_cast<std::uint32_t>(buffer.size()));
            unsigned char *destination = is_exif ? exif_data.data() + consumed : buffer.data();
            auto data_ok =
                read_exact(file, reinterpret_cast<char *>(destination), static_cast<qint64>(step),
                           "truncated_png_chunk_payload", "png_read_failed");
            if (!data_ok)
            {
                return data_ok.error();
            }
            calculated = crc32(calculated, destination, static_cast<uInt>(step));
            consumed += step;
        }
        unsigned char crc_bytes[4];
        auto crc_ok = read_exact(file, reinterpret_cast<char *>(crc_bytes), 4,
                                 "truncated_png_chunk_crc", "png_read_failed");
        if (!crc_ok)
        {
            return crc_ok.error();
        }
        const std::uint32_t stored_crc = read_be32(crc_bytes);
        if (static_cast<std::uint32_t>(calculated) != stored_crc)
        {
            return make_error(ErrorCode::kValidation, "PNG chunk CRC does not match its payload",
                              {{"reason", "png_chunk_crc_mismatch"}});
        }
        if (is_ihdr)
        {
            saw_ihdr = true;
        }
        else if (is_exif)
        {
            static constexpr unsigned char kJpegPrefix[] = {'E', 'x', 'i', 'f', 0, 0};
            if (length >= 6U && std::memcmp(exif_data.data(), kJpegPrefix, 6) == 0)
            {
                return make_error(ErrorCode::kValidation,
                                  "PNG eXIf must not use the JPEG Exif prefix",
                                  {{"reason", "jpeg_exif_prefix_in_png"}});
            }
            payload = std::move(exif_data);
        }
        else if (is_idat)
        {
            saw_idat = true;
        }
        else if (is_iend)
        {
            saw_iend = true;
        }
        else if (saw_idat)
        {
            idat_ended = true;
        }
    }
    char trailing = 0;
    const qint64 trailing_size = file.read(&trailing, 1);
    if (trailing_size < 0 || file.error() != QFile::NoError)
    {
        return make_error(ErrorCode::kIo, "Unable to finish reading the PNG container",
                          {{"reason", "png_read_failed"}});
    }
    if (trailing_size != 0)
    {
        return make_error(ErrorCode::kValidation, "PNG contains trailing bytes after IEND",
                          {{"reason", "png_trailing_data"}});
    }
    return payload;
}

[[nodiscard]] bool peek_png_signature(const QString &path, bool &is_png)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        return false;
    }
    unsigned char signature[8];
    if (file.read(reinterpret_cast<char *>(signature), 8) != 8)
    {
        is_png = false;
        return true;
    }
    static constexpr unsigned char kSignature[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    is_png = std::memcmp(signature, kSignature, sizeof(kSignature)) == 0;
    return true;
}

} // namespace

Result<EngineCaptureMetadata> read_embedded_capture_metadata(const std::string_view input_uri,
                                                             const CancellationToken &cancellation)
{
    configure_exiv2_diagnostics();
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    const QString path = local_path(input_uri);
    if (path.isEmpty())
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Capture metadata input path must not be empty",
                          capture_source_context(input_uri));
    }
    QFileInfo info(path);
    if (!info.exists())
    {
        return make_error(ErrorCode::kNotFound, "Capture metadata input does not exist",
                          capture_source_context(input_uri));
    }
    if (!info.isFile())
    {
        return make_error(ErrorCode::kValidation, "Capture metadata input must be a regular file",
                          capture_source_context(input_uri, "non_regular_capture_source"));
    }
    cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    try
    {
        bool is_png = false;
        if (!peek_png_signature(path, is_png))
        {
            return make_error(ErrorCode::kIo,
                              "Unable to read the source for embedded capture metadata",
                              capture_source_context(input_uri));
        }
        Exiv2::Image::UniquePtr image;
        std::vector<std::uint8_t> png_payload;
        if (is_png)
        {
            auto extracted = extract_png_exif_payload(path);
            if (!extracted)
            {
                return extracted.error();
            }
            if (!extracted.value())
            {
                cancelled = cancellation.check();
                if (!cancelled)
                {
                    return cancelled.error();
                }
                return EngineCaptureMetadata{};
            }
            png_payload = std::move(*extracted.value());
            image = Exiv2::ImageFactory::open(png_payload.data(), png_payload.size());
        }
        else
        {
            const QByteArray utf8 = path.toUtf8();
            image = Exiv2::ImageFactory::open(utf8.constData());
        }
        if (!image)
        {
            return make_error(
                ErrorCode::kValidation, "Exiv2 did not create an embedded-metadata reader",
                capture_source_context(input_uri, "embedded_capture_reader_unavailable"));
        }
        image->readMetadata();
        cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        auto extracted = extract_from_exif(image->exifData());
        if (!extracted)
        {
            return extracted.error();
        }
        cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        return extracted;
    }
    catch (const std::bad_alloc &)
    {
        return make_error(ErrorCode::kIo, "Embedded capture Exif allocation failed",
                          capture_source_context(input_uri, "embedded_capture_allocation_failed"));
    }
    catch (const Exiv2::Error &error)
    {
        return make_error(
            ErrorCode::kValidation, "Embedded capture Exif could not be read",
            capture_source_context(input_uri, "embedded_capture_exif_failed", error.what()));
    }
    catch (const std::exception &error)
    {
        return make_error(
            ErrorCode::kValidation, "Embedded capture Exif could not be read",
            capture_source_context(input_uri, "embedded_capture_exif_failed", error.what()));
    }
}

} // namespace ravo
