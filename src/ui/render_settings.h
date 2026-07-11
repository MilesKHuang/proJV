#pragma once
// Settings dialogs extracted from app.cpp
// Note: these are implemented as App:: member functions in render_settings.cpp
// to avoid changing the class definition in app.h.

class App;

// Free-function wrappers (declared for documentation; the actual
// implementations remain App:: methods accessible via app.h).
void renderConfigPopup(App& app);
void renderSystemPromptPopup(App& app);
void renderToolApprovalDialog(App& app);
