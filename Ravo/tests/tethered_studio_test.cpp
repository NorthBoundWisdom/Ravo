#include <gtest/gtest.h>

#include "ravo/services/tethered_studio.h"

namespace ravo
{

TEST(TetheredStudioTest, ProbeFailClosesAsDeferred)
{
    auto probed = probe_tethered_studio_support();
    ASSERT_FALSE(probed);
    EXPECT_EQ(probed.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(probed.error().context.at("reason"), "tethered_deferred");
    EXPECT_EQ(probed.error().context.at("contract"), kTetheredStudioContractVersion);
    EXPECT_EQ(probed.error().context.at("track"), "tethered_studio");
}

} // namespace ravo
