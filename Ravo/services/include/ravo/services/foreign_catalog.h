#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <utility>
#include <map>

#include "ravo/domain/types.h"
#include "ravo/domain/foreign_catalog.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"
#include "ravo/recipe/crs_types.h"

namespace ravo
{

// Foreign catalog conversion (ADR-0131/0164). Closed Lightroom SQLite catalogs
// and fixture/v1 documents are supported. Capture One binaries remain unsupported.
inline constexpr std::string_view kForeignCatalogFixtureContractVersion =
    "ravo.foreign-catalog.fixture/v1";
inline constexpr std::int64_t kForeignCatalogFixtureSchemaVersion = 1;

struct ForeignCatalogInspectionPhoto
{
    std::string foreign_id;
    std::string original_path;
    std::optional<std::string> master_id;
};

struct ForeignCatalogInspection
{
    std::string source_path;
    std::string source_sha256;
    std::size_t photos = 0, virtual_copies = 0, current_edits = 0;
    std::size_t history_steps = 0, snapshots = 0, collections = 0;
    std::size_t metadata_photos = 0, available_originals = 0, malformed_edits = 0;
    std::map<std::string, std::size_t> develop_fields;
    std::map<std::string, std::size_t> camera_profiles;
    std::vector<std::string> archived_only_tables;
    std::vector<ForeignCatalogInspectionPhoto> sample_photos;
    std::map<std::string, ForeignCatalogInspectionPhoto> editing_samples;
    std::vector<std::map<std::string, std::string>> malformed_samples;
    std::string companion_path;
    bool companion_present = false;
    bool companion_is_directory = false;
};

enum class ForeignCatalogSourceKind : std::uint8_t
{
    kLightroomClassic = 0,
    kCaptureOne = 1,
};

enum class ForeignCatalogItemStatus : std::uint8_t
{
    kImported = 0,
    kSkipped = 1,
    kUnsupported = 2,
    kFailed = 3,
};

struct ForeignCatalogFileFingerprint
{
    std::string path;
    std::string sha256;
    std::uint64_t size_bytes = 0;
    std::int64_t mtime_unix_ms = 0;
};

struct ForeignCatalogItemReport
{
    std::string foreign_id;
    std::optional<std::string> original_path;
    std::optional<std::string> asset_id;
    ForeignCatalogItemStatus status = ForeignCatalogItemStatus::kFailed;
    std::vector<std::string> mapped_fields;
    std::vector<CrsOmission> unsupported_fields;
    std::vector<std::string> reasons;
};

struct ForeignCatalogCollectionReport
{
    std::string foreign_id;
    std::optional<std::string> set_id;
    std::string name;
    std::vector<std::string> reasons;
    std::size_t imported_members = 0;
};

struct ForeignCatalogConversionReport
{
    std::string schema{std::string(kForeignCatalogFixtureContractVersion)};
    std::int64_t schema_version = kForeignCatalogFixtureSchemaVersion;
    ForeignCatalogSourceKind source_kind = ForeignCatalogSourceKind::kLightroomClassic;
    std::optional<std::string> source_product_version;
    std::string source_path;
    std::string destination_catalog;
    std::size_t imported = 0;
    std::size_t skipped = 0;
    std::size_t unsupported = 0;
    std::size_t failed = 0;
    std::size_t unsupported_fields = 0;
    bool originals_unchanged = true;
    bool cancelled = false;
    std::vector<ForeignCatalogFileFingerprint> source_originals;
    std::vector<ForeignCatalogItemReport> items;
    std::vector<ForeignCatalogCollectionReport> collections;
    std::optional<ForeignCatalogArchive> source_archive;
    std::vector<std::string> archived_only_tables;
    std::size_t source_photo_count = 0;
    std::size_t selected_photo_count = 0;
};

struct ForeignCatalogConversionRequest
{
    std::string source_path;
    std::optional<ForeignCatalogSourceKind> source_kind;
    ImportTransferMode mode = ImportTransferMode::kAdd;
    ImportPreviewPolicy preview = ImportPreviewPolicy::kMinimal;
    bool defer_previews = true;
    CancellationToken cancellation{};
    // Explicit segment-prefix mappings, longest match wins. A mapped path never
    // falls back to the foreign path when the local volume is unavailable.
    std::vector<std::pair<std::string, std::string>> path_mappings;
    // Empty selects the complete source. Explicit IDs are validated before any
    // write; a selected copy must include its master in the same selection.
    std::vector<std::string> foreign_ids;
    // Required with explicit IDs; binds them to the observed immutable source.
    std::string expected_source_sha256;
};

[[nodiscard]] constexpr std::string_view
foreign_catalog_source_kind_name(const ForeignCatalogSourceKind kind) noexcept
{
    switch (kind)
    {
    case ForeignCatalogSourceKind::kLightroomClassic:
        return "lightroom-classic";
    case ForeignCatalogSourceKind::kCaptureOne:
        return "capture-one";
    }
    return "lightroom-classic";
}

[[nodiscard]] constexpr std::string_view
foreign_catalog_item_status_name(const ForeignCatalogItemStatus status) noexcept
{
    switch (status)
    {
    case ForeignCatalogItemStatus::kImported:
        return "imported";
    case ForeignCatalogItemStatus::kSkipped:
        return "skipped";
    case ForeignCatalogItemStatus::kUnsupported:
        return "unsupported";
    case ForeignCatalogItemStatus::kFailed:
        return "failed";
    }
    return "failed";
}

[[nodiscard]] Result<ForeignCatalogSourceKind>
parse_foreign_catalog_source_kind(std::string_view text);

} // namespace ravo
