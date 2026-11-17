// proJV TUI -- paste detection implementation (see paste_flatten.h).
#include "paste_flatten.h"

namespace paste_flatten {

bool BurstWindow::tick(std::chrono::steady_clock::time_point now) {
    const bool inBurst = hasLast_ && (now - last_) < kBurstGap;
    last_ = now;
    hasLast_ = true;
    return inBurst;
}

void BurstWindow::reset() {
    last_ = std::chrono::steady_clock::time_point{};
    hasLast_ = false;
}

bool shouldFlattenReturn(bool inBurst, size_t currentInputSize, size_t maxChars) {
    return inBurst && currentInputSize < maxChars;
}

} // namespace paste_flatten
