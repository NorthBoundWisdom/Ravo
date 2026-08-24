# Shortcuts System

The shortcut system in darktable is built upon a unified **Action System**. Actions abstract user interactions (like pressing a key, clicking a button, or scrolling) and map them to specific functions in modules or views.

## The Action System (`dt_action_def_t`)

Every shortcut-able action is defined by a `dt_action_def_t` structure. This structure links a human-readable name to a processing function.

### Key Components

-   **`dt_action_t`**: The runtime representation of an action. It forms a hierarchical tree (e.g., `iop/exposure/exposure`).
-   **`dt_action_def_t`**: The static definition.
    ```c
    typedef struct dt_action_def_t
    {
      const gchar *name;          // Internal name
      // Function to execute when action is triggered
      float (*process)(gpointer target, dt_action_element_t, dt_action_effect_t, float size);
      const dt_action_element_def_t *elements; // Sub-elements (optional)
      const dt_shortcut_fallback_t *fallbacks; // Default shortcuts
      const gboolean no_widget;   // True if not associated with a specific widget
    } dt_action_def_t;
    ```

## Implementing Shortcuts in Modules

For most IOP modules, you don't need to manually define actions. The `dt_bauhaus` widget library handles this automatically.

### Automatic Registration
When you create a widget using introspection (e.g., `dt_bauhaus_slider_from_params`), the system automatically registers an action for it. The action ID is derived from the parameter name.

### Manual Registration
If you are creating custom widgets or non-widget actions, you can register them manually.

#### 1. Define the Action
In your module code (often in `gui_init` or a helper), define the action structure.

#### 2. Register with `dt_action_register`
```c
dt_action_register(parent_action, "action_name", callback_function, default_key, modifiers);
```

### Shortcuts for Buttons
To create a button that has a shortcut, use `dt_action_button_new`:
```c
GtkWidget *btn = dt_action_button_new(module, _("button label"), 
                                      callback, user_data, 
                                      _("tooltip"), 
                                      GDK_KEY_e, GDK_CONTROL_MASK); // Default shortcut Ctrl+E
```

## Adding Shortcuts to existing Modules
If an existing module control lacks a shortcut:
1.  Check if it's a standard `dt_bauhaus` widget. If so, it should already have one. Check `shortcuts` tab in preferences.
2.  If it's a custom widget (e.g., a drawing area), you may need to implement a specific action definition and connect it.

## User Configuration
Users can customize shortcuts in **Preferences > Shortcuts**. The system scans all registered actions and presents them in a hierarchical list.

## Lightroom-style Direct View Shortcuts

DarkTableNext provides direct, idempotent view commands in addition to the older toggle and hold
actions. User mappings loaded from the shortcut configuration continue to take precedence over
these defaults.

| Key | Action ID / scope | Default behavior |
| --- | --- | --- |
| `G` | `global/grid` | Enter Lighttable Grid, exit Preview/Culling, and use the 2–10 images-per-row Grid range. |
| `E` | `global/loupe` | Enter the single-image Loupe at Fit, with the header, side panels, and filmstrip visible but without pointer hover interactions. The canvas uses a magnifier cursor; a click toggles Fit/100%, a double-click returns to Grid, and `I` pins basic image information at its upper-left. |
| `C` | `global/compare` | Enter fixed two-image Culling. |
| `N` | `global/survey` | Enter dynamic Culling driven by the current selection. |
| `D` | `global/switch views/darkroom` | Enter Darkroom; invoking it in Darkroom is a no-op. |
| `I` | `lighttable/show infos` | Toggle the persistent information label for Loupe, Compare, and Survey. The state is retained while Grid or Darkroom hides the label and across application restarts. |
| `Z` | active Lighttable or Darkroom view | Toggle Fit/100% in Loupe/Culling; enter Loupe at 100% from Grid; reuse Darkroom's close-up zoom toggle. |

On macOS, application-wide commands use Command: `⌘Q` quits, `⌘W` closes the main window,
`⌘H` hides the application, `⌘M` minimizes the window, `⌘,` opens Preferences, and `⌘?` opens
documentation. These remain available alongside the direct view keys.

The layout tool records a requested `global/grid`, `global/loupe`, `global/compare`, or
`global/survey` mode before a cross-view transition and applies it only after the
`DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED` signal confirms Lighttable. A failed transition drops the
request, and a later request replaces an earlier pending mode.

Existing aliases remain available: `L` enters Lighttable, `F` toggles Preview, `X` toggles fixed
Culling, `Control+X` toggles dynamic Culling, `W` holds Preview, and `Option+1` toggles Darkroom
close-up zoom. To keep the direct global keys unique in Darkroom, guide lines, the exposure control,
and the crop module now default to `Shift+G`, `Shift+E`, and `Shift+C`, respectively.

## macOS System Command Projection

`src/gui/system_commands.c` is a projection of the existing Action system, not a second shortcut
registry. Once the GUI Action tree and user shortcut configuration are ready, it registers a
`GtkApplication`, exports a product-level `GMenuModel`, and exposes a selected subset of Actions as
`GSimpleAction` entries. This lets macOS Accessibility clients discover the menu and its simple
keyboard equivalents.

The development command `build/<preset>/bin/darktable` launches the matching `darktable.app` bundle
instead of a bare Mach-O executable. The bundle's `org.darktable.darktable` identity is required for
per-application Accessibility clients such as Paletro and KeyClu to associate the active process with
its exported menu; the wrapper preserves the documented command-line entry point.

- `dt_action` and the configured `GSequence` remain the source of truth. A projected command keeps
  only its stable Action ID plus element/effect/instance; it re-resolves the Action, queries
  `dt_action_get_status()`, then invokes it through `dt_action_invoke()`.
- Only a plain keyboard chord with one stable Action target is eligible. Mouse, scroll, hold,
  multiple-click, move/device input, provider-only Actions, object hit tests, and ambiguous IOP
  instances stay in the existing dispatcher or context-menu provider.
- `dt_action_get_gtk_accels()` returns the active view's eligible chords and
  `dt_shortcut_normalize_modifiers()` is the shared input policy. On macOS the projection uses
  `<Primary>` so Command is exported as a native key equivalent while Quartz-specific marker bits do
  not change dispatch matching.
- The main-window dispatcher lets the focused editor consume input first, then routes a matching
  projected chord to the same `GSimpleAction` activation path as a menu click. This prevents the
  native representation and legacy dispatcher from executing an Action twice.
- Loading or saving shortcuts schedules an accelerator refresh. Menu/action state refreshes on view,
  selection, active-image, history, and style changes; all GTK/GAction updates run on the GTK main
  thread.
