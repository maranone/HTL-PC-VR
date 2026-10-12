// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstring>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "common/types.h"
#include "core/memory.h"

/// Here They Lie's Unreal objects, as the emulator finds them in guest memory (HtlHands,
/// HtlOptions). Everything is read defensively: objects come and go under the game's feet.
namespace Core::HtlUnreal {

// Where Here They Lie (CUSA05344, its only release) keeps Unreal's tables: pointers to the
// FName chunks and to the UObject items (16 bytes each, the object first), with their count.
constexpr VAddr NamesGlobal = 0x80453cb70;
constexpr VAddr ObjectsGlobal = 0x8045c3d60;
constexpr u32 NamesPerChunk = 16384;

// UObject: flags at +0x08, class at +0x10, name at +0x18 (index, then number), outer at +0x20.
constexpr u64 ObjectFlags = 0x08;
constexpr u64 ObjectClass = 0x10;
constexpr u64 ObjectName = 0x18;
constexpr u64 ObjectOuter = 0x20;
constexpr u32 ClassDefaultObject = 0x10;

/// Reads guest memory that may have gone away since it was last looked at.
inline bool Read(VAddr address, void* out, size_t size) {
#ifdef _WIN32
    SIZE_T done = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), out,
                             size, &done) &&
           done == size;
#else
    if (!Memory::Instance()->IsValidMapping(address, size)) {
        return false;
    }
    std::memcpy(out, reinterpret_cast<const void*>(address), size);
    return true;
#endif
}

template <typename T>
bool Read(VAddr address, T& out) {
    return Read(address, &out, sizeof(T));
}

inline void Write(VAddr address, const void* data, size_t size) {
    std::memcpy(reinterpret_cast<void*>(address), data, size);
}

template <typename T>
void Write(VAddr address, const T& value) {
    Write(address, &value, sizeof(T));
}

/// The name an FName index stands for.
inline bool Name(u32 index, char* out, size_t size) {
    VAddr chunks, chunk, entry;
    if (!Read(NamesGlobal, chunks) || !Read(chunks + 8 * (index / NamesPerChunk), chunk) ||
        chunk == 0 || !Read(chunk + 8 * (index % NamesPerChunk), entry) || entry == 0 ||
        !Read(entry + 0x10, out, size)) {
        return false;
    }
    out[size - 1] = '\0';
    return true;
}

inline bool NameIs(u32 index, std::string_view name) {
    std::array<char, 64> text{};
    return Name(index, text.data(), text.size()) && name == text.data();
}

/// The object is called `name` (with no number after it).
inline bool ObjectIs(VAddr object, std::string_view name) {
    u32 index, number;
    return Read(object + ObjectName, index) && Read(object + ObjectName + 4, number) &&
           number == 0 && NameIs(index, name);
}

inline bool ClassIs(VAddr object, std::string_view name) {
    VAddr klass;
    u32 index;
    return object != 0 && Read(object + ObjectClass, klass) && Read(klass + ObjectName, index) &&
           NameIs(index, name);
}

/// Calls `visit(object, klass)` for every live object that is not a class default, until it
/// returns true. False if the table could not be read.
template <typename Visit>
bool ForEachObject(Visit&& visit) {
    struct Objects {
        VAddr items;
        s32 max;
        s32 count;
    } objects;
    if (!Read(ObjectsGlobal, objects) || objects.count <= 0 || objects.count > objects.max ||
        objects.max > (1 << 24)) {
        return false;
    }
    std::vector<VAddr> items(static_cast<size_t>(objects.count) * 2);
    if (!Read(objects.items, items.data(), items.size() * sizeof(VAddr))) {
        return false;
    }
    for (size_t i = 0; i < items.size(); i += 2) {
        const VAddr object = items[i];
        VAddr klass;
        u32 flags;
        if (object == 0 || !Read(object + ObjectClass, klass) ||
            !Read(object + ObjectFlags, flags) || (flags & ClassDefaultObject) != 0) {
            continue;
        }
        if (visit(object, klass)) {
            break;
        }
    }
    return true;
}

} // namespace Core::HtlUnreal
