# Shared Stash Page Text

A D2RLoader ABI 4 plugin that places an editable note above the Shared stash page indicator. `page_count` supports modded Shared stashes with up to 1,000 pages.

Click the note itself to focus it and type. Enter saves and leaves the note visible; clicking outside saves and returns keyboard focus to the game. The visible field is a top-level native overlay that contains the edit control, so its displayed rectangle and hit area share the same window. A host-thread keyboard hook consumes each key before D2R's UI hotkeys process it. Text is written to TOML as it changes. Each page has its own `text` and `color` in `d2rloader/config/shared-stash-page-text.toml`.

The plugin also records `current_page` and `shared_tab_selected` (You shouldn't touch these ones) in that file. It retains the current page when the stash closes and reopens during a game, then resets page tracking to page 1 and clears the remembered Shared-tab selection when a new game session starts. It polls the game's current stash tab and page every 250 ms, as well as reacting to stash UI messages, so notes follow the page actually shown with controller input, mouse-wheel changes, arrow clicks, Shift/Ctrl jumps, and the game's own page-limit behavior. Before switching the editor to a newly observed page, it saves the text to the page currently loaded in the editor. These two values are managed by the plugin.

The position is configurable in that TOML file with `window_x`, `window_y`, `window_width`, `window_height`, `anchor_x`, and `anchor_y`. `anchor_x` and `anchor_y` are normalized screen positions from 0 to 1; x/y are offsets from that anchor in D2R UI coordinates. Text is centered horizontally and vertically in the field. The `font` setting defaults to `exocetblizzardot-medium`; `font_size` defaults to `23` D2R UI coordinates and accepts values from `8` to `96`, scaling with the game window. The plugin first searches the active mod's `data/hd/ui/fonts` folder, including D2R `fontFace` aliases such as `Exocet`, and privately loads a matching `.otf` or `.ttf` into the loader process. If the asset is missing, it uses the selected Windows font family instead. Restart D2RLoader after changing the layout or font values.

Set `page_count` to match the Shared stash's actual number of pages. Valid values are 5–1,000; add matching `[pages.N]` sections for the notes you want.

If `page_count` is lower than the game's actual number of Shared pages, the plugin still follows the game page, but hides the note window when the current page exceeds the configured count. When the plugin next saves the TOML, it writes only `[pages.1]` through `[pages.N]`, so note sections above the configured count can be removed. Back up the TOML before lowering `page_count` if it contains notes on higher pages.

Example:

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

Colors use `#RRGGBB`. The checked-in TOML template is also embedded as the first-load fallback, so the loader-managed config created on first startup uses the same defaults. The template is synchronized with the active config used for this mod; the loader-managed config file is where edits are saved.

## Build

Build on Windows x64 with CMake 3.29+, Visual Studio 2022+, and the Windows SDK:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --target shared_stash_page_text
```

The Release DLL is `build/Release/d2rl-shared-stash-page-text.dll`.

## Compatibility notes

The panel and shared UI-message services are public PluginSDK APIs. Some stash layouts emit only a generic `onSwitchTabMessage` with no selected-tab index. Those layouts need a `tabMessages` entry for each tab so the selection message carries its index as the final colon-separated value. For example, Grailer3's three-tab `BankTabs` uses `BankPanelMessage:SelectTab:0`, `BankPanelMessage:SelectTab:1`, and `BankPanelMessage:SelectTab:2`; Shared is index 1. This mapping is applied in the local Grailer3 layout under `mods/Grailer3/Grailer3.mpq/data/global/ui/layouts/bankexpansionlayouthd.json`.

As with any UI overlay, compare the note's alignment on the installed game build and display scale before distributing it.
