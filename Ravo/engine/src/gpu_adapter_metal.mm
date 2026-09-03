#include "gpu_adapter.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>

namespace ravo
{

namespace
{

constexpr const char *kCopyLibrary = R"(
#include <metal_stdlib>
using namespace metal;
kernel void copy_rgb(device const float *input [[buffer(0)]],
                     device float *output [[buffer(1)]],
                     uint index [[thread_position_in_grid]])
{
    output[index] = input[index];
}
)";

} // namespace

struct GpuAdapter::Impl
{
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLComputePipelineState> copy_pipeline = nil;
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
        NSString *source = [NSString stringWithUTF8String:kCopyLibrary];
        id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
        if (library == nil)
        {
            return make_error(ErrorCode::kIo, "Metal library compilation failed",
                              {{"reason", "gpu_pipeline_failed"},
                               {"detail", error != nil ? error.localizedDescription.UTF8String : ""}});
        }
        id<MTLFunction> function = [library newFunctionWithName:@"copy_rgb"];
        if (function == nil)
        {
            return make_error(ErrorCode::kIo, "Metal copy kernel is missing",
                              {{"reason", "gpu_pipeline_failed"}});
        }
        id<MTLComputePipelineState> pipeline =
            [device newComputePipelineStateWithFunction:function error:&error];
        if (pipeline == nil)
        {
            return make_error(ErrorCode::kIo, "Metal pipeline creation failed",
                              {{"reason", "gpu_pipeline_failed"},
                               {"detail", error != nil ? error.localizedDescription.UTF8String : ""}});
        }
        auto impl = std::make_unique<Impl>();
        impl->device = device;
        impl->queue = queue;
        impl->copy_pipeline = pipeline;
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
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    if (impl_ == nullptr || impl_->device == nil || impl_->queue == nil ||
        impl_->copy_pipeline == nil)
    {
        return make_error(ErrorCode::kUnsupported, "GPU adapter is not initialized",
                          {{"reason", "gpu_unavailable"}});
    }
    if (input.size() == 0U || input.size() != output.size())
    {
        return make_error(ErrorCode::kInvalidArgument, "GPU copy buffers must be the same size",
                          {{"reason", "gpu_copy_size_mismatch"}});
    }
    if (input.size() > std::numeric_limits<NSUInteger>::max() / sizeof(float))
    {
        return make_error(ErrorCode::kInvalidArgument, "GPU copy buffer is too large",
                          {{"reason", "gpu_copy_size_mismatch"}});
    }
    @autoreleasepool
    {
        const NSUInteger bytes = static_cast<NSUInteger>(input.size() * sizeof(float));
        id<MTLBuffer> source = [impl_->device newBufferWithBytes:input.data()
                                                          length:bytes
                                                         options:MTLResourceStorageModeShared];
        id<MTLBuffer> destination = [impl_->device newBufferWithLength:bytes
                                                               options:MTLResourceStorageModeShared];
        if (source == nil || destination == nil)
        {
            return make_error(ErrorCode::kIo, "Metal buffer allocation failed",
                              {{"reason", "gpu_pipeline_failed"}});
        }
        id<MTLCommandBuffer> command = [impl_->queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        if (command == nil || encoder == nil)
        {
            return make_error(ErrorCode::kIo, "Metal command encoder failed",
                              {{"reason", "gpu_pipeline_failed"}});
        }
        [encoder setComputePipelineState:impl_->copy_pipeline];
        [encoder setBuffer:source offset:0 atIndex:0];
        [encoder setBuffer:destination offset:0 atIndex:1];
        const NSUInteger count = static_cast<NSUInteger>(input.size());
        const NSUInteger group = std::min(impl_->copy_pipeline.maxTotalThreadsPerThreadgroup, count);
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
            return make_error(ErrorCode::kIo, "Metal copy command failed",
                              {{"reason", "gpu_pipeline_failed"}});
        }
        std::memcpy(output.data(), [destination contents], bytes);
        return {};
    }
}

} // namespace ravo
