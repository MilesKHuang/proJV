// proJV TUI -- config/welcome/approval dialog components implementation.
#include "config_view.h"

#include "approval_logic.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <memory>
#include <string>

using namespace ftxui;

namespace config_view {

Component makeConfigDialog(TuiApp& app, std::function<void()> onClose) {
    const auto& cfg = app.getConfig();
    auto apiKey = std::make_shared<std::string>(cfg.apiKey);
    auto baseUrl = std::make_shared<std::string>(cfg.baseUrl);
    auto model = std::make_shared<std::string>(cfg.model);
    auto workspace = std::make_shared<std::string>(cfg.workspacePath);
    auto compiler = std::make_shared<std::string>(cfg.cppCompilerPath);
    auto python = std::make_shared<std::string>(cfg.pythonPath);
    auto maxTokens = std::make_shared<std::string>(std::to_string(cfg.maxTokens));
    auto temperature = std::make_shared<std::string>(
        cfg.temperature < 0.0001 ? "0" : std::to_string(cfg.temperature));

    InputOption singleLine;
    singleLine.multiline = false;

    InputOption password;
    password.password = true;
    password.multiline = false;

    Component apiKeyInput = Input(apiKey.get(), password);
    Component baseUrlInput = Input(baseUrl.get(), singleLine);
    Component modelInput = Input(model.get(), singleLine);
    Component maxTokensInput = Input(maxTokens.get(), singleLine);
    Component tempInput = Input(temperature.get(), singleLine);
    Component wsInput = Input(workspace.get(), singleLine);
    Component cppInput = Input(compiler.get(), singleLine);
    Component pyInput = Input(python.get(), singleLine);

    Component saveBtn = Button("Save & Connect", [&app, onClose, apiKey, baseUrl,
                                                   maxTokens, temperature, workspace,
                                                   compiler, python, model] {
        int mt = 0;
        try { mt = std::stoi(*maxTokens); } catch (...) { mt = 0; }
        if (mt < 512) mt = 512;
        if (mt > 65536) mt = 65536;

        double t = 0.0;
        try { t = std::stod(*temperature); } catch (...) { t = 0.0; }
        if (t < 0.0) t = 0.0;
        if (t > 2.0) t = 2.0;

        app.saveFullConfig(*apiKey, *baseUrl, mt, t, *workspace, *compiler, *python, *model);
        onClose();
    });
    Component cancelBtn = Button("Cancel", onClose);

    auto container = Container::Vertical({
        apiKeyInput, baseUrlInput, modelInput, maxTokensInput, tempInput,
        wsInput, cppInput, pyInput,
        Container::Horizontal({ saveBtn, cancelBtn }),
    });

    return Renderer(container, [=] {
        return vbox({
            text("Configuration") | bold,
            separator(),
            hbox({ text("API Key: ") | size(WIDTH, EQUAL, 16), apiKeyInput->Render() | size(WIDTH, EQUAL, 44) }),
            hbox({ text("Base URL: ") | size(WIDTH, EQUAL, 16), baseUrlInput->Render() | size(WIDTH, EQUAL, 44) }),
            hbox({ text("Model: ") | size(WIDTH, EQUAL, 16), modelInput->Render() | size(WIDTH, EQUAL, 44) }),
            hbox({ text("Max Tokens: ") | size(WIDTH, EQUAL, 16), maxTokensInput->Render() | size(WIDTH, EQUAL, 44) }),
            hbox({ text("Temperature: ") | size(WIDTH, EQUAL, 16), tempInput->Render() | size(WIDTH, EQUAL, 44) }),
            hbox({ text("Workspace: ") | size(WIDTH, EQUAL, 16), wsInput->Render() | size(WIDTH, EQUAL, 44) }),
            hbox({ text("C++ Compiler: ") | size(WIDTH, EQUAL, 16), cppInput->Render() | size(WIDTH, EQUAL, 44) }),
            hbox({ text("Python: ") | size(WIDTH, EQUAL, 16), pyInput->Render() | size(WIDTH, EQUAL, 44) }),
            separator(),
            hbox({ saveBtn->Render(), text("  "), cancelBtn->Render() }),
        }) | border;
    });
}

Component makeWelcome(TuiApp& app, std::function<void()> onSaved) {
    auto apiKey = std::make_shared<std::string>("");
    InputOption password;
    password.password = true;
    password.multiline = false;

    Component input = Input(apiKey.get(), password);
    input |= CatchEvent([&](Event e) {
        if (e == Event::Return) {
            if (!apiKey->empty()) {
                app.saveApiKeyAndConnect(*apiKey);
                if (onSaved) onSaved();
            }
            return true;
        }
        return false;
    });
    Component saveBtn = Button("Save & Connect", [&app, apiKey, onSaved] {
        if (!apiKey->empty()) {
            app.saveApiKeyAndConnect(*apiKey);
            if (onSaved) onSaved();
        }
    });

    auto container = Container::Vertical({ input, saveBtn });
    return Renderer(container, [=] {
        return vbox({
            text("Welcome to proJV") | bold,
            separator(),
            text("You need a DeepSeek API key to get started."),
            hbox({ text("API Key: "), input->Render() | size(WIDTH, EQUAL, 44) }),
            hbox({ saveBtn->Render() }),
            separator(),
            text("Commands: /help, /workspace, /clear, /compress") | dim,
        }) | border;
    });
}

Component makeApprovalDialog(TuiApp& app, std::function<void()> onClose) {
    Component yes = Button("YES -- Execute Once", [&app, onClose] { app.approveTool(1); onClose(); });
    Component always = Button("ALWAYS -- Allow This Session", [&app, onClose] { app.approveTool(2); onClose(); });
    Component no = Button("NO -- Cancel", [&app, onClose] { app.approveTool(0); onClose(); });
    auto buttons = Container::Horizontal({ yes, always, no });

    return Renderer(buttons, [&app, yes, always, no] {
        Elements els;
        els.push_back(text("DELETE CONFIRMATION") | bold);
        els.push_back(separator());
        els.push_back(text("The AI wants to DELETE files. Review the operation below:"));

        if (auto* agent = app.getAgent()) {
            auto msgs = agent->getSession().getContextMessages();
            if (!msgs.empty()) {
                const auto& toolCalls = msgs.back().toolCalls;
                for (const auto& call : toolCalls) {
                    std::string cmd = approval_logic::extractCommand(call.arguments);
                    auto files = approval_logic::extractDeleteFiles(cmd);
                    els.push_back(separator());
                    els.push_back(text("Command: " + cmd) | color(Color::RGB(224, 200, 96)));
                    if (files.empty()) {
                        els.push_back(text("  " + call.arguments));
                    } else {
                        els.push_back(text("Files to delete:"));
                        for (const auto& f : files) {
                            els.push_back(text("  - " + f));
                        }
                    }
                }
            }
        }

        els.push_back(separator());
        els.push_back(text("Approve this delete operation?"));
        els.push_back(hbox({ yes->Render(), text("  "), always->Render(), text("  "), no->Render() }));
        return vbox(std::move(els)) | border;
    });
}

} // namespace config_view
