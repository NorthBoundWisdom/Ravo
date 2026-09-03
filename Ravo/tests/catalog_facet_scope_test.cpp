#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/domain/types.h"

#include "catalog_test_support.h"

namespace ravo
{
namespace
{

// Seeds six assets: four carry capture metadata (two RavoCam, two OtherCam) and
// the first three share one capture day so a scoped count differs from the
// catalog-wide count in an observable way.
void seed_scoped_facet_assets(SqliteCatalogRepository &repository)
{
    for (int index = 0; index < 6; ++index)
    {
        AssetRecord asset;
        asset.id = "ast_scope_" + std::to_string(index);
        asset.normalized_uri = "file:///library/scope/photo-" + std::to_string(index) + ".jpg";
        asset.media_type = std::string(kMediaTypeJpeg);
        asset.size_bytes = 2000U + static_cast<std::uint64_t>(index);
        asset.mtime_unix_ms = 30'000 + index;
        asset.width = 100;
        asset.height = 80;
        asset.created_unix_ms = 40'000 + index;
        asset.review.rating = index < 2 ? 5 : 0;
        if (index < 4)
        {
            asset.capture.camera_make = index < 2 ? "RavoCam" : "OtherCam";
            asset.capture.camera_model = index % 2 == 0 ? "Alpha" : "Beta";
            asset.capture.focal_length_mm = index < 2 ? 35.0 : 85.0;
            CaptureDateTime when;
            when.local_exif = (index < 3 ? "2024:05:01" : "2024:05:02") + std::string(" 12:00:00");
            asset.capture.captured_datetime = when;
            asset.capture.captured_unix_s = 1'700'000'000 + index;
            asset.capture.iso = 100.0 * static_cast<double>(index + 1);
        }
        ASSERT_TRUE(repository.commit_imported_asset(asset));
        WritableMetadata metadata;
        metadata.country = index < 3 ? "China" : "Japan";
        metadata.city = index < 3 ? "Shanghai" : "Tokyo";
        if (index == 0)
            metadata.sublocation = "Bund";
        ASSERT_TRUE(repository.upsert_writable_metadata(asset.id, metadata));
    }
}

} // namespace

TEST_F(CatalogServiceTest, CaptureFacetCountsHonorLibraryQueryScope)
{
    auto repository = SqliteCatalogRepository::create(database_path);
    ASSERT_TRUE(repository) << repository.error().message;
    seed_scoped_facet_assets(*repository.value());
    repository.value().reset();

    auto service_opened = open_service(false);
    ASSERT_TRUE(service_opened) << service_opened.error().message;

    auto unscoped = service->list_capture_facets();
    ASSERT_TRUE(unscoped) << unscoped.error().message;
    EXPECT_FALSE(unscoped.value().scoped);
    ASSERT_EQ(unscoped.value().cameras.size(), 4U);
    ASSERT_EQ(unscoped.value().lenses.size(), 2U);
    ASSERT_EQ(unscoped.value().capture_dates.size(), 2U);

    // An empty query keeps the whole-catalog answer and stays flagged unscoped.
    auto empty_scope = service->list_capture_facets(LibraryQuery{});
    ASSERT_TRUE(empty_scope) << empty_scope.error().message;
    EXPECT_FALSE(empty_scope.value().scoped);
    EXPECT_EQ(empty_scope.value().cameras, unscoped.value().cameras);
    EXPECT_EQ(empty_scope.value().lenses, unscoped.value().lenses);
    EXPECT_EQ(empty_scope.value().capture_dates, unscoped.value().capture_dates);

    LibraryQuery scope;
    scope.captured_local_date = "2024:05:01";
    auto scoped = service->list_capture_facets(scope);
    ASSERT_TRUE(scoped) << scoped.error().message;
    EXPECT_TRUE(scoped.value().scoped);
    EXPECT_FALSE(scoped.value().truncated);
    ASSERT_EQ(scoped.value().capture_dates.size(), 1U);
    EXPECT_EQ(scoped.value().capture_dates.front().captured_local_date, "2024:05:01");
    EXPECT_EQ(scoped.value().capture_dates.front().count, 3U);
    ASSERT_EQ(scoped.value().cameras.size(), 3U);
    std::size_t camera_total = 0U;
    for (const auto &entry : scoped.value().cameras)
        camera_total += entry.count;
    EXPECT_EQ(camera_total, 3U);
    ASSERT_EQ(scoped.value().lenses.size(), 2U);

    // Scoped counts must agree with the asset listing for the same query.
    auto listed = service->list_assets(scope);
    ASSERT_TRUE(listed) << listed.error().message;
    EXPECT_EQ(listed.value().size(), camera_total);

    LibraryQuery camera_scope;
    camera_scope.camera_make_equals = "RavoCam";
    camera_scope.camera_model_equals = "Alpha";
    auto camera_scoped = service->list_capture_facets(camera_scope);
    ASSERT_TRUE(camera_scoped) << camera_scoped.error().message;
    EXPECT_TRUE(camera_scoped.value().scoped);
    ASSERT_EQ(camera_scoped.value().cameras.size(), 1U);
    EXPECT_EQ(camera_scoped.value().cameras.front().count, 1U);
    ASSERT_EQ(camera_scoped.value().lenses.size(), 1U);
    EXPECT_EQ(camera_scoped.value().lenses.front().count, 1U);
}

TEST_F(CatalogServiceTest, LocationFacetCountsHonorLibraryQueryScope)
{
    auto repository = SqliteCatalogRepository::create(database_path);
    ASSERT_TRUE(repository) << repository.error().message;
    seed_scoped_facet_assets(*repository.value());
    repository.value().reset();

    auto service_opened = open_service(false);
    ASSERT_TRUE(service_opened) << service_opened.error().message;

    auto unscoped = service->list_location_facets();
    ASSERT_TRUE(unscoped) << unscoped.error().message;
    EXPECT_FALSE(unscoped.value().scoped);
    ASSERT_EQ(unscoped.value().countries.size(), 2U);
    ASSERT_EQ(unscoped.value().cities.size(), 2U);
    ASSERT_EQ(unscoped.value().sublocations.size(), 1U);

    LibraryQuery scope;
    scope.country_equals = "China";
    auto scoped = service->list_location_facets(scope);
    ASSERT_TRUE(scoped) << scoped.error().message;
    EXPECT_TRUE(scoped.value().scoped);
    ASSERT_EQ(scoped.value().countries.size(), 1U);
    EXPECT_EQ(scoped.value().countries.front().key, "China");
    EXPECT_EQ(scoped.value().countries.front().count, 3U);
    ASSERT_EQ(scoped.value().cities.size(), 1U);
    EXPECT_EQ(scoped.value().cities.front().key, "Shanghai");
    EXPECT_EQ(scoped.value().cities.front().count, 3U);
    ASSERT_EQ(scoped.value().sublocations.size(), 1U);
    EXPECT_EQ(scoped.value().sublocations.front().count, 1U);

    LibraryQuery rating_scope;
    rating_scope.rating_mode = RatingFilterMode::kMinimum;
    rating_scope.rating_value = 5;
    auto rating_scoped = service->list_location_facets(rating_scope);
    ASSERT_TRUE(rating_scoped) << rating_scoped.error().message;
    EXPECT_TRUE(rating_scoped.value().scoped);
    ASSERT_EQ(rating_scoped.value().countries.size(), 1U);
    EXPECT_EQ(rating_scoped.value().countries.front().count, 2U);
    ASSERT_EQ(rating_scoped.value().sublocations.size(), 1U);
    EXPECT_EQ(rating_scoped.value().sublocations.front().label, "Bund");

    LibraryQuery empty_result_scope;
    empty_result_scope.city_equals = "Kyoto";
    auto empty_scoped = service->list_location_facets(empty_result_scope);
    ASSERT_TRUE(empty_scoped) << empty_scoped.error().message;
    EXPECT_TRUE(empty_scoped.value().scoped);
    EXPECT_TRUE(empty_scoped.value().countries.empty());
    EXPECT_TRUE(empty_scoped.value().cities.empty());
}

TEST_F(CatalogServiceTest, ScopedFacetCountsRejectInvalidQueriesLikeListing)
{
    auto service_opened = open_service(true);
    ASSERT_TRUE(service_opened) << service_opened.error().message;

    LibraryQuery half_camera;
    half_camera.camera_make_equals = "RavoCam";
    auto capture_rejected = service->list_capture_facets(half_camera);
    ASSERT_FALSE(capture_rejected);
    EXPECT_EQ(capture_rejected.error().context.at("reason"), "invalid_library_camera_facet");
    auto listing_rejected = service->list_assets(half_camera);
    ASSERT_FALSE(listing_rejected);
    EXPECT_EQ(listing_rejected.error().context.at("reason"),
              capture_rejected.error().context.at("reason"));

    LibraryQuery bad_date;
    bad_date.captured_local_date = "2024-05-01";
    auto location_rejected = service->list_location_facets(bad_date);
    ASSERT_FALSE(location_rejected);
    EXPECT_EQ(location_rejected.error().context.at("reason"), "invalid_library_capture_date_facet");
}

} // namespace ravo
