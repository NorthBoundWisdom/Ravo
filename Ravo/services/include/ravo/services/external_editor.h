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

// ADR-0139: user-initiated OS open-with intent (service records; UI/CLI may
// invoke platform open only after explicit user action).
inline constexpr std::string_view kExternalEditorOpenIntentContractVersion =
    "ravo.external-editor.open-intent/v1";
inline constexpr std::int64_t kExternalEditorOpenIntentSchemaVersion = 1;

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
    // ADR-0139: after successful register, stack source+derived (pick=derived).
    // Fail-closed on stack conflict; derived publication is retained.
    bool auto_stack = false;
    CancellationToken cancellation{};
};

struct ExternalEditorRegisterResult
{
    AssetRecord derived_asset;
    ExternalEditorProvenance provenance;
    bool source_original_unchanged = true;
    bool auto_stacked = false;
    std::optional<LibraryStackMutation> stack;
};

enum class ExternalEditorOpenKind
{
    kOriginal = 0,
    kDerivedWorkingCopy = 1,
};

[[nodiscard]] inline std::string_view
external_editor_open_kind_name(const ExternalEditorOpenKind kind) noexcept
{
    switch (kind)
    {
    case ExternalEditorOpenKind::kDerivedWorkingCopy:
        return "derived_working_copy";
    case ExternalEditorOpenKind::kOriginal:
        return "original";
    }
    return "original";
}

struct ExternalEditorOpenIntent
{
    std::string schema{std::string(kExternalEditorOpenIntentContractVersion)};
    std::int64_t schema_version = kExternalEditorOpenIntentSchemaVersion;
    std::string intent_id;
    std::string asset_id;
    std::string source_asset_id;
    std::string open_path;
    std::string open_uri;
    ExternalEditorOpenKind open_kind = ExternalEditorOpenKind::kOriginal;
    std::optional<std::string> editor_id;
    std::int64_t recorded_unix_ms = 0;
    std::int64_t observed_catalog_revision = 0;
};

struct ExternalEditorOpenRequest
{
    std::string asset_id;
    std::optional<std::string> editor_id;
    std::optional<std::int64_t> expected_catalog_revision;
    bool user_initiated = false;
    CancellationToken cancellation{};
};

struct ExternalEditorOpenResult
{
    ExternalEditorOpenIntent intent;
};

// EDITIN-01: explicit TIFF working-copy create + check-returned (no watch-folder).
inline constexpr std::string_view kExternalEditorWorkingCopyContractVersion =
    "ravo.external-editor.working-copy/v1";
inline constexpr std::int64_t kExternalEditorWorkingCopySchemaVersion = 1;

struct ExternalEditorWorkingCopyRequest
{
    std::string asset_id;
    std::string editor_id;
    std::optional<std::string> editor_version;
    std::optional<std::string> application_path;
    TiffSampleType tiff_sample_type = TiffSampleType::kUint16;
    std::string profile{"srgb"};
    std::optional<std::uint32_t> max_edge;
    bool auto_stack = true;
    bool user_initiated = false;
    std::optional<std::int64_t> expected_catalog_revision;
    CancellationToken cancellation{};
};

struct ExternalEditorWorkingCopySession
{
    std::string schema{std::string(kExternalEditorWorkingCopyContractVersion)};
    std::int64_t schema_version = kExternalEditorWorkingCopySchemaVersion;
    std::string working_copy_id;
    std::string source_asset_id;
    std::string editor_id;
    std::optional<std::string> editor_version;
    std::string working_path;
    std::string working_uri;
    ExternalEditorFileFingerprint source_original;
    ExternalEditorFileFingerprint working_copy;
    TiffSampleType tiff_sample_type = TiffSampleType::kUint16;
    std::string profile{"srgb"};
    std::optional<std::uint32_t> max_edge;
    bool auto_stack = true;
    std::int64_t created_unix_ms = 0;
    std::int64_t observed_catalog_revision = 0;
    std::optional<std::string> open_intent_id;
};

struct ExternalEditorWorkingCopyResult
{
    ExternalEditorWorkingCopySession session;
    bool originals_unchanged = true;
};

struct ExternalEditorCheckReturnedRequest
{
    std::string working_copy_id;
    std::optional<std::string> returned_path;
    std::optional<std::int64_t> expected_catalog_revision;
    CancellationToken cancellation{};
};

struct ExternalEditorCheckReturnedResult
{
    ExternalEditorRegisterResult registration;
    ExternalEditorWorkingCopySession session;
};

// ADR-0154: reopen / abandon / conflict machine states for working-copy sessions.
enum class ExternalEditorWorkingCopyMachineState : std::uint8_t
{
    kPending = 0,
    kModified = 1,
    kMissingWorkingCopy = 2,
    kSourceConflict = 3,
    kStaleCatalog = 4,
};

[[nodiscard]] inline std::string_view external_editor_working_copy_machine_state_name(
    const ExternalEditorWorkingCopyMachineState state) noexcept
{
    switch (state)
    {
    case ExternalEditorWorkingCopyMachineState::kPending:
        return "pending";
    case ExternalEditorWorkingCopyMachineState::kModified:
        return "modified";
    case ExternalEditorWorkingCopyMachineState::kMissingWorkingCopy:
        return "missing_working_copy";
    case ExternalEditorWorkingCopyMachineState::kSourceConflict:
        return "source_conflict";
    case ExternalEditorWorkingCopyMachineState::kStaleCatalog:
        return "stale_catalog";
    }
    return "pending";
}

struct ExternalEditorWorkingCopyStatus
{
    std::string schema{std::string(kExternalEditorWorkingCopyContractVersion)};
    ExternalEditorWorkingCopySession session;
    ExternalEditorWorkingCopyMachineState machine_state =
        ExternalEditorWorkingCopyMachineState::kPending;
    bool working_copy_present = false;
    bool working_copy_modified = false;
    bool source_original_unchanged = true;
    bool catalog_revision_current = true;
    std::string reason{"pending"};
};

struct ExternalEditorAbandonRequest
{
    std::string working_copy_id;
    bool user_initiated = false;
    std::optional<std::int64_t> expected_catalog_revision;
    CancellationToken cancellation{};
};

struct ExternalEditorAbandonResult
{
    std::string working_copy_id;
    bool session_removed = false;
    bool working_copy_removed = false;
    bool originals_unchanged = true;
};

struct ExternalEditorReopenRequest
{
    std::string working_copy_id;
    bool user_initiated = false;
    CancellationToken cancellation{};
};

struct ExternalEditorReopenResult
{
    ExternalEditorWorkingCopyStatus status;
};

} // namespace ravo
