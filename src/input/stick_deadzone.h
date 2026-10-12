// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace Input {
// A square central deadzone matches each axis' neutral range while preserving
// the stick direction outside it. Rescaling retains the full range at the edge.
inline std::array<float, 2> ApplyStickDeadzone(float x, float y, float deadzone) {
    if (!std::isfinite(x) || !std::isfinite(y)) return {0.0f, 0.0f};
    x = std::clamp(x, -1.0f, 1.0f);
    y = std::clamp(y, -1.0f, 1.0f);
    deadzone = std::isfinite(deadzone) ? std::clamp(deadzone, 0.0f, 0.8f) : 0.25f;
    const float magnitude = std::max(std::abs(x), std::abs(y));
    if (magnitude <= deadzone) return {0.0f, 0.0f};
    const float scale = (magnitude - deadzone) / ((1.0f - deadzone) * magnitude);
    return {x * scale, y * scale};
}
} // namespace Input
