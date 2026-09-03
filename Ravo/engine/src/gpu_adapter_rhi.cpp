#include "gpu_adapter.h"

#include <QByteArray>
#include <QCoreApplication>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

extern unsigned char const ravo_gpu_affine_rgb_qsb[];
extern unsigned long long const ravo_gpu_affine_rgb_qsb_size;
extern unsigned char const ravo_gpu_sigmoid_rgb_qsb[];
extern unsigned long long const ravo_gpu_sigmoid_rgb_qsb_size;

namespace ravo
{

namespace
{

constexpr quint32 kWorkgroup = 256U;

void ensure_core_application()
{
    if (QCoreApplication::instance() != nullptr)
    {
        return;
    }
    static int argc = 1;
    static char arg0[] = "ravo-engine-gpu";
    static char *argv[] = {arg0, nullptr};
    new QCoreApplication(argc, argv);
}

[[nodiscard]] std::string_view backend_id_for(const QRhi::Implementation implementation) noexcept
{
    switch (implementation)
    {
    case QRhi::Metal:
        return "metal";
    case QRhi::D3D12:
        return "d3d12";
    case QRhi::D3D11:
        return "d3d11";
    case QRhi::Vulkan:
        return "vulkan";
    case QRhi::OpenGLES2:
        return "opengl";
    case QRhi::Null:
        return "unavailable";
    }
    return "rhi";
}

struct AffineUniforms
{
    quint32 count = 0;
    quint32 pad0[3] = {};
    float scale = 1.0F;
    float black = 0.0F;
    float pad1[2] = {};
};

struct SigmoidUniforms
{
    quint32 pixel_count = 0;
    quint32 mode = 0;
    quint32 pad0[2] = {};
    float white_target = 1.0F;
    float black_target = 0.000152F;
    float paper_exposure = 1.0F;
    float film_fog = 0.0F;
    float film_power = 1.0F;
    float paper_power = 1.0F;
    float hue_preservation = 1.0F;
    float pad1 = 0.0F;
};

[[nodiscard]] Result<QShader> load_shader(const unsigned char *bytes, const unsigned long long size)
{
    if (bytes == nullptr || size == 0ULL ||
        size > static_cast<unsigned long long>(std::numeric_limits<int>::max()))
    {
        return make_error(ErrorCode::kIo, "GPU compute shader is invalid",
                          {{"reason", "gpu_pipeline_failed"}});
    }
    const auto shader = QShader::fromSerialized(
        QByteArray::fromRawData(reinterpret_cast<const char *>(bytes), static_cast<int>(size)));
    if (!shader.isValid())
    {
        return make_error(ErrorCode::kIo, "GPU compute shader is invalid",
                          {{"reason", "gpu_pipeline_failed"}});
    }
    return shader;
}

} // namespace

struct GpuAdapter::Impl
{
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
    std::unique_ptr<QVulkanInstance> vulkan;
#endif
    std::unique_ptr<QRhi> rhi;
    std::unique_ptr<QRhiBuffer> affine_uniforms;
    std::unique_ptr<QRhiBuffer> sigmoid_uniforms;
    std::unique_ptr<QRhiShaderResourceBindings> layout_bindings;
    std::unique_ptr<QRhiComputePipeline> affine_pipeline;
    std::unique_ptr<QRhiComputePipeline> sigmoid_pipeline;
    std::string_view backend;
    mutable std::mutex mutex;

    [[nodiscard]] Result<void> open();
    [[nodiscard]] Result<std::unique_ptr<QRhiComputePipeline>>
    make_pipeline(const QShader &shader);
    [[nodiscard]] Result<void> apply_passes(std::span<const float> input, std::span<float> output,
                                            std::span<const GpuRgbPass> passes,
                                            const CancellationToken &cancellation) const;
};

Result<std::unique_ptr<QRhiComputePipeline>> GpuAdapter::Impl::make_pipeline(const QShader &shader)
{
    std::unique_ptr<QRhiComputePipeline> pipeline(rhi->newComputePipeline());
    if (pipeline == nullptr)
    {
        return make_error(ErrorCode::kIo, "GPU pipeline allocation failed",
                          {{"reason", "gpu_pipeline_failed"}});
    }
    pipeline->setShaderStage({QRhiShaderStage::Compute, shader});
    pipeline->setShaderResourceBindings(layout_bindings.get());
    if (!pipeline->create())
    {
        return make_error(ErrorCode::kIo, "GPU compute pipeline failed",
                          {{"reason", "gpu_pipeline_failed"}});
    }
    return pipeline;
}

Result<void> GpuAdapter::Impl::open()
{
    ensure_core_application();
    std::unique_ptr<QRhi> created;
#if defined(Q_OS_MACOS) || defined(Q_OS_IOS)
    QRhiMetalInitParams metal;
    created.reset(QRhi::create(QRhi::Metal, &metal));
#elif defined(Q_OS_WIN)
    QRhiD3D12InitParams d3d12;
    created.reset(QRhi::create(QRhi::D3D12, &d3d12));
    if (created == nullptr)
    {
        QRhiD3D11InitParams d3d11;
        created.reset(QRhi::create(QRhi::D3D11, &d3d11));
    }
#else
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
    vulkan = std::make_unique<QVulkanInstance>();
    vulkan->setExtensions(QRhiVulkanInitParams::preferredInstanceExtensions());
    if (vulkan->create())
    {
        QRhiVulkanInitParams vulkan_params;
        vulkan_params.inst = vulkan.get();
        created.reset(QRhi::create(QRhi::Vulkan, &vulkan_params));
    }
    if (created == nullptr)
    {
        vulkan.reset();
    }
#endif
#endif
    if (created == nullptr)
    {
        return make_error(ErrorCode::kUnsupported, "QRhi device is unavailable",
                          {{"reason", "gpu_unavailable"}});
    }
    if (!created->isFeatureSupported(QRhi::Compute) ||
        !created->isFeatureSupported(QRhi::ReadBackNonUniformBuffer))
    {
        return make_error(ErrorCode::kUnsupported, "QRhi compute readback is unavailable",
                          {{"reason", "gpu_unavailable"}});
    }
    auto affine_shader = load_shader(ravo_gpu_affine_rgb_qsb, ravo_gpu_affine_rgb_qsb_size);
    if (!affine_shader)
    {
        return affine_shader.error();
    }
    auto sigmoid_shader = load_shader(ravo_gpu_sigmoid_rgb_qsb, ravo_gpu_sigmoid_rgb_qsb_size);
    if (!sigmoid_shader)
    {
        return sigmoid_shader.error();
    }
    const auto aligned = created->ubufAlignment();
    constexpr auto kUniformBytes = sizeof(SigmoidUniforms);
    if (aligned <= 0 || static_cast<unsigned int>(aligned) < kUniformBytes)
    {
        return make_error(ErrorCode::kIo, "GPU uniform alignment is invalid",
                          {{"reason", "gpu_pipeline_failed"}});
    }
    rhi = std::move(created);
    affine_uniforms.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                                         static_cast<quint32>(aligned)));
    sigmoid_uniforms.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                                          static_cast<quint32>(aligned)));
    layout_bindings.reset(rhi->newShaderResourceBindings());
    if (affine_uniforms == nullptr || sigmoid_uniforms == nullptr || layout_bindings == nullptr ||
        !affine_uniforms->create() || !sigmoid_uniforms->create())
    {
        return make_error(ErrorCode::kIo, "GPU pipeline allocation failed",
                          {{"reason", "gpu_pipeline_failed"}});
    }
    layout_bindings->setBindings({
        QRhiShaderResourceBinding::bufferLoadStore(0, QRhiShaderResourceBinding::ComputeStage,
                                                   nullptr),
        QRhiShaderResourceBinding::uniformBuffer(1, QRhiShaderResourceBinding::ComputeStage,
                                                 affine_uniforms.get()),
    });
    if (!layout_bindings->create())
    {
        return make_error(ErrorCode::kIo, "GPU resource layout failed",
                          {{"reason", "gpu_pipeline_failed"}});
    }
    auto affine = make_pipeline(affine_shader.value());
    if (!affine)
    {
        return affine.error();
    }
    auto sigmoid = make_pipeline(sigmoid_shader.value());
    if (!sigmoid)
    {
        return sigmoid.error();
    }
    affine_pipeline = std::move(affine).value();
    sigmoid_pipeline = std::move(sigmoid).value();
    backend = backend_id_for(rhi->backend());
    return {};
}

Result<void> GpuAdapter::Impl::apply_passes(const std::span<const float> input,
                                            const std::span<float> output,
                                            const std::span<const GpuRgbPass> passes,
                                            const CancellationToken &cancellation) const
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    if (rhi == nullptr || affine_uniforms == nullptr || sigmoid_uniforms == nullptr ||
        affine_pipeline == nullptr || sigmoid_pipeline == nullptr)
    {
        return make_error(ErrorCode::kUnsupported, "GPU adapter is not initialized",
                          {{"reason", "gpu_unavailable"}});
    }
    if (passes.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "GPU RGB pass list is empty",
                          {{"reason", "gpu_copy_size_mismatch"}});
    }
    if (input.size() == 0U || input.size() != output.size())
    {
        return make_error(ErrorCode::kInvalidArgument, "GPU buffers must be the same size",
                          {{"reason", "gpu_copy_size_mismatch"}});
    }
    bool needs_pixels = false;
    for (const auto &pass : passes)
    {
        if (pass.kind == GpuRgbPass::Kind::kSigmoid)
        {
            needs_pixels = true;
            break;
        }
    }
    if (needs_pixels && input.size() % 3U != 0U)
    {
        return make_error(ErrorCode::kInvalidArgument, "GPU RGB buffers must be packed RGB",
                          {{"reason", "gpu_copy_size_mismatch"}});
    }
    if (input.size() > std::numeric_limits<quint32>::max() / sizeof(float))
    {
        return make_error(ErrorCode::kInvalidArgument, "GPU buffer is too large",
                          {{"reason", "gpu_copy_size_mismatch"}});
    }
    const auto count = static_cast<quint32>(input.size());
    const auto pixels = count / 3U;
    const auto bytes = count * static_cast<quint32>(sizeof(float));
    std::lock_guard<std::mutex> lock(mutex);
    cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    if (rhi->isDeviceLost())
    {
        return make_error(ErrorCode::kIo, "GPU device was lost", {{"reason", "gpu_pipeline_failed"}});
    }
    std::unique_ptr<QRhiBuffer> buffer(
        rhi->newBuffer(QRhiBuffer::Static, QRhiBuffer::StorageBuffer, bytes));
    if (buffer == nullptr || !buffer->create())
    {
        return make_error(ErrorCode::kIo, "GPU buffer allocation failed",
                          {{"reason", "gpu_pipeline_failed"}});
    }
    QRhiCommandBuffer *command = nullptr;
    const auto began = rhi->beginOffscreenFrame(&command);
    if (began != QRhi::FrameOpSuccess || command == nullptr)
    {
        return make_error(ErrorCode::kIo, "GPU frame begin failed",
                          {{"reason", "gpu_pipeline_failed"}});
    }
    QRhiReadbackResult readback;
    for (std::size_t pass_index = 0; pass_index < passes.size(); ++pass_index)
    {
        cancelled = cancellation.check();
        if (!cancelled)
        {
            rhi->endOffscreenFrame();
            return cancelled.error();
        }
        const auto &pass = passes[pass_index];
        QRhiComputePipeline *pipeline = nullptr;
        quint32 groups = 0;
        QRhiResourceUpdateBatch *updates = rhi->nextResourceUpdateBatch();
        if (pass_index == 0U)
        {
            updates->uploadStaticBuffer(buffer.get(), input.data());
        }
        if (pass.kind == GpuRgbPass::Kind::kAffine)
        {
            if (!std::isfinite(pass.affine.scale) || !std::isfinite(pass.affine.black))
            {
                rhi->endOffscreenFrame();
                return make_error(ErrorCode::kInvalidArgument, "GPU affine parameters must be finite",
                                  {{"reason", "gpu_affine_non_finite"}});
            }
            AffineUniforms params;
            params.count = count;
            params.scale = pass.affine.scale;
            params.black = pass.affine.black;
            updates->updateDynamicBuffer(affine_uniforms.get(), 0, sizeof(params), &params);
            pipeline = affine_pipeline.get();
            groups = (count + kWorkgroup - 1U) / kWorkgroup;
        }
        else
        {
            SigmoidUniforms params;
            params.pixel_count = pixels;
            params.mode = pass.sigmoid.mode;
            params.white_target = pass.sigmoid.white_target;
            params.black_target = pass.sigmoid.black_target;
            params.paper_exposure = pass.sigmoid.paper_exposure;
            params.film_fog = pass.sigmoid.film_fog;
            params.film_power = pass.sigmoid.film_power;
            params.paper_power = pass.sigmoid.paper_power;
            params.hue_preservation = pass.sigmoid.hue_preservation;
            updates->updateDynamicBuffer(sigmoid_uniforms.get(), 0, sizeof(params), &params);
            pipeline = sigmoid_pipeline.get();
            groups = (pixels + kWorkgroup - 1U) / kWorkgroup;
        }
        if (groups == 0U || groups > static_cast<quint32>(std::numeric_limits<int>::max()) ||
            pipeline == nullptr)
        {
            rhi->endOffscreenFrame();
            return make_error(ErrorCode::kInvalidArgument, "GPU dispatch is too large",
                              {{"reason", "gpu_copy_size_mismatch"}});
        }
        std::unique_ptr<QRhiShaderResourceBindings> bindings(rhi->newShaderResourceBindings());
        if (bindings == nullptr)
        {
            rhi->endOffscreenFrame();
            return make_error(ErrorCode::kIo, "GPU resource bindings failed",
                              {{"reason", "gpu_pipeline_failed"}});
        }
        bindings->setBindings({
            QRhiShaderResourceBinding::bufferLoadStore(0, QRhiShaderResourceBinding::ComputeStage,
                                                       buffer.get()),
            QRhiShaderResourceBinding::uniformBuffer(
                1, QRhiShaderResourceBinding::ComputeStage,
                pass.kind == GpuRgbPass::Kind::kAffine ? affine_uniforms.get() :
                                                         sigmoid_uniforms.get()),
        });
        if (!bindings->create())
        {
            rhi->endOffscreenFrame();
            return make_error(ErrorCode::kIo, "GPU resource bindings failed",
                              {{"reason", "gpu_pipeline_failed"}});
        }
        command->beginComputePass(updates);
        command->setComputePipeline(pipeline);
        command->setShaderResources(bindings.get());
        command->dispatch(static_cast<int>(groups), 1, 1);
        if (pass_index + 1U == passes.size())
        {
            QRhiResourceUpdateBatch *download = rhi->nextResourceUpdateBatch();
            download->readBackBuffer(buffer.get(), 0, bytes, &readback);
            command->endComputePass(download);
        }
        else
        {
            command->endComputePass();
        }
    }
    const auto ended = rhi->endOffscreenFrame();
    cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    if (ended != QRhi::FrameOpSuccess)
    {
        return make_error(ErrorCode::kIo, "GPU frame end failed", {{"reason", "gpu_pipeline_failed"}});
    }
    if (static_cast<quint32>(readback.data.size()) != bytes)
    {
        return make_error(ErrorCode::kIo, "GPU readback size mismatch",
                          {{"reason", "gpu_pipeline_failed"}});
    }
    std::memcpy(output.data(), readback.data.constData(), bytes);
    return {};
}

GpuAdapter::GpuAdapter(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
GpuAdapter::~GpuAdapter() = default;
GpuAdapter::GpuAdapter(GpuAdapter &&) noexcept = default;
GpuAdapter &GpuAdapter::operator=(GpuAdapter &&) noexcept = default;

Result<std::shared_ptr<GpuAdapter>> GpuAdapter::try_create()
{
    auto impl = std::make_unique<Impl>();
    auto opened = impl->open();
    if (!opened)
    {
        return opened.error();
    }
    return std::shared_ptr<GpuAdapter>(new GpuAdapter(std::move(impl)));
}

std::string_view GpuAdapter::backend_id() const noexcept
{
    return impl_ != nullptr ? impl_->backend : std::string_view{"unavailable"};
}

Result<void> GpuAdapter::copy_rgb(const std::span<const float> input, const std::span<float> output,
                                  const CancellationToken &cancellation) const
{
    GpuRgbPass pass;
    pass.kind = GpuRgbPass::Kind::kAffine;
    pass.affine.scale = 1.0F;
    pass.affine.black = 0.0F;
    return apply_rgb_passes(input, output, std::span<const GpuRgbPass>(&pass, 1U), cancellation);
}

Result<void> GpuAdapter::apply_affine_rgb(const std::span<const float> input,
                                          const std::span<float> output, const float scale,
                                          const float black,
                                          const CancellationToken &cancellation) const
{
    GpuRgbPass pass;
    pass.kind = GpuRgbPass::Kind::kAffine;
    pass.affine.scale = scale;
    pass.affine.black = black;
    return apply_rgb_passes(input, output, std::span<const GpuRgbPass>(&pass, 1U), cancellation);
}

Result<void> GpuAdapter::apply_rgb_passes(const std::span<const float> input,
                                          const std::span<float> output,
                                          const std::span<const GpuRgbPass> passes,
                                          const CancellationToken &cancellation) const
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kUnsupported, "GPU adapter is not initialized",
                          {{"reason", "gpu_unavailable"}});
    }
    return impl_->apply_passes(input, output, passes, cancellation);
}

} // namespace ravo
