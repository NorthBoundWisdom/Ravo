/*
    This file is part of darktable,
    Copyright (C) 2018-2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/usermanual_url.h"
#include "common/darktable.h"
#include "common/utility.h"
#include "control/conf.h"
#include <glib.h>

typedef struct _help_url
{
    char *name;
    char *url;
} dt_help_url;

dt_help_url urls_db[] = {
    {"document_root", "/"},
    {"ratings", "lighttable/digital-asset-management/star-color/#star-ratings"},
    {"layout_filemanager", "lighttable/lighttable-modes/filemanager/"},
    {"layout_culling", "lighttable/lighttable-modes/culling/"},
    {"layout_preview", "lighttable/lighttable-modes/full-preview/"},
    {"colorlabels", "lighttable/digital-asset-management/star-color/#color-labels"},
    {"import", "module-reference/utility-modules/lighttable/import/"},
    {"import_dialog", "module-reference/utility-modules/lighttable/import/#import-dialog"},
    {"select", "module-reference/utility-modules/lighttable/select/"},
    {"image", "module-reference/utility-modules/lighttable/selected-image/"},
    {"copy_history", "module-reference/utility-modules/lighttable/history-stack/"},
    {"styles", "module-reference/utility-modules/lighttable/styles/#module-controls"},
    {"metadata", "module-reference/utility-modules/shared/metadata-editor/"},
    {"metadata_preferences",
     "module-reference/utility-modules/shared/metadata-editor/#preferences"},
    {"tagging", "module-reference/utility-modules/shared/tagging/"},
    {"recentcollect", "module-reference/utility-modules/shared/recent-collections/"},
    {"metadata_view", "module-reference/utility-modules/shared/image-information/"},
    {"export", "module-reference/utility-modules/shared/export/"},
    {"export_dialog", "module-reference/utility-modules/shared/export/#metadata-preferences"},
    {"histogram", "module-reference/utility-modules/shared/scopes/"},
    {"navigation", "module-reference/utility-modules/darkroom/navigation/"},
    {"snapshots", "module-reference/utility-modules/darkroom/snapshots/"},
    {"history", "module-reference/utility-modules/darkroom/history-stack/"},
    {"masks", "module-reference/utility-modules/darkroom/mask-manager/"},
    {"modulegroups", "darkroom/organization/manage-module-layouts/"},
    {"masks_drawn", "darkroom/masking-and-blending/masks/drawn/"},
    {"masks_parametric", "darkroom/masking-and-blending/masks/parametric/"},
    {"masks_raster", "darkroom/masking-and-blending/masks/raster/"},
    {"masks_blending_op", "darkroom/masking-and-blending/masks/drawn-and-parametric/"},
    {"masks_blending", "darkroom/masking-and-blending/overview/"},
    {"masks_combined", "darkroom/masking-and-blending/masks/drawn-and-parametric/"},
    {"masks_refinement", "darkroom/masking-and-blending/masks/refinement-controls/"},
    {"import_fr", "overview/workflow/import-rate-tag/"},
    {"lighttable_mode", "lighttable/overview/"},
    {"lighttable_filemanager", "lighttable/lighttable-modes/filemanager/"},
    {"darkroom_bottom_panel", "darkroom/darkroom-view-layout/#bottom-panel"},
    {"module_header", "darkroom/processing-modules/module-header/"},
    {"module_toolbox", NULL},
    {"view_toolbox", NULL},
    {"backgroundjobs", NULL},
    {"hinter", NULL},
    {"filter", NULL},
    {"filmstrip", "overview/user-interface/filmstrip/"},
    {"viewswitcher", "overview/user-interface/views/"},
    {"favorite_presets", "darkroom/darkroom-view-layout/#bottom-panel"},
    {"bottom_panel_styles", "darkroom/darkroom-view-layout/#bottom-panel"},
    {"rawoverexposed", "module-reference/utility-modules/darkroom/raw-overexposed/"},
    {"overexposed", "module-reference/utility-modules/darkroom/clipping/"},
    {"softproof", "module-reference/utility-modules/darkroom/soft-proof/"},
    {"gamut", "module-reference/utility-modules/darkroom/gamut/"},
    {"shortcuts", "preferences-settings/shortcuts/"},
    {"presets", "preferences-settings/presets/"},
    {"css_tweaks", "preferences-settings/general/#css-theme-modifications"},
    {"preset_dialog", "darkroom/processing-modules/presets/#creating-and-editing-presets"},
    {"message_log", "overview/user-interface/bottom-panel/#common-controls"},

    // iop links
    {"agx", "module-reference/processing-modules/agx/"},
    {"ashift", "module-reference/processing-modules/rotate-perspective/"},
    {"atrous", "module-reference/processing-modules/contrast-equalizer/"},
    {"basecurve", "module-reference/processing-modules/base-curve/"},
    {"bilateral", "module-reference/processing-modules/surface-blur/"},
    {"bilat", "module-reference/processing-modules/local-contrast/"},
    {"bloom", "module-reference/processing-modules/bloom/"},
    {"borders", "module-reference/processing-modules/framing/"},
    {"cacorrect", "module-reference/processing-modules/raw-chromatic-aberrations/"},
    {"cacorrectrgb", "module-reference/processing-modules/chromatic-aberrations/"},
    {"censorize", "module-reference/processing-modules/censorize/"},
    {"channelmixerrgb", "module-reference/processing-modules/color-calibration/"},
    {"colorbalance", "module-reference/processing-modules/color-balance/"},
    {"colorbalancergb", "module-reference/processing-modules/color-balance-rgb/"},
    {"colorchecker", "module-reference/processing-modules/color-look-up-table/"},
    {"colorcontrast", "module-reference/processing-modules/color-contrast/"},
    {"colorcorrection", "module-reference/processing-modules/color-correction/"},
    {"colorequal", "module-reference/processing-modules/color-equalizer/"},
    {"colorin", "module-reference/processing-modules/input-color-profile/"},
    {"colorize", "module-reference/processing-modules/colorize/"},
    {"colormapping", "module-reference/processing-modules/color-mapping/"},
    {"colorout", "module-reference/processing-modules/output-color-profile/"},
    {"colorreconstruct", "module-reference/processing-modules/color-reconstruction/"},
    {"colorzones", "module-reference/processing-modules/color-zones/"},
    {"crop", "module-reference/processing-modules/crop/"},
    {"demosaic", "module-reference/processing-modules/demosaic/"},
    {"denoiseprofile", "module-reference/processing-modules/denoise-profiled/"},
    {"diffuse", "module-reference/processing-modules/diffuse/"},
    {"dither", "module-reference/processing-modules/dithering/"},
    {"enlargecanvas", "module-reference/processing-modules/enlarge-canvas/"},
    {"exposure", "module-reference/processing-modules/exposure/"},
    {"filmicrgb", "module-reference/processing-modules/filmic-rgb/"},
    {"sigmoid", "module-reference/processing-modules/sigmoid/"},
    {"flip", "module-reference/processing-modules/orientation/"},
    {"graduatednd", "module-reference/processing-modules/graduated-density/"},
    {"grain", "module-reference/processing-modules/grain/"},
    {"hazeremoval", "module-reference/processing-modules/haze-removal/"},
    {"highlights", "module-reference/processing-modules/highlight-reconstruction/"},
    {"highpass", "module-reference/processing-modules/highpass/"},
    {"hotpixels", "module-reference/processing-modules/hot-pixels/"},
    {"lens", "module-reference/processing-modules/lens-correction/"},
    {"liquify", "module-reference/processing-modules/liquify/"},
    {"lowlight", "module-reference/processing-modules/lowlight-vision/"},
    {"lowpass", "module-reference/processing-modules/lowpass/"},
    {"lut3d", "module-reference/processing-modules/lut-3d/"},
    {"monochrome", "module-reference/processing-modules/monochrome/"},
    {"negadoctor", "module-reference/processing-modules/negadoctor/"},
    {"nlmeans", "module-reference/processing-modules/astrophoto-denoise/"},
    {"overlay", "module-reference/processing-modules/composite/"},
    {"primaries", "module-reference/processing-modules/rgb-primaries/"},
    {"profile_gamma", "module-reference/processing-modules/unbreak-input-profile/"},
    {"rawdenoise", "module-reference/processing-modules/raw-denoise/"},
    {"rawprepare", "module-reference/processing-modules/raw-black-white-point/"},
    {"retouch", "module-reference/processing-modules/retouch/"},
    {"rgbcurve", "module-reference/processing-modules/rgb-curve/"},
    {"rgblevels", "module-reference/processing-modules/rgb-levels/"},
    {"rotatepixels", "module-reference/processing-modules/rotate-pixels/"},
    {"scalepixels", "module-reference/processing-modules/scale-pixels/"},
    {"shadhi", "module-reference/processing-modules/shadows-and-highlights/"},
    {"sharpen", "module-reference/processing-modules/sharpen/"},
    {"soften", "module-reference/processing-modules/soften/"},
    {"splittoning", "module-reference/processing-modules/split-toning/"},
    {"temperature", "module-reference/processing-modules/white-balance/"},
    {"tonecurve", "module-reference/processing-modules/tone-curve/"},
    {"toneequal", "module-reference/processing-modules/tone-equalizer/"},
    {"velvia", "module-reference/processing-modules/velvia/"},
    {"vignette", "module-reference/processing-modules/vignetting/"},
    {"watermark", "module-reference/processing-modules/watermark/"},
};

char *dt_get_help_url(const char *name)
{
    if (name == NULL)
        return NULL;

    for (int k = 0; k < sizeof(urls_db) / 2 / sizeof(char *); k++)
        if (!strcmp(urls_db[k].name, name))
            return urls_db[k].url;

    return NULL;
}

static char *_get_base_url()
{
    const gboolean use_default_url = dt_conf_get_bool("context_help/use_default_url");
    const char *c_base_url = dt_confgen_get("context_help/url", DT_DEFAULT);
    char *base_url = dt_conf_get_string("context_help/url");

    if (use_default_url)
    {
        // want to use default URL, reset darktablerc
        dt_conf_set_string("context_help/url", c_base_url);
        g_free(base_url);
        return g_strdup(c_base_url);
    }
    else
        return base_url;
}

char *dt_get_manual_base_url()
{
    char *base_url = _get_base_url();

    // The base URL comes from the active project documentation setting.
    // The full format for the documentation pages is:
    //    <base-url>/<ver>/<lang>[/path/to/page]
    // Where:
    //   <ver>  = development | 3.6 | 3.8 ...
    //   <lang> = en / fr ...              (default = en)

    // in case of a standard release, append the dt version to the url
    if (dt_is_dev_version())
    {
        dt_util_str_cat(&base_url, "development/");
    }
    else
    {
        char *ver = dt_version_major_minor();
        dt_util_str_cat(&base_url, "%s/", ver);
        g_free(ver);
    }
    return base_url;
}

char *dt_get_manual_url(const char *topic)
{
    char *base_url = dt_get_manual_base_url();
    char *url = g_build_path("/", base_url, "en", topic, NULL);
    g_free(base_url);
    return url;
}
