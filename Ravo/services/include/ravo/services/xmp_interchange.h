#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/foundation/error.h"
#include "ravo/domain/types.h"
#include "ravo/recipe/crs_types.h"

namespace ravo
{

enum class XmpInterchangeConflictClass : std::uint8_t
{
    kMissing = 0,
    kIdentical,
    kCatalogNewer,
    kSidecarNewer,
    kBothChanged,
};

enum class XmpInterchangeResolve : std::uint8_t
{
    kAbort = 0,
    kCatalog,
    kSidecar,
};

[[nodiscard]] constexpr std::string_view
xmp_interchange_conflict_class_name(XmpInterchangeConflictClass value) noexcept
{
    switch (value)
    {
    case XmpInterchangeConflictClass::kMissing:
        return "missing";
    case XmpInterchangeConflictClass::kIdentical:
        return "identical";
    case XmpInterchangeConflictClass::kCatalogNewer:
        return "catalog-newer";
    case XmpInterchangeConflictClass::kSidecarNewer:
        return "sidecar-newer";
    case XmpInterchangeConflictClass::kBothChanged:
        return "both-changed";
    }
    return "missing";
}

[[nodiscard]] constexpr std::string_view
xmp_interchange_resolve_name(XmpInterchangeResolve value) noexcept
{
    switch (value)
    {
    case XmpInterchangeResolve::kAbort:
        return "abort";
    case XmpInterchangeResolve::kCatalog:
        return "catalog";
    case XmpInterchangeResolve::kSidecar:
        return "sidecar";
    }
    return "abort";
}

[[nodiscard]] Result<XmpInterchangeResolve> parse_xmp_interchange_resolve(std::string_view text);

struct XmpInterchangeCatalogFingerprint
{
    std::int64_t recovery_generation = 0;
    std::string recipe_sha256;
};

struct XmpInterchangeSidecarFingerprint
{
    std::string sha256;
    std::uint64_t size_bytes = 0;
    std::int64_t mtime_unix_ms = 0;
};

struct XmpInterchangeStatus
{
    std::string asset_id;
    std::string original_path;
    std::optional<std::string> sidecar_path;
    XmpInterchangeConflictClass conflict_class = XmpInterchangeConflictClass::kMissing;
    bool has_baseline = false;
    bool has_edits = false;
    XmpInterchangeCatalogFingerprint catalog;
    std::optional<XmpInterchangeSidecarFingerprint> sidecar;
    std::optional<XmpInterchangeCatalogFingerprint> baseline_catalog;
    std::optional<XmpInterchangeSidecarFingerprint> baseline_sidecar;
    bool crs_parse_ok = false;
    std::optional<std::string> crs_parse_reason;
    std::vector<CrsOmission> omitted;
};

struct XmpInterchangeImportResult
{
    AssetRecord asset;
    XmpInterchangeStatus status;
    std::string preset_name;
    std::vector<CrsOmission> omitted;
};

struct XmpInterchangeExportResult
{
    AssetRecord asset;
    XmpInterchangeStatus status;
    std::string sidecar_path;
    std::vector<std::string> omitted_catalog_fields;
};

} // namespace ravo
