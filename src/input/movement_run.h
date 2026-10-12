#pragma once

namespace Input {
// One click starts a run; neutral movement ends it and requires a fresh click.
class MovementRun {
public:
    bool Update(bool moving, bool click, bool cancelled = false) {
        const bool pressed = click && !click_held;
        click_held = click;
        if (!moving || cancelled) running = false;
        else if (pressed) running = true;
        return running;
    }
    void Reset() { running = click_held = false; }
private:
    bool running{}, click_held{};
};
} // namespace Input
