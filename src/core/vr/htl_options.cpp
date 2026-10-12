// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unordered_map>

#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/types.h"
#include "core/vr/htl_options.h"
#include "core/vr/htl_unreal.h"

namespace Core::HtlOptions {

namespace {

using namespace Core::HtlUnreal;

// MenuWidgetTemplate_BP's Button4 and Button5: the Options page's two slots that the game leaves
// unused, and the lab's menu overlay turns into checkboxes.
constexpr std::array<u64, 2> MenuButtons{0x2d0, 0x2d8};
// ButtonWidgetTemplate_BP's CheckBoxWidget. The UCheckBox keeps CheckedState, and its SCheckBox
// (the shared pointer MyCheckbox) keeps a copy of its own in the IsCheckboxChecked attribute
// (the value, then whether it is set), made when it was built: what shows on screen. A click
// changes both; so must the emulator.
constexpr u64 ButtonCheckBox = 0x2a0;
constexpr u64 CheckBoxState = 0x159;
constexpr u64 CheckBoxSlate = 0x7f8;
constexpr u64 SlateChecked = 0x470;
// DGPlayer's StrafeScalar: the player's Blueprint sets 0.3 (SlowStrafe) or 1.0 (NormalStrafe).
constexpr u64 StrafeScalar = 0xb44;

enum Option { Hands, Vignette, Count };
constexpr std::array<const char*, Count> Keys{"controller_hands", "comfort_vignette"};
constexpr std::array<const char*, Count> Labels{"Controller Hands", "Comfort Vignette"};

bool IsHtl() {
    static const bool htl = Common::ElfInfo::Instance().GameSerial() == "CUSA05344";
    return htl;
}

class Options {
public:
    Options() {
        Load();
    }

    bool Get(Option option) const {
        return values[option].load(std::memory_order_relaxed);
    }

    void Start() {
        if (!IsHtl() || started.exchange(true)) {
            return;
        }
        std::thread{[this] { Run(); }}.detach();
    }

private:
    static std::filesystem::path File() {
        return Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "htl_options.ini";
    }

    void Load() {
        std::ifstream file{File()};
        std::string line;
        while (std::getline(file, line)) {
            const auto equals = line.find('=');
            for (int option = 0; option < Count && equals != std::string::npos; ++option) {
                if (line.compare(0, equals, Keys[option]) == 0 && equals + 1 < line.size()) {
                    values[option] = line[equals + 1] != '0';
                }
            }
        }
    }

    void Save() {
        std::ofstream file{File(), std::ios::trunc};
        for (int option = 0; option < Count; ++option) {
            file << Keys[option] << '=' << (Get(static_cast<Option>(option)) ? 1 : 0) << '\n';
        }
    }

    void Run() {
        using namespace std::chrono_literals;
        static const bool full_strafe = [] {
            const char* value = std::getenv("SHADPS4_HTL_FULL_STRAFE");
            return value == nullptr || *value != '0';
        }();
        LOG_INFO(Core_Vr, "Here They Lie options: controller hands {}, comfort vignette {}",
                 Get(Hands) ? "on" : "off", Get(Vignette) ? "on" : "off");
        auto next_search = std::chrono::steady_clock::now();
        while (true) {
            std::this_thread::sleep_for(100ms);
            const bool menu_found = Valid(menu, menu_class);
            const bool player_found = Valid(player, player_class);
            const auto now = std::chrono::steady_clock::now();
            if ((!menu_found || !player_found) && now >= next_search) {
                next_search = now + 1s;
                Search(!menu_found, !player_found);
            }
            if (Valid(menu, menu_class)) {
                SyncMenu();
            }
            if (full_strafe && Valid(player, player_class)) {
                Strafe();
            }
        }
    }

    static bool Valid(VAddr object, VAddr klass) {
        VAddr current;
        u32 flags;
        return object != 0 && Read(object + ObjectClass, current) && current == klass &&
               Read(object + ObjectFlags, flags) && (flags & ClassDefaultObject) == 0;
    }

    enum Kind { Other, MenuTemplate, Player };

    /// The pause menu's Options page and the player, through GUObjectArray. Each class's name is
    /// looked up once.
    void Search(bool want_menu, bool want_player) {
        ForEachObject([&](VAddr object, VAddr klass) {
            auto [kind, added] = kinds.try_emplace(klass, Other);
            if (added) {
                kind->second = ClassIs(object, "MenuWidgetTemplate_BP_C") ? MenuTemplate
                               : ClassIs(object, "BP_DGPlayer_C")        ? Player
                                                                         : Other;
            }
            VAddr tree, owner;
            if (want_menu && kind->second == MenuTemplate && ObjectIs(object, "OptionsMenu") &&
                Read(object + ObjectOuter, tree) && Read(tree + ObjectOuter, owner) &&
                ClassIs(owner, "GamePauseMenuWidget_BP_C")) {
                menu = object;
                menu_class = klass;
                slots.fill(0);
                want_menu = false;
            }
            if (want_player && kind->second == Player) {
                player = object;
                player_class = klass;
                strafe_logged = false;
                want_player = false;
            }
            return !want_menu && !want_player;
        });
    }

    /// A checkbox seen for the first time is shown as the option is; after that, a click on it
    /// sets the option.
    void SyncMenu() {
        for (int option = 0; option < Count; ++option) {
            VAddr button, checkbox;
            u8 state;
            if (!Read(menu + MenuButtons[option], button) ||
                !ClassIs(button, "ButtonWidgetTemplate_BP_C") ||
                !Read(button + ButtonCheckBox, checkbox) || !ClassIs(checkbox, "CheckBox") ||
                !Read(checkbox + CheckBoxState, state) || state > 1) {
                continue;
            }
            const bool value = Get(static_cast<Option>(option));
            if (checkbox != slots[option]) {
                slots[option] = checkbox;
                Show(checkbox, value);
                continue;
            }
            if ((state == 1) != value) {
                values[option] = state == 1;
                Save();
                LOG_INFO(Core_Vr, "Here They Lie options: {} {}", Labels[option],
                         state == 1 ? "on" : "off");
            }
        }
    }

    static void Show(VAddr checkbox, bool value) {
        const u8 state = value ? 1 : 0;
        Write(checkbox + CheckBoxState, state);
        VAddr slate;
        std::array<u8, 2> attribute;
        if (Read(checkbox + CheckBoxSlate, slate) && slate != 0 &&
            Read(slate + SlateChecked, attribute) && attribute[0] <= 1 && attribute[1] == 1) {
            Write(slate + SlateChecked, state);
        }
    }

    void Strafe() {
        float scalar;
        if (!Read(player + StrafeScalar, scalar) || !(scalar > 0.0f && scalar < 1.0f)) {
            return;
        }
        Write(player + StrafeScalar, 1.0f);
        if (!strafe_logged) {
            strafe_logged = true;
            LOG_INFO(Core_Vr, "Here They Lie: the player strafes at full speed ({} -> 1)", scalar);
        }
    }

    // The comfort vignette is off unless chosen: it drew a dark wedge in the view (2026-10).
    std::array<std::atomic_bool, Count> values{true, false};
    std::atomic_bool started{};

    std::unordered_map<VAddr, Kind> kinds;
    VAddr menu{};
    VAddr menu_class{};
    std::array<VAddr, Count> slots{};
    VAddr player{};
    VAddr player_class{};
    bool strafe_logged{};
};

Options& Instance() {
    static Options options;
    return options;
}

} // namespace

void Start() {
    Instance().Start();
}

bool ControllerHands() {
    return !IsHtl() || Instance().Get(Hands);
}

bool ComfortVignette() {
    return !IsHtl() || Instance().Get(Vignette);
}

} // namespace Core::HtlOptions
