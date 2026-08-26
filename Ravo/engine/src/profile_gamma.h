#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

#include "input_color.h"
#include "ravo/recipe/profile_gamma.h"

namespace ravo
{

inline constexpr std::size_t kProfileGammaLutEntries = 0x10000U;

enum class ProfileGammaRenderMode
{
    kLogarithmic,
    kGamma,
};

// Render-local frozen CPU state. The gamma table uses the source's 65,536
// samples at k / 65,536 and the extrapolation keeps the fitted a*x^g form.
struct ProfileGammaDerived
{
    ProfileGammaRenderMode mode = ProfileGammaRenderMode::kLogarithmic;
    float dynamic_range = 0.0F;
    float grey = 0.0F;
    float shadows_range = 0.0F;
    std::vector<float> table;
    std::array<float, 3> unbounded_coefficients{};
};

[[nodiscard]] float profile_gamma_fastlog2(float value) noexcept;
[[nodiscard]] Result<ProfileGammaDerived>
derive_profile_gamma(const ProfileGammaParams &params, const CancellationToken &cancellation);
[[nodiscard]] Result<std::optional<ProfileGammaParams>> resolve_profile_gamma(const Recipe &recipe);

// `input` is source/pre-input RGB: raster samples are encoded RGB while RAW
// samples are camera RGB after demosaic. Its source colour state is copied
// verbatim into the owned result; the operation does not select or alter a
// profile and intentionally needs no matrix representation.
[[nodiscard]] Result<ProfiledColorBuffer>
apply_profile_gamma(const ProfiledColorBuffer &input, const ProfileGammaParams &params,
                    const CancellationToken &cancellation);
[[nodiscard]] Result<ProfiledColorBuffer>
apply_profile_gamma(const ProfiledColorBuffer &input, const OperationInstance &operation,
                    const CancellationToken &cancellation);

} // namespace ravo
