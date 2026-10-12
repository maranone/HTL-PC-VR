// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>

#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/libs.h"
#include "core/libraries/move/move.h"
#include "core/libraries/move/move_error.h"
#include "core/vr/vr_runtime.h"
#include "move.h"

namespace Libraries::Move {

static bool g_library_initialized = false;

// Handles are told apart from those of other devices (libSceVrTracker requires it); the low
// byte is the controller's index, which is the hand that stands in for it (0 right, 1 left).
static constexpr s32 HandleBase = 0x30b0000;
static std::array<std::atomic<u32>, Core::Vr::MaxMoves> g_read_count{};

/// The controller a handle opened, or -1.
static s32 IndexOf(s32 handle) {
    const s32 index = handle - HandleBase;
    return index >= 0 && index < Core::Vr::MaxMoves ? index : -1;
}

/// What a Move controller's sensors would read where the host's controller is: the
/// accelerometer in units of gravity (the reaction to it, +1 up at rest), the gyroscope in
/// radians a second, both in the controller's own directions (+X right, +Y out of its buttons,
/// +Z back along it towards the player).
static void FillSensors(const Core::Vr::DeviceState& device, OrbisMoveData* data) {
    using namespace Core::Vr;
    const Quat to_controller = Conjugate(device.pose.orientation);
    const Vec3 up = Rotate(to_controller, {0.0f, 1.0f, 0.0f});
    const Vec3 spin = Rotate(to_controller, device.angular_velocity);
    data->accelerometer[0] = up.x;
    data->accelerometer[1] = up.y;
    data->accelerometer[2] = up.z;
    data->gyro[0] = spin.x;
    data->gyro[1] = spin.y;
    data->gyro[2] = spin.z;
}

s32 PS4_SYSV_ABI sceMoveInit() {
    if (g_library_initialized) {
        return ORBIS_MOVE_ERROR_ALREADY_INIT;
    }
    g_library_initialized = true;
    auto& runtime = Core::Vr::Runtime::Instance();
    if (runtime.IsHeadsetConnected()) {
        runtime.RequestMoves();
        LOG_INFO(Lib_Move, "Move controllers asked for: the headset's controllers stand in for "
                           "a pair (right = 0, left = 1)");
    } else {
        LOG_WARNING(Lib_Move, "Move controllers are only there with a virtual headset");
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveOpen(Libraries::UserService::OrbisUserServiceUserId user_id, s32 type,
                             s32 index) {
    LOG_INFO(Lib_Move, "called, user_id = {}, type = {}, index = {}", user_id, type, index);
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (index < 0 || index >= Core::Vr::MaxMoves) {
        return ORBIS_MOVE_ERROR_INVALID_ARG;
    }
    // Even when no controller is connected, this returns a handle.
    return HandleBase + index;
}

s32 PS4_SYSV_ABI sceMoveGetDeviceInfo(s32 handle, OrbisMoveDeviceInfo* info) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (info == nullptr) {
        return ORBIS_MOVE_ERROR_INVALID_ARG;
    }
    const s32 index = IndexOf(handle);
    if (index < 0) {
        return ORBIS_MOVE_ERROR_INVALID_HANDLE;
    }
    if (!Core::Vr::Runtime::Instance().GetMove(index).connected) {
        return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
    }
    // The sphere is 45 mm across; the sensors need no correction.
    info->sphere_radius = 22.5f;
    info->accelerometer_offset[0] = 0.0f;
    info->accelerometer_offset[1] = 0.0f;
    info->accelerometer_offset[2] = 0.0f;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveReadStateLatest(s32 handle, OrbisMoveData* data) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (data == nullptr) {
        return ORBIS_MOVE_ERROR_INVALID_ARG;
    }
    const s32 index = IndexOf(handle);
    if (index < 0) {
        return ORBIS_MOVE_ERROR_INVALID_HANDLE;
    }
    const Core::Vr::MoveState move = Core::Vr::Runtime::Instance().GetMove(index);
    if (!move.connected) {
        return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
    }
    *data = {};
    FillSensors(move.device, data);
    data->button_data.button_data = move.buttons;
    data->button_data.trigger_data = move.trigger;
    // Each change of what the title is told is pressed (for finding what it reacts to).
    static std::array<std::atomic<u32>, Core::Vr::MaxMoves> shown_buttons{};
    if (const u32 shown = shown_buttons[index].exchange(move.buttons); shown != move.buttons) {
        LOG_INFO(Lib_Move, "Move {} buttons {:#06x} (were {:#06x}), tracked {}", index,
                 move.buttons, shown, move.device.tracked);
    }
    data->timestamp = static_cast<s64>(Libraries::Kernel::sceKernelGetProcessTime());
    data->count = static_cast<s32>(g_read_count[index]++);
    data->temperature = 25.0f;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveReadStateRecent(s32 handle, s64 timestamp, OrbisMoveData* data,
                                        s32* out_count) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (timestamp < 0 || data == nullptr || out_count == nullptr) {
        return ORBIS_MOVE_ERROR_INVALID_ARG;
    }
    // Only the latest state is kept: it is the one sample since any moment asked about.
    const s32 result = sceMoveReadStateLatest(handle, data);
    *out_count = result == ORBIS_OK ? 1 : 0;
    return result;
}

s32 PS4_SYSV_ABI sceMoveGetExtensionPortInfo(s32 handle, void* data) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (data == nullptr) {
        return ORBIS_MOVE_ERROR_INVALID_ARG;
    }
    return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
}

s32 PS4_SYSV_ABI sceMoveSetVibration(s32 handle, u8 intensity) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    const s32 index = IndexOf(handle);
    if (index < 0) {
        return ORBIS_MOVE_ERROR_INVALID_HANDLE;
    }
    auto& runtime = Core::Vr::Runtime::Instance();
    if (!runtime.GetMove(index).connected) {
        return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
    }
    runtime.SetMoveVibration(index, intensity);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveSetLightSphere(s32 handle, u8 red, u8 green, u8 blue) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    const s32 index = IndexOf(handle);
    if (index < 0) {
        return ORBIS_MOVE_ERROR_INVALID_HANDLE;
    }
    // Nothing to light: the host's controllers are located without it.
    return Core::Vr::Runtime::Instance().GetMove(index).connected
               ? ORBIS_OK
               : ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
}

s32 PS4_SYSV_ABI sceMoveResetLightSphere(s32 handle) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    // Returns success, even if no controllers are connected.
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveClose(s32 handle) {
    LOG_DEBUG(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (const s32 index = IndexOf(handle); index >= 0) {
        Core::Vr::Runtime::Instance().SetMoveVibration(index, 0);
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveTerm() {
    LOG_DEBUG(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    g_library_initialized = false;
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("j1ITE-EoJmE", "libSceMove", 1, "libSceMove", sceMoveInit);
    LIB_FUNCTION("HzC60MfjJxU", "libSceMove", 1, "libSceMove", sceMoveOpen);
    LIB_FUNCTION("GWXTyxs4QbE", "libSceMove", 1, "libSceMove", sceMoveGetDeviceInfo);
    LIB_FUNCTION("ttU+JOhShl4", "libSceMove", 1, "libSceMove", sceMoveReadStateLatest);
    LIB_FUNCTION("f2bcpK6kJfg", "libSceMove", 1, "libSceMove", sceMoveReadStateRecent);
    LIB_FUNCTION("y5h7f8H1Jnk", "libSceMove", 1, "libSceMove", sceMoveGetExtensionPortInfo);
    LIB_FUNCTION("IFQwtT2CeY0", "libSceMove", 1, "libSceMove", sceMoveSetVibration);
    LIB_FUNCTION("T8KYHPs1JE8", "libSceMove", 1, "libSceMove", sceMoveSetLightSphere);
    LIB_FUNCTION("zuxWAg3HAac", "libSceMove", 1, "libSceMove", sceMoveResetLightSphere);
    LIB_FUNCTION("XX6wlxpHyeo", "libSceMove", 1, "libSceMove", sceMoveClose);
    LIB_FUNCTION("tsZi60H4ypY", "libSceMove", 1, "libSceMove", sceMoveTerm);
};

} // namespace Libraries::Move
