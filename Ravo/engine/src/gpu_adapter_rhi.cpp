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
extern unsigned char const ravo_gpu_rapidraw_basic_tone_qsb[];
extern unsigned long long const ravo_gpu_rapidraw_basic_tone_qsb_size;
extern unsigned char const ravo_gpu_rapidraw_tone_controls_qsb[];
extern unsigned long long const ravo_gpu_rapidraw_tone_controls_qsb_size;
extern unsigned char const ravo_gpu_light_controls_qsb[];
extern unsigned long long const ravo_gpu_light_controls_qsb_size;
extern unsigned char const ravo_gpu_sharpen_lab_qsb[];
extern unsigned long long const ravo_gpu_sharpen_lab_qsb_size;
extern unsigned char const ravo_gpu_rcd_demosaic_qsb[];
extern unsigned long long const ravo_gpu_rcd_demosaic_qsb_size;
extern unsigned char const ravo_gpu_copy_rgb_qsb[];
extern unsigned long long const ravo_gpu_copy_rgb_qsb_size;
extern unsigned char const ravo_gpu_pack_rgba8_qsb[];
extern unsigned long long const ravo_gpu_pack_rgba8_qsb_size;

#if defined(Q_OS_MACOS)
#include "gpu_display_metal.h"
#endif

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

struct RapidRawToneUniforms
{
    quint32 width = 0;
    quint32 height = 0;
    quint32 pixel_count = 0;
    quint32 radius = 1;
    float ev_shift = 0.0F;
    float exposure = 0.0F;
    float contrast = 0.0F;
    float shadows = 0.0F;
    float highlights = 0.0F;
    float pad0 = 0.0F;
    float whites = 0.0F;
    float blacks = 0.0F;
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

struct CopyUniforms
{
    quint32 src_width = 0;
    quint32 src_height = 0;
    quint32 dst_width = 0;
    quint32 dst_height = 0;
    quint32 origin_x = 0;
    quint32 origin_y = 0;
    quint32 pad0[2] = {};
};

struct PackUniforms
{
    quint32 width = 0;
    quint32 height = 0;
    quint32 pad0[2] = {};
};

void upload_storage(QRhiResourceUpdateBatch *updates, QRhiBuffer *buffer, const void *data,
                    const quint32 bytes)
{
    if (updates == nullptr || buffer == nullptr || data == nullptr || bytes == 0U)
    {
        return;
    }
    updates->uploadStaticBuffer(buffer, 0, bytes, data);
}

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
    std::unique_ptr<QRhiBuffer> rapidraw_basic_tone_uniforms;
    std::unique_ptr<QRhiBuffer> rapidraw_tone_uniforms;
    std::unique_ptr<QRhiBuffer> sigmoid_uniforms;
    std::unique_ptr<QRhiBuffer> light_uniforms[4];
    std::unique_ptr<QRhiBuffer> sharpen_uniforms[4];
    std::unique_ptr<QRhiBuffer> rcd_uniforms[9];
    std::unique_ptr<QRhiBuffer> copy_uniforms;
    std::unique_ptr<QRhiBuffer> pack_uniforms;
    std::unique_ptr<QRhiShaderResourceBindings> layout_bindings;
    std::unique_ptr<QRhiShaderResourceBindings> sharpen_layout;
    std::unique_ptr<QRhiShaderResourceBindings> rapidraw_tone_layout;
    std::unique_ptr<QRhiShaderResourceBindings> rcd_layout;
    std::unique_ptr<QRhiShaderResourceBindings> copy_layout;
    std::unique_ptr<QRhiShaderResourceBindings> pack_layout;
    std::unique_ptr<QRhiComputePipeline> affine_pipeline;
    std::unique_ptr<QRhiComputePipeline> sigmoid_pipeline;
    std::unique_ptr<QRhiComputePipeline> rapidraw_basic_tone_pipeline;
    std::unique_ptr<QRhiComputePipeline> rapidraw_tone_pipeline;
    std::unique_ptr<QRhiComputePipeline> light_pipeline;
    std::unique_ptr<QRhiComputePipeline> sharpen_pipeline;
    std::unique_ptr<QRhiComputePipeline> rcd_pipeline;
    std::unique_ptr<QRhiComputePipeline> copy_pipeline;
    std::unique_ptr<QRhiComputePipeline> pack_pipeline;
    std::string_view backend;
    mutable std::mutex mutex;
    mutable std::unique_ptr<QRhiBuffer> rgb_source;
    mutable quint32 rgb_source_bytes = 0;
    mutable std::unique_ptr<QRhiBuffer> rgb_working;
    mutable quint32 rgb_working_bytes = 0;
    mutable std::unique_ptr<QRhiBuffer> lab_storage;
    mutable quint32 lab_storage_bytes = 0;
    mutable std::unique_ptr<QRhiBuffer> blur_storage;
    mutable quint32 blur_storage_bytes = 0;
    mutable std::unique_ptr<QRhiBuffer> rcd_cfa;
    mutable quint32 rcd_cfa_bytes = 0;
    mutable std::unique_ptr<QRhiBuffer> rcd_rgb;
    mutable quint32 rcd_rgb_bytes = 0;
    mutable std::unique_ptr<QRhiBuffer> rcd_vh;
    mutable quint32 rcd_vh_bytes = 0;
    mutable std::unique_ptr<QRhiBuffer> rcd_pq;
    mutable quint32 rcd_pq_bytes = 0;
    mutable std::unique_ptr<QRhiBuffer> rcd_p_high;
    mutable quint32 rcd_p_high_bytes = 0;
    mutable std::unique_ptr<QRhiBuffer> rcd_q_high;
    mutable quint32 rcd_q_high_bytes = 0;
    mutable std::unique_ptr<QRhiBuffer> rcd_out;
    mutable quint32 rcd_out_bytes = 0;
    mutable std::unique_ptr<QRhiTexture> pack_texture;
    mutable quint32 pack_texture_width = 0;
    mutable quint32 pack_texture_height = 0;
    mutable std::uint32_t retained_width = 0;
    mutable std::uint32_t retained_height = 0;
    mutable std::string retained_key;
    static constexpr std::uint32_t kDisplaySlots = 2;
    static constexpr std::uint32_t kDisplayBuffers = 2;
    mutable std::uint64_t display_generation[kDisplaySlots] = {};
    mutable std::uint32_t display_width[kDisplaySlots] = {};
    mutable std::uint32_t display_height[kDisplaySlots] = {};
    mutable std::uint32_t display_write[kDisplaySlots] = {};
    mutable std::uint32_t display_read[kDisplaySlots] = {};
    mutable void *display_surface[kDisplaySlots][kDisplayBuffers] = {};
    mutable void *display_metal_texture[kDisplaySlots][kDisplayBuffers] = {};

    ~Impl();
    [[nodiscard]] Result<void> open();
    [[nodiscard]] Result<std::unique_ptr<QRhiComputePipeline>>
    make_pipeline(const QShader &shader, QRhiShaderResourceBindings *bindings);
    [[nodiscard]] Result<QRhiBuffer *> ensure_storage(std::unique_ptr<QRhiBuffer> &slot,
                                                      quint32 &slot_bytes, quint32 bytes) const;
    [[nodiscard]] Result<void> apply_passes(std::span<const float> input, std::span<float> output,
                                            std::span<const GpuRgbPass> passes,
                                            GpuRgbApplyOptions options,
                                            const CancellationToken &cancellation) const;
    [[nodiscard]] Result<void> demosaic_rcd(std::span<const float> cfa, std::span<float> rgb,
                                            std::uint32_t width, std::uint32_t height,
                                            std::array<std::uint8_t, 4> pattern,
                                            std::uint32_t crop_x, std::uint32_t crop_y,
                                            std::uint32_t crop_width, std::uint32_t crop_height,
                                            const CancellationToken &cancellation) const;
    [[nodiscard]] Result<void> copy_rgb_window(QRhiCommandBuffer *command,
                                               QRhiResourceUpdateBatch *updates, QRhiBuffer *source,
                                               QRhiBuffer *destination, quint32 src_width,
                                               quint32 src_height, quint32 origin_x,
                                               quint32 origin_y, quint32 dst_width,
                                               quint32 dst_height) const;
    [[nodiscard]] Result<void> publish_display(QRhiCommandBuffer *command, QRhiBuffer *rgb,
                                               quint32 width, quint32 height, bool last) const;
};

GpuAdapter::Impl::~Impl()
{
#if defined(Q_OS_MACOS)
    for (std::uint32_t slot = 0; slot < kDisplaySlots; ++slot)
    {
        for (std::uint32_t buffer = 0; buffer < kDisplayBuffers; ++buffer)
        {
            gpu_metal::release_texture(display_metal_texture[slot][buffer]);
            gpu_metal::release_iosurface(display_surface[slot][buffer]);
            display_metal_texture[slot][buffer] = nullptr;
            display_surface[slot][buffer] = nullptr;
        }
    }
#endif
}

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

Result<QRhiBuffer *> GpuAdapter::Impl::ensure_storage(std::unique_ptr<QRhiBuffer> &slot,
                                                      quint32 &slot_bytes,
                                                      const quint32 bytes) const
{
    if (bytes == 0U)
    {
        return make_error(ErrorCode::kInvalidArgument, "GPU buffer is too large",
                          {{"reason", "gpu_copy_size_mismatch"}});
    }
    if (slot != nullptr && slot_bytes >= bytes)
    {
        return slot.get();
    }
    slot.reset(rhi->newBuffer(QRhiBuffer::Static, QRhiBuffer::StorageBuffer, bytes));
    if (slot == nullptr || !slot->create())
    {
        slot.reset();
        slot_bytes = 0;
        return make_error(ErrorCode::kIo, "GPU buffer allocation failed",
                          {{"reason", "gpu_pipeline_failed"}});
    }
    slot_bytes = bytes;
    return slot.get();
}

Result<void> GpuAdapter::Impl::copy_rgb_window(QRhiCommandBuffer *command,
                                               QRhiResourceUpdateBatch *updates, QRhiBuffer *source,
                                               QRhiBuffer *destination, const quint32 src_width,
                                               const quint32 src_height, const quint32 origin_x,
                                               const quint32 origin_y, const quint32 dst_width,
                                               const quint32 dst_height) const
{
    if (command == nullptr || source == nullptr || destination == nullptr ||
        copy_pipeline == nullptr || copy_uniforms == nullptr || dst_width == 0U || dst_height == 0U)
    {
        rhi->endOffscreenFrame();
        return make_error(ErrorCode::kIo, "GPU copy dispatch failed",
                          {{"reason", "gpu_pipeline_failed"}});
    }
    CopyUniforms params;
    params.src_width = src_width;
    params.src_height = src_height;
    params.dst_width = dst_width;
    params.dst_height = dst_height;
    params.origin_x = origin_x;
    params.origin_y = origin_y;
    if (updates == nullptr)
    {
        updates = rhi->nextResourceUpdateBatch();
    }
    updates->updateDynamicBuffer(copy_uniforms.get(), 0, sizeof(params), &params);
    std::unique_ptr<QRhiShaderResourceBindings> bindings(rhi->newShaderResourceBindings());
    if (bindings == nullptr)
    {
        rhi->endOffscreenFrame();
        return make_error(ErrorCode::kIo, "GPU resource bindings failed",
                          {{"reason", "gpu_pipeline_failed"}});
    }
    bindings->setBindings({
        QRhiShaderResourceBinding::bufferLoad(0, QRhiShaderResourceBinding::ComputeStage, source),
        QRhiShaderResourceBinding::bufferLoadStore(1, QRhiShaderResourceBinding::ComputeStage,
                                                   destination),
        QRhiShaderResourceBinding::uniformBuffer(2, QRhiShaderResourceBinding::ComputeStage,
                                                 copy_uniforms.get()),
    });
    if (!bindings->create())
    {
        rhi->endOffscreenFrame();
        return make_error(ErrorCode::kIo, "GPU resource bindings failed",
                          {{"reason", "gpu_pipeline_failed"}});
    }
    const auto groups = (dst_width * dst_height + kWorkgroup - 1U) / kWorkgroup;
    if (groups == 0U || groups > static_cast<quint32>(std::numeric_limits<int>::max()))
    {
        rhi->endOffscreenFrame();
        return make_error(ErrorCode::kInvalidArgument, "GPU dispatch is too large",
                          {{"reason", "gpu_copy_size_mismatch"}});
    }
    command->beginComputePass(updates);
    command->setComputePipeline(copy_pipeline.get());
    command->setShaderResources(bindings.get());
    command->dispatch(static_cast<int>(groups), 1, 1);
    command->endComputePass();
    return {};
}

Result<void> GpuAdapter::Impl::publish_display(QRhiCommandBuffer *command, QRhiBuffer *rgb,
                                               const quint32 width, const quint32 height,
                                               const bool last) const
{
    if (command == nullptr || rgb == nullptr || pack_pipeline == nullptr ||
        pack_uniforms == nullptr || width == 0U || height == 0U)
    {
        rhi->endOffscreenFrame();
        return make_error(ErrorCode::kIo, "GPU display pack failed",
                          {{"reason", "gpu_pipeline_failed"}});
    }
    if (pack_texture == nullptr || pack_texture_width != width || pack_texture_height != height)
    {
        pack_texture.reset(rhi->newTexture(QRhiTexture::RGBA8,
                                           QSize(static_cast<int>(width), static_cast<int>(height)),
                                           1, QRhiTexture::UsedWithLoadStore));
        if (pack_texture == nullptr || !pack_texture->create())
        {
            pack_texture.reset();
            pack_texture_width = 0;
            pack_texture_height = 0;
            rhi->endOffscreenFrame();
            return make_error(ErrorCode::kIo, "GPU display texture allocation failed",
                              {{"reason", "gpu_pipeline_failed"}});
        }
        pack_texture_width = width;
        pack_texture_height = height;
    }
    PackUniforms params;
    params.width = width;
    params.height = height;
    QRhiResourceUpdateBatch *updates = rhi->nextResourceUpdateBatch();
    updates->updateDynamicBuffer(pack_uniforms.get(), 0, sizeof(params), &params);
    std::unique_ptr<QRhiShaderResourceBindings> bindings(rhi->newShaderResourceBindings());
    if (bindings == nullptr)
    {
        rhi->endOffscreenFrame();
        return make_error(ErrorCode::kIo, "GPU resource bindings failed",
                          {{"reason", "gpu_pipeline_failed"}});
    }
    bindings->setBindings({
        QRhiShaderResourceBinding::bufferLoad(0, QRhiShaderResourceBinding::ComputeStage, rgb),
        QRhiShaderResourceBinding::imageStore(1, QRhiShaderResourceBinding::ComputeStage,
                                              pack_texture.get(), 0),
        QRhiShaderResourceBinding::uniformBuffer(2, QRhiShaderResourceBinding::ComputeStage,
                                                 pack_uniforms.get()),
    });
    if (!bindings->create())
    {
        rhi->endOffscreenFrame();
        return make_error(ErrorCode::kIo, "GPU resource bindings failed",
                          {{"reason", "gpu_pipeline_failed"}});
    }
    const auto groups_x = (width + kDemosaicGroup - 1U) / kDemosaicGroup;
    const auto groups_y = (height + kDemosaicGroup - 1U) / kDemosaicGroup;
    command->beginComputePass(updates);
    command->setComputePipeline(pack_pipeline.get());
    command->setShaderResources(bindings.get());
    command->dispatch(static_cast<int>(groups_x), static_cast<int>(groups_y), 1);
    command->endComputePass();
    static_cast<void>(last);
    return {};
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
    auto rapidraw_basic_tone_shader =
        load_shader(ravo_gpu_rapidraw_basic_tone_qsb, ravo_gpu_rapidraw_basic_tone_qsb_size);
    if (!rapidraw_basic_tone_shader)
    {
        return rapidraw_basic_tone_shader.error();
    }
    auto rapidraw_tone_shader =
        load_shader(ravo_gpu_rapidraw_tone_controls_qsb, ravo_gpu_rapidraw_tone_controls_qsb_size);
    if (!rapidraw_tone_shader)
    {
        return rapidraw_tone_shader.error();
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
    const auto ubo_bytes =
        std::max(static_cast<quint32>(aligned), static_cast<quint32>(sizeof(SharpenUniforms)));
    const auto ubo_aligned =
        ((ubo_bytes + static_cast<quint32>(aligned) - 1U) / static_cast<quint32>(aligned)) *
        static_cast<quint32>(aligned);
    rhi = std::move(created);
    affine_uniforms.reset(
        rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, ubo_aligned));
    rapidraw_basic_tone_uniforms.reset(
        rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, ubo_aligned));
    rapidraw_tone_uniforms.reset(
        rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, ubo_aligned));
    sigmoid_uniforms.reset(
        rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, ubo_aligned));
    layout_bindings.reset(rhi->newShaderResourceBindings());
    sharpen_layout.reset(rhi->newShaderResourceBindings());
    rapidraw_tone_layout.reset(rhi->newShaderResourceBindings());
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
    if (affine_uniforms == nullptr || rapidraw_basic_tone_uniforms == nullptr ||
        rapidraw_tone_uniforms == nullptr || sigmoid_uniforms == nullptr ||
        layout_bindings == nullptr || sharpen_layout == nullptr || rapidraw_tone_layout == nullptr ||
        !light_ubos || !sharpen_ubos || !affine_uniforms->create() ||
        !rapidraw_basic_tone_uniforms->create() || !rapidraw_tone_uniforms->create() ||
        !sigmoid_uniforms->create())
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
    rapidraw_tone_layout->setBindings({
        QRhiShaderResourceBinding::bufferLoadStore(0, QRhiShaderResourceBinding::ComputeStage,
                                                   nullptr),
        QRhiShaderResourceBinding::bufferLoad(1, QRhiShaderResourceBinding::ComputeStage,
                                              nullptr),
        QRhiShaderResourceBinding::uniformBuffer(2, QRhiShaderResourceBinding::ComputeStage,
                                                 rapidraw_tone_uniforms.get()),
    });
    if (!rapidraw_tone_layout->create())
    {
        return make_error(ErrorCode::kIo, "GPU RapidRAW tone resource layout failed",
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
    auto rapidraw_basic_tone =
        make_pipeline(rapidraw_basic_tone_shader.value(), layout_bindings.get());
    if (!rapidraw_basic_tone)
    {
        return rapidraw_basic_tone.error();
    }
    auto rapidraw_tone =
        make_pipeline(rapidraw_tone_shader.value(), rapidraw_tone_layout.get());
    if (!rapidraw_tone)
    {
        return rapidraw_tone.error();
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
    rapidraw_basic_tone_pipeline = std::move(rapidraw_basic_tone).value();
    rapidraw_tone_pipeline = std::move(rapidraw_tone).value();
    light_pipeline = std::move(light).value();
    sharpen_pipeline = std::move(sharpen).value();
    auto copy_shader = load_shader(ravo_gpu_copy_rgb_qsb, ravo_gpu_copy_rgb_qsb_size);
    if (!copy_shader)
    {
        return copy_shader.error();
    }
    auto pack_shader = load_shader(ravo_gpu_pack_rgba8_qsb, ravo_gpu_pack_rgba8_qsb_size);
    if (!pack_shader)
    {
        return pack_shader.error();
    }
    copy_uniforms.reset(
        rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, ubo_aligned));
    pack_uniforms.reset(
        rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, ubo_aligned));
    copy_layout.reset(rhi->newShaderResourceBindings());
    pack_layout.reset(rhi->newShaderResourceBindings());
    if (copy_uniforms == nullptr || pack_uniforms == nullptr || copy_layout == nullptr ||
        pack_layout == nullptr || !copy_uniforms->create() || !pack_uniforms->create())
    {
        return make_error(ErrorCode::kIo, "GPU pipeline allocation failed",
                          {{"reason", "gpu_pipeline_failed"}});
    }
    copy_layout->setBindings({
        QRhiShaderResourceBinding::bufferLoad(0, QRhiShaderResourceBinding::ComputeStage, nullptr),
        QRhiShaderResourceBinding::bufferLoadStore(1, QRhiShaderResourceBinding::ComputeStage,
                                                   nullptr),
        QRhiShaderResourceBinding::uniformBuffer(2, QRhiShaderResourceBinding::ComputeStage,
                                                 copy_uniforms.get()),
    });
    if (!copy_layout->create())
    {
        return make_error(ErrorCode::kIo, "GPU resource layout failed",
                          {{"reason", "gpu_pipeline_failed"}});
    }
    pack_layout->setBindings({
        QRhiShaderResourceBinding::bufferLoad(0, QRhiShaderResourceBinding::ComputeStage, nullptr),
        QRhiShaderResourceBinding::imageStore(1, QRhiShaderResourceBinding::ComputeStage, nullptr,
                                              0),
        QRhiShaderResourceBinding::uniformBuffer(2, QRhiShaderResourceBinding::ComputeStage,
                                                 pack_uniforms.get()),
    });
    if (!pack_layout->create())
    {
        return make_error(ErrorCode::kIo, "GPU resource layout failed",
                          {{"reason", "gpu_pipeline_failed"}});
    }
    auto copy = make_pipeline(copy_shader.value(), copy_layout.get());
    if (!copy)
    {
        return copy.error();
    }
    auto pack = make_pipeline(pack_shader.value(), pack_layout.get());
    if (!pack)
    {
        return pack.error();
    }
    copy_pipeline = std::move(copy).value();
    pack_pipeline = std::move(pack).value();
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
                                            const GpuRgbApplyOptions options,
                                            const CancellationToken &cancellation) const
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    if (rhi == nullptr || affine_uniforms == nullptr || rapidraw_basic_tone_uniforms == nullptr ||
        rapidraw_tone_uniforms == nullptr || sigmoid_uniforms == nullptr ||
        light_uniforms[0] == nullptr || affine_pipeline == nullptr || sigmoid_pipeline == nullptr ||
        rapidraw_basic_tone_pipeline == nullptr || rapidraw_tone_pipeline == nullptr ||
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
    if (input.size() == 0U || (options.download && input.size() != output.size()))
    {
        return make_error(ErrorCode::kInvalidArgument, "GPU buffers must be the same size",
                          {{"reason", "gpu_copy_size_mismatch"}});
    }
    bool needs_pixels = false;
    bool needs_sharpen = false;
    bool needs_rapidraw_tone = false;
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
        else if (pass.kind == GpuRgbPass::Kind::kRapidRawToneControls)
        {
            if (needs_rapidraw_tone)
            {
                return make_error(ErrorCode::kInvalidArgument,
                                  "GPU accepts one RapidRAW tone pass per batch",
                                  {{"reason", "gpu_pipeline_failed"}});
            }
            needs_rapidraw_tone = true;
        }
        else if (pass.kind != GpuRgbPass::Kind::kSigmoid &&
                 pass.kind != GpuRgbPass::Kind::kRapidRawBasicTone &&
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
    if (options.from_retained_source &&
        (retained_width == 0U ||
         static_cast<std::size_t>(retained_width) * retained_height * 3U != input.size()))
    {
        return make_error(ErrorCode::kInvalidArgument, "GPU retained source does not match input",
                          {{"reason", "gpu_copy_size_mismatch"}});
    }
    auto source = ensure_storage(rgb_source, rgb_source_bytes, bytes);
    if (!source)
    {
        return source.error();
    }
    auto working = ensure_storage(rgb_working, rgb_working_bytes, bytes);
    if (!working)
    {
        return working.error();
    }
    QRhiBuffer *lab_buffer = nullptr;
    QRhiBuffer *blur_buffer = nullptr;
    if (needs_sharpen || needs_rapidraw_tone)
    {
        if (pixels == 0U)
        {
            return make_error(ErrorCode::kInvalidArgument, "GPU RGB buffers must be packed RGB",
                              {{"reason", "gpu_copy_size_mismatch"}});
        }
        auto lab = ensure_storage(lab_storage, lab_storage_bytes, lab_bytes);
        if (!lab)
        {
            return lab.error();
        }
        if (needs_sharpen)
        {
            auto blur = ensure_storage(blur_storage, blur_storage_bytes, std::max(blur_bytes, 4U));
            if (!blur)
            {
                return blur.error();
            }
            blur_buffer = blur.value();
        }
        lab_buffer = lab.value();
    }
    QRhiBuffer *const rgb_buffer = working.value();
    QRhiBuffer *const source_buffer = source.value();
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
                                                       rgb_buffer),
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
        command->endComputePass();
        static_cast<void>(last);
        return {};
    };
    QRhiResourceUpdateBatch *seed = rhi->nextResourceUpdateBatch();
    const bool rgb_shaped = options.width != 0U && options.height != 0U &&
                            static_cast<std::uint64_t>(options.width) * options.height == pixels;
    if (!options.from_retained_source)
    {
        upload_storage(seed, source_buffer, input.data(), bytes);
        if (rgb_shaped)
        {
            retained_width = options.width;
            retained_height = options.height;
        }
        else
        {
            retained_width = pixels;
            retained_height = 1U;
        }
        retained_key = options.retained_key;
    }
    QRhiResourceUpdateBatch *pending_updates = nullptr;
    if (options.from_retained_source || needs_pixels || rgb_shaped)
    {
        const auto copy_width = rgb_shaped ? options.width : pixels;
        const auto copy_height = rgb_shaped ? options.height : 1U;
        auto copied = copy_rgb_window(command, seed, source_buffer, rgb_buffer, copy_width,
                                      copy_height, 0U, 0U, copy_width, copy_height);
        if (!copied)
        {
            return copied.error();
        }
    }
    else
    {
        upload_storage(seed, rgb_buffer, input.data(), bytes);
        pending_updates = seed;
    }
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
        if (pass.kind == GpuRgbPass::Kind::kRapidRawToneControls)
        {
            const auto &tone = pass.rapidraw_tone;
            if (tone.width == 0U || tone.height == 0U || tone.radius == 0U || tone.radius > 64U ||
                static_cast<std::uint64_t>(tone.width) * tone.height != pixels ||
                lab_buffer == nullptr || !std::isfinite(tone.ev_shift) ||
                !std::isfinite(tone.exposure) || !std::isfinite(tone.contrast) ||
                !std::isfinite(tone.highlights) || !std::isfinite(tone.shadows) ||
                !std::isfinite(tone.whites) || !std::isfinite(tone.blacks))
            {
                rhi->endOffscreenFrame();
                return make_error(ErrorCode::kInvalidArgument,
                                  "GPU RapidRAW tone pass is invalid",
                                  {{"reason", "gpu_pipeline_failed"}});
            }
            QRhiResourceUpdateBatch *copy_updates =
                pending_updates != nullptr ? pending_updates : rhi->nextResourceUpdateBatch();
            pending_updates = nullptr;
            auto copied = copy_rgb_window(command, copy_updates, rgb_buffer, lab_buffer, tone.width,
                                          tone.height, 0U, 0U, tone.width, tone.height);
            if (!copied)
            {
                return copied.error();
            }
            RapidRawToneUniforms params;
            params.width = tone.width;
            params.height = tone.height;
            params.pixel_count = pixels;
            params.radius = tone.radius;
            params.ev_shift = tone.ev_shift;
            params.exposure = tone.exposure;
            params.contrast = tone.contrast;
            params.shadows = tone.shadows;
            params.highlights = tone.highlights;
            params.whites = tone.whites;
            params.blacks = tone.blacks;
            QRhiResourceUpdateBatch *updates = rhi->nextResourceUpdateBatch();
            updates->updateDynamicBuffer(rapidraw_tone_uniforms.get(), 0, sizeof(params), &params);
            std::unique_ptr<QRhiShaderResourceBindings> bindings(
                rhi->newShaderResourceBindings());
            if (bindings == nullptr)
            {
                return fail_bindings();
            }
            bindings->setBindings({
                QRhiShaderResourceBinding::bufferLoadStore(
                    0, QRhiShaderResourceBinding::ComputeStage, rgb_buffer),
                QRhiShaderResourceBinding::bufferLoad(1, QRhiShaderResourceBinding::ComputeStage,
                                                      lab_buffer),
                QRhiShaderResourceBinding::uniformBuffer(
                    2, QRhiShaderResourceBinding::ComputeStage, rapidraw_tone_uniforms.get()),
            });
            if (!bindings->create())
            {
                return fail_bindings();
            }
            auto dispatched =
                dispatch(updates, rapidraw_tone_pipeline.get(), bindings.get(), pixel_groups, false);
            if (!dispatched)
            {
                return dispatched.error();
            }
            continue;
        }
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
                const quint32 stage =
                    pass.sharpen.radius == 0U && stage_index == 1U ? 3U : stage_index;
                SharpenUniforms params;
                params.width = pass.sharpen.width;
                params.height = pass.sharpen.height;
                params.radius = pass.sharpen.radius;
                params.stage = stage;
                params.amount = pass.sharpen.amount;
                params.threshold = pass.sharpen.threshold;
                std::memcpy(params.kernel, pass.sharpen.kernel.data(),
                            pass.sharpen.kernel.size() * sizeof(float));
                QRhiResourceUpdateBatch *updates =
                    pending_updates != nullptr ? pending_updates : rhi->nextResourceUpdateBatch();
                pending_updates = nullptr;
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
                        0, QRhiShaderResourceBinding::ComputeStage, rgb_buffer),
                    QRhiShaderResourceBinding::bufferLoadStore(
                        1, QRhiShaderResourceBinding::ComputeStage, lab_buffer),
                    QRhiShaderResourceBinding::bufferLoadStore(
                        2, QRhiShaderResourceBinding::ComputeStage, blur_buffer),
                    QRhiShaderResourceBinding::uniformBuffer(
                        3, QRhiShaderResourceBinding::ComputeStage, sharpen_uniforms[stage].get()),
                });
                if (!bindings->create())
                {
                    return fail_bindings();
                }
                const bool last = last_pass && stage_index + 1U == stages;
                auto dispatched =
                    dispatch(updates, sharpen_pipeline.get(), bindings.get(), pixel_groups, last);
                if (!dispatched)
                {
                    return dispatched.error();
                }
            }
            continue;
        }

        QRhiResourceUpdateBatch *updates =
            pending_updates != nullptr ? pending_updates : rhi->nextResourceUpdateBatch();
        pending_updates = nullptr;
        QRhiComputePipeline *pipeline = nullptr;
        QRhiBuffer *ubo = nullptr;
        quint32 groups = 0;
        if (pass.kind == GpuRgbPass::Kind::kAffine ||
            pass.kind == GpuRgbPass::Kind::kRapidRawBasicTone)
        {
            if (pass.kind == GpuRgbPass::Kind::kAffine &&
                (!std::isfinite(pass.affine.scale) || !std::isfinite(pass.affine.black)))
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
            QRhiBuffer *const params_buffer = pass.kind == GpuRgbPass::Kind::kAffine ?
                                                  affine_uniforms.get() :
                                                  rapidraw_basic_tone_uniforms.get();
            updates->updateDynamicBuffer(params_buffer, 0, sizeof(params), &params);
            pipeline = pass.kind == GpuRgbPass::Kind::kAffine ? affine_pipeline.get() :
                                                                rapidraw_basic_tone_pipeline.get();
            ubo = params_buffer;
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
        auto dispatched = dispatch(updates, pipeline, bindings.get(), groups, false);
        if (!dispatched)
        {
            return dispatched.error();
        }
    }
    if (options.publish_display)
    {
        const auto display_w = options.width != 0U ? options.width : pixels;
        const auto display_h = options.height != 0U ? options.height : 1U;
        if (static_cast<std::uint64_t>(display_w) * display_h != pixels)
        {
            rhi->endOffscreenFrame();
            return make_error(ErrorCode::kInvalidArgument, "GPU display size does not match pixels",
                              {{"reason", "gpu_copy_size_mismatch"}});
        }
        auto published =
            publish_display(command, rgb_buffer, display_w, display_h, !options.download);
        if (!published)
        {
            return published.error();
        }
    }
    if (options.download)
    {
        QRhiResourceUpdateBatch *download = rhi->nextResourceUpdateBatch();
        download->readBackBuffer(rgb_buffer, 0, bytes, &readback);
        command->beginComputePass();
        command->endComputePass(download);
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
#if defined(Q_OS_MACOS)
    if (options.publish_display && pack_texture != nullptr)
    {
        if (options.display_slot >= kDisplaySlots)
        {
            return make_error(ErrorCode::kInvalidArgument, "GPU display slot is invalid",
                              {{"reason", "gpu_pipeline_failed"}});
        }
        const auto *metal = static_cast<const QRhiMetalNativeHandles *>(rhi->nativeHandles());
        const auto native = pack_texture->nativeTexture();
        if (metal == nullptr || metal->dev == nullptr || native.object == 0)
        {
            return make_error(ErrorCode::kIo, "GPU display native texture is unavailable",
                              {{"reason", "gpu_pipeline_failed"}});
        }
        const auto display_w = options.width != 0U ? options.width : pixels;
        const auto display_h = options.height != 0U ? options.height : 1U;
        const auto slot = options.display_slot;
        if (display_width[slot] != display_w || display_height[slot] != display_h)
        {
            for (std::uint32_t buffer = 0; buffer < kDisplayBuffers; ++buffer)
            {
                gpu_metal::release_texture(display_metal_texture[slot][buffer]);
                gpu_metal::release_iosurface(display_surface[slot][buffer]);
                display_metal_texture[slot][buffer] = nullptr;
                display_surface[slot][buffer] = nullptr;
            }
            display_write[slot] = 0;
            display_read[slot] = 0;
            display_width[slot] = display_w;
            display_height[slot] = display_h;
        }
        const auto write = display_write[slot] % kDisplayBuffers;
        if (display_surface[slot][write] == nullptr)
        {
            display_surface[slot][write] = gpu_metal::create_iosurface(display_w, display_h);
            display_metal_texture[slot][write] = gpu_metal::texture_from_iosurface(
                metal->dev, display_surface[slot][write], display_w, display_h);
        }
        if (display_surface[slot][write] == nullptr ||
            display_metal_texture[slot][write] == nullptr ||
            !gpu_metal::blit_texture(metal->dev, metal->cmdQueue,
                                     reinterpret_cast<void *>(native.object),
                                     display_metal_texture[slot][write], display_w, display_h))
        {
            return make_error(ErrorCode::kIo, "GPU display blit failed",
                              {{"reason", "gpu_pipeline_failed"}});
        }
        display_read[slot] = write;
        display_write[slot] = 1U - write;
        ++display_generation[slot];
    }
#else
    if (options.publish_display)
    {
        return make_error(ErrorCode::kUnsupported, "GPU display transport is unavailable",
                          {{"reason", "gpu_unavailable"}});
    }
#endif
    if (options.download)
    {
        if (static_cast<quint32>(readback.data.size()) != bytes)
        {
            return make_error(ErrorCode::kIo, "GPU readback size mismatch",
                              {{"reason", "gpu_pipeline_failed"}});
        }
        std::memcpy(output.data(), readback.data.constData(), bytes);
    }
    return {};
}

Result<void> GpuAdapter::Impl::demosaic_rcd(
    const std::span<const float> cfa, const std::span<float> rgb, const std::uint32_t width,
    const std::uint32_t height, const std::array<std::uint8_t, 4> pattern,
    const std::uint32_t crop_x, const std::uint32_t crop_y, const std::uint32_t crop_width,
    const std::uint32_t crop_height, const CancellationToken &cancellation) const
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
    const auto out_width = crop_width == 0U ? width : crop_width;
    const auto out_height = crop_height == 0U ? height : crop_height;
    if (crop_x > width || crop_y > height || out_width > width - crop_x ||
        out_height > height - crop_y)
    {
        return make_error(ErrorCode::kInvalidArgument, "GPU Bayer crop is outside the window",
                          {{"reason", "invalid_bayer_window"}});
    }
    const auto pixels = static_cast<std::size_t>(width) * height;
    const auto out_pixels = static_cast<std::size_t>(out_width) * out_height;
    if (cfa.size() != pixels || rgb.size() != out_pixels * 3U)
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
    auto cfa_ok = ensure_storage(rcd_cfa, rcd_cfa_bytes, cfa_bytes);
    auto rgb_ok = ensure_storage(rcd_rgb, rcd_rgb_bytes, rgb_bytes);
    auto vh_ok = ensure_storage(rcd_vh, rcd_vh_bytes, cfa_bytes);
    auto pq_ok = ensure_storage(rcd_pq, rcd_pq_bytes, cfa_bytes);
    auto p_high_ok = ensure_storage(rcd_p_high, rcd_p_high_bytes, cfa_bytes);
    auto q_high_ok = ensure_storage(rcd_q_high, rcd_q_high_bytes, cfa_bytes);
    auto out_ok = ensure_storage(rcd_out, rcd_out_bytes, rgb_bytes);
    if (!cfa_ok || !rgb_ok || !vh_ok || !pq_ok || !p_high_ok || !q_high_ok || !out_ok)
    {
        return !cfa_ok    ? cfa_ok.error() :
               !rgb_ok    ? rgb_ok.error() :
               !vh_ok     ? vh_ok.error() :
               !pq_ok     ? pq_ok.error() :
               !p_high_ok ? p_high_ok.error() :
               !q_high_ok ? q_high_ok.error() :
                            out_ok.error();
    }
    QRhiBuffer *const cfa_buffer = cfa_ok.value();
    QRhiBuffer *const rgb_buffer = rgb_ok.value();
    QRhiBuffer *const vh_buffer = vh_ok.value();
    QRhiBuffer *const pq_buffer = pq_ok.value();
    QRhiBuffer *const p_high_buffer = p_high_ok.value();
    QRhiBuffer *const q_high_buffer = q_high_ok.value();
    QRhiBuffer *const out_buffer = out_ok.value();
    auto source_ok = ensure_storage(rgb_source, rgb_source_bytes,
                                    static_cast<quint32>(out_pixels * sizeof(float) * 3U));
    if (!source_ok)
    {
        return source_ok.error();
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
            upload_storage(updates, cfa_buffer, cfa.data(), cfa_bytes);
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
                                                       cfa_buffer),
            QRhiShaderResourceBinding::bufferLoadStore(1, QRhiShaderResourceBinding::ComputeStage,
                                                       rgb_buffer),
            QRhiShaderResourceBinding::bufferLoadStore(2, QRhiShaderResourceBinding::ComputeStage,
                                                       vh_buffer),
            QRhiShaderResourceBinding::bufferLoadStore(3, QRhiShaderResourceBinding::ComputeStage,
                                                       pq_buffer),
            QRhiShaderResourceBinding::bufferLoadStore(4, QRhiShaderResourceBinding::ComputeStage,
                                                       p_high_buffer),
            QRhiShaderResourceBinding::bufferLoadStore(5, QRhiShaderResourceBinding::ComputeStage,
                                                       q_high_buffer),
            QRhiShaderResourceBinding::bufferLoadStore(6, QRhiShaderResourceBinding::ComputeStage,
                                                       out_buffer),
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
        command->endComputePass();
    }
    auto copied = copy_rgb_window(command, nullptr, out_buffer, source_ok.value(), width, height,
                                  crop_x, crop_y, out_width, out_height);
    if (!copied)
    {
        return copied.error();
    }
    const auto out_bytes = static_cast<quint32>(out_pixels * sizeof(float) * 3U);
    QRhiResourceUpdateBatch *download = rhi->nextResourceUpdateBatch();
    download->readBackBuffer(source_ok.value(), 0, out_bytes, &readback);
    command->beginComputePass();
    command->endComputePass(download);
    retained_width = out_width;
    retained_height = out_height;
    retained_key = "rcd";
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
    if (static_cast<quint32>(readback.data.size()) != out_bytes)
    {
        return make_error(ErrorCode::kIo, "GPU readback size mismatch",
                          {{"reason", "gpu_pipeline_failed"}});
    }
    std::memcpy(rgb.data(), readback.data.constData(), out_bytes);
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
    return apply_rgb_passes(input, output, passes, GpuRgbApplyOptions{}, cancellation);
}

Result<void> GpuAdapter::apply_rgb_passes(const std::span<const float> input,
                                          const std::span<float> output,
                                          const std::span<const GpuRgbPass> passes,
                                          const GpuRgbApplyOptions options,
                                          const CancellationToken &cancellation) const
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kUnsupported, "GPU adapter is not initialized",
                          {{"reason", "gpu_unavailable"}});
    }
    return impl_->apply_passes(input, output, passes, options, cancellation);
}

Result<void> GpuAdapter::demosaic_rcd(const std::span<const float> cfa, const std::span<float> rgb,
                                      const std::uint32_t width, const std::uint32_t height,
                                      const std::array<std::uint8_t, 4> pattern,
                                      const CancellationToken &cancellation) const
{
    return demosaic_rcd(cfa, rgb, width, height, pattern, 0U, 0U, width, height, cancellation);
}

Result<void> GpuAdapter::demosaic_rcd(const std::span<const float> cfa, const std::span<float> rgb,
                                      const std::uint32_t width, const std::uint32_t height,
                                      const std::array<std::uint8_t, 4> pattern,
                                      const std::uint32_t crop_x, const std::uint32_t crop_y,
                                      const std::uint32_t crop_width,
                                      const std::uint32_t crop_height,
                                      const CancellationToken &cancellation) const
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kUnsupported, "GPU adapter is not initialized",
                          {{"reason", "gpu_unavailable"}});
    }
    return impl_->demosaic_rcd(cfa, rgb, width, height, pattern, crop_x, crop_y, crop_width,
                               crop_height, cancellation);
}

bool GpuAdapter::has_retained_source(const std::uint32_t width,
                                     const std::uint32_t height) const noexcept
{
    if (impl_ == nullptr)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->retained_width != 0U && impl_->retained_height != 0U &&
           static_cast<std::uint64_t>(impl_->retained_width) * impl_->retained_height ==
               static_cast<std::uint64_t>(width) * height;
}

std::string_view GpuAdapter::retained_source_key() const noexcept
{
    if (impl_ == nullptr)
    {
        return {};
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->retained_key;
}

Result<void> GpuAdapter::retain_source_rgb(const std::span<const float> rgb,
                                           const std::uint32_t width, const std::uint32_t height,
                                           const CancellationToken &cancellation,
                                           const std::string_view key) const
{
    GpuRgbApplyOptions options;
    options.from_retained_source = false;
    options.download = false;
    options.publish_display = false;
    options.width = width;
    options.height = height;
    options.retained_key = std::string(key);
    std::vector<float> ignored;
    GpuRgbPass pass;
    pass.kind = GpuRgbPass::Kind::kAffine;
    pass.affine.scale = 1.0F;
    pass.affine.black = 0.0F;
    return apply_rgb_passes(rgb, ignored, std::span<const GpuRgbPass>(&pass, 1U), options,
                            cancellation);
}

GpuDisplayFrame GpuAdapter::display_frame(const std::uint32_t slot) const noexcept
{
    GpuDisplayFrame frame;
    if (impl_ == nullptr || slot >= Impl::kDisplaySlots)
    {
        return frame;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    frame.width = impl_->display_width[slot];
    frame.height = impl_->display_height[slot];
    frame.generation = impl_->display_generation[slot];
    frame.backend = impl_->backend;
#if defined(Q_OS_MACOS)
    const auto read = impl_->display_read[slot] % Impl::kDisplayBuffers;
    frame.native_surface = reinterpret_cast<std::uint64_t>(impl_->display_surface[slot][read]);
#endif
    return frame;
}

} // namespace ravo
