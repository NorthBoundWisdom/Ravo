#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/domain/types.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"

namespace ravo
{

// ADR-0125 ingest transport contracts + ADR-0148 native PTP/MTP session,
// platform matrix, and resume checkpoints. Filesystem-card remains first-ship
// for mounts/DCIM. Live PTP/MTP fail closed until an adapter is packaged.
// ptp-stub is a fixture-only harness for resume/copy contracts.

inline constexpr std::string_view kIngestUriScheme = "ravo-ingest:";
inline constexpr std::string_view kIngestTransportFilesystemCard = "filesystem-card";
inline constexpr std::string_view kIngestTransportPtpUsb = "ptp-usb";
inline constexpr std::string_view kIngestTransportMtp = "mtp";
inline constexpr std::string_view kIngestTransportPtpStub = "ptp-stub";
inline constexpr std::string_view kFilesystemCardIngestUriPrefix = "ravo-ingest:filesystem-card:";
inline constexpr std::string_view kPtpUsbIngestUriPrefix = "ravo-ingest:ptp-usb:";
inline constexpr std::string_view kMtpIngestUriPrefix = "ravo-ingest:mtp:";
inline constexpr std::string_view kPtpStubIngestUriPrefix = "ravo-ingest:ptp-stub:";

inline constexpr std::string_view kNativeIngestContractVersion = "ravo.ingest.native-ptp-mtp/v1";
inline constexpr std::string_view kIngestResumeContractVersion = "ravo.ingest.resume/v1";
inline constexpr std::int64_t kIngestResumeSchemaVersion = 1;

enum class IngestTransportKind : std::uint8_t
{
    kFilesystemCard = 0,
    kPtpUsb = 1,
    kMtp = 2,
    kPtpStub = 3, // fixture-only; never claims packaged native support
};

enum class NativeIngestSupportState : std::uint8_t
{
    kUnsupported = 0,
    kAvailable = 1,
};

[[nodiscard]] inline std::string_view
native_ingest_support_state_name(const NativeIngestSupportState state) noexcept
{
    switch (state)
    {
    case NativeIngestSupportState::kUnsupported:
        return "unsupported";
    case NativeIngestSupportState::kAvailable:
        return "available";
    }
    return "unsupported";
}

[[nodiscard]] inline std::string_view
ingest_transport_kind_name(const IngestTransportKind kind) noexcept
{
    switch (kind)
    {
    case IngestTransportKind::kFilesystemCard:
        return kIngestTransportFilesystemCard;
    case IngestTransportKind::kPtpUsb:
        return kIngestTransportPtpUsb;
    case IngestTransportKind::kMtp:
        return kIngestTransportMtp;
    case IngestTransportKind::kPtpStub:
        return kIngestTransportPtpStub;
    }
    return kIngestTransportFilesystemCard;
}

struct NativeIngestPlatformSupport
{
    std::string schema{std::string(kNativeIngestContractVersion)};
    std::string platform; // macos | windows | linux | unknown
    NativeIngestSupportState ptp_usb = NativeIngestSupportState::kUnsupported;
    NativeIngestSupportState mtp = NativeIngestSupportState::kUnsupported;
    bool adapter_packaged = false;
    std::string reason{"native_ingest_adapter_not_packaged"};
    std::string ptp_planned_stack;
    std::string mtp_planned_stack;
};

struct PtpMtpSessionIdentity
{
    IngestTransportKind transport = IngestTransportKind::kPtpUsb;
    std::string vendor_id;
    std::string product_id;
    std::string serial;
    std::string display_name;
    std::string uri;
};

struct IngestSourceUri
{
    IngestTransportKind transport = IngestTransportKind::kFilesystemCard;
    std::string root; // absolute filesystem root for filesystem-card / ptp-stub
    std::string uri;  // canonical ravo-ingest:… serialization
    std::optional<PtpMtpSessionIdentity> session;
};

struct IngestObjectId
{
    // Portable relative path under the enumeration root (filesystem-card /
    // ptp-stub). Native adapters map storage/object handles onto this shape.
    std::string relative_path;
    std::string absolute_path;
    std::optional<std::string> object_handle;
    std::optional<std::uint64_t> size_bytes;
};

struct IngestSourceSnapshot
{
    IngestSourceUri source;
    std::string enumeration_root;
    bool dcim_discovered = false;
    std::vector<IngestObjectId> objects;
    std::vector<std::string> media_paths;
};

struct IngestRequest
{
    std::string source_root; // user-selected mount / folder / DCIM / stub fixture
    // Empty => filesystem-card. Values: filesystem-card | ptp-usb | mtp | ptp-stub
    std::string transport;
    std::optional<std::string> session_vendor_id;
    std::optional<std::string> session_product_id;
    std::optional<std::string> session_serial;
    std::optional<std::string> resume_batch_id;
    ImportTransferMode mode = ImportTransferMode::kCopy;
    ImportOrganization organization = ImportOrganization::kSingleFolder;
    ImportPreviewPolicy preview = ImportPreviewPolicy::kMinimal;
    std::string destination_directory;
    std::string filename_template;
    std::string second_copy_directory;
    bool recursive = true;
    bool include_xmp_sidecars = true;
    bool defer_previews = true;
    bool skip_existing = false;
    std::optional<std::int64_t> expected_catalog_revision;
    std::vector<std::pair<std::string, std::string>> expected_content_hashes;
    // Optional absolute-path filter (Studio selection). Empty => ingest all
    // enumerated objects from the transport snapshot.
    std::vector<std::string> selected_paths;
    CancellationToken cancellation{};
};

struct IngestResumeCheckpoint
{
    std::string schema{std::string(kIngestResumeContractVersion)};
    std::int64_t schema_version = kIngestResumeSchemaVersion;
    std::string batch_id;
    std::string source_uri;
    std::string transport;
    std::string destination_directory;
    std::string second_copy_directory;
    std::string filename_template;
    std::vector<std::string> completed_relative_paths;
    std::int64_t created_unix_ms = 0;
    std::int64_t updated_unix_ms = 0;
};

struct IngestBatchResult
{
    ImportBatchResult import;
    std::string transport{std::string(kIngestTransportFilesystemCard)};
    std::string source_uri;
    std::optional<std::string> resume_batch_id;
    bool resume_checkpoint_cleared = false;
    NativeIngestPlatformSupport support{};
};

[[nodiscard]] std::string format_filesystem_card_ingest_uri(std::string_view absolute_root);
[[nodiscard]] std::string format_ptp_usb_ingest_uri(std::string_view vendor_id,
                                                    std::string_view product_id,
                                                    std::string_view serial);
[[nodiscard]] std::string format_mtp_ingest_uri(std::string_view vendor_id,
                                                std::string_view product_id,
                                                std::string_view serial);
[[nodiscard]] std::string format_ptp_stub_ingest_uri(std::string_view absolute_fixture_root);

[[nodiscard]] Result<IngestSourceUri> parse_ingest_source_uri(std::string_view uri);
[[nodiscard]] Result<void> ensure_ingest_source_connected(const IngestSourceUri &source);

[[nodiscard]] NativeIngestPlatformSupport probe_native_ingest_support() noexcept;
[[nodiscard]] bool native_ingest_adapter_is_packaged() noexcept;

// Opens a user-selected filesystem card mount or folder. Prefers DCIM/ when
// present. Does not mutate source bytes.
[[nodiscard]] Result<IngestSourceSnapshot>
open_filesystem_card_ingest(std::string_view root_path, const CancellationToken &cancellation,
                            bool recursive = true);

// Fixture-only PTP-shaped enumeration for resume/copy contract tests.
[[nodiscard]] Result<IngestSourceSnapshot>
open_ptp_stub_ingest(std::string_view fixture_root, const CancellationToken &cancellation,
                     bool recursive = true);

// Live PTP/MTP open. Fail-closed when adapter is not packaged.
[[nodiscard]] Result<IngestSourceSnapshot>
open_native_ptp_mtp_ingest(const IngestRequest &request, const CancellationToken &cancellation);

[[nodiscard]] Result<ImportRequest> make_ingest_import_request(const IngestSourceSnapshot &snapshot,
                                                               const IngestRequest &request);

[[nodiscard]] std::string ingest_resume_directory(std::string_view database_path);
[[nodiscard]] std::string ingest_resume_checkpoint_path(std::string_view database_path,
                                                        std::string_view batch_id);
[[nodiscard]] Result<IngestResumeCheckpoint>
load_ingest_resume_checkpoint(std::string_view database_path, std::string_view batch_id);
[[nodiscard]] Result<void> save_ingest_resume_checkpoint(std::string_view database_path,
                                                         const IngestResumeCheckpoint &checkpoint);
[[nodiscard]] Result<void> clear_ingest_resume_checkpoint(std::string_view database_path,
                                                          std::string_view batch_id);
[[nodiscard]] std::string make_ingest_resume_batch_id(std::string_view source_uri);

} // namespace ravo
