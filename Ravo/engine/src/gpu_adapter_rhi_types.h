#pragma once

#include <QtCore/qtypes.h>

namespace ravo::gpu_adapter_rhi_internal
{

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

struct ContrastUniforms
{
    quint32 pixel_count = 0;
    quint32 pad0[3] = {};
    float amount = 0.0F;
    float pad1[3] = {};
};

struct GammaUniforms
{
    quint32 sample_count = 0;
    quint32 pad0[3] = {};
    float exponent = 1.0F;
    float pad1[3] = {};
};

struct VibranceSaturationUniforms
{
    quint32 pixel_count = 0;
    quint32 pad0[3] = {};
    float vibrance_amount = 0.0F;
    float saturation_amount = 0.0F;
    float pad1[2] = {};
};

struct VelviaUniforms
{
    quint32 pixel_count = 0;
    quint32 pad0[3] = {};
    float strength = 0.0F;
    float bias = 1.0F;
    float pad1[2] = {};
};

struct SplitToningUniforms
{
    quint32 pixel_count = 0;
    quint32 pad0[3] = {};
    float shadow_hue = 0.0F;
    float shadow_saturation = 0.5F;
    float highlight_hue = 0.2F;
    float highlight_saturation = 0.5F;
    float balance = 0.5F;
    float compression = 0.0F;
    float mix = 1.0F;
    float pad1 = 0.0F;
};

struct ColorContrastUniforms
{
    quint32 pixel_count = 0;
    quint32 unbound = 1;
    quint32 pad0[2] = {};
    float a_steepness = 1.0F;
    float a_offset = 0.0F;
    float b_steepness = 1.0F;
    float b_offset = 0.0F;
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

static_assert(sizeof(LightUniforms) == 32U, "light UBO must match std140 vec4 packing");
static_assert(sizeof(ContrastUniforms) == 32U, "contrast UBO must match std140 vec4 packing");
static_assert(sizeof(GammaUniforms) == 32U, "gamma UBO must match std140 vec4 packing");
static_assert(sizeof(VibranceSaturationUniforms) == 32U,
              "vibrance/saturation UBO must match std140 vec4 packing");
static_assert(sizeof(VelviaUniforms) == 32U, "velvia UBO must match std140 vec4 packing");
static_assert(sizeof(SplitToningUniforms) == 48U,
              "split toning UBO must match std140 vec4 packing");
static_assert(sizeof(ColorContrastUniforms) == 32U,
              "color contrast UBO must match std140 vec4 packing");
static_assert(sizeof(SharpenUniforms) == 144U, "sharpen UBO must match std140 kernel[7]");

} // namespace ravo::gpu_adapter_rhi_internal
