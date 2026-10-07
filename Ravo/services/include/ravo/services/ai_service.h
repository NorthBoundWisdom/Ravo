#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/domain/types.h"
#include "ravo/services/ai_proposal.h"
#include "ravo/services/ai_suggestion.h"

namespace ravo
{
class CatalogService;
class CatalogRepository;
class RasterDecoder;
class EngineFacade;
class PreviewCache;
class RecoveryStore;
class DevelopService;
class LibraryService;
class MetadataService;
class RecoveryService;

// One owner-thread use-case service. Borrowed slots/capabilities outlive this
// object; closed resource slots preserve the existing structured failures.
class AiService
{
public:
    AiService(const AiService &) = delete;
    AiService &operator=(const AiService &) = delete;
    AiService(AiService &&) = delete;
    AiService &operator=(AiService &&) = delete;

    [[nodiscard]] Result<AiProposal> create_ai_proposal(const AiProposalCreateRequest &request);
    [[nodiscard]] Result<std::vector<AiProposal>>
    create_shoot_consistency_proposals(const AiShootConsistencyRequest &request);
    [[nodiscard]] Result<AiProposal> get_ai_proposal(std::string_view proposal_id) const;
    [[nodiscard]] Result<std::vector<AiProposal>>
    list_ai_proposals(std::optional<std::string_view> asset_id = std::nullopt) const;
    [[nodiscard]] Result<AiProposalApplyResult>
    apply_ai_proposal(std::string_view proposal_id,
                      std::optional<std::int64_t> expected_catalog_revision = std::nullopt);
    [[nodiscard]] Result<AiProposal> reject_ai_proposal(std::string_view proposal_id);
    [[nodiscard]] Result<AiProposal> cancel_ai_proposal(std::string_view proposal_id);
    [[nodiscard]] Result<AiSuggestion>
    create_ai_suggestion(const AiSuggestionCreateRequest &request);
    [[nodiscard]] Result<AiSuggestion> get_ai_suggestion(std::string_view suggestion_id) const;
    [[nodiscard]] Result<std::vector<AiSuggestion>>
    list_ai_suggestions(std::optional<std::string_view> asset_id = std::nullopt) const;
    [[nodiscard]] Result<AiSuggestionAcceptResult>
    accept_ai_suggestion(std::string_view suggestion_id,
                         std::optional<std::int64_t> expected_catalog_revision = std::nullopt);
    [[nodiscard]] Result<AiSuggestion> reject_ai_suggestion(std::string_view suggestion_id);
    [[nodiscard]] Result<AiSuggestion> cancel_ai_suggestion(std::string_view suggestion_id);

private:
    friend class CatalogService;
    AiService(const std::unique_ptr<CatalogRepository> &repository, DevelopService &develop_service,
              LibraryService &library_service, MetadataService &metadata_service,
              RecoveryService &recovery_service) noexcept;
    [[nodiscard]] Result<std::filesystem::path> ai_proposals_directory() const;
    [[nodiscard]] Result<void> ensure_ai_proposals_loaded() const;
    [[nodiscard]] Result<void> persist_ai_proposal(const AiProposal &proposal);
    [[nodiscard]] Result<std::filesystem::path> ai_suggestions_directory() const;
    [[nodiscard]] Result<void> ensure_ai_suggestions_loaded() const;
    [[nodiscard]] Result<void> persist_ai_suggestion(const AiSuggestion &suggestion);
    mutable std::map<std::string, AiProposal, std::less<>> ai_proposals_;
    mutable bool ai_proposals_loaded_ = false;
    mutable std::map<std::string, AiSuggestion, std::less<>> ai_suggestions_;
    mutable bool ai_suggestions_loaded_ = false;

    const std::unique_ptr<CatalogRepository> &repository_;
    DevelopService &develop_service_;
    LibraryService &library_service_;
    MetadataService &metadata_service_;
    RecoveryService &recovery_service_;
};

} // namespace ravo
