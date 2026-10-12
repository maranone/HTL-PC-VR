// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cmath>
#include <cstring>
// Read-only probes of the fingerprinted HTL UE4 build. No guest functions are called.
#include <optional>
#include <string>
#include <unordered_map>
#include "common/session_diagnostics.h"
#include "core/memory.h"

namespace Core::HtlObserver {
template <typename T> std::optional<T> Peek(u64 address) {
    if (!address || !Memory::Instance()->IsValidMapping(address, sizeof(T))) return {};
    T value{};
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

inline std::string Name(u64 base, u64 object) {
    const auto index = Peek<s32>(object + 0x18);
    const auto names = Peek<u64>(base + 0x453cb70);
    if (!index || *index < 0 || *index >= 0x200000 || !names || !*names) return {};
    static thread_local std::unordered_map<u64,std::string> cache;
    const auto key = (*names << 21) ^ static_cast<u32>(*index);
    if (const auto found = cache.find(key); found != cache.end()) return found->second;
    const auto block = Peek<u64>(*names + (*index / 16384) * 8);
    if (!block || !*block) return {};
    const auto entry = Peek<u64>(*block + (*index % 16384) * 8);
    if (!entry || !*entry) return {};
    const auto flags = Peek<u32>(*entry);
    if (!flags || (*flags & 1)) return {}; // Only bounded ANSI property/class names.
    std::string result;
    for (u32 i = 0; i < 128; ++i) {
        auto ch = Peek<u8>(*entry + 0x10 + i);
        if (!ch) return {};
        if (!*ch) {
            if (cache.size() < 8192) cache.emplace(key,result);
            return result;
        }
        if (*ch < 32 || *ch >= 127) return {};
        result += static_cast<char>(*ch);
    }
    return {};
}

inline std::string Class(u64 base, u64 object) {
    const auto cls = Peek<u64>(object + 0x10);
    return cls && *cls ? Name(base, *cls) : std::string{};
}

inline u64 Property(u64 base, u64 object, std::string_view name, std::string_view type) {
    auto cls = Peek<u64>(object + 0x10);
    static thread_local std::unordered_map<std::string,u64> cache;
    const auto key = std::to_string(cls.value_or(0))+":"+std::string(name)+":"+std::string(type);
    if (const auto found = cache.find(key); found != cache.end()) {
        if (Name(base,found->second)==name && Class(base,found->second)==type) return found->second;
        cache.erase(found);
    }
    for (u32 depth = 0; cls && *cls && depth < 32; ++depth) {
        const auto kind = Class(base, *cls);
        if (kind != "Class" && kind != "BlueprintGeneratedClass" && kind != "WidgetBlueprintGeneratedClass" && kind != "DynamicClass") return 0;
        auto child = Peek<u64>(*cls + 0x38);
        for (u32 i = 0; child && *child && i < 512; ++i) {
            if (Name(base, *child) == name && Class(base, *child) == type) {
                if (cache.size()<512) cache.emplace(key,*child);
                return *child;
            }
            const auto next = Peek<u64>(*child + 0x28);
            if (next == child) return 0;
            child = next;
        }
        const auto parent = Peek<u64>(*cls + 0x30);
        if (parent == cls) return 0;
        cls = parent;
    }
    return 0;
}

inline std::optional<u64> Value(u64 base, u64 object, std::string_view name,
                                std::string_view type, size_t size) {
    const auto property = Property(base, object, name, type);
    const auto offset = Peek<s32>(property + 0x50);
    if (!property || !offset || *offset < 0x28 || *offset > 0x20000 ||
        !Memory::Instance()->IsValidMapping(object + *offset, size)) return {};
    return object + *offset;
}

inline std::optional<bool> Bool(u64 base, u64 object, std::string_view name) {
    const auto property = Property(base, object, name, "BoolProperty");
    const auto offset = Peek<s32>(property + 0x50);
    const auto byte = Peek<u8>(property + 0x79);
    const auto mask = Peek<u8>(property + 0x7b);
    if (!property || !offset || *offset < 0x28 || *offset > 0x20000 || !byte || *byte > 8 ||
        !mask || !*mask || (*mask != 255 && (*mask & (*mask - 1)))) return {};
    const auto value = Peek<u8>(object + *offset + *byte);
    return value ? std::optional<bool>{(*value & *mask) != 0} : std::nullopt;
}

inline u64 Object(u64 base, u64 object, std::string_view name) {
    const auto at = Value(base, object, name, "ObjectProperty", 8);
    const auto value = at ? Peek<u64>(*at) : std::nullopt;
    return value && *value && Peek<u64>(*value) ? *value : 0;
}

inline std::optional<u8> Byte(u64 base, u64 object, std::string_view name) {
    const auto value = Value(base, object, name, "ByteProperty", 1);
    return value ? Peek<u8>(*value) : std::nullopt;
}

inline bool Fingerprinted(u64 base) {
    constexpr std::array<u8, 7> names_code{0x48,0x8b,0x1d,0x6f,0x57,0x0e,0x04};
    constexpr std::array<u8, 7> bool_code{0x48,0x63,0x41,0x50,0x8a,0x51,0x7b};
    return Memory::Instance()->IsValidMapping(base + 0x4573fa, 7) &&
        Memory::Instance()->IsValidMapping(base + 0x1a20d56, 7) &&
        !std::memcmp(reinterpret_cast<void*>(base + 0x4573fa), names_code.data(), 7) &&
        !std::memcmp(reinterpret_cast<void*>(base + 0x1a20d56), bool_code.data(), 7);
}

inline void Observe(u64 base) {
    auto& sink = Common::Diagnostics::Session::Get();
    if (!sink.Enabled()) return;
    static auto last = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::milliseconds(500)) return;
    last = now;
    // Verified native FName::ToString and SetBoolPropertyByName accessors: names are
    // 16K blocks, UObject FName is +0x18, UProperty offset +0x50, bool byte/mask +79/+7b.
    constexpr std::array<u8, 7> names_code{0x48,0x8b,0x1d,0x6f,0x57,0x0e,0x04};
    constexpr std::array<u8, 7> bool_code{0x48,0x63,0x41,0x50,0x8a,0x51,0x7b};
    if (!Memory::Instance()->IsValidMapping(base + 0x4573fa, 7) ||
        !Memory::Instance()->IsValidMapping(base + 0x1a20d56, 7) ||
        std::memcmp(reinterpret_cast<void*>(base + 0x4573fa), names_code.data(), 7) ||
        std::memcmp(reinterpret_cast<void*>(base + 0x1a20d56), bool_code.data(), 7)) {
        sink.Record({{"type","htl_state"},{"verified",false},{"source","unsupported-build"}});
        return;
    }
    Common::Diagnostics::Json event{{"type","htl_state"},{"verified",true},
                                    {"source","fingerprinted-native-reflection"}};
    const auto world = Peek<u64>(base + 0x46cb708);
    event["world_ready"] = world && *world && Peek<float>(*world + 0x884).has_value();
    if (!event["world_ready"].get<bool>()) { sink.Record(std::move(event)); return; }
    event["scene"] = Name(base, *world);
    const auto count = Peek<s32>(*world + 0x168);
    const auto weak = Peek<u64>(*world + 0x160);
    u64 controller = 0;
    if (count && *count > 0 && *count <= 8 && weak && *weak) {
        const auto index = Peek<s32>(*weak);
        const auto serial = Peek<s32>(*weak + 4);
        const auto objects = Peek<u64>(base + 0x45c3d50 + 0x10);
        const auto total = Peek<s32>(base + 0x45c3d50 + 0x1c);
        if (index && *index >= 0 && serial && *serial && objects && *objects && total && *index < *total) {
            const auto item = *objects + static_cast<u64>(*index) * 16;
            const auto item_serial = Peek<s32>(item + 12);
            const auto flags = Peek<u8>(item + 11);
            if (item_serial == serial && flags && !(*flags & 0x30)) controller = Peek<u64>(item).value_or(0);
        }
    }
    const auto pawn = controller ? Peek<u64>(controller + 0x388).value_or(0) : 0;
    event["controller_list_count"] = count.value_or(-1);
    event["controller_class"] = Class(base, controller);
    event["pawn_class"] = Class(base, pawn);
    event["pawn_reflected_class"] = Class(base, Object(base, controller, "Pawn"));
    if (!pawn || Class(base, pawn) != "BP_DGPlayer_C" ||
        Object(base, controller, "Pawn") != pawn || Class(base, controller) != "BP_DGPlayerController_C") {
        event["player_ready"] = false;
        sink.Record(std::move(event)); return;
    }
    event["player_ready"] = true;
    const auto tutorial = Peek<u8>(pawn + 0xe68);
    if (tutorial && *tutorial <= 1) event["tutorial_finished"] = (*tutorial != 0);
    const auto put = [&](const char* key, std::optional<bool> value) {
        if (value) event[key] = *value;
    };
    put("ui_active", Bool(base, pawn, "UIActive"));
    put("comfort_turning", Bool(base, pawn, "Comforturning"));
    const auto periphery = Object(base, pawn, "PeripheryCircle");
    if (periphery) put("periphery_enabled", Bool(base, periphery, "bEnabled"));
    auto instance = Object(base,*world,"OwningGameInstance");
    if (Class(base,instance)!="BP_GameInstance_C") {
        const auto fallback = Peek<u64>(pawn+0x9f8).value_or(0);
        if (Class(base,fallback)=="BP_GameInstance_C") instance=fallback;
    }
    event["game_instance_class"] = Class(base,instance);
    const auto progress = Object(base, instance, "GameProgressData");
    if (Class(base, progress) == "BP_DGGameProgressData_C") {
        if (const auto value = Byte(base, progress, "GameProgress")) event["game_progress"] = *value;
        if (const auto value = Byte(base, progress, "ForceLoadedLevel")) event["forced_checkpoint"] = *value;
    }
    const auto manager = Object(base, instance, "CheckpointManager");
    event["checkpoint_manager_class"] = Class(base, manager);
    static bool described{};
    if (!described && Class(base, manager) == "BG_DGCheckpointManager_C") {
        Common::Diagnostics::Json fields = Common::Diagnostics::Json::array();
        auto cls = Peek<u64>(manager + 0x10);
        for (u32 depth = 0; cls && *cls && depth < 8; ++depth) {
            auto child = Peek<u64>(*cls + 0x38);
            for (u32 i = 0; child && *child && i < 128; ++i) {
                const auto kind = Class(base, *child);
                if (kind.ends_with("Property")) fields.push_back({{"name",Name(base,*child)},
                    {"type",kind},{"offset",Peek<s32>(*child + 0x50).value_or(-1)}});
                const auto next = Peek<u64>(*child + 0x28);
                if (next == child) break;
                child = next;
            }
            const auto parent = Peek<u64>(*cls + 0x30);
            if (parent == cls) break;
            cls = parent;
        }
        const auto vtable = Peek<u64>(manager).value_or(0);
        const auto loader = Peek<u64>(vtable + 0x228).value_or(0);
        sink.Record({{"type","htl_checkpoint_layout"},{"fields",std::move(fields)},
                     {"load_checkpoint_rva",loader >= base ? loader - base : 0}});
        described = true;
    }
    const auto data = Object(base, manager, "CheckpointData");
    event["checkpoint_data_class"] = Class(base, data);
    static bool described_data{};
    if (!described_data && data && !Class(base, data).empty()) {
        Common::Diagnostics::Json fields = Common::Diagnostics::Json::array();
        auto cls = Peek<u64>(data + 0x10);
        for (u32 depth = 0; cls && *cls && depth < 8; ++depth) {
            auto child = Peek<u64>(*cls + 0x38);
            for (u32 i = 0; child && *child && i < 128; ++i) {
                const auto kind = Class(base, *child);
                if (kind.ends_with("Property")) fields.push_back({{"name",Name(base,*child)},
                    {"type",kind},{"offset",Peek<s32>(*child + 0x50).value_or(-1)}});
                const auto next = Peek<u64>(*child + 0x28);
                if (next == child) break;
                child = next;
            }
            const auto parent = Peek<u64>(*cls + 0x30);
            if (parent == cls) break;
            cls = parent;
        }
        sink.Record({{"type","htl_checkpoint_layout"},{"class",Class(base,data)},
                     {"fields",std::move(fields)}});
        described_data = true;
    }
    const auto menu = Object(base, instance, "DisplayedGamePauseWidget");
    const auto menu_field = Value(base, instance, "DisplayedGamePauseWidget", "ObjectProperty", 8);
    event["menu_class"] = Class(base,menu);
    if (menu_field && Peek<u64>(*menu_field) == std::optional<u64>{0}) {
        event["menu_visible"] = false;
        event["pause_visible"] = false;
    }
    if (menu && Class(base, menu) == "GamePauseMenuWidget_BP_C") {
        const auto in_main = Bool(base, menu, "IsInMainMenuFlag");
        const auto activated = Bool(base, menu, "MainMenuActivated");
        const auto pause = Bool(base, menu, "PauseActivated");
        const auto visible = Value(base, menu, "Visibility", "ByteProperty", 1);
        const auto visibility = visible ? Peek<u8>(*visible) : std::nullopt;
        const bool shown = visibility && (*visibility == 0 || *visibility == 3 || *visibility == 4);
        if (in_main && activated && visibility) event["menu_visible"] = *in_main && *activated && shown;
        if (pause && in_main && visibility) event["pause_visible"] = *pause && !*in_main && shown;
        event["menu_options"] = Object(base, menu, "OptionsButtonTemplate") &&
                                Object(base, menu, "NewGameButtonTemplate") &&
                                Object(base, menu, "LoadCheckpointButtonTemplate");
    }
    const auto root = Object(base, pawn, "RootComponent");
    const auto location = root ? Value(base, root, "RelativeLocation", "StructProperty", 12) : std::nullopt;
    if (location) {
        auto x = Peek<float>(*location), y = Peek<float>(*location + 4), z = Peek<float>(*location + 8);
        if (x && y && z && std::isfinite(*x) && std::isfinite(*y) && std::isfinite(*z))
            event["player_position"] = {*x,*y,*z};
    }
    const auto rotation = Value(base, controller, "ControlRotation", "StructProperty", 12);
    if (rotation) {
        const auto yaw = Peek<float>(*rotation + 4);
        if (yaw && std::isfinite(*yaw)) event["player_yaw"] = *yaw;
    }
    sink.Record(std::move(event));
}
} // namespace Core::HtlObserver
