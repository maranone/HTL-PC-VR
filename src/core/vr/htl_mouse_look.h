// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Core::HtlMouseLook {

/// Here They Lie played in its own 2D mode (no headset): the mouse turns the camera the way it
/// does in a PC game, by adding to the player controller's ControlRotation, instead of tilting
/// the right stick. Degrees: yaw to the right, pitch down. Starts its worker on first use, and only
/// for this game.
void Add(float yaw_degrees, float pitch_degrees);

/// While held, the view sinks to a crouch (the camera only: the player's body and speed stay as
/// they are), and rises again when let go.
void Crouch(bool held);

} // namespace Core::HtlMouseLook
