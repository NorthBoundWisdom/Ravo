# Shortcuts and Command Palette

## Goal

Find Studio actions quickly from the keyboard and understand when a command is
disabled.

**Last reviewed:** 2026-09-03 against the current Studio command registry.

## Command palette

Open the command palette with:

- `Cmd+Shift+P` on macOS.
- `Ctrl+Shift+P` on Windows and Linux.

Type a command name or keyword, move with Up/Down, and press Return to run the
highlighted action. The palette searches command titles, categories, keywords,
and command IDs. When an action is unavailable, the palette keeps it visible
and shows the reason; selecting it does not bypass the state requirement.

Press `Esc` to close the palette. Text fields keep their normal typing behavior;
Studio does not dispatch non-text shortcuts while a text input is active.

## Common shortcuts

The table uses `Cmd` on macOS and `Ctrl` on Windows/Linux for the primary
modifier.

| Action | Shortcut |
| --- | --- |
| Create Library | `Cmd/Ctrl+N` |
| Open Library | `Cmd/Ctrl+O` |
| Import Photos | `Cmd/Ctrl+I` |
| Import Folder | `Cmd/Ctrl+Shift+I` |
| Export Selected | `Cmd/Ctrl+Shift+E` |
| Close Window | `Cmd/Ctrl+W` |
| Settings | `Cmd/Ctrl+,` |
| Assistant | `Cmd/Ctrl+Shift+A` (also the top-toolbar Assistant button) |
| Select All | `Cmd/Ctrl+A` |
| Quit Ravo Studio | `Cmd/Ctrl+Q` |
| Undo | `Cmd/Ctrl+Z` |
| Redo | `Cmd/Ctrl+Shift+Z` |
| Copy Parameters | `Cmd/Ctrl+Shift+C` |
| Paste Parameters | `Cmd/Ctrl+Alt+V` |
| Paste Parameters to Selection | `Cmd/Ctrl+Alt+Shift+V` |
| Reset All Edits | `Cmd/Ctrl+Shift+R` |
| Gallery | `Cmd/Ctrl+1` or `G` |
| Loupe | `Cmd/Ctrl+2`, `E`, or Return. While Crop & Rotate is active, these apply the crop and stay in Edit. |
| Edit | `Cmd/Ctrl+3` or `D` |
| Fit | `Cmd/Ctrl+0` or `F` |
| Fill | `Cmd/Ctrl+9` |
| Actual Size | `Cmd/Ctrl+Alt+0` or `Shift+1` |
| Before / After | `\` |
| Left / Right comparison | `Y` |
| Photo information overlay | `I` |
| Previous / Next Photo | Left / Right Arrow |
| Crop & Rotate | `R` |
| Rotate Left / Right | `Cmd/Ctrl+[` / `Cmd/Ctrl+]` |
| Flip Horizontal / Vertical | `Cmd/Ctrl+Shift+H` / `Cmd/Ctrl+Shift+V` |
| Reject | `X` |
| Remove from Catalog | Delete or Backspace |

The exact native modifier label is rendered by Qt, so macOS menus show Command
where the cross-platform command definition uses Ctrl.

## Selection shortcuts

- `Cmd/Ctrl+A` selects every photo currently loaded in Gallery and the filmstrip.
- `Cmd`/`Ctrl`-click adds or removes a photo from the selection.
- `Shift`-click selects a range from the selection anchor.
- `Shift+Left` and `Shift+Right` extend the selection to the previous or next
  photo when that action is available.

Review actions and batch export can operate on the selected set. Loupe, Edit,
and Inspector use the active photo; single-photo export uses that primary
selection.

## Menus and controls

Every built-in command is projected into the relevant menu, control, shortcut,
and command palette entry. If a command requires an open library, a selected
photo, a non-grid view, a completed import, or a confirmation token, Studio
shows a disabled reason. Use the menu or palette as the authoritative list for
the current context.

## Result

You can reach the same action through a visible control, a menu, or the command
palette without relying on undocumented hidden behavior.

## Common questions

### Why did a shortcut do nothing while I was typing?

Shortcuts that could corrupt text entry are disabled while a text field has
focus. Finish or cancel the text edit, then use the command.

### Why is a command visible but disabled?

The command registry keeps unavailable actions visible with a reason. For
example, Export Selected needs an open library and at least one selected photo,
while Undo needs an available edit history.

### Why does macOS show Command instead of Ctrl?

The cross-platform registry stores a portable primary modifier and Qt renders
the native macOS equivalent in menus and shortcuts.
