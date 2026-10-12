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
using studio_import_keyboard_harness::KeyTiming;
using studio_import_keyboard_harness::make_candidates;

namespace
{
JsonValue::Object phase_workload(const std::vector<KeyTiming> &timings, const char *key)
{
    JsonValue::Array pre, dispatch, post, cpu, fonts;
    for (const auto &timing : timings)
    {
        pre.push_back(JsonValue::number(std::to_string(timing.pre_dispatch_us)));
        dispatch.push_back(JsonValue::number(std::to_string(timing.dispatch_us)));
        post.push_back(JsonValue::number(std::to_string(timing.post_dispatch_us)));
        cpu.push_back(timing.process_cpu_us ?
                          JsonValue::number(std::to_string(*timing.process_cpu_us)) :
                          JsonValue{nullptr});
    }
    for (const auto &family : QGuiApplication::font().families())
        fonts.emplace_back(family.toStdString());
    auto workload = JsonValue::Object{{"schema", "ravo.perf01.input-phases/v1"},
                                      {"key", key},
                                      {"pre_dispatch_us", std::move(pre)},
                                      {"dispatch_us", std::move(dispatch)},
                                      {"post_dispatch_us", std::move(post)},
                                      {"process_cpu_us", std::move(cpu)},
                                      {"process_cpu_scope", "all_process_threads"},
                                      {"font_families", std::move(fonts)},
                                      {"qpa", QGuiApplication::platformName().toStdString()},
                                      {"window_width", JsonValue::number("800")},
                                      {"window_height", JsonValue::number("600")},
                                      {"includes_frame_presentation", false}};
#ifdef _WIN32
    workload.emplace("process_cpu_clock", "GetProcessTimes");
#else
    workload.emplace("process_cpu_clock", "std_clock");
#endif
    return workload;
}

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
    std::vector<KeyTiming> down_timings, right_timings, page_timings, combined_timings;

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
        KeyTiming down_timing, right_timing, page_timing;
        harness.key(Qt::Key_Down, Qt::NoModifier, &down_timing);
        const auto down_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - down_start)
                                 .count();
        const int after_down = exclusive_changed_rows;
        exclusive_changed_rows = 0;
        const auto right_start = std::chrono::steady_clock::now();
        harness.key(Qt::Key_Right, Qt::NoModifier, &right_timing);
        const auto right_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - right_start)
                                  .count();
        const int after_right = exclusive_changed_rows;
        const auto page_start = std::chrono::steady_clock::now();
        harness.key(Qt::Key_PageDown, Qt::NoModifier, &page_timing);
        const auto page_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - page_start)
                                 .count();
        if (i >= warmups)
        {
            down_samples.push_back(down_us);
            right_samples.push_back(right_us);
            page_samples.push_back(page_us);
            down_timings.push_back(down_timing);
            right_timings.push_back(right_timing);
            page_timings.push_back(page_timing);
            combined_timings.push_back(
                {down_timing.pre_dispatch_us + right_timing.pre_dispatch_us,
                 down_timing.dispatch_us + right_timing.dispatch_us,
                 down_timing.post_dispatch_us + right_timing.post_dispatch_us,
                 down_timing.process_cpu_us && right_timing.process_cpu_us ?
                     std::optional<std::int64_t>{*down_timing.process_cpu_us +
                                                 *right_timing.process_cpu_us} :
                     std::nullopt});
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
    down.workload = phase_workload(down_timings, "down");
    emit_case(down, down_samples);

    CaseMeta right = base;
    right.case_id = "import_candidate_keyboard_focus_right_" + std::to_string(candidate_count);
    right.workload = phase_workload(right_timings, "right");
    emit_case(right, right_samples);

    // Preserve historical case id semantics: old focus_move counted Down+Right.
    CaseMeta legacy = base;
    legacy.case_id = "import_candidate_keyboard_focus_move";
    legacy.workload = phase_workload(combined_timings, "down_plus_right");
    std::vector<std::int64_t> legacy_samples;
    legacy_samples.reserve(recorded);
    for (std::size_t i = 0; i < recorded; ++i)
        legacy_samples.push_back(down_samples[i] + right_samples[i]);
    emit_case(legacy, legacy_samples);

    CaseMeta page = base;
    page.case_id = "import_candidate_keyboard_page_scroll_" + std::to_string(candidate_count);
    page.workload = phase_workload(page_timings, "page_down");
    emit_case(page, page_samples);
}
} // namespace

TEST(StudioImportKeyboardPerf, LargeCandidateFocusScrollBudgets)
{
    ensure_qt_core();
    observe_focus_scales(1200);
}

TEST(StudioImportKeyboardPerf, InputPhaseTimingPreservesFocusAndWallBoundary)
{
    ensure_qt_core();
    ImportCandidateListModel model;
    model.setCandidates(make_candidates(1000));
    ImportKeyboardHarness harness;
    ASSERT_TRUE(harness.load(&model));
    KeyTiming timing;
    const auto start = std::chrono::steady_clock::now();
    harness.key(Qt::Key_Down, Qt::NoModifier, &timing);
    const auto wall = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - start)
                          .count();
    EXPECT_EQ(harness.currentIndex(), harness.columnCount());
    EXPECT_TRUE(harness.gridHasFocus());
    EXPECT_GE(timing.pre_dispatch_us, 0);
    EXPECT_GE(timing.dispatch_us, 0);
    EXPECT_GE(timing.post_dispatch_us, 0);
    EXPECT_LE(timing.pre_dispatch_us + timing.dispatch_us + timing.post_dispatch_us, wall);
    if (timing.process_cpu_us)
        EXPECT_GE(*timing.process_cpu_us, 0);
    const auto workload = phase_workload({timing}, "down");
    auto encoded = parse_json(serialize_json(JsonValue{workload}));
    ASSERT_TRUE(encoded);
    EXPECT_EQ(encoded.value().find("dispatch_us")->array_if()->size(), 1U);
    EXPECT_FALSE(encoded.value().find("font_families")->array_if()->empty());
    EXPECT_FALSE(*encoded.value().find("includes_frame_presentation")->boolean_if());
}

TEST(StudioImportKeyboardPerf, FocusScrollBudgetsAcrossCandidateScales)
{
    ensure_qt_core();
    for (const int count : {1000, 10000, 100000})
        observe_focus_scales(count);
}

} // namespace ravo
