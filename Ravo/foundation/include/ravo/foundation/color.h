#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ravo
{

enum class ColorModel
{
    kRgb,
    kXyz,
    kLab,
};

enum class ColorProfileKind
{
    kMissing,
    kBuiltin,
    kMatrix,
    kIcc,
};

// Immutable decode/render colour state. It owns profile bytes and numeric matrix
// data, but never a codec, Qt, LittleCMS, or legacy profile handle.
struct ColorProfileState
{
    ColorProfileKind kind = ColorProfileKind::kMissing;
    ColorModel model = ColorModel::kRgb;
    std::string identifier;
    std::vector<std::uint8_t> icc_bytes;
    std::array<float, 9> matrix_to_xyz_d50{1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    bool has_matrix = false;
    bool camera_input = false;

    [[nodiscard]] bool operator==(const ColorProfileState &) const noexcept = default;
};

[[nodiscard]] std::string color_profile_fingerprint(const ColorProfileState &profile);

} // namespace ravo
