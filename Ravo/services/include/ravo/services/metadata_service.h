#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/domain/types.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"

namespace ravo
{

class CatalogService;
class CatalogRepository;
class EngineFacade;
class RecoveryService;

class MetadataService
{
public:
    MetadataService(const MetadataService &) = delete;
    MetadataService &operator=(const MetadataService &) = delete;
    MetadataService(MetadataService &&) = delete;
    MetadataService &operator=(MetadataService &&) = delete;

    [[nodiscard]] Result<AssetRecord> set_tags(std::string_view asset_id,
                                               const std::vector<std::string> &tags);
    [[nodiscard]] Result<KeywordMembershipMutation>
    set_tags_selection(const std::vector<std::string> &asset_ids,
                       const std::vector<std::string> &tags,
                       std::optional<std::int64_t> expected_revision = std::nullopt);
    [[nodiscard]] Result<LibraryCaptureFacets> list_capture_facets() const;
    [[nodiscard]] Result<LibraryLocationFacets> list_location_facets() const;
    [[nodiscard]] Result<LibraryCaptureFacets> list_capture_facets(const LibraryQuery &scope) const;
    [[nodiscard]] Result<LibraryLocationFacets>
    list_location_facets(const LibraryQuery &scope) const;
    [[nodiscard]] Result<std::vector<KeywordRecord>> list_keywords() const;
    [[nodiscard]] Result<KeywordMutation>
    create_keyword(std::string_view name, std::optional<std::string_view> parent_id = std::nullopt,
                   std::optional<std::int64_t> expected_revision = std::nullopt);
    [[nodiscard]] Result<KeywordMutation>
    rename_keyword(std::string_view keyword_id, std::string_view name,
                   std::optional<std::int64_t> expected_revision = std::nullopt);
    [[nodiscard]] Result<KeywordMutation>
    move_keyword(std::string_view keyword_id,
                 std::optional<std::string_view> parent_id = std::nullopt,
                 std::optional<std::int64_t> expected_revision = std::nullopt);
    [[nodiscard]] Result<std::int64_t>
    delete_keyword(std::string_view keyword_id, bool recursive = false,
                   std::optional<std::int64_t> expected_revision = std::nullopt);
    [[nodiscard]] Result<AssetRecord> set_writable_metadata(std::string_view asset_id,
                                                            const WritableMetadata &metadata);
    [[nodiscard]] Result<WritableMetadataMutation>
    set_writable_metadata_selection(const std::vector<std::string> &asset_ids,
                                    const WritableMetadataPatch &patch,
                                    std::optional<std::int64_t> expected_revision = std::nullopt);

    [[nodiscard]] Result<AssetRecord>
    refresh_capture_metadata(std::string_view asset_id, const CancellationToken &cancellation);

private:
    friend class CatalogService;
    // Borrowed owner slots stay valid until this capability is destroyed. The
    // composition owner is immovable; reset slots make post-close calls fail.
    MetadataService(const std::unique_ptr<CatalogRepository> &repository,
                    const EngineFacade *const &engine, RecoveryService &recovery_service) noexcept;

    const std::unique_ptr<CatalogRepository> &repository_;
    const EngineFacade *const &engine_;
    RecoveryService &recovery_service_;
};

} // namespace ravo
