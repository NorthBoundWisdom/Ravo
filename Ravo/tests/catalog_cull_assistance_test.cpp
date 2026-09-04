#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include <QColor>
#include <QColorSpace>
#include <QImage>
#include <QString>
#include <gtest/gtest.h>

#include "ravo/adapters/text_file.h"
#include "ravo/domain/uri.h"
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

TEST_F(CatalogServiceTest, CullExactDuplicatesReportsSameBytesAndSameFile)
{
    ASSERT_TRUE(open_service(true));
    const auto first_path = root / "dup-a.jpg";
    const auto second_path = root / "dup-b.jpg";
    const auto unique_path = root / "unique.jpg";
    ASSERT_TRUE(write_jpeg(first_path, QColor(10, 20, 30)));
    {
        std::error_code error;
        std::filesystem::copy_file(first_path, second_path, error);
        ASSERT_FALSE(error) << error.message();
    }
    ASSERT_TRUE(write_jpeg(unique_path, QColor(200, 10, 10)));

    auto a = service->import_one(first_path.string(), CancellationToken{});
    ASSERT_TRUE(a) << a.error().message;
    auto b = service->import_one(second_path.string(), CancellationToken{});
    ASSERT_TRUE(b) << b.error().message;
    auto unique = service->import_one(unique_path.string(), CancellationToken{});
    ASSERT_TRUE(unique) << unique.error().message;

    auto version = service->create_asset_version(a.value().asset->id);
    ASSERT_TRUE(version) << version.error().message;

    auto report = service->find_exact_duplicate_groups({});
    ASSERT_TRUE(report) << report.error().message;
    ASSERT_GE(report.value().groups.size(), 2U);

    bool saw_same_bytes = false;
    bool saw_same_file = false;
    for (const auto &group : report.value().groups)
    {
        if (group.outcome == ExactDuplicateOutcome::kSameBytes)
        {
            saw_same_bytes = true;
            EXPECT_GE(group.members.size(), 2U);
            std::set<std::string> uris;
            for (const auto &member : group.members)
                uris.insert(member.normalized_uri);
            EXPECT_EQ(uris.size(), group.members.size());
        }
        if (group.outcome == ExactDuplicateOutcome::kSameFile)
        {
            saw_same_file = true;
            EXPECT_GE(group.members.size(), 2U);
            const auto uri = group.members.front().normalized_uri;
            for (const auto &member : group.members)
                EXPECT_EQ(member.normalized_uri, uri);
        }
    }
    EXPECT_TRUE(saw_same_bytes);
    EXPECT_TRUE(saw_same_file);
}

TEST_F(CatalogServiceTest, CullBurstProposeAndAcceptStacksWithoutAutoDelete)
{
    // Seed capture metadata via commit_imported_asset (burst propose is metadata-only).
    {
        auto repository = SqliteCatalogRepository::create(database_path);
        ASSERT_TRUE(repository) << repository.error().message;
        std::vector<std::string> seeded;
        for (int index = 0; index < 3; ++index)
        {
            AssetRecord asset;
            asset.id = "ast_burst_" + std::to_string(index);
            asset.normalized_uri = "file:///library/burst/photo-" + std::to_string(index) + ".jpg";
            asset.media_type = std::string(kMediaTypeJpeg);
            asset.size_bytes = 1000U + static_cast<std::uint64_t>(index);
            asset.mtime_unix_ms = 10'000 + index;
            asset.created_unix_ms = 20'000 + index;
            asset.capture.camera_make = "RavoCam";
            asset.capture.camera_model = "Burst";
            asset.capture.captured_unix_s = 1'700'000'000 + index;
            ASSERT_TRUE(repository.value()->commit_imported_asset(asset));
            seeded.push_back(asset.id);
        }
        AssetRecord outlier;
        outlier.id = "ast_burst_outlier";
        outlier.normalized_uri = "file:///library/burst/photo-outlier.jpg";
        outlier.media_type = std::string(kMediaTypeJpeg);
        outlier.size_bytes = 1100;
        outlier.mtime_unix_ms = 11'000;
        outlier.created_unix_ms = 21'000;
        outlier.capture.camera_make = "RavoCam";
        outlier.capture.camera_model = "Burst";
        outlier.capture.captured_unix_s = 1'700'000'060;
        ASSERT_TRUE(repository.value()->commit_imported_asset(outlier));
        ASSERT_TRUE(repository.value()->close());
    }

    ASSERT_TRUE(open_service(false));
    const std::vector<std::string> burst_ids{"ast_burst_0", "ast_burst_1", "ast_burst_2"};

    BurstProposeRequest propose;
    propose.window_seconds = 1;
    auto report = service->propose_burst_groups(propose);
    ASSERT_TRUE(report) << report.error().message;
    ASSERT_EQ(report.value().proposals.size(), 1U);
    ASSERT_EQ(report.value().proposals.front().members.size(), 3U);

    BurstAcceptRequest blocked;
    blocked.asset_ids = burst_ids;
    blocked.user_initiated = false;
    auto missing = service->accept_burst_group_proposal(blocked);
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().context.at("reason"), "missing_user_initiated");

    BurstAcceptRequest accept;
    accept.asset_ids = burst_ids;
    accept.user_initiated = true;
    auto accepted = service->accept_burst_group_proposal(accept);
    ASSERT_TRUE(accepted) << accepted.error().message;
    EXPECT_TRUE(accepted.value().catalog_mutated);
    EXPECT_EQ(accepted.value().stack.stack.member_ids.size(), 3U);
    EXPECT_EQ(accepted.value().stack.stack.pick_asset_id, "ast_burst_0");

    // Default Library list collapses stacks to the pick; originals remain catalogued.
    auto after = service->list_assets();
    ASSERT_TRUE(after) << after.error().message;
    EXPECT_GE(after.value().size(), 2U);
    auto stacked = service->find_library_stack(accepted.value().stack.stack.id);
    ASSERT_TRUE(stacked && stacked.value())
        << (stacked ? "missing stack" : stacked.error().message);
    EXPECT_EQ(stacked.value()->member_ids.size(), 3U);
}

TEST_F(CatalogServiceTest, CullNearDuplicatesGroupsVisuallySimilarJpegs)
{
    ASSERT_TRUE(open_service(true));
    const auto a_path = root / "near-a.jpg";
    const auto b_path = root / "near-b.jpg";
    const auto different_path = root / "different.jpg";

    const auto write_pattern = [](const std::filesystem::path &path, int quality,
                                  bool invert) -> bool
    {
        QImage image(32, 24, QImage::Format_RGB888);
        image.setColorSpace(QColorSpace(QColorSpace::SRgb));
        for (int y = 0; y < image.height(); ++y)
        {
            for (int x = 0; x < image.width(); ++x)
            {
                const int band = ((x / 4) + (y / 4)) % 2;
                const int value = invert ? (band ? 30 : 220) : (band ? 220 : 30);
                image.setPixel(x, y, qRgb(value, value / 2, 255 - value));
            }
        }
        return image.save(QString::fromStdString(path.string()), "JPEG", quality);
    };
    ASSERT_TRUE(write_pattern(a_path, 92, false));
    ASSERT_TRUE(write_pattern(b_path, 40, false));        // same pattern, heavier JPEG
    ASSERT_TRUE(write_pattern(different_path, 92, true)); // inverted checker

    auto a = service->import_one(a_path.string(), CancellationToken{});
    ASSERT_TRUE(a) << a.error().message;
    auto b = service->import_one(b_path.string(), CancellationToken{});
    ASSERT_TRUE(b) << b.error().message;
    auto different = service->import_one(different_path.string(), CancellationToken{});
    ASSERT_TRUE(different) << different.error().message;

    NearDuplicateRequest request;
    request.max_hamming = 5;
    auto report = service->find_near_duplicate_groups(request);
    ASSERT_TRUE(report) << report.error().message;
    EXPECT_EQ(report.value().schema, kCullNearDuplicateContractVersion);
    EXPECT_EQ(report.value().assets_fingerprinted, 3U);
    ASSERT_FALSE(report.value().groups.empty());

    bool found_pair = false;
    for (const auto &group : report.value().groups)
    {
        std::set<std::string> ids;
        for (const auto &member : group.members)
            ids.insert(member.asset_id);
        if (ids.count(a.value().asset->id) && ids.count(b.value().asset->id))
        {
            found_pair = true;
            EXPECT_EQ(ids.count(different.value().asset->id), 0U);
            EXPECT_GE(group.members.size(), 2U);
            EXPECT_FALSE(group.fingerprint_hex.empty());
        }
    }
    EXPECT_TRUE(found_pair);
}

TEST_F(CatalogServiceTest, CullNearDuplicatesDoesNotMutateCatalog)
{
    ASSERT_TRUE(open_service(true));
    ASSERT_TRUE(write_jpeg(root / "x.jpg", QColor(1, 2, 3)));
    ASSERT_TRUE(write_jpeg(root / "y.jpg", QColor(1, 2, 3)));
    ASSERT_TRUE(service->import_one((root / "x.jpg").string(), CancellationToken{}));
    ASSERT_TRUE(service->import_one((root / "y.jpg").string(), CancellationToken{}));
    auto before = service->snapshot();
    ASSERT_TRUE(before);
    auto report = service->find_near_duplicate_groups({});
    ASSERT_TRUE(report) << report.error().message;
    auto after = service->snapshot();
    ASSERT_TRUE(after);
    EXPECT_EQ(before.value().revision, after.value().revision);
}

TEST(BurstComparePairTest, ResolvesAdjacentPairAndClampsAtEnds)
{
    LibraryStackRecord stack;
    stack.id = "stk_1";
    stack.pick_asset_id = "a";
    stack.member_ids = {"a", "b", "c"};

    auto current = resolve_burst_compare_pair(stack, "b", BurstCompareStep::kCurrent);
    ASSERT_TRUE(current) << current.error().message;
    EXPECT_EQ(current.value().focus_asset_id, "b");
    EXPECT_EQ(current.value().compare_asset_id, "c");
    EXPECT_EQ(current.value().focus_index, 1U);

    auto next = resolve_burst_compare_pair(stack, "b", BurstCompareStep::kNext);
    ASSERT_TRUE(next);
    EXPECT_EQ(next.value().focus_asset_id, "c");
    EXPECT_EQ(next.value().compare_asset_id, "b");

    auto next_end = resolve_burst_compare_pair(stack, "c", BurstCompareStep::kNext);
    ASSERT_TRUE(next_end);
    EXPECT_EQ(next_end.value().focus_asset_id, "c");
    EXPECT_EQ(next_end.value().compare_asset_id, "b");

    auto previous = resolve_burst_compare_pair(stack, "b", BurstCompareStep::kPrevious);
    ASSERT_TRUE(previous);
    EXPECT_EQ(previous.value().focus_asset_id, "a");
    EXPECT_EQ(previous.value().compare_asset_id, "b");

    auto previous_start = resolve_burst_compare_pair(stack, "a", BurstCompareStep::kPrevious);
    ASSERT_TRUE(previous_start);
    EXPECT_EQ(previous_start.value().focus_asset_id, "a");
    EXPECT_EQ(previous_start.value().compare_asset_id, "b");

    LibraryStackRecord single;
    single.id = "s";
    single.member_ids = {"only"};
    auto singleton = resolve_burst_compare_pair(single, "only", BurstCompareStep::kCurrent);
    ASSERT_FALSE(singleton);
    EXPECT_EQ(singleton.error().context.at("reason"), "burst_compare_singleton_stack");
}

TEST_F(CatalogServiceTest, CullBurstCompareResolvesSurveyPairAndSteps)
{
    {
        auto repository = SqliteCatalogRepository::create(database_path);
        ASSERT_TRUE(repository) << repository.error().message;
        for (int index = 0; index < 3; ++index)
        {
            AssetRecord asset;
            asset.id = "ast_cmp_" + std::to_string(index);
            asset.normalized_uri = "file:///library/cmp/photo-" + std::to_string(index) + ".jpg";
            asset.media_type = std::string(kMediaTypeJpeg);
            asset.size_bytes = 1000U + static_cast<std::uint64_t>(index);
            asset.mtime_unix_ms = 10'000 + index;
            asset.created_unix_ms = 20'000 + index;
            ASSERT_TRUE(repository.value()->commit_imported_asset(asset));
        }
        ASSERT_TRUE(repository.value()->close());
    }
    ASSERT_TRUE(open_service(false));
    auto stacked = service->stack_assets({"ast_cmp_0", "ast_cmp_1", "ast_cmp_2"}, "ast_cmp_0", {});
    ASSERT_TRUE(stacked) << stacked.error().message;

    BurstCompareRequest current;
    current.asset_id = "ast_cmp_1";
    current.step = BurstCompareStep::kCurrent;
    auto pair = service->resolve_burst_compare_pair(current);
    ASSERT_TRUE(pair) << pair.error().message;
    EXPECT_EQ(pair.value().schema, kCullBurstCompareContractVersion);
    EXPECT_EQ(pair.value().focus_asset_id, "ast_cmp_1");
    EXPECT_EQ(pair.value().compare_asset_id, "ast_cmp_2");
    EXPECT_EQ(pair.value().member_ids.size(), 3U);

    BurstCompareRequest next;
    next.asset_id = "ast_cmp_1";
    next.step = BurstCompareStep::kNext;
    auto stepped = service->resolve_burst_compare_pair(next);
    ASSERT_TRUE(stepped);
    EXPECT_EQ(stepped.value().focus_asset_id, "ast_cmp_2");
    EXPECT_EQ(stepped.value().compare_asset_id, "ast_cmp_1");

    // Fail-closed when not stacked after dissolve.
    ASSERT_TRUE(service->unstack_assets(stacked.value().stack.id, {}));
    auto missing = service->resolve_burst_compare_pair(current);
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().context.at("reason"), "burst_compare_not_stacked");
}

TEST_F(CatalogServiceTest, Cor01NearDupAssetBoundFailsClosed)
{
    ASSERT_TRUE(open_service(true));
    ASSERT_TRUE(write_jpeg(root / "bound-a.jpg", QColor(1, 2, 3)));
    ASSERT_TRUE(write_jpeg(root / "bound-b.jpg", QColor(4, 5, 6)));
    ASSERT_TRUE(service->import_one((root / "bound-a.jpg").string(), CancellationToken{}));
    ASSERT_TRUE(service->import_one((root / "bound-b.jpg").string(), CancellationToken{}));
    NearDuplicateRequest request;
    request.max_assets = 1;
    auto report = service->find_near_duplicate_groups(request);
    ASSERT_FALSE(report);
    EXPECT_EQ(report.error().context.at("reason"), "near_dup_asset_bound_exceeded");
}

TEST_F(CatalogServiceTest, Cor01NearDupGroupsArePairwiseNonTransitive)
{
    ASSERT_TRUE(open_service(true));
    ASSERT_TRUE(write_jpeg(root / "p1.jpg", QColor(10, 10, 10)));
    ASSERT_TRUE(write_jpeg(root / "p2.jpg", QColor(10, 10, 10)));
    ASSERT_TRUE(write_jpeg(root / "p3.jpg", QColor(10, 10, 10)));
    ASSERT_TRUE(service->import_one((root / "p1.jpg").string(), CancellationToken{}));
    ASSERT_TRUE(service->import_one((root / "p2.jpg").string(), CancellationToken{}));
    ASSERT_TRUE(service->import_one((root / "p3.jpg").string(), CancellationToken{}));
    NearDuplicateRequest request;
    request.max_hamming = 0;
    auto report = service->find_near_duplicate_groups(request);
    ASSERT_TRUE(report) << report.error().message;
    EXPECT_TRUE(report.value().non_authoritative);
    for (const auto &group : report.value().groups)
    {
        EXPECT_EQ(group.members.size(), 2U);
        EXPECT_LE(group.max_hamming_in_group, request.max_hamming);
    }
}

} // namespace ravo
