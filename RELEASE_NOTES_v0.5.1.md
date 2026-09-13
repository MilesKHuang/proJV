# proJV v0.5.1 Release Notes

> Date: 2026-09-13
> Compare: `v0.5.0` -> `v0.5.1`

---

## Highlights

### TODO moves to a right-side dock

The TODO panel is no longer a 6-line bottom strip. It now docks on the right
of the chat area (matching the legacy GUI layout), runs the full content
height, and soft-wraps long task lines so nothing is clipped. `F9` still
toggles it. The chat column re-flows its CJK soft-wrap automatically when the
dock is shown or hidden, so row-granular scrolling keeps working.

### Bubble text follows the theme semantic grading

Bubble frames no longer use a rainbow of role-colored borders. Borders are a
single neutral theme color, and each bubble's text follows the existing
`ThemeColors` semantic mapping used by the legacy GUI:

| Bubble | Color source |
|--------|--------------|
| You / proJV / System body | `text` |
| Tool title / result | `toolTitleColor` / `toolResultText` |
| Reasoning label / body | `reasoningTextColor` / `reasoningBodyText` |
| Context-compacted warning | `todoInProgress` |

No new background colors were introduced.

### AI reply title is now proJV

Assistant bubbles (history and streaming) are titled `── proJV ──` instead of
`── AI ──`.

### Agent status uses theme grading

The `[Agent: phase]` indicator and the live status line now color by state:
`phaseIdle / phaseStreaming / phaseExecutingTools / phaseAwaitApproval /
phaseError` for the phase, and `statusIdle / statusRunning / statusAwaiting /
statusError / reasoningTextColor` for the status line.

### Animated busy indicator during tool execution

The working spinner now animates whenever the agent is busy -- streaming,
executing tools, or awaiting approval -- instead of only during streaming with
a frozen frame. Tool runs no longer look like the UI is stuck.

### Inter-bubble separators removed

Horizontal separator bars between chat bubbles are gone. Bubbles are separated
by their own borders and spacing only.

---

## Changes by Area

| Area | Before (v0.5.0) | After (v0.5.1) |
|------|-----------------|----------------|
| TODO panel | Bottom strip, max 6 rows, clipped text | Right dock, full height, soft-wrapped |
| Bubble borders | Per-role colored borders | Single neutral border |
| Bubble labels | You / AI / System / Tool / Result | You / proJV / System / Tool / Result with semantic theme colors |
| Agent status | Plain/dim text | Theme-graded phase + status line |
| Busy spinner | Only while streaming, frozen frame | Animated while streaming / executing / awaiting approval |
| Bubble separator | Heavy separator between bubbles | Removed |

---

## Bug Fixes

| Issue | Fix |
|-------|-----|
| TODO limited to 6 rows and clipped long lines | Right dock + `renderPlainText` soft-wrap |
| Bubble text looked garish (mismatched role colors) | Restored legacy theme semantic text-color grading, neutral borders |
| AI replies titled `── AI ──` | Renamed to `── proJV ──` |
| Agent status lacked theme grading | Phase + status line colored by `ThemeColors` |
| No visual feedback during tool execution | Animated spinner for all busy states |
| Horizontal bars between bubbles | Removed |

---

## File Changelog

### Modified
| File | Key Changes |
|------|-------------|
| `src/tui/chat_view.cpp` | Neutral borders, semantic text grading, `── proJV ──` title, removed separators |
| `src/tui/main_tui.cpp` | TODO right dock, phase/status theme colors, animated busy spinner |
| `src/tui/status_line.cpp/.h` | Return text + theme color field |
| `src/tui/todo_view.cpp` | TODO lines soft-wrap via `renderPlainText` |
| `CMakeLists.txt` | Project version bumped to 0.5.1 |
| `src/core/agent.cpp` | `/help` version string bumped to v0.5.1 |
| `tests/unit/test_status_line.cpp` | Updated for `status_line::Line` |
| `tests/golden/test_chat_render.cpp` | Updated golden assertions for `── proJV ──` |

### Removed
| File | Reason |
|------|--------|
| `doc/*` | Superseded design/migration docs cleared for the v0.5.1 baseline |
| `RELEASE_NOTES_v0.4.8.md` | Superseded by v0.5.1 |
| `RELEASE_NOTES_v0.5.0.md` | Superseded by v0.5.1 |
