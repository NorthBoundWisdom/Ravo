#include <string>
#include <vector>

#include <QColor>
#include <QColorSpace>
#include <QImage>
#include <QString>
#include <gtest/gtest.h>

#include "ravo/foundation/cancellation.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/recipe.h"
#include "ravo/services/ai_proposal.h"
#include "ravo/services/catalog_service.h"

#include "catalog_test_support.h"

namespace ravo
{
namespace
{

[[nodiscard]] bool write_jpeg(const std::filesystem::path &path, const QColor &color)
{
    QImage image(12, 8, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(color);
    return image.save(QString::fromStdString(path.string()), "JPEG", 90);
}

} // namespace

TEST_F(CatalogServiceTest, AiProposalCreateRejectLeavesRecipeUnchanged)
{
    ASSERT_TRUE(open_service(true));
    const auto path = root / "ai01.jpg";
    ASSERT_TRUE(write_jpeg(path, QColor(40, 90, 140)));
    auto imported = service->import_one(path.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto asset_id = imported.value().asset->id;

    auto before = service->load_recipe(asset_id);
    ASSERT_TRUE(before) << before.error().message;
    auto snapshot = service->snapshot();
    ASSERT_TRUE(snapshot) << snapshot.error().message;

    AiProposalCreateRequest request;
    request.asset_id = asset_id;
    request.user_initiated = true;
    request.expected_catalog_revision = snapshot.value().revision;
    auto created = service->create_ai_proposal(request);
    ASSERT_TRUE(created) << created.error().message;
    EXPECT_EQ(created.value().contract_version, kAiProposalContractVersion);
    EXPECT_EQ(created.value().provider.provider_id, kAiStubProviderId);
    EXPECT_EQ(created.value().status, AiProposalStatus::kPending);
    EXPECT_FALSE(created.value().fields.empty());
    EXPECT_FALSE(created.value().alternatives.empty());

    auto rejected = service->reject_ai_proposal(created.value().id);
    ASSERT_TRUE(rejected) << rejected.error().message;
    EXPECT_EQ(rejected.value().status, AiProposalStatus::kRejected);

    auto after = service->load_recipe(asset_id);
    ASSERT_TRUE(after) << after.error().message;
    auto before_json = serialize_recipe(before.value());
    auto after_json = serialize_recipe(after.value());
    ASSERT_TRUE(before_json) << before_json.error().message;
    ASSERT_TRUE(after_json) << after_json.error().message;
    EXPECT_EQ(before_json.value(), after_json.value());
    auto history = service->list_recipe_history(asset_id);
    ASSERT_TRUE(history) << history.error().message;
    EXPECT_TRUE(history.value().empty());
}

TEST_F(CatalogServiceTest, AiProposalApplyCreatesHistoryAndSurvivesReload)
{
    ASSERT_TRUE(open_service(true));
    const auto path = root / "ai01-apply.jpg";
    ASSERT_TRUE(write_jpeg(path, QColor(20, 70, 120)));
    auto imported = service->import_one(path.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto asset_id = imported.value().asset->id;

    AiProposalCreateRequest request;
    request.asset_id = asset_id;
    request.user_initiated = true;
    auto created = service->create_ai_proposal(request);
    ASSERT_TRUE(created) << created.error().message;

    auto applied = service->apply_ai_proposal(created.value().id);
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_EQ(applied.value().proposal.status, AiProposalStatus::kApplied);
    ASSERT_TRUE(applied.value().history_id.has_value());

    auto recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto params = develop_from_recipe(recipe.value());
    ASSERT_TRUE(params) << params.error().message;
    EXPECT_GT(params.value().exposure_ev, 0.0);
    EXPECT_NEAR(params.value().crop_width, 0.96, 1e-9);

    auto history = service->list_recipe_history(asset_id);
    ASSERT_TRUE(history) << history.error().message;
    EXPECT_FALSE(history.value().empty());
}

TEST_F(CatalogServiceTest, AiProposalFailsClosedOnUnknownFieldStaleRevisionMissingInitiation)
{
    ASSERT_TRUE(open_service(true));
    const auto path = root / "ai01-fail.jpg";
    ASSERT_TRUE(write_jpeg(path, QColor(80, 40, 20)));
    auto imported = service->import_one(path.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto asset_id = imported.value().asset->id;

    AiProposalCreateRequest missing_initiation;
    missing_initiation.asset_id = asset_id;
    missing_initiation.user_initiated = false;
    auto denied = service->create_ai_proposal(missing_initiation);
    ASSERT_FALSE(denied);
    EXPECT_EQ(denied.error().context.at("reason"), "ai_proposal_not_user_initiated");

    AiProposalCreateRequest bad_provider;
    bad_provider.asset_id = asset_id;
    bad_provider.user_initiated = true;
    bad_provider.provider_id = "vendor.remote.unpackaged";
    bad_provider.model_id = "weights-v1";
    auto remote = service->create_ai_proposal(bad_provider);
    ASSERT_FALSE(remote);
    EXPECT_EQ(remote.error().context.at("reason"), "ai_provider_not_packaged");

    AiProposalCreateRequest ok;
    ok.asset_id = asset_id;
    ok.user_initiated = true;
    auto created = service->create_ai_proposal(ok);
    ASSERT_TRUE(created) << created.error().message;

    DevelopParams mutate;
    mutate.exposure_ev = 0.5;
    ASSERT_TRUE(service->save_develop(asset_id, mutate));

    auto stale = service->apply_ai_proposal(created.value().id);
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().context.at("reason"), "stale_catalog_revision");

    AiProposalFieldChange unknown;
    unknown.field = "notARealDevelopField";
    unknown.value = 1.0;
    auto invalid = validate_ai_proposal_fields({unknown});
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().context.at("reason"), "unknown_ai_proposal_field");

    AiProposalFieldChange oob;
    oob.field = "exposure";
    oob.value = 1.0e9;
    auto bounds = validate_ai_proposal_fields({oob});
    ASSERT_FALSE(bounds);
}

TEST_F(CatalogServiceTest, AiProposalCancelLeavesCatalogUntouched)
{
    ASSERT_TRUE(open_service(true));
    const auto path = root / "ai01-cancel.jpg";
    ASSERT_TRUE(write_jpeg(path, QColor(10, 10, 10)));
    auto imported = service->import_one(path.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto asset_id = imported.value().asset->id;
    auto before_snap = service->snapshot();
    ASSERT_TRUE(before_snap);

    AiProposalCreateRequest request;
    request.asset_id = asset_id;
    request.user_initiated = true;
    auto created = service->create_ai_proposal(request);
    ASSERT_TRUE(created) << created.error().message;
    auto cancelled = service->cancel_ai_proposal(created.value().id);
    ASSERT_TRUE(cancelled) << cancelled.error().message;
    EXPECT_EQ(cancelled.value().status, AiProposalStatus::kCancelled);

    auto after_snap = service->snapshot();
    ASSERT_TRUE(after_snap);
    EXPECT_EQ(before_snap.value().revision, after_snap.value().revision);
    auto history = service->list_recipe_history(asset_id);
    ASSERT_TRUE(history);
    EXPECT_TRUE(history.value().empty());
}

} // namespace ravo
