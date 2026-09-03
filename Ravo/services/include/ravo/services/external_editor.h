#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "ravo/domain/types.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"

namespace ravo
{

// Versioned external-editor derived-asset provenance (ADR-0122).
inline constexpr std::string_view kExternalEditorDerivedContractVersion =
    "ravo.external-editor.derived/v1";
inline constexpr std::int64_t kExternalEditorDerivedSchemaVersion = 1;

struct ExternalEditorFileFingerprint
{
    std::string sha256;
    std::uint64_t size_bytes = 0;
    std::int64_t mtime_unix_ms = 0;
};

struct ExternalEditorProvenance
{
    std::string schema{std::string(kExternalEditorDerivedContractVersion)};
    std::int64_t schema_version = kExternalEditorDerivedSchemaVersion;
    std::string derived_asset_id;
    std::string source_asset_id;
    std::int64_t observed_catalog_revision = 0;
    std::int64_t source_recovery_generation = 0;
    ExternalEditorFileFingerprint source_original;
    ExternalEditorFileFingerprint derived;
    std::string editor_id;
    std::optional<std::string> editor_version;
    std::int64_t registered_unix_ms = 0;
    std::string derived_path;
    std::string source_original_path;
};

struct ExternalEditorRegisterRequest
{
    std::string source_asset_id;
    std::string editor_output_path;
    std::string editor_id;
    std::optional<std::string> editor_version;
    std::optional<std::string> destination_directory;
    std::optional<std::int64_t> expected_catalog_revision;
    CancellationToken cancellation{};
};

struct ExternalEditorRegisterResult
{
    AssetRecord derived_asset;
    ExternalEditorProvenance provenance;
    bool source_original_unchanged = true;
};

} // namespace ravo
