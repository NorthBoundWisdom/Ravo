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

} // namespace ravo
