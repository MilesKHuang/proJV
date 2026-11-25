// proJV studio -- resource gauges: the arithmetic behind the System panel.
//
// The rule this module exists for: a number the user has to compare in their head
// is a number they will misread. "212 MB" next to "32 GB" is a mental division;
// the same value as a bar against a scaled ceiling is a glance. So the panel draws
// a bar and keeps the exact value in the tooltip -- and this file decides the bar,
// the label and the ceiling, never the drawing.
//
// ImGui-free on purpose: every function here is a pure conversion, so the three
// cases that go wrong in practice are unit tests instead of screenshots:
//   - a value that cannot be measured prints "-", never "0%" (they mean different
//     things, and 0% reads as "idle" when the truth is "unknown");
//   - a ceiling that scales with what is there, so a bar at 100% means "the
//     heaviest of what I have", not "some constant I picked";
//   - human sizes that stop at three units instead of printing 13 digits.
#pragma once

#include <optional>
#include <string>

namespace studio {
namespace gauges {

// "1.2 GB" / "212 MB" / "48 KB". Binary units (1024), because a task manager
// shows binary units and a user compares this with one. 0 bytes is "0 KB".
std::string humanBytes(unsigned long long bytes);

// "-" when unknown, "37%" otherwise (clamped to 100, no decimals).
std::string percentLabel(const std::optional<double>& percent);

// "-" when unknown, "12.5%" otherwise: memory is the one value a tenth matters
// for, because a 200 MB process on a 32 GB machine is 0.6% and rounding to "1%"
// loses the comparison.
std::string memoryLabel(const std::optional<double>& percent);

// value / ceiling clamped to 0..1. A ceiling <= 0 gives 0, never a division.
double fraction(double value, double ceiling);

// The ceiling a bar should use: at least `minimum`, never below the value itself,
// so the fill can never be drawn past the end of the track.
double ceilingFor(double value, double minimum);

} // namespace gauges
} // namespace studio
