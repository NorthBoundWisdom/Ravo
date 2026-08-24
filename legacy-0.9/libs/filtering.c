/*
    This file is part of darktable,
    Copyright (C) 2025-2026 darktable developers.

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

#include "bauhaus/bauhaus.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/metadata.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "dtgtk/button.h"
#include "dtgtk/range.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/collect.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "views/view.h"
#ifndef _WIN32
#include <gio/gunixmounts.h>
#endif
#ifdef GDK_WINDOWING_QUARTZ
#endif

#include <locale.h>

DT_MODULE(2)

#define PARAM_STRING_SIZE 256 // FIXME: is this enough !?
#define FILTERING_HISTORY_SLOTS 50 // former configuration-schema limit

static const dt_introspection_type_enum_tuple_t _collection_sort_names[] = {
    {N_("filename"), DT_COLLECTION_SORT_FILENAME},
    {N_("full path"), DT_COLLECTION_SORT_PATH},
    {N_("aspect ratio"), DT_COLLECTION_SORT_ASPECT_RATIO},

    {N_("capture time"), DT_COLLECTION_SORT_DATETIME},
    {N_("import time"), DT_COLLECTION_SORT_IMPORT_TIMESTAMP},
    {N_("modification time"), DT_COLLECTION_SORT_CHANGE_TIMESTAMP},
    {N_("export time"), DT_COLLECTION_SORT_EXPORT_TIMESTAMP},
    {N_("print time"), DT_COLLECTION_SORT_PRINT_TIMESTAMP},

    {N_("rating"), DT_COLLECTION_SORT_RATING},
    {N_("color label"), DT_COLLECTION_SORT_COLOR},
    {N_("title"), DT_COLLECTION_SORT_TITLE},
    {N_("description"), DT_COLLECTION_SORT_DESCRIPTION},

    {N_("group"), DT_COLLECTION_SORT_GROUP},
    {N_("id"), DT_COLLECTION_SORT_ID},
    {N_("custom sort"), DT_COLLECTION_SORT_CUSTOM_ORDER},
    {N_("shuffle"), DT_COLLECTION_SORT_SHUFFLE},
    {}};

typedef enum _preset_save_type_t
{
    _PRESET_NONE = 0,
    _PRESET_FILTERS = 1 << 0,
    _PRESET_SORT = 1 << 1,
    _PRESET_ALL = _PRESET_FILTERS | _PRESET_SORT
} _preset_save_type_t;

typedef struct _widgets_sort_t
{
    dt_collection_sort_t sortid;
    GtkWidget *box;
    GtkWidget *sort;
    GtkWidget *direction;
    GtkWidget *close;

    int num;
    struct dt_lib_filtering_t *lib;
} _widgets_sort_t;

typedef struct dt_lib_filtering_rule_t
{
    int num;

    dt_collection_properties_t prop;

    GtkWidget *w_main;
    GtkWidget *w_operator;
    GtkWidget *w_prop;
    GtkWidget *w_btn_box;
    GtkWidget *w_close;
    GtkWidget *w_off;

    GtkWidget *w_widget_box;
    char raw_text[PARAM_STRING_SIZE];
    GtkWidget *w_special_box;
    void *w_specific; // structure which contains all the widgets specific to the rule type
    int manual_widget_set; // when we update manually the widget, we don't want events to be handled
    gboolean cleaning; // if we have started a gui_cleanup (we don't want certain event to occurs)

    struct dt_lib_filtering_t *lib;
} dt_lib_filtering_rule_t;

typedef struct dt_lib_filtering_t
{
    dt_lib_filtering_rule_t rule[DT_COLLECTION_MAX_RULES];
    int nb_rules;

    GtkWidget *rules_box;
    GtkWidget *rules_sw;

    _widgets_sort_t sort[DT_COLLECTION_MAX_RULES];
    int nb_sort;
    GtkWidget *sort_box;
    gboolean manual_sort_set;
    gboolean leaving;

    struct dt_lib_filtering_params_t *params;

    gchar *last_where_ext;
} dt_lib_filtering_t;

typedef struct dt_lib_filtering_params_rule_t
{
    uint32_t item : 16;
    uint32_t mode : 16;
    uint32_t off : 16;
    char string[PARAM_STRING_SIZE];
} dt_lib_filtering_params_rule_t;

typedef struct dt_lib_filtering_params_sort_t
{
    uint32_t item : 16;
    uint32_t order : 16;
} dt_lib_filtering_params_sort_t;

typedef struct dt_lib_filtering_params_t
{
    uint32_t rules;
    dt_lib_filtering_params_rule_t rule[DT_COLLECTION_MAX_RULES];
    uint32_t sorts;
    dt_lib_filtering_params_sort_t sort[DT_COLLECTION_MAX_RULES];
    uint32_t preset_type;
} dt_lib_filtering_params_t;

typedef struct _widgets_range_t
{
    dt_lib_filtering_rule_t *rule;

    GtkWidget *range_select;
} _widgets_range_t;

typedef enum _tree_cols_t
{
    TREE_COL_TEXT = 0,
    TREE_COL_TOOLTIP,
    TREE_COL_PATH,
    TREE_COL_COUNT,
    TREE_NUM_COLS
} _tree_cols_t;

static void _filters_gui_update(dt_lib_module_t *self);
static void _sort_gui_update(dt_lib_module_t *self);
static void _filtering_gui_update(dt_lib_module_t *self);
static void _dt_collection_updated(gpointer instance, dt_collection_change_t query_change,
                                   dt_collection_properties_t changed_property, gpointer imgs,
                                   int next, gpointer self);
static void _rule_set_raw_text(dt_lib_filtering_rule_t *rule, const gchar *text,
                               const gboolean signal);

static void _range_changed(GtkWidget *widget, gpointer user_data);
static void _range_widget_add_to_rule(dt_lib_filtering_rule_t *rule, _widgets_range_t *special);
static void _sort_append_sort(GtkWidget *widget, dt_lib_module_t *self);

typedef void (*_widget_init_func)(dt_lib_filtering_rule_t *rule,
                                  const dt_collection_properties_t prop, const gchar *text,
                                  dt_lib_module_t *self);

typedef gboolean (*_widget_update_func)(dt_lib_filtering_rule_t *rule);
typedef struct _filter_t
{
    dt_collection_properties_t prop;
    _widget_init_func widget_init;
    _widget_update_func update;
} _filter_t;

// filters definitions
#include "libs/filters/aperture.c"
#include "libs/filters/colors.c"
#include "libs/filters/date.c"
#include "libs/filters/exposure.c"
#include "libs/filters/exposure_bias.c"
#include "libs/filters/filename.c"
#include "libs/filters/focal.c"
#include "libs/filters/history.c"
#include "libs/filters/iso.c"
#include "libs/filters/local_copy.c"
#include "libs/filters/duplicates.c"
#include "libs/filters/misc.c"
#include "libs/filters/module_order.c"
#include "libs/filters/rating.c"
#include "libs/filters/rating_range.c"
#include "libs/filters/ratio.c"
#include "libs/filters/search.c"

static _filter_t filters[] = {
    {DT_COLLECTION_PROP_COLORLABEL, _colors_widget_init, _colors_update},
    {DT_COLLECTION_PROP_FILENAME, _filename_widget_init, _filename_update},
    {DT_COLLECTION_PROP_TEXTSEARCH, _search_widget_init, _search_update},
    {DT_COLLECTION_PROP_DAY, _date_widget_init, _date_update},
    {DT_COLLECTION_PROP_MONTH, _month_widget_init, _month_update},
    {DT_COLLECTION_PROP_CHANGE_TIMESTAMP, _date_widget_init, _date_update},
    {DT_COLLECTION_PROP_EXPORT_TIMESTAMP, _date_widget_init, _date_update},
    {DT_COLLECTION_PROP_TIME, _date_widget_init, _date_update},
    {DT_COLLECTION_PROP_IMPORT_TIMESTAMP, _date_widget_init, _date_update},
    {DT_COLLECTION_PROP_PRINT_TIMESTAMP, _date_widget_init, _date_update},
    {DT_COLLECTION_PROP_ASPECT_RATIO, _ratio_widget_init, _ratio_update},
    {DT_COLLECTION_PROP_RATING_RANGE, _rating_range_widget_init, _rating_range_update},
    {DT_COLLECTION_PROP_APERTURE, _aperture_widget_init, _aperture_update},
    {DT_COLLECTION_PROP_FOCAL_LENGTH, _focal_widget_init, _focal_update},
    {DT_COLLECTION_PROP_ISO, _iso_widget_init, _iso_update},
    {DT_COLLECTION_PROP_EXPOSURE, _exposure_widget_init, _exposure_update},
    {DT_COLLECTION_PROP_EXPOSURE_BIAS, _exposure_bias_widget_init, _exposure_bias_update},
    {DT_COLLECTION_PROP_GROUP_ID, _misc_widget_init, _misc_update},
    {DT_COLLECTION_PROP_DUPLICATES, _duplicates_widget_init, _duplicates_update},
    {DT_COLLECTION_PROP_LOCAL_COPY, _local_copy_widget_init, _local_copy_update},
    {DT_COLLECTION_PROP_HISTORY, _history_widget_init, _history_update},
    {DT_COLLECTION_PROP_ORDER, _module_order_widget_init, _module_order_update},
    {DT_COLLECTION_PROP_RATING, _rating_widget_init, _rating_update},
    {DT_COLLECTION_PROP_LENS, _misc_widget_init, _misc_update},
    {DT_COLLECTION_PROP_CAMERA, _misc_widget_init, _misc_update},
    {DT_COLLECTION_PROP_WHITEBALANCE, _misc_widget_init, _misc_update},
    {DT_COLLECTION_PROP_FLASH, _misc_widget_init, _misc_update},
    {DT_COLLECTION_PROP_EXPOSURE_PROGRAM, _misc_widget_init, _misc_update},
    {DT_COLLECTION_PROP_METERING_MODE, _misc_widget_init, _misc_update}};

static _filter_t *_filters_get(const dt_collection_properties_t prop)
{
    const int nb = sizeof(filters) / sizeof(_filter_t);
    for (int i = 0; i < nb; i++)
    {
        if (filters[i].prop == prop)
            return &filters[i];
    }
    return NULL;
}

const char *name(dt_lib_module_t *self)
{
    return _("collection filters");
}

const char *description(dt_lib_module_t *self)
{
    return _("refine the set of images to display or edit");
}

void init_presets(dt_lib_module_t *self)
{
    dt_lib_filtering_params_t params;

#define CLEAR_PARAMS(t, r, s)                                                                      \
    {                                                                                              \
        memset(&params, 0, sizeof(params));                                                        \
        params.preset_type = t;                                                                    \
        params.rules = 1;                                                                          \
        params.rule[0].mode = 0;                                                                   \
        params.rule[0].off = 0;                                                                    \
        params.rule[0].item = r;                                                                   \
        params.sorts = 1;                                                                          \
        params.sort[0].item = s;                                                                   \
        params.sort[0].order = 0;                                                                  \
    }

    // initial preset
    CLEAR_PARAMS(_PRESET_ALL, DT_COLLECTION_PROP_RATING_RANGE, DT_COLLECTION_SORT_DATETIME);
    params.rules = 3;
    params.rule[1].item = DT_COLLECTION_PROP_COLORLABEL;
    params.rule[1].mode = 0;
    params.rule[1].off = 0;
    params.rule[2].item = DT_COLLECTION_PROP_TEXTSEARCH;
    params.rule[2].mode = 0;
    params.rule[2].off = 0;
    dt_lib_presets_add(_("initial setting"), self->plugin_name, self->version(), &params,
                       sizeof(params), TRUE, 0);

    // based on aspect-ratio
    CLEAR_PARAMS(_PRESET_FILTERS, DT_COLLECTION_PROP_ASPECT_RATIO, DT_COLLECTION_SORT_DATETIME);
    g_strlcpy(params.rule[0].string, "[1;1]", PARAM_STRING_SIZE);
    dt_lib_presets_add(_("square"), self->plugin_name, self->version(), &params, sizeof(params),
                       TRUE, 0);

    CLEAR_PARAMS(_PRESET_FILTERS, DT_COLLECTION_PROP_ASPECT_RATIO, DT_COLLECTION_SORT_DATETIME);
    g_strlcpy(params.rule[0].string, ">=1.01", PARAM_STRING_SIZE);
    dt_lib_presets_add(_("landscape"), self->plugin_name, self->version(), &params, sizeof(params),
                       TRUE, 0);

    CLEAR_PARAMS(_PRESET_FILTERS, DT_COLLECTION_PROP_ASPECT_RATIO, DT_COLLECTION_SORT_DATETIME);
    g_strlcpy(params.rule[0].string, "<=0.99", PARAM_STRING_SIZE);
    dt_lib_presets_add(_("portrait"), self->plugin_name, self->version(), &params, sizeof(params),
                       TRUE, 0);

    // presets based on import
    CLEAR_PARAMS(_PRESET_FILTERS | _PRESET_SORT, DT_COLLECTION_PROP_IMPORT_TIMESTAMP,
                 DT_COLLECTION_SORT_IMPORT_TIMESTAMP);
    g_strlcpy(params.rule[0].string, "[-0000:00:01 00:00:00;now]", PARAM_STRING_SIZE);
    dt_lib_presets_add(_("imported: last 24h"), self->plugin_name, self->version(), &params,
                       sizeof(params), TRUE, 0);

    CLEAR_PARAMS(_PRESET_FILTERS | _PRESET_SORT, DT_COLLECTION_PROP_IMPORT_TIMESTAMP,
                 DT_COLLECTION_SORT_IMPORT_TIMESTAMP);
    g_strlcpy(params.rule[0].string, "[-0000:00:30 00:00:00;now]", PARAM_STRING_SIZE);
    dt_lib_presets_add(_("imported: last 30 days"), self->plugin_name, self->version(), &params,
                       sizeof(params), TRUE, 0);

    // presets based on image metadata (image taken)
    CLEAR_PARAMS(_PRESET_FILTERS | _PRESET_SORT, DT_COLLECTION_PROP_TIME,
                 DT_COLLECTION_SORT_DATETIME);
    g_strlcpy(params.rule[0].string, "[-0000:00:01 00:00:00;now]", PARAM_STRING_SIZE);
    dt_lib_presets_add(_("taken: last 24h"), self->plugin_name, self->version(), &params,
                       sizeof(params), TRUE, 0);

    CLEAR_PARAMS(_PRESET_FILTERS | _PRESET_SORT, DT_COLLECTION_PROP_TIME,
                 DT_COLLECTION_SORT_DATETIME);
    g_strlcpy(params.rule[0].string, "[-0000:00:30 00:00:00;now]", PARAM_STRING_SIZE);
    dt_lib_presets_add(_("taken: last 30 days"), self->plugin_name, self->version(), &params,
                       sizeof(params), TRUE, 0);

#undef CLEAR_PARAMS
}

static void _filtering_reset(const _preset_save_type_t reset)
{
    if (reset & _PRESET_FILTERS)
        dt_conf_set_int("plugins/lighttable/filtering/num_rules", 0);

    if (reset & _PRESET_SORT)
    {
        // we reset the sorting orders
        dt_conf_set_int("plugins/lighttable/filtering/num_sort", 1);
        dt_conf_set_int("plugins/lighttable/filtering/sort0", 0);
        dt_conf_set_int("plugins/lighttable/filtering/sortorder0", 0);
    }
}

static void _remove_topbar_state(void)
{
    char confname[200] = {0};
    for (int i = 0; i < DT_COLLECTION_MAX_RULES; i++)
    {
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/top%1d", i);
        dt_conf_remove_key(confname);
    }
}

static void _remove_history_state(void)
{
    char confname[200] = {0};
    for (int i = 0; i < FILTERING_HISTORY_SLOTS; i++)
    {
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/history%1d", i);
        dt_conf_remove_key(confname);
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sort_history%1d", i);
        dt_conf_remove_key(confname);
    }
    dt_conf_remove_key("plugins/lighttable/filtering/history_max");
    dt_conf_remove_key("plugins/lighttable/filtering/sort_history_max");
}

/* Update the params struct with active ruleset */
static void _filters_update_params(dt_lib_filtering_t *d)
{
    /* reset params */
    dt_lib_filtering_params_t *p = d->params;
    memset(p, 0, sizeof(dt_lib_filtering_params_t));
    p->preset_type = _PRESET_ALL;

    /* for each active rule set update params */
    p->rules = CLAMP(dt_conf_get_int("plugins/lighttable/filtering/num_rules"), 0,
                     DT_COLLECTION_MAX_RULES);
    char confname[200] = {0};
    for (int i = 0; i < p->rules; i++)
    {
        /* get item */
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/item%1d", i);
        p->rule[i].item = dt_conf_get_int(confname);

        /* get mode */
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/mode%1d", i);
        p->rule[i].mode = dt_conf_get_int(confname);

        /* get on-off */
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/off%1d", i);
        p->rule[i].off = dt_conf_get_int(confname);

        /* get string */
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/string%1d", i);
        const char *string = dt_conf_get_string_const(confname);
        if (string != NULL)
        {
            g_strlcpy(p->rule[i].string, string, PARAM_STRING_SIZE);
        }
    }

    /* for each sort orders set update params */
    p->sorts =
        CLAMP(dt_conf_get_int("plugins/lighttable/filtering/num_sort"), 0, DT_COLLECTION_MAX_RULES);
    for (int i = 0; i < p->sorts; i++)
    {
        /* get item */
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sort%1d", i);
        p->sort[i].item = dt_conf_get_int(confname);

        /* get sort order */
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sortorder%1d", i);
        p->sort[i].order = dt_conf_get_int(confname);
    }
}

void *get_params(dt_lib_module_t *self, int *size)
{
    _filters_update_params(self->data);

    /* allocate a copy of params to return, freed by caller */
    *size = sizeof(dt_lib_filtering_params_t);
    void *p = malloc(*size);
    memcpy(p, ((dt_lib_filtering_t *)self->data)->params, *size);
    return p;
}

int set_params(dt_lib_module_t *self, const void *params, int size)
{
    /* update conf settings from params */
    dt_lib_filtering_params_t *p = (dt_lib_filtering_params_t *)params;

    // reset conf value
    _filtering_reset(p->preset_type);

    char confname[200] = {0};
    const int nb_rules_ini = dt_conf_get_int("plugins/lighttable/filtering/num_rules");

    for (uint32_t i = 0; i < p->rules; i++)
    {
        const int pos = i + nb_rules_ini;
        /* set item */
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/item%1d", pos);
        dt_conf_set_int(confname, p->rule[i].item);

        /* set mode */
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/mode%1d", pos);
        dt_conf_set_int(confname, p->rule[i].mode);

        /* set on-off */
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/off%1d", pos);
        dt_conf_set_int(confname, p->rule[i].off);

        /* set string */
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/string%1d", pos);
        dt_conf_set_string(confname, p->rule[i].string);
    }

    /* set number of rules */
    g_strlcpy(confname, "plugins/lighttable/filtering/num_rules", sizeof(confname));
    dt_conf_set_int(confname, p->rules + nb_rules_ini);

    if (p->preset_type & _PRESET_SORT)
    {
        for (uint32_t i = 0; i < p->sorts; i++)
        {
            /* set item */
            snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sort%1u", i);
            dt_conf_set_int(confname, p->sort[i].item);

            /* set order */
            snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sortorder%1u", i);
            dt_conf_set_int(confname, p->sort[i].order);
        }

        /* set number of sorts */
        g_strlcpy(confname, "plugins/lighttable/filtering/num_sort", sizeof(confname));
        dt_conf_set_int(confname, p->sorts);
    }

    /* update internal params */
    _filters_update_params(self->data);

    /* update ui */
    _filters_gui_update(self);
    _sort_gui_update(self);

    /* update view */
    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD,
                               DT_COLLECTION_PROP_UNDEF, NULL);
    return 0;
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
    return DT_VIEW_LIGHTTABLE;
}

uint32_t container(dt_lib_module_t *self)
{
    return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

static void _conf_update_rule(dt_lib_filtering_rule_t *rule)
{
    const dt_lib_collect_mode_t mode = MAX(0, dt_bauhaus_combobox_get(rule->w_operator));
    const gboolean off = !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(rule->w_off));

    char confname[200] = {0};
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/string%1d", rule->num);
    dt_conf_set_string(confname, rule->raw_text);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/item%1d", rule->num);
    dt_conf_set_int(confname, rule->prop);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/mode%1d", rule->num);
    dt_conf_set_int(confname, mode);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/off%1d", rule->num);
    dt_conf_set_int(confname, off);

}

static void _event_rule_changed(GtkWidget *entry, dt_lib_filtering_rule_t *rule)
{
    if (rule->manual_widget_set)
        return;

    // update the config files
    _conf_update_rule(rule);

    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, rule->prop, NULL);
}

static void _rule_set_raw_text(dt_lib_filtering_rule_t *rule, const gchar *text,
                               const gboolean signal)
{
    snprintf(rule->raw_text, sizeof(rule->raw_text), "%s", (text == NULL) ? "" : text);
    if (signal)
        _event_rule_changed(NULL, rule);
}

static void _range_changed(GtkWidget *widget, gpointer user_data)
{
    _widgets_range_t *special = (_widgets_range_t *)user_data;
    if (special->rule->manual_widget_set)
        return;
    if (special->rule->lib->leaving)
        return;

    // we recreate the right raw text and put it in the raw entry
    gchar *txt = dtgtk_range_select_get_raw_text(DTGTK_RANGE_SELECT(special->range_select));
    _rule_set_raw_text(special->rule, txt, TRUE);
    g_free(txt);

}

static void _range_widget_add_to_rule(dt_lib_filtering_rule_t *rule, _widgets_range_t *special)
{
    special->rule = rule;

    // we create the static part of the tooltip
    gchar *txt = g_strdup_printf("\n<b>%s</b>\n%s\n%s", dt_collection_name(special->rule->prop),
                                 _("click or click&#38;drag to select one or multiple values"),
                                 _("right-click opens a menu to select the available values"));
    if (DTGTK_RANGE_SELECT(special->range_select)->cur_help)
        g_free(DTGTK_RANGE_SELECT(special->range_select)->cur_help);
    DTGTK_RANGE_SELECT(special->range_select)->cur_help = txt;

    gtk_box_pack_start(GTK_BOX(rule->w_special_box), special->range_select, TRUE, TRUE, 0);
    g_signal_connect(G_OBJECT(special->range_select), "value-changed", G_CALLBACK(_range_changed),
                     special);
    rule->w_specific = special;
}

static gboolean _widget_update(dt_lib_filtering_rule_t *rule)
{
    _filter_t *f = _filters_get(rule->prop);
    if (f)
        return f->update(rule);
    return FALSE;
}

static gboolean _widget_init_special(dt_lib_filtering_rule_t *rule, const gchar *text,
                                     dt_lib_module_t *self)
{
    // remove eventual existing box
    if (rule->w_special_box)
        gtk_widget_destroy(rule->w_special_box);

    // recreate the box
    rule->w_special_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *special_box = rule->w_special_box;
    gtk_box_pack_start(GTK_BOX(rule->w_widget_box), rule->w_special_box, TRUE, TRUE, 0);

    _filter_t *f = _filters_get(rule->prop);
    if (f)
        f->widget_init(rule, rule->prop, text, self);
    else
        return FALSE;

    gtk_widget_show_all(special_box);
    return TRUE;
}

static void _event_rule_change_type(GtkWidget *widget, dt_lib_module_t *self)
{
    const int mode = GPOINTER_TO_INT(dt_bauhaus_combobox_get_data(widget));
    dt_lib_filtering_rule_t *rule =
        (dt_lib_filtering_rule_t *)g_object_get_data(G_OBJECT(widget), "rule");

    if (mode == rule->prop)
        return;

    dt_collection_properties_t old = rule->prop;
    rule->prop = mode;

    // re-init the special widgets
    _widget_init_special(rule, "", self);
    _widget_update(rule);

    // reset the raw entry
    _rule_set_raw_text(rule, "", FALSE);

    // update the config files
    _conf_update_rule(rule);

    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, old, NULL);
}

static void _event_append_rule(GtkWidget *widget, dt_lib_module_t *self)
{
    // add new rule
    dt_lib_filtering_t *d = self->data;
    const int mode = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "collect_id"));
    char confname[200] = {0};

    if (mode >= 0)
    {
        // add an empty rule
        if (d->nb_rules >= DT_COLLECTION_MAX_RULES)
        {
            dt_control_log(_("you can't have more than %d rules"), DT_COLLECTION_MAX_RULES);
            return;
        }
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/item%1d", d->nb_rules);
        dt_conf_set_int(confname, mode);
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/mode%1d", d->nb_rules);
        dt_conf_set_int(confname, DT_LIB_COLLECT_MODE_AND);
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/off%1d", d->nb_rules);
        dt_conf_set_int(confname, 0);
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/string%1d", d->nb_rules);
        dt_conf_set_string(confname, "");
        d->nb_rules++;
        dt_conf_set_int("plugins/lighttable/filtering/num_rules", d->nb_rules);

        _filters_gui_update(self);
        dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, mode, NULL);
    }
}

static void _popup_add_item(GtkMenuShell *pop, const gchar *name, const int id,
                            const gboolean title, GCallback callback, gpointer data,
                            dt_lib_module_t *self, const float xalign)
{
    // we first verify that the filter is defined
    if (callback != G_CALLBACK(_sort_append_sort) && !title && !_filters_get(id))
        return;

    GtkWidget *smt = gtk_menu_item_new_with_label(name);
    if (title)
    {
        GtkWidget *child = gtk_bin_get_child(GTK_BIN(smt));
        gtk_label_set_xalign(GTK_LABEL(child), xalign);
        gtk_widget_set_sensitive(smt, FALSE);
    }
    else
    {
        GtkWidget *child = gtk_bin_get_child(GTK_BIN(smt));
        gtk_label_set_xalign(GTK_LABEL(child), xalign);
        g_object_set_data(G_OBJECT(smt), "collect_id", GINT_TO_POINTER(id));
        if (data)
            g_object_set_data(G_OBJECT(smt), "collect_data", data);
        g_signal_connect_data(G_OBJECT(smt), "activate", callback, self, NULL, 0);
    }
    gtk_menu_shell_append(pop, smt);
}

static gboolean _rule_show_popup(GtkWidget *widget, dt_lib_filtering_rule_t *rule,
                                 dt_lib_module_t *self)
{
#define ADD_COLLECT_ENTRY(menu, value)                                                             \
    _popup_add_item(menu, dt_collection_name(value), value, FALSE, G_CALLBACK(_event_append_rule), \
                    rule, self, 0.5);

    // we show a popup with all the possible rules
    // note that only rules with defined filters will be shown
    GtkMenuShell *spop = GTK_MENU_SHELL(gtk_menu_new());
    gtk_widget_set_size_request(GTK_WIDGET(spop), 200, -1);

    // the different categories
    _popup_add_item(spop, _("files"), 0, TRUE, NULL, NULL, self, 0.0);
    ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_FILMROLL);
    ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_FOLDERS);
    ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_FILENAME);

    _popup_add_item(spop, _("metadata"), 0, TRUE, NULL, NULL, self, 0.0);
    ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_TAG);
    ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_RATING_RANGE);
    ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_RATING);
    ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_COLORLABEL);
    ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_TEXTSEARCH);

    _popup_add_item(spop, _("times"), 0, TRUE, NULL, NULL, self, 0.0);
    ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_DAY);
    ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_MONTH);
    ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_TIME);
    ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_IMPORT_TIMESTAMP);
    ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_CHANGE_TIMESTAMP);
    ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_EXPORT_TIMESTAMP);
    ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_PRINT_TIMESTAMP);

    _popup_add_item(spop, _("capture details"), 0, TRUE, NULL, NULL, self, 0.0);
    ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_CAMERA);
    ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_LENS);
    ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_APERTURE);
    ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_EXPOSURE);
    ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_EXPOSURE_BIAS);
    ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_FOCAL_LENGTH);
    ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_ISO);
    ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_ASPECT_RATIO);
    ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_WHITEBALANCE);
    ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_FLASH);
    ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_EXPOSURE_PROGRAM);
    ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_METERING_MODE);

    _popup_add_item(spop, _("darktable"), 0, TRUE, NULL, NULL, self, 0.0);
    ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_GROUP_ID);
    ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_DUPLICATES);
    ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_LOCAL_COPY);
    ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_HISTORY);
    ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_MODULE);
    ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_ORDER);

    dt_gui_menu_popup(GTK_MENU(spop), widget, GDK_GRAVITY_SOUTH, GDK_GRAVITY_NORTH);
    return TRUE;
#undef ADD_COLLECT_ENTRY
}

static void _rule_populate_prop_combo_add(GtkWidget *w, const dt_collection_properties_t prop)
{
    if (!_filters_get(prop))
        return;
    dt_bauhaus_combobox_add_full(w, dt_collection_name(prop), DT_BAUHAUS_COMBOBOX_ALIGN_MIDDLE,
                                 GUINT_TO_POINTER(prop), NULL, TRUE);
}

static void _populate_rules_combo(GtkWidget *w)
{
#define ADD_COLLECT_ENTRY(value) _rule_populate_prop_combo_add(w, value);
    gtk_widget_set_tooltip_text(w, _("rule property"));

    dt_bauhaus_combobox_add_section(w, _("files"));
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_FILMROLL);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_FOLDERS);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_FILENAME);

    dt_bauhaus_combobox_add_section(w, _("metadata"));
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_TAG);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_RATING_RANGE);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_RATING);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_COLORLABEL);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_TEXTSEARCH);

    dt_bauhaus_combobox_add_section(w, _("times"));
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_DAY);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_MONTH);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_TIME);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_IMPORT_TIMESTAMP);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_CHANGE_TIMESTAMP);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_EXPORT_TIMESTAMP);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_PRINT_TIMESTAMP);

    dt_bauhaus_combobox_add_section(w, _("capture details"));
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_CAMERA);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_LENS);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_APERTURE);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_EXPOSURE);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_EXPOSURE_BIAS);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_FOCAL_LENGTH);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_ISO);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_ASPECT_RATIO);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_WHITEBALANCE);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_FLASH);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_EXPOSURE_PROGRAM);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_METERING_MODE);

    dt_bauhaus_combobox_add_section(w, _("darktable"));
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_GROUP_ID);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_DUPLICATES);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_LOCAL_COPY);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_HISTORY);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_MODULE);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_ORDER);

#undef ADD_COLLECT_ENTRY
}

static void _rule_populate_prop_combo(dt_lib_filtering_rule_t *rule)
{
    GtkWidget *w = rule->w_prop;
    // first we cleanup existing entries
    dt_bauhaus_combobox_clear(w);

    _populate_rules_combo(w);

    rule->manual_widget_set++;
    dt_bauhaus_combobox_set_from_value(rule->w_prop, rule->prop);
    rule->manual_widget_set--;
}

static void _event_rule_append(GtkWidget *widget, gpointer user_data)
{
    _rule_show_popup(widget, NULL, (dt_lib_module_t *)user_data);
}

static void _event_rule_disable(GtkWidget *widget, dt_lib_filtering_rule_t *rule)
{
    if (rule->manual_widget_set)
        return;
    _event_rule_changed(widget, rule);
    gtk_widget_set_tooltip_text(rule->w_off,
                                gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(rule->w_off))
                                    ? _("this rule is enabled")
                                    : _("this rule is disabled"));
}

static gboolean _event_rule_close(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
    dt_lib_filtering_rule_t *rule =
        (dt_lib_filtering_rule_t *)g_object_get_data(G_OBJECT(widget), "rule");
    if (rule->manual_widget_set)
        return TRUE;

    // decrease the number of active rules
    dt_lib_filtering_t *d = rule->lib;
    if (d->nb_rules <= 0)
        return FALSE;
    d->nb_rules--;
    dt_conf_set_int("plugins/lighttable/filtering/num_rules", d->nb_rules);

    // move up all still active rules by one.
    for (int i = rule->num; i < DT_COLLECTION_MAX_RULES - 1; i++)
    {
        char confname[200] = {0};
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/mode%1d", i + 1);
        const int mode = dt_conf_get_int(confname);
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/item%1d", i + 1);
        const int item = dt_conf_get_int(confname);
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/off%1d", i + 1);
        const int off = dt_conf_get_int(confname);
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/string%1d", i + 1);
        gchar *string = dt_conf_get_string(confname);
        if (string)
        {
            snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/mode%1d", i);
            dt_conf_set_int(confname, mode);
            snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/item%1d", i);
            dt_conf_set_int(confname, item);
            snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/off%1d", i);
            dt_conf_set_int(confname, off);
            snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/string%1d", i);
            dt_conf_set_string(confname, string);
            g_free(string);
        }
    }

    _filters_gui_update(self);
    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, rule->prop, NULL);

    return TRUE;
}

// initialise or update a rule widget. Return if the a new widget has been created
static gboolean _widget_init(dt_lib_filtering_rule_t *rule, const dt_collection_properties_t prop,
                             const gchar *text, const dt_lib_collect_mode_t mode, gboolean off,
                             const int pos, dt_lib_module_t *self)
{
    rule->manual_widget_set++;

    const gboolean newmain = (rule->w_main == NULL);
    const gboolean newprop = (prop != rule->prop);
    GtkWidget *hbox = NULL;

    rule->prop = prop;

    if (newmain)
    {
        // the main box
        rule->w_main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_name(rule->w_main, "collect-rule-widget");

        // the first line
        hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_box_pack_start(GTK_BOX(rule->w_main), hbox, TRUE, TRUE, 0);
        gtk_widget_set_name(hbox, "collect-header-box");

        // operator type
        rule->w_operator = dt_bauhaus_combobox_new(NULL);
        dt_bauhaus_widget_set_quad_visibility(rule->w_operator, FALSE);
        dt_bauhaus_combobox_add_aligned(rule->w_operator, _("and"), DT_BAUHAUS_COMBOBOX_ALIGN_LEFT);
        dt_bauhaus_combobox_add_aligned(rule->w_operator, _("or"), DT_BAUHAUS_COMBOBOX_ALIGN_LEFT);
        dt_bauhaus_combobox_add_aligned(rule->w_operator, _("and not"),
                                        DT_BAUHAUS_COMBOBOX_ALIGN_LEFT);
        dt_bauhaus_combobox_set_selected_text_align(rule->w_operator,
                                                    DT_BAUHAUS_COMBOBOX_ALIGN_LEFT);
        gtk_widget_set_tooltip_text(
            rule->w_operator, _("define how this rule should interact with the previous one"));
        gtk_box_pack_start(GTK_BOX(hbox), rule->w_operator, FALSE, FALSE, 0);
        g_signal_connect(G_OBJECT(rule->w_operator), "value-changed",
                         G_CALLBACK(_event_rule_changed), rule);
    }

    dt_bauhaus_combobox_set(rule->w_operator, mode);
    gtk_widget_set_sensitive(rule->w_operator, pos > 0);

    // property
    if (newmain)
    {
        rule->w_prop = dt_bauhaus_combobox_new(NULL);
        dt_bauhaus_combobox_set_selected_text_align(rule->w_prop, DT_BAUHAUS_COMBOBOX_ALIGN_MIDDLE);
        dt_bauhaus_widget_set_quad_visibility(rule->w_prop, FALSE);
        _rule_populate_prop_combo(rule);
        g_object_set_data(G_OBJECT(rule->w_prop), "rule", rule);
        dt_bauhaus_combobox_set_from_value(rule->w_prop, prop);
        g_signal_connect(G_OBJECT(rule->w_prop), "value-changed",
                         G_CALLBACK(_event_rule_change_type), self);
        gtk_box_pack_start(GTK_BOX(hbox), rule->w_prop, TRUE, TRUE, 0);
    }
    else if (newprop)
    {
        _rule_populate_prop_combo(rule);
        dt_bauhaus_combobox_set_from_value(rule->w_prop, prop);
    }

    if (newmain)
    {
        rule->w_btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_box_pack_start(GTK_BOX(hbox), rule->w_btn_box, FALSE, FALSE, 0);

        // on-off button
        rule->w_off = dtgtk_togglebutton_new(dtgtk_cairo_paint_switch, 0, NULL);
        dt_gui_add_class(rule->w_off, "dt_transparent_background");
        g_object_set_data(G_OBJECT(rule->w_off), "rule", rule);
        g_signal_connect(G_OBJECT(rule->w_off), "toggled", G_CALLBACK(_event_rule_disable), rule);
        gtk_box_pack_end(GTK_BOX(rule->w_btn_box), rule->w_off, FALSE, FALSE, 0);

        // remove button
        rule->w_close = dtgtk_button_new(dtgtk_cairo_paint_remove, 0, NULL);
        g_object_set_data(G_OBJECT(rule->w_close), "rule", rule);
        g_signal_connect(G_OBJECT(rule->w_close), "button-press-event",
                         G_CALLBACK(_event_rule_close), self);
        gtk_box_pack_end(GTK_BOX(rule->w_btn_box), rule->w_close, FALSE, FALSE, 0);
    }

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rule->w_off), !off);
    gtk_widget_set_tooltip_text(rule->w_close, _("remove this collect rule"));
    gtk_widget_set_tooltip_text(rule->w_off, !off ? _("this rule is enabled")
                                                     : _("this rule is disabled"));

    if (newmain)
    {
        // the second line
        rule->w_widget_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_box_pack_start(GTK_BOX(rule->w_main), rule->w_widget_box, TRUE, TRUE, 0);
    }

    _rule_set_raw_text(rule, text, FALSE);

    // initialize the specific entries if any
    _widget_init_special(rule, text, self);

    rule->manual_widget_set--;
    return newmain;
}

static void _widget_special_destroy(dt_lib_filtering_rule_t *rule)
{
    if (rule->w_special_box)
    {
        gtk_widget_destroy(rule->w_special_box);
        rule->w_special_box = NULL;
        g_free(rule->w_specific);
        rule->w_specific = NULL;
    }
}

static void _filters_gui_update(dt_lib_module_t *self)
{
    dt_lib_filtering_t *d = self->data;

    DT_ENTER_GUI_UPDATE();
    d->nb_rules = CLAMP(dt_conf_get_int("plugins/lighttable/filtering/num_rules"), 0,
                        DT_COLLECTION_MAX_RULES);
    char confname[200] = {0};

    // create or update defined rules
    for (int i = 0; i < d->nb_rules; i++)
    {
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/item%1d", i);
        const dt_collection_properties_t prop = dt_conf_get_int(confname);
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/string%1d", i);
        gchar *txt = dt_conf_get_string(confname);
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/mode%1d", i);
        const dt_lib_collect_mode_t rmode = dt_conf_get_int(confname);
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/off%1d", i);
        const int off = dt_conf_get_int(confname);
        // cleanup previous special widgets
        _widget_special_destroy(&d->rule[i]);
        // recreate main widget
        if (_widget_init(&d->rule[i], prop, txt, rmode, off, i, self))
            gtk_box_pack_start(GTK_BOX(d->rules_box), d->rule[i].w_main, FALSE, TRUE, 0);
        gtk_widget_show_all(d->rule[i].w_main);

        g_free(txt);
        _widget_update(&d->rule[i]);
    }

    // remove all remaining rules
    for (int i = d->nb_rules; i < DT_COLLECTION_MAX_RULES; i++)
    {
        d->rule[i].prop = 0;
        if (d->rule[i].w_main)
        {
            gtk_widget_destroy(d->rule[i].w_main);
            d->rule[i].w_main = NULL;
            d->rule[i].w_special_box = NULL;
        }
    }

    DT_LEAVE_GUI_UPDATE();
}

static void _filtering_gui_update(dt_lib_module_t *self)
{
    _filters_gui_update(self);
    _sort_gui_update(self);
}

void gui_reset(dt_lib_module_t *self)
{
    _filtering_reset(_PRESET_ALL);

    _filters_gui_update(self);
    _sort_gui_update(self);

    dt_collection_set_query_flags(darktable.collection, COLLECTION_QUERY_FULL);
    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD,
                               DT_COLLECTION_PROP_UNDEF, NULL);
}

int position(const dt_lib_module_t *self)
{
    return 350;
}

static void _dt_collection_updated(gpointer instance, dt_collection_change_t query_change,
                                   dt_collection_properties_t changed_property, gpointer imgs,
                                   int next, gpointer self)
{
    dt_lib_module_t *dm = (dt_lib_module_t *)self;
    dt_lib_filtering_t *d = dm->data;

    gchar *where_ext = dt_collection_get_extended_where(darktable.collection, 99999);
    if (g_strcmp0(where_ext, d->last_where_ext))
    {
        g_free(d->last_where_ext);
        d->last_where_ext = where_ext;
    }
    else
        g_free(where_ext);

    for (int i = 0; i <= d->nb_rules; i++)
    {
        _widget_update(&d->rule[i]);
    }
}

// save a sort rule inside the conf
static void _conf_update_sort(_widgets_sort_t *sort)
{
    const gboolean order = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(sort->direction));
    const int sortid = GPOINTER_TO_UINT(dt_bauhaus_combobox_get_data(sort->sort));

    char confname[200] = {0};
    // if it's the last sort order, remember previous value for last order
    if (sort->num == sort->lib->nb_sort - 1)
    {
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sort%1d", sort->num);
        const dt_collection_sort_t lastsort = dt_conf_get_int(confname);
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sortorder%1d",
                 sort->num);
        const int lastsortorder = dt_conf_get_int(confname);
        if (lastsort != sortid)
        {
            dt_conf_set_int("plugins/lighttable/filtering/lastsort", lastsort);
            dt_conf_set_int("plugins/lighttable/filtering/lastsortorder", lastsortorder);
        }
    }

    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sort%1d", sort->num);
    dt_conf_set_int(confname, sortid);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sortorder%1d", sort->num);
    dt_conf_set_int(confname, order);

}

// update the sort asc/desc arrow
static void _sort_update_arrow(GtkWidget *widget)
{
    const gboolean reverse = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
    if (reverse)
        dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(widget), dtgtk_cairo_paint_sortby,
                                     CPF_DIRECTION_DOWN, NULL);
    else
        dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(widget), dtgtk_cairo_paint_sortby,
                                     CPF_DIRECTION_UP, NULL);
    gtk_widget_queue_draw(widget);
}

// set the sort order to the collection and update the query
static void _sort_update_query(_widgets_sort_t *sort)
{
    // we save the sort in conf
    _conf_update_sort(sort);

    /* sometimes changes */
    dt_collection_set_query_flags(darktable.collection, COLLECTION_QUERY_FULL);

    /* updates query */
    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD,
                               DT_COLLECTION_PROP_SORT, NULL);
}

static void _sort_reverse_changed(GtkDarktableToggleButton *widget, _widgets_sort_t *sort)
{
    if (sort->lib->manual_sort_set)
        return;

    _sort_update_arrow(GTK_WIDGET(widget));
    _sort_update_query(sort);
}

static void _sort_combobox_changed(GtkWidget *widget, gpointer user_data)
{
    _widgets_sort_t *sort = (_widgets_sort_t *)user_data;
    if (sort->lib->manual_sort_set)
        return;

    _sort_update_query(sort);
}

// reset filter values without changing the configured rules
static void _proxy_reset_filter(dt_lib_module_t *self, gboolean smart_filter)
{
    dt_lib_filtering_t *d = self->data;

    // reset each rule. we only throw the signal for the last one
    for (int i = 0; i < d->nb_rules; i++)
    {
        _rule_set_raw_text(&d->rule[i], "", (i == d->nb_rules - 1));
        _widget_update(&d->rule[i]);
        _conf_update_rule(&d->rule[i]);
    }
}

static gboolean _sort_close(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
    _widgets_sort_t *sort = (_widgets_sort_t *)g_object_get_data(G_OBJECT(widget), "sort");
    if (sort->lib->manual_sort_set)
        return TRUE;

    // decrease the nb of active rules
    dt_lib_filtering_t *d = sort->lib;
    if (d->nb_sort <= 1)
        return FALSE;
    d->nb_sort--;
    dt_conf_set_int("plugins/lighttable/filtering/num_sort", d->nb_sort);

    // move up all still active rules by one.
    for (int i = sort->num; i < DT_COLLECTION_MAX_RULES - 1; i++)
    {
        char confname[200] = {0};
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sort%1d", i + 1);
        const int sortid = dt_conf_get_int(confname);
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sortorder%1d", i + 1);
        const int sortorder = dt_conf_get_int(confname);

        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sort%1d", i);
        dt_conf_set_int(confname, sortid);
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sortorder%1d", i);
        dt_conf_set_int(confname, sortorder);
    }

    _sort_gui_update(self);
    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD,
                               DT_COLLECTION_PROP_SORT, NULL);

    return TRUE;
}

static gboolean _sort_init(_widgets_sort_t *sort, const dt_collection_sort_t sortid,
                           const int sortorder, const int num, dt_lib_module_t *self)
{
    dt_lib_filtering_t *d = self->data;
    d->manual_sort_set++;
    sort->num = num;
    sort->sortid = sortid;

    const gboolean ret = (!sort->box);

    if (!sort->box)
    {
        sort->lib = d;
        sort->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        // we only allow shortcut for the first sort order, always visible
        if (num == 0)
            sort->sort = dt_bauhaus_combobox_new_action(DT_ACTION(self));
        else
            sort->sort = dt_bauhaus_combobox_new(NULL);
        dt_action_t *ac = dt_bauhaus_widget_set_label(sort->sort, NULL, _("sort order"));
        dt_bauhaus_widget_hide_label(sort->sort);
        dt_bauhaus_combobox_mute_scrolling(sort->sort);
        gtk_widget_set_tooltip_text(sort->sort, _("determine the sort order of shown images"));
        g_signal_connect(G_OBJECT(sort->sort), "value-changed", G_CALLBACK(_sort_combobox_changed),
                         sort);
        gtk_box_pack_start(GTK_BOX(sort->box), sort->sort, TRUE, TRUE, 0);

        dt_bauhaus_combobox_add_section(sort->sort, _("files"));
        dt_bauhaus_combobox_add_introspection(sort->sort, ac, _collection_sort_names,
                                              DT_COLLECTION_SORT_FILENAME,
                                              DT_COLLECTION_SORT_ASPECT_RATIO);
        dt_bauhaus_combobox_add_section(sort->sort, _("times"));
        dt_bauhaus_combobox_add_introspection(sort->sort, ac, _collection_sort_names,
                                              DT_COLLECTION_SORT_DATETIME,
                                              DT_COLLECTION_SORT_PRINT_TIMESTAMP);
        dt_bauhaus_combobox_add_section(sort->sort, _("metadata"));
        dt_bauhaus_combobox_add_introspection(sort->sort, ac, _collection_sort_names,
                                              DT_COLLECTION_SORT_RATING,
                                              DT_COLLECTION_SORT_DESCRIPTION);
        dt_bauhaus_combobox_add_section(sort->sort, _("darktable"));
        dt_bauhaus_combobox_add_introspection(sort->sort, ac, _collection_sort_names,
                                              DT_COLLECTION_SORT_GROUP, DT_COLLECTION_SORT_SHUFFLE);

        /* reverse order checkbutton */
        sort->direction = dtgtk_togglebutton_new(dtgtk_cairo_paint_sortby, CPF_DIRECTION_UP, NULL);
        gtk_widget_set_halign(sort->direction, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(sort->box), sort->direction, FALSE, TRUE, 0);
        g_signal_connect(G_OBJECT(sort->direction), "toggled", G_CALLBACK(_sort_reverse_changed),
                         sort);
        dt_gui_add_class(sort->direction, "dt_ignore_fg_state");
        if (num == 0)
            dt_action_define(DT_ACTION(self), NULL, _("sort direction"), sort->direction,
                             &dt_action_def_toggle);

        sort->close = dtgtk_button_new(dtgtk_cairo_paint_remove, 0, NULL);
        gtk_widget_set_no_show_all(sort->close, TRUE);
        g_object_set_data(G_OBJECT(sort->close), "sort", sort);
        gtk_widget_set_tooltip_text(sort->close, _("remove this sort order"));
        g_signal_connect(G_OBJECT(sort->close), "button-press-event", G_CALLBACK(_sort_close),
                         self);
        gtk_box_pack_start(GTK_BOX(sort->box), sort->close, FALSE, FALSE, 0);
    }

    dt_bauhaus_combobox_set_from_value(sort->sort, sortid);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sort->direction), sortorder);
    gtk_widget_set_visible(sort->close, sort->lib->nb_sort > 1);
    _sort_update_arrow(sort->direction);

    gtk_widget_show_all(sort->box);

    d->manual_sort_set--;
    return ret;
}

static void _sort_gui_update(dt_lib_module_t *self)
{
    dt_lib_filtering_t *d = self->data;

    DT_ENTER_GUI_UPDATE();
    d->nb_sort =
        CLAMP(dt_conf_get_int("plugins/lighttable/filtering/num_sort"), 0, DT_COLLECTION_MAX_RULES);
    char confname[200] = {0};

    // handle the case where no sort item is already defined
    if (d->nb_sort == 0)
    {
        // set conf values
        dt_conf_set_int("plugins/lighttable/filtering/num_sort", 1);
        dt_conf_set_int("plugins/lighttable/filtering/sort0", DT_COLLECTION_SORT_FILENAME);
        dt_conf_set_int("plugins/lighttable/filtering/sortorder0", 0);
        d->nb_sort = 1;
    }

    // create or update defined rules
    for (int i = 0; i < d->nb_sort; i++)
    {
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sort%1d", i);
        const dt_collection_sort_t sort = dt_conf_get_int(confname);
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sortorder%1d", i);
        const int sortorder = dt_conf_get_int(confname);

        // recreate main widget
        if (_sort_init(&d->sort[i], sort, sortorder, i, self))
            gtk_grid_attach(GTK_GRID(d->sort_box), d->sort[i].box, 1, i, 1, 1);

    }

    // remove all remaining rules
    for (int i = d->nb_sort; i < DT_COLLECTION_MAX_RULES; i++)
    {
        d->sort[i].sortid = 0;
        if (d->sort[i].box)
        {
            gtk_widget_destroy(d->sort[i].box);
            d->sort[i].box = NULL;
        }
    }

    DT_LEAVE_GUI_UPDATE();
}

static void _sort_append_sort(GtkWidget *widget, dt_lib_module_t *self)
{
    // add new rule
    dt_lib_filtering_t *d = self->data;
    const int sortid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "collect_id"));
    char confname[200] = {0};

    if (sortid >= 0)
    {
        // add an empty rule
        if (d->nb_sort >= DT_COLLECTION_MAX_RULES)
        {
            dt_control_log(_("you can't have more than %d sort orders"), DT_COLLECTION_MAX_RULES);
            return;
        }
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sort%1d", d->nb_sort);
        dt_conf_set_int(confname, sortid);
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sortorder%1d",
                 d->nb_sort);
        dt_conf_set_int(confname, 0);
        d->nb_sort++;
        dt_conf_set_int("plugins/lighttable/filtering/num_sort", d->nb_sort);

        _sort_gui_update(self);
        dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD,
                                   DT_COLLECTION_PROP_SORT, NULL);
    }
}

static void _sort_show_add_popup(GtkWidget *widget, dt_lib_module_t *self)
{
    // we show a popup with all the possible sort
    GtkMenuShell *spop = GTK_MENU_SHELL(gtk_menu_new());
    gtk_widget_set_size_request(GTK_WIDGET(spop), 200, -1);

    for (const dt_introspection_type_enum_tuple_t *list = _collection_sort_names; list->name;
         list++)
        _popup_add_item(spop, Q_(list->name), list->value, FALSE, G_CALLBACK(_sort_append_sort),
                        NULL, self, 0.0);

    dt_gui_menu_popup(GTK_MENU(spop), widget, GDK_GRAVITY_SOUTH, GDK_GRAVITY_NORTH);
#undef ADD_SORT_ENTRY
}

static void _dt_images_order_change(gpointer instance, gpointer order, gpointer self)
{
    gchar *txt = (gchar *)order;
    if (txt)
    {
        dt_collection_sort_deserialize(txt);
        _sort_gui_update(self);
    }
}

void gui_init(dt_lib_module_t *self)
{
    dt_lib_filtering_t *d = calloc(1, sizeof(dt_lib_filtering_t));

    self->data = (void *)d;
    self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(self->widget, "module-filtering");
    dt_gui_add_class(self->widget, "dt_big_btn_canvas");
    dt_gui_add_help_link(self->widget, self->plugin_name);

    d->nb_rules = 0;
    d->params = (dt_lib_filtering_params_t *)g_malloc0(sizeof(dt_lib_filtering_params_t));
    _remove_topbar_state();
    _remove_history_state();

    const int nb = sizeof(filters) / sizeof(_filter_t);
    for (int i = 0; i < nb; i++)
    {
        dt_lib_filtering_rule_t temp_rule = {0};
        temp_rule.w_special_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

        filters[i].widget_init(&temp_rule, filters[i].prop, "", self);

        gtk_widget_destroy(temp_rule.w_special_box);
        g_free(temp_rule.w_specific);
    }

    for (int i = 0; i < DT_COLLECTION_MAX_RULES; i++)
    {
        d->rule[i].num = i;
        d->rule[i].lib = d;
    }

    // the box to insert the collect rules
    d->rules_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(self->widget), d->rules_box, FALSE, TRUE, 0);

    // add a rule
    GtkWidget *btn = dt_action_button_new(self, N_("new rule"), G_CALLBACK(_event_rule_append),
                                          self, _("append new rule to collect images"), 0, 0);
    gtk_box_pack_start(GTK_BOX(self->widget), btn, TRUE, TRUE, 0);

    // the sorting part
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(self->widget), spacer, TRUE, TRUE, 0);
    d->sort_box = gtk_grid_new();
    gtk_grid_attach(GTK_GRID(d->sort_box), gtk_label_new(_("sort by")), 0, 0, 1, 1);
    gtk_widget_set_name(d->sort_box, "filter-sort-box");
    gtk_box_pack_start(GTK_BOX(self->widget), d->sort_box, TRUE, TRUE, 0);

    // add a sort order
    btn = dt_action_button_new(self, N_("new sort"), G_CALLBACK(_sort_show_add_popup), self,
                               _("append new sort to order images"), 0, 0);
    gtk_box_pack_start(GTK_BOX(self->widget), btn, TRUE, TRUE, 0);

    /* setup proxy */
    darktable.view_manager->proxy.module_filtering.module = self;
    darktable.view_manager->proxy.module_filtering.update = _filtering_gui_update;
    darktable.view_manager->proxy.module_filtering.reset_filter = _proxy_reset_filter;

    d->last_where_ext = dt_collection_get_extended_where(darktable.collection, 99999);

    _filtering_gui_update(self);

    DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_COLLECTION_CHANGED, _dt_collection_updated);
    DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_IMAGES_ORDER_CHANGE, _dt_images_order_change);
}

void gui_cleanup(dt_lib_module_t *self)
{
    dt_lib_filtering_t *d = self->data;

    for (int i = 0; i < DT_COLLECTION_MAX_RULES; i++)
    {
        d->rule[i].cleaning = TRUE;
    }

    darktable.view_manager->proxy.module_filtering.module = NULL;
    free(d->params);

    /* TODO: Make sure we are cleaning up all allocations */

    free(self->data);
    self->data = NULL;
}

void view_enter(struct dt_lib_module_t *self, struct dt_view_t *old_view,
                struct dt_view_t *new_view)
{
    dt_lib_filtering_t *d = self->data;
    d->leaving = FALSE;
    gtk_widget_set_tooltip_text(self->reset_button, _("reset filters and sort order"));
}

void view_leave(struct dt_lib_module_t *self, struct dt_view_t *old_view,
                struct dt_view_t *new_view)
{
    if (!new_view)
    {
        // we are leaving dt, so we want to avoid pb with focus and such
        dt_lib_filtering_t *d = self->data;
        d->leaving = TRUE;
    }
}
