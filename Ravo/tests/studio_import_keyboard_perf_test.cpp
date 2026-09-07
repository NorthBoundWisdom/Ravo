#include <chrono>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "interactive_perf_report.h"
#include "ravo/desktop/import_candidate_list_model.h"
#include "studio_import_keyboard_harness.h"
#include "studio_test_support.h"

namespace ravo
{
using namespace studio_test_support;
using interactive_perf_report::CaseMeta;
using interactive_perf_report::emit_case;
using interactive_perf_report::recorded_samples_from_env;
using interactive_perf_report::warmups_from_env;
using studio_import_keyboard_harness::ImportKeyboardHarness;
using studio_import_keyboard_harness::make_candidates;

TEST(StudioImportKeyboardPerf, LargeCandidateFocusScrollBudgets)
{
    ensure_qt_core();
    // Enforceable harness ceilings + PERF-01-style observation only — not a PERF-02 admit.
    constexpr int kCandidateCount = 1200;
    constexpr std::int64_t kFocusMoveCeilingUs = 50'000;   // 50 ms / move
    constexpr std::int64_t kPageScrollCeilingUs = 100'000; // 100 ms / page step

    ImportCandidateListModel model;
    model.setCandidates(make_candidates(kCandidateCount));
    ImportKeyboardHarness harness;
    ASSERT_TRUE(harness.load(&model));
    ASSERT_EQ(harness.currentIndex(), 0);

    const std::size_t warmups = warmups_from_env(1U);
    const std::size_t recorded = recorded_samples_from_env(6U);
    std::vector<std::int64_t> arrow_samples;
    std::vector<std::int64_t> page_samples;
    arrow_samples.reserve(recorded);
    page_samples.reserve(recorded);

    for (std::size_t i = 0; i < warmups + recorded; ++i)
    {
        harness.key(Qt::Key_Home);
        const auto before_y = harness.contentY();
        const auto start = std::chrono::steady_clock::now();
        harness.key(Qt::Key_Down);
        harness.key(Qt::Key_Right);
        const auto arrow_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - start)
                                  .count();
        const auto page_start = std::chrono::steady_clock::now();
        harness.key(Qt::Key_PageDown);
        const auto page_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - page_start)
                                 .count();
        if (i >= warmups)
        {
            arrow_samples.push_back(arrow_us);
            page_samples.push_back(page_us);
        }
        EXPECT_GE(harness.currentIndex(), 0);
        static_cast<void>(before_y);
    }

    CaseMeta arrow_meta;
    arrow_meta.case_id = "import_candidate_keyboard_focus_move";
    arrow_meta.path = "import_candidate_grid";
    arrow_meta.unit = "us";
    arrow_meta.cache_state = "warm";
    arrow_meta.source_kind = "synthetic_candidates";
    arrow_meta.file_count = static_cast<std::size_t>(kCandidateCount);
    arrow_meta.warmups = warmups;
    arrow_meta.recorded_samples = recorded;
    emit_case(arrow_meta, arrow_samples);

    CaseMeta page_meta = arrow_meta;
    page_meta.case_id = "import_candidate_keyboard_page_scroll";
    emit_case(page_meta, page_samples);

    const auto arrow_summary = interactive_perf_report::summarize(arrow_samples);
    const auto page_summary = interactive_perf_report::summarize(page_samples);
    EXPECT_LE(arrow_summary.p90, kFocusMoveCeilingUs)
        << "import focus move p90=" << arrow_summary.p90;
    EXPECT_LE(page_summary.p90, kPageScrollCeilingUs)
        << "import page scroll p90=" << page_summary.p90;
    EXPECT_GT(harness.contentY(), 0);
}

} // namespace ravo
