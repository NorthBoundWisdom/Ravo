#include "ravo/services/ingest_transport.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include "catalog_internal.h"
#include "ravo/domain/uri.h"
#include "ravo/services/catalog_service.h"

namespace ravo
{
namespace
{

[[nodiscard]] std::string path_text(const std::filesystem::path &path)
{
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char *>(value.data()), value.size()};
}

[[nodiscard]] std::string ascii_lower(std::string value)
{
    for (char &character : value)
    {
        const auto unsigned_character = static_cast<unsigned char>(character);
        if (unsigned_character <= 0x7FU)
            character = static_cast<char>(std::tolower(unsigned_character));
    }
    return value;
}

[[nodiscard]] bool name_equals_dcim(const std::filesystem::path &path)
{
    return ascii_lower(path_text(path.filename())) == "dcim";
}

[[nodiscard]] Result<std::filesystem::path>
resolve_enumeration_root(const std::filesystem::path &root, bool &dcim_discovered)
{
    dcim_discovered = false;
    std::error_code error;
    if (!std::filesystem::is_directory(root, error) || error)
        return make_error(ErrorCode::kNotFound, "Ingest source root is not an existing directory",
                          {{"path", path_text(root)},
                           {"reason", "ingest_source_root_missing"},
                           {"detail", error.message()}});
    if (name_equals_dcim(root))
    {
        dcim_discovered = true;
        return root;
    }
    const auto dcim_child = root / "DCIM";
    if (std::filesystem::is_directory(dcim_child, error) && !error)
    {
        dcim_discovered = true;
        return dcim_child;
    }
    const auto dcim_lower = root / "dcim";
    if (std::filesystem::is_directory(dcim_lower, error) && !error)
    {
        dcim_discovered = true;
        return dcim_lower;
    }
    return root;
}

[[nodiscard]] std::string relative_under(const std::filesystem::path &root,
                                         const std::filesystem::path &absolute)
{
    std::error_code error;
    const auto relative = std::filesystem::relative(absolute, root, error);
    if (error)
        return path_text(absolute.filename());
    return path_text(relative);
}

} // namespace

std::string format_filesystem_card_ingest_uri(const std::string_view absolute_root)
{
    return std::string(kFilesystemCardIngestUriPrefix) + std::string(absolute_root);
}

Result<IngestSourceUri> parse_ingest_source_uri(const std::string_view uri)
{
    if (uri.rfind(kFilesystemCardIngestUriPrefix, 0) == 0)
    {
        const auto root = std::string(uri.substr(kFilesystemCardIngestUriPrefix.size()));
        if (root.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "filesystem-card ingest URI is missing a root path",
                              {{"uri", std::string(uri)}, {"reason", "ingest_uri_missing_root"}});
        IngestSourceUri parsed;
        parsed.transport = IngestTransportKind::kFilesystemCard;
        parsed.root = root;
        parsed.uri = format_filesystem_card_ingest_uri(root);
        return parsed;
    }
    if (uri.rfind(std::string(kIngestUriScheme) + std::string(kIngestTransportPtpUsb), 0) == 0)
        return make_error(ErrorCode::kUnsupported,
                          "PTP USB ingest transport is not implemented in this tranche",
                          {{"uri", std::string(uri)},
                           {"transport", std::string(kIngestTransportPtpUsb)},
                           {"reason", "ingest_transport_ptp_usb_unimplemented"}});
    return make_error(ErrorCode::kInvalidArgument, "Unrecognized ingest source URI",
                      {{"uri", std::string(uri)}, {"reason", "ingest_uri_unrecognized"}});
}

Result<void> ensure_ingest_source_connected(const IngestSourceUri &source)
{
    if (source.transport != IngestTransportKind::kFilesystemCard)
        return make_error(ErrorCode::kUnsupported,
                          "Ingest source liveness is only implemented for filesystem-card",
                          {{"reason", "ingest_transport_liveness_unsupported"}});
    auto location = normalize_local_input(source.root);
    if (!location)
        return location.error();
    std::error_code error;
    if (!std::filesystem::is_directory(utf8_path(location.value().path), error) || error)
        return make_error(ErrorCode::kIo, "Ingest source disconnected or no longer mounted",
                          {{"path", location.value().path},
                           {"uri", source.uri},
                           {"reason", "ingest_source_disconnected"},
                           {"detail", error.message()}});
    return {};
}

Result<IngestSourceSnapshot> open_filesystem_card_ingest(const std::string_view root_path,
                                                         const CancellationToken &cancellation,
                                                         const bool recursive)
{
    auto active = cancellation.check();
    if (!active)
        return active.error();
    auto location = normalize_local_input(root_path);
    if (!location)
        return location.error();
    const auto root = utf8_path(location.value().path);
    bool dcim_discovered = false;
    auto enumeration = resolve_enumeration_root(root, dcim_discovered);
    if (!enumeration)
        return enumeration.error();
    const auto enumeration_root = std::move(enumeration).value();
    auto paths = collect_import_paths({path_text(enumeration_root)}, cancellation, recursive);
    if (!paths)
        return paths.error();

    IngestSourceSnapshot snapshot;
    snapshot.source.transport = IngestTransportKind::kFilesystemCard;
    snapshot.source.root = location.value().path;
    snapshot.source.uri = format_filesystem_card_ingest_uri(location.value().path);
    snapshot.enumeration_root = path_text(enumeration_root);
    snapshot.dcim_discovered = dcim_discovered;
    snapshot.media_paths = std::move(paths).value();
    snapshot.objects.reserve(snapshot.media_paths.size());
    for (const auto &media : snapshot.media_paths)
    {
        IngestObjectId object;
        object.absolute_path = media;
        object.relative_path = relative_under(enumeration_root, utf8_path(media));
        snapshot.objects.push_back(std::move(object));
    }
    return snapshot;
}

Result<ImportRequest> make_ingest_import_request(const IngestSourceSnapshot &snapshot,
                                                 const IngestRequest &request)
{
    if (snapshot.source.transport != IngestTransportKind::kFilesystemCard)
        return make_error(ErrorCode::kUnsupported,
                          "Only filesystem-card ingest can build an ImportRequest in this tranche",
                          {{"reason", "ingest_transport_unsupported"}});
    if (request.mode == ImportTransferMode::kMove)
        return make_error(ErrorCode::kInvalidArgument,
                          "Ingest transports reject Move; source bytes stay read-only",
                          {{"reason", "ingest_move_unsupported"}});
    if (snapshot.media_paths.empty())
        return make_error(ErrorCode::kNotFound, "Ingest source enumeration found no media",
                          {{"root", snapshot.source.root},
                           {"enumeration_root", snapshot.enumeration_root},
                           {"reason", "ingest_source_empty"}});

    ImportRequest import_request;
    import_request.inputs = snapshot.media_paths;
    import_request.source_root = snapshot.enumeration_root;
    import_request.mode = request.mode;
    import_request.organization = request.organization;
    import_request.preview = request.preview;
    import_request.destination_directory = request.destination_directory;
    import_request.filename_template = request.filename_template;
    import_request.second_copy_directory = request.second_copy_directory;
    import_request.recursive = false; // paths already enumerated
    import_request.include_xmp_sidecars = request.include_xmp_sidecars;
    import_request.defer_previews = request.defer_previews;
    import_request.cancellation = request.cancellation;
    return import_request;
}

Result<ImportBatchResult> CatalogService::execute_ingest(
    const IngestRequest &request,
    const std::function<void(std::size_t, std::size_t, const ImportItemResult *)> &progress)
{
    auto snapshot =
        open_filesystem_card_ingest(request.source_root, request.cancellation, request.recursive);
    if (!snapshot)
        return snapshot.error();
    auto connected = ensure_ingest_source_connected(snapshot.value().source);
    if (!connected)
        return connected.error();
    auto import_request = make_ingest_import_request(snapshot.value(), request);
    if (!import_request)
        return import_request.error();

    const IngestSourceUri source = snapshot.value().source;
    ingest_source_liveness_ = [source]() -> Result<void>
    { return ensure_ingest_source_connected(source); };
    ingest_report_remaining_on_stop_ = true;
    auto batch = execute_import(import_request.value(), progress);
    ingest_source_liveness_ = {};
    ingest_report_remaining_on_stop_ = false;
    return batch;
}

} // namespace ravo
