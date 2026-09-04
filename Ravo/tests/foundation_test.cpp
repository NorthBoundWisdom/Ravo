#include <chrono>
#include <clocale>
#include <future>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/color.h"
#include "ravo/foundation/executor.h"
#include "ravo/foundation/json.h"
#include "ravo/foundation/parse_number.h"

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

TEST(ParseAsciiDoubleTest, AcceptsBoundaryFiniteTokens)
{
    double value = 0.0;
    ASSERT_TRUE(parse_ascii_double("0", value));
    EXPECT_DOUBLE_EQ(value, 0.0);
    ASSERT_TRUE(parse_ascii_double("-0.0", value));
    EXPECT_DOUBLE_EQ(value, 0.0);
    ASSERT_TRUE(parse_ascii_double("+1.5", value));
    EXPECT_DOUBLE_EQ(value, 1.5);
    ASSERT_TRUE(parse_ascii_double("1e-3", value));
    EXPECT_DOUBLE_EQ(value, 0.001);
    ASSERT_TRUE(parse_ascii_double("2.5E2", value));
    EXPECT_DOUBLE_EQ(value, 250.0);
}

TEST(ParseAsciiDoubleTest, RejectsMalformedAndNonFiniteTokens)
{
    double value = 99.0;
    EXPECT_FALSE(parse_ascii_double("", value));
    EXPECT_FALSE(parse_ascii_double(" ", value));
    EXPECT_FALSE(parse_ascii_double("1.2.3", value));
    EXPECT_FALSE(parse_ascii_double("12abc", value));
    EXPECT_FALSE(parse_ascii_double("abc", value));
    EXPECT_FALSE(parse_ascii_double("1,5", value));
    EXPECT_FALSE(parse_ascii_double("nan", value));
    EXPECT_FALSE(parse_ascii_double("NaN", value));
    EXPECT_FALSE(parse_ascii_double("inf", value));
    EXPECT_FALSE(parse_ascii_double("+inf", value));
    EXPECT_FALSE(parse_ascii_double("-infinity", value));
    EXPECT_FALSE(parse_ascii_double("0x10", value));
    EXPECT_FALSE(parse_ascii_double(std::string(65, '1'), value));
    EXPECT_DOUBLE_EQ(value, 99.0);
}

TEST(ParseAsciiDoubleTest, RemainsDotDecimalUnderCommaProcessLocale)
{
    const char *previous = std::setlocale(LC_NUMERIC, nullptr);
    ASSERT_NE(previous, nullptr);
    const std::string saved(previous);
    const char *comma_locales[] = {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "fr_FR.utf8",
                                   "de_DE",       "fr_FR",      "nl_NL.UTF-8"};
    bool switched = false;
    for (const char *candidate : comma_locales)
    {
        if (std::setlocale(LC_NUMERIC, candidate) != nullptr)
        {
            switched = true;
            break;
        }
    }
    if (!switched)
    {
        GTEST_SKIP() << "No comma-decimal LC_NUMERIC locale is installed";
    }

    double value = 0.0;
    EXPECT_TRUE(parse_ascii_double("1.25", value));
    EXPECT_DOUBLE_EQ(value, 1.25);
    EXPECT_FALSE(parse_ascii_double("1,25", value));
    // Bare strtod would follow the process locale; the owner must not.
    EXPECT_TRUE(parse_ascii_double("3.1415", value));
    EXPECT_DOUBLE_EQ(value, 3.1415);

    std::setlocale(LC_NUMERIC, saved.c_str());
}

} // namespace
} // namespace ravo
