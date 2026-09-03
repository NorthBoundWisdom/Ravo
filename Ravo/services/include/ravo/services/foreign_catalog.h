#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/domain/types.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"
#include "ravo/recipe/crs_types.h"

namespace ravo
{

// Fixture-backed foreign catalog conversion (ADR-0131). First Ready tranche
// reads ravo.foreign-catalog.fixture/v1 documents only. Vendor .lrcat / Capture
// One session binaries fail closed — no Adobe/Phase One runtime in the default
// package.
inline constexpr std::string_view kForeignCatalogFixtureContractVersion =
    "ravo.foreign-catalog.fixture/v1";
inline constexpr std::int64_t kForeignCatalogFixtureSchemaVersion = 1;

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
};

struct ForeignCatalogConversionRequest
{
    std::string source_path;
    std::optional<ForeignCatalogSourceKind> source_kind;
    ImportTransferMode mode = ImportTransferMode::kAdd;
    ImportPreviewPolicy preview = ImportPreviewPolicy::kMinimal;
    bool defer_previews = true;
    CancellationToken cancellation{};
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
