#include "catalog_cull_fingerprint_store.h"

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

#include "catalog_internal.h"
#include "ravo/adapters/text_file.h"
#include "ravo/foundation/json.h"

namespace ravo::cull_fingerprint_store
{
namespace
{

[[nodiscard]] Result<std::int64_t> require_i64(const JsonValue::Object &object, const char *key)
{
    const auto found = object.find(key);
    if (found == object.end() || found->second.number_if() == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Cull fingerprint cache field missing",
                          {{"field", key}, {"reason", "corrupt_cull_fingerprint_cache"}});
    }
    return std::stoll(found->second.number_if()->text);
}

[[nodiscard]] Result<std::string> require_string(const JsonValue::Object &object, const char *key)
{
    const auto found = object.find(key);
    if (found == object.end() || found->second.string_if() == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Cull fingerprint cache field missing",
                          {{"field", key}, {"reason", "corrupt_cull_fingerprint_cache"}});
    }
    return *found->second.string_if();
}

} // namespace

std::string source_identity_for_asset(const std::uint64_t size_bytes,
                                      const std::int64_t mtime_unix_ms,
                                      const std::string_view normalized_uri,
                                      const std::optional<std::string_view> content_fp)
{
    std::string out = "size=";
    out += std::to_string(size_bytes);
    out += ";mtime=";
    out += std::to_string(mtime_unix_ms);
    out += ";fp=";
    if (content_fp)
        out += *content_fp;
    out += ";uri=";
    out += normalized_uri;
    return out;
}

std::string near_dup_group_dismiss_key(const std::string_view left_hex,
                                       const std::string_view right_hex)
{
    if (left_hex <= right_hex)
        return std::string(left_hex) + "|" + std::string(right_hex);
    return std::string(right_hex) + "|" + std::string(left_hex);
}

std::string dismiss_map_key(const CullSuggestionKind kind, const std::string_view key)
{
    return std::string(cull_suggestion_kind_name(kind)) + ":" + std::string(key);
}

Result<std::filesystem::path> cull_support_directory(const std::string_view database_path)
{
    if (database_path.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "Cull support path requires catalog path",
                          {{"reason", "cull_support_path_required"}});
    }
    return std::filesystem::path(std::string(database_path)).concat(".cull");
}

Result<Store> load_or_empty(const std::string_view database_path)
{
    Store store;
    auto directory = cull_support_directory(database_path);
    if (!directory)
        return directory.error();
    const auto path = directory.value() / "fingerprint_cache.v1.json";
    std::error_code exists_error;
    if (!std::filesystem::exists(path, exists_error))
        return store;

    auto text = read_utf8_text_file(path.string());
    if (!text)
    {
        // Corrupt/unreadable cache fails closed to empty (rebuildable).
        return store;
    }
    auto parsed = parse_json(text.value());
    if (!parsed || parsed.value().object_if() == nullptr)
        return store;
    const auto &object = *parsed.value().object_if();
    auto schema = require_string(object, "schema");
    if (!schema || schema.value() != kCullFingerprintCacheContractVersion)
        return store;
    auto algorithm = require_string(object, "algorithm");
    if (!algorithm || algorithm.value() != kCullNearDupFingerprintAlgorithm)
        return store;
    if (const auto max = object.find("max_entries");
        max != object.end() && max->second.number_if() != nullptr)
    {
        const auto value = std::stoll(max->second.number_if()->text);
        if (value > 0)
            store.max_entries = static_cast<std::size_t>(value);
    }
    if (const auto last = object.find("last_scan_unix_ms");
        last != object.end() && last->second.number_if() != nullptr)
    {
        store.last_scan_unix_ms = std::stoll(last->second.number_if()->text);
    }
    if (const auto entries = object.find("entries");
        entries != object.end() && entries->second.object_if() != nullptr)
    {
        for (const auto &[asset_id, value] : *entries->second.object_if())
        {
            const auto *entry_object = value.object_if();
            if (entry_object == nullptr)
                continue;
            auto fingerprint = require_string(*entry_object, "fingerprint_hex");
            auto identity = require_string(*entry_object, "source_identity");
            auto updated = require_i64(*entry_object, "updated_unix_ms");
            if (!fingerprint || !identity || !updated)
                continue;
            FingerprintEntry entry;
            entry.fingerprint_hex = std::move(fingerprint).value();
            entry.source_identity = std::move(identity).value();
            entry.updated_unix_ms = updated.value();
            entry.access_unix_ms = entry.updated_unix_ms;
            if (const auto access = entry_object->find("access_unix_ms");
                access != entry_object->end() && access->second.number_if() != nullptr)
            {
                entry.access_unix_ms = std::stoll(access->second.number_if()->text);
            }
            store.entries.insert_or_assign(asset_id, std::move(entry));
        }
    }
    if (const auto dismissed = object.find("dismissed");
        dismissed != object.end() && dismissed->second.object_if() != nullptr)
    {
        for (const auto &[key, value] : *dismissed->second.object_if())
        {
            if (value.number_if() == nullptr)
                continue;
            store.dismissed.insert_or_assign(key, std::stoll(value.number_if()->text));
        }
    }
    return store;
}

Result<void> save(const std::string_view database_path, Store &store)
{
    auto directory = cull_support_directory(database_path);
    if (!directory)
        return directory.error();
    std::error_code ec;
    std::filesystem::create_directories(directory.value(), ec);
    if (ec)
    {
        return make_error(ErrorCode::kIo, "Failed to create cull support directory",
                          {{"path", directory.value().string()}, {"reason", ec.message()}});
    }
    evict_to_bound(store);
    JsonValue::Object entries;
    for (const auto &[asset_id, entry] : store.entries)
    {
        entries.emplace(
            asset_id,
            JsonValue{JsonValue::Object{
                {"fingerprint_hex", entry.fingerprint_hex},
                {"source_identity", entry.source_identity},
                {"updated_unix_ms", JsonValue::number(std::to_string(entry.updated_unix_ms))},
                {"access_unix_ms", JsonValue::number(std::to_string(entry.access_unix_ms))},
            }});
    }
    JsonValue::Object dismissed;
    for (const auto &[key, when] : store.dismissed)
        dismissed.emplace(key, JsonValue::number(std::to_string(when)));

    const JsonValue document{JsonValue::Object{
        {"schema", store.schema},
        {"algorithm", store.algorithm},
        {"max_entries", JsonValue::number(std::to_string(store.max_entries))},
        {"last_scan_unix_ms", JsonValue::number(std::to_string(store.last_scan_unix_ms))},
        {"entries", JsonValue{std::move(entries)}},
        {"dismissed", JsonValue{std::move(dismissed)}},
    }};
    const auto path = directory.value() / "fingerprint_cache.v1.json";
    auto written = write_utf8_text_file_replace_atomically(path.string(), serialize_json(document));
    if (!written)
        return written.error();
    store.dirty = false;
    return {};
}

void upsert_fingerprint(Store &store, const std::string_view asset_id,
                        const std::string_view fingerprint_hex,
                        const std::string_view source_identity, const std::int64_t now_unix_ms)
{
    FingerprintEntry entry;
    entry.fingerprint_hex = std::string(fingerprint_hex);
    entry.source_identity = std::string(source_identity);
    entry.updated_unix_ms = now_unix_ms;
    entry.access_unix_ms = now_unix_ms;
    store.entries.insert_or_assign(std::string(asset_id), std::move(entry));
    store.dirty = true;
    evict_to_bound(store);
}

void touch_access(Store &store, const std::string_view asset_id, const std::int64_t now_unix_ms)
{
    const auto found = store.entries.find(std::string(asset_id));
    if (found == store.entries.end())
        return;
    found->second.access_unix_ms = now_unix_ms;
    store.dirty = true;
}

void evict_to_bound(Store &store)
{
    if (store.max_entries == 0U || store.entries.size() <= store.max_entries)
        return;
    std::vector<std::pair<std::int64_t, std::string>> order;
    order.reserve(store.entries.size());
    for (const auto &[asset_id, entry] : store.entries)
        order.push_back({entry.access_unix_ms, asset_id});
    std::sort(order.begin(), order.end());
    const auto excess = store.entries.size() - store.max_entries;
    for (std::size_t i = 0; i < excess; ++i)
    {
        store.entries.erase(order[i].second);
        store.dirty = true;
    }
}

bool is_dismissed(const Store &store, const CullSuggestionKind kind, const std::string_view key)
{
    return store.dismissed.find(dismiss_map_key(kind, key)) != store.dismissed.end();
}

void dismiss(Store &store, const CullSuggestionKind kind, const std::string_view key,
             const std::int64_t now_unix_ms)
{
    store.dismissed.insert_or_assign(dismiss_map_key(kind, key), now_unix_ms);
    store.dirty = true;
}

} // namespace ravo::cull_fingerprint_store
