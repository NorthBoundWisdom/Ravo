#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ravo
{

// Shared CRS interchange records. They live in recipe so adapters can emit
// them and services/CLI can surface them without services public headers
// depending on adapters (ADR-0120 / ADR-0143).
struct CrsOmission
{
    std::string key;
    std::string value;
    std::string reason;
};

enum class CrsProcessVersionClass : std::uint8_t
{
    kAbsent = 0,
    kSupportedPv2012,
    kUnsupported,
};

[[nodiscard]] constexpr std::string_view
crs_process_version_class_name(CrsProcessVersionClass value) noexcept
{
    switch (value)
    {
    case CrsProcessVersionClass::kAbsent:
        return "absent";
    case CrsProcessVersionClass::kSupportedPv2012:
        return "supported-pv2012";
    case CrsProcessVersionClass::kUnsupported:
        return "unsupported";
    }
    return "absent";
}

struct CrsProcessVersionInfo
{
    CrsProcessVersionClass version_class = CrsProcessVersionClass::kAbsent;
    std::optional<std::string> process_version;
    std::optional<std::string> reason;
};

} // namespace ravo
