// proJV studio -- the left column: sessions, the file tree, the System panel.
//
// One switchboard, three panels stacked in one column (the same shape as the
// reference window this was designed from):
//
//   Sessions   the panes THIS window owns. Selecting one shows its page, rebinds
//              the prompt and re-roots the tree. "Running a turn" is visible, and
//              a busy pane refuses to close instead of orphaning its worker.
//   Files      the workspace tree, with `>`/`@` on every row: send the path, or
//              send `@path` (the image/context form proJV already understands).
//   System     CPU / MEM / GPU of this window's whole process tree, plus the state
//              of the turn that is running. A number that cannot be measured shows
//              "-" and the reason, never a fabricated 0.
//
// This file draws and wires; it owns no state. The tree, the panes, the receipt
// and the sampler all live in App, so a panel cannot disagree with the rest of the
// window about what is true.
#pragma once

namespace studio {

class App;
struct Frame;

// Draw the whole column inside `frame.left*` and wire clicks into `app`.
void drawLeftBar(App& app, const Frame& f);

} // namespace studio
