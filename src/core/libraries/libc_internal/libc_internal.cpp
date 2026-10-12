// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <common/va_ctx.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "libc_internal.h"
#include "libc_internal_cxa.h"
#include "libc_internal_io.h"
#include "libc_internal_math.h"
#include "libc_internal_memory.h"
#include "libc_internal_str.h"
#include "libc_internal_threads.h"
#include "printf.h"

namespace Libraries::LibcInternal {

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    RegisterlibSceLibcInternalMath(sym);
    RegisterlibSceLibcInternalStr(sym);
    RegisterlibSceLibcInternalMemory(sym);
    RegisterlibSceLibcInternalIo(sym);
    RegisterlibSceLibcInternalThreads(sym);
#ifndef SHADPS4_ENABLE_FEX_GUEST_CPU
    // The C++ runtime's static-initialisation guards and atexit: modules a game ships (Here They
    // Lie's libSceNpToolkit2.prx) take them from here when no libSceLibcInternal.sprx is around,
    // and a guard that answers "already done" leaves their statics unmade. (The FEX build
    // registers these in ForceRegisterLib.)
    RegisterFexLibcCxaAliases(sym);
#endif
}

void ForceRegisterLib(Core::Loader::SymbolsResolver* sym) {
    // Used to forcibly enable HLEs for broken LLE functions.
    ForceRegisterlibSceLibcInternalIo(sym);
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
    RegisterFexLibcMemoryAliases(sym);
    RegisterFexLibcMathAliases(sym);
    RegisterFexLibcStrAliases(sym);
    RegisterFexLibcCxaAliases(sym);
#endif
}
} // namespace Libraries::LibcInternal
