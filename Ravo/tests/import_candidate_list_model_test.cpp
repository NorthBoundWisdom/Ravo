#include <algorithm>
#include <random>
#include <string>
#include <QImage>
#include <gtest/gtest.h>
#include <QAbstractItemModel>

#include "ravo/desktop/import_candidate_list_model.h"
#include "studio_test_support.h"

namespace ravo
{
using namespace studio_test_support;

namespace
{
ImportCandidate make_row(const char *path, const std::uint64_t bytes, const bool duplicate = false)
{
    ImportCandidate candidate;
    candidate.source_path = path;
    candidate.display_name = path;
    candidate.size_bytes = bytes;
    candidate.duplicate = duplicate;
    if (duplicate)
        candidate.duplicate_reason = "catalog_content";
    return candidate;
}
} // namespace

TEST(ImportCandidateListModel, UpdateCandidateNotifiesWhenSelectedBytesChange)
{
    ensure_qt_core();
    ImportCandidateListModel model;
    model.setCandidates({make_row("/a.png", 10), make_row("/b.png", 20)});
    ASSERT_EQ(model.selectedCount(), 2);
    ASSERT_EQ(model.selectedBytes(), 30ULL);

    int notifications = 0;
    QObject::connect(&model, &ImportCandidateListModel::selectionChanged, &model,
                     [&] { ++notifications; });

    model.updateCandidate(0, make_row("/a.png", 20));
    EXPECT_EQ(model.selectedCount(), 2);
    EXPECT_EQ(model.selectedBytes(), 40ULL);
    EXPECT_GE(notifications, 1);

    model.toggleSelected(1);
    ASSERT_FALSE(model.data(model.index(1, 0), ImportCandidateListModel::SelectedRole).toBool());
    notifications = 0;
    model.updateCandidate(1, make_row("/b.png", 50));
    EXPECT_EQ(model.selectedCount(), 1);
    EXPECT_EQ(model.selectedBytes(), 20ULL);
    EXPECT_EQ(notifications, 0) << "unselected byte changes must not notify selection totals";

    notifications = 0;
    model.updateCandidate(0, make_row("/a.png", 20));
    EXPECT_EQ(notifications, 0) << "identical aggregates must not notify";

    notifications = 0;
    model.updateCandidate(0, make_row("/a.png", 20, true));
    EXPECT_EQ(model.selectedCount(), 0);
    EXPECT_EQ(model.selectedBytes(), 0ULL);
    EXPECT_GE(notifications, 1);
}

TEST(ImportCandidateListModel, SelectionOracleMatchesModel)
{
    ensure_qt_core();

    enum class OpKind
    {
        HighlightExclusive,
        HighlightToggle,
        HighlightRange,
        HighlightAll,
        ApplyCheck,
        ToggleSelected,
        SetAllSelected,
        SelectRange,
        UpdateCandidate,
        AppendCandidate,
        ApplyScanBatch,
        Rebuild,
        InvalidRowNoOp,
    };

    struct Op
    {
        OpKind kind = OpKind::HighlightExclusive;
        int first = 0;
        int last = 0;
        bool additive = false;
        bool selected = false;
        int duplicate_row = -1;
        int count = 0;
        std::uint64_t bytes = 0;
        bool make_duplicate = false;
        bool make_unsupported = false;
        std::string path;
    };

    struct RefRow
    {
        bool selected = false;
        bool highlighted = false;
        bool eligible = true;
        bool supported = true;
        bool duplicate = false;
        std::uint64_t bytes = 0;
        std::string path;
    };

    struct Reference
    {
        std::vector<RefRow> rows;
        bool select_new_candidates = true;

        static bool eligible(const RefRow &row) noexcept
        {
            return row.supported && !row.duplicate;
        }

        void rebuild(const int count, const int duplicate_row)
        {
            rows.clear();
            rows.reserve(static_cast<std::size_t>(count));
            for (int row = 0; row < count; ++row)
            {
                RefRow entry;
                entry.path = "/oracle-" + std::to_string(row) + ".png";
                entry.bytes = static_cast<std::uint64_t>((row + 1) * 10);
                entry.duplicate = row == duplicate_row;
                entry.supported = true;
                entry.eligible = eligible(entry);
                entry.selected = select_new_candidates && entry.eligible;
                entry.highlighted = false;
                rows.push_back(entry);
            }
        }

        void highlight_exclusive(const int row)
        {
            if (row < 0 || row >= static_cast<int>(rows.size()))
                return;
            if (!eligible(rows[static_cast<std::size_t>(row)]))
                return; // production: leave prior highlight untouched
            for (auto &entry : rows)
                entry.highlighted = false;
            rows[static_cast<std::size_t>(row)].highlighted = true;
        }

        void highlight_toggle(const int row)
        {
            if (row < 0 || row >= static_cast<int>(rows.size()))
                return;
            auto &entry = rows[static_cast<std::size_t>(row)];
            if (!eligible(entry))
                return;
            entry.highlighted = !entry.highlighted;
        }

        void highlight_range(int first, int last, const bool additive)
        {
            if (rows.empty())
                return;
            first = std::clamp(first, 0, static_cast<int>(rows.size()) - 1);
            last = std::clamp(last, 0, static_cast<int>(rows.size()) - 1);
            if (first > last)
                std::swap(first, last);
            if (!additive)
            {
                for (int row = 0; row < static_cast<int>(rows.size()); ++row)
                    if (row < first || row > last)
                        rows[static_cast<std::size_t>(row)].highlighted = false;
            }
            for (int row = first; row <= last; ++row)
                if (eligible(rows[static_cast<std::size_t>(row)]))
                    rows[static_cast<std::size_t>(row)].highlighted = true;
        }

        void highlight_all()
        {
            for (auto &entry : rows)
                entry.highlighted = eligible(entry);
        }

        void apply_check(const int row)
        {
            if (row < 0 || row >= static_cast<int>(rows.size()))
                return;
            auto &clicked = rows[static_cast<std::size_t>(row)];
            if (!eligible(clicked))
                return;
            const bool next = !clicked.selected;
            if (clicked.highlighted)
            {
                for (auto &entry : rows)
                    if (entry.highlighted && eligible(entry))
                        entry.selected = next;
            }
            else
                clicked.selected = next;
        }

        void toggle_selected(const int row)
        {
            if (row < 0 || row >= static_cast<int>(rows.size()))
                return;
            auto &entry = rows[static_cast<std::size_t>(row)];
            if (!eligible(entry))
                return;
            entry.selected = !entry.selected;
        }

        void set_all_selected(const bool selected)
        {
            select_new_candidates = selected;
            for (auto &entry : rows)
                entry.selected = selected && eligible(entry);
        }

        void select_range(int first, int last, const bool additive)
        {
            if (rows.empty())
                return;
            first = std::clamp(first, 0, static_cast<int>(rows.size()) - 1);
            last = std::clamp(last, 0, static_cast<int>(rows.size()) - 1);
            if (first > last)
                std::swap(first, last);
            if (!additive)
            {
                for (auto &entry : rows)
                    entry.selected = false;
            }
            for (int row = first; row <= last; ++row)
            {
                auto &entry = rows[static_cast<std::size_t>(row)];
                if (eligible(entry))
                    entry.selected = true;
            }
        }

        void update_candidate(const int row, const std::uint64_t bytes, const bool make_duplicate,
                              const bool make_unsupported)
        {
            if (row < 0 || row >= static_cast<int>(rows.size()))
                return;
            auto &entry = rows[static_cast<std::size_t>(row)];
            entry.bytes = bytes;
            // Match production: scan-owned duplicate sticks for this generation.
            if (make_duplicate || entry.duplicate)
                entry.duplicate = true;
            if (make_unsupported)
                entry.supported = false;
            entry.eligible = eligible(entry);
            if (!entry.eligible)
            {
                entry.selected = false;
                entry.highlighted = false;
            }
        }

        void append_candidate(const std::string &path, const std::uint64_t bytes,
                              const bool duplicate)
        {
            RefRow entry;
            entry.path = path;
            entry.bytes = bytes;
            entry.duplicate = duplicate;
            entry.supported = true;
            entry.eligible = eligible(entry);
            entry.selected = select_new_candidates && entry.eligible;
            entry.highlighted = false;
            rows.push_back(entry);
        }

        void apply_scan_batch(const int first, const std::vector<RefRow> &batch)
        {
            if (first < 0 ||
                first + static_cast<int>(batch.size()) > static_cast<int>(rows.size()) ||
                batch.empty())
                return;
            for (std::size_t offset = 0; offset < batch.size(); ++offset)
            {
                auto &entry = rows[static_cast<std::size_t>(first) + offset];
                const bool keep_selected = entry.selected;
                const bool keep_highlighted = entry.highlighted;
                entry.bytes = batch[offset].bytes;
                entry.duplicate = batch[offset].duplicate;
                entry.supported = batch[offset].supported;
                entry.eligible = eligible(entry);
                if (!entry.eligible)
                {
                    entry.selected = false;
                    entry.highlighted = false;
                }
                else
                {
                    entry.selected = keep_selected;
                    entry.highlighted = keep_highlighted;
                }
            }
        }

        void apply(const Op &op)
        {
            switch (op.kind)
            {
            case OpKind::HighlightExclusive:
                highlight_exclusive(op.first);
                break;
            case OpKind::HighlightToggle:
                highlight_toggle(op.first);
                break;
            case OpKind::HighlightRange:
                highlight_range(op.first, op.last, op.additive);
                break;
            case OpKind::HighlightAll:
                highlight_all();
                break;
            case OpKind::ApplyCheck:
                apply_check(op.first);
                break;
            case OpKind::ToggleSelected:
                toggle_selected(op.first);
                break;
            case OpKind::SetAllSelected:
                set_all_selected(op.selected);
                break;
            case OpKind::SelectRange:
                select_range(op.first, op.last, op.additive);
                break;
            case OpKind::UpdateCandidate:
                update_candidate(op.first, op.bytes, op.make_duplicate, op.make_unsupported);
                break;
            case OpKind::AppendCandidate:
                append_candidate(op.path, op.bytes, op.make_duplicate);
                break;
            case OpKind::ApplyScanBatch:
            {
                std::vector<RefRow> batch(1);
                batch[0].bytes = op.bytes;
                batch[0].duplicate = op.make_duplicate;
                batch[0].supported = !op.make_unsupported;
                apply_scan_batch(op.first, batch);
                break;
            }
            case OpKind::Rebuild:
                rebuild(op.count, op.duplicate_row);
                break;
            case OpKind::InvalidRowNoOp:
                highlight_exclusive(op.first);
                toggle_selected(op.first);
                apply_check(op.first);
                break;
            }
        }
    };

    auto apply_to_model = [](ImportCandidateListModel &model, const Op &op)
    {
        switch (op.kind)
        {
        case OpKind::HighlightExclusive:
            model.highlightExclusive(op.first);
            break;
        case OpKind::HighlightToggle:
            model.highlightToggle(op.first);
            break;
        case OpKind::HighlightRange:
            model.highlightRange(op.first, op.last, op.additive);
            break;
        case OpKind::HighlightAll:
            model.highlightAll();
            break;
        case OpKind::ApplyCheck:
            model.applyCheck(op.first);
            break;
        case OpKind::ToggleSelected:
            model.toggleSelected(op.first);
            break;
        case OpKind::SetAllSelected:
            model.setAllSelected(op.selected);
            break;
        case OpKind::SelectRange:
            model.selectRange(op.first, op.last, op.additive);
            break;
        case OpKind::UpdateCandidate:
        {
            ImportCandidate candidate;
            candidate.source_path = model.sourcePath(op.first).toStdString();
            if (candidate.source_path.empty())
                break;
            candidate.display_name = candidate.source_path;
            candidate.size_bytes = op.bytes;
            candidate.duplicate = op.make_duplicate;
            candidate.supported = !op.make_unsupported;
            if (op.make_duplicate)
                candidate.duplicate_reason = "catalog_content";
            model.updateCandidate(op.first, candidate);
            break;
        }
        case OpKind::AppendCandidate:
        {
            ImportCandidate candidate;
            candidate.source_path = op.path;
            candidate.display_name = op.path;
            candidate.size_bytes = op.bytes;
            candidate.duplicate = op.make_duplicate;
            if (op.make_duplicate)
                candidate.duplicate_reason = "catalog_content";
            model.appendCandidate(std::move(candidate));
            break;
        }
        case OpKind::ApplyScanBatch:
        {
            ImportCandidate candidate;
            candidate.source_path = model.sourcePath(op.first).toStdString();
            candidate.display_name = candidate.source_path;
            candidate.size_bytes = op.bytes;
            candidate.duplicate = op.make_duplicate;
            candidate.supported = !op.make_unsupported;
            if (op.make_duplicate)
                candidate.duplicate_reason = "catalog_content";
            model.applyScanBatch(op.first, {candidate});
            break;
        }
        case OpKind::Rebuild:
        {
            std::vector<ImportCandidate> candidates;
            candidates.reserve(static_cast<std::size_t>(op.count));
            for (int row = 0; row < op.count; ++row)
            {
                ImportCandidate candidate;
                candidate.source_path = "/oracle-" + std::to_string(row) + ".png";
                candidate.display_name = candidate.source_path;
                candidate.size_bytes = static_cast<std::uint64_t>((row + 1) * 10);
                if (row == op.duplicate_row)
                {
                    candidate.duplicate = true;
                    candidate.duplicate_reason = "catalog_content";
                }
                candidates.push_back(candidate);
            }
            model.setCandidates(std::move(candidates), true);
            break;
        }
        case OpKind::InvalidRowNoOp:
            model.highlightExclusive(op.first);
            model.toggleSelected(op.first);
            model.applyCheck(op.first);
            break;
        }
    };

    auto compare =
        [](const ImportCandidateListModel &model, const Reference &ref, const std::string &context)
    {
        ASSERT_EQ(model.rowCount(), static_cast<int>(ref.rows.size())) << context;
        int count = 0;
        qulonglong bytes = 0;
        for (const auto &row : ref.rows)
            if (row.selected)
            {
                ++count;
                bytes += row.bytes;
            }
        EXPECT_EQ(model.selectedCount(), count) << context;
        EXPECT_EQ(model.selectedBytes(), bytes) << context;
        for (int row = 0; row < model.rowCount(); ++row)
        {
            const auto &expected = ref.rows[static_cast<std::size_t>(row)];
            EXPECT_EQ(
                model.data(model.index(row, 0), ImportCandidateListModel::SelectedRole).toBool(),
                expected.selected)
                << context << " selected row " << row;
            EXPECT_EQ(model.highlighted(row), expected.highlighted)
                << context << " highlighted row " << row;
            EXPECT_EQ(
                model.data(model.index(row, 0), ImportCandidateListModel::EligibleRole).toBool(),
                expected.eligible)
                << context << " eligible row " << row;
        }
    };

    auto run_ops = [&](const unsigned seed, const std::vector<Op> &fixed_ops)
    {
        ImportCandidateListModel model;
        Reference ref;
        std::vector<Op> replay = fixed_ops;
        for (const auto &op : replay)
        {
            ref.apply(op);
            apply_to_model(model, op);
            compare(model, ref, "fixed seed=" + std::to_string(seed));
            if (::testing::Test::HasFatalFailure())
                return;
        }

        std::mt19937 rng(seed);
        constexpr int kRows = 24;
        Op rebuild_op;
        rebuild_op.kind = OpKind::Rebuild;
        rebuild_op.count = kRows;
        rebuild_op.duplicate_row = 7;
        replay.push_back(rebuild_op);
        ref.apply(rebuild_op);
        apply_to_model(model, rebuild_op);
        compare(model, ref, "rebuild seed=" + std::to_string(seed));

        for (int step = 0; step < 80; ++step)
        {
            Op op;
            const int kind = static_cast<int>(rng() % 12U);
            op.first = static_cast<int>(rng() % 30U) - 3;
            op.last = static_cast<int>(rng() % 30U) - 3;
            op.additive = (rng() % 2U) == 0U;
            op.selected = (rng() % 2U) == 0U;
            op.bytes = static_cast<std::uint64_t>((rng() % 50U) + 1U) * 10U;
            op.make_duplicate = (rng() % 5U) == 0U;
            op.make_unsupported = (rng() % 7U) == 0U;
            switch (kind)
            {
            case 0:
                op.kind = OpKind::HighlightExclusive;
                break;
            case 1:
                op.kind = OpKind::HighlightToggle;
                break;
            case 2:
                op.kind = OpKind::HighlightRange;
                break;
            case 3:
                op.kind = OpKind::ApplyCheck;
                break;
            case 4:
                op.kind = OpKind::SetAllSelected;
                break;
            case 5:
                op.kind = OpKind::ToggleSelected;
                break;
            case 6:
                op.kind = OpKind::HighlightAll;
                break;
            case 7:
                op.kind = OpKind::SelectRange;
                break;
            case 8:
                op.kind = OpKind::UpdateCandidate;
                if (op.first < 0 || op.first >= static_cast<int>(ref.rows.size()))
                {
                    if (ref.rows.empty())
                        continue;
                    op.first = static_cast<int>(rng() % static_cast<unsigned>(ref.rows.size()));
                }
                break;
            case 9:
                op.kind = OpKind::AppendCandidate;
                op.path = "/oracle-append-" + std::to_string(step) + ".png";
                break;
            case 10:
                op.kind = OpKind::ApplyScanBatch;
                if (ref.rows.empty())
                    continue;
                op.first = static_cast<int>(rng() % static_cast<unsigned>(ref.rows.size()));
                break;
            default:
                op.kind = OpKind::InvalidRowNoOp;
                op.first = -1;
                break;
            }
            replay.push_back(op);
            ref.apply(op);
            apply_to_model(model, op);
            compare(model, ref, "seed=" + std::to_string(seed) + " step=" + std::to_string(step));
            if (::testing::Test::HasFatalFailure())
            {
                std::string dump;
                for (const auto &recorded : replay)
                    dump += std::to_string(static_cast<int>(recorded.kind)) + ":" +
                            std::to_string(recorded.first) + ":" + std::to_string(recorded.last) +
                            ":" + (recorded.additive ? "1" : "0") + ";";
                ADD_FAILURE() << "oracle replay seed=" << seed << " ops=" << dump;
                return;
            }
        }
    };

    std::vector<Op> boundaries;
    {
        Op op;
        op.kind = OpKind::Rebuild;
        op.count = 12;
        op.duplicate_row = 3;
        boundaries.push_back(op);
    }
    {
        Op op;
        op.kind = OpKind::HighlightExclusive;
        op.first = 1;
        boundaries.push_back(op);
    }
    {
        Op op;
        op.kind = OpKind::HighlightExclusive;
        op.first = 3;
        boundaries.push_back(op);
    }
    {
        Op op;
        op.kind = OpKind::HighlightRange;
        op.first = 2;
        op.last = 5;
        op.additive = false;
        boundaries.push_back(op);
    }
    {
        Op op;
        op.kind = OpKind::HighlightRange;
        op.first = 5;
        op.last = 1;
        op.additive = true;
        boundaries.push_back(op);
    }
    {
        Op op;
        op.kind = OpKind::HighlightRange;
        op.first = 4;
        op.last = 4;
        op.additive = false;
        boundaries.push_back(op);
    }
    {
        Op op;
        op.kind = OpKind::ApplyCheck;
        op.first = 2;
        boundaries.push_back(op);
    }
    {
        Op op;
        op.kind = OpKind::SetAllSelected;
        op.selected = false;
        boundaries.push_back(op);
    }
    {
        Op op;
        op.kind = OpKind::AppendCandidate;
        op.path = "/oracle-intent-keep.png";
        op.bytes = 42;
        boundaries.push_back(op);
    }
    {
        Op op;
        op.kind = OpKind::SetAllSelected;
        op.selected = true;
        boundaries.push_back(op);
    }
    {
        Op op;
        op.kind = OpKind::Rebuild;
        op.count = 4;
        op.duplicate_row = -1;
        boundaries.push_back(op);
    }
    {
        Op op;
        op.kind = OpKind::SelectRange;
        op.first = 0;
        op.last = 0;
        op.additive = false;
        boundaries.push_back(op);
    }
    {
        Op op;
        op.kind = OpKind::SelectRange;
        op.first = 1;
        op.last = 1;
        op.additive = false;
        boundaries.push_back(op);
    }
    {
        Op op;
        op.kind = OpKind::UpdateCandidate;
        op.first = 1;
        op.bytes = 999;
        boundaries.push_back(op);
    }
    {
        Op op;
        op.kind = OpKind::UpdateCandidate;
        op.first = 2;
        op.bytes = 30;
        op.make_duplicate = true;
        boundaries.push_back(op);
    }
    {
        Op op;
        op.kind = OpKind::ApplyScanBatch;
        op.first = 0;
        op.bytes = 11;
        op.make_unsupported = true;
        boundaries.push_back(op);
    }
    {
        Op op;
        op.kind = OpKind::InvalidRowNoOp;
        op.first = -5;
        boundaries.push_back(op);
    }
    {
        Op op;
        op.kind = OpKind::ToggleSelected;
        op.first = 0;
        boundaries.push_back(op);
    }

    run_ops(0xC0FFEEU, boundaries);
    run_ops(0xBADC0DEU, {});
    run_ops(0x1234567U, {});
}

TEST(ImportCandidateListModel, EqualSizeMembershipSwapNotifiesSelection)
{
    ensure_qt_core();
    ImportCandidateListModel model;
    model.setCandidates({make_row("/a.png", 10), make_row("/b.png", 10)});
    model.selectRange(0, 0, false);
    ASSERT_EQ(model.selectedCount(), 1);
    ASSERT_EQ(model.selectedBytes(), 10ULL);
    const auto before_revision = model.selectionRevision();
    int notifications = 0;
    QObject::connect(&model, &ImportCandidateListModel::selectionChanged, &model,
                     [&] { ++notifications; });
    model.selectRange(1, 1, false); // same aggregates, different membership
    EXPECT_EQ(model.selectedCount(), 1);
    EXPECT_EQ(model.selectedBytes(), 10ULL);
    EXPECT_GT(model.selectionRevision(), before_revision);
    EXPECT_GE(notifications, 1);
    EXPECT_FALSE(model.data(model.index(0, 0), ImportCandidateListModel::SelectedRole).toBool());
    EXPECT_TRUE(model.data(model.index(1, 0), ImportCandidateListModel::SelectedRole).toBool());

    notifications = 0;
    const auto noop_revision = model.selectionRevision();
    model.selectRange(1, 1, false); // true no-op
    EXPECT_EQ(model.selectionRevision(), noop_revision);
    EXPECT_EQ(notifications, 0);
}

TEST(ImportCandidateListModel, ExclusiveHighlightNotifiesAtMostTwoRows)
{
    ensure_qt_core();
    constexpr int kCount = 100000;
    std::vector<ImportCandidate> candidates(static_cast<std::size_t>(kCount));
    for (int row = 0; row < kCount; ++row)
    {
        candidates[static_cast<std::size_t>(row)].source_path = std::to_string(row);
        candidates[static_cast<std::size_t>(row)].size_bytes = 1;
    }
    ImportCandidateListModel model;
    model.setCandidates(std::move(candidates));
    model.highlightExclusive(10);

    int changed_rows = 0;
    bool saw_reset = false;
    QObject::connect(&model, &QAbstractItemModel::modelReset, &model, [&] { saw_reset = true; });
    QObject::connect(&model, &QAbstractItemModel::dataChanged, &model,
                     [&](const QModelIndex &top, const QModelIndex &bottom, const QList<int> &)
                     { changed_rows += bottom.row() - top.row() + 1; });
    model.highlightExclusive(90000);
    EXPECT_FALSE(saw_reset);
    EXPECT_LE(changed_rows, 2);
    EXPECT_TRUE(model.highlighted(90000));
    EXPECT_FALSE(model.highlighted(10));
}

TEST(ImportCandidateListModel, SingleSpaceCheckIsRowLocal)
{
    ensure_qt_core();
    constexpr int kCount = 100000;
    std::vector<ImportCandidate> candidates(static_cast<std::size_t>(kCount));
    for (int row = 0; row < kCount; ++row)
    {
        candidates[static_cast<std::size_t>(row)].source_path = std::to_string(row);
        candidates[static_cast<std::size_t>(row)].size_bytes = 3;
    }
    ImportCandidateListModel model;
    model.setCandidates(std::move(candidates));
    model.highlightExclusive(50);
    int changed_rows = 0;
    QObject::connect(&model, &QAbstractItemModel::dataChanged, &model,
                     [&](const QModelIndex &top, const QModelIndex &bottom, const QList<int> &roles)
                     {
                         if (roles.contains(ImportCandidateListModel::SelectedRole) ||
                             roles.isEmpty())
                             changed_rows += bottom.row() - top.row() + 1;
                     });
    const auto before_bytes = model.selectedBytes();
    model.applyCheck(50);
    EXPECT_EQ(changed_rows, 1);
    EXPECT_NE(model.selectedBytes(), before_bytes);
}

TEST(ImportCandidateListModel, ThumbnailByteAndCountBudgets)
{
    ensure_qt_core();
    ImportCandidateListModel model;
    std::vector<ImportCandidate> candidates(400);
    for (int row = 0; row < 400; ++row)
        candidates[static_cast<std::size_t>(row)].source_path = std::to_string(row);
    model.setCandidates(std::move(candidates));
    int present = 0;
    for (int row = 0; row < 400; ++row)
    {
        QImage image(512, 512, QImage::Format_RGB888);
        image.fill(Qt::green);
        model.setThumbnail(row, image);
    }
    for (int row = 0; row < 400; ++row)
        if (!model.thumbnail(row).isNull())
            ++present;
    EXPECT_LE(present, 256);
}

} // namespace ravo
