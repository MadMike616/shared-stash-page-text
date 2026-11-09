# Shared Stash Page Text

Shared Stash Page Text is a D2RLoader plugin that displays an editable, colored note above the Shared stash page indicator. It supports up to 1,000 pages and hides the note when another stash tab is selected.

## Release 0.1.40

- The native `DrawStyledText` hook now waits until the stash UI has loaded. This lets Pyre Stash Search validate and install its `BankPanel::Draw` hook first. Both plugins were confirmed working together with Pyre Stash Search 1.3.1+rev.1.
- Existing TOML files and saved notes remain compatible; no configuration changes are required.
- Built for Windows x64 as a Release DLL with the MSVC runtime linked statically. Compatibility remains D2RLoader PluginSDK 0.4.0 / Plugin ABI 4.
- Thanks to PyreFly for the StashSearch reference and insight that helped diagnose the hook-order conflict.

SHA-256: `83BCC0C53B5DD27C26EABFE1AE5218DBE170E190896F536A66F7BFF810BD0A2C`

## Release 0.1.39

This release corrects the published DLL build: it is compiled as x64 Release and statically links the MSVC runtime. It does not require Visual C++ debug runtime DLLs. Existing TOML files and notes remain compatible.

## Using notes

Click the note and type. Press **Enter** or click outside the field to save. Each page has its own note and color, saved in `d2rloader/config/shared-stash-page-text.toml`.

The plugin tracks the page shown by the game, including controller input, mouse-wheel changes, arrow clicks, Shift/Ctrl jumps, and page-limit behavior. It saves the note for the current editor page before switching to another page. Notes remain saved across game sessions.

## Configuration

Set `page_count` to the Shared stash page count in your mod. Valid values are 1–1,000. Add a `[pages.N]` section for each note; pages may be left blank between notes.

```toml
[pages.1]
text = "Perfect gems"
color = "#79D7FF"

[pages.2]
text = "Runes"
color = "#FFD27F"

[pages.3]
text = "Bases"
color = "#B8FF8C"
```

Colors use `#RRGGBB`. On first startup, the plugin creates the loader-managed TOML from the checked-in template. It writes note changes there as you type.

If `page_count` is lower than the game's actual Shared stash page count, page tracking still follows the game, but the note is hidden above the configured count. When the plugin saves the TOML, it writes only `[pages.1]` through `[pages.N]`; back up the file before lowering `page_count` if it contains notes on higher pages.

The plugin manages `current_page` and `shared_tab_selected`; do not edit those values.

## Position and appearance

The note is centered horizontally and vertically. Its position and size are configurable in the TOML:

| Setting | Description |
| --- | --- |
| `window_x`, `window_y` | Offsets from the anchor point, in D2R UI coordinates. |
| `window_width`, `window_height` | Note field dimensions. |
| `anchor_x`, `anchor_y` | Normalized screen anchor positions from `0` to `1`. |
| `font_size` | Text size in D2R UI coordinates; default `23`, valid `8`–`96`. |
| `font` | Legacy setting. It no longer changes the visible note font, and can safely remain in the TOML. |

The visible note uses the game's font for the Shared page indicator. The plugin first looks for matching `.otf` or `.ttf` files in the active mod's `data/hd/ui/fonts` folder, including D2R `fontFace` aliases. Restart D2RLoader after changing layout or font-size settings.

## Compatibility

This release uses D2RLoader PluginSDK 0.4.0 and Plugin ABI 4. Use a D2RLoader build that supports ABI 4 and the plugin's panel, resource, thread, shared-event, and lifecycle services. Install the DLL in `d2rloader/plugins/`; its config is stored in `d2rloader/config/`.

Some stash layouts report a generic tab-switch message without the selected tab index. For those layouts, configure `tabMessages` so each tab's selection message ends with its index. For example, a three-tab layout can use `BankPanelMessage:SelectTab:0`, `BankPanelMessage:SelectTab:1`, and `BankPanelMessage:SelectTab:2`; Shared is index 1 in that arrangement.

## Build

Build on Windows x64 with CMake 3.29+, Visual Studio 2022 or newer, and the Windows SDK:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --target shared_stash_page_text
```

The DLL is `build/Release/d2rl-shared-stash-page-text.dll`.
The target uses the static MSVC runtime for Release builds (`/MT`). Build with `--config Release`; do not distribute a DLL from the Debug output folder.

## Acknowledgements

Thanks to PyreFly for the StashSearch plugin, which served as a reference while investigating native text input and tooltip layering for this update.
