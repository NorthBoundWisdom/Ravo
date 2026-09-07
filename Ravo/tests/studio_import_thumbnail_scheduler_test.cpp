#include <gtest/gtest.h>

#include "ravo/desktop/import_candidate_list_model.h"
#include "studio_import_thumbnail_controller.h"
#include "studio_test_support.h"

namespace ravo
{
using namespace studio_test_support;

TEST(StudioImportThumbnailScheduler, ViewportDemandCapsPendingAndPrefersNewViewport)
{
    ensure_qt_core();
    ImportCandidateListModel model;
    std::vector<ImportCandidate> candidates(100000);
    for (int row = 0; row < 100000; ++row)
        candidates[static_cast<std::size_t>(row)].source_path = std::to_string(row);
    model.setCandidates(std::move(candidates));

    StudioImportThumbnailController controller({
        &model,
        nullptr,
        [] { return true; },
        [] { return false; },
        [] { return false; },
        [] { return 1ULL; },
        {},
    });

    std::vector<int> start_rows(300);
    for (int i = 0; i < 300; ++i)
        start_rows[static_cast<std::size_t>(i)] = i;
    controller.setViewportDemand(start_rows, 2);
    EXPECT_LE(controller.pendingCount(), controller.pendingHardCap());
    EXPECT_EQ(controller.pendingHardCap(), 256U);

    // Jump to end: pending should reflect new viewport, not retain all early rows.
    std::vector<int> end_rows;
    for (int row = 99900; row < 100000; ++row)
        end_rows.push_back(row);
    controller.setViewportDemand(end_rows, 2);
    EXPECT_LE(controller.pendingCount(), controller.pendingHardCap());
    // First pending should be in the new viewport band.
    // We cannot peek the set; ensure pending is small and demand was replaced.
    EXPECT_LE(controller.pendingCount(), end_rows.size() + 2U);
}

TEST(StudioImportThumbnailScheduler, EnsureRespectsPendingHardCap)
{
    ensure_qt_core();
    ImportCandidateListModel model;
    std::vector<ImportCandidate> candidates(1000);
    for (int row = 0; row < 1000; ++row)
        candidates[static_cast<std::size_t>(row)].source_path = std::to_string(row);
    model.setCandidates(std::move(candidates));
    StudioImportThumbnailController controller({
        &model,
        nullptr,
        [] { return true; },
        [] { return false; },
        [] { return false; },
        [] { return 1ULL; },
        {},
    });
    for (int row = 0; row < 1000; ++row)
        controller.ensure(row);
    EXPECT_LE(controller.pendingCount(), controller.pendingHardCap());
}

} // namespace ravo
