#pragma once

#include <optional>
#include <string>
#include <vector>

#include "ravo/domain/types.h"

namespace ravo
{
struct ForeignDevelopState
{
    std::string name;
    std::string settings;
    bool snapshot = false;
};

struct ForeignCatalogCollection
{
    std::string foreign_id;
    std::string name;
    std::optional<std::string> parent_id;
    std::string creation_id;
    std::vector<std::string> photo_ids;
};

// Owned values from a closed foreign catalog snapshot; no database handles escape.
struct ForeignCatalogPhoto
{
    std::string foreign_id;
    std::string original_path;
    std::optional<int> rating;
    std::optional<ColorLabel> color_label;
    std::optional<bool> rejected;
    std::optional<bool> picked;
    std::optional<std::string> master_id;
    std::string copy_name;
    WritableMetadata metadata;
    std::vector<std::string> keywords;
    std::optional<std::string> crs_xmp_path;
    std::optional<std::string> develop_settings;
    std::vector<ForeignDevelopState> develop_states;
    std::vector<std::string> unsupported_adjusts;
    std::optional<std::string> skip_reason;
};

struct ForeignCatalogSnapshot
{
    std::vector<ForeignCatalogPhoto> photos;
    std::vector<ForeignCatalogCollection> collections;
    std::string source_sha256;
    // Nonempty tables without an interpreted import owner. Their exact bytes
    // remain in the source archive; this is distinct from applied Ravo fields.
    std::vector<std::string> archived_only_tables;
};

struct ForeignCatalogArchive
{
    std::string source_id;
    std::string source_path;
    std::string sha256;
    std::uint64_t size_bytes = 0;
};
} // namespace ravo
