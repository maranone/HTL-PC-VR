// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Core::HtlOptions {

/// Here They Lie's pause menu carries two options of the emulator's own, Controller Hands and
/// Comfort Vignette: the lab's menu overlay adds them as checkboxes to its Options page. This
/// keeps those checkboxes in step with htl_options.ini in the user folder, and has the player
/// strafe as fast as they walk (SHADPS4_HTL_FULL_STRAFE=0: as slowly as the game has it).
/// Called from the pad's polling; it starts once, and only for this game.
void Start();

/// The controllers stand in for the hands: the right one reaches, the left one aims the torch,
/// and the triggers use things and switch the torch. Off, the torch follows the head and the
/// triggers are R2 and L2, as on a PS4. Always on for other games.
bool ControllerHands();

/// The view narrows while the stick turns the player smoothly (the game's comfort vignette).
bool ComfortVignette();

} // namespace Core::HtlOptions
