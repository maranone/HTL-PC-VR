// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string>

#include "common/assert.h"
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/hmd/hmd_setup_dialog.h"
#include "core/libraries/libs.h"
#include "core/vr/vr_runtime.h"

namespace Libraries::HmdSetupDialog {

s32 PS4_SYSV_ABI sceHmdSetupDialogInitialize() {
    LOG_ERROR(Lib_HmdSetupDialog, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdSetupDialogClose() {
    LOG_ERROR(Lib_HmdSetupDialog, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdSetupDialogOpen(const OrbisHmdSetupDialogParam* param) {
    LOG_ERROR(Lib_HmdSetupDialog, "(STUBBED) called");
    // Who asks, the first times (titles open it when they find the headset not ready).
    static int traced = 0;
    if (traced < 6) {
        ++traced;
        std::string chain = fmt::format("{:#x}", reinterpret_cast<u64>(__builtin_return_address(0)));
        u64 frame = *static_cast<u64*>(__builtin_frame_address(0));
        for (int depth = 0; depth < 8 && frame > 0x10000 && (frame & 7) == 0; ++depth) {
            const u64* at = reinterpret_cast<const u64*>(frame);
            chain += fmt::format(" <- {:#x}", at[1]);
            if (at[0] <= frame) {
                break;
            }
            frame = at[0];
        }
        LOG_INFO(Lib_HmdSetupDialog, "opened from {}; game flags at 0x8024404b0 = {:#x}", chain,
                 *reinterpret_cast<const u32*>(0x8024404b0ULL));
    }
    // On real hardware, a dialog would show up telling the user to connect a PSVR headset.
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdSetupDialogGetResult(OrbisHmdSetupDialogResult* result) {
    LOG_DEBUG(Lib_HmdSetupDialog, "called");
    // Result::OK means a headset was connected. Without one, simulate the user pressing circle
    // to cancel the dialog.
    result->result = Core::Vr::Runtime::Instance().IsHeadsetConnected()
                         ? Libraries::CommonDialog::Result::OK
                         : Libraries::CommonDialog::Result::USER_CANCELED;
    return ORBIS_OK;
}

Libraries::CommonDialog::Status PS4_SYSV_ABI sceHmdSetupDialogUpdateStatus() {
    LOG_ERROR(Lib_HmdSetupDialog, "(STUBBED) called");
    return Libraries::CommonDialog::Status::FINISHED;
}

Libraries::CommonDialog::Status PS4_SYSV_ABI sceHmdSetupDialogGetStatus() {
    LOG_ERROR(Lib_HmdSetupDialog, "(STUBBED) called");
    return Libraries::CommonDialog::Status::FINISHED;
}

s32 PS4_SYSV_ABI sceHmdSetupDialogTerminate() {
    LOG_ERROR(Lib_HmdSetupDialog, "(STUBBED) called");
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("nmHzU4Gh0xs", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogClose);
    LIB_FUNCTION("6lVRHMV5LY0", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogGetResult);
    LIB_FUNCTION("J9eBpW1udl4", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogGetStatus);
    LIB_FUNCTION("NB1Y2kA2jCY", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogInitialize);
    LIB_FUNCTION("NNgiV4T+akU", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogOpen);
    LIB_FUNCTION("+z4OJmFreZc", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogTerminate);
    LIB_FUNCTION("Ud7j3+RDIBg", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogUpdateStatus);
};

} // namespace Libraries::HmdSetupDialog