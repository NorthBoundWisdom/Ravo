#include <gtest/gtest.h>
#include "studio_operation_store.h"
#include "mcp_test_support.h"

namespace ravo
{
using studio_test_support::json_string;
TEST(StudioOperationStoreTest, UnresolvedOutcomePreservesKnownCommitAndCannotRestart)
{
    StudioOperationStore store;
    auto reserved = store.reserve("session");
    ASSERT_TRUE(reserved);
    const auto id = json_string(reserved.value(), "operation_id");
    const JsonValue request = JsonValue::Object{{"kind", "annotation"}};
    ASSERT_TRUE(store.begin(id, request));
    store.unresolved(id, make_error(ErrorCode::kIo, "Committed revision unavailable",
                                    {{"catalog_committed", "true"}}));
    auto receipt = store.get(id);
    ASSERT_TRUE(receipt);
    EXPECT_EQ(json_string(receipt.value(), "phase"), "unknown");
    EXPECT_TRUE(*receipt.value().find("mutation_applied")->boolean_if());
    EXPECT_EQ(receipt.value().find("catalog_revision"), nullptr);
    auto cancelled = store.cancel(id);
    ASSERT_TRUE(cancelled);
    EXPECT_EQ(serialize_json(cancelled.value()), serialize_json(receipt.value()));
    auto replay = store.begin(id, request);
    ASSERT_TRUE(replay);
    EXPECT_FALSE(replay.value());
    auto uncertain = store.reserve("session");
    ASSERT_TRUE(uncertain);
    const auto other = json_string(uncertain.value(), "operation_id");
    ASSERT_TRUE(store.begin(other, request));
    store.unresolved(other, make_error(ErrorCode::kIo, "Outcome unavailable"));
    EXPECT_TRUE(store.get(other).value().find("mutation_applied")->is_null());
}

TEST(StudioOperationStoreTest, ReservationIsRequiredAndReplayDoesNotStartAnotherWrite)
{
    StudioOperationStore store;
    const JsonValue request = JsonValue::Object{{"asset_id", "one"}, {"kind", "patch"}};
    EXPECT_FALSE(store.begin("unknown", request));
    auto reserved = store.reserve("session");
    ASSERT_TRUE(reserved);
    const auto id = json_string(reserved.value(), "operation_id");
    auto begun = store.begin(id, request);
    ASSERT_TRUE(begun);
    EXPECT_TRUE(begun.value());
    auto duplicate = store.begin(id, request);
    ASSERT_TRUE(duplicate);
    EXPECT_FALSE(duplicate.value());
    EXPECT_FALSE(store.begin(id, JsonValue::Object{{"asset_id", "other"}}));
    store.finish(id, true, 42, 9);
    const auto saved = store.get(id);
    ASSERT_TRUE(saved);
    auto cancelled = store.cancel(id);
    ASSERT_TRUE(cancelled);
    EXPECT_EQ(serialize_json(saved.value()), serialize_json(cancelled.value()));
    ASSERT_TRUE(store.token(id));
    EXPECT_FALSE(store.token(id).value().is_cancellation_requested());
    duplicate = store.begin(id, request);
    ASSERT_TRUE(duplicate);
    EXPECT_FALSE(duplicate.value());
}
TEST(StudioOperationStoreTest, PendingPartialProgressNeverLooksTerminalAndCancelledIdsCannotBegin)
{
    StudioOperationStore store;
    auto reserved = store.reserve("session");
    ASSERT_TRUE(reserved);
    const auto id = json_string(reserved.value(), "operation_id");
    ASSERT_TRUE(store.begin(id, JsonValue::Object{{"kind", "batch"}}));
    const JsonValue partial =
        JsonValue::Object{{"applied", JsonValue::number("1")}, {"partial", true}};
    store.progress(id, partial);
    auto progress = store.get(id);
    ASSERT_TRUE(progress);
    EXPECT_EQ(json_string(progress.value(), "phase"), "pending");
    EXPECT_TRUE(*progress.value().find("mutation_applied")->boolean_if());
    ASSERT_TRUE(store.cancel(id));
    ASSERT_TRUE(store.token(id));
    EXPECT_TRUE(store.token(id).value().is_cancellation_requested());
    store.finish(id, true, 7, 0);
    store.progress(id, partial);
    auto finished = store.get(id);
    ASSERT_TRUE(finished);
    EXPECT_EQ(json_string(finished.value(), "phase"), "partially_committed");
    auto next = store.reserve("session");
    ASSERT_TRUE(next);
    const auto next_id = json_string(next.value(), "operation_id");
    ASSERT_TRUE(store.cancel(next_id));
    EXPECT_FALSE(store.begin(next_id, JsonValue::Object{}));
}
TEST(StudioOperationStoreTest, EvictedReservationsCannotBeReplayedAndActiveReceiptsAreRetained)
{
    StudioOperationStore store;
    auto active = store.reserve("session");
    ASSERT_TRUE(active);
    const auto active_id = json_string(active.value(), "operation_id");
    ASSERT_TRUE(store.begin(active_id, JsonValue::Object{}));
    auto victim = store.reserve("session");
    ASSERT_TRUE(victim);
    const auto old_id = json_string(victim.value(), "operation_id");
    for (int index = 0; index < 128; ++index)
        ASSERT_TRUE(store.reserve("session"));
    EXPECT_TRUE(store.get(active_id));
    EXPECT_FALSE(store.get(old_id));
    EXPECT_FALSE(store.begin(old_id, JsonValue::Object{}));
    store.cancel_all();
    ASSERT_TRUE(store.token(active_id));
    EXPECT_TRUE(store.token(active_id).value().is_cancellation_requested());
    EXPECT_FALSE(store.token(old_id));
}
} // namespace ravo
