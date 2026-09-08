#include "ravo/services/metadata_service.h"

#include "ravo/services/catalog_service.h"

namespace ravo
{

MetadataService::MetadataService(CatalogService &catalog) noexcept
    : catalog_(&catalog)
{
}

Result<AssetRecord> MetadataService::set_tags(const std::string_view asset_id,
                                              const std::vector<std::string> &tags)
{
    return catalog_->set_tags(asset_id, tags);
}

Result<KeywordMembershipMutation>
MetadataService::set_tags_selection(const std::vector<std::string> &asset_ids,
                                    const std::vector<std::string> &tags,
                                    const std::optional<std::int64_t> expected_revision)
{
    return catalog_->set_tags_selection(asset_ids, tags, expected_revision);
}

Result<LibraryCaptureFacets> MetadataService::list_capture_facets() const
{
    return catalog_->list_capture_facets();
}

Result<LibraryLocationFacets> MetadataService::list_location_facets() const
{
    return catalog_->list_location_facets();
}

Result<LibraryCaptureFacets> MetadataService::list_capture_facets(const LibraryQuery &scope) const
{
    return catalog_->list_capture_facets(scope);
}

Result<LibraryLocationFacets> MetadataService::list_location_facets(const LibraryQuery &scope) const
{
    return catalog_->list_location_facets(scope);
}

Result<std::vector<KeywordRecord>> MetadataService::list_keywords() const
{
    return catalog_->list_keywords();
}

Result<KeywordMutation>
MetadataService::create_keyword(const std::string_view name,
                                const std::optional<std::string_view> parent_id,
                                const std::optional<std::int64_t> expected_revision)
{
    return catalog_->create_keyword(name, parent_id, expected_revision);
}

Result<KeywordMutation>
MetadataService::rename_keyword(const std::string_view keyword_id, const std::string_view name,
                                const std::optional<std::int64_t> expected_revision)
{
    return catalog_->rename_keyword(keyword_id, name, expected_revision);
}

Result<KeywordMutation>
MetadataService::move_keyword(const std::string_view keyword_id,
                              const std::optional<std::string_view> parent_id,
                              const std::optional<std::int64_t> expected_revision)
{
    return catalog_->move_keyword(keyword_id, parent_id, expected_revision);
}

Result<std::int64_t>
MetadataService::delete_keyword(const std::string_view keyword_id, const bool recursive,
                                const std::optional<std::int64_t> expected_revision)
{
    return catalog_->delete_keyword(keyword_id, recursive, expected_revision);
}

Result<AssetRecord> MetadataService::set_writable_metadata(const std::string_view asset_id,
                                                           const WritableMetadata &metadata)
{
    return catalog_->set_writable_metadata(asset_id, metadata);
}

Result<WritableMetadataMutation> MetadataService::set_writable_metadata_selection(
    const std::vector<std::string> &asset_ids, const WritableMetadataPatch &patch,
    const std::optional<std::int64_t> expected_revision)
{
    return catalog_->set_writable_metadata_selection(asset_ids, patch, expected_revision);
}

} // namespace ravo
