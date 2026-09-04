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
// COR-01 / ADR-0146: create-time export is an 8-bit sRGB presentation raster with
// the then-current recipe already baked. Develop must apply identity (no
// double-grade) while media_state=proxy for this provenance.
inline constexpr std::string_view kOfflineEditProxyPixelProvenanceRecipeBakedSrgb8 =
    "recipe_baked_srgb8";
inline constexpr std::string_view kOfflineEditPreviewApplyIdentityBaked = "identity_baked";
inline constexpr std::string_view kOfflineEditPreviewApplyCatalogRecipe = "catalog_recipe";

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
    // "recipe_baked_srgb8" => presentation pixels; Develop uses identity recipe.
    std::string pixel_provenance{std::string(kOfflineEditProxyPixelProvenanceRecipeBakedSrgb8)};
    bool pinned = false;
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

struct OfflineEditProxyCorruptEntry
{
    std::string asset_id;
    std::string path;
    std::string reason;
};

struct OfflineEditProxyListReport
{
    std::vector<OfflineEditProxyManifest> manifests;
    std::vector<OfflineEditProxyCorruptEntry> corrupt;
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
    bool clear_proxy = false;
    CancellationToken cancellation{};
};

struct OfflineEditProxyReconnectResult
{
    OfflineEditProxyStatus status;
    bool source_hash_matched = false;
    bool originals_unchanged = true;
    bool offline_states_cleared = false;
    bool proxy_cleared = false;
};

struct OfflineEditProxyDeleteRequest
{
    std::string asset_id;
    bool user_initiated = false;
    bool force = false;
    CancellationToken cancellation{};
};

struct OfflineEditProxyDeleteResult
{
    bool deleted = false;
    bool originals_unchanged = true;
    std::string reason;
};

struct OfflineEditProxyPinRequest
{
    std::string asset_id;
    bool pinned = true;
    bool user_initiated = false;
    CancellationToken cancellation{};
};

struct OfflineEditProxyPinResult
{
    OfflineEditProxyManifest manifest;
};

struct OfflineEditProxyEvictRequest
{
    bool user_initiated = false;
    std::uint64_t max_total_bytes = 0;
    CancellationToken cancellation{};
};

struct OfflineEditProxyEvictResult
{
    std::size_t evicted = 0;
    std::size_t retained_pinned = 0;
    std::uint64_t bytes_retained = 0;
    std::vector<std::string> evicted_asset_ids;
    std::vector<std::string> retained_pinned_asset_ids;
};

} // namespace ravo
