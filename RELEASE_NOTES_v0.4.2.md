# proJV v0.4.2 Release Notes

> Date: 2026-07-20
> Compare: `v0.4.0` → `v0.4.2`

---

## Highlights

### Theme System (New)
- **`ThemeColors` struct**: 65 named color fields covering all UI elements — ImGui style, chat bubbles, tool calls, reasoning cards, agent phases, status bar, TODO panel, markdown, and D3D clear color. All stored as `#RRGGBB` hex strings for human readability and JSON compatibility.
- **`ThemeManager` singleton**: manages current theme, built-in presets, and user-installed themes. On startup, auto-scans `projv_theme/` for `.json` files and loads the preferred theme from `config.toml`.
- **Visual theme editor** (`theme_popup.cpp`): category-based color picker with live mini-preview panel. Edit colors by category (ImGui Style / Chat / Tool / Reasoning / Phase / Status / TODO / Markdown), see changes in real-time, export/import JSON.
- **8 preset themes**:

| Theme | Style |
|-------|-------|
| **Obsidian** (default) | Refined warm-dark with amber/gold accents |
| **Light** (default) | Clean white with muted accents |
| **Forest** | Airy sage green, nature-inspired |
| **Artism** | Warm caramel, cozy café aesthetic |
| **Monochrome Dark** | Pure grayscale, zero color distraction |
| **Colorblind Safe** | Blue-orange palette, accessible for deuteranopia/protanopia/tritanopia |
| **Vibrant Focus** | High-contrast neon, strong visual blocks |
| **Cyber Punk** | Neon yellow/pink/cyan on pure black, Cyberpunk 2077 style |

- **config.toml persistence**: Theme choice auto-saved to `config.toml` as `theme` key, restored on next launch. `onThemeChanged` callback fires on every switch.

### Bugfix: Release build log file
- **`clean_and_build.bat`**: Fixed `CMAKE_BUILD_TYPE=RELEASE` → `Release` (case-sensitive CMake comparison). Previously `PROJV_RELEASE` was never defined, causing all builds to write `proJV.exe.log` via loguru.

---

## Architecture Changes

| Area | Before (v0.4.0) | After (v0.4.2) |
|------|----------------|----------------|
| Color management | ~50 hardcoded `ImVec4` across 5 files | Single `ThemeColors` struct in `theme.h`, all refs via `ThemeManager::instance().current()` |
| Theme switching | 2 hardcoded themes in `applyTheme(int)` | Dynamic list: 2 built-ins + N installed + visual editor |
| Config | No theme persistence | `config.toml` `theme` key, read on startup, written on change |
| ImGui style | 8 `ImGuiCol_*` overrides | 36 `ImGuiCol_*` keys set by `ThemeColors::applyToImGui()` |

---

## File Changelog

### New Files
| File | Description |
|------|-------------|
| `src/ui/theme.h` | `ThemeColors` struct (65 fields) + `ThemeManager` singleton declaration |
| `src/ui/theme.cpp` | `toVec4/toU32`, `applyToImGui` (36 ImGuiCol_*), `toJson/fromJson`, `init/switchTo/loadFromFile/exportToFile/scanThemeDir` |
| `src/ui/theme_popup.cpp` | Theme editor popup: category selector, hex inputs, live preview, Load/Save/Export/Reset buttons |
| `projv_theme/*.json` | 6 installable theme JSONs + 2 built-in defaults in code |
| `assets/themes/*.png` | 600×380 preview images for each theme |
| `assets/customize_theme_color.png` | Screenshot of the visual theme editor |
| `doc/design_theme_system.md` | Full architecture design doc for the theme system |

### Modified Files
| File | Key Changes |
|------|-------------|
| `src/client/models.h` | Added `AppConfig::themeName` field |
| `src/core/config.cpp` | Read/write `theme` key in `config.toml`; `saveConfig` conditionally writes it |
| `src/main.cpp` | `ThemeManager::init()` after `g_app.initialize()`; D3D clear color from theme |
| `src/ui/app.h` | Removed old `themeNames[2]`, `selectedThemeIndex`, `applyTheme()`; added `getConfigThemeName()` |
| `src/ui/app.cpp` | Dynamic Theme combo; `onThemeChanged` callback saves to config; all ~15 status/welcome/TODO colors replaced with theme refs |
| `src/ui/render_chat.cpp` | All ~35 hardcoded `ImVec4` → `ThemeColors::toVec4(T().xxx)` |
| `src/ui/render_settings.cpp` | 4 hardcoded colors → theme refs |
| `src/ui/markdown_render.cpp` | Deleted 14 `static const ImVec4`; all refs → `ThemeManager::instance().current()` |
| `src/tools/md_file_tool.cpp` | Added `mode` param (`write`/`append`) and chunking for large content |
| `CMakeLists.txt` | Added `theme.cpp`, `theme_popup.cpp` |
| `clean_and_build.bat` | Fixed `RELEASE` → `Release` case mismatch |
| `README.md` / `README_zh.md` | Rewritten with theme table, revised project description |

---

## Bugfixes
- **md_file_tool mode + chunking**: Added `mode` parameter (`write`/`append`) and large-content chunking support, matching `file_tool.cpp` behavior. Previously the tool only supported overwrite mode and had no size limit handling.

---

## Upgrade Notes
- Existing `config.toml` works as-is. Add `theme = "Obsidian"` (or any theme name) to persist your preference.
- Drop custom theme `.json` files into `projv_theme/` next to the executable — they auto-appear in the Theme menu.
- Old `proJV.exe.log` files will no longer be generated in Release builds.
