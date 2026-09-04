#pragma once

// PERF-01 measurement harness: shared report schema + percentile helpers.
// Measure-only — does not admit browse optimizations (PERF-02).

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/foundation/json.h"

namespace ravo::interactive_perf_report
{

inline constexpr std::string_view kSchema = "ravo.perf01.report/v1";
inline constexpr std::size_t kDefaultWarmups = 2U;
inline constexpr std::size_t kDefaultRecordedSamples = 8U;

struct PercentileSummary
{
    std::int64_t min = 0;
    std::int64_t p50 = 0;
    std::int64_t p90 = 0;
    std::int64_t max = 0;
};

[[nodiscard]] inline PercentileSummary summarize(std::vector<std::int64_t> values)
{
    PercentileSummary out{};
    if (values.empty())
        return out;
    std::sort(values.begin(), values.end());
    out.min = values.front();
    out.max = values.back();
    out.p50 = values[values.size() / 2U];
    out.p90 = values[(values.size() * 9U - 1U) / 10U];
    return out;
}

struct CaseMeta
{
    std::string case_id;
    std::string path = "gallery_viewer_develop";
    std::string unit = "us";
    std::string cache_state = "warm";
    std::string source_kind;
    std::size_t file_count = 0;
    std::size_t warmups = kDefaultWarmups;
    std::size_t recorded_samples = kDefaultRecordedSamples;
    std::string asset_id;
    std::string catalog_path;
    std::string host;
    std::string storage;
    std::string power_state;
    std::string gpu_backend;
    std::optional<std::int64_t> workers;
    std::optional<std::int64_t> peak_owned_bytes;
    std::optional<double> display_refresh_hz;
    std::optional<std::uint32_t> max_edge;
};

[[nodiscard]] inline std::string env_or_empty(const char *name)
{
    const char *value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string{value};
}

[[nodiscard]] inline CaseMeta enrich_from_env(CaseMeta meta)
{
    if (meta.host.empty())
        meta.host = env_or_empty("RAVO_INTERACTIVE_PERF_HOST");
    if (meta.storage.empty())
        meta.storage = env_or_empty("RAVO_INTERACTIVE_PERF_STORAGE");
    if (meta.power_state.empty())
        meta.power_state = env_or_empty("RAVO_INTERACTIVE_PERF_POWER_STATE");
    if (meta.gpu_backend.empty())
        meta.gpu_backend = env_or_empty("RAVO_INTERACTIVE_PERF_GPU_BACKEND");
    if (!meta.workers.has_value())
    {
        if (const auto raw = env_or_empty("RAVO_INTERACTIVE_PERF_WORKERS"); !raw.empty())
            meta.workers = std::stoll(raw);
    }
    if (!meta.peak_owned_bytes.has_value())
    {
        if (const auto raw = env_or_empty("RAVO_INTERACTIVE_PERF_PEAK_OWNED_BYTES"); !raw.empty())
            meta.peak_owned_bytes = std::stoll(raw);
    }
    if (!meta.display_refresh_hz.has_value())
    {
        if (const auto raw = env_or_empty("RAVO_INTERACTIVE_PERF_DISPLAY_REFRESH_HZ"); !raw.empty())
            meta.display_refresh_hz = std::stod(raw);
    }
    if (meta.catalog_path.empty())
        meta.catalog_path = env_or_empty("RAVO_INTERACTIVE_PERF_CATALOG");
    if (meta.asset_id.empty())
        meta.asset_id = env_or_empty("RAVO_INTERACTIVE_PERF_ASSET_ID");
    return meta;
}

[[nodiscard]] inline JsonValue case_to_json(const CaseMeta &meta, const PercentileSummary &summary,
                                            const std::vector<std::int64_t> &samples)
{
    JsonValue::Object object{
        {"schema", std::string(kSchema)},
        {"case", meta.case_id},
        {"path", meta.path},
        {"unit", meta.unit},
        {"warmups", JsonValue::number(std::to_string(meta.warmups))},
        {"recorded_samples", JsonValue::number(std::to_string(meta.recorded_samples))},
        {"sample_count", JsonValue::number(std::to_string(samples.size()))},
        {"min", JsonValue::number(std::to_string(summary.min))},
        {"p50", JsonValue::number(std::to_string(summary.p50))},
        {"p90", JsonValue::number(std::to_string(summary.p90))},
        {"max", JsonValue::number(std::to_string(summary.max))},
        {"cache_state", meta.cache_state},
        {"source_kind", meta.source_kind},
        {"file_count", JsonValue::number(std::to_string(meta.file_count))},
        {"asset_id", meta.asset_id},
        {"catalog_path", meta.catalog_path},
        {"host", meta.host},
        {"storage", meta.storage},
        {"power_state", meta.power_state},
        {"gpu_backend", meta.gpu_backend},
    };
    if (meta.workers)
        object.emplace("workers", JsonValue::number(std::to_string(*meta.workers)));
    else
        object.emplace("workers", nullptr);
    if (meta.peak_owned_bytes)
        object.emplace("peak_owned_bytes",
                       JsonValue::number(std::to_string(*meta.peak_owned_bytes)));
    else
        object.emplace("peak_owned_bytes", nullptr);
    if (meta.display_refresh_hz)
        object.emplace("display_refresh_hz",
                       JsonValue::number(std::to_string(*meta.display_refresh_hz)));
    else
        object.emplace("display_refresh_hz", nullptr);
    if (meta.max_edge)
        object.emplace("max_edge", JsonValue::number(std::to_string(*meta.max_edge)));
    JsonValue::Array sample_array;
    sample_array.reserve(samples.size());
    for (const auto sample : samples)
        sample_array.push_back(JsonValue::number(std::to_string(sample)));
    object.emplace("samples", std::move(sample_array));
    return JsonValue{std::move(object)};
}

inline void emit_case(const CaseMeta &raw_meta, const std::vector<std::int64_t> &samples,
                      std::ostream &out = std::cerr)
{
    const auto meta = enrich_from_env(raw_meta);
    const auto summary = summarize(samples);
    const auto json = case_to_json(meta, summary, samples);
    out << "perf01_report " << serialize_json(json) << '\n';
    out << "perf01_case=" << meta.case_id << " path=" << meta.path
        << " cache_state=" << meta.cache_state << " source_kind=" << meta.source_kind
        << " unit=" << meta.unit << " warmups=" << meta.warmups << " samples=" << samples.size()
        << " p50=" << summary.p50 << " p90=" << summary.p90 << " max=" << summary.max
        << " min=" << summary.min << '\n';

    const auto report_path = env_or_empty("RAVO_INTERACTIVE_PERF_REPORT_PATH");
    if (report_path.empty())
        return;
    std::error_code ec;
    const std::filesystem::path path{report_path};
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::app);
    if (!file)
        return;
    file << serialize_json(json) << '\n';
}

[[nodiscard]] inline std::size_t warmups_from_env(std::size_t fallback = kDefaultWarmups)
{
    if (const char *raw = std::getenv("RAVO_INTERACTIVE_PERF_WARMUPS"))
        return static_cast<std::size_t>(std::stoul(raw));
    return fallback;
}

[[nodiscard]] inline std::size_t
recorded_samples_from_env(std::size_t fallback = kDefaultRecordedSamples)
{
    if (const char *raw = std::getenv("RAVO_INTERACTIVE_PERF_RECORDED_SAMPLES"))
        return static_cast<std::size_t>(std::stoul(raw));
    // Prefer explicit recorded-sample count; fall back to legacy RUNS when set.
    if (const char *raw = std::getenv("RAVO_INTERACTIVE_PERF_RUNS"))
        return static_cast<std::size_t>(std::stoul(raw));
    return fallback;
}

} // namespace ravo::interactive_perf_report
