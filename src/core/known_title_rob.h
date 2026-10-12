// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <bit>
#include <span>
#include <string_view>
#include "core/known_title_builds.h"

namespace Core::KnownTitle::RobBuilds {

// CUSA02350 1.00, SHA256 of the inspected fSELF:
// 427d027915dba4eba418166eae35d1c24d1b876957dcae1428081bde6f1cdf5c.
// The game packs both eyes into one surface. Its base-console scene is 2304x1296,
// not Astro Bot's 1440x1536 per eye. Both console selections use this fixed HD size.
inline constexpr u32 StereoWidth = 4608;
inline constexpr u32 StereoHeight = 2592;
inline constexpr u32 VideoPoolMiB = 2706; // ceil(615 MiB * 4 pixels * 1.10).
inline constexpr s32 ExtraMemoryMiB = 2304; // Pool growth rounded up to 256 MiB.

inline constexpr std::array<Builds::Change, 6> Changes{{
    {0x1a09108, 2304, StereoWidth, 4},
    {0x1a0910c, 1296, StereoHeight, 4},
    {0x1a09128, 2688, StereoWidth, 4},
    {0x1a0912c, 1512, StereoHeight, 4},
    // mov edx,815 MiB; mov ecx,615 MiB; cmovg rcx,rdx. These MOVs zero-extend
    // the pool size; the game's GNM allocator compares its u32 sizes unsigned.
    {0x1999a5, 0x32f00000, static_cast<u64>(VideoPoolMiB) << 20, 4},
    {0x1999aa, 0x26700000, static_cast<u64>(VideoPoolMiB) << 20, 4},
}};

struct Fingerprint {
    u64 at;
    std::span<const u8> bytes;
};

inline constexpr std::array<u8, 16> RenderSetup{
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
    0x53, 0x48, 0x81, 0xec, 0xf8, 0x00, 0x00, 0x00};
inline constexpr std::array<u8, 16> PoolSetup{
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
    0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec};
inline constexpr std::array<u8, 8> BaseTableRead{
    0xc5, 0xfc, 0x28, 0x15, 0x99, 0x59, 0x26, 0x01};
inline constexpr std::array<u8, 8> NeoTableRead{
    0xc5, 0xfc, 0x28, 0x0d, 0xb1, 0x59, 0x26, 0x01};
inline constexpr std::array<u8, 20> SceneSizeCopy{
    0x41, 0x8b, 0x47, 0x08, 0x89, 0x83, 0x68, 0x4b, 0x00, 0x00,
    0x41, 0x8b, 0x4f, 0x0c, 0x89, 0x8b, 0x6c, 0x4b, 0x00, 0x00};
inline constexpr std::array<u8, 1> MovEdx{0xba};
inline constexpr std::array<u8, 1> MovEcx{0xb9};
inline constexpr std::array<u8, 4> SelectPool{0x48, 0x0f, 0x4f, 0xca};
inline constexpr std::array<Fingerprint, 8> Fingerprints{{
    {0x7a3740, RenderSetup}, {0x199900, PoolSetup},
    {0x7a375f, BaseTableRead}, {0x7a3767, NeoTableRead},
    {0xf7f4d, SceneSizeCopy}, {0x1999a4, MovEdx},
    {0x1999a9, MovEcx}, {0x1999b0, SelectPool},
}};

inline bool Matches(std::span<const u8> image, std::string_view serial) {
    if (serial != "CUSA02350") return false;
    for (const auto& fingerprint : Fingerprints) {
        if (fingerprint.at > image.size() ||
            image.size() - fingerprint.at < fingerprint.bytes.size() ||
            std::memcmp(image.data() + fingerprint.at, fingerprint.bytes.data(),
                        fingerprint.bytes.size()) != 0) {
            return false;
        }
    }
    return Builds::FirstUnexpected(image, Changes) == nullptr;
}

// Validate every instruction and original value before any write. An unsupported or
// already-modified image cannot receive a partial HD patch.
inline bool Apply(std::span<u8> image, std::string_view serial) {
    return Matches(image, serial) && Builds::Apply(image, Changes) == nullptr;
}

// The end callback (0xeba30) is stubbed by HMD HLE, so its counter stays zero.
// The fallback at 0xebb10 then waits three vblanks instead of two callbacks.
// At an explicitly selected 72 Hz, retain the first event wait but leave the
// loop immediately afterwards. GPU frame-label ownership remains intact. The clock conversion correction
// below is also required before increasing the presentation rate.
inline constexpr std::array<u8, 61> FrameWaitCode{
    0x49,0x8b,0xbd,0x90,0x14,0x00,0x00,0xba,0x01,0x00,0x00,0x00,
    0x45,0x31,0xc0,0x48,0x8d,0x74,0x24,0x60,0x48,0x8d,0x4c,0x24,
    0x28,0xe8,0x9f,0x40,0x6c,0x01,0x83,0xfb,0x01,0x74,0x1a,0x41,
    0x83,0xfe,0x01,0x7f,0x14,0x31,0xc0,0x31,0xc9,0x41,0xff,0xc6,
    0xf0,0x41,0x0f,0xb1,0x0c,0x24,0x89,0xc3,0x83,0xfb,0x01,0x7e,
    0x98};
inline constexpr std::array<Builds::Change, 1> PaceChanges{{{0xebb5c,0x74,0xeb,1}}};

inline bool PaceMatches(std::span<const u8> image) {
    return image.size() >= 0xebb3b + FrameWaitCode.size() &&
           std::memcmp(image.data() + 0xebb3b, FrameWaitCode.data(), FrameWaitCode.size()) == 0;
}

// Validate both requested features before either writes to the image.
inline constexpr std::array<Builds::Change, 7> ClockChanges{{
    {0x19edca0,0x3ea50ceae0000000ULL,0,8},
    {0x19edca8,0x3e058e4060000000ULL,0,8},
    {0x19edcb0,0x3ea50ceae0000000ULL,0,8},
    {0x19edcb8,0x3e058e4060000000ULL,0,8},
    {0x8e6ce5,0x09804040,0,4},
    {0x8e6cfc,0x09804040,0,4},
    {0x8e6d23,0x09804040,0,4},
}};

inline constexpr std::array<u8, 21> ClockUpdateCode{
    0x89,0xf0,0xff,0x47,0x14,0xc5,0xfb,0x10,0x0d,0x73,0x4a,0x50,0x01,
    0xc5,0xfb,0x10,0x1d,0x73,0x4a,0x50,0x01};
inline constexpr std::array<u8, 18> ClockReadCode{
    0x0f,0x31,0xc5,0xfb,0x10,0x0d,0x06,0x4b,0x50,0x01,
    0xc5,0xfb,0x10,0x1d,0x06,0x4b,0x50,0x01};

inline constexpr std::array<u8, 81> ClockClampCode{
    0x0f, 0x31, 0x41, 0xbe, 0x40, 0x40, 0x80, 0x09, 0x4c, 0x89, 0xef, 0x89,
    0xc0, 0x48, 0xc1, 0xe2, 0x20, 0x48, 0x09, 0xc2, 0x48, 0x2b, 0x53, 0x48,
    0x48, 0x81, 0xfa, 0x40, 0x40, 0x80, 0x09, 0x41, 0x0f, 0x43, 0xd6, 0x89,
    0xd6, 0xe8, 0x25, 0x25, 0xc0, 0xff, 0x0f, 0x31, 0x4c, 0x89, 0xe7, 0x89,
    0xc0, 0x48, 0xc1, 0xe2, 0x20, 0x48, 0x09, 0xc2, 0x48, 0x2b, 0x93, 0x80,
    0x00, 0x00, 0x00, 0x48, 0x81, 0xfa, 0x40, 0x40, 0x80, 0x09, 0x41, 0x0f,
    0x43, 0xd6, 0x89, 0xd6, 0xe8, 0xfe, 0x24, 0xc0, 0xff};

inline bool ClockMatches(std::span<const u8> image) {
    return image.size() >= 0x19edcc0 &&
           std::memcmp(image.data()+0x4e9230, ClockUpdateCode.data(), ClockUpdateCode.size()) == 0 &&
           std::memcmp(image.data()+0x4e9190, ClockReadCode.data(), ClockReadCode.size()) == 0 &&
           std::memcmp(image.data()+0x8e6ce1, ClockClampCode.data(), ClockClampCode.size()) == 0 &&
           Builds::FirstUnexpected(image, ClockChanges) == nullptr;
}

// Rush of Blood reads the host RDTSC directly but converts its ticks using a
// hardcoded PS4 frequency (1594 MHz). All four conversion constants and the
// two capped clock deltas must use the actual host frequency together.
inline bool ApplyOptions(std::span<u8> image, std::string_view serial, bool hd, bool pace72,
                         u64 host_tsc_hz = 1594000000) {
    if (!Matches(image, serial) ||
        (pace72 && (!PaceMatches(image) || !ClockMatches(image) ||
                    host_tsc_hz < 100000000 || host_tsc_hz > 10000000000ULL))) return false;
    auto clocks = ClockChanges;
    if (pace72) {
        for (size_t i=0; i<4; ++i) {
            clocks[i].now = std::bit_cast<u64>((i % 2 == 0 ? 1e6 : 1.0) / host_tsc_hz);
        }
        for (size_t i=4; i<clocks.size(); ++i) clocks[i].now = host_tsc_hz / 10;
    }
    if (hd) Builds::Apply(image, Changes);
    if (pace72) Builds::Apply(image, clocks);
    if (pace72) Builds::Apply(image, PaceChanges);
    return true;
}

} // namespace Core::KnownTitle::RobBuilds
