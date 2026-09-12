// proJV TUI -- theme menu + editor implementation (keyboard-first).
#include "theme_editor_view.h"

#include "theme_colors.h"
#include "theme_manager.h"
#include "theme_map.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace ftxui;

namespace theme_editor {

Component makeThemeMenu(std::function<void()> onClose) {
    auto names = std::make_shared<std::vector<std::string>>();
    names->push_back(ThemeManager::builtinName0);
    names->push_back(ThemeManager::builtinName1);
    for (const auto& n : ThemeManager::instance().installedNames()) {
        if (n != ThemeManager::builtinName0 && n != ThemeManager::builtinName1) {
            names->push_back(n);
        }
    }

    auto selected = std::make_shared<int>(0);
    MenuOption opt;
    opt.on_enter = [names, selected, onClose] {
        if (*selected >= 0 && *selected < static_cast<int>(names->size())) {
            ThemeManager::instance().switchTo((*names)[*selected]);
        }
        onClose();
    };
    Component menu = Menu(names.get(), selected.get(), opt);
    menu |= CatchEvent([onClose](Event e) {
        if (e == Event::Escape) { onClose(); return true; }
        return false;
    });

    return Renderer(menu, [menu] {
        return vbox({
            text("Theme") | bold,
            separator(),
            menu->Render() | frame | size(HEIGHT, LESS_THAN, 16),
            text("Esc close · ↑↓ select · Enter apply") | dim,
        }) | border;
    });
}

Component makeThemeEditor(std::function<void()> onClose) {
    auto editing = std::make_shared<ThemeColors>(ThemeManager::instance().current());

    auto names = std::make_shared<std::vector<std::string>>();
    for (const auto& f : themeColorFields()) names->push_back(f.name);

    auto selected = std::make_shared<int>(0);
    auto hex = std::make_shared<std::string>((*editing).*themeColorFields()[*selected].member);

    auto applyHexToField = [editing, selected, hex] {
        if (*selected >= 0 && *selected < static_cast<int>(themeColorFields().size())) {
            (*editing).*themeColorFields()[*selected].member = *hex;
        }
    };
    auto loadFieldToHex = [editing, selected, hex] {
        if (*selected >= 0 && *selected < static_cast<int>(themeColorFields().size())) {
            hex->assign((*editing).*themeColorFields()[*selected].member);
        }
    };

    // Menu Enter moves focus to the hex input (the input is created below).
    auto input_ref = std::make_shared<Component>();

    MenuOption menu_opt;
    menu_opt.on_change = [loadFieldToHex] { loadFieldToHex(); };
    menu_opt.on_enter = [input_ref] { if (*input_ref) (*input_ref)->TakeFocus(); };
    Component menu = Menu(names.get(), selected.get(), menu_opt);

    InputOption input_opt;
    input_opt.multiline = false;
    input_opt.on_enter = [menu, applyHexToField] { applyHexToField(); menu->TakeFocus(); };
    Component input = Input(hex.get(), input_opt);
    *input_ref = input;

    Component applyBtn = Button("Apply", [editing, applyHexToField] {
        applyHexToField();
        ThemeManager::instance().applyCustom(*editing);
    });
    Component saveBtn = Button("Save", [editing, applyHexToField] {
        applyHexToField();
        ThemeManager::instance().applyCustom(*editing);
        ThemeManager::instance().exportToFile(editing->name);
    });
    Component closeBtn = Button("Close", onClose);

    auto right = Container::Vertical({
        input,
        Container::Horizontal({ applyBtn, saveBtn, closeBtn }),
    });

    auto container = Container::Horizontal({ menu, right });
    container |= CatchEvent([onClose, menu, input, applyBtn, saveBtn, closeBtn](Event e) {
        if (e == Event::Escape) { onClose(); return true; }
        if (e == Event::Tab || e == Event::TabReverse) {
            // Menu swallows Tab by default; cycle focus manually.
            if (menu->Focused()) input->TakeFocus();
            else if (input->Focused()) applyBtn->TakeFocus();
            else if (applyBtn->Focused()) saveBtn->TakeFocus();
            else if (saveBtn->Focused()) closeBtn->TakeFocus();
            else menu->TakeFocus();
            return true;
        }
        return false;
    });

    return Renderer(container, [editing, names, selected, hex, menu, input, applyBtn, saveBtn, closeBtn] {
        // Keep the hex buffer reflected in the preview/apply operations.
        const auto& fields = themeColorFields();
        int idx = (*selected >= 0 && *selected < static_cast<int>(fields.size())) ? *selected : 0;
        (*editing).*fields[idx].member = *hex;
        const auto& f = fields[idx];
        return vbox({
            text("Theme Editor") | bold,
            separator(),
            hbox({
                menu->Render() | size(WIDTH, EQUAL, 28) | frame | size(HEIGHT, LESS_THAN, 20),
                separator(),
                vbox({
                    text("Field: " + std::string(f.name)) | bold,
                    hbox({ text("Hex: "), input->Render() | size(WIDTH, EQUAL, 12) }),
                    text("Preview") | dim,
                    text("        ") | bgcolor(theme_map::hexToColor(*hex)),
                    separator(),
                    hbox({
                        applyBtn->Render(), text("  "),
                        saveBtn->Render(), text("  "),
                        closeBtn->Render(),
                    }),
                    text("Esc close · ↑↓ field · Enter edit hex · Tab next") | dim,
                }) | flex,
            }),
        }) | border;
    });
}

} // namespace theme_editor
