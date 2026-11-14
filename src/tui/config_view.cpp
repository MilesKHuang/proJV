// proJV TUI -- config/welcome/approval dialog components implementation.
#include "config_view.h"

#include "approval_logic.h"
#include "core/snapshot.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <memory>
#include <string>
#include <algorithm>
#include <optional>

using namespace ftxui;

namespace config_view {

Component makeConfigDialog(TuiApp& app, std::function<void()> onClose) {
    // Everything the file can hold is edited here. The dialog works on one copy of
    // AppConfig and hands it back, so a key can never be half-wired: missing from the
    // panel, present in the file, impossible to change from the UI.
    auto cfg = std::make_shared<AppConfig>(app.getConfig());
    auto maxTokens = std::make_shared<std::string>(std::to_string(cfg->maxTokens));
    auto temperature = std::make_shared<std::string>(
        cfg->temperature < 0.0001 ? "0" : std::to_string(cfg->temperature));
    auto contextWindow = std::make_shared<std::string>(std::to_string(cfg->contextWindow));
    auto effect = std::make_shared<std::string>(cfg->reasoningEffort);
    auto snapGb = std::make_shared<std::string>(std::to_string(cfg->snapshotsMaxWorkspaceGb));
    auto snapDays = std::make_shared<std::string>(std::to_string(cfg->snapshotsKeepDays));

    InputOption singleLine;
    singleLine.multiline = false;

    InputOption password;
    password.password = true;
    password.multiline = false;

    Component apiKeyInput = Input(&cfg->apiKey, password);
    Component baseUrlInput = Input(&cfg->baseUrl, singleLine);
    Component modelInput = Input(&cfg->model, singleLine);
    Component maxTokensInput = Input(maxTokens.get(), singleLine);
    Component tempInput = Input(temperature.get(), singleLine);
    Component effectInput = Input(effect.get(), singleLine);
    Component contextInput = Input(contextWindow.get(), singleLine);
    Component cppInput = Input(&cfg->cppCompilerPath, singleLine);
    Component pyInput = Input(&cfg->pythonPath, singleLine);
    Component clangInput = Input(&cfg->clangFormatPath, singleLine);
    Component snapGbInput = Input(snapGb.get(), singleLine);
    Component snapDaysInput = Input(snapDays.get(), singleLine);
    Component themeInput = Input(&cfg->themeName, singleLine);

    // approval_mode: one dial, three stops. Radiobox keeps exactly one selected.
    auto modeNames = std::make_shared<std::vector<std::string>>(
        std::vector<std::string>{"Strict -- everything asks, nothing remembered",
                                 "Balanced -- destructive asks, build output runs",
                                 "Loose -- only format/partition is refused"});
    auto modeIndex = std::make_shared<int>(static_cast<int>(cfg->approvalMode));
    Component modeBox = Radiobox(modeNames.get(), modeIndex.get());
    // Grants are shown, not edited: revoking is one button so the list cannot grow
    // into something the user forgets about.
    Component clearGrantsBtn = Button("Clear remembered commands", [cfg, app = &app] {
        cfg->approvalGrants.clear();
        if (app->getAgent()) app->getAgent()->clearGrants();
    });
    Component oscBox =
        Checkbox("osc_status -- report state to the terminal (OSC 7501)", &cfg->oscStatus);

    auto intOr = [](const std::string& s, int fallback) {
        try {
            return std::stoi(s);
        } catch (...) {
            return fallback;
        }
    };
    auto doubleOr = [](const std::string& s, double fallback) {
        try {
            return std::stod(s);
        } catch (...) {
            return fallback;
        }
    };

    Component saveBtn = Button("Save", [&app, onClose, cfg, maxTokens, temperature, effect,
                                        contextWindow, snapGb, snapDays, intOr, doubleOr, modeIndex,
                                        modeNames] {
        // Clamp exactly where config.cpp clamps on load: the file and the UI must
        // never disagree about what a legal value is.
        cfg->maxTokens =
            std::clamp(intOr(*maxTokens, cfg->maxTokens), kMinOutputTokens, kMaxOutputTokens);
        cfg->temperature = std::clamp(doubleOr(*temperature, cfg->temperature), 0.0, 2.0);
        cfg->reasoningEffort = normalizeReasoningEffort(*effect);
        if (cfg->reasoningEffort.empty()) cfg->reasoningEffort = kReasoningEffortOfficialDefault();
        const int cw = intOr(*contextWindow, static_cast<int>(cfg->contextWindow));
        cfg->contextWindow = cw < 0 ? 0 : static_cast<size_t>(cw);
        cfg->snapshotsMaxWorkspaceGb = std::max(0, intOr(*snapGb, cfg->snapshotsMaxWorkspaceGb));
        cfg->snapshotsKeepDays = std::max(1, intOr(*snapDays, cfg->snapshotsKeepDays));
        cfg->approvalMode = static_cast<ApprovalMode>(std::clamp(*modeIndex, 0, 2));
        app.saveFullConfig(*cfg);
        onClose();
    });
    Component cancelBtn = Button("Cancel", onClose);

    auto container = Container::Vertical({
        apiKeyInput,
        baseUrlInput,
        modelInput,
        maxTokensInput,
        tempInput,
        effectInput,
        contextInput,
        cppInput,
        pyInput,
        clangInput,
        snapGbInput,
        snapDaysInput,
        themeInput,
        modeBox,
        clearGrantsBtn,
        oscBox,
        Container::Horizontal({saveBtn, cancelBtn}),
    });

    auto row = [](const char* label, Component c) {
        return hbox({text(label) | size(WIDTH, EQUAL, 16), c->Render() | size(WIDTH, EQUAL, 44)});
    };

    // Same shape as the dialog always had: one column, built INSIDE the renderer so
    // every frame produces a fresh tree. (Building the elements once outside and
    // reusing them made the inputs visible but dead -- no cursor, no editing.)
    // The buttons live in a pinned footer: a body taller than the terminal can
    // scroll, but Save/Cancel can never be pushed off the bottom again.
    return Renderer(container, [=] {
        return vbox({
                   hbox({text("Configuration (F2)") | bold,
                         text("   Esc or F2 closes   Tab moves") | dim}),
                   separator(),
                   vbox({
                       row("API Key", apiKeyInput),
                       row("Base URL", baseUrlInput),
                       row("Model", modelInput),
                       row("Max Tokens", maxTokensInput),
                       row("Temperature", tempInput),
                       row("Effort", effectInput),
                       row("Context Window", contextInput),
                       separator(),
                       row("C++ Compiler", cppInput),
                       row("Python", pyInput),
                       row("clang-format", clangInput),
                       separator(),
                       row("Snapshot GB", snapGbInput),
                       row("Snapshot days", snapDaysInput),
                       row("Theme", themeInput),
                       oscBox->Render(),
                       separator(),
                       text("Command approval") | bold,
                       modeBox->Render(),
                       hbox({text("remembered: " + std::to_string(cfg->approvalGrants.size()) +
                                  " command(s)  ") |
                                 dim,
                             clearGrantsBtn->Render()}),
                       text("max_tokens 1024-65536; effort none/low/high/max; context 0 = auto; "
                            "strict offers no ALWAYS") |
                           dim,
                   }) | yframe |
                       vscroll_indicator | flex,
                   separator(),
                   hbox({saveBtn->Render(), text("  "), cancelBtn->Render()}),
               }) |
               border;
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

    auto container = Container::Vertical({input, saveBtn});
    return Renderer(container, [=] {
        return vbox({
                   text("Welcome to proJV") | bold,
                   separator(),
                   text("You need a DeepSeek API key to get started."),
                   hbox({text("API Key: "), input->Render() | size(WIDTH, EQUAL, 44)}),
                   hbox({saveBtn->Render()}),
                   separator(),
                   text("Commands: /help, /workspace, /clear, /compress, /skill") | dim,
               }) |
               border;
    });
}

Component makeApprovalDialog(TuiApp& app, std::function<void()> onClose) {
    Component yes = Button("YES -- Execute Once", [&app, onClose] {
        app.approveTool(1);
        onClose();
    });
    Component always = Button("ALWAYS -- Allow This Command", [&app, onClose] {
        app.approveTool(2);
        onClose();
    });
    Component no = Button("NO -- Cancel", [&app, onClose] {
        app.approveTool(0);
        onClose();
    });
    auto buttons = Container::Horizontal({yes, always, no});

    return Renderer(buttons, [&app, yes, always, no] {
        Elements els;

        // The agent owns the truth: one call at a time. Reading the session's
        // last message here used to work only because the whole batch happened
        // to be in it.
        std::optional<ToolCall> call;
        if (auto* agent = app.getAgent()) call = agent->pendingApproval();
        const std::string cmd = call ? approval_logic::extractCommand(call->arguments) : "";
        const auto files = approval_logic::extractDeleteFiles(cmd);

        els.push_back(text("COMMAND APPROVAL") | bold);
        els.push_back(separator());
        els.push_back(text("This command deletes or overwrites files:"));
        els.push_back(text("  " + (cmd.empty() ? std::string("(arguments unreadable)") : cmd)) |
                      color(Color::RGB(224, 200, 96)));

        if (!files.empty()) {
            els.push_back(text("Targets:"));
            const size_t shown = std::min<size_t>(files.size(), 10);
            for (size_t i = 0; i < shown; ++i)
                els.push_back(text("  - " + files[i]));
            if (files.size() > shown)
                els.push_back(text("  ... and " + std::to_string(files.size() - shown) + " more"));
        }

        els.push_back(separator());
        // Strict mode has nothing to remember, so the button is not offered at all --
        // hiding it is the honest signal, a disabled button invites clicking.
        const bool canAlways = app.getAgent() && app.getAgent()->canAlways();
        els.push_back(text(canAlways
                               ? "ALWAYS remembers this exact command in config.toml; NO drops "
                                 "the whole batch."
                               : "strict mode: nothing is remembered; NO drops the whole batch.") |
                      dim);
        els.push_back(
            text(snapshot::isAvailable()
                     ? "[Undo available] the files this turn changes can be restored with /undo"
                     : "[Undo unavailable] a deletion here cannot be reverted") |
            dim);
        els.push_back(hbox({yes->Render(), text("  "),
                            canAlways ? always->Render() : text("(no ALWAYS in strict mode)") | dim,
                            text("  "), no->Render()}));
        return vbox(std::move(els)) | border;
    });
}

} // namespace config_view
