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

} // namespace ravo
