// proJV TUI -- theme menu + editor implementation.
#include "theme_editor_view.h"

#include "theme_colors.h"
#include "theme_manager.h"
#include "theme_map.h"

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace ftxui;

namespace theme_editor {

Component makeThemeMenu(std::function<void()> onClose) {
    std::vector<std::string> names;
    names.push_back(ThemeManager::builtinName0);
    names.push_back(ThemeManager::builtinName1);
    for (const auto& n : ThemeManager::instance().installedNames()) {
        if (n != ThemeManager::builtinName0 && n != ThemeManager::builtinName1) {
            names.push_back(n);
        }
    }

    int selected = 0;
    Component menu = Menu(&names, &selected);
    Component applyBtn = Button("Apply", [&names, &selected, onClose] {
        if (selected >= 0 && selected < static_cast<int>(names.size())) {
            ThemeManager::instance().switchTo(names[selected]);
        }
        onClose();
    });
    Component cancelBtn = Button("Cancel", onClose);

    auto container = Container::Vertical({ menu, Container::Horizontal({ applyBtn, cancelBtn }) });
    return Renderer(container, [=] {
        return vbox({
            text("Theme") | bold,
            separator(),
            menu->Render() | frame | size(HEIGHT, LESS_THAN, 12),
            separator(),
            hbox({ applyBtn->Render(), text("  "), cancelBtn->Render() }),
        }) | border;
    });
}

Component makeThemeEditor(std::function<void()> onClose) {
    auto editing = std::make_shared<ThemeColors>(ThemeManager::instance().current());

    std::vector<std::string> names;
    for (const auto& f : themeColorFields()) names.push_back(f.name);

    int selected = 0;
    auto hex = std::make_shared<std::string>((*editing).*themeColorFields()[selected].member);

    MenuOption menu_opt;
    menu_opt.on_change = [editing, hex, &selected] {
        if (selected >= 0 && selected < static_cast<int>(themeColorFields().size())) {
            hex->assign((*editing).*themeColorFields()[selected].member);
        }
    };

    Component menu = Menu(&names, &selected, menu_opt);
    Component input = Input(hex.get());

    Component applyBtn = Button("Apply", [editing] {
        ThemeManager::instance().applyCustom(*editing);
    });
    Component saveBtn = Button("Save", [editing] {
        ThemeManager::instance().applyCustom(*editing);
        ThemeManager::instance().exportToFile(editing->name);
    });
    Component closeBtn = Button("Close", onClose);

    auto container = Container::Horizontal({
        menu,
        Container::Vertical({
            input,
            Container::Horizontal({ applyBtn, saveBtn, closeBtn }),
        }),
    });

    return Renderer(container, [=] {
        // Write the hex input back into the selected field (live edit).
        if (selected >= 0 && selected < static_cast<int>(themeColorFields().size())) {
            (*editing).*themeColorFields()[selected].member = *hex;
        }

        const auto& f = themeColorFields()[selected];
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
                }) | flex,
            }),
        }) | border;
    });
}

} // namespace theme_editor
