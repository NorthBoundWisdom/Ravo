#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"

namespace ravo
{

// ADR-0146: offline-edit proxy distinct from ADR-0141 browse-only Smart Preview.
inline constexpr std::string_view kOfflineEditProxyContractVersion = "ravo.offline-edit-proxy/v1";
inline constexpr std::int64_t kOfflineEditProxySchemaVersion = 1;
inline constexpr std::uint32_t kOfflineEditProxyDefaultMaxEdge = 2048;

enum class OfflineEditMediaState : std::uint8_t
{
    kOriginal = 0,
    kProxy = 1,
    kPlaceholder = 2,
    kMissing = 3,
};

[[nodiscard]] inline std::string_view
offline_edit_media_state_name(const OfflineEditMediaState state) noexcept
{
    switch (state)
    {
    case OfflineEditMediaState::kOriginal:
        return "original";
    case OfflineEditMediaState::kProxy:
        return "proxy";
    case OfflineEditMediaState::kPlaceholder:
        return "placeholder";
    case OfflineEditMediaState::kMissing:
        return "missing";
    }
    return "missing";
}

struct OfflineEditProxyManifest
{
    std::string schema{std::string(kOfflineEditProxyContractVersion)};
    std::int64_t schema_version = kOfflineEditProxySchemaVersion;
    std::string asset_id;
    std::string source_sha256;
    std::uint64_t source_size_bytes = 0;
    std::int64_t source_mtime_unix_ms = 0;
    std::string recipe_cache_key{"baseline"};
    std::uint32_t max_edge = kOfflineEditProxyDefaultMaxEdge;
    std::string profile{"srgb"};
    std::string proxy_path;
    std::string proxy_sha256;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::int64_t created_unix_ms = 0;
};

struct OfflineEditProxyCreateRequest
{
    std::string asset_id;
    std::uint32_t max_edge = kOfflineEditProxyDefaultMaxEdge;
    std::string profile{"srgb"};
    bool user_initiated = false;
    CancellationToken cancellation{};
};

struct OfflineEditProxyCreateResult
{
    OfflineEditProxyManifest manifest;
    bool originals_unchanged = true;
};

struct OfflineEditProxyStatus
{
    std::string schema{std::string(kOfflineEditProxyContractVersion)};
    std::string asset_id;
    OfflineEditMediaState media_state = OfflineEditMediaState::kMissing;
    bool proxy_present = false;
    bool proxy_verified = false;
    bool usable_for_develop = false;
    bool usable_for_export = false;
    std::optional<OfflineEditProxyManifest> manifest;
    std::string reason;
};

struct OfflineEditProxyReconnectRequest
{
    std::string asset_id;
    bool user_initiated = false;
    CancellationToken cancellation{};
};

struct OfflineEditProxyReconnectResult
{
    OfflineEditProxyStatus status;
    bool source_hash_matched = false;
    bool originals_unchanged = true;
    bool offline_states_cleared = false;
};

} // namespace ravo
