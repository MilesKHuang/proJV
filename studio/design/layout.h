// proJV studio -- a flex layout solver (row / column, gap, grow, percent).
//
// Why not ImGui's own layout? ImGui places items as a side effect of drawing
// them: the position of a region is only known after everything above it has
// been submitted, so a panel that must share an edge with something drawn later
// ends up reading a height recorded on the PREVIOUS frame (that is literally what
// studio_ui.cpp did with `static float bottomHeight`). One resize and the window
// is a frame behind; one font change and the bands overlap.
//
// This solver is the other way round: the whole tree is sized and placed first,
// then drawing happens inside known rectangles. It is pure arithmetic with no
// ImGui, no globals and no clock, so the unit tests pin the geometry directly.
//
// The subset is deliberately small -- the window is built from rows and columns:
//   direction      Row | Column
//   size policy    Fixed | Fit (content) | Grow (share of what is left) | Percent
//   box            padding, gap, min/max clamp
//   cross axis     Start | Center | End | Stretch
//   main axis      Start | Center | End | SpaceBetween
// No wrapping, no absolute children, no aspect ratios: anything the window needs
// is expressible as nested rows and columns.
#pragma once

#include <vector>

namespace studio {
namespace layout {

// An axis-aligned rectangle in pixels, absolute screen space once solved.
struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    float right() const { return x + w; }
    float bottom() const { return y + h; }
    float centerX() const { return x + w * 0.5f; }
    float centerY() const { return y + h * 0.5f; }
    bool empty() const { return w <= 0.0f || h <= 0.0f; }
    bool contains(float px, float py) const {
        return px >= x && py >= y && px < right() && py < bottom();
    }
    Rect inset(float l, float t, float r, float b) const;
    Rect inset(float all) const { return inset(all, all, all, all); }
    Rect shrink(float dw, float dh) const { return inset(dw, dh, dw, dh); }
    // Same box with a new height, keeping the top edge (bands stacked downwards).
    Rect withHeight(float newH) const { return Rect{x, y, w, newH}; }
    Rect withWidth(float newW) const { return Rect{x, y, newW, h}; }
    Rect moved(float dx, float dy) const { return Rect{x + dx, y + dy, w, h}; }
    // The bottom `amount` pixels of this box.
    Rect fromBottom(float amount) const { return Rect{x, bottom() - amount, w, amount}; }
    // The top `amount` pixels of this box.
    Rect fromTop(float amount) const { return Rect{x, y, w, amount}; }
    // This box cut down so it ends at `limit` (a band plus everything reserved
    // below it can never overlap).
    Rect clippedTo(float maxHeight) const;
};

struct Edges {
    float l = 0.0f;
    float t = 0.0f;
    float r = 0.0f;
    float b = 0.0f;

    static Edges all(float v) { return Edges{v, v, v, v}; }
    static Edges xy(float x, float y) { return Edges{x, y, x, y}; }
    static Edges top(float v) { return Edges{0.0f, v, 0.0f, 0.0f}; }
    float horizontal() const { return l + r; }
    float vertical() const { return t + b; }
};

enum class Dir { Row, Column };
enum class Policy { Fixed, Fit, Grow, Percent };
enum class Align { Start, Center, End, Stretch };
enum class Justify { Start, Center, End, SpaceBetween };

// One node of the tree. Build it with the methods and let solve() fill `rect`.
// A child is placed by its PARENT: the parent's `align`/`justify`/`gap` decide
// where it lands, which is why they live here rather than on the child.
struct Node {
    Dir dir = Dir::Column;
    Policy policyW = Policy::Fit;
    Policy policyH = Policy::Fit;
    float fixedW = 0.0f;
    float fixedH = 0.0f;
    float percentW = 0.0f; // 0..1 of the parent's inner width
    float percentH = 0.0f;
    float grow = 1.0f; // weight when policy is Grow
    float minW = 0.0f;
    float minH = 0.0f;
    float maxW = 0.0f; // 0 = unbounded
    float maxH = 0.0f;
    Edges pad;
    float gap = 0.0f;
    Align align = Align::Stretch; // cross axis (this node's children)
    Justify justify = Justify::Start;
    Rect rect; // result
    std::vector<Node> children;

    // --- shape ------------------------------------------------------------
    static Node row(float gap = 0.0f);
    static Node col(float gap = 0.0f);
    static Node none(); // a leaf box, sized by the parent

    Node& size(float w, float h) {
        policyW = policyH = Policy::Fixed;
        fixedW = w;
        fixedH = h;
        return *this;
    }
    Node& width(float w) {
        policyW = Policy::Fixed;
        fixedW = w;
        return *this;
    }
    Node& height(float h) {
        policyH = Policy::Fixed;
        fixedH = h;
        return *this;
    }
    Node& wGrow(float weight = 1.0f) {
        policyW = Policy::Grow;
        grow = weight;
        return *this;
    }
    Node& hGrow(float weight = 1.0f) {
        policyH = Policy::Grow;
        grow = weight;
        return *this;
    }
    Node& wPercent(float fraction) {
        policyW = Policy::Percent;
        percentW = fraction;
        return *this;
    }
    Node& hPercent(float fraction) {
        policyH = Policy::Percent;
        percentH = fraction;
        return *this;
    }
    // Fit is the default; these two exist so a builder line reads as a statement
    // of intent rather than relying on a default.
    Node& wFit() {
        policyW = Policy::Fit;
        return *this;
    }
    Node& hFit() {
        policyH = Policy::Fit;
        return *this;
    }
    Node& minsize(float w, float h) {
        minW = w;
        minH = h;
        return *this;
    }
    Node& maxsize(float w, float h) {
        maxW = w;
        maxH = h;
        return *this;
    }
    Node& padding(const Edges& e) {
        pad = e;
        return *this;
    }
    Node& padding(float all) {
        pad = Edges::all(all);
        return *this;
    }
    Node& padding(float x, float y) {
        pad = Edges::xy(x, y);
        return *this;
    }
    Node& spacing(float g) {
        gap = g;
        return *this;
    }
    Node& crossAlign(Align a) {
        align = a;
        return *this;
    }
    Node& mainAlign(Justify j) {
        justify = j;
        return *this;
    }
    Node& add(Node child) {
        children.push_back(std::move(child));
        return *this;
    }
};

// Size and place `root` inside `area`. Every node's `rect` is absolute and final
// when this returns; children always sit inside the parent's padding box (a
// child that cannot fit is clamped, never pushed outside).
void solve(Node& root, Rect area);

// The size a node would like on its own (padding + content), ignoring Grow and
// Percent. Exposed for callers that must reserve space before solving.
float preferredWidth(const Node& n);
float preferredHeight(const Node& n);

} // namespace layout
} // namespace studio
