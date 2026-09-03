#include "gpu_adapter.h"

#include <QByteArray>
#include <QCoreApplication>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include <algorithm>
#include <array>
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
extern unsigned char const ravo_gpu_light_controls_qsb[];
extern unsigned long long const ravo_gpu_light_controls_qsb_size;
extern unsigned char const ravo_gpu_sharpen_lab_qsb[];
extern unsigned long long const ravo_gpu_sharpen_lab_qsb_size;
extern unsigned char const ravo_gpu_rcd_demosaic_qsb[];
extern unsigned long long const ravo_gpu_rcd_demosaic_qsb_size;

namespace ravo
{

namespace
{

constexpr quint32 kWorkgroup = 256U;
constexpr quint32 kDemosaicGroup = 16U;

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

struct LightUniforms
{
    quint32 pixel_count = 0;
    quint32 pad0[3] = {};
    float highlight_ev = 0.0F;
    float shadow_ev = 0.0F;
    float white_ev = 0.0F;
    float black_ev = 0.0F;
};

struct SharpenUniforms
{
    quint32 width = 0;
    quint32 height = 0;
    quint32 radius = 0;
    quint32 stage = 0;
    float amount = 0.5F;
    float threshold = 0.5F;
    float pad0[2] = {};
    float kernel[28] = {};
};

static_assert(sizeof(LightUniforms) == 32U, "light UBO must match std140 vec4 packing");
static_assert(sizeof(SharpenUniforms) == 144U, "sharpen UBO must match std140 kernel[7]");

struct RcdUniforms
{
    quint32 width = 0;
    quint32 height = 0;
    quint32 stage = 0;
    quint32 pad = 0;
    quint32 pattern[4] = {};
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
    std::unique_ptr<QRhiBuffer> light_uniforms[4];
    std::unique_ptr<QRhiBuffer> sharpen_uniforms[4];
    std::unique_ptr<QRhiBuffer> rcd_uniforms[9];
    std::unique_ptr<QRhiShaderResourceBindings> layout_bindings;
    std::unique_ptr<QRhiShaderResourceBindings> sharpen_layout;
    std::unique_ptr<QRhiShaderResourceBindings> rcd_layout;
    std::unique_ptr<QRhiComputePipeline> affine_pipeline;
    std::unique_ptr<QRhiComputePipeline> sigmoid_pipeline;
    std::unique_ptr<QRhiComputePipeline> light_pipeline;
    std::unique_ptr<QRhiComputePipeline> sharpen_pipeline;
    std::unique_ptr<QRhiComputePipeline> rcd_pipeline;
    std::string_view backend;
    mutable std::mutex mutex;

    [[nodiscard]] Result<void> open();
    [[nodiscard]] Result<std::unique_ptr<QRhiComputePipeline>>
    make_pipeline(const QShader &shader, QRhiShaderResourceBindings *bindings);
    [[nodiscard]] Result<void> apply_passes(std::span<const float> input, std::span<float> output,
                                            std::span<const GpuRgbPass> passes,
                                            const CancellationToken &cancellation) const;
    [[nodiscard]] Result<void> demosaic_rcd(std::span<const float> cfa, std::span<float> rgb,
                                            std::uint32_t width, std::uint32_t height,
                                            std::array<std::uint8_t, 4> pattern,
                                            const CancellationToken &cancellation) const;
};

Result<std::unique_ptr<QRhiComputePipeline>>
GpuAdapter::Impl::make_pipeline(const QShader &shader, QRhiShaderResourceBindings *bindings)
{
    std::unique_ptr<QRhiComputePipeline> pipeline(rhi->newComputePipeline());
    if (pipeline == nullptr)
    {
        return make_error(ErrorCode::kIo, "GPU pipeline allocation failed",
                          {{"reason", "gpu_pipeline_failed"}});
    }
    pipeline->setShaderStage({QRhiShaderStage::Compute, shader});
    pipeline->setShaderResourceBindings(bindings);
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
    auto light_shader = load_shader(ravo_gpu_light_controls_qsb, ravo_gpu_light_controls_qsb_size);
    if (!light_shader)
    {
        return light_shader.error();
    }
    auto sharpen_shader = load_shader(ravo_gpu_sharpen_lab_qsb, ravo_gpu_sharpen_lab_qsb_size);
    if (!sharpen_shader)
    {
        return sharpen_shader.error();
    }
    const auto aligned = created->ubufAlignment();
    if (aligned <= 0)
    {
        return make_error(ErrorCode::kIo, "GPU uniform alignment is invalid",
                          {{"reason", "gpu_pipeline_failed"}});
    }
    const auto ubo_bytes = std::max(static_cast<quint32>(aligned),
                                    static_cast<quint32>(sizeof(SharpenUniforms)));
    const auto ubo_aligned =
        ((ubo_bytes + static_cast<quint32>(aligned) - 1U) / static_cast<quint32>(aligned)) *
        static_cast<quint32>(aligned);
    rhi = std::move(created);
    affine_uniforms.reset(
        rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, ubo_aligned));
    sigmoid_uniforms.reset(
        rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, ubo_aligned));
    layout_bindings.reset(rhi->newShaderResourceBindings());
    sharpen_layout.reset(rhi->newShaderResourceBindings());
    bool light_ubos = true;
    for (auto &ubo : light_uniforms)
    {
        ubo.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, ubo_aligned));
        if (ubo == nullptr || !ubo->create())
        {
            light_ubos = false;
            break;
        }
    }
    bool sharpen_ubos = true;
    for (auto &ubo : sharpen_uniforms)
    {
        ubo.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, ubo_aligned));
        if (ubo == nullptr || !ubo->create())
        {
            sharpen_ubos = false;
            break;
        }
    }
    if (affine_uniforms == nullptr || sigmoid_uniforms == nullptr ||
        layout_bindings == nullptr || sharpen_layout == nullptr || !light_ubos || !sharpen_ubos ||
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
    sharpen_layout->setBindings({
        QRhiShaderResourceBinding::bufferLoadStore(0, QRhiShaderResourceBinding::ComputeStage,
                                                   nullptr),
        QRhiShaderResourceBinding::bufferLoadStore(1, QRhiShaderResourceBinding::ComputeStage,
                                                   nullptr),
        QRhiShaderResourceBinding::bufferLoadStore(2, QRhiShaderResourceBinding::ComputeStage,
                                                   nullptr),
        QRhiShaderResourceBinding::uniformBuffer(3, QRhiShaderResourceBinding::ComputeStage,
                                                 sharpen_uniforms[0].get()),
    });
    if (!sharpen_layout->create())
    {
        return make_error(ErrorCode::kIo, "GPU resource layout failed",
                          {{"reason", "gpu_pipeline_failed"}});
    }
    auto affine = make_pipeline(affine_shader.value(), layout_bindings.get());
    if (!affine)
    {
        return affine.error();
    }
    auto sigmoid = make_pipeline(sigmoid_shader.value(), layout_bindings.get());
    if (!sigmoid)
    {
        return sigmoid.error();
    }
    auto light = make_pipeline(light_shader.value(), layout_bindings.get());
    if (!light)
    {
        return light.error();
    }
    auto sharpen = make_pipeline(sharpen_shader.value(), sharpen_layout.get());
    if (!sharpen)
    {
        return sharpen.error();
    }
    affine_pipeline = std::move(affine).value();
    sigmoid_pipeline = std::move(sigmoid).value();
    light_pipeline = std::move(light).value();
    sharpen_pipeline = std::move(sharpen).value();
    auto rcd_shader = load_shader(ravo_gpu_rcd_demosaic_qsb, ravo_gpu_rcd_demosaic_qsb_size);
    if (rcd_shader)
    {
        bool rcd_ready = true;
        rcd_layout.reset(rhi->newShaderResourceBindings());
        for (auto &ubo : rcd_uniforms)
        {
            ubo.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                                     static_cast<quint32>(aligned)));
            if (ubo == nullptr || !ubo->create())
            {
                rcd_ready = false;
                break;
            }
        }
        if (rcd_ready && rcd_layout != nullptr)
        {
            rcd_layout->setBindings({
                QRhiShaderResourceBinding::bufferLoadStore(
                    0, QRhiShaderResourceBinding::ComputeStage, nullptr),
                QRhiShaderResourceBinding::bufferLoadStore(
                    1, QRhiShaderResourceBinding::ComputeStage, nullptr),
                QRhiShaderResourceBinding::bufferLoadStore(
                    2, QRhiShaderResourceBinding::ComputeStage, nullptr),
                QRhiShaderResourceBinding::bufferLoadStore(
                    3, QRhiShaderResourceBinding::ComputeStage, nullptr),
                QRhiShaderResourceBinding::bufferLoadStore(
                    4, QRhiShaderResourceBinding::ComputeStage, nullptr),
                QRhiShaderResourceBinding::bufferLoadStore(
                    5, QRhiShaderResourceBinding::ComputeStage, nullptr),
                QRhiShaderResourceBinding::bufferLoadStore(
                    6, QRhiShaderResourceBinding::ComputeStage, nullptr),
                QRhiShaderResourceBinding::uniformBuffer(7, QRhiShaderResourceBinding::ComputeStage,
                                                         rcd_uniforms[0].get()),
            });
            if (rcd_layout->create())
            {
                auto rcd = make_pipeline(rcd_shader.value(), rcd_layout.get());
                if (rcd)
                {
                    rcd_pipeline = std::move(rcd).value();
                }
            }
        }
    }
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
        light_uniforms[0] == nullptr || affine_pipeline == nullptr || sigmoid_pipeline == nullptr ||
        light_pipeline == nullptr || sharpen_pipeline == nullptr)
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
    bool needs_sharpen = false;
    for (const auto &pass : passes)
    {
        if (pass.kind == GpuRgbPass::Kind::kAffine)
        {
            continue;
        }
        needs_pixels = true;
        if (pass.kind == GpuRgbPass::Kind::kSharpen)
        {
            needs_sharpen = true;
        }
        else if (pass.kind != GpuRgbPass::Kind::kSigmoid &&
                 pass.kind != GpuRgbPass::Kind::kLightControls)
        {
            return make_error(ErrorCode::kInvalidArgument, "GPU RGB pass kind is unsupported",
                              {{"reason", "gpu_pipeline_failed"}});
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
    const auto lab_bytes = pixels * 3U * static_cast<quint32>(sizeof(float));
    const auto blur_bytes = pixels * static_cast<quint32>(sizeof(float));
    std::lock_guard<std::mutex> lock(mutex);
    cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    if (rhi->isDeviceLost())
    {
        return make_error(ErrorCode::kIo, "GPU device was lost",
                          {{"reason", "gpu_pipeline_failed"}});
    }
    const auto make_storage = [&](const quint32 size) -> std::unique_ptr<QRhiBuffer>
    {
        std::unique_ptr<QRhiBuffer> created(
            rhi->newBuffer(QRhiBuffer::Static, QRhiBuffer::StorageBuffer, size));
        if (created == nullptr || !created->create())
        {
            return nullptr;
        }
        return created;
    };
    auto rgb_buffer = make_storage(bytes);
    std::unique_ptr<QRhiBuffer> lab_buffer;
    std::unique_ptr<QRhiBuffer> blur_buffer;
    if (needs_sharpen)
    {
        if (pixels == 0U)
        {
            return make_error(ErrorCode::kInvalidArgument, "GPU RGB buffers must be packed RGB",
                              {{"reason", "gpu_copy_size_mismatch"}});
        }
        lab_buffer = make_storage(lab_bytes);
        blur_buffer = make_storage(std::max(blur_bytes, 4U));
    }
    if (rgb_buffer == nullptr || (needs_sharpen && (lab_buffer == nullptr || blur_buffer == nullptr)))
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
    const auto fail_dispatch = [&]() -> Result<void>
    {
        rhi->endOffscreenFrame();
        return make_error(ErrorCode::kInvalidArgument, "GPU dispatch is too large",
                          {{"reason", "gpu_copy_size_mismatch"}});
    };
    const auto fail_bindings = [&]() -> Result<void>
    {
        rhi->endOffscreenFrame();
        return make_error(ErrorCode::kIo, "GPU resource bindings failed",
                          {{"reason", "gpu_pipeline_failed"}});
    };
    const auto bind_rgb = [&](QRhiBuffer *ubo) -> std::unique_ptr<QRhiShaderResourceBindings>
    {
        std::unique_ptr<QRhiShaderResourceBindings> bindings(rhi->newShaderResourceBindings());
        if (bindings == nullptr || ubo == nullptr)
        {
            return nullptr;
        }
        bindings->setBindings({
            QRhiShaderResourceBinding::bufferLoadStore(0, QRhiShaderResourceBinding::ComputeStage,
                                                       rgb_buffer.get()),
            QRhiShaderResourceBinding::uniformBuffer(1, QRhiShaderResourceBinding::ComputeStage,
                                                     ubo),
        });
        if (!bindings->create())
        {
            return nullptr;
        }
        return bindings;
    };
    const auto dispatch = [&](QRhiResourceUpdateBatch *updates, QRhiComputePipeline *pipeline,
                              QRhiShaderResourceBindings *bindings, const quint32 groups,
                              const bool last) -> Result<void>
    {
        if (groups == 0U || groups > static_cast<quint32>(std::numeric_limits<int>::max()) ||
            pipeline == nullptr || bindings == nullptr)
        {
            return fail_dispatch();
        }
        command->beginComputePass(updates);
        command->setComputePipeline(pipeline);
        command->setShaderResources(bindings);
        command->dispatch(static_cast<int>(groups), 1, 1);
        if (last)
        {
            QRhiResourceUpdateBatch *download = rhi->nextResourceUpdateBatch();
            download->readBackBuffer(rgb_buffer.get(), 0, bytes, &readback);
            command->endComputePass(download);
        }
        else
        {
            command->endComputePass();
        }
        return {};
    };
    const auto pixel_groups = (pixels + kWorkgroup - 1U) / kWorkgroup;
    const auto sample_groups = (count + kWorkgroup - 1U) / kWorkgroup;
    quint32 light_slot = 0;
    for (std::size_t pass_index = 0; pass_index < passes.size(); ++pass_index)
    {
        cancelled = cancellation.check();
        if (!cancelled)
        {
            rhi->endOffscreenFrame();
            return cancelled.error();
        }
        const auto &pass = passes[pass_index];
        const bool last_pass = pass_index + 1U == passes.size();
        if (pass.kind == GpuRgbPass::Kind::kSharpen)
        {
            if (pass.sharpen.width == 0U || pass.sharpen.height == 0U ||
                static_cast<std::uint64_t>(pass.sharpen.width) * pass.sharpen.height != pixels ||
                lab_buffer == nullptr || blur_buffer == nullptr || sharpen_uniforms[0] == nullptr)
            {
                rhi->endOffscreenFrame();
                return make_error(ErrorCode::kInvalidArgument, "GPU sharpen window is invalid",
                                  {{"reason", "gpu_copy_size_mismatch"}});
            }
            const quint32 stages = pass.sharpen.radius == 0U ? 2U : 4U;
            for (quint32 stage_index = 0; stage_index < stages; ++stage_index)
            {
                cancelled = cancellation.check();
                if (!cancelled)
                {
                    rhi->endOffscreenFrame();
                    return cancelled.error();
                }
                const quint32 stage = pass.sharpen.radius == 0U && stage_index == 1U ? 3U :
                                                                                      stage_index;
                SharpenUniforms params;
                params.width = pass.sharpen.width;
                params.height = pass.sharpen.height;
                params.radius = pass.sharpen.radius;
                params.stage = stage;
                params.amount = pass.sharpen.amount;
                params.threshold = pass.sharpen.threshold;
                std::memcpy(params.kernel, pass.sharpen.kernel.data(),
                            pass.sharpen.kernel.size() * sizeof(float));
                QRhiResourceUpdateBatch *updates = rhi->nextResourceUpdateBatch();
                if (pass_index == 0U && stage_index == 0U)
                {
                    updates->uploadStaticBuffer(rgb_buffer.get(), input.data());
                }
                updates->updateDynamicBuffer(sharpen_uniforms[stage].get(), 0, sizeof(params),
                                             &params);
                std::unique_ptr<QRhiShaderResourceBindings> bindings(
                    rhi->newShaderResourceBindings());
                if (bindings == nullptr)
                {
                    return fail_bindings();
                }
                bindings->setBindings({
                    QRhiShaderResourceBinding::bufferLoadStore(
                        0, QRhiShaderResourceBinding::ComputeStage, rgb_buffer.get()),
                    QRhiShaderResourceBinding::bufferLoadStore(
                        1, QRhiShaderResourceBinding::ComputeStage, lab_buffer.get()),
                    QRhiShaderResourceBinding::bufferLoadStore(
                        2, QRhiShaderResourceBinding::ComputeStage, blur_buffer.get()),
                    QRhiShaderResourceBinding::uniformBuffer(
                        3, QRhiShaderResourceBinding::ComputeStage, sharpen_uniforms[stage].get()),
                });
                if (!bindings->create())
                {
                    return fail_bindings();
                }
                const bool last = last_pass && stage_index + 1U == stages;
                auto dispatched = dispatch(updates, sharpen_pipeline.get(), bindings.get(),
                                           pixel_groups, last);
                if (!dispatched)
                {
                    return dispatched.error();
                }
            }
            continue;
        }

        QRhiResourceUpdateBatch *updates = rhi->nextResourceUpdateBatch();
        if (pass_index == 0U)
        {
            updates->uploadStaticBuffer(rgb_buffer.get(), input.data());
        }
        QRhiComputePipeline *pipeline = nullptr;
        QRhiBuffer *ubo = nullptr;
        quint32 groups = 0;
        if (pass.kind == GpuRgbPass::Kind::kAffine)
        {
            if (!std::isfinite(pass.affine.scale) || !std::isfinite(pass.affine.black))
            {
                rhi->endOffscreenFrame();
                return make_error(ErrorCode::kInvalidArgument,
                                  "GPU affine parameters must be finite",
                                  {{"reason", "gpu_affine_non_finite"}});
            }
            AffineUniforms params;
            params.count = count;
            params.scale = pass.affine.scale;
            params.black = pass.affine.black;
            updates->updateDynamicBuffer(affine_uniforms.get(), 0, sizeof(params), &params);
            pipeline = affine_pipeline.get();
            ubo = affine_uniforms.get();
            groups = sample_groups;
        }
        else if (pass.kind == GpuRgbPass::Kind::kLightControls)
        {
            if (light_slot >= 4U || light_uniforms[light_slot] == nullptr)
            {
                rhi->endOffscreenFrame();
                return make_error(ErrorCode::kInvalidArgument,
                                  "GPU light-control pass count exceeds the UBO pool",
                                  {{"reason", "gpu_pipeline_failed"}});
            }
            LightUniforms params;
            params.pixel_count = pixels;
            params.highlight_ev = pass.light.highlight_ev;
            params.shadow_ev = pass.light.shadow_ev;
            params.white_ev = pass.light.white_ev;
            params.black_ev = pass.light.black_ev;
            updates->updateDynamicBuffer(light_uniforms[light_slot].get(), 0, sizeof(params),
                                         &params);
            pipeline = light_pipeline.get();
            ubo = light_uniforms[light_slot].get();
            groups = pixel_groups;
            ++light_slot;
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
            ubo = sigmoid_uniforms.get();
            groups = pixel_groups;
        }
        auto bindings = bind_rgb(ubo);
        if (bindings == nullptr)
        {
            return fail_bindings();
        }
        auto dispatched = dispatch(updates, pipeline, bindings.get(), groups, last_pass);
        if (!dispatched)
        {
            return dispatched.error();
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
        return make_error(ErrorCode::kIo, "GPU frame end failed",
                          {{"reason", "gpu_pipeline_failed"}});
    }
    if (static_cast<quint32>(readback.data.size()) != bytes)
    {
        return make_error(ErrorCode::kIo, "GPU readback size mismatch",
                          {{"reason", "gpu_pipeline_failed"}});
    }
    std::memcpy(output.data(), readback.data.constData(), bytes);
    return {};
}

Result<void> GpuAdapter::Impl::demosaic_rcd(const std::span<const float> cfa,
                                            const std::span<float> rgb, const std::uint32_t width,
                                            const std::uint32_t height,
                                            const std::array<std::uint8_t, 4> pattern,
                                            const CancellationToken &cancellation) const
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    if (rhi == nullptr || rcd_pipeline == nullptr || rcd_uniforms[0] == nullptr)
    {
        return make_error(ErrorCode::kUnsupported, "GPU RCD demosaic is unavailable",
                          {{"reason", "gpu_unavailable"}});
    }
    if (width == 0U || height == 0U || width > std::numeric_limits<std::uint32_t>::max() / height)
    {
        return make_error(ErrorCode::kInvalidArgument, "GPU Bayer window is invalid",
                          {{"reason", "invalid_bayer_window"}});
    }
    const auto pixels = static_cast<std::size_t>(width) * height;
    if (cfa.size() != pixels || rgb.size() != pixels * 3U)
    {
        return make_error(ErrorCode::kInvalidArgument, "GPU Bayer buffers must match the window",
                          {{"reason", "gpu_copy_size_mismatch"}});
    }
    if (pixels > std::numeric_limits<quint32>::max() / sizeof(float) ||
        pixels * 3U > std::numeric_limits<quint32>::max() / sizeof(float))
    {
        return make_error(ErrorCode::kInvalidArgument, "GPU Bayer window is too large",
                          {{"reason", "gpu_copy_size_mismatch"}});
    }
    const auto cfa_bytes = static_cast<quint32>(pixels * sizeof(float));
    const auto rgb_bytes = cfa_bytes * 3U;
    std::lock_guard<std::mutex> lock(mutex);
    cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    if (rhi->isDeviceLost())
    {
        return make_error(ErrorCode::kIo, "GPU device was lost",
                          {{"reason", "gpu_pipeline_failed"}});
    }
    const auto make_storage = [&](const quint32 bytes) -> std::unique_ptr<QRhiBuffer>
    {
        std::unique_ptr<QRhiBuffer> buffer(
            rhi->newBuffer(QRhiBuffer::Static, QRhiBuffer::StorageBuffer, bytes));
        if (buffer == nullptr || !buffer->create())
        {
            return nullptr;
        }
        return buffer;
    };
    auto cfa_buffer = make_storage(cfa_bytes);
    auto rgb_buffer = make_storage(rgb_bytes);
    auto vh_buffer = make_storage(cfa_bytes);
    auto pq_buffer = make_storage(cfa_bytes);
    auto p_high_buffer = make_storage(cfa_bytes);
    auto q_high_buffer = make_storage(cfa_bytes);
    auto out_buffer = make_storage(rgb_bytes);
    if (cfa_buffer == nullptr || rgb_buffer == nullptr || vh_buffer == nullptr ||
        pq_buffer == nullptr || p_high_buffer == nullptr || q_high_buffer == nullptr ||
        out_buffer == nullptr)
    {
        return make_error(ErrorCode::kIo, "GPU demosaic buffer allocation failed",
                          {{"reason", "gpu_pipeline_failed"}});
    }
    QRhiCommandBuffer *command = nullptr;
    const auto began = rhi->beginOffscreenFrame(&command);
    if (began != QRhi::FrameOpSuccess || command == nullptr)
    {
        return make_error(ErrorCode::kIo, "GPU frame begin failed",
                          {{"reason", "gpu_pipeline_failed"}});
    }
    const auto groups_x = (width + kDemosaicGroup - 1U) / kDemosaicGroup;
    const auto groups_y = (height + kDemosaicGroup - 1U) / kDemosaicGroup;
    if (groups_x == 0U || groups_y == 0U ||
        groups_x > static_cast<quint32>(std::numeric_limits<int>::max()) ||
        groups_y > static_cast<quint32>(std::numeric_limits<int>::max()))
    {
        rhi->endOffscreenFrame();
        return make_error(ErrorCode::kInvalidArgument, "GPU dispatch is too large",
                          {{"reason", "gpu_copy_size_mismatch"}});
    }
    QRhiReadbackResult readback;
    for (quint32 stage = 0; stage < 9U; ++stage)
    {
        cancelled = cancellation.check();
        if (!cancelled)
        {
            rhi->endOffscreenFrame();
            return cancelled.error();
        }
        RcdUniforms params;
        params.width = width;
        params.height = height;
        params.stage = stage;
        params.pattern[0] = pattern[0];
        params.pattern[1] = pattern[1];
        params.pattern[2] = pattern[2];
        params.pattern[3] = pattern[3];
        QRhiResourceUpdateBatch *updates = rhi->nextResourceUpdateBatch();
        if (stage == 0U)
        {
            updates->uploadStaticBuffer(cfa_buffer.get(), cfa.data());
        }
        updates->updateDynamicBuffer(rcd_uniforms[stage].get(), 0, sizeof(params), &params);
        std::unique_ptr<QRhiShaderResourceBindings> bindings(rhi->newShaderResourceBindings());
        if (bindings == nullptr)
        {
            rhi->endOffscreenFrame();
            return make_error(ErrorCode::kIo, "GPU resource bindings failed",
                              {{"reason", "gpu_pipeline_failed"}});
        }
        bindings->setBindings({
            QRhiShaderResourceBinding::bufferLoadStore(0, QRhiShaderResourceBinding::ComputeStage,
                                                       cfa_buffer.get()),
            QRhiShaderResourceBinding::bufferLoadStore(1, QRhiShaderResourceBinding::ComputeStage,
                                                       rgb_buffer.get()),
            QRhiShaderResourceBinding::bufferLoadStore(2, QRhiShaderResourceBinding::ComputeStage,
                                                       vh_buffer.get()),
            QRhiShaderResourceBinding::bufferLoadStore(3, QRhiShaderResourceBinding::ComputeStage,
                                                       pq_buffer.get()),
            QRhiShaderResourceBinding::bufferLoadStore(4, QRhiShaderResourceBinding::ComputeStage,
                                                       p_high_buffer.get()),
            QRhiShaderResourceBinding::bufferLoadStore(5, QRhiShaderResourceBinding::ComputeStage,
                                                       q_high_buffer.get()),
            QRhiShaderResourceBinding::bufferLoadStore(6, QRhiShaderResourceBinding::ComputeStage,
                                                       out_buffer.get()),
            QRhiShaderResourceBinding::uniformBuffer(7, QRhiShaderResourceBinding::ComputeStage,
                                                     rcd_uniforms[stage].get()),
        });
        if (!bindings->create())
        {
            rhi->endOffscreenFrame();
            return make_error(ErrorCode::kIo, "GPU resource bindings failed",
                              {{"reason", "gpu_pipeline_failed"}});
        }
        command->beginComputePass(updates);
        command->setComputePipeline(rcd_pipeline.get());
        command->setShaderResources(bindings.get());
        command->dispatch(static_cast<int>(groups_x), static_cast<int>(groups_y), 1);
        if (stage == 8U)
        {
            QRhiResourceUpdateBatch *download = rhi->nextResourceUpdateBatch();
            download->readBackBuffer(out_buffer.get(), 0, rgb_bytes, &readback);
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
        return make_error(ErrorCode::kIo, "GPU frame end failed",
                          {{"reason", "gpu_pipeline_failed"}});
    }
    if (static_cast<quint32>(readback.data.size()) != rgb_bytes)
    {
        return make_error(ErrorCode::kIo, "GPU readback size mismatch",
                          {{"reason", "gpu_pipeline_failed"}});
    }
    std::memcpy(rgb.data(), readback.data.constData(), rgb_bytes);
    return {};
}

GpuAdapter::GpuAdapter(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl))
{
}
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

Result<void> GpuAdapter::demosaic_rcd(const std::span<const float> cfa, const std::span<float> rgb,
                                      const std::uint32_t width, const std::uint32_t height,
                                      const std::array<std::uint8_t, 4> pattern,
                                      const CancellationToken &cancellation) const
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kUnsupported, "GPU adapter is not initialized",
                          {{"reason", "gpu_unavailable"}});
    }
    return impl_->demosaic_rcd(cfa, rgb, width, height, pattern, cancellation);
}

} // namespace ravo
