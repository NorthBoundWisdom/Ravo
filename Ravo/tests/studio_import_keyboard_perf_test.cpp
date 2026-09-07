#include <chrono>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "interactive_perf_report.h"
#include "ravo/desktop/import_candidate_list_model.h"
#include "studio_import_keyboard_harness.h"
#include "studio_test_support.h"
#include <QAbstractItemModel>

namespace ravo
{
using namespace studio_test_support;
using interactive_perf_report::CaseMeta;
using interactive_perf_report::emit_case;
using interactive_perf_report::recorded_samples_from_env;
using interactive_perf_report::warmups_from_env;
using studio_import_keyboard_harness::ImportKeyboardHarness;
using studio_import_keyboard_harness::make_candidates;

namespace
{
void observe_focus_scales(const int candidate_count)
{
    // Observation only — not a PERF-02 / C3 admit. processEvents completion is
    // input-handling done, not frame-presented.
    constexpr std::int64_t kFocusMoveCeilingUs = 50'000;
    constexpr std::int64_t kPageScrollCeilingUs = 100'000;

    ImportCandidateListModel model;
    model.setCandidates(make_candidates(candidate_count));
    ImportKeyboardHarness harness;
    ASSERT_TRUE(harness.load(&model));
    ASSERT_EQ(harness.currentIndex(), 0);

    const std::size_t warmups = warmups_from_env(1U);
    const std::size_t recorded = recorded_samples_from_env(6U);
    std::vector<std::int64_t> down_samples;
    std::vector<std::int64_t> right_samples;
    std::vector<std::int64_t> page_samples;
    down_samples.reserve(recorded);
    right_samples.reserve(recorded);
    page_samples.reserve(recorded);

    int exclusive_changed_rows = 0;
    QObject::connect(&model, &QAbstractItemModel::dataChanged, &model,
                     [&](const QModelIndex &top, const QModelIndex &bottom, const QList<int> &roles)
                     {
                         if (roles.contains(ImportCandidateListModel::HighlightedRole) ||
                             roles.isEmpty())
                             exclusive_changed_rows += bottom.row() - top.row() + 1;
                     });

    for (std::size_t i = 0; i < warmups + recorded; ++i)
    {
        harness.key(Qt::Key_Home);
        exclusive_changed_rows = 0;
        const auto down_start = std::chrono::steady_clock::now();
        harness.key(Qt::Key_Down);
        const auto down_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - down_start)
                                 .count();
        const int after_down = exclusive_changed_rows;
        exclusive_changed_rows = 0;
        const auto right_start = std::chrono::steady_clock::now();
        harness.key(Qt::Key_Right);
        const auto right_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - right_start)
                                  .count();
        const int after_right = exclusive_changed_rows;
        const auto page_start = std::chrono::steady_clock::now();
        harness.key(Qt::Key_PageDown);
        const auto page_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - page_start)
                                 .count();
        if (i >= warmups)
        {
            down_samples.push_back(down_us);
            right_samples.push_back(right_us);
            page_samples.push_back(page_us);
            EXPECT_LE(after_down, 2);
            EXPECT_LE(after_right, 2);
        }
        EXPECT_GE(harness.currentIndex(), 0);
        if (i >= warmups)
        {
            EXPECT_LE(down_us, kFocusMoveCeilingUs);
            EXPECT_LE(right_us, kFocusMoveCeilingUs);
            EXPECT_LE(page_us, kPageScrollCeilingUs);
        }
    }

    CaseMeta base;
    base.path = "import_candidate_grid";
    base.unit = "us";
    base.cache_state = "warm";
    base.source_kind = "synthetic_candidates";
    base.file_count = static_cast<std::size_t>(candidate_count);
    base.warmups = warmups;
    base.recorded_samples = recorded;
    CaseMeta down = base;
    down.case_id = "import_candidate_keyboard_focus_down_" + std::to_string(candidate_count);
    emit_case(down, down_samples);

    CaseMeta right = base;
    right.case_id = "import_candidate_keyboard_focus_right_" + std::to_string(candidate_count);
    emit_case(right, right_samples);

    // Preserve historical case id semantics: old focus_move counted Down+Right.
    CaseMeta legacy = base;
    legacy.case_id = "import_candidate_keyboard_focus_move";
    std::vector<std::int64_t> legacy_samples;
    legacy_samples.reserve(recorded);
    for (std::size_t i = 0; i < recorded; ++i)
        legacy_samples.push_back(down_samples[i] + right_samples[i]);
    emit_case(legacy, legacy_samples);

    CaseMeta page = base;
    page.case_id = "import_candidate_keyboard_page_scroll_" + std::to_string(candidate_count);
    emit_case(page, page_samples);
}
} // namespace

TEST(StudioImportKeyboardPerf, LargeCandidateFocusScrollBudgets)
{
    ensure_qt_core();
    observe_focus_scales(1200);
}

TEST(StudioImportKeyboardPerf, FocusScrollBudgetsAcrossCandidateScales)
{
    ensure_qt_core();
    for (const int count : {1000, 10000, 100000})
        observe_focus_scales(count);
}

} // namespace ravo
