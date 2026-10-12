// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Core::HtlHands {

/// Here They Lie's player is a full body animated with HumanIK. The torch hand follows the
/// pad's tilt (see the torch hand in openxr_host), the other hand only ever plays animations.
/// This drives that free hand, the right one, to where a tracked controller is: the player's
/// animation instance has an unused right-wrist reach node, which is aimed at that point each
/// frame. `position` is the controller's grip from the head, in the head's own axes (metres;
/// x right, y up, -z forward, as OpenXR gives it), and `orientation` (x, y, z, w) its turn from
/// the head's: the hand is held as if it gripped that controller.
void SetRightHand(const float (&position)[3], const float (&orientation)[4]);

/// The controller is no longer tracked: the hand goes back to its animations.
void ClearRightHand();

/// The torch hand, the left one, reaches for where the left controller is (the same axes as
/// SetRightHand), whether the torch is lit or not; the torch itself still aims by the pad's tilt.
void SetLeftHand(const float (&position)[3]);

/// The left controller is no longer tracked: the torch hand goes back to the game.
void ClearLeftHand();

/// For desktop tests, SHADPS4_HTL_TEST_HAND=x,y,z[,pitch,yaw,roll] holds the hand at that point
/// from the head (the same axes as SetRightHand), turned by those degrees about the head's x, y
/// and z; SHADPS4_HTL_TEST_LEFT_HAND=x,y,z holds the torch hand at a point. Called from the
/// pad's polling; does nothing otherwise.
void PollTestHand();

} // namespace Core::HtlHands
