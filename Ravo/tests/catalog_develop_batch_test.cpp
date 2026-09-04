#include <string>
#include <vector>

#include <QColor>
#include <QString>
#include <QColorSpace>
#include <QImage>
#include <gtest/gtest.h>

#include "ravo/foundation/cancellation.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/mask.h"
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

TEST_F(CatalogServiceTest, ApplyDevelopSelectionOverlaysChosenFieldsAndPreservesOthers)
{
    ASSERT_TRUE(open_service(true));
    const auto first_path = root / "one.jpg";
    const auto second_path = root / "two.jpg";
    ASSERT_TRUE(write_jpeg(first_path, QColor(20, 80, 140)));
    ASSERT_TRUE(write_jpeg(second_path, QColor(140, 80, 20)));
    auto first = service->import_one(first_path.string(), CancellationToken{});
    auto second = service->import_one(second_path.string(), CancellationToken{});
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(second) << second.error().message;
    const auto first_id = first.value().asset->id;
    const auto second_id = second.value().asset->id;

    DevelopParams source;
    source.exposure_ev = 0.75;
    source.saturation = 0.4;
    ASSERT_TRUE(service->save_develop(first_id, source)) << "source save failed";
    DevelopParams destination;
    destination.saturation = -0.3;
    destination.contrast = 0.2;
    ASSERT_TRUE(service->save_develop(second_id, destination));

    auto snapshot = service->snapshot();
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    DevelopApplyRequest request;
    request.source = source;
    request.fields = {"exposure"};
    request.asset_ids = {first_id, second_id};
    request.expected_revision = snapshot.value().revision;
    auto applied = service->apply_develop_selection(request);
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_EQ(applied.value().applied, 2U);
    EXPECT_EQ(applied.value().failed, 0U);
    EXPECT_EQ(applied.value().skipped, 0U);
    EXPECT_EQ(applied.value().items.size(), 2U);
    EXPECT_GT(applied.value().revision, snapshot.value().revision);

    auto first_recipe = service->load_recipe(first_id);
    ASSERT_TRUE(first_recipe) << first_recipe.error().message;
    auto first_params = develop_from_recipe(first_recipe.value());
    ASSERT_TRUE(first_params) << first_params.error().message;
    EXPECT_NEAR(first_params.value().exposure_ev, 0.75, 1e-9);
    EXPECT_NEAR(first_params.value().saturation, 0.4, 1e-9);

    auto second_recipe = service->load_recipe(second_id);
    ASSERT_TRUE(second_recipe) << second_recipe.error().message;
    auto second_params = develop_from_recipe(second_recipe.value());
    ASSERT_TRUE(second_params) << second_params.error().message;
    EXPECT_NEAR(second_params.value().exposure_ev, 0.75, 1e-9);
    EXPECT_NEAR(second_params.value().saturation, -0.3, 1e-9);
    EXPECT_NEAR(second_params.value().contrast, 0.2, 1e-9);
}

TEST_F(CatalogServiceTest, ApplyDevelopSelectionRejectsPreflightWithoutWrites)
{
    ASSERT_TRUE(open_service(true));
    const auto first_path = root / "one.jpg";
    const auto second_path = root / "two.jpg";
    ASSERT_TRUE(write_jpeg(first_path, QColor(20, 80, 140)));
    ASSERT_TRUE(write_jpeg(second_path, QColor(140, 80, 20)));
    auto first = service->import_one(first_path.string(), CancellationToken{});
    auto second = service->import_one(second_path.string(), CancellationToken{});
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(second) << second.error().message;
    const auto first_id = first.value().asset->id;
    const auto second_id = second.value().asset->id;
    DevelopParams source;
    source.exposure_ev = 0.5;
    ASSERT_TRUE(service->save_develop(first_id, source));
    auto before = service->snapshot();
    ASSERT_TRUE(before);

    DevelopApplyRequest request;
    request.source = source;
    request.fields = {"exposure"};
    request.asset_ids = {first_id, second_id};

    auto empty_fields = request;
    empty_fields.fields.clear();
    auto rejected = service->apply_develop_selection(empty_fields);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kValidation);

    auto unknown = request;
    unknown.fields = {"notADevelopField"};
    rejected = service->apply_develop_selection(unknown);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kUnsupported);

    auto duplicate_ids = request;
    duplicate_ids.asset_ids = {first_id, first_id};
    rejected = service->apply_develop_selection(duplicate_ids);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kValidation);

    auto missing = request;
    missing.asset_ids = {first_id, "ast_missing"};
    rejected = service->apply_develop_selection(missing);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kNotFound);

    auto stale = request;
    stale.expected_revision = before.value().revision - 1;
    rejected = service->apply_develop_selection(stale);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kConflict);
    EXPECT_EQ(rejected.error().context.find("reason")->second, "stale_catalog_revision");

    auto after = service->snapshot();
    ASSERT_TRUE(after);
    EXPECT_EQ(after.value().revision, before.value().revision);
    auto second_recipe = service->load_recipe(second_id);
    ASSERT_TRUE(second_recipe);
    auto second_params = develop_from_recipe(second_recipe.value());
    ASSERT_TRUE(second_params);
    EXPECT_NEAR(second_params.value().exposure_ev, 0.0, 1e-9);
}

TEST_F(CatalogServiceTest, ApplyDevelopSelectionCancellationKeepsCompletedPhotos)
{
    ASSERT_TRUE(open_service(true));
    const auto first_path = root / "one.jpg";
    const auto second_path = root / "two.jpg";
    ASSERT_TRUE(write_jpeg(first_path, QColor(20, 80, 140)));
    ASSERT_TRUE(write_jpeg(second_path, QColor(140, 80, 20)));
    auto first = service->import_one(first_path.string(), CancellationToken{});
    auto second = service->import_one(second_path.string(), CancellationToken{});
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(second) << second.error().message;
    const auto first_id = first.value().asset->id;
    const auto second_id = second.value().asset->id;
    DevelopParams source;
    source.exposure_ev = 0.9;
    ASSERT_TRUE(service->save_develop(first_id, source));

    CancellationSource cancellation;
    DevelopApplyRequest request;
    request.source = source;
    request.fields = {"exposure"};
    request.asset_ids = {second_id, first_id};
    request.cancellation = cancellation.token();
    auto applied = service->apply_develop_selection(
        request,
        [&](const std::size_t completed, const std::size_t, const DevelopApplyItemResult *)
        {
            if (completed == 1U)
                static_cast<void>(cancellation.cancel("develop-apply-test"));
        });
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_EQ(applied.value().applied, 1U);
    EXPECT_EQ(applied.value().failed, 0U);
    EXPECT_EQ(applied.value().skipped, 1U);
    ASSERT_EQ(applied.value().items.size(), 2U);
    EXPECT_EQ(applied.value().items[0].status, DevelopApplyItemStatus::kApplied);
    EXPECT_EQ(applied.value().items[0].asset_id, second_id);
    EXPECT_EQ(applied.value().items[1].status, DevelopApplyItemStatus::kSkipped);
    EXPECT_EQ(applied.value().items[1].asset_id, first_id);

    auto second_recipe = service->load_recipe(second_id);
    ASSERT_TRUE(second_recipe);
    auto second_params = develop_from_recipe(second_recipe.value());
    ASSERT_TRUE(second_params);
    EXPECT_NEAR(second_params.value().exposure_ev, 0.9, 1e-9);
}

TEST_F(CatalogServiceTest, ApplyDevelopSelectionReportsPartialItemFailure)
{
    ASSERT_TRUE(open_service(true));
    const auto first_path = root / "one.jpg";
    const auto second_path = root / "two.jpg";
    ASSERT_TRUE(write_jpeg(first_path, QColor(20, 80, 140)));
    ASSERT_TRUE(write_jpeg(second_path, QColor(140, 80, 20)));
    auto first = service->import_one(first_path.string(), CancellationToken{});
    auto second = service->import_one(second_path.string(), CancellationToken{});
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(second) << second.error().message;
    const auto first_id = first.value().asset->id;
    const auto second_id = second.value().asset->id;
    DevelopParams source;
    source.exposure_ev = 0.35;
    ASSERT_TRUE(service->save_develop(first_id, source));

    DevelopApplyRequest request;
    request.source = source;
    request.fields = {"exposure"};
    request.asset_ids = {first_id, second_id};
    auto applied = service->apply_develop_selection(
        request,
        [&](const std::size_t completed, const std::size_t, const DevelopApplyItemResult *)
        {
            if (completed == 1U)
                ASSERT_TRUE(service->close());
        });
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_EQ(applied.value().applied, 1U);
    EXPECT_EQ(applied.value().failed, 1U);
    EXPECT_EQ(applied.value().skipped, 0U);
    ASSERT_EQ(applied.value().items.size(), 2U);
    EXPECT_EQ(applied.value().items[0].status, DevelopApplyItemStatus::kApplied);
    EXPECT_EQ(applied.value().items[1].status, DevelopApplyItemStatus::kFailed);
    ASSERT_TRUE(applied.value().items[1].error);
    EXPECT_EQ(applied.value().items[1].error->code, ErrorCode::kIo);

    ASSERT_TRUE(open_service(false));
    auto first_recipe = service->load_recipe(first_id);
    ASSERT_TRUE(first_recipe) << first_recipe.error().message;
    auto first_params = develop_from_recipe(first_recipe.value());
    ASSERT_TRUE(first_params);
    EXPECT_NEAR(first_params.value().exposure_ev, 0.35, 1e-9);
    auto second_recipe = service->load_recipe(second_id);
    ASSERT_TRUE(second_recipe);
    auto second_params = develop_from_recipe(second_recipe.value());
    ASSERT_TRUE(second_params);
    EXPECT_NEAR(second_params.value().exposure_ev, 0.0, 1e-9);
}

TEST_F(CatalogServiceTest, ApplyDevelopSelectionCarriesMultiInstanceExposure)
{
    ASSERT_TRUE(open_service(true));
    const auto first_path = root / "multi-a.jpg";
    const auto second_path = root / "multi-b.jpg";
    ASSERT_TRUE(write_jpeg(first_path, QColor(20, 80, 140)));
    ASSERT_TRUE(write_jpeg(second_path, QColor(140, 80, 20)));
    auto first = service->import_one(first_path.string(), CancellationToken{});
    auto second = service->import_one(second_path.string(), CancellationToken{});
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(second) << second.error().message;
    const auto first_id = first.value().asset->id;
    const auto second_id = second.value().asset->id;

    DevelopParams source;
    DevelopExposureInstance global;
    global.instance_id = "exposure-global";
    global.name = "Global";
    global.exposure_ev = 0.2;
    DevelopExposureInstance dodge;
    dodge.instance_id = "exposure-dodge";
    dodge.name = "Dodge";
    dodge.exposure_ev = 0.55;
    dodge.mask_id = "batch-mask-ellipse";
    source.exposure_instances = {global, dodge};
    Mask mask{"batch-mask-ellipse", kCanonicalMaskSchemaVersion, MaskKind::kEllipse};
    mask.payload = EllipseMask{0.5, 0.45, 0.2, 0.15, 0.0, 0.05};
    source.masks.push_back(mask);
    source.saturation = 0.15;
    ASSERT_TRUE(service->save_develop(first_id, source)) << "source save failed";

    DevelopParams destination;
    destination.saturation = -0.25;
    destination.contrast = 0.1;
    ASSERT_TRUE(service->save_develop(second_id, destination));

    auto snapshot = service->snapshot();
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    DevelopApplyRequest request;
    request.source = source;
    request.fields = {"exposure"};
    request.asset_ids = {second_id};
    request.expected_revision = snapshot.value().revision;
    auto applied = service->apply_develop_selection(request);
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_EQ(applied.value().applied, 1U);
    EXPECT_EQ(applied.value().failed, 0U);

    auto second_recipe = service->load_recipe(second_id);
    ASSERT_TRUE(second_recipe) << second_recipe.error().message;
    auto second_params = develop_from_recipe(second_recipe.value());
    ASSERT_TRUE(second_params) << second_params.error().message;
    ASSERT_EQ(second_params.value().exposure_instances.size(), 2U);
    EXPECT_EQ(second_params.value().exposure_instances[1].mask_id, "batch-mask-ellipse");
    EXPECT_NEAR(second_params.value().saturation, -0.25, 1e-9);
    EXPECT_NEAR(second_params.value().contrast, 0.1, 1e-9);
    ASSERT_FALSE(second_params.value().masks.empty());
    EXPECT_EQ(second_params.value().masks.front().id, "batch-mask-ellipse");
}

} // namespace ravo
