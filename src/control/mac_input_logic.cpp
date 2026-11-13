#include "mac_input_logic.hpp"

#include <algorithm>
#include <cmath>

namespace cppdesk {

namespace {

int stateIndex(MouseButtonId b) {
    switch (b) {
        case MouseButtonId::Left: return 0;
        case MouseButtonId::Right: return 1;
        default: return 2;
    }
}

MacButton macButton(int index) { return index == 0 ? MacButton::Left : index == 1 ? MacButton::Right : MacButton::Other; }

constexpr uint64_t kClickWindowMs = 500;
constexpr float kClickSlopPoints = 4.0f;

} // namespace

MacMouseEvent MacMouseTracker::onMove() const {
    MacMouseEvent ev;
    for (int i = 0; i < 3; ++i) {
        if (state_[i].down) {
            ev.kind = MacMouseKind::Drag;
            ev.button = macButton(i);
            ev.clickCount = state_[i].count;
            return ev;
        }
    }
    return ev;
}

MacMouseEvent MacMouseTracker::onButton(MouseButtonId button, bool down, float x, float y, uint64_t nowMs) {
    const int i = stateIndex(button);
    State& s = state_[i];
    if (down) {
        const bool chained = s.hasPrevious && nowMs >= s.lastDownMs && nowMs - s.lastDownMs <= kClickWindowMs &&
                             std::fabs(x - s.x) <= kClickSlopPoints && std::fabs(y - s.y) <= kClickSlopPoints;
        s.count = chained ? s.count + 1 : 1;
        s.hasPrevious = true;
        s.lastDownMs = nowMs;
        s.x = x;
        s.y = y;
    }
    s.down = down;
    MacMouseEvent ev;
    ev.kind = down ? MacMouseKind::Down : MacMouseKind::Up;
    ev.button = macButton(i);
    ev.clickCount = s.count;
    return ev;
}

bool MacMouseTracker::isDown(MacButton button) const {
    return state_[button == MacButton::Left ? 0 : button == MacButton::Right ? 1 : 2].down;
}

void MacMouseTracker::reset() {
    for (State& s : state_) s = State{};
}

uint16_t macHostKeyCodeForVk(uint16_t vk) {
    switch (vk) {
        case 0x11: case 0xA2: return 0x37;   // Ctrl -> Command
        case 0xA3: return 0x36;              // right Ctrl -> right Command
        case 0x5B: return 0x3B;              // Win -> Control
        case 0x5C: return 0x3E;              // right Win -> right Control
        default: return InputInjector::mapVkToMacKeyCode(vk);
    }
}

PointXY macPointForNormalized(float nx, float ny, const PointRect& d) {
    const double cx = std::clamp(static_cast<double>(nx), 0.0, 1.0);
    const double cy = std::clamp(static_cast<double>(ny), 0.0, 1.0);
    return PointXY{ d.x + cx * std::max(1.0, d.w - 1.0), d.y + cy * std::max(1.0, d.h - 1.0) };
}

PointRect macDisplayForIndex(int index, const std::vector<PointRect>& displays) {
    if (displays.empty()) return PointRect{ 0, 0, 0, 0 };
    if (index < 0 || static_cast<size_t>(index) >= displays.size()) return displays.front();
    return displays[static_cast<size_t>(index)];
}

} // namespace cppdesk
