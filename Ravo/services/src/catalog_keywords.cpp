#include "ravo/services/catalog_service.h"

#include "catalog_internal.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace ravo
{

Result<KeywordMembershipMutation>
CatalogService::set_tags_selection(const std::vector<std::string> &asset_ids,
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
        auto recovered = synchronize_committed_change(asset.id);
        if (!recovered)
        {
            return recovered.error();
        }
    }
    return mutated;
}

Result<LibraryCaptureFacets> CatalogService::list_capture_facets() const
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    return repository_->list_capture_facets();
}

Result<LibraryLocationFacets> CatalogService::list_location_facets() const
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    return repository_->list_location_facets();
}

Result<std::vector<KeywordRecord>> CatalogService::list_keywords() const
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    return repository_->list_keywords();
}

Result<KeywordMutation>
CatalogService::create_keyword(const std::string_view name,
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
CatalogService::rename_keyword(const std::string_view keyword_id, const std::string_view name,
                               const std::optional<std::int64_t> expected_revision)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    return repository_->rename_keyword(keyword_id, name, expected_revision);
}

Result<KeywordMutation>
CatalogService::move_keyword(const std::string_view keyword_id,
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
CatalogService::delete_keyword(const std::string_view keyword_id, const bool recursive,
                               const std::optional<std::int64_t> expected_revision)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    return repository_->delete_keyword(keyword_id, recursive, expected_revision);
}

} // namespace ravo
