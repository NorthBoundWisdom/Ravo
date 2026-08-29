#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <QFile>
#include <QTemporaryDir>

#include "ravo/adapters/filesystem_preview_cache.h"

namespace ravo
{
namespace
{

[[nodiscard]] std::vector<std::uint8_t> test_png_bytes(const std::size_t size,
                                                       const std::uint8_t fill)
{
    static constexpr std::uint8_t kSignature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::vector<std::uint8_t> bytes(std::max<std::size_t>(size, sizeof(kSignature)), fill);
    std::copy(std::begin(kSignature), std::end(kSignature), bytes.begin());
    return bytes;
}

TEST(FilesystemPreviewCacheTest, RejectsInvalidBudgetPayloadAndOversizedEntry)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto zero_budget = FilesystemPreviewCache::create(directory.path().toStdString(), 0);
    ASSERT_FALSE(zero_budget);
    EXPECT_EQ(zero_budget.error().code, ErrorCode::kInvalidArgument);

    auto cache = FilesystemPreviewCache::create(directory.path().toStdString(), 15);
    ASSERT_TRUE(cache) << cache.error().message;
    const std::vector<std::uint8_t> invalid(8, 0);
    auto invalid_commit = cache.value()->commit_png_bytes("invalid", invalid);
    ASSERT_FALSE(invalid_commit);
    EXPECT_EQ(invalid_commit.error().code, ErrorCode::kValidation);

    auto oversized = cache.value()->commit_png_bytes("oversized", test_png_bytes(16, 1));
    ASSERT_FALSE(oversized);
    EXPECT_EQ(oversized.error().code, ErrorCode::kValidation);
    EXPECT_EQ(oversized.error().context.at("required_bytes"), "16");
    EXPECT_EQ(oversized.error().context.at("max_bytes"), "15");
    EXPECT_EQ(cache.value()->used_bytes(), 0U);
    EXPECT_EQ(cache.value()->entry_count(), 0U);
}

TEST(FilesystemPreviewCacheTest, CacheHitPromotesEntryBeforeDeterministicLruEviction)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto cache = FilesystemPreviewCache::create(directory.path().toStdString(), 32);
    ASSERT_TRUE(cache) << cache.error().message;
    auto &value = *cache.value();
    ASSERT_TRUE(value.commit_png_bytes("first", test_png_bytes(16, 1)));
    ASSERT_TRUE(value.commit_png_bytes("second", test_png_bytes(16, 2)));
    ASSERT_TRUE(value.existing_png("first"));

    auto third = value.commit_png_bytes("third", test_png_bytes(16, 3));
    ASSERT_TRUE(third) << third.error().message;
    auto first = value.existing_png("first");
    auto second = value.existing_png("second");
    auto third_existing = value.existing_png("third");
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    ASSERT_TRUE(third_existing);
    EXPECT_TRUE(first.value());
    EXPECT_FALSE(second.value());
    EXPECT_TRUE(third_existing.value());
    EXPECT_EQ(value.used_bytes(), 32U);
    EXPECT_EQ(value.entry_count(), 2U);
    EXPECT_LE(value.used_bytes(), value.max_bytes());
}

TEST(FilesystemPreviewCacheTest, ReopenIndexesAccessTimesAndPrunesToNewBudget)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    std::string first_path;
    std::string second_path;
    std::string third_path;
    {
        auto cache = FilesystemPreviewCache::create(directory.path().toStdString(), 48);
        ASSERT_TRUE(cache) << cache.error().message;
        auto first = cache.value()->commit_png_bytes("first", test_png_bytes(16, 1));
        auto second = cache.value()->commit_png_bytes("second", test_png_bytes(16, 2));
        auto third = cache.value()->commit_png_bytes("third", test_png_bytes(16, 3));
        ASSERT_TRUE(first);
        ASSERT_TRUE(second);
        ASSERT_TRUE(third);
        first_path = first.value();
        second_path = second.value();
        third_path = third.value();
    }
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(first_path, now - std::chrono::hours(3));
    std::filesystem::last_write_time(second_path, now - std::chrono::hours(2));
    std::filesystem::last_write_time(third_path, now - std::chrono::hours(1));

    auto reopened = FilesystemPreviewCache::create(directory.path().toStdString(), 32);
    ASSERT_TRUE(reopened) << reopened.error().message;
    EXPECT_FALSE(std::filesystem::exists(first_path));
    EXPECT_TRUE(std::filesystem::exists(second_path));
    EXPECT_TRUE(std::filesystem::exists(third_path));
    EXPECT_EQ(reopened.value()->used_bytes(), 32U);
    EXPECT_EQ(reopened.value()->entry_count(), 2U);
}

TEST(FilesystemPreviewCacheTest, CorruptEntriesAndAssetRemovalReleaseAccounting)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto cache = FilesystemPreviewCache::create(directory.path().toStdString(), 64);
    ASSERT_TRUE(cache) << cache.error().message;
    auto &value = *cache.value();
    auto first = value.commit_png_bytes("v3_ast_one_1x1_a_b", test_png_bytes(16, 1));
    auto second = value.commit_png_bytes("v3_ast_two_1x1_a_b", test_png_bytes(16, 2));
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    {
        QFile corrupt(QString::fromStdString(second.value()));
        ASSERT_TRUE(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_EQ(corrupt.write("corrupt", 7), 7);
    }
    auto corrupt = value.existing_png("v3_ast_two_1x1_a_b");
    ASSERT_TRUE(corrupt) << corrupt.error().message;
    EXPECT_FALSE(corrupt.value());
    EXPECT_FALSE(std::filesystem::exists(second.value()));
    EXPECT_EQ(value.used_bytes(), 16U);
    EXPECT_EQ(value.entry_count(), 1U);

    auto removed = value.remove_for_asset("ast_one");
    ASSERT_TRUE(removed) << removed.error().message;
    EXPECT_FALSE(std::filesystem::exists(first.value()));
    EXPECT_EQ(value.used_bytes(), 0U);
    EXPECT_EQ(value.entry_count(), 0U);
}

} // namespace
} // namespace ravo
