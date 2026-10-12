// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "core/libraries/system/userservice.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::Move {

/// OrbisMoveButtonData::button_data, as on the PlayStation 3's Move.
enum OrbisMoveButton : u16 {
    ORBIS_MOVE_BUTTON_SELECT = 1 << 0,
    ORBIS_MOVE_BUTTON_T = 1 << 1,
    ORBIS_MOVE_BUTTON_MOVE = 1 << 2,
    ORBIS_MOVE_BUTTON_START = 1 << 3,
    ORBIS_MOVE_BUTTON_TRIANGLE = 1 << 4,
    ORBIS_MOVE_BUTTON_CIRCLE = 1 << 5,
    ORBIS_MOVE_BUTTON_CROSS = 1 << 6,
    ORBIS_MOVE_BUTTON_SQUARE = 1 << 7,
};

struct OrbisMoveDeviceInfo {
    float sphere_radius;
    float accelerometer_offset[3];
};

struct OrbisMoveButtonData {
    u16 button_data;
    u16 trigger_data;
};

struct OrbisMoveExtensionPortData {
    u16 status;
    u16 digital0;
    u16 digital1;
    u16 analog_right_x;
    u16 analog_right_y;
    u16 analog_left_x;
    u16 analog_left_y;
    unsigned char custom[5];
};

struct OrbisMoveData {
    float accelerometer[3];
    float gyro[3];
    OrbisMoveButtonData button_data;
    OrbisMoveExtensionPortData extension_data;
    s64 timestamp;
    s32 count;
    float temperature;
};

void RegisterLib(Core::Loader::SymbolsResolver* sym);
} // namespace Libraries::Move