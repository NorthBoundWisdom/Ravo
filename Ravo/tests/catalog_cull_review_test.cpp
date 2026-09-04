#include <filesystem>
#include <string>
#include <vector>

#include <QColor>
#include <QColorSpace>
#include <QImage>
#include <QString>
#include <gtest/gtest.h>

#include "ravo/foundation/cancellation.h"
#include "ravo/services/catalog_service.h"
#include "ravo/services/cull_assistance.h"

#include "catalog_test_support.h"
#include "catalog_repository_test_control.h"

namespace ravo
{
namespace
{

[[nodiscard]] bool write_jpeg(const std::filesystem::path &path, const QColor &color)
{
    QImage image(16, 12, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(color);
    return image.save(QString::fromStdString(path.string()), "JPEG", 90);
}

} // namespace

TEST_F(CatalogServiceTest, CullReviewPickRejectRatingAutoAdvanceAndUndo)
{
    ASSERT_TRUE(open_service(true));
    std::vector<std::string> ids;
    for (int index = 0; index < 3; ++index)
    {
        const auto path = root / ("cull-" + std::to_string(index) + ".jpg");
        ASSERT_TRUE(write_jpeg(path, QColor(10 + index * 40, 20, 30)));
        auto imported = service->import_one(path.string(), CancellationToken{});
        ASSERT_TRUE(imported) << imported.error().message;
        ids.push_back(imported.value().asset->id);
    }

    CullReviewRequest pick;
    pick.asset_id = ids[0];
    pick.flag_action = CullReviewFlagAction::kPick;
    pick.rating = 5;
    pick.color_label = ColorLabel::kGreen;
    pick.auto_advance = true;
    pick.selection_asset_ids = ids;
    auto picked = service->apply_cull_review(pick);
    ASSERT_TRUE(picked) << picked.error().message;
    EXPECT_TRUE(picked.value().review.picked);
    EXPECT_FALSE(picked.value().review.rejected);
    EXPECT_EQ(picked.value().review.rating, 5);
    EXPECT_EQ(picked.value().review.color_label, ColorLabel::kGreen);
    ASSERT_TRUE(picked.value().next_asset_id);
    EXPECT_EQ(*picked.value().next_asset_id, ids[1]);
    EXPECT_FALSE(picked.value().previous_review.picked);

    CullReviewRequest reject;
    reject.asset_id = ids[1];
    reject.flag_action = CullReviewFlagAction::kReject;
    reject.auto_advance = true;
    reject.selection_asset_ids = ids;
    auto rejected = service->apply_cull_review(reject);
    ASSERT_TRUE(rejected) << rejected.error().message;
    EXPECT_TRUE(rejected.value().review.rejected);
    EXPECT_FALSE(rejected.value().review.picked);
    ASSERT_TRUE(rejected.value().next_asset_id);
    EXPECT_EQ(*rejected.value().next_asset_id, ids[2]);

    // Pick clears reject on the same asset.
    CullReviewRequest flip;
    flip.asset_id = ids[1];
    flip.flag_action = CullReviewFlagAction::kPick;
    auto flipped = service->apply_cull_review(flip);
    ASSERT_TRUE(flipped) << flipped.error().message;
    EXPECT_TRUE(flipped.value().review.picked);
    EXPECT_FALSE(flipped.value().review.rejected);

    // Undo via previous_review replay.
    CullReviewRequest undo;
    undo.asset_id = ids[0];
    undo.flag_action = CullReviewFlagAction::kUnflag;
    undo.rating = picked.value().previous_review.rating;
    undo.color_label = picked.value().previous_review.color_label;
    auto undone = service->apply_cull_review(undo);
    ASSERT_TRUE(undone) << undone.error().message;
    EXPECT_FALSE(undone.value().review.picked);
    EXPECT_FALSE(undone.value().review.rejected);
    EXPECT_EQ(undone.value().review.rating, 0);
    EXPECT_EQ(undone.value().review.color_label, ColorLabel::kNone);

    auto listed = service->list_assets();
    ASSERT_TRUE(listed);
    ASSERT_EQ(listed.value().size(), 3U);
}

TEST_F(CatalogServiceTest, CullReviewRequiresMutationAndHonorsRevision)
{
    ASSERT_TRUE(open_service(true));
    const auto path = root / "solo.jpg";
    ASSERT_TRUE(write_jpeg(path, QColor(1, 2, 3)));
    auto imported = service->import_one(path.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;

    CullReviewRequest empty;
    empty.asset_id = imported.value().asset->id;
    auto missing = service->apply_cull_review(empty);
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().context.at("reason"), "cull_review_mutation_required");

    auto snap = service->snapshot();
    ASSERT_TRUE(snap);
    CullReviewRequest stale;
    stale.asset_id = imported.value().asset->id;
    stale.flag_action = CullReviewFlagAction::kPick;
    stale.expected_catalog_revision = snap.value().revision - 1;
    auto conflict = service->apply_cull_review(stale);
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().context.at("reason"), "stale_catalog_revision");

    auto picked = service->set_picked(imported.value().asset->id, true);
    ASSERT_TRUE(picked) << picked.error().message;
    EXPECT_TRUE(picked.value().review.picked);
    auto rejected = service->set_rejected(imported.value().asset->id, true);
    ASSERT_TRUE(rejected) << rejected.error().message;
    EXPECT_TRUE(rejected.value().review.rejected);
    EXPECT_FALSE(rejected.value().review.picked);
}

TEST_F(CatalogServiceTest, Cor01CullReviewAtomicCommitAndCancelBeforePublish)
{
    ASSERT_TRUE(open_service(true));
    const auto path = root / "cor01-cull.jpg";
    ASSERT_TRUE(write_jpeg(path, QColor(9, 8, 7)));
    auto imported = service->import_one(path.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto asset_id = imported.value().asset->id;
    auto before = service->snapshot();
    ASSERT_TRUE(before);

    CancellationSource cancel;
    ASSERT_TRUE(cancel.cancel("test"));
    CullReviewRequest cancelled;
    cancelled.asset_id = asset_id;
    cancelled.flag_action = CullReviewFlagAction::kPick;
    cancelled.cancellation = cancel.token();
    auto denied = service->apply_cull_review(cancelled);
    ASSERT_FALSE(denied);
    EXPECT_EQ(denied.error().context.at("catalog_committed"), "false");
    auto listed = service->list_assets();
    ASSERT_TRUE(listed);
    ASSERT_EQ(listed.value().size(), 1U);
    EXPECT_FALSE(listed.value().front().review.picked);
    auto after_cancel = service->snapshot();
    ASSERT_TRUE(after_cancel);
    EXPECT_EQ(after_cancel.value().revision, before.value().revision);

    ASSERT_NE(sqlite_repository, nullptr);
    testing::SqliteCatalogTestControl::inject_review(*sqlite_repository,
                                                     testing::SqliteReviewFailure::kRevisionBump);
    CullReviewRequest fail_bump;
    fail_bump.asset_id = asset_id;
    fail_bump.flag_action = CullReviewFlagAction::kPick;
    auto failed = service->apply_cull_review(fail_bump);
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().context.at("catalog_committed"), "false");
    EXPECT_EQ(failed.error().context.at("reason"), "injected_review_revision_bump");
    listed = service->list_assets();
    ASSERT_TRUE(listed);
    EXPECT_FALSE(listed.value().front().review.picked);
    auto after_fail = service->snapshot();
    ASSERT_TRUE(after_fail);
    EXPECT_EQ(after_fail.value().revision, before.value().revision);

    CullReviewRequest ok;
    ok.asset_id = asset_id;
    ok.flag_action = CullReviewFlagAction::kPick;
    ok.rating = 4;
    auto picked = service->apply_cull_review(ok);
    ASSERT_TRUE(picked) << picked.error().message;
    EXPECT_TRUE(picked.value().catalog_mutated);
    EXPECT_TRUE(picked.value().review.picked);
    EXPECT_GT(picked.value().revision, before.value().revision);
}

TEST_F(CatalogServiceTest, Cor01SetPickedUsesTransactionalCommit)
{
    ASSERT_TRUE(open_service(true));
    const auto path = root / "cor01-picked.jpg";
    ASSERT_TRUE(write_jpeg(path, QColor(3, 4, 5)));
    auto imported = service->import_one(path.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_NE(sqlite_repository, nullptr);
    testing::SqliteCatalogTestControl::inject_review(*sqlite_repository,
                                                     testing::SqliteReviewFailure::kCommit);
    auto failed = service->set_picked(imported.value().asset->id, true);
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().context.at("reason"), "injected_review_commit");
    auto listed = service->list_assets();
    ASSERT_TRUE(listed);
    ASSERT_EQ(listed.value().size(), 1U);
    EXPECT_FALSE(listed.value().front().review.picked);
}

TEST_F(CatalogServiceTest, Cull01ReviewUnderPagingCollapsedStacksFiltersAndRestart)
{
    ASSERT_TRUE(open_service(true));
    std::vector<std::string> ids;
    for (int index = 0; index < 5; ++index)
    {
        const auto path = root / ("page-" + std::to_string(index) + ".jpg");
        ASSERT_TRUE(write_jpeg(path, QColor(20 + index * 30, 40, 50)));
        auto imported = service->import_one(path.string(), CancellationToken{});
        ASSERT_TRUE(imported) << imported.error().message;
        ids.push_back(imported.value().asset->id);
    }

    // Collapse three members into one stack pick; library list hides the rest.
    auto stacked = service->stack_assets({ids[1], ids[2], ids[3]}, ids[1], {});
    ASSERT_TRUE(stacked) << stacked.error().message;

    LibraryPageRequest page;
    page.limit = 2;
    page.collapse_stacks = true;
    auto first_page = service->list_assets_page(page);
    ASSERT_TRUE(first_page) << first_page.error().message;
    ASSERT_EQ(first_page.value().assets.size(), 2U);
    EXPECT_TRUE(first_page.value().has_more);

    // Auto-advance uses collapsed library order when selection ids omitted.
    CullReviewRequest pick;
    pick.asset_id = first_page.value().assets.front().id;
    pick.flag_action = CullReviewFlagAction::kPick;
    pick.rating = 3;
    pick.color_label = ColorLabel::kBlue;
    pick.auto_advance = true;
    auto advanced = service->apply_cull_review(pick);
    ASSERT_TRUE(advanced) << advanced.error().message;
    ASSERT_TRUE(advanced.value().next_asset_id);
    EXPECT_EQ(*advanced.value().next_asset_id, first_page.value().assets[1].id);

    // Page-scoped selection: next stays inside the loaded page ids.
    CullReviewRequest page_advance;
    page_advance.asset_id = first_page.value().assets[0].id;
    page_advance.flag_action = CullReviewFlagAction::kReject;
    page_advance.auto_advance = true;
    page_advance.selection_asset_ids = {first_page.value().assets[0].id,
                                        first_page.value().assets[1].id};
    auto page_next = service->apply_cull_review(page_advance);
    ASSERT_TRUE(page_next) << page_next.error().message;
    ASSERT_TRUE(page_next.value().next_asset_id);
    EXPECT_EQ(*page_next.value().next_asset_id, first_page.value().assets[1].id);

    // Filters: picked / rejected / unreviewed.
    LibraryQuery picked_query;
    picked_query.cull_flag_filter = CullFlagFilter::kPicked;
    auto picked_rows = service->list_assets(picked_query, true);
    ASSERT_TRUE(picked_rows);
    // Reject flipped the first page asset off pick; may still have picks from earlier.
    LibraryQuery rejected_query;
    rejected_query.cull_flag_filter = CullFlagFilter::kRejected;
    auto rejected_rows = service->list_assets(rejected_query, true);
    ASSERT_TRUE(rejected_rows);
    ASSERT_FALSE(rejected_rows.value().empty());
    EXPECT_TRUE(rejected_rows.value().front().review.rejected);

    LibraryQuery unreviewed_query;
    unreviewed_query.cull_flag_filter = CullFlagFilter::kUnreviewed;
    auto unreviewed_rows = service->list_assets(unreviewed_query, true);
    ASSERT_TRUE(unreviewed_rows);

    // Survey compare still resolves for a non-pick stacked member while list is collapsed.
    BurstCompareRequest compare;
    compare.asset_id = ids[2];
    compare.step = BurstCompareStep::kCurrent;
    auto pair = service->resolve_burst_compare_pair(compare);
    ASSERT_TRUE(pair) << pair.error().message;
    EXPECT_EQ(pair.value().member_ids.size(), 3U);

    // Restart: reopen catalog; review chips and previous_review undo remain deterministic.
    const auto previous = page_next.value().previous_review;
    const auto mutated_id = page_advance.asset_id;
    service.reset();
    sqlite_repository = nullptr;
    ASSERT_TRUE(open_service(false));
    auto listed = service->list_assets();
    ASSERT_TRUE(listed);
    bool found_rejected = false;
    for (const auto &asset : listed.value())
    {
        if (asset.id == mutated_id)
        {
            found_rejected = asset.review.rejected;
            break;
        }
    }
    EXPECT_TRUE(found_rejected);

    CullReviewRequest undo;
    undo.asset_id = mutated_id;
    undo.flag_action = previous.picked   ? CullReviewFlagAction::kPick :
                       previous.rejected ? CullReviewFlagAction::kReject :
                                           CullReviewFlagAction::kUnflag;
    undo.rating = previous.rating;
    undo.color_label = previous.color_label;
    auto undone = service->apply_cull_review(undo);
    ASSERT_TRUE(undone) << undone.error().message;
    EXPECT_EQ(undone.value().review.picked, previous.picked);
    EXPECT_EQ(undone.value().review.rejected, previous.rejected);
    EXPECT_EQ(undone.value().review.rating, previous.rating);
    EXPECT_EQ(undone.value().review.color_label, previous.color_label);

    // Missing asset fails closed.
    CullReviewRequest missing;
    missing.asset_id = "ast_missing_cull";
    missing.flag_action = CullReviewFlagAction::kPick;
    auto absent = service->apply_cull_review(missing);
    ASSERT_FALSE(absent);
}

TEST_F(CatalogServiceTest, Cull01UnflagColourAndCommittedMutationNeverReportedFailed)
{
    ASSERT_TRUE(open_service(true));
    const auto path = root / "flag-colour.jpg";
    ASSERT_TRUE(write_jpeg(path, QColor(8, 9, 10)));
    auto imported = service->import_one(path.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;

    CullReviewRequest pick;
    pick.asset_id = imported.value().asset->id;
    pick.flag_action = CullReviewFlagAction::kPick;
    pick.color_label = ColorLabel::kRed;
    pick.rating = 2;
    auto picked = service->apply_cull_review(pick);
    ASSERT_TRUE(picked) << picked.error().message;
    EXPECT_TRUE(picked.value().catalog_mutated);
    EXPECT_TRUE(picked.value().review.picked);
    EXPECT_EQ(picked.value().review.color_label, ColorLabel::kRed);

    CullReviewRequest unflag;
    unflag.asset_id = imported.value().asset->id;
    unflag.flag_action = CullReviewFlagAction::kUnflag;
    auto cleared = service->apply_cull_review(unflag);
    ASSERT_TRUE(cleared) << cleared.error().message;
    EXPECT_FALSE(cleared.value().review.picked);
    EXPECT_FALSE(cleared.value().review.rejected);
    EXPECT_TRUE(cleared.value().catalog_mutated);
}
} // namespace ravo
