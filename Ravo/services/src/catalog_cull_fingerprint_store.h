#pragma once

#include <cstddef>
#include <filesystem>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/foundation/error.h"
#include "ravo/services/cull_assistance.h"

namespace ravo::cull_fingerprint_store
{

struct FingerprintEntry
{
    std::string fingerprint_hex;
    std::string source_identity;
    std::int64_t updated_unix_ms = 0;
    std::int64_t access_unix_ms = 0;
};

struct Store
{
    std::string schema{std::string(kCullFingerprintCacheContractVersion)};
    std::string algorithm{std::string(kCullNearDupFingerprintAlgorithm)};
    std::size_t max_entries = kCullNearDupDefaultMaxAssets;
    std::int64_t last_scan_unix_ms = 0;
    std::map<std::string, FingerprintEntry, std::less<>> entries;
    std::map<std::string, std::int64_t, std::less<>> dismissed; // kind:key -> unix_ms
    bool dirty = false;
};

[[nodiscard]] std::string source_identity_for_asset(std::uint64_t size_bytes,
                                                    std::int64_t mtime_unix_ms,
                                                    std::string_view normalized_uri,
                                                    std::optional<std::string_view> content_fp);

[[nodiscard]] std::string near_dup_group_dismiss_key(std::string_view left_hex,
                                                     std::string_view right_hex);

[[nodiscard]] std::string dismiss_map_key(CullSuggestionKind kind, std::string_view key);

[[nodiscard]] Result<std::filesystem::path> cull_support_directory(std::string_view database_path);

[[nodiscard]] Result<Store> load_or_empty(std::string_view database_path);

[[nodiscard]] Result<void> save(std::string_view database_path, Store &store);

void upsert_fingerprint(Store &store, std::string_view asset_id, std::string_view fingerprint_hex,
                        std::string_view source_identity, std::int64_t now_unix_ms);

void touch_access(Store &store, std::string_view asset_id, std::int64_t now_unix_ms);

void evict_to_bound(Store &store);

[[nodiscard]] bool is_dismissed(const Store &store, CullSuggestionKind kind, std::string_view key);

void dismiss(Store &store, CullSuggestionKind kind, std::string_view key, std::int64_t now_unix_ms);

} // namespace ravo::cull_fingerprint_store
