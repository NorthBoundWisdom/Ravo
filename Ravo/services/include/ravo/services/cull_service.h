#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ravo/domain/types.h"
#include "ravo/services/cull_assistance.h"

namespace ravo
{
class CatalogService;
class CatalogRepository;
class RasterDecoder;
class EngineFacade;
class PreviewCache;
class RecoveryStore;
class ImportService;
class LibraryService;
class RecoveryService;

// One owner-thread use-case service. Borrowed slots/capabilities outlive this
// object; closed resource slots preserve the existing structured failures.
class CullService
{
public:
    CullService(const CullService &) = delete;
    CullService &operator=(const CullService &) = delete;
    CullService(CullService &&) = delete;
    CullService &operator=(CullService &&) = delete;

    [[nodiscard]] Result<CullReviewResult> apply_cull_review(const CullReviewRequest &request);
    [[nodiscard]] Result<ExactDuplicateReport>
    find_exact_duplicate_groups(const ExactDuplicateRequest &request = {}) const;
    [[nodiscard]] Result<NearDuplicateReport>
    find_near_duplicate_groups(const NearDuplicateRequest &request = {}) const;
    [[nodiscard]] Result<BurstProposeReport>
    propose_burst_groups(const BurstProposeRequest &request = {}) const;
    [[nodiscard]] Result<BurstAcceptResult>
    accept_burst_group_proposal(const BurstAcceptRequest &request);
    [[nodiscard]] Result<BurstComparePair>
    resolve_burst_compare_pair(const BurstCompareRequest &request) const;
    [[nodiscard]] Result<CullSuggestionDismissResult>
    dismiss_cull_suggestion(const CullSuggestionDismissRequest &request);
    [[nodiscard]] Result<bool> is_cull_suggestion_dismissed(CullSuggestionKind kind,
                                                            std::string_view key) const;

private:
    friend class CatalogService;
    CullService(const std::unique_ptr<CatalogRepository> &repository, ImportService &import_service,
                LibraryService &library_service, RecoveryService &recovery_service) noexcept;

    const std::unique_ptr<CatalogRepository> &repository_;
    ImportService &import_service_;
    LibraryService &library_service_;
    RecoveryService &recovery_service_;
    const std::thread::id owner_thread_{std::this_thread::get_id()};
    [[nodiscard]] Result<void> check_analysis_thread() const;
};

} // namespace ravo
