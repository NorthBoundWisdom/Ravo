#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "gpu_adapter.h"
#include "ravo/engine/engine.h"

namespace ravo
{
namespace
{

TEST(EngineFacadeTest, GpuAdapterDoesNotFailCpuCreate)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    EXPECT_FALSE(engine.value().gpu_backend().empty());
    EXPECT_FALSE(engine.value().operations().empty());
}

TEST(EngineFacadeTest, GpuCopyRgbHonorsCancellation)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    CancellationSource cancellation;
    ASSERT_TRUE(cancellation.cancel("gpu_test_cancel"));
    std::vector<float> input{1.0F, 2.0F, 3.0F};
    std::vector<float> output(3U, 0.0F);
    const auto copied = engine.value().gpu_copy_rgb(input, output, cancellation.token());
    ASSERT_FALSE(copied);
    EXPECT_EQ(copied.error().code, ErrorCode::kCancelled);
}

TEST(EngineFacadeTest, GpuCopyRgbMatchesHostAvailability)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    std::vector<float> input{0.0F, -1.5F, 2.25F, 1.0F};
    std::vector<float> output(input.size(), 99.0F);
    const auto copied = engine.value().gpu_copy_rgb(input, output, CancellationToken{});
#ifdef __APPLE__
    EXPECT_EQ(engine.value().gpu_backend(), "metal");
    ASSERT_TRUE(copied) << copied.error().message;
    EXPECT_EQ(output, input);
#else
    EXPECT_EQ(engine.value().gpu_backend(), "unavailable");
    ASSERT_FALSE(copied);
    EXPECT_EQ(copied.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(copied.error().context.at("reason"), "gpu_unavailable");
    EXPECT_EQ(output[0], 99.0F);
#endif
}

TEST(EngineFacadeTest, GpuCopyRgbRejectsSizeMismatchWhenAvailable)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    std::vector<float> input{1.0F, 2.0F};
    std::vector<float> output{0.0F};
    const auto copied = engine.value().gpu_copy_rgb(input, output, CancellationToken{});
    ASSERT_FALSE(copied);
#ifdef __APPLE__
    EXPECT_EQ(copied.error().code, ErrorCode::kInvalidArgument);
    EXPECT_EQ(copied.error().context.at("reason"), "gpu_copy_size_mismatch");
#else
    EXPECT_EQ(copied.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(copied.error().context.at("reason"), "gpu_unavailable");
#endif
}

TEST(GpuAdapterTest, TryCreateReportsTheSameBackendAsTheFacade)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    auto created = GpuAdapter::try_create();
#ifdef __APPLE__
    ASSERT_TRUE(created) << created.error().message;
    EXPECT_EQ(created.value()->backend_id(), "metal");
    EXPECT_EQ(engine.value().gpu_backend(), created.value()->backend_id());
    std::vector<float> input{4.0F, 5.0F};
    std::vector<float> output(2U, 0.0F);
    const auto copied = created.value()->copy_rgb(input, output, CancellationToken{});
    ASSERT_TRUE(copied) << copied.error().message;
    EXPECT_EQ(output, input);
#else
    ASSERT_FALSE(created);
    EXPECT_EQ(created.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(created.error().context.at("reason"), "gpu_unavailable");
    EXPECT_EQ(engine.value().gpu_backend(), "unavailable");
#endif
}

} // namespace
} // namespace ravo
