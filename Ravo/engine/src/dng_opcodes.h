#pragma once

// This product includes DNG technology under license by Adobe.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <variant>
#include <vector>

#include "image_ops.h"

namespace ravo
{

inline constexpr std::size_t kMaxDngOpcodeListBytes = 4U * 1024U * 1024U;
inline constexpr float kMaxDngGain = 64.0F;

struct DngGainMap
{
    std::uint32_t top = 0;
    std::uint32_t left = 0;
    std::uint32_t bottom = 0;
    std::uint32_t right = 0;
    std::uint32_t plane = 0;
    std::uint32_t planes = 0;
    std::uint32_t row_pitch = 0;
    std::uint32_t column_pitch = 0;
    std::uint32_t map_points_vertical = 0;
    std::uint32_t map_points_horizontal = 0;
    double map_spacing_vertical = 0.0;
    double map_spacing_horizontal = 0.0;
    double map_origin_vertical = 0.0;
    double map_origin_horizontal = 0.0;
    std::uint32_t map_planes = 0;
    std::vector<float> gains;
};

struct DngWarpRectilinear
{
    std::uint32_t planes = 0;
    std::array<std::array<double, 6>, 3> coefficients{};
    std::array<double, 2> center{};
};

struct DngFixVignetteRadial
{
    std::array<double, 5> coefficients{};
    std::array<double, 2> center{};
};

using DngOpcodeList3Operation =
    std::variant<DngWarpRectilinear, DngFixVignetteRadial, DngGainMap>;

struct DngSkippedOptionalOpcode
{
    std::uint32_t list = 0;
    std::uint32_t index = 0;
    std::uint32_t id = 0;
    std::uint32_t minimum_version = 0;
    std::uint32_t flags = 0;
};

struct DngOpcodeMetadata
{
    bool list2_present = false;
    bool list3_present = false;
    std::uint32_t source_width = 0U;
    std::uint32_t source_height = 0U;
    // Both lists preserve file order. List2 currently accepts GainMap only;
    // List3 accepts GainMap and the evidenced geometric corrections.
    std::vector<DngGainMap> list2_gain_maps;
    std::vector<DngOpcodeList3Operation> list3_operations;
    std::vector<DngSkippedOptionalOpcode> skipped_optional;
};

struct DngOpcodeListView
{
    bool present = false;
    std::span<const std::uint8_t> bytes;
};

// Parser layout follows Adobe DNG's big-endian opcode envelope. ART
// 6f511409a (rtengine/gainmap.cc), RawTherapee 498f62378
// (rtengine/imagedata.cc/lensmetadata.cc), and frozen legacy dng_opcode.c were
// read as implementation evidence. Bounds, overflow, mandatory-opcode, finite,
// and duplicate handling are Ravo-owned and intentionally stricter.
[[nodiscard]] Result<std::shared_ptr<const DngOpcodeMetadata>>
parse_dng_opcode_metadata(DngOpcodeListView list2, DngOpcodeListView list3,
                          std::uint32_t raw_width, std::uint32_t raw_height);

// Applies the ordered OpcodeList2 GainMaps to one normalized CFA sample and
// enforces DNG's logical [0, 1] range after every opcode.
[[nodiscard]] float apply_dng_opcode_list2_sample(const DngOpcodeMetadata &metadata,
                                                  std::uint32_t x, std::uint32_t y,
                                                  std::uint32_t raw_width,
                                                  std::uint32_t raw_height,
                                                  float normalized_sample) noexcept;

[[nodiscard]] std::size_t dng_gain_map_count(const DngOpcodeMetadata &metadata) noexcept;

// OpcodeList3 is applied after demosaic while samples are still camera RGB.
// The source is passed by value so every failure/cancellation path discards
// partial local work and publishes nothing.
[[nodiscard]] Result<WorkingImage>
apply_dng_opcode_list3(WorkingImage input, const DngOpcodeMetadata &metadata,
                       const CancellationToken &cancellation);

[[nodiscard]] std::uint64_t
estimate_dng_opcode_memory(const DngOpcodeMetadata &metadata) noexcept;

} // namespace ravo
