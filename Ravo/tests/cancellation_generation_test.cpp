#include <gtest/gtest.h>

#include "ravo/foundation/cancellation_generation.h"

namespace ravo
{

TEST(CancellationGenerationTest, ReplacementKeepsBorrowedTokensCancelledAndRejectsLateResults)
{
    CancellationGeneration owner;
    const auto first = owner.begin();
    const auto first_revision = owner.revision();
    EXPECT_TRUE(owner.accepts(first_revision));
    EXPECT_EQ(owner.invalidate("selection_changed"), first_revision + 1);
    EXPECT_TRUE(first.is_cancellation_requested());
    EXPECT_EQ(first.reason(), "selection_changed");
    EXPECT_FALSE(owner.accepts(first_revision));
    // Invalidation must not silently mint an uncancelled token before begin().
    EXPECT_TRUE(owner.token().is_cancellation_requested());
    const auto next = owner.begin();
    EXPECT_FALSE(next.is_cancellation_requested());
    EXPECT_TRUE(first.is_cancellation_requested());
    EXPECT_TRUE(owner.accepts(first_revision + 1));
    owner.cancel("window_closed");
    owner.cancel("later_reason");
    EXPECT_EQ(next.reason(), "window_closed");
    EXPECT_EQ(owner.revision(), first_revision + 1);
}

} // namespace ravo
