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

// ADR-0125 ingest transport contracts. First-ship adapter is filesystem-card
// (mounted volume / DCIM folder). PTP USB is a later adapter on the same URI
// and disconnect semantics.

inline constexpr std::string_view kIngestUriScheme = "ravo-ingest:";
inline constexpr std::string_view kIngestTransportFilesystemCard = "filesystem-card";
inline constexpr std::string_view kIngestTransportPtpUsb = "ptp-usb";
inline constexpr std::string_view kFilesystemCardIngestUriPrefix = "ravo-ingest:filesystem-card:";

enum class IngestTransportKind : std::uint8_t
{
    kFilesystemCard = 0,
    kPtpUsb = 1, // reserved; not implemented in this tranche
};

struct IngestSourceUri
{
    IngestTransportKind transport = IngestTransportKind::kFilesystemCard;
    std::string root; // absolute filesystem root for filesystem-card
    std::string uri;  // canonical ravo-ingest:… serialization
};

struct IngestObjectId
{
    // Portable relative path under the enumeration root (filesystem-card).
    std::string relative_path;
    std::string absolute_path;
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
    std::string source_root; // user-selected mount / folder / DCIM path
    ImportTransferMode mode = ImportTransferMode::kCopy;
    ImportOrganization organization = ImportOrganization::kSingleFolder;
    ImportPreviewPolicy preview = ImportPreviewPolicy::kMinimal;
    std::string destination_directory;
    std::string filename_template;
    std::string second_copy_directory;
    bool recursive = true;
    bool include_xmp_sidecars = true;
    bool defer_previews = true;
    CancellationToken cancellation{};
};

[[nodiscard]] std::string format_filesystem_card_ingest_uri(std::string_view absolute_root);
[[nodiscard]] Result<IngestSourceUri> parse_ingest_source_uri(std::string_view uri);
[[nodiscard]] Result<void> ensure_ingest_source_connected(const IngestSourceUri &source);

// Opens a user-selected filesystem card mount or folder. Prefers DCIM/ when
// present. Does not mutate source bytes.
[[nodiscard]] Result<IngestSourceSnapshot>
open_filesystem_card_ingest(std::string_view root_path, const CancellationToken &cancellation,
                            bool recursive = true);

[[nodiscard]] Result<ImportRequest> make_ingest_import_request(const IngestSourceSnapshot &snapshot,
                                                               const IngestRequest &request);

} // namespace ravo
