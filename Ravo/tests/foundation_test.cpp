#include <chrono>
#include <future>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/color.h"
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

TEST(ColorProfileStateTest, FingerprintIncludesOwnedIccAndMatrixState)
{
    ColorProfileState profile;
    profile.kind = ColorProfileKind::kIcc;
    profile.model = ColorModel::kRgb;
    profile.identifier = "embedded_icc";
    profile.icc_bytes = {1, 2, 3, 4};
    const auto first = color_profile_fingerprint(profile);
    EXPECT_EQ(first.size(), 16U);
    EXPECT_EQ(first, color_profile_fingerprint(profile));

    profile.icc_bytes.back() = 5;
    EXPECT_NE(first, color_profile_fingerprint(profile));
    profile.kind = ColorProfileKind::kMatrix;
    profile.icc_bytes.clear();
    profile.has_matrix = true;
    const auto matrix = color_profile_fingerprint(profile);
    profile.matrix_to_xyz_d50[0] = 0.5F;
    EXPECT_NE(matrix, color_profile_fingerprint(profile));
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

TEST(SerialExecutorTest, ForegroundWorkLeadsQueuedNormalWorkWithoutReorderingItsLane)
{
    SerialExecutor executor;
    std::promise<void> worker_started;
    auto worker_started_future = worker_started.get_future();
    std::promise<void> release_worker;
    auto release_worker_future = release_worker.get_future().share();
    std::vector<int> order;
    ASSERT_TRUE(executor.post(
        [&worker_started, release_worker_future]()
        {
            worker_started.set_value();
            release_worker_future.wait();
        }));
    worker_started_future.wait();

    ASSERT_TRUE(executor.post([&order]() { order.push_back(1); }));
    ASSERT_TRUE(executor.post([&order]() { order.push_back(2); }, TaskPriority::kForeground));
    ASSERT_TRUE(executor.post([&order]() { order.push_back(3); }, TaskPriority::kForeground));
    release_worker.set_value();
    executor.wait_idle();

    EXPECT_EQ(order, (std::vector<int>{2, 3, 1}));
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
