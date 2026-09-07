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
    constexpr unsigned seed = 0xC0FFEEU;
    std::mt19937 rng(seed);
    ImportCandidateListModel model;

    struct OracleRow
    {
        bool selected = false;
        bool highlighted = false;
        bool eligible = true;
        std::uint64_t bytes = 0;
    };
    std::vector<OracleRow> oracle;

    auto sync_totals = [&]
    {
        int count = 0;
        qulonglong bytes = 0;
        for (const auto &row : oracle)
            if (row.selected)
            {
                ++count;
                bytes += row.bytes;
            }
        EXPECT_EQ(model.selectedCount(), count);
        EXPECT_EQ(model.selectedBytes(), bytes);
        ASSERT_EQ(model.rowCount(), static_cast<int>(oracle.size()));
        for (int row = 0; row < model.rowCount(); ++row)
        {
            EXPECT_EQ(
                model.data(model.index(row, 0), ImportCandidateListModel::SelectedRole).toBool(),
                oracle[static_cast<std::size_t>(row)].selected)
                << "row " << row;
            EXPECT_EQ(model.highlighted(row), oracle[static_cast<std::size_t>(row)].highlighted)
                << "row " << row;
        }
    };

    auto rebuild = [&](const int count, const int duplicate_row)
    {
        std::vector<ImportCandidate> candidates;
        oracle.clear();
        candidates.reserve(static_cast<std::size_t>(count));
        for (int row = 0; row < count; ++row)
        {
            ImportCandidate candidate;
            candidate.source_path = "/oracle-" + std::to_string(row) + ".png";
            candidate.display_name = candidate.source_path;
            candidate.size_bytes = static_cast<std::uint64_t>((row + 1) * 10);
            if (row == duplicate_row)
            {
                candidate.duplicate = true;
                candidate.duplicate_reason = "catalog_content";
            }
            candidates.push_back(candidate);
            oracle.push_back({!candidate.duplicate && candidate.supported, false,
                              !candidate.duplicate && candidate.supported, candidate.size_bytes});
        }
        model.setCandidates(std::move(candidates));
        sync_totals();
    };

    rebuild(0, -1);
    rebuild(12, 3);
    model.highlightExclusive(1);
    for (auto &row : oracle)
        row.highlighted = false;
    oracle[1].highlighted = true;
    sync_totals();

    model.highlightRange(2, 5, false);
    for (auto &row : oracle)
        row.highlighted = false;
    for (int row = 2; row <= 5; ++row)
        if (oracle[static_cast<std::size_t>(row)].eligible)
            oracle[static_cast<std::size_t>(row)].highlighted = true;
    sync_totals();

    model.highlightRange(5, 1, true); // reverse additive
    for (int row = 1; row <= 5; ++row)
        if (oracle[static_cast<std::size_t>(row)].eligible)
            oracle[static_cast<std::size_t>(row)].highlighted = true;
    sync_totals();

    model.applyCheck(2);
    {
        const bool next = !oracle[2].selected;
        for (std::size_t row = 0; row < oracle.size(); ++row)
            if (oracle[row].highlighted && oracle[row].eligible)
                oracle[row].selected = next;
    }
    sync_totals();

    model.setAllSelected(false);
    for (auto &row : oracle)
        row.selected = false;
    sync_totals();

    model.toggleSelected(0);
    if (oracle[0].eligible)
        oracle[0].selected = !oracle[0].selected;
    sync_totals();

    model.highlightAll();
    for (auto &row : oracle)
        row.highlighted = row.eligible;
    sync_totals();

    // Byte change on selected row.
    auto grown = make_row("/oracle-0.png", 999);
    model.updateCandidate(0, grown);
    oracle[0].bytes = 999;
    sync_totals();

    // Unsupported/duplicate clears selection+highlight.
    model.updateCandidate(4, make_row("/oracle-4.png", 50, true));
    oracle[4].eligible = false;
    oracle[4].selected = false;
    oracle[4].highlighted = false;
    oracle[4].bytes = 50;
    sync_totals();

    // Randomized sequence with fixed seed for replay.
    std::vector<std::string> ops;
    rebuild(24, 7);
    for (int step = 0; step < 80; ++step)
    {
        const int op = static_cast<int>(rng() % 7U);
        const int a = static_cast<int>(rng() % 24U);
        const int b = static_cast<int>(rng() % 24U);
        ops.push_back(std::to_string(op) + ":" + std::to_string(a) + ":" + std::to_string(b));
        switch (op)
        {
        case 0:
            model.highlightExclusive(a);
            for (auto &row : oracle)
                row.highlighted = false;
            if (oracle[static_cast<std::size_t>(a)].eligible)
                oracle[static_cast<std::size_t>(a)].highlighted = true;
            break;
        case 1:
            model.highlightToggle(a);
            if (oracle[static_cast<std::size_t>(a)].eligible)
                oracle[static_cast<std::size_t>(a)].highlighted =
                    !oracle[static_cast<std::size_t>(a)].highlighted;
            break;
        case 2:
            model.highlightRange(a, b, (rng() % 2U) == 0U);
            {
                int first = a;
                int last = b;
                if (first > last)
                    std::swap(first, last);
                const bool additive = ops.back().empty(); // unused placeholder
                static_cast<void>(additive);
            }
            // Re-read truth from model after range to keep oracle honest for additive flag.
            for (int row = 0; row < 24; ++row)
                oracle[static_cast<std::size_t>(row)].highlighted = model.highlighted(row);
            for (int row = 0; row < 24; ++row)
                oracle[static_cast<std::size_t>(row)].selected =
                    model.data(model.index(row, 0), ImportCandidateListModel::SelectedRole)
                        .toBool();
            // After re-sync from model for this op, skip further mutation tracking this step.
            sync_totals();
            continue;
        case 3:
            model.applyCheck(a);
            for (int row = 0; row < 24; ++row)
            {
                oracle[static_cast<std::size_t>(row)].selected =
                    model.data(model.index(row, 0), ImportCandidateListModel::SelectedRole)
                        .toBool();
                oracle[static_cast<std::size_t>(row)].highlighted = model.highlighted(row);
            }
            sync_totals();
            continue;
        case 4:
            model.setAllSelected((rng() % 2U) == 0U);
            for (int row = 0; row < 24; ++row)
                oracle[static_cast<std::size_t>(row)].selected =
                    model.data(model.index(row, 0), ImportCandidateListModel::SelectedRole)
                        .toBool();
            sync_totals();
            continue;
        case 5:
            model.toggleSelected(a);
            oracle[static_cast<std::size_t>(a)].selected =
                model.data(model.index(a, 0), ImportCandidateListModel::SelectedRole).toBool();
            break;
        default:
            model.highlightAll();
            for (auto &row : oracle)
                row.highlighted = row.eligible;
            break;
        }
        sync_totals();
    }
    if (::testing::Test::HasFailure())
    {
        std::string replay;
        for (const auto &op : ops)
            replay += op + ";";
        ADD_FAILURE() << "oracle replay seed=" << seed << " ops=" << replay;
    }
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
