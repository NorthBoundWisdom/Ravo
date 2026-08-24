#include <chrono>
#include <string>

#include <gtest/gtest.h>

#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/executor.h"
#include "ravo/foundation/json.h"

namespace ravo
{
namespace
{

TEST(JsonTest, ParsesUnicodeAndWritesStableObjectOrder)
{
    const auto parsed = parse_json(R"({"z":true,"a":"\uD83D\uDE80"})");

    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_EQ(serialize_json(parsed.value()), R"({"a":"🚀","z":true})");
}

TEST(JsonTest, RejectsDuplicateKeysWithPositionContext)
{
    const auto parsed = parse_json(R"({"id":1,"id":2})");

    ASSERT_FALSE(parsed);
    EXPECT_EQ(parsed.error().code, ErrorCode::kValidation);
    EXPECT_TRUE(parsed.error().context.contains("line"));
    EXPECT_TRUE(parsed.error().context.contains("column"));
}

TEST(CancellationTest, FirstCancellationWinsAndTokensKeepTheReason)
{
    CancellationSource source;
    const auto token = source.token();

    EXPECT_TRUE(source.cancel("user_requested"));
    EXPECT_FALSE(source.cancel("ignored"));
    EXPECT_TRUE(token.is_cancellation_requested());
    EXPECT_EQ(token.reason(), "user_requested");

    const auto checked = token.check();
    ASSERT_FALSE(checked);
    EXPECT_EQ(checked.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(checked.error().context.at("reason"), "user_requested");
}

TEST(CancellationTest, ExpiredDeadlineCancelsWithAStructuredReason)
{
    const auto source = CancellationSource::with_deadline(std::chrono::steady_clock::now() -
                                                          std::chrono::milliseconds{1});
    const auto checked = source.token().check();

    ASSERT_FALSE(checked);
    EXPECT_EQ(checked.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(checked.error().context.at("reason"), "deadline_exceeded");
}

TEST(SerialExecutorTest, SubmitRunsOnTheWorkerAndNestedSubmitDoesNotDeadlock)
{
    SerialExecutor executor;
    int value = 0;
    executor.submit([&value]() { value = 1; });
    EXPECT_EQ(value, 1);
    executor.submit(
        [&value, &executor]()
        {
            EXPECT_TRUE(executor.is_worker_thread());
            executor.submit([&value]() { value = 2; });
        });
    EXPECT_EQ(value, 2);
}

TEST(SerialExecutorTest, PostedWorkCompletesBeforeWaitIdle)
{
    SerialExecutor executor;
    int value = 0;
    ASSERT_TRUE(executor.post([&value]() { value = 7; }));
    executor.wait_idle();
    EXPECT_EQ(value, 7);
}

TEST(SerialExecutorTest, WorkerStackIsLargeEnoughForRawImport)
{
    SerialExecutor executor;
    int marker = 0;
    executor.submit(
        [&marker]()
        {
            alignas(16) unsigned char pad[2U * 1024U * 1024U];
            pad[0] = 1U;
            pad[sizeof(pad) - 1U] = 2U;
            marker = static_cast<int>(pad[0]) + static_cast<int>(pad[sizeof(pad) - 1U]);
        });
    EXPECT_EQ(marker, 3);
}

} // namespace
} // namespace ravo
