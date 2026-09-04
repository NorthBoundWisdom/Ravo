#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/color.h"
#include "ravo/foundation/error.h"
#include "ravo/foundation/json.h"

namespace ravo
{

// ADR-0144: on-screen monitor ICC presentation. Never mutates recipe, history,
// catalog revision, settled preview authority, or export profile.

inline constexpr std::string_view kDisplayPresentationContractVersion =
    "ravo.display.presentation/v1";

enum class DisplayProfileSource : std::uint8_t
{
    kSystemMonitor = 0,
    kInjectedPath = 1,
    kSyntheticMatrix = 2,
    kSyntheticLut = 3,
    kFallbackSrgb = 4,
};

[[nodiscard]] constexpr std::string_view
display_profile_source_name(DisplayProfileSource source) noexcept
{
    switch (source)
    {
    case DisplayProfileSource::kSystemMonitor:
        return "system_monitor";
    case DisplayProfileSource::kInjectedPath:
        return "injected_path";
    case DisplayProfileSource::kSyntheticMatrix:
        return "synthetic_matrix";
    case DisplayProfileSource::kSyntheticLut:
        return "synthetic_lut";
    case DisplayProfileSource::kFallbackSrgb:
        return "fallback_srgb";
    }
    return "fallback_srgb";
}

struct DisplayPresentationState
{
    std::string contract_version{std::string(kDisplayPresentationContractVersion)};
    std::string screen_token{"primary"};
    DisplayProfileSource source = DisplayProfileSource::kFallbackSrgb;
    std::string reason{"uninitialized"};
    ColorProfileState monitor_profile{};
    std::string profile_fingerprint;
    bool valid = false;
};

struct DisplayPresentationRgb8
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgb8;
    ColorProfileState color_profile; // presentation / monitor profile after convert
};

// Resolve presentation profile. OS discovery may fall back to sRGB with an
// explicit reason; injectable/synthetic paths never claim system_monitor.
// macOS: map a global desktop point to cg:<CGDirectDisplayID>. Other hosts
// return "primary". Studio uses this when the window's screen changes.
[[nodiscard]] std::string macos_display_screen_token_for_point(double global_x, double global_y);

[[nodiscard]] std::optional<std::uint32_t>
parse_macos_cg_screen_token(std::string_view screen_token);

[[nodiscard]] std::string make_macos_cg_screen_token(std::uint32_t display_id);

[[nodiscard]] Result<DisplayPresentationState>
discover_monitor_presentation(std::string_view screen_token = "primary");

[[nodiscard]] Result<DisplayPresentationState>
inject_monitor_presentation_from_icc_path(std::string_view path,
                                          std::string_view screen_token = "injected");

// 3x3 row-major RGB matrix applied in encoded sRGB-approx space for CPU
// reference tests (documented synthetic path, not a real display ICC).
[[nodiscard]] Result<DisplayPresentationState>
make_synthetic_matrix_monitor_presentation(const std::array<float, 9> &matrix_rgb,
                                           std::string_view screen_token = "synthetic-matrix");

// Identity LUT size marker for CPU reference; apply path is bit-exact copy with
// synthetic_lut source identity (expand later for non-identity LUTs).
[[nodiscard]] Result<DisplayPresentationState>
make_synthetic_lut_monitor_presentation(std::uint32_t lut_size,
                                        std::string_view screen_token = "synthetic-lut");

[[nodiscard]] Result<DisplayPresentationState>
refresh_monitor_presentation(const DisplayPresentationState &previous,
                             std::string_view new_screen_token);

// Convert output-profiled (or soft-proofed) RGB8 to monitor presentation RGB8.
// Does not read or write Recipe / DevelopParams.
[[nodiscard]] Result<DisplayPresentationRgb8>
apply_display_presentation_rgb8(const std::vector<std::uint8_t> &source_rgb8, std::uint32_t width,
                                std::uint32_t height, const ColorProfileState &source_profile,
                                const DisplayPresentationState &presentation,
                                const CancellationToken &cancellation);

[[nodiscard]] JsonValue display_presentation_state_to_json(const DisplayPresentationState &state);

} // namespace ravo
