#include <clocale>
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

} // namespace
} // namespace ravo
