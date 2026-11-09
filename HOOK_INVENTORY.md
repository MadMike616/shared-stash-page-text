# Hook Inventory — Shared Stash Page Text

## Plugin metadata

| Field | Value |
|---|---|
| Plugin name | Shared Stash Page Text |
| Plugin ID | shared-stash-page-text |
| Release version | 0.1.40 |
| Inventory updated | 2026-11-09 |
| Source revision | Release tag V0.1.40 |
| D2R build(s) audited | Not validated during this source audit |
| D2RLoader SDK / Plugin ABI | PluginSDK 0.4.0 / Plugin ABI 4 |
| Audit status | Complete |

## Scope and method

Inspected `src/plugin.cpp`, `src/plugin.rc`, `CMakeLists.txt`, and `README.md` in the v0.1.40 release source. Counted the plugin's native inline hook installation sites and Win32 window-procedure and input/message hooks. Calls to native stash and text helpers are not hook sites. The native renderer initialization is deferred until the first stash UI sync, allowing other plugins to validate their native targets before this plugin installs its preferred `DrawStyledText` hook. `BankPanel::Draw` remains a fallback only.

Numeric native addresses are RVAs. `InitializeNativeRenderer` validates expected instruction bytes before installing either native hook. The `DrawStyledText` hook is also gated on expected bytes for the widget, text measurement, style, and filled-rectangle helpers. Win32 hooks are installed only while the native editor is initialized and are removed or restored during teardown.

## Hook sites

| Site ID | Type | Target | Address / identifier | Operation | Guard / validation | Source reference | Notes |
|---|---|---|---|---|---|---|---|
| H-01 | Inline | D2R `DrawStyledText` | RVA 0x0902E20 | Installs `NativeDrawStyledText` through `InstallInlineHook` after the first stash UI sync | Exact 32-byte prologue checked; required widget, style, measure, and rectangle helper signatures also checked | `src/plugin.cpp` — `SyncPanelUi`, `InitializeNativeRenderer` | Primary renderer hook; note is drawn after the Shared page label. Deferred so other plugins can complete signature checks first. |
| H-02 | Inline | D2R `BankPanel::Draw` | RVA 0x023AA60 | Installs `NativeBankPanelDraw` through `InstallInlineHook` | Exact 17-byte prologue checked; attempted only if H-01 cannot be installed | `src/plugin.cpp` — `InitializeNativeRenderer` | Fallback renderer hook; same target as Pyre Stash Search if the primary hook is unavailable. |
| H-03 | Win32 input-message | D2R host window procedure (`GWLP_WNDPROC`) | Dynamic: `HostWindow` | Subclasses with `HostWindowProc` using `SetWindowLongPtrW` | Host window and previous procedure must be valid; restores the saved procedure on teardown if the plugin procedure is still installed | `src/plugin.cpp` — `CreateNativeEditor`, `DestroyNativeEditor`, `HostWindowProc` | Installed for native editor input handling. |
| H-04 | Win32 input-message | Plugin overlay window procedure (`GWLP_WNDPROC`) | Dynamic: `OverlayWindow` | Subclasses with `OverlayWindowProc` using `SetWindowLongPtrW` | Plugin-owned window; setup failure and teardown restore the saved procedure | `src/plugin.cpp` — `CreateNativeEditor`, `DestroyNativeEditor`, `OverlayWindowProc` | Top-level note overlay. |
| H-05 | Win32 input-message | Plugin editor window procedure (`GWLP_WNDPROC`) | Dynamic: `EditorWindow` | Subclasses with `NativeEditorWindowProc` using `SetWindowLongPtrW` | Plugin-owned edit control; setup failure and teardown restore the saved procedure | `src/plugin.cpp` — `CreateNativeEditor`, `DestroyNativeEditor`, `NativeEditorWindowProc` | Native text input control. |
| H-06 | Win32 input-message | D2R host window thread message queue (`WH_GETMESSAGE`) | N/A | Installs `NativeMessageHook` with `SetWindowsHookExW` | Requires resolved host window thread ID; unhooked during editor teardown | `src/plugin.cpp` — `CreateNativeEditor`, `DestroyNativeEditor`, `NativeMessageHook` | Thread-scoped message hook. |
| H-07 | Win32 input-message | Low-level keyboard input (`WH_KEYBOARD_LL`) | N/A | Installs `NativeKeyboardHook` with `SetWindowsHookExW` | Installs from a dedicated hook thread while the native editor exists; unhooked when the editor is destroyed | `src/plugin.cpp` — `NativeKeyboardHookThread`, `StartNativeKeyboardHook`, `StopNativeKeyboardHook` | OS-level low-level keyboard hook. |

## Known collision overlaps

| Other plugin | Shared target | Relationship | Evidence / notes |
|---|---|---|---|
| Pyre Stash Search | BankPanel::Draw, RVA 0x23AA60 | Same entry / fallback-only overlap | Notes now delays renderer initialization until the stash UI is loaded, allowing Pyre Stash Search 1.3.1+rev.1 to install first. Notes targets this same entry only if its preferred `DrawStyledText` hook cannot be installed; coexistence was confirmed in user testing. |
| Pyre Stash Search | WH_KEYBOARD_LL | Possible shared OS hook chain | Both sources install a low-level keyboard hook. |
| Quest Reward Reset | WH_GETMESSAGE on the D2R host thread | Possible shared thread message-hook chain | Both sources install `WH_GETMESSAGE`; runtime overlap depends on the host thread. |

## Audit notes

This is a source-level hook inventory; the D2R build and live hook installation were not independently validated as part of this source audit. Hook targets are unchanged from v0.1.39; v0.1.40 defers renderer hook installation until the stash UI has loaded. User testing confirmed coexistence with Pyre Stash Search 1.3.1+rev.1.
