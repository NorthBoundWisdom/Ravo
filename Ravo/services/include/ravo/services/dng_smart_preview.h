#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"

namespace ravo
{

// ADR-0141: Copy-mode DNG conversion + browse-only Smart Preview contracts.
// First Ready fails closed without a packaged converter/encoder.

inline constexpr std::string_view kDngConversionContractVersion = "ravo.dng.conversion/v1";
inline constexpr std::string_view kSmartPreviewContractVersion = "ravo.smart-preview/v1";

struct DngConversionRequest
{
    std::string asset_id;
    std::optional<std::string> output_path;
    bool user_initiated = false;
    CancellationToken cancellation{};
};

struct DngConversionResult
{
    std::string schema{std::string(kDngConversionContractVersion)};
    std::string asset_id;
    std::string source_path;
    bool originals_unchanged = true;
    bool converter_available = false;
    std::string reason{"dng_converter_unavailable"};
};

struct SmartPreviewStatus
{
    std::string schema{std::string(kSmartPreviewContractVersion)};
    std::string asset_id;
    bool encoder_available = false;
    bool present = false;
    bool develop_fallback = false;
    std::optional<std::string> path;
    std::string reason{"smart_preview_encoder_unavailable"};
};

struct SmartPreviewEnsureRequest
{
    std::string asset_id;
    bool user_initiated = false;
    CancellationToken cancellation{};
};

[[nodiscard]] bool dng_converter_is_packaged() noexcept;
[[nodiscard]] bool smart_preview_encoder_is_packaged() noexcept;

} // namespace ravo
