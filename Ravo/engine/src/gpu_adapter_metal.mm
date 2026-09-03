#include "gpu_adapter.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <utility>

namespace ravo
{

namespace
{

constexpr const char *kLibrarySource = R"(
#include <metal_stdlib>
using namespace metal;
kernel void copy_rgb(device const float *input [[buffer(0)]],
                     device float *output [[buffer(1)]],
                     uint index [[thread_position_in_grid]])
{
    output[index] = input[index];
}
kernel void affine_rgb(device const float *input [[buffer(0)]],
                       device float *output [[buffer(1)]],
                       constant float2 &affine [[buffer(2)]],
                       uint index [[thread_position_in_grid]])
{
    output[index] = (input[index] - affine.y) * affine.x;
}
)";

[[nodiscard]] TaskError metal_error(std::string message, NSError *error)
{
    std::map<std::string, std::string, std::less<>> context{{"reason", "gpu_pipeline_failed"}};
    if (error != nil && error.localizedDescription.UTF8String != nullptr)
    {
        context.emplace("detail", error.localizedDescription.UTF8String);
    }
    return make_error(ErrorCode::kIo, std::move(message), std::move(context));
}

} // namespace

struct GpuAdapter::Impl
{
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLComputePipelineState> copy_pipeline = nil;
    id<MTLComputePipelineState> affine_pipeline = nil;

    [[nodiscard]] Result<id<MTLComputePipelineState>> pipeline(id<MTLLibrary> library,
                                                               NSString *name)
    {
        NSError *error = nil;
        id<MTLFunction> function = [library newFunctionWithName:name];
        if (function == nil)
        {
            return make_error(ErrorCode::kIo, "Metal kernel is missing",
                              {{"reason", "gpu_pipeline_failed"},
                               {"kernel", name.UTF8String != nullptr ? name.UTF8String : ""}});
        }
        id<MTLComputePipelineState> state =
            [device newComputePipelineStateWithFunction:function error:&error];
        if (state == nil)
        {
            return metal_error("Metal pipeline creation failed", error);
        }
        return state;
    }

    [[nodiscard]] Result<void> dispatch(id<MTLComputePipelineState> pipeline,
                                        std::span<const float> input, std::span<float> output,
                                        const void *constants, const NSUInteger constant_bytes,
                                        const CancellationToken &cancellation) const
    {
        auto cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        if (device == nil || queue == nil || pipeline == nil)
        {
            return make_error(ErrorCode::kUnsupported, "GPU adapter is not initialized",
                              {{"reason", "gpu_unavailable"}});
        }
        if (input.size() == 0U || input.size() != output.size())
        {
            return make_error(ErrorCode::kInvalidArgument, "GPU buffers must be the same size",
                              {{"reason", "gpu_copy_size_mismatch"}});
        }
        if (input.size() > std::numeric_limits<NSUInteger>::max() / sizeof(float))
        {
            return make_error(ErrorCode::kInvalidArgument, "GPU buffer is too large",
                              {{"reason", "gpu_copy_size_mismatch"}});
        }
        @autoreleasepool
        {
            const NSUInteger bytes = static_cast<NSUInteger>(input.size() * sizeof(float));
            id<MTLBuffer> source = [device newBufferWithBytes:input.data()
                                                       length:bytes
                                                      options:MTLResourceStorageModeShared];
            id<MTLBuffer> destination = [device newBufferWithLength:bytes
                                                            options:MTLResourceStorageModeShared];
            id<MTLBuffer> constant_buffer = nil;
            if (constants != nullptr && constant_bytes > 0U)
            {
                constant_buffer = [device newBufferWithBytes:constants
                                                      length:constant_bytes
                                                     options:MTLResourceStorageModeShared];
            }
            if (source == nil || destination == nil ||
                (constants != nullptr && constant_buffer == nil))
            {
                return make_error(ErrorCode::kIo, "Metal buffer allocation failed",
                                  {{"reason", "gpu_pipeline_failed"}});
            }
            id<MTLCommandBuffer> command = [queue commandBuffer];
            id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
            if (command == nil || encoder == nil)
            {
                return make_error(ErrorCode::kIo, "Metal command encoder failed",
                                  {{"reason", "gpu_pipeline_failed"}});
            }
            [encoder setComputePipelineState:pipeline];
            [encoder setBuffer:source offset:0 atIndex:0];
            [encoder setBuffer:destination offset:0 atIndex:1];
            if (constant_buffer != nil)
            {
                [encoder setBuffer:constant_buffer offset:0 atIndex:2];
            }
            const NSUInteger count = static_cast<NSUInteger>(input.size());
            const NSUInteger group = std::min(pipeline.maxTotalThreadsPerThreadgroup, count);
            if (group == 0U)
            {
                return make_error(ErrorCode::kIo, "Metal threadgroup size is zero",
                                  {{"reason", "gpu_pipeline_failed"}});
            }
            [encoder dispatchThreads:MTLSizeMake(count, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(group, 1, 1)];
            [encoder endEncoding];
            [command commit];
            [command waitUntilCompleted];
            cancelled = cancellation.check();
            if (!cancelled)
            {
                return cancelled.error();
            }
            if (command.status != MTLCommandBufferStatusCompleted)
            {
                return make_error(ErrorCode::kIo, "Metal command failed",
                                  {{"reason", "gpu_pipeline_failed"}});
            }
            std::memcpy(output.data(), [destination contents], bytes);
            return {};
        }
    }
};

GpuAdapter::GpuAdapter(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
GpuAdapter::~GpuAdapter() = default;
GpuAdapter::GpuAdapter(GpuAdapter &&) noexcept = default;
GpuAdapter &GpuAdapter::operator=(GpuAdapter &&) noexcept = default;

Result<std::shared_ptr<GpuAdapter>> GpuAdapter::try_create()
{
    @autoreleasepool
    {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil)
        {
            return make_error(ErrorCode::kUnsupported, "Metal device is unavailable",
                              {{"reason", "gpu_unavailable"}});
        }
        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (queue == nil)
        {
            return make_error(ErrorCode::kIo, "Metal command queue failed",
                              {{"reason", "gpu_pipeline_failed"}});
        }
        NSError *error = nil;
        NSString *source = [NSString stringWithUTF8String:kLibrarySource];
        id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
        if (library == nil)
        {
            return metal_error("Metal library compilation failed", error);
        }
        auto impl = std::make_unique<Impl>();
        impl->device = device;
        impl->queue = queue;
        auto copy = impl->pipeline(library, @"copy_rgb");
        if (!copy)
        {
            return copy.error();
        }
        auto affine = impl->pipeline(library, @"affine_rgb");
        if (!affine)
        {
            return affine.error();
        }
        impl->copy_pipeline = copy.value();
        impl->affine_pipeline = affine.value();
        return std::shared_ptr<GpuAdapter>(new GpuAdapter(std::move(impl)));
    }
}

std::string_view GpuAdapter::backend_id() const noexcept
{
    return "metal";
}

Result<void> GpuAdapter::copy_rgb(std::span<const float> input, std::span<float> output,
                                  const CancellationToken &cancellation) const
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kUnsupported, "GPU adapter is not initialized",
                          {{"reason", "gpu_unavailable"}});
    }
    return impl_->dispatch(impl_->copy_pipeline, input, output, nullptr, 0U, cancellation);
}

Result<void> GpuAdapter::apply_affine_rgb(std::span<const float> input, std::span<float> output,
                                          const float scale, const float black,
                                          const CancellationToken &cancellation) const
{
    if (!std::isfinite(scale) || !std::isfinite(black))
    {
        return make_error(ErrorCode::kInvalidArgument, "GPU affine parameters must be finite",
                          {{"reason", "gpu_affine_non_finite"}});
    }
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kUnsupported, "GPU adapter is not initialized",
                          {{"reason", "gpu_unavailable"}});
    }
    const float affine[2] = {scale, black};
    return impl_->dispatch(impl_->affine_pipeline, input, output, affine, sizeof(affine),
                           cancellation);
}

} // namespace ravo
