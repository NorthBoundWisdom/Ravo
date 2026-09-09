#include <clocale>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "application_internal.h"
#include "ravo/foundation/parse_number.h"

namespace ravo
{
namespace
{

TEST(CliCatalogArgsParseTest, DoubleFlagUsesSharedAsciiParser)
{
    auto accepted = cli_internal::parse_double_flag("-1.5", "--exposure-ev");
    ASSERT_TRUE(accepted);
    EXPECT_DOUBLE_EQ(accepted.value(), -1.5);

    auto rejected = cli_internal::parse_double_flag("1,5", "--exposure-ev");
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kInvalidArgument);
    EXPECT_EQ(rejected.error().context.at("option"), "--exposure-ev");
    EXPECT_EQ(rejected.error().context.at("value"), "1,5");

    auto overlong = cli_internal::parse_double_flag(std::string(65, '1'), "--contrast");
    ASSERT_FALSE(overlong);
}

TEST(CliCatalogArgsParseTest, DoubleFlagIgnoresCommaProcessLocale)
{
    const char *previous = std::setlocale(LC_NUMERIC, nullptr);
    const std::string previous_locale = previous == nullptr ? std::string("C") : previous;
    if (std::setlocale(LC_NUMERIC, "de_DE.UTF-8") == nullptr &&
        std::setlocale(LC_NUMERIC, "de_DE") == nullptr &&
        std::setlocale(LC_NUMERIC, "fr_FR.UTF-8") == nullptr)
    {
        GTEST_SKIP() << "No comma-decimal locale available on this host";
    }

    auto accepted = cli_internal::parse_double_flag("2.25", "--contrast");
    ASSERT_TRUE(accepted);
    EXPECT_DOUBLE_EQ(accepted.value(), 2.25);
    EXPECT_FALSE(cli_internal::parse_double_flag("2,25", "--contrast"));

    std::setlocale(LC_NUMERIC, previous_locale.c_str());
}

TEST(CliCatalogArgsParseTest, RoiParsingUsesSharedAsciiParser)
{
    const std::vector<std::string_view> positional{"catalog", "preview", "--roi",
                                                   "0.1,0.2,0.3,0.4"};
    auto parsed = cli_internal::parse_catalog_flags(positional);
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_TRUE(parsed.value().roi.has_value());
    EXPECT_DOUBLE_EQ(parsed.value().roi->x, 0.1);
    EXPECT_DOUBLE_EQ(parsed.value().roi->y, 0.2);
    EXPECT_DOUBLE_EQ(parsed.value().roi->width, 0.3);
    EXPECT_DOUBLE_EQ(parsed.value().roi->height, 0.4);

    const std::vector<std::string_view> malformed{"catalog", "preview", "--roi", "0.1,0.2,bad,0.4"};
    auto rejected = cli_internal::parse_catalog_flags(malformed);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_preview_roi");
}

TEST(CliCatalogArgsParseTest, DuplicateFloatOptionsAreRejected)
{
    const std::vector<std::string_view> duplicate_roi{"catalog", "preview", "--roi",
                                                      "0,0,1,1", "--roi",   "0.1,0.1,0.2,0.2"};
    // First --roi uses classic '.' decimals after commas as field separators:
    // "0","0","1","1" are valid tokens.
    auto rejected_roi = cli_internal::parse_catalog_flags(duplicate_roi);
    ASSERT_FALSE(rejected_roi);
    EXPECT_NE(rejected_roi.error().message.find("specified more than once"), std::string::npos);

    const std::vector<std::string_view> duplicate_exposure{
        "catalog", "develop", "--exposure-ev", "0.5", "--exposure-ev", "1.0"};
    auto rejected_exposure = cli_internal::parse_catalog_flags(duplicate_exposure);
    ASSERT_FALSE(rejected_exposure);
    EXPECT_NE(rejected_exposure.error().message.find("specified more than once"),
              std::string::npos);
}

TEST(CliCatalogArgsParseTest, SmartPreviewEnsureIsBooleanFlag)
{
    const std::vector<std::string_view> args{"catalog", "smart-preview", "--asset-id", "asset-1",
                                             "--ensure"};
    auto parsed = cli_internal::parse_catalog_flags(args);
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_EQ(parsed.value().asset_id, "asset-1");
    EXPECT_TRUE(parsed.value().ensure);

    const std::vector<std::string_view> with_following{
        "catalog", "smart-preview", "--asset-id", "asset-1", "--ensure", "--max-edge", "2048"};
    auto parsed_following = cli_internal::parse_catalog_flags(with_following);
    ASSERT_TRUE(parsed_following) << parsed_following.error().message;
    EXPECT_TRUE(parsed_following.value().ensure);
    ASSERT_TRUE(parsed_following.value().max_edge.has_value());
    EXPECT_EQ(*parsed_following.value().max_edge, 2048);
}

TEST(CliCatalogArgsParseTest, DuplicateSmartPreviewEnsureIsRejected)
{
    const std::vector<std::string_view> args{"catalog", "smart-preview", "--asset-id",
                                             "asset-1", "--ensure",      "--ensure"};
    auto rejected = cli_internal::parse_catalog_flags(args);
    ASSERT_FALSE(rejected);
    EXPECT_NE(rejected.error().message.find("--ensure can only be specified once"),
              std::string::npos);
}

TEST(CliCatalogArgsParseTest, ExportJobCreateParsesMultiAssetDirectoryFlags)
{
    const std::vector<std::string_view> args{
        "catalog",
        "export-job-create",
        "--asset-id",
        "asset-a",
        "--asset-id",
        "asset-b",
        "--output-dir",
        "/tmp/output",
        "--export-job",
        "/tmp/job.json",
        "--job-id",
        "job-1",
        "--filename-template",
        "{stem}-{index}",
    };
    auto parsed = cli_internal::parse_catalog_flags(args);
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_TRUE(parsed.value().asset_id.empty());
    ASSERT_EQ(parsed.value().asset_ids.size(), 2U);
    EXPECT_EQ(parsed.value().asset_ids[0], "asset-a");
    EXPECT_EQ(parsed.value().asset_ids[1], "asset-b");
    EXPECT_EQ(parsed.value().output_directory, "/tmp/output");
    EXPECT_EQ(parsed.value().export_job, "/tmp/job.json");
    EXPECT_EQ(parsed.value().job_id, "job-1");
    EXPECT_EQ(parsed.value().filename_template, "{stem}-{index}");
}

TEST(CliCatalogArgsParseTest, OfflineProxyMaxTotalBytesUsesUint64)
{
    const auto parse_with = [](const std::string_view value)
    {
        return cli_internal::parse_catalog_flags(std::vector<std::string_view>{
            "catalog", "offline-proxy-evict", "--max-total-bytes", value});
    };

    auto eight_gib = parse_with("8589934592");
    ASSERT_TRUE(eight_gib) << eight_gib.error().message;
    ASSERT_TRUE(eight_gib.value().max_total_bytes.has_value());
    EXPECT_EQ(*eight_gib.value().max_total_bytes, 8589934592ULL);

    auto one = parse_with("1");
    ASSERT_TRUE(one) << one.error().message;
    ASSERT_TRUE(one.value().max_total_bytes.has_value());
    EXPECT_EQ(*one.value().max_total_bytes, 1ULL);

    auto zero = parse_with("0");
    ASSERT_FALSE(zero);
    EXPECT_NE(zero.error().message.find("must be positive"), std::string::npos);

    auto negative = parse_with("-1");
    ASSERT_FALSE(negative);
    EXPECT_EQ(negative.error().code, ErrorCode::kInvalidArgument);

    auto max_u64 = parse_with("18446744073709551615");
    ASSERT_TRUE(max_u64) << max_u64.error().message;
    ASSERT_TRUE(max_u64.value().max_total_bytes.has_value());
    EXPECT_EQ(*max_u64.value().max_total_bytes, UINT64_C(18446744073709551615));

    auto overflow = parse_with("18446744073709551616");
    ASSERT_FALSE(overflow);
    EXPECT_EQ(overflow.error().code, ErrorCode::kInvalidArgument);
}

} // namespace
} // namespace ravo
