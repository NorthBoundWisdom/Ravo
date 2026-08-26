#pragma once

#include "ravo/recipe/develop.h"

namespace ravo::test
{

[[nodiscard]] inline ColorBalanceRgbParams color_balance_0083_params()
{
    // Static little-endian decode of the 128-byte schema-v4 blob in
    // legacy/tests/0083-colorbalancergb/colorbalancergb.xmp. Schema v4 predates the
    // formula field; current frozen migration semantics select the default DT UCS path.
    ColorBalanceRgbParams params;
    params.shadows_y = -0.25450000166893005;
    params.shadows_chroma = 0.058600004762411118;
    params.shadows_hue = 213.10511779785156;
    params.midtones_y = 0.0;
    params.midtones_chroma = 0.0087000001221895218;
    params.midtones_hue = 288.49560546875;
    params.highlights_y = 0.22280001640319824;
    params.highlights_chroma = 0.031199997290968895;
    params.highlights_hue = 243.85670471191406;
    params.global_y = 0.012700002640485764;
    params.global_chroma = 0.0043999999761581421;
    params.global_hue = 47.639701843261719;
    params.shadows_falloff = 1.1750999689102173;
    params.white_fulcrum_ev = 0.32590007781982422;
    params.highlights_falloff = 0.69709998369216919;
    params.chroma_shadows = 0.2518000602722168;
    params.chroma_highlights = -0.40170001983642578;
    params.chroma_global = 0.099699974060058594;
    params.chroma_midtones = -0.46990001201629639;
    params.saturation_global = 0.078199982643127441;
    params.saturation_highlights = 0.065800011157989502;
    params.saturation_midtones = -0.1200999915599823;
    params.saturation_shadows = 0.073799997568130493;
    params.hue_rotation = 33.49761962890625;
    params.brilliance_global = -0.092299997806549072;
    params.brilliance_highlights = 0.068399995565414429;
    params.brilliance_midtones = -0.068199992179870605;
    params.brilliance_shadows = 0.083599984645843506;
    params.mask_grey_fulcrum = 0.37409999966621399;
    params.vibrance = -0.16479998826980591;
    params.grey_fulcrum = 0.23669999837875366;
    params.contrast = 0.1371999979019165;
    return params;
}

[[nodiscard]] inline ColorBalanceRgbParams color_balance_0093_params()
{
    // Static little-endian decode of the schema-v5 DT UCS blob in
    // legacy/tests/0093-colorbalancergb-ucs/colorbalancergb-ucs.xmp.
    ColorBalanceRgbParams params;
    params.shadows_falloff = 1.0;
    params.highlights_falloff = 1.0;
    params.chroma_shadows = 0.5;
    params.chroma_highlights = -0.30000001192092896;
    params.chroma_global = 0.30000001192092896;
    params.chroma_midtones = 0.5;
    params.saturation_global = 0.29999995231628418;
    params.saturation_highlights = -0.30000001192092896;
    params.saturation_midtones = 0.5;
    params.saturation_shadows = 0.5;
    params.brilliance_highlights = 0.10000002384185791;
    params.brilliance_midtones = 0.0099999904632568359;
    params.brilliance_shadows = -0.10000002384185791;
    params.mask_grey_fulcrum = 0.18449999392032623;
    params.vibrance = 0.19999998807907104;
    params.grey_fulcrum = 0.18449999392032623;
    params.saturation_formula = std::string(kColorBalanceRgbFormulaDtUcs2022);
    return params;
}

} // namespace ravo::test
