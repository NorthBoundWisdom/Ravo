#include "ravo/services/ingest_transport.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <utility>

#include "catalog_internal.h"
#include "ravo/adapters/text_file.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/json.h"
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

[[nodiscard]] std::string host_platform_name() noexcept
{
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

[[nodiscard]] std::string planned_ptp_stack_for(const std::string_view platform)
{
    if (platform == "macos")
        return "ImageCaptureCore";
    if (platform == "windows")
        return "WinRT Portable Devices";
    if (platform == "linux")
        return "libmtp";
    return "unspecified";
}

[[nodiscard]] std::string planned_mtp_stack_for(const std::string_view platform)
{
    if (platform == "macos")
        return "packaged MTP host adapter";
    if (platform == "windows")
        return "WinRT MTP";
    if (platform == "linux")
        return "libmtp";
    return "unspecified";
}

[[nodiscard]] Result<IngestTransportKind> parse_transport_name(const std::string_view name)
{
    if (name.empty() || name == kIngestTransportFilesystemCard)
        return IngestTransportKind::kFilesystemCard;
    if (name == kIngestTransportPtpUsb)
        return IngestTransportKind::kPtpUsb;
    if (name == kIngestTransportMtp)
        return IngestTransportKind::kMtp;
    if (name == kIngestTransportPtpStub)
        return IngestTransportKind::kPtpStub;
    return make_error(ErrorCode::kInvalidArgument, "Unknown ingest transport",
                      {{"transport", std::string(name)}, {"reason", "ingest_transport_unknown"}});
}

[[nodiscard]] Result<IngestSourceSnapshot>
open_enumerated_tree(const std::string_view root_path, const IngestTransportKind transport,
                     const CancellationToken &cancellation, const bool recursive,
                     const std::optional<PtpMtpSessionIdentity> &session)
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
    snapshot.source.transport = transport;
    snapshot.source.root = location.value().path;
    if (session)
        snapshot.source.session = session;
    snapshot.enumeration_root = path_text(enumeration_root);
    snapshot.dcim_discovered = dcim_discovered;
    snapshot.media_paths = std::move(paths).value();
    snapshot.objects.reserve(snapshot.media_paths.size());
    for (const auto &media : snapshot.media_paths)
    {
        IngestObjectId object;
        object.absolute_path = media;
        object.relative_path = relative_under(enumeration_root, utf8_path(media));
        if (transport == IngestTransportKind::kPtpStub ||
            transport == IngestTransportKind::kPtpUsb || transport == IngestTransportKind::kMtp)
        {
            object.object_handle = "stub:" + object.relative_path;
            object.relative_path = "storage/0001/" + object.relative_path;
        }
        std::error_code size_error;
        const auto size = std::filesystem::file_size(utf8_path(media), size_error);
        if (!size_error)
            object.size_bytes = static_cast<std::uint64_t>(size);
        snapshot.objects.push_back(std::move(object));
    }
    return snapshot;
}

[[nodiscard]] ImportItemResult skipped_item(std::string path, TaskError error)
{
    ImportItemResult result;
    result.status = ImportItemStatus::kSkipped;
    result.input_path = std::move(path);
    result.error = std::move(error);
    return result;
}

[[nodiscard]] Result<void> ensure_resume_directory(const std::string_view database_path)
{
    const auto root = ingest_resume_directory(database_path);
    std::error_code error;
    std::filesystem::create_directories(utf8_path(root), error);
    if (error)
    {
        return make_error(ErrorCode::kIo, "Unable to create ingest-resume directory",
                          {{"path", root},
                           {"reason", "ingest_resume_directory_create_failed"},
                           {"detail", error.message()}});
    }
    return {};
}

[[nodiscard]] Result<std::string> require_json_string(const JsonValue &object, const char *field,
                                                      const std::string_view path)
{
    const auto *value = object.find(field);
    if (value == nullptr || value->string_if() == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Ingest resume checkpoint field missing",
                          {{"path", std::string(path)},
                           {"field", field},
                           {"reason", "invalid_ingest_resume_checkpoint"}});
    }
    return *value->string_if();
}

[[nodiscard]] Result<std::int64_t> require_json_i64(const JsonValue &object, const char *field,
                                                    const std::string_view path)
{
    const auto *value = object.find(field);
    if (value == nullptr || value->number_if() == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Ingest resume checkpoint number missing",
                          {{"path", std::string(path)},
                           {"field", field},
                           {"reason", "invalid_ingest_resume_checkpoint"}});
    }
    try
    {
        return static_cast<std::int64_t>(std::stoll(value->number_if()->text));
    }
    catch (...)
    {
        return make_error(ErrorCode::kValidation, "Ingest resume checkpoint number invalid",
                          {{"path", std::string(path)},
                           {"field", field},
                           {"reason", "invalid_ingest_resume_checkpoint"}});
    }
}

} // namespace

std::string format_filesystem_card_ingest_uri(const std::string_view absolute_root)
{
    return std::string(kFilesystemCardIngestUriPrefix) + std::string(absolute_root);
}

std::string format_ptp_usb_ingest_uri(const std::string_view vendor_id,
                                      const std::string_view product_id,
                                      const std::string_view serial)
{
    return std::string(kPtpUsbIngestUriPrefix) + std::string(vendor_id) + ":" +
           std::string(product_id) + ":" + std::string(serial);
}

std::string format_mtp_ingest_uri(const std::string_view vendor_id,
                                  const std::string_view product_id, const std::string_view serial)
{
    return std::string(kMtpIngestUriPrefix) + std::string(vendor_id) + ":" +
           std::string(product_id) + ":" + std::string(serial);
}

std::string format_ptp_stub_ingest_uri(const std::string_view absolute_fixture_root)
{
    return std::string(kPtpStubIngestUriPrefix) + std::string(absolute_fixture_root);
}

bool native_ingest_adapter_is_packaged() noexcept
{
    return false;
}

NativeIngestPlatformSupport probe_native_ingest_support() noexcept
{
    NativeIngestPlatformSupport support;
    support.platform = host_platform_name();
    support.adapter_packaged = native_ingest_adapter_is_packaged();
    support.ptp_planned_stack = planned_ptp_stack_for(support.platform);
    support.mtp_planned_stack = planned_mtp_stack_for(support.platform);
    if (support.adapter_packaged)
    {
        support.ptp_usb = NativeIngestSupportState::kAvailable;
        support.mtp = NativeIngestSupportState::kAvailable;
        support.reason = "native_ingest_adapter_packaged";
    }
    else
    {
        support.ptp_usb = NativeIngestSupportState::kUnsupported;
        support.mtp = NativeIngestSupportState::kUnsupported;
        support.reason = "native_ingest_adapter_not_packaged";
    }
    return support;
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
    if (uri.rfind(kPtpStubIngestUriPrefix, 0) == 0)
    {
        const auto root = std::string(uri.substr(kPtpStubIngestUriPrefix.size()));
        if (root.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "ptp-stub ingest URI is missing a fixture root",
                              {{"uri", std::string(uri)}, {"reason", "ingest_uri_missing_root"}});
        IngestSourceUri parsed;
        parsed.transport = IngestTransportKind::kPtpStub;
        parsed.root = root;
        parsed.uri = format_ptp_stub_ingest_uri(root);
        PtpMtpSessionIdentity session;
        session.transport = IngestTransportKind::kPtpStub;
        session.vendor_id = "stub";
        session.product_id = "fixture";
        session.serial = "local";
        session.display_name = "PTP stub fixture";
        session.uri = parsed.uri;
        parsed.session = std::move(session);
        return parsed;
    }
    if (uri.rfind(kPtpUsbIngestUriPrefix, 0) == 0 || uri.rfind(kMtpIngestUriPrefix, 0) == 0)
    {
        const auto support = probe_native_ingest_support();
        const bool is_mtp = uri.rfind(kMtpIngestUriPrefix, 0) == 0;
        return make_error(
            ErrorCode::kUnsupported,
            is_mtp ? "MTP ingest transport adapter is not packaged" :
                     "PTP USB ingest transport adapter is not packaged",
            {{"uri", std::string(uri)},
             {"transport", std::string(is_mtp ? kIngestTransportMtp : kIngestTransportPtpUsb)},
             {"platform", support.platform},
             {"adapter_packaged", support.adapter_packaged ? "true" : "false"},
             {"ptp_state", std::string(native_ingest_support_state_name(support.ptp_usb))},
             {"mtp_state", std::string(native_ingest_support_state_name(support.mtp))},
             {"reason", support.reason}});
    }
    return make_error(ErrorCode::kInvalidArgument, "Unrecognized ingest source URI",
                      {{"uri", std::string(uri)}, {"reason", "ingest_uri_unrecognized"}});
}

Result<void> ensure_ingest_source_connected(const IngestSourceUri &source)
{
    if (source.transport != IngestTransportKind::kFilesystemCard &&
        source.transport != IngestTransportKind::kPtpStub)
    {
        return make_error(
            ErrorCode::kUnsupported,
            "Ingest source liveness requires a packaged native adapter or "
            "filesystem/stub root",
            {{"reason", "ingest_transport_liveness_unsupported"},
             {"transport", std::string(ingest_transport_kind_name(source.transport))}});
    }
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
    auto snapshot = open_enumerated_tree(root_path, IngestTransportKind::kFilesystemCard,
                                         cancellation, recursive, std::nullopt);
    if (!snapshot)
        return snapshot.error();
    snapshot.value().source.uri = format_filesystem_card_ingest_uri(snapshot.value().source.root);
    return snapshot;
}

Result<IngestSourceSnapshot> open_ptp_stub_ingest(const std::string_view fixture_root,
                                                  const CancellationToken &cancellation,
                                                  const bool recursive)
{
    PtpMtpSessionIdentity session;
    session.transport = IngestTransportKind::kPtpStub;
    session.vendor_id = "stub";
    session.product_id = "fixture";
    session.serial = "local";
    session.display_name = "PTP stub fixture";
    auto snapshot = open_enumerated_tree(fixture_root, IngestTransportKind::kPtpStub, cancellation,
                                         recursive, session);
    if (!snapshot)
        return snapshot.error();
    snapshot.value().source.uri = format_ptp_stub_ingest_uri(snapshot.value().source.root);
    snapshot.value().source.session->uri = snapshot.value().source.uri;
    return snapshot;
}

Result<IngestSourceSnapshot> open_native_ptp_mtp_ingest(const IngestRequest &request,
                                                        const CancellationToken &cancellation)
{
    auto active = cancellation.check();
    if (!active)
        return active.error();
    auto kind = parse_transport_name(request.transport);
    if (!kind)
        return kind.error();
    if (kind.value() != IngestTransportKind::kPtpUsb && kind.value() != IngestTransportKind::kMtp)
    {
        return make_error(
            ErrorCode::kInvalidArgument, "open_native_ptp_mtp_ingest requires ptp-usb or mtp",
            {{"transport", request.transport}, {"reason", "ingest_transport_not_native"}});
    }
    const auto support = probe_native_ingest_support();
    return make_error(
        ErrorCode::kUnsupported, "Native PTP/MTP ingest adapter is not packaged on this machine",
        {{"transport", request.transport},
         {"platform", support.platform},
         {"adapter_packaged", "false"},
         {"ptp_state", std::string(native_ingest_support_state_name(support.ptp_usb))},
         {"mtp_state", std::string(native_ingest_support_state_name(support.mtp))},
         {"ptp_planned_stack", support.ptp_planned_stack},
         {"mtp_planned_stack", support.mtp_planned_stack},
         {"reason", support.reason}});
}

Result<ImportRequest> make_ingest_import_request(const IngestSourceSnapshot &snapshot,
                                                 const IngestRequest &request)
{
    if (snapshot.source.transport != IngestTransportKind::kFilesystemCard &&
        snapshot.source.transport != IngestTransportKind::kPtpStub)
    {
        return make_error(
            ErrorCode::kUnsupported,
            "Only filesystem-card or ptp-stub ingest can build an ImportRequest in "
            "this tranche",
            {{"reason", "ingest_transport_unsupported"},
             {"transport", std::string(ingest_transport_kind_name(snapshot.source.transport))}});
    }
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
    import_request.skip_existing = request.skip_existing;
    import_request.expected_catalog_revision = request.expected_catalog_revision;
    import_request.expected_content_hashes = request.expected_content_hashes;
    import_request.cancellation = request.cancellation;
    return import_request;
}

std::string ingest_resume_directory(const std::string_view database_path)
{
    return std::string(database_path) + ".ravo/ingest-resume";
}

std::string ingest_resume_checkpoint_path(const std::string_view database_path,
                                          const std::string_view batch_id)
{
    return ingest_resume_directory(database_path) + "/" + std::string(batch_id) + ".json";
}

std::string make_ingest_resume_batch_id(const std::string_view source_uri)
{
    return "ingest_" + sha256_utf8_hex(source_uri).substr(0, 24);
}

Result<void> save_ingest_resume_checkpoint(const std::string_view database_path,
                                           const IngestResumeCheckpoint &checkpoint)
{
    if (checkpoint.batch_id.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "Ingest resume checkpoint needs a batch id",
                          {{"reason", "ingest_resume_missing_batch_id"}});
    }
    auto created = ensure_resume_directory(database_path);
    if (!created)
        return created.error();

    JsonValue::Array completed;
    completed.reserve(checkpoint.completed_relative_paths.size());
    for (const auto &path : checkpoint.completed_relative_paths)
        completed.push_back(path);

    JsonValue::Object object{
        {"schema", checkpoint.schema},
        {"schema_version", JsonValue::number(std::to_string(checkpoint.schema_version))},
        {"batch_id", checkpoint.batch_id},
        {"source_uri", checkpoint.source_uri},
        {"transport", checkpoint.transport},
        {"destination_directory", checkpoint.destination_directory},
        {"second_copy_directory", checkpoint.second_copy_directory},
        {"filename_template", checkpoint.filename_template},
        {"completed_relative_paths", JsonValue{std::move(completed)}},
        {"created_unix_ms", JsonValue::number(std::to_string(checkpoint.created_unix_ms))},
        {"updated_unix_ms", JsonValue::number(std::to_string(checkpoint.updated_unix_ms))},
    };
    const auto path = ingest_resume_checkpoint_path(database_path, checkpoint.batch_id);
    return write_utf8_text_file_replace_atomically(path,
                                                   serialize_json(JsonValue{std::move(object)}));
}

Result<IngestResumeCheckpoint> load_ingest_resume_checkpoint(const std::string_view database_path,
                                                             const std::string_view batch_id)
{
    const auto path = ingest_resume_checkpoint_path(database_path, batch_id);
    auto text = read_utf8_text_file(path);
    if (!text)
        return text.error();
    auto parsed = parse_json(text.value());
    if (!parsed)
        return parsed.error();
    const auto *object = parsed.value().object_if();
    if (object == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Ingest resume checkpoint is not an object",
                          {{"path", path}, {"reason", "invalid_ingest_resume_checkpoint"}});
    }

    IngestResumeCheckpoint checkpoint;
    auto schema = require_json_string(parsed.value(), "schema", path);
    if (!schema)
        return schema.error();
    checkpoint.schema = std::move(schema).value();
    if (checkpoint.schema != kIngestResumeContractVersion)
    {
        return make_error(ErrorCode::kUnsupported, "Ingest resume checkpoint schema unsupported",
                          {{"path", path},
                           {"schema", checkpoint.schema},
                           {"reason", "unsupported_ingest_resume_schema"}});
    }
    auto schema_version = require_json_i64(parsed.value(), "schema_version", path);
    if (!schema_version)
        return schema_version.error();
    checkpoint.schema_version = schema_version.value();
    auto id = require_json_string(parsed.value(), "batch_id", path);
    if (!id)
        return id.error();
    checkpoint.batch_id = std::move(id).value();
    auto source_uri = require_json_string(parsed.value(), "source_uri", path);
    if (!source_uri)
        return source_uri.error();
    checkpoint.source_uri = std::move(source_uri).value();
    auto transport = require_json_string(parsed.value(), "transport", path);
    if (!transport)
        return transport.error();
    checkpoint.transport = std::move(transport).value();
    auto destination = require_json_string(parsed.value(), "destination_directory", path);
    if (!destination)
        return destination.error();
    checkpoint.destination_directory = std::move(destination).value();
    auto second = require_json_string(parsed.value(), "second_copy_directory", path);
    if (!second)
        return second.error();
    checkpoint.second_copy_directory = std::move(second).value();
    auto filename = require_json_string(parsed.value(), "filename_template", path);
    if (!filename)
        return filename.error();
    checkpoint.filename_template = std::move(filename).value();
    auto created = require_json_i64(parsed.value(), "created_unix_ms", path);
    if (!created)
        return created.error();
    checkpoint.created_unix_ms = created.value();
    auto updated = require_json_i64(parsed.value(), "updated_unix_ms", path);
    if (!updated)
        return updated.error();
    checkpoint.updated_unix_ms = updated.value();

    const auto *completed = parsed.value().find("completed_relative_paths");
    if (completed == nullptr || completed->array_if() == nullptr)
    {
        return make_error(ErrorCode::kValidation,
                          "Ingest resume checkpoint missing completed_relative_paths",
                          {{"path", path}, {"reason", "invalid_ingest_resume_checkpoint"}});
    }
    for (const auto &entry : *completed->array_if())
    {
        if (entry.string_if() == nullptr)
        {
            return make_error(ErrorCode::kValidation,
                              "Ingest resume completed path must be a string",
                              {{"path", path}, {"reason", "invalid_ingest_resume_checkpoint"}});
        }
        checkpoint.completed_relative_paths.push_back(*entry.string_if());
    }
    return checkpoint;
}

Result<void> clear_ingest_resume_checkpoint(const std::string_view database_path,
                                            const std::string_view batch_id)
{
    const auto path = ingest_resume_checkpoint_path(database_path, batch_id);
    std::error_code error;
    if (!std::filesystem::exists(utf8_path(path), error))
        return {};
    std::filesystem::remove(utf8_path(path), error);
    if (error)
    {
        return make_error(ErrorCode::kIo, "Unable to clear ingest resume checkpoint",
                          {{"path", path},
                           {"reason", "ingest_resume_clear_failed"},
                           {"detail", error.message()}});
    }
    return {};
}

Result<ImportBatchResult> CatalogService::execute_ingest(
    const IngestRequest &request,
    const std::function<void(std::size_t, std::size_t, const ImportItemResult *)> &progress)
{
    auto detailed = execute_ingest_detailed(request, progress);
    if (!detailed)
        return detailed.error();
    return detailed.value().import;
}

Result<IngestBatchResult> CatalogService::execute_ingest_detailed(
    const IngestRequest &request,
    const std::function<void(std::size_t, std::size_t, const ImportItemResult *)> &progress)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kInvalidArgument, "Catalog is not open",
                          {{"reason", "catalog_not_open"}});
    }

    IngestBatchResult detailed;
    detailed.support = probe_native_ingest_support();

    auto kind = parse_transport_name(request.transport);
    if (!kind)
        return kind.error();
    detailed.transport = std::string(ingest_transport_kind_name(kind.value()));

    Result<IngestSourceSnapshot> snapshot = make_error(ErrorCode::kInternal, "unset");
    if (kind.value() == IngestTransportKind::kFilesystemCard)
    {
        snapshot = open_filesystem_card_ingest(request.source_root, request.cancellation,
                                               request.recursive);
    }
    else if (kind.value() == IngestTransportKind::kPtpStub)
    {
        snapshot =
            open_ptp_stub_ingest(request.source_root, request.cancellation, request.recursive);
    }
    else
    {
        snapshot = open_native_ptp_mtp_ingest(request, request.cancellation);
    }
    if (!snapshot)
        return snapshot.error();

    detailed.source_uri = snapshot.value().source.uri;
    if (!request.selected_paths.empty())
    {
        std::set<std::string> selected;
        for (const auto &path : request.selected_paths)
        {
            auto normalized = normalize_local_input(path);
            if (!normalized)
                return normalized.error();
            selected.insert(normalized.value().path);
        }
        IngestSourceSnapshot filtered = snapshot.value();
        filtered.objects.clear();
        filtered.media_paths.clear();
        for (const auto &object : snapshot.value().objects)
        {
            auto object_path = normalize_local_input(object.absolute_path);
            const std::string key = object_path ? object_path.value().path : object.absolute_path;
            if (selected.count(key) == 0U)
                continue;
            filtered.objects.push_back(object);
            filtered.media_paths.push_back(object.absolute_path);
            selected.erase(key);
        }
        if (!selected.empty())
        {
            return make_error(ErrorCode::kNotFound,
                              "Selected ingest path is not present in the transport snapshot",
                              {{"path", *selected.begin()},
                               {"source_uri", detailed.source_uri},
                               {"reason", "ingest_selected_path_missing"}});
        }
        if (filtered.media_paths.empty())
        {
            return make_error(
                ErrorCode::kNotFound, "Ingest selection matched no media objects",
                {{"source_uri", detailed.source_uri}, {"reason", "ingest_selection_empty"}});
        }
        snapshot = std::move(filtered);
    }
    auto connected = ensure_ingest_source_connected(snapshot.value().source);
    if (!connected)
        return connected.error();

    auto catalog_snapshot = CatalogService::snapshot();
    if (!catalog_snapshot)
        return catalog_snapshot.error();
    const auto &database_path = catalog_snapshot.value().database_path;

    std::string batch_id =
        request.resume_batch_id.value_or(make_ingest_resume_batch_id(detailed.source_uri));
    detailed.resume_batch_id = batch_id;

    std::set<std::string> completed;
    IngestResumeCheckpoint checkpoint;
    checkpoint.batch_id = batch_id;
    checkpoint.source_uri = detailed.source_uri;
    checkpoint.transport = detailed.transport;
    checkpoint.destination_directory = request.destination_directory;
    checkpoint.second_copy_directory = request.second_copy_directory;
    checkpoint.filename_template = request.filename_template;
    checkpoint.created_unix_ms = now_unix_ms();
    checkpoint.updated_unix_ms = checkpoint.created_unix_ms;

    if (request.resume_batch_id)
    {
        auto loaded = load_ingest_resume_checkpoint(database_path, batch_id);
        if (loaded)
        {
            checkpoint = std::move(loaded).value();
            if (checkpoint.source_uri != detailed.source_uri)
            {
                return make_error(ErrorCode::kConflict,
                                  "Ingest resume checkpoint source URI does not match session",
                                  {{"batch_id", batch_id},
                                   {"checkpoint_uri", checkpoint.source_uri},
                                   {"source_uri", detailed.source_uri},
                                   {"reason", "ingest_resume_source_mismatch"}});
            }
            for (const auto &path : checkpoint.completed_relative_paths)
                completed.insert(path);
        }
        else if (loaded.error().code != ErrorCode::kNotFound &&
                 loaded.error().code != ErrorCode::kIo)
        {
            return loaded.error();
        }
    }

    // Build skipped reports for already-completed objects, and filter remaining.
    IngestSourceSnapshot remaining = snapshot.value();
    remaining.objects.clear();
    remaining.media_paths.clear();
    ImportBatchResult pre_skipped;
    for (const auto &object : snapshot.value().objects)
    {
        if (completed.count(object.relative_path) == 0U)
        {
            remaining.objects.push_back(object);
            remaining.media_paths.push_back(object.absolute_path);
            continue;
        }
        auto item = skipped_item(object.absolute_path,
                                 make_error(ErrorCode::kConflict,
                                            "Ingest object already completed in resume checkpoint",
                                            {{"relative_path", object.relative_path},
                                             {"batch_id", batch_id},
                                             {"reason", "ingest_resume_already_completed"}}));
        ++pre_skipped.skipped;
        pre_skipped.items.push_back(std::move(item));
    }

    if (remaining.media_paths.empty() && !pre_skipped.items.empty())
    {
        detailed.import = std::move(pre_skipped);
        auto cleared = clear_ingest_resume_checkpoint(database_path, batch_id);
        if (!cleared)
            return cleared.error();
        detailed.resume_checkpoint_cleared = true;
        return detailed;
    }

    auto import_request = make_ingest_import_request(remaining, request);
    if (!import_request)
        return import_request.error();

    // Map absolute path -> relative object id for checkpoint updates.
    std::map<std::string, std::string> relative_by_absolute;
    for (const auto &object : remaining.objects)
        relative_by_absolute.emplace(object.absolute_path, object.relative_path);

    const IngestSourceUri source = remaining.source;
    ingest_source_liveness_ = [source]() -> Result<void>
    { return ensure_ingest_source_connected(source); };
    ingest_report_remaining_on_stop_ = true;

    const bool resume_requested = request.resume_batch_id.has_value();
    auto batch = execute_import(
        import_request.value(),
        [&](std::size_t done, std::size_t total, const ImportItemResult *item)
        {
            const bool second_copy_required = !request.second_copy_directory.empty();
            if (item != nullptr && item->status == ImportItemStatus::kImported &&
                (!second_copy_required || item->copies_verified))
            {
                const auto found = relative_by_absolute.find(item->input_path);
                if (found != relative_by_absolute.end())
                {
                    if (std::find(checkpoint.completed_relative_paths.begin(),
                                  checkpoint.completed_relative_paths.end(),
                                  found->second) == checkpoint.completed_relative_paths.end())
                    {
                        checkpoint.completed_relative_paths.push_back(found->second);
                        checkpoint.updated_unix_ms = now_unix_ms();
                        // Persist during the run so disconnect leaves a reusable
                        // checkpoint even if the process stops abruptly.
                        (void)save_ingest_resume_checkpoint(database_path, checkpoint);
                    }
                }
            }
            if (progress)
                progress(done + pre_skipped.items.size(), total + pre_skipped.items.size(), item);
        });
    ingest_source_liveness_ = {};
    ingest_report_remaining_on_stop_ = false;
    if (!batch)
        return batch.error();

    detailed.import = std::move(batch).value();
    // Prepend resume skips so callers see a full per-item report.
    if (!pre_skipped.items.empty())
    {
        ImportBatchResult merged = std::move(pre_skipped);
        merged.mode = detailed.import.mode;
        merged.preview = detailed.import.preview;
        merged.imported = detailed.import.imported;
        merged.duplicates = detailed.import.duplicates;
        merged.unsupported = detailed.import.unsupported;
        merged.failed = detailed.import.failed;
        merged.skipped += detailed.import.skipped;
        merged.source_cleanup_failed = detailed.import.source_cleanup_failed;
        merged.verified_second_copies = detailed.import.verified_second_copies;
        for (auto &item : detailed.import.items)
            merged.items.push_back(std::move(item));
        detailed.import = std::move(merged);
    }

    const bool batch_had_stop = detailed.import.failed > 0U || detailed.import.unsupported > 0U;
    const bool all_objects_completed =
        !batch_had_stop &&
        checkpoint.completed_relative_paths.size() >= snapshot.value().objects.size();
    if (all_objects_completed || (!batch_had_stop && remaining.objects.empty()))
    {
        auto cleared = clear_ingest_resume_checkpoint(database_path, batch_id);
        if (!cleared)
            return cleared.error();
        detailed.resume_checkpoint_cleared = true;
    }
    else if (batch_had_stop && !checkpoint.completed_relative_paths.empty())
    {
        checkpoint.updated_unix_ms = now_unix_ms();
        if (checkpoint.created_unix_ms == 0)
            checkpoint.created_unix_ms = checkpoint.updated_unix_ms;
        auto saved = save_ingest_resume_checkpoint(database_path, checkpoint);
        if (!saved)
            return saved.error();
    }
    else if (resume_requested && !checkpoint.completed_relative_paths.empty())
    {
        checkpoint.updated_unix_ms = now_unix_ms();
        auto saved = save_ingest_resume_checkpoint(database_path, checkpoint);
        if (!saved)
            return saved.error();
    }

    return detailed;
}

Result<NativeIngestPlatformSupport> CatalogService::probe_ingest_native_support() const
{
    return probe_native_ingest_support();
}

} // namespace ravo
