Shared Stash Page Text

A D2RLoader ABI 4 plugin that displays an editable note above the Shared stash page indicator. It supports up to 1,000 pages and hides the note when another stash tab is selected.

## Use the notes

Click the note to focus it and start typing; there is no Edit button. Each page has its own note and color. Changes are saved to `d2rloader/config/shared-stash-page-text.toml` as you type.

- Press **Enter** to save and leave the note visible.
- Click outside the note to save and return keyboard focus to the game.
- Press **Esc** while the note is unfocused to close it with the stash.

## Configure the notes

Add one `[pages.N]` section for each page you want to label. Set `page_count` to match the number of Shared stash pages in your mod; valid values are 1–1,000.

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

Colors use the `#RRGGBB` format. You can leave a page's note blank and add notes to later pages.

If `page_count` is lower than the game's actual number of Shared pages, the plugin continues tracking the page shown in game and hides the note on pages above the configured count. When it next saves the TOML, it writes only `[pages.1]` through `[pages.N]`. Back up the TOML before lowering `page_count` if it contains notes on higher pages; those sections may be removed.

The checked-in TOML template is embedded as the first-load fallback. The loader creates its config from that template on first startup. The template matches the active mod's defaults; edit the loader-managed config file to save your notes.

## Position and font

The note's layout and font are configured in the same TOML file. Text is centered horizontally and vertically.

| Setting | Description |
| --- | --- |
| `window_x`, `window_y` | Position offsets from the screen anchor, in D2R UI coordinates. |
| `window_width`, `window_height` | Note field dimensions. |
| `anchor_x`, `anchor_y` | Screen position for the anchor, from `0` to `1`. |
| `font` | Font family; defaults to `exocetblizzardot-medium`. |
| `font_size` | Size in D2R UI coordinates; defaults to `23` and accepts `8`–`96`. |

Font size scales with the game window. The plugin first looks in the active mod's `data/hd/ui/fonts` folder, including aliases from D2R's `fontFace` settings such as `Exocet`, for a matching `.otf` or `.ttf` file. If it cannot find one, it uses the selected Windows font family. Restart D2RLoader after changing layout or font settings.

## Page tracking

The plugin checks the game's selected stash tab and page every 250 ms and also listens for stash UI messages. Notes follow the page actually shown when you use a controller, the mouse wheel, the page arrows, Shift/Ctrl jumps, or reach a game page limit. Before it changes pages in the editor, it saves the note to the page that was open there.

The plugin retains the page when you close and reopen the stash during a game. A new game session resets page tracking to page 1 and clears the remembered Shared-tab selection. The `current_page` and `shared_tab_selected` values in the TOML are managed by the plugin; do not edit them.

<details>
<summary>Input and overlay implementation</summary>

The note uses a top-level native overlay that contains the edit control, keeping its visible area and hit area aligned. A keyboard hook on the host thread captures keys before D2R's UI hotkeys process them.

</details>

## Build

Build on Windows x64 with CMake 3.29+, Visual Studio 2022+, and the Windows SDK:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --target shared_stash_page_text
```

The Release DLL is `build/Release/d2rl-shared-stash-page-text.dll`.

## Compatibility notes

The panel and shared UI-message services use public PluginSDK APIs. Some stash layouts send only a generic `onSwitchTabMessage` without the selected-tab index. For those layouts, add a `tabMessages` entry for each tab so the selection message ends with that tab's index.

For example, Grailer3's three-tab `BankTabs` uses:

```text
BankPanelMessage:SelectTab:0
BankPanelMessage:SelectTab:1
BankPanelMessage:SelectTab:2
```

Shared is tab index 1. The local Grailer3 mapping is in `mods/Grailer3/Grailer3.mpq/data/global/ui/layouts/bankexpansionlayouthd.json`.

Before distributing the plugin, check the note's alignment with the installed game build and display scale.
