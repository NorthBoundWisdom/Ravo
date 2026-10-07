#include "ravo/services/metadata_service.h"

#include <filesystem>
#include <utility>
#include <set>

#include "catalog_internal.h"
#include "ravo/engine/engine.h"
#include "catalog_service_internal.h"
#include "ravo/domain/catalog_repository.h"
#include "ravo/domain/uri.h"
#include "ravo/services/recovery_service.h"

namespace ravo
{
using namespace catalog_service_internal;

MetadataService::MetadataService(const std::unique_ptr<CatalogRepository> &repository,
                                 const EngineFacade *const &engine,
                                 RecoveryService &recovery_service) noexcept
    : repository_(repository)
    , engine_(engine)
    , recovery_service_(recovery_service)
{
}

Result<AssetRecord> MetadataService::set_tags(const std::string_view asset_id,
                                              const std::vector<std::string> &tags)
{
    auto mutated = set_tags_selection({std::string(asset_id)}, tags, std::nullopt);
    if (!mutated)
    {
        return mutated.error();
    }
    if (mutated.value().assets.empty())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    return mutated.value().assets.front();
}

Result<WritableMetadataMutation> MetadataService::set_writable_metadata_selection(
    const std::vector<std::string> &asset_ids, const WritableMetadataPatch &patch,
    const std::optional<std::int64_t> expected_revision)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto mutated = repository_->patch_assets_writable_metadata(asset_ids, patch, expected_revision);
    if (!mutated)
    {
        return mutated.error();
    }
    for (const auto &asset : mutated.value().assets)
    {
        auto recovered = recovery_service_.synchronize_committed_change(asset.id);
        if (!recovered)
        {
            return recovered.error();
        }
    }
    return mutated;
}

Result<AssetRecord> MetadataService::set_writable_metadata(const std::string_view asset_id,
                                                           const WritableMetadata &metadata)
{
    auto mutated = set_writable_metadata_selection(
        {std::string(asset_id)}, writable_metadata_patch_all(metadata), std::nullopt);
    if (!mutated)
    {
        return mutated.error();
    }
    if (mutated.value().assets.empty())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    return mutated.value().assets.front();
}

Result<KeywordMembershipMutation>
MetadataService::set_tags_selection(const std::vector<std::string> &asset_ids,
                                    const std::vector<std::string> &tags,
                                    const std::optional<std::int64_t> expected_revision)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto mutated = repository_->replace_assets_tags(asset_ids, tags, expected_revision);
    if (!mutated)
    {
        return mutated.error();
    }
    for (const auto &asset : mutated.value().assets)
    {
        auto recovered = recovery_service_.synchronize_committed_change(asset.id);
        if (!recovered)
        {
            return recovered.error();
        }
    }
    return mutated;
}

Result<LibraryCaptureFacets> MetadataService::list_capture_facets() const
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    return repository_->list_capture_facets();
}

Result<LibraryLocationFacets> MetadataService::list_location_facets() const
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    return repository_->list_location_facets();
}

Result<LibraryCaptureFacets> MetadataService::list_capture_facets(const LibraryQuery &scope) const
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    // A scoped count must reject exactly what `list_assets` rejects, otherwise a
    // facet panel could show totals for a selection the library cannot list.
    auto valid_scope = validate_library_query(scope);
    if (!valid_scope)
    {
        return valid_scope.error();
    }
    return repository_->list_capture_facets(scope);
}

Result<LibraryLocationFacets> MetadataService::list_location_facets(const LibraryQuery &scope) const
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto valid_scope = validate_library_query(scope);
    if (!valid_scope)
    {
        return valid_scope.error();
    }
    return repository_->list_location_facets(scope);
}

Result<std::vector<KeywordRecord>> MetadataService::list_keywords() const
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    return repository_->list_keywords();
}

Result<KeywordMutation>
MetadataService::create_keyword(const std::string_view name,
                                const std::optional<std::string_view> parent_id,
                                const std::optional<std::int64_t> expected_revision)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    return repository_->create_keyword(name, parent_id, expected_revision);
}

Result<KeywordMutation>
MetadataService::rename_keyword(const std::string_view keyword_id, const std::string_view name,
                                const std::optional<std::int64_t> expected_revision)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    return repository_->rename_keyword(keyword_id, name, expected_revision);
}

Result<KeywordMutation>
MetadataService::move_keyword(const std::string_view keyword_id,
                              const std::optional<std::string_view> parent_id,
                              const std::optional<std::int64_t> expected_revision)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    return repository_->move_keyword(keyword_id, parent_id, expected_revision);
}

Result<std::int64_t>
MetadataService::delete_keyword(const std::string_view keyword_id, const bool recursive,
                                const std::optional<std::int64_t> expected_revision)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    return repository_->delete_keyword(keyword_id, recursive, expected_revision);
}

Result<AssetRecord> MetadataService::refresh_capture_metadata(const std::string_view asset_id,
                                                              const CancellationToken &cancellation)
{
    auto active = cancellation.check();
    if (!active)
        return active.error();
    if (repository_ == nullptr || engine_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    auto existing = repository_->find_asset_by_id(asset_id);
    if (!existing)
        return existing.error();
    if (!existing.value())
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    auto location = normalize_local_input(existing.value()->normalized_uri);
    if (!location)
        return location.error();
    auto identity = read_file_identity(location.value().path);
    if (!identity)
        return identity.error();

    CaptureMetadata refreshed;
    if (is_raw_media_type(existing.value()->media_type))
    {
        auto inspected = engine_->inspect(location.value().path, cancellation);
        if (!inspected)
            return inspected.error();
        if (!inspected.value().is_raw)
            return make_error(ErrorCode::kValidation,
                              "Catalog RAW asset no longer identifies as RAW",
                              {{"asset_id", std::string(asset_id)},
                               {"reason", "metadata_refresh_media_mismatch"}});
        if (!inspected.value().make.empty())
            refreshed.camera_make = inspected.value().make;
        if (!inspected.value().model.empty())
            refreshed.camera_model = inspected.value().model;
        refreshed.iso = inspected.value().iso;
        refreshed.aperture = inspected.value().aperture;
        refreshed.focal_length_mm = inspected.value().focal_length_mm;
        refreshed.shutter_s = inspected.value().shutter_s;
        refreshed.captured_unix_s = inspected.value().captured_unix_s;
    }
    if (media_type_has_embedded_capture(existing.value()->media_type))
    {
        auto extracted =
            engine_->read_embedded_capture_metadata(location.value().path, cancellation);
        if (!extracted)
            return extracted.error();
        merge_engine_capture(refreshed, extracted.value());
    }
    auto valid = validate_capture_metadata(refreshed);
    if (!valid)
        return valid.error();
    active = cancellation.check();
    if (!active)
        return active.error();
    AssetRecord updated = *existing.value();
    updated.size_bytes = identity.value().size_bytes;
    updated.mtime_unix_ms = identity.value().mtime_unix_ms;
    updated.content_fingerprint = make_content_fingerprint(identity.value());
    updated.import_state = std::string(kImportStateImported);
    updated.error_code.reset();
    updated.error_message.reset();
    updated.capture = std::move(refreshed);
    auto published = repository_->commit_refreshed_asset(updated);
    if (!published)
        return published.error();
    auto recovered = recovery_service_.synchronize_committed_change(asset_id, cancellation);
    if (!recovered)
        return recovered.error();
    return updated;
}

} // namespace ravo
