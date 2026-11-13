#pragma once

// The decisions the macOS input injector makes, as plain functions with no Apple headers, so every platform's
// tests can check them: drag or move, click counts, which Mac key a remote key becomes, and where a normalized
// pointer position lands on a display measured in points.

#include "input_injector.hpp"

#include <cstdint>
#include <vector>

namespace cppdesk {

enum class MacButton { Left, Right, Other };
enum class MacMouseKind { Move, Drag, Down, Up };
struct MacMouseEvent {
    MacMouseKind kind = MacMouseKind::Move;
    MacButton button = MacButton::Left;
    int clickCount = 1;
};

// Tracks the held buttons and recent clicks of the one pointer the host drives.
class MacMouseTracker {
public:
    // Drag (of the held button) while a button is held, otherwise Move.
    MacMouseEvent onMove() const;
    // A press counts as the next click of a double or triple click when it follows the previous press of the same
    // button within 500 ms and 4 points. The release carries its press's count.
    MacMouseEvent onButton(MouseButtonId button, bool down, float x, float y, uint64_t nowMs);
    void reset();

private:
    struct State {
        bool down = false;
        bool hasPrevious = false;
        uint64_t lastDownMs = 0;
        float x = 0.0f, y = 0.0f;
        int count = 1;
    };
    State state_[3];
};

// Remote Ctrl acts as Command, remote Win as Control, otherwise InputInjector::mapVkToMacKeyCode; 0xFFFF if unmapped.
uint16_t macHostKeyCodeForVk(uint16_t vk);

struct PointRect { double x, y, w, h; };
struct PointXY { double x, y; };

// A normalized (0..1, clamped) position inside `display` (points); 1.0 is the last pixel, not one past it.
PointXY macPointForNormalized(float nx, float ny, const PointRect& display);

// The display for a monitor index, in CGGetActiveDisplayList order: the first one if the index is out of range,
// an empty rectangle for an empty list.
PointRect macDisplayForIndex(int index, const std::vector<PointRect>& displays);

} // namespace cppdesk
