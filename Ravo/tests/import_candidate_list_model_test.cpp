#include <gtest/gtest.h>

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

} // namespace ravo
