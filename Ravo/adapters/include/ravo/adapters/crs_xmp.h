#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "ravo/adapters/legacy_xmp.h"
#include "ravo/foundation/error.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

inline constexpr std::string_view kCrsNamespaceUri = "http://ns.adobe.com/camera-raw-settings/1.0/";

struct CrsOmission
{
    std::string key;
    std::string value;
    std::string reason;
};

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
