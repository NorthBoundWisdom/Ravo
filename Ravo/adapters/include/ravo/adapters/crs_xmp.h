#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/adapters/legacy_xmp.h"
#include "ravo/foundation/error.h"
#include "ravo/recipe/crs_types.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

// True for ProcessVersion strings accepted by the PV2012 recipe-field dialect
// (ADR-0143). Absent ProcessVersion is not "supported" by this predicate.
[[nodiscard]] bool crs_process_version_is_supported(std::string_view process_version) noexcept;

// Classifies crs:ProcessVersion without applying a look. Malformed / non-CRS
// documents fail closed; version class is still filled when the CRS namespace
// parses and only the ProcessVersion gate fails.
[[nodiscard]] Result<CrsProcessVersionInfo> classify_crs_process_version(std::string_view xmp_utf8);

inline constexpr std::string_view kCrsNamespaceUri = "http://ns.adobe.com/camera-raw-settings/1.0/";

struct CrsLookMask
{
    bool white_balance = false;
    bool exposure = false;
    bool contrast = false;
    bool highlights = false;
    bool shadows = false;
    bool whites = false;
    bool blacks = false;
    bool vibrance = false;
    bool saturation = false;
    bool clarity = false;
    bool dehaze = false;
    bool color_eq_hue = false;
    bool color_eq_sat = false;
    bool color_eq_light = false;
    bool split_toning = false;
    bool rgb_curve = false;
    bool tone_curve = false;
    bool primaries = false;
    bool sharpen = false;
    bool denoise = false;
    bool vignette = false;
    bool grain = false;
    bool grayscale = false;
};

struct CrsImportResult
{
    Recipe recipe;
    DevelopParams look;
    CrsLookMask mask;
    std::string name;
    std::vector<CrsOmission> omitted;
};

[[nodiscard]] bool is_crs_xmp_document(std::string_view xmp_utf8) noexcept;
[[nodiscard]] Result<std::string> crs_xmp_preset_name(std::string_view xmp_utf8);

struct CrsExportRequest
{
    DevelopParams look;
    std::string_view preset_name = "Ravo";
};

struct CrsExportResult
{
    std::string xmp_utf8;
    std::vector<std::string> omitted_catalog_fields;
};

// ADR-0156: stable reason when CRS/XMP cannot represent Ravo multi-instance
// Exposure or Color Balance RGB without approximation. Empty / singleton
// instance vectors are representable as the existing singleton look mapping.
[[nodiscard]] std::optional<std::string>
crs_xmp_unrepresentable_multi_instance_reason(const DevelopParams &look) noexcept;

// Serializes the CRS-mapped Develop look subset to a deterministic PV2012 XMP
// packet. Unmapped catalog features are listed in omitted_catalog_fields; the
// catalog remains the live authority for those fields.
[[nodiscard]] Result<CrsExportResult> export_crs_xmp(const CrsExportRequest &request);

// Parses Adobe CRS XMP (Lightroom/ACR presets and sidecars) onto DevelopParams.
// Unknown crs keys, Kelvin/tint, custom DCP, and non-identity unsupported looks
// fail closed. CameraProfile Adobe Standard is omitted, not applied.
[[nodiscard]] Result<CrsImportResult> import_crs_xmp(const LegacyXmpImportRequest &request);

void apply_crs_look(DevelopParams &dest, const DevelopParams &look, const CrsLookMask &mask);

} // namespace ravo
