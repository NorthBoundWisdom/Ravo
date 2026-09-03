#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "gpu_adapter.h"
#include "image_ops.h"
#include "ravo/engine/engine.h"
#include "ravo/recipe/operation.h"

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

TEST(EngineFacadeTest, GpuApplyExposureMatchesCpuGoldWhenAvailable)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    ColorProfileState profile;
    profile.kind = ColorProfileKind::kMatrix;
    profile.model = ColorModel::kRgb;
    profile.identifier = "working-fixture";
    profile.has_matrix = true;
    profile.camera_input = true;
    profile.icc_bytes = {1U, 2U, 3U};
    const LinearWorkingBuffer input{2, 1, {-0.5F, 0.0F, 0.25F, 0.5F, 1.0F, 2.0F}, profile, {}, {}, {}};
    ExposureParams params;
    params.black = -0.25;
    params.exposure_ev = 1.0;
    const auto gpu = engine.value().gpu_apply_exposure(input, params, CancellationToken{});
#ifdef __APPLE__
    const auto cpu = apply_exposure(input, params, CancellationToken{});
    ASSERT_TRUE(cpu) << cpu.error().message;
    ASSERT_TRUE(gpu) << gpu.error().message;
    ASSERT_EQ(gpu.value().rgb.size(), cpu.value().rgb.size());
    EXPECT_EQ(gpu.value().width, input.width);
    EXPECT_EQ(gpu.value().height, input.height);
    EXPECT_EQ(gpu.value().color_profile, input.color_profile);
    EXPECT_NE(gpu.value().rgb.data(), input.rgb.data());
    for (std::size_t index = 0; index < cpu.value().rgb.size(); ++index)
    {
        EXPECT_NEAR(gpu.value().rgb[index], cpu.value().rgb[index], 1.0e-5) << index;
    }
    EXPECT_EQ(input.rgb[0], -0.5F);
#else
    ASSERT_FALSE(gpu);
    EXPECT_EQ(gpu.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(gpu.error().context.at("reason"), "gpu_unavailable");
#endif
}

TEST(EngineFacadeTest, GpuApplyExposureHonorsCancellation)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    LinearWorkingBuffer input{1, 1, {0.25F, 0.5F, 0.75F}, {}, {}, {}, {}};
    input.color_profile.kind = ColorProfileKind::kBuiltin;
    input.color_profile.model = ColorModel::kRgb;
    input.color_profile.identifier = "linear-rec709";
    CancellationSource cancellation;
    ASSERT_TRUE(cancellation.cancel("gpu_exposure_cancel"));
    const auto gpu =
        engine.value().gpu_apply_exposure(input, ExposureParams{}, cancellation.token());
    ASSERT_FALSE(gpu);
    EXPECT_EQ(gpu.error().code, ErrorCode::kCancelled);
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
