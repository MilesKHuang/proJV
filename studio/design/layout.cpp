// proJV studio -- the flex solver (see layout.h).
#include "layout.h"

#include <algorithm>
#include <cstddef>

namespace studio {
namespace layout {

Rect Rect::inset(float l, float t, float r, float b) const {
    Rect out;
    out.x = x + l;
    out.y = y + t;
    out.w = w - l - r;
    out.h = h - t - b;
    if (out.w < 0.0f) out.w = 0.0f;
    if (out.h < 0.0f) out.h = 0.0f;
    return out;
}

Rect Rect::clippedTo(float maxHeight) const {
    Rect out = *this;
    const float limit = maxHeight < 0.0f ? 0.0f : maxHeight;
    if (out.h > limit) out.h = limit;
    return out;
}

namespace {

// One code path for both axes: every helper takes an Axis instead of working on
// "width" or "height" directly, so a row and a column cannot drift apart.
enum class Axis { X, Y };

Axis flip(Axis a) {
    return a == Axis::X ? Axis::Y : Axis::X;
}

Axis mainAxisOf(Dir d) {
    return d == Dir::Row ? Axis::X : Axis::Y;
}

Policy policyOf(const Node& n, Axis a) {
    return a == Axis::X ? n.policyW : n.policyH;
}

float fixedOf(const Node& n, Axis a) {
    return a == Axis::X ? n.fixedW : n.fixedH;
}

float percentOf(const Node& n, Axis a) {
    return a == Axis::X ? n.percentW : n.percentH;
}

float minOf(const Node& n, Axis a) {
    return a == Axis::X ? n.minW : n.minH;
}

float maxOf(const Node& n, Axis a) {
    return a == Axis::X ? n.maxW : n.maxH; // 0 = unbounded
}

float padStart(const Edges& e, Axis a) {
    return a == Axis::X ? e.l : e.t;
}

float padEnd(const Edges& e, Axis a) {
    return a == Axis::X ? e.r : e.b;
}

float rectPos(const Rect& r, Axis a) {
    return a == Axis::X ? r.x : r.y;
}

float rectLen(const Rect& r, Axis a) {
    return a == Axis::X ? r.w : r.h;
}

void setPos(Rect& r, Axis a, float v) {
    if (a == Axis::X)
        r.x = v;
    else
        r.y = v;
}

void setLen(Rect& r, Axis a, float v) {
    if (a == Axis::X)
        r.w = v;
    else
        r.h = v;
}

// min/max clamp. A max of 0 means "unbounded", which is why the caller sets it
// only when it means it: a window section with maxH = 0 would vanish.
float clampLen(const Node& n, Axis a, float v) {
    const float lo = minOf(n, a);
    const float hi = maxOf(n, a);
    if (v < lo) v = lo;
    if (hi > 0.0f && v > hi) v = hi;
    return v;
}

float preferredLen(const Node& n, Axis a);
float contentLen(const Node& n, Axis a);
float fitLen(const Node& n, Axis a);
float crossFor(const Node& n, Axis a, float avail, Align al);
void place(Node& n, Rect area);

// How much this node wants, before its own min/max clamp.
float preferredLen(const Node& n, Axis a) {
    const Policy p = policyOf(n, a);
    if (p == Policy::Fixed) return fixedOf(n, a);
    // A percent size is a share of something this pass cannot see; treating it as
    // zero keeps "fit" from inventing a size for it.
    if (p == Policy::Percent) return 0.0f;
    return contentLen(n, a) + padStart(n.pad, a) + padEnd(n.pad, a);
}

// What the children need along `a`.
float contentLen(const Node& n, Axis a) {
    if (n.children.empty()) return 0.0f;
    const Axis main = mainAxisOf(n.dir);
    float acc = 0.0f;
    if (a == main) {
        for (const Node& c : n.children) acc += preferredLen(c, a);
        acc += n.gap * static_cast<float>(n.children.size() - 1);
    } else {
        for (const Node& c : n.children) acc = std::max(acc, preferredLen(c, a));
    }
    return acc;
}

float fitLen(const Node& n, Axis a) {
    return clampLen(n, a, preferredLen(n, a));
}

// The size a child gets on the parent's cross axis. Stretch fills the parent;
// the other alignments use the child's own content size (or its fixed size).
float crossFor(const Node& n, Axis a, float avail, Align al) {
    const Policy p = policyOf(n, a);
    if (p == Policy::Fixed) return clampLen(n, a, fixedOf(n, a));
    if (p == Policy::Percent) return clampLen(n, a, avail * percentOf(n, a));
    if (p == Policy::Grow) return clampLen(n, a, avail);
    return clampLen(n, a, al == Align::Stretch ? avail : fitLen(n, a));
}

void place(Node& n, Rect area) {
    n.rect = area;
    if (n.children.empty()) return;

    const Rect inner = area.inset(n.pad.l, n.pad.t, n.pad.r, n.pad.b);
    const Axis main = mainAxisOf(n.dir);
    const Axis cross = flip(main);
    const float innerMain = rectLen(inner, main);
    const float innerCross = rectLen(inner, cross);
    const size_t count = n.children.size();
    const float gapTotal = n.gap * static_cast<float>(count - 1);

    // Pass 1: every child that is not Grow takes its size now, and the rest is
    // what the Grow children share.
    std::vector<float> mainLen(count, 0.0f);
    float used = 0.0f;
    float weightSum = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        const Node& c = n.children[i];
        const Policy p = policyOf(c, main);
        if (p == Policy::Grow) {
            weightSum += std::max(0.0f, c.grow > 0.0f ? c.grow : 1.0f);
            continue;
        }
        float v = 0.0f;
        if (p == Policy::Fixed)
            v = fixedOf(c, main);
        else if (p == Policy::Percent)
            v = innerMain * percentOf(c, main);
        else
            v = fitLen(c, main);
        mainLen[i] = clampLen(c, main, v);
        used += mainLen[i];
    }

    const float freeSpace = std::max(0.0f, innerMain - used - gapTotal);
    if (weightSum > 0.0f) {
        // Distribute, then hand whatever a clamped child refused to the ones that
        // can still grow. Without this a max-width child leaves a hole: the row
        // ends short of its parent even though a sibling had room. Four passes is
        // enough for any window here and guarantees termination.
        float remaining = freeSpace;
        float remainingWeight = weightSum;
        std::vector<bool> locked(count, false);
        for (int pass = 0; pass < 4 && remainingWeight > 0.0f; ++pass) {
            const float unit = remaining / remainingWeight;
            float consumed = 0.0f;
            bool anyLocked = false;
            for (size_t i = 0; i < count; ++i) {
                const Node& c = n.children[i];
                if (policyOf(c, main) != Policy::Grow || locked[i]) continue;
                const float weight = std::max(0.0f, c.grow > 0.0f ? c.grow : 1.0f);
                const float want = unit * weight;
                const float capped = clampLen(c, main, mainLen[i] + want);
                const float added = std::max(0.0f, capped - mainLen[i]);
                mainLen[i] = capped;
                consumed += added;
                if (added < want - 0.001f) {
                    locked[i] = true;
                    remainingWeight -= weight;
                    anyLocked = true;
                }
            }
            remaining = std::max(0.0f, remaining - consumed);
            if (!anyLocked) break;
        }
    }

    // Pass 2: justify the run along the main axis.
    float sumMain = 0.0f;
    for (float v : mainLen) sumMain += v;
    const float leftover = std::max(0.0f, innerMain - sumMain - gapTotal);
    float offset = 0.0f;
    float extraGap = 0.0f;
    switch (n.justify) {
    case Justify::Start:
        break;
    case Justify::Center:
        offset = leftover * 0.5f;
        break;
    case Justify::End:
        offset = leftover;
        break;
    case Justify::SpaceBetween:
        if (count > 1) extraGap = leftover / static_cast<float>(count - 1);
        break;
    }

    // Pass 3: place, then recurse. A child is only ever solved against the rect
    // it was just given, so nesting cannot reach outside its parent.
    float cursor = rectPos(inner, main) + offset;
    for (size_t i = 0; i < count; ++i) {
        Node& c = n.children[i];
        const float crossLen = crossFor(c, cross, innerCross, n.align);
        float crossPos = rectPos(inner, cross);
        if (n.align == Align::Center)
            crossPos += (innerCross - crossLen) * 0.5f;
        else if (n.align == Align::End)
            crossPos += innerCross - crossLen;

        Rect r;
        setPos(r, main, cursor);
        setLen(r, main, mainLen[i]);
        setPos(r, cross, crossPos);
        setLen(r, cross, crossLen);
        place(c, r);

        cursor += mainLen[i] + n.gap + extraGap;
    }
}

} // namespace

Node Node::row(float gap) {
    Node n;
    n.dir = Dir::Row;
    n.gap = gap;
    return n;
}

Node Node::col(float gap) {
    Node n;
    n.dir = Dir::Column;
    n.gap = gap;
    return n;
}

Node Node::none() {
    return Node();
}

void solve(Node& root, Rect area) {
    place(root, area);
}

float preferredWidth(const Node& n) {
    return fitLen(n, Axis::X);
}

float preferredHeight(const Node& n) {
    return fitLen(n, Axis::Y);
}

} // namespace layout
} // namespace studio
