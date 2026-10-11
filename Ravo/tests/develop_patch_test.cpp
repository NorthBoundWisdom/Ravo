#include <gtest/gtest.h>
#include "ravo/recipe/develop_patch.h"
#include "ravo/recipe/local_adjustment.h"

namespace ravo
{
TEST(DevelopPatchTest, NumericAndTextFieldsAreStrictAndTransactional)
{
    DevelopParams base;
    auto fields = parse_json(
        R"([{"name":"exposure","value":0.12345678912345678},{"name":"watermarkText","value":"RAVO"}])");
    ASSERT_TRUE(fields);
    auto applied = apply_develop_patch(base, fields.value());
    ASSERT_TRUE(applied);
    EXPECT_DOUBLE_EQ(applied.value().exposure_ev, 0.12345678912345678);
    EXPECT_EQ(applied.value().watermark.text, "RAVO");
    for (const auto *invalid :
         {R"([])", R"([{"name":"missing","value":1}])", R"([{"name":"exposure","value":1000}])",
          R"([{"name":"exposure","value":true}])",
          R"([{"name":"exposure","value":1},{"name":"exposure","value":2}])",
          R"([{"name":"exposure","value":1,"extra":true}])",
          R"([{"name":"exposure","value":1e999}])",
          R"([{"name":"exposure","value":1},{"name":"missing","value":2}])"})
    {
        auto parsed = parse_json(invalid);
        ASSERT_TRUE(parsed);
        EXPECT_FALSE(apply_develop_patch(base, parsed.value())) << invalid;
        EXPECT_EQ(base, DevelopParams{});
    }
}
TEST(DevelopPatchTest, LocalPatchPreservesGlobalAndOtherLocalAdjustments)
{
    DevelopParams base;
    base.exposure_ev = 0.25;
    ASSERT_TRUE(create_local_adjustment(base, "one", 5));
    ASSERT_TRUE(create_local_adjustment(base, "two", 3));
    auto fields = parse_json(R"([{"name":"exposure","value":0.75}])");
    ASSERT_TRUE(fields);
    auto next = apply_develop_patch(base, fields.value(), "one");
    ASSERT_TRUE(next);
    EXPECT_DOUBLE_EQ(next.value().exposure_ev, 0.25);
    auto first = local_adjustment_develop(next.value(), "one");
    ASSERT_TRUE(first);
    EXPECT_DOUBLE_EQ(first.value().exposure_ev, 0.75);
    auto second = local_adjustment_develop(next.value(), "two");
    ASSERT_TRUE(second);
    EXPECT_DOUBLE_EQ(second.value().exposure_ev, 0);
    EXPECT_FALSE(apply_develop_patch(base, fields.value(), "missing"));
    EXPECT_DOUBLE_EQ(base.exposure_ev, 0.25);
}
} // namespace ravo
