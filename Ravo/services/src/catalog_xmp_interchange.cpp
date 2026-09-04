#include "ravo/services/catalog_service.h"

#include "catalog_internal.h"
#include "catalog_service_internal.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ravo/adapters/crs_xmp.h"
#include "ravo/adapters/text_file.h"
#include "ravo/adapters/xmp_adjacent_metadata.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/json.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/recipe.h"
#include "ravo/services/xmp_interchange.h"

namespace ravo
{
namespace
{

[[nodiscard]] std::string path_text(const std::filesystem::path &path)
{
    return catalog_service_internal::utf8_string(path.generic_u8string());
}

[[nodiscard]] Result<XmpInterchangeSidecarFingerprint>
fingerprint_sidecar_file(const std::string_view path)
{
    auto digest = sha256_file_hex(path);
    if (!digest)
        return digest.error();
    auto identity = read_file_identity(path);
    if (!identity)
        return identity.error();
    XmpInterchangeSidecarFingerprint fingerprint;
    fingerprint.sha256 = std::move(digest).value();
    fingerprint.size_bytes = identity.value().size_bytes;
    fingerprint.mtime_unix_ms = identity.value().mtime_unix_ms;
    return fingerprint;
}

[[nodiscard]] Result<std::optional<std::string>> adjacent_xmp_path(const std::string_view source)
{
    const auto path = utf8_path(source);
    std::vector<std::filesystem::path> found;
    for (const auto &extension : {u8".xmp", u8".XMP"})
    {
        auto candidate = path;
        candidate.replace_extension(extension);
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error) && !error)
        {
            if (std::none_of(found.begin(), found.end(),
                             [&](const auto &existing)
                             {
                                 std::error_code equivalent_error;
                                 return std::filesystem::equivalent(existing, candidate,
                                                                    equivalent_error) &&
                                        !equivalent_error;
                             }))
                found.push_back(std::move(candidate));
        }
        else if (error && error != std::errc::no_such_file_or_directory)
        {
            return make_error(ErrorCode::kIo, "Unable to inspect adjacent XMP sidecar",
                              {{"path", path_text(candidate)},
                               {"reason", "xmp_sidecar_inspect_failed"},
                               {"detail", error.message()}});
        }
    }
    if (found.size() > 1U)
    {
        return make_error(
            ErrorCode::kConflict, "Multiple XMP sidecars match one photo",
            {{"source", std::string(source)}, {"reason", "import_sidecar_ambiguous"}});
    }
    return found.empty() ? std::optional<std::string>{} :
                           std::optional<std::string>{path_text(found.front())};
}

[[nodiscard]] std::string exchange_root_for_catalog(const std::string_view database_path)
{
    return std::string(database_path) + ".ravo/xmp-exchange";
}

[[nodiscard]] std::string exchange_baseline_path(const std::string_view database_path,
                                                 const std::string_view asset_id)
{
    return exchange_root_for_catalog(database_path) + "/" + std::string(asset_id) + ".json";
}

struct ExchangeBaseline
{
    XmpInterchangeCatalogFingerprint catalog;
    XmpInterchangeSidecarFingerprint sidecar;
    std::string sidecar_path;
};

[[nodiscard]] std::string empty_metadata_fingerprint()
{
    return xmp_adjacent_metadata_fingerprint_sha256(WritableMetadata{}, {});
}

[[nodiscard]] Result<std::optional<ExchangeBaseline>>
load_exchange_baseline(const std::string_view database_path, const std::string_view asset_id)
{
    const auto path = exchange_baseline_path(database_path, asset_id);
    std::error_code error;
    if (!std::filesystem::is_regular_file(utf8_path(path), error))
    {
        if (error && error != std::errc::no_such_file_or_directory)
        {
            return make_error(ErrorCode::kIo, "Unable to inspect XMP exchange baseline",
                              {{"path", path},
                               {"reason", "xmp_exchange_baseline_inspect_failed"},
                               {"detail", error.message()}});
        }
        return std::optional<ExchangeBaseline>{};
    }
    auto text = read_utf8_text_file(path);
    if (!text)
        return text.error();
    auto parsed = parse_json(text.value());
    if (!parsed)
        return parsed.error();
    const auto *object = parsed.value().object_if();
    if (object == nullptr)
    {
        return make_error(ErrorCode::kValidation, "XMP exchange baseline is not an object",
                          {{"path", path}, {"reason", "invalid_xmp_exchange_baseline"}});
    }
    const auto *version = parsed.value().find("version");
    if (version == nullptr || version->number_if() == nullptr ||
        (version->number_if()->text != "1" && version->number_if()->text != "2"))
    {
        return make_error(ErrorCode::kValidation, "XMP exchange baseline version is unsupported",
                          {{"path", path}, {"reason", "unsupported_xmp_exchange_baseline"}});
    }
    const bool version2 = version->number_if()->text == "2";
    ExchangeBaseline baseline;
    const auto *generation = parsed.value().find("catalog_generation");
    const auto *recipe_sha = parsed.value().find("recipe_sha256");
    const auto *sidecar_sha = parsed.value().find("sidecar_sha256");
    const auto *sidecar_size = parsed.value().find("sidecar_size_bytes");
    const auto *sidecar_mtime = parsed.value().find("sidecar_mtime_unix_ms");
    const auto *sidecar_path = parsed.value().find("sidecar_path");
    if (generation == nullptr || generation->number_if() == nullptr || recipe_sha == nullptr ||
        recipe_sha->string_if() == nullptr || sidecar_sha == nullptr ||
        sidecar_sha->string_if() == nullptr || sidecar_size == nullptr ||
        sidecar_size->number_if() == nullptr || sidecar_mtime == nullptr ||
        sidecar_mtime->number_if() == nullptr || sidecar_path == nullptr ||
        sidecar_path->string_if() == nullptr)
    {
        return make_error(ErrorCode::kValidation, "XMP exchange baseline is incomplete",
                          {{"path", path}, {"reason", "invalid_xmp_exchange_baseline"}});
    }
    baseline.catalog.recovery_generation = std::stoll(generation->number_if()->text);
    baseline.catalog.recipe_sha256 = *recipe_sha->string_if();
    if (version2)
    {
        const auto *metadata_sha = parsed.value().find("metadata_sha256");
        if (metadata_sha == nullptr || metadata_sha->string_if() == nullptr)
        {
            return make_error(ErrorCode::kValidation, "XMP exchange baseline is incomplete",
                              {{"path", path}, {"reason", "invalid_xmp_exchange_baseline"}});
        }
        baseline.catalog.metadata_sha256 = *metadata_sha->string_if();
    }
    else
    {
        // ADR-0138: v1 baselines predate metadata fingerprints.
        baseline.catalog.metadata_sha256 = empty_metadata_fingerprint();
    }
    baseline.sidecar.sha256 = *sidecar_sha->string_if();
    baseline.sidecar.size_bytes =
        static_cast<std::uint64_t>(std::stoull(sidecar_size->number_if()->text));
    baseline.sidecar.mtime_unix_ms = std::stoll(sidecar_mtime->number_if()->text);
    baseline.sidecar_path = *sidecar_path->string_if();
    return std::optional<ExchangeBaseline>{std::move(baseline)};
}

[[nodiscard]] Result<void> write_exchange_baseline(const std::string_view database_path,
                                                   const std::string_view asset_id,
                                                   const ExchangeBaseline &baseline)
{
    const auto root = exchange_root_for_catalog(database_path);
    std::error_code error;
    std::filesystem::create_directories(utf8_path(root), error);
    if (error)
    {
        return make_error(ErrorCode::kIo, "Unable to create XMP exchange baseline directory",
                          {{"path", root},
                           {"reason", "xmp_exchange_baseline_create_failed"},
                           {"detail", error.message()}});
    }
    JsonValue::Object object{
        {"version", JsonValue::number("2")},
        {"asset_id", std::string(asset_id)},
        {"catalog_generation",
         JsonValue::number(std::to_string(baseline.catalog.recovery_generation))},
        {"recipe_sha256", baseline.catalog.recipe_sha256},
        {"metadata_sha256", baseline.catalog.metadata_sha256},
        {"sidecar_path", baseline.sidecar_path},
        {"sidecar_sha256", baseline.sidecar.sha256},
        {"sidecar_size_bytes", JsonValue::number(std::to_string(baseline.sidecar.size_bytes))},
        {"sidecar_mtime_unix_ms",
         JsonValue::number(std::to_string(baseline.sidecar.mtime_unix_ms))},
    };
    const auto path = exchange_baseline_path(database_path, asset_id);
    return write_utf8_text_file_replace_atomically(path,
                                                   serialize_json(JsonValue{std::move(object)}));
}

[[nodiscard]] bool fingerprints_equal(const XmpInterchangeCatalogFingerprint &left,
                                      const XmpInterchangeCatalogFingerprint &right) noexcept
{
    return left.recovery_generation == right.recovery_generation &&
           left.recipe_sha256 == right.recipe_sha256 &&
           left.metadata_sha256 == right.metadata_sha256;
}

[[nodiscard]] bool fingerprints_equal(const XmpInterchangeSidecarFingerprint &left,
                                      const XmpInterchangeSidecarFingerprint &right) noexcept
{
    return left.sha256 == right.sha256 && left.size_bytes == right.size_bytes &&
           left.mtime_unix_ms == right.mtime_unix_ms;
}

[[nodiscard]] XmpInterchangeConflictClass
classify_conflict(const bool has_sidecar, const bool has_baseline, const bool catalog_has_content,
                  const bool catalog_changed, const bool sidecar_changed) noexcept
{
    if (!has_sidecar)
        return XmpInterchangeConflictClass::kMissing;
    if (!has_baseline)
    {
        return catalog_has_content ? XmpInterchangeConflictClass::kBothChanged :
                                     XmpInterchangeConflictClass::kSidecarNewer;
    }
    if (!catalog_changed && !sidecar_changed)
        return XmpInterchangeConflictClass::kIdentical;
    if (catalog_changed && !sidecar_changed)
        return XmpInterchangeConflictClass::kCatalogNewer;
    if (!catalog_changed && sidecar_changed)
        return XmpInterchangeConflictClass::kSidecarNewer;
    return XmpInterchangeConflictClass::kBothChanged;
}

[[nodiscard]] WritableMetadataPatch
writable_patch_from_present_fields(const WritableMetadata &sidecar)
{
    WritableMetadataPatch patch;
    if (sidecar.title)
    {
        patch.update_title = true;
        patch.title = sidecar.title;
    }
    if (sidecar.description)
    {
        patch.update_description = true;
        patch.description = sidecar.description;
    }
    if (sidecar.creator)
    {
        patch.update_creator = true;
        patch.creator = sidecar.creator;
    }
    if (sidecar.copyright)
    {
        patch.update_copyright = true;
        patch.copyright = sidecar.copyright;
    }
    if (sidecar.country)
    {
        patch.update_country = true;
        patch.country = sidecar.country;
    }
    if (sidecar.province_state)
    {
        patch.update_province_state = true;
        patch.province_state = sidecar.province_state;
    }
    if (sidecar.city)
    {
        patch.update_city = true;
        patch.city = sidecar.city;
    }
    if (sidecar.sublocation)
    {
        patch.update_sublocation = true;
        patch.sublocation = sidecar.sublocation;
    }
    return patch;
}

} // namespace

Result<XmpInterchangeResolve> parse_xmp_interchange_resolve(const std::string_view text)
{
    if (text == "abort")
        return XmpInterchangeResolve::kAbort;
    if (text == "catalog")
        return XmpInterchangeResolve::kCatalog;
    if (text == "sidecar")
        return XmpInterchangeResolve::kSidecar;
    return make_error(ErrorCode::kInvalidArgument, "Unknown XMP interchange resolve mode",
                      {{"resolve", std::string(text)}, {"reason", "invalid_xmp_resolve"}});
}

Result<XmpInterchangeStatus>
CatalogService::xmp_interchange_status(const std::string_view asset_id,
                                       const std::optional<std::string_view> sidecar_path) const
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
        return asset.error();
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Catalog asset was not found",
                          {{"asset_id", std::string(asset_id)}});
    }
    auto snapshot = repository_->snapshot();
    if (!snapshot)
        return snapshot.error();
    auto location = normalize_local_input(asset.value()->normalized_uri);
    if (!location)
        return location.error();
    auto has_edits = asset_has_edits(asset_id);
    if (!has_edits)
        return has_edits.error();
    auto recovery = repository_->recovery_state(asset_id);
    if (!recovery)
        return recovery.error();
    auto recipe_json = repository_->load_recipe_json(asset_id);
    if (!recipe_json)
        return recipe_json.error();
    const std::string recipe_text = recipe_json.value() ? *recipe_json.value() : std::string{};

    XmpInterchangeStatus status;
    status.asset_id = asset.value()->id;
    status.original_path = location.value().path;
    status.has_edits = has_edits.value();
    status.has_adjacent_metadata =
        xmp_adjacent_metadata_catalog_has_content(asset.value()->metadata, asset.value()->tags);
    status.catalog.recovery_generation = recovery.value().generation;
    status.catalog.recipe_sha256 = sha256_utf8_hex(recipe_text);
    status.catalog.metadata_sha256 =
        xmp_adjacent_metadata_fingerprint_sha256(asset.value()->metadata, asset.value()->tags);

    std::optional<std::string> resolved_sidecar;
    if (sidecar_path && !sidecar_path->empty())
    {
        auto normalized = normalize_local_input(*sidecar_path);
        if (!normalized)
            return normalized.error();
        resolved_sidecar = normalized.value().path;
    }
    else
    {
        auto adjacent = adjacent_xmp_path(location.value().path);
        if (!adjacent)
            return adjacent.error();
        resolved_sidecar = std::move(adjacent).value();
    }

    if (resolved_sidecar)
    {
        status.sidecar_path = *resolved_sidecar;
        auto fingerprint = fingerprint_sidecar_file(*resolved_sidecar);
        if (!fingerprint)
            return fingerprint.error();
        status.sidecar = std::move(fingerprint).value();
        auto text = read_utf8_text_file(*resolved_sidecar);
        if (!text)
            return text.error();

        const auto metadata = parse_xmp_adjacent_metadata(text.value());
        status.metadata_parse_ok = metadata.parse_ok;
        status.metadata_parse_reason = metadata.parse_reason;

        if (!is_crs_xmp_document(text.value()))
        {
            status.crs_parse_ok = false;
            // Metadata-only sidecars are allowed (ADR-0138); CRS absence is not
            // an error by itself.
            if (metadata.metadata.has_any_writable_element || metadata.metadata.keyword_paths)
                status.crs_parse_reason = "missing_crs_namespace";
            else
                status.crs_parse_reason = "unsupported_xmp_dialect";
        }
        else
        {
            AssetDescriptor descriptor{asset.value()->id, asset.value()->normalized_uri,
                                       asset.value()->content_fingerprint};
            auto imported = import_crs_xmp({text.value(), descriptor});
            if (!imported)
            {
                status.crs_parse_ok = false;
                const auto reason = imported.error().context.find("reason");
                status.crs_parse_reason =
                    reason != imported.error().context.end() ? reason->second : "unsupported_crs";
            }
            else
            {
                status.crs_parse_ok = true;
                status.omitted = imported.value().omitted;
            }
        }
    }
    else
    {
        status.metadata_parse_ok = true;
    }

    auto baseline = load_exchange_baseline(snapshot.value().database_path, asset_id);
    if (!baseline)
        return baseline.error();
    bool catalog_changed = false;
    bool sidecar_changed = false;
    if (baseline.value())
    {
        status.has_baseline = true;
        status.baseline_catalog = baseline.value()->catalog;
        status.baseline_sidecar = baseline.value()->sidecar;
        catalog_changed = !fingerprints_equal(status.catalog, baseline.value()->catalog);
        if (status.sidecar)
            sidecar_changed = !fingerprints_equal(*status.sidecar, baseline.value()->sidecar);
        else
            sidecar_changed = true;
    }
    const bool catalog_has_content = status.has_edits || status.has_adjacent_metadata;
    status.conflict_class =
        classify_conflict(status.sidecar_path.has_value(), status.has_baseline, catalog_has_content,
                          catalog_changed, sidecar_changed);
    return status;
}

Result<XmpInterchangeImportResult>
CatalogService::xmp_interchange_import(const std::string_view asset_id,
                                       const XmpInterchangeResolve resolve,
                                       const std::optional<std::string_view> sidecar_path)
{
    auto status = xmp_interchange_status(asset_id, sidecar_path);
    if (!status)
        return status.error();
    if (!status.value().sidecar_path)
    {
        return make_error(ErrorCode::kNotFound, "No XMP sidecar is available to import",
                          {{"asset_id", std::string(asset_id)},
                           {"reason", "xmp_sidecar_missing"},
                           {"conflict_class", std::string(xmp_interchange_conflict_class_name(
                                                  status.value().conflict_class))}});
    }
    if (!status.value().metadata_parse_ok)
    {
        return make_error(ErrorCode::kUnsupported, "XMP sidecar adjacent metadata is unsupported",
                          {{"asset_id", std::string(asset_id)},
                           {"path", *status.value().sidecar_path},
                           {"reason", status.value().metadata_parse_reason.value_or(
                                          "unsupported_hierarchical_keyword_shape")},
                           {"conflict_class", std::string(xmp_interchange_conflict_class_name(
                                                  status.value().conflict_class))}});
    }

    auto text = read_utf8_text_file(*status.value().sidecar_path);
    if (!text)
        return text.error();
    const auto adjacent = parse_xmp_adjacent_metadata(text.value());
    if (!adjacent.parse_ok)
    {
        return make_error(
            ErrorCode::kUnsupported, "XMP sidecar adjacent metadata is unsupported",
            {{"asset_id", std::string(asset_id)},
             {"path", *status.value().sidecar_path},
             {"reason", adjacent.parse_reason.value_or("unsupported_hierarchical_keyword_shape")}});
    }

    const bool has_crs_namespace = is_crs_xmp_document(text.value());
    if (has_crs_namespace && !status.value().crs_parse_ok)
    {
        // CRS present but unsupported: never partially apply metadata (ADR-0120).
        return make_error(ErrorCode::kUnsupported, "XMP sidecar is not a supported CRS document",
                          {{"asset_id", std::string(asset_id)},
                           {"path", *status.value().sidecar_path},
                           {"reason", status.value().crs_parse_reason.value_or("unsupported_crs")},
                           {"conflict_class", std::string(xmp_interchange_conflict_class_name(
                                                  status.value().conflict_class))}});
    }

    const bool has_metadata_payload =
        adjacent.metadata.has_any_writable_element || adjacent.metadata.keyword_paths.has_value();
    if (!status.value().crs_parse_ok && !has_metadata_payload)
    {
        return make_error(ErrorCode::kUnsupported, "XMP sidecar has no supported CRS or metadata",
                          {{"asset_id", std::string(asset_id)},
                           {"path", *status.value().sidecar_path},
                           {"reason", status.value().crs_parse_reason.value_or("unsupported_xmp")},
                           {"conflict_class", std::string(xmp_interchange_conflict_class_name(
                                                  status.value().conflict_class))}});
    }

    const auto conflict = status.value().conflict_class;
    const bool allowed = conflict == XmpInterchangeConflictClass::kIdentical ||
                         (conflict == XmpInterchangeConflictClass::kSidecarNewer &&
                          resolve == XmpInterchangeResolve::kSidecar) ||
                         (conflict == XmpInterchangeConflictClass::kBothChanged &&
                          resolve == XmpInterchangeResolve::kSidecar) ||
                         (conflict == XmpInterchangeConflictClass::kCatalogNewer &&
                          resolve == XmpInterchangeResolve::kSidecar);
    if (!allowed)
    {
        return make_error(
            ErrorCode::kConflict, "XMP import requires an explicit conflict resolve",
            {{"asset_id", std::string(asset_id)},
             {"conflict_class", std::string(xmp_interchange_conflict_class_name(conflict))},
             {"resolve", std::string(xmp_interchange_resolve_name(resolve))},
             {"reason", "xmp_import_conflict"}});
    }

    if (conflict == XmpInterchangeConflictClass::kIdentical)
    {
        XmpInterchangeImportResult result;
        auto asset = repository_->find_asset_by_id(asset_id);
        if (!asset || !asset.value())
            return make_error(ErrorCode::kNotFound, "Catalog asset was not found",
                              {{"asset_id", std::string(asset_id)}});
        result.asset = *asset.value();
        result.status = std::move(status).value();
        return result;
    }

    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset || !asset.value())
        return make_error(ErrorCode::kNotFound, "Catalog asset was not found",
                          {{"asset_id", std::string(asset_id)}});

    XmpInterchangeImportResult result;
    result.omitted = status.value().omitted;
    AssetRecord latest = *asset.value();

    if (status.value().crs_parse_ok)
    {
        auto loaded = load_recipe(asset_id);
        if (!loaded)
            return loaded.error();
        auto params = develop_from_recipe(loaded.value());
        if (!params)
            return params.error();
        AssetDescriptor descriptor{asset.value()->id, asset.value()->normalized_uri,
                                   asset.value()->content_fingerprint};
        auto imported = import_crs_xmp({text.value(), descriptor});
        if (!imported)
            return imported.error();
        apply_crs_look(params.value(), imported.value().look, imported.value().mask);
        auto saved = save_develop(asset_id, params.value());
        if (!saved)
            return saved.error();
        latest = std::move(saved).value();
        result.preset_name = imported.value().name;
        result.omitted = imported.value().omitted;
        result.applied_crs = true;
    }

    const auto patch = writable_patch_from_present_fields(adjacent.metadata.writable);
    if (!patch.empty())
    {
        auto mutated =
            set_writable_metadata_selection({std::string(asset_id)}, patch, std::nullopt);
        if (!mutated)
            return mutated.error();
        if (!mutated.value().assets.empty())
            latest = mutated.value().assets.front();
        result.applied_metadata = true;
    }

    if (adjacent.metadata.keyword_paths)
    {
        auto tagged = set_tags(asset_id, *adjacent.metadata.keyword_paths);
        if (!tagged)
            return tagged.error();
        latest = std::move(tagged).value();
        result.applied_keywords = true;
    }

    auto snapshot = repository_->snapshot();
    if (!snapshot)
        return snapshot.error();
    auto refreshed = xmp_interchange_status(asset_id, status.value().sidecar_path);
    if (!refreshed)
        return refreshed.error();
    ExchangeBaseline baseline;
    baseline.catalog = refreshed.value().catalog;
    baseline.sidecar = *refreshed.value().sidecar;
    baseline.sidecar_path = *refreshed.value().sidecar_path;
    auto written = write_exchange_baseline(snapshot.value().database_path, asset_id, baseline);
    if (!written)
    {
        auto error = written.error();
        error.context.insert_or_assign("catalog_committed", "true");
        error.context.insert_or_assign("exchange_baseline_pending", "true");
        return error;
    }
    refreshed = xmp_interchange_status(asset_id, status.value().sidecar_path);
    if (!refreshed)
        return refreshed.error();

    result.asset = std::move(latest);
    result.status = std::move(refreshed).value();
    return result;
}

Result<XmpInterchangeExportResult>
CatalogService::xmp_interchange_export(const std::string_view asset_id,
                                       const XmpInterchangeResolve resolve,
                                       const std::optional<std::string_view> sidecar_path)
{
    auto status = xmp_interchange_status(asset_id, sidecar_path);
    if (!status)
        return status.error();

    std::string destination;
    if (sidecar_path && !sidecar_path->empty())
    {
        auto normalized = normalize_local_input(*sidecar_path);
        if (!normalized)
            return normalized.error();
        destination = normalized.value().path;
    }
    else if (status.value().sidecar_path)
    {
        destination = *status.value().sidecar_path;
    }
    else
    {
        auto path = utf8_path(status.value().original_path);
        path.replace_extension(u8".xmp");
        destination = path_text(path);
    }

    // Re-evaluate conflict against the chosen destination when it differs.
    if (!status.value().sidecar_path || *status.value().sidecar_path != destination)
    {
        std::error_code error;
        if (std::filesystem::is_regular_file(utf8_path(destination), error) && !error)
        {
            auto redirected = xmp_interchange_status(asset_id, destination);
            if (!redirected)
                return redirected.error();
            status = std::move(redirected);
        }
        else
        {
            status.value().sidecar_path.reset();
            status.value().sidecar.reset();
            status.value().conflict_class = XmpInterchangeConflictClass::kMissing;
            status.value().crs_parse_ok = false;
            status.value().crs_parse_reason.reset();
            status.value().omitted.clear();
            status.value().metadata_parse_ok = true;
            status.value().metadata_parse_reason.reset();
        }
    }

    const auto conflict = status.value().conflict_class;
    const bool creating = conflict == XmpInterchangeConflictClass::kMissing;
    const bool allowed = creating || conflict == XmpInterchangeConflictClass::kIdentical ||
                         (conflict == XmpInterchangeConflictClass::kCatalogNewer &&
                          resolve == XmpInterchangeResolve::kCatalog) ||
                         (conflict == XmpInterchangeConflictClass::kBothChanged &&
                          resolve == XmpInterchangeResolve::kCatalog) ||
                         (conflict == XmpInterchangeConflictClass::kSidecarNewer &&
                          resolve == XmpInterchangeResolve::kCatalog);
    if (!allowed)
    {
        return make_error(
            ErrorCode::kConflict, "XMP export requires an explicit conflict resolve",
            {{"asset_id", std::string(asset_id)},
             {"conflict_class", std::string(xmp_interchange_conflict_class_name(conflict))},
             {"resolve", std::string(xmp_interchange_resolve_name(resolve))},
             {"reason", "xmp_export_conflict"}});
    }

    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset || !asset.value())
        return make_error(ErrorCode::kNotFound, "Catalog asset was not found",
                          {{"asset_id", std::string(asset_id)}});

    auto loaded = load_recipe(asset_id);
    if (!loaded)
        return loaded.error();
    auto params = develop_from_recipe(loaded.value());
    if (!params)
        return params.error();
    auto exported = export_xmp_adjacent_interchange(
        {params.value(), "Ravo", asset.value()->metadata, asset.value()->tags});
    if (!exported)
        return exported.error();

    // Preserve original bytes: only write the sidecar path.
    if (conflict != XmpInterchangeConflictClass::kIdentical || creating)
    {
        auto written =
            write_utf8_text_file_replace_atomically(destination, exported.value().xmp_utf8);
        if (!written)
            return written.error();
    }

    auto snapshot = repository_->snapshot();
    if (!snapshot)
        return snapshot.error();
    auto refreshed = xmp_interchange_status(asset_id, destination);
    if (!refreshed)
        return refreshed.error();
    if (!refreshed.value().sidecar)
    {
        return make_error(ErrorCode::kIo, "Exported XMP sidecar was not readable after write",
                          {{"path", destination}, {"reason", "xmp_export_verify_failed"}});
    }
    ExchangeBaseline baseline;
    baseline.catalog = refreshed.value().catalog;
    baseline.sidecar = *refreshed.value().sidecar;
    baseline.sidecar_path = destination;
    auto baseline_written =
        write_exchange_baseline(snapshot.value().database_path, asset_id, baseline);
    if (!baseline_written)
    {
        auto error = baseline_written.error();
        error.context.insert_or_assign("sidecar_written", "true");
        error.context.insert_or_assign("exchange_baseline_pending", "true");
        return error;
    }
    refreshed = xmp_interchange_status(asset_id, destination);
    if (!refreshed)
        return refreshed.error();

    asset = repository_->find_asset_by_id(asset_id);
    if (!asset || !asset.value())
        return make_error(ErrorCode::kNotFound, "Catalog asset was not found",
                          {{"asset_id", std::string(asset_id)}});

    XmpInterchangeExportResult result;
    result.asset = *asset.value();
    result.status = std::move(refreshed).value();
    result.sidecar_path = destination;
    result.omitted_catalog_fields = exported.value().omitted_catalog_fields;
    return result;
}

} // namespace ravo
