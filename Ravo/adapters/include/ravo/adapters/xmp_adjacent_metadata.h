#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/domain/types.h"
#include "ravo/foundation/error.h"
#include "ravo/recipe/develop.h"

namespace ravo
{

// Adjacent-XMP keyword / IPTC Core / location packets for ADR-0138 interchange.
// Catalog SQLite remains the sole live authority; this adapter only parses and
// serializes conversion artifacts.

struct XmpAdjacentMetadata
{
    WritableMetadata writable;
    // Present when lr:hierarchicalSubject or dc:subject appeared in the sidecar
    // (including an empty Bag → clear membership on import).
    std::optional<std::vector<std::string>> keyword_paths;
    bool has_any_writable_element = false;
};

struct XmpAdjacentMetadataParseResult
{
    XmpAdjacentMetadata metadata;
    bool parse_ok = false;
    std::optional<std::string> parse_reason;
};

// Deterministic SHA-256 hex of catalog-owned adjacent fields (Core + location +
// sorted keyword display paths). Empty/absent fields serialize stably.
[[nodiscard]] std::string
xmp_adjacent_metadata_fingerprint_sha256(const WritableMetadata &writable,
                                         const std::vector<std::string> &keyword_paths);

[[nodiscard]] bool
xmp_adjacent_metadata_catalog_has_content(const WritableMetadata &writable,
                                          const std::vector<std::string> &keyword_paths) noexcept;

// Parses dublin-core / photoshop / Iptc4xmpCore / lr:hierarchicalSubject from an
// adjacent XMP document. Fail-closed reasons include unsupported hierarchical
// keyword shapes. A document with none of those elements still parse_ok=true
// with empty metadata.
[[nodiscard]] XmpAdjacentMetadataParseResult parse_xmp_adjacent_metadata(std::string_view xmp_utf8);

struct XmpAdjacentExportRequest
{
    DevelopParams look;
    std::string_view preset_name = "Ravo";
    WritableMetadata writable;
    std::vector<std::string> keyword_paths;
};

struct XmpAdjacentExportResult
{
    std::string xmp_utf8;
    std::vector<std::string> omitted_catalog_fields;
};

// CRS PV2012 look subset plus catalog Core/location/keyword packets in one
// adjacent XMP document (ADR-0120 + ADR-0138).
[[nodiscard]] Result<XmpAdjacentExportResult>
export_xmp_adjacent_interchange(const XmpAdjacentExportRequest &request);

} // namespace ravo
