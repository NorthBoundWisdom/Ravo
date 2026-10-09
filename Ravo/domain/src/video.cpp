#include "ravo/domain/video.h"
#include <algorithm>
#include <cmath>
#include <charconv>
#include <cctype>
#include <climits>
#include <set>
#include <filesystem>
#include <locale>
#include <sstream>
#include <iomanip>
#include <limits>
#include "ravo/domain/uri.h"
#include "ravo/foundation/parse_number.h"

namespace ravo
{
bool is_video_path(const std::string_view path)
{
    auto extension =
        std::filesystem::path(std::u8string(path.begin(), path.end())).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".mov" || extension == ".mp4" || extension == ".m4v" ||
           extension == ".mkv" || extension == ".avi" || extension == ".webm" ||
           extension == ".mts" || extension == ".m2ts";
}
bool is_video_media_type(const std::string_view type) noexcept
{
    return type.starts_with("video/");
}
JsonValue video_info_json(const VideoInfo &v)
{
    std::ostringstream peak;
    peak.imbue(std::locale::classic());
    peak << std::setprecision(std::numeric_limits<double>::max_digits10) << v.peak_nits;
    JsonValue::Object object{
        {"schema", "ravo.video_info/v1"},
        {"container", v.container},
        {"codec", v.codec},
        {"width", JsonValue::number(std::to_string(v.width))},
        {"height", JsonValue::number(std::to_string(v.height))},
        {"frame_rate_num", JsonValue::number(std::to_string(v.frame_rate_num))},
        {"frame_rate_den", JsonValue::number(std::to_string(v.frame_rate_den))},
        {"rotation", JsonValue::number(std::to_string(v.rotation))},
        {"has_audio", v.has_audio},
        {"audio_codec", v.audio_codec},
        {"transfer", v.transfer},
        {"primaries", v.primaries},
        {"peak_nits", JsonValue::number(peak.str())},
        {"peak_source", v.peak_source},
        {"dolby_vision", v.dolby_vision}};
    if (v.duration_us)
        object.emplace("duration_us", JsonValue::number(std::to_string(*v.duration_us)));
    if (v.captured_unix_s)
        object.emplace("captured_unix_s", JsonValue::number(std::to_string(*v.captured_unix_s)));
    JsonValue::Array warnings;
    for (const auto &warning : v.warnings)
        warnings.emplace_back(warning);
    object.emplace("warnings", std::move(warnings));
    if (v.matrix)
        object.emplace("matrix", *v.matrix);
    if (v.full_range)
        object.emplace("full_range", *v.full_range);
    return JsonValue{std::move(object)};
}
Result<VideoInfo> parse_video_info(const JsonValue &value)
{
    const auto invalid = []
    {
        return make_error(ErrorCode::kValidation, "Invalid video metadata",
                          {{"reason", "invalid_video_info"}});
    };
    const auto *schema = value.find("schema");
    if (!schema || !schema->string_if() || *schema->string_if() != "ravo.video_info/v1")
        return invalid();
    static const std::set<std::string, std::less<>> allowed{
        "schema",         "container",      "codec",     "width",       "height",
        "frame_rate_num", "frame_rate_den", "rotation",  "has_audio",   "audio_codec",
        "transfer",       "primaries",      "peak_nits", "peak_source", "captured_unix_s",
        "duration_us",    "dolby_vision",   "warnings",  "matrix",      "full_range"};
    if (!value.object_if())
        return invalid();
    for (const auto &[key, ignored] : *value.object_if())
    {
        static_cast<void>(ignored);
        if (!allowed.contains(key))
            return invalid();
    }
    VideoInfo v;
    if (const auto *range = value.find("full_range"))
    {
        if (!range->boolean_if())
            return invalid();
        v.full_range = *range->boolean_if();
    }
    if (const auto *matrix = value.find("matrix"))
    {
        if (!matrix->string_if() ||
            (*matrix->string_if() != "bt601" && *matrix->string_if() != "bt709" &&
             *matrix->string_if() != "bt2020"))
            return invalid();
        v.matrix = *matrix->string_if();
    }
    for (auto pair : {std::pair{"container", &v.container},
                      {"codec", &v.codec},
                      {"audio_codec", &v.audio_codec},
                      {"transfer", &v.transfer},
                      {"primaries", &v.primaries},
                      {"peak_source", &v.peak_source},
                      {"dolby_vision", &v.dolby_vision}})
    {
        const auto *field = value.find(pair.first);
        if (!field || !field->string_if() || field->string_if()->size() > 128)
            return invalid();
        *pair.second = *field->string_if();
    }
    auto integer = [&](const char *key) -> Result<std::int64_t>
    {
        const auto *field = value.find(key);
        if (!field || !field->number_if())
            return invalid();
        const auto &text = field->number_if()->text;
        std::int64_t parsed = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
            return invalid();
        return parsed;
    };
    auto w = integer("width"), h = integer("height"), n = integer("frame_rate_num"),
         d = integer("frame_rate_den"), r = integer("rotation");
    if (!w || !h || !n || !d || !r || w.value() < 1 || h.value() < 1 || w.value() > 16384 ||
        h.value() > 16384 || n.value() < 0 || n.value() > INT32_MAX || d.value() < 1 ||
        d.value() > INT32_MAX ||
        (r.value() != 0 && r.value() != 90 && r.value() != 180 && r.value() != 270))
        return invalid();
    v.width = static_cast<std::uint32_t>(w.value());
    v.height = static_cast<std::uint32_t>(h.value());
    v.frame_rate_num = static_cast<std::int32_t>(n.value());
    v.frame_rate_den = static_cast<std::int32_t>(d.value());
    v.rotation = static_cast<int>(r.value());
    auto *audio = value.find("has_audio"), *peak = value.find("peak_nits");
    if (!audio || !audio->boolean_if() || !peak || !peak->number_if())
        return invalid();
    v.has_audio = *audio->boolean_if();
    if (const auto *warnings = value.find("warnings"))
    {
        if (!warnings->array_if() || warnings->array_if()->size() > 3)
            return invalid();
        for (const auto &warning : *warnings->array_if())
        {
            const auto *code = warning.string_if();
            if (!code ||
                (*code != "unknown_cover_ignored" && *code != "extra_channel_descriptions_capped" &&
                 *code != "unsupported_auxiliary_audio") ||
                std::find(v.warnings.begin(), v.warnings.end(), *code) != v.warnings.end())
                return invalid();
            v.warnings.push_back(*code);
        }
    }
    if ((v.container != "mov" && v.container != "mp4") ||
        (v.codec != "h264" && v.codec != "hevc" && v.codec != "prores") ||
        (v.transfer != "bt709" && v.transfer != "srgb" && v.transfer != "pq" &&
         v.transfer != "hlg") ||
        (v.primaries != "bt709" && v.primaries != "bt2020" && v.primaries != "p3_d65") ||
        (v.dolby_vision != "none" && v.dolby_vision != "compatible_base_layer_only") ||
        (!v.has_audio && !v.audio_codec.empty()) || (v.has_audio && v.audio_codec.empty()))
        return invalid();
    const auto &peak_text = peak->number_if()->text;
    if (!parse_ascii_double(peak_text, v.peak_nits) || !std::isfinite(v.peak_nits) ||
        v.peak_nits <= 100 || v.peak_nits > 10000)
        return invalid();
    for (auto pair :
         {std::pair{"duration_us", &v.duration_us}, {"captured_unix_s", &v.captured_unix_s}})
    {
        if (value.find(pair.first))
        {
            auto parsed = integer(pair.first);
            if (!parsed || (std::string_view(pair.first) == "duration_us" && parsed.value() < 0))
                return invalid();
            *pair.second = parsed.value();
        }
    }
    return v;
}
} // namespace ravo
