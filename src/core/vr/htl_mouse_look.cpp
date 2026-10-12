// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/types.h"
#include "core/vr/htl_mouse_look.h"
#include "core/vr/htl_unreal.h"

namespace Core::HtlMouseLook {

namespace {

using namespace Core::HtlUnreal;

// AController::ControlRotation in BP_DGPlayerController_C: an FRotator (pitch, yaw, roll) in
// degrees, yaw growing to the right and, in this game's camera, pitch growing downwards. The game keeps the pitch in [-90, 90]
// (stored as 270..360 below the horizon).
constexpr u64 ControlRotation = 0x3b0;
constexpr float PitchLimit = 89.0f;
// BP_DGPlayer_C's FirstPersonCameraComponent, and a USceneComponent's RelativeLocation in it.
constexpr u64 PlayerCamera = 0x9e0;
constexpr u64 RelativeLocation = 0x160;
// How far a crouch lowers the view (cm), and how fast it gets there (cm a second).
constexpr float CrouchDepth = 60.0f;
// The body stays standing, so the lowered view also moves forward out of its chest (cm at a full
// crouch; the camera's parent faces where the player does, X forward).
constexpr float CrouchForward = 30.0f;
constexpr float CrouchSpeed = 300.0f;

class MouseLook {
public:
    void Add(float yaw, float pitch) {
        {
            std::scoped_lock lock{mutex};
            pending_yaw += yaw;
            pending_pitch += pitch;
        }
        StartOnce();
    }

    void Crouch(bool held) {
        crouch_held = held;
        LOG_INFO(Core_Vr, "Here They Lie 2D: crouch {}", held ? "held" : "let go");
        StartOnce();
    }

private:
    void StartOnce() {
        if (!started.exchange(true)) {
            std::thread{[this] { Run(); }}.detach();
        }
    }

    void Run() {
        using namespace std::chrono_literals;
        LOG_INFO(Core_Vr, "Here They Lie 2D: the mouse turns the camera directly");
        auto next_search = std::chrono::steady_clock::now();
        auto last = next_search;
        while (true) {
            // Often enough that a turn lands in the frame it was made for.
            std::this_thread::sleep_for(1ms);
            const auto tick = std::chrono::steady_clock::now();
            const float seconds = std::chrono::duration<float>(tick - last).count();
            last = tick;
            if ((!Valid(controller, controller_class) || !Valid(player, player_class)) &&
                tick >= next_search) {
                next_search = tick + 1s;
                Search();
            }
            Lower(seconds);
            if (!Valid(controller, controller_class)) {
                Take(); // Movement while there is no player is dropped.
                continue;
            }
            const auto [yaw, pitch] = Take();
            if (yaw == 0.0f && pitch == 0.0f) {
                continue;
            }
            std::array<float, 3> rotation;
            if (!Read(controller + ControlRotation, rotation) || !std::isfinite(rotation[0]) ||
                !std::isfinite(rotation[1])) {
                continue;
            }
            float current_pitch = std::remainder(rotation[0], 360.0f);
            current_pitch = std::clamp(current_pitch + pitch, -PitchLimit, PitchLimit);
            rotation[0] = current_pitch < 0.0f ? current_pitch + 360.0f : current_pitch;
            float new_yaw = std::fmod(rotation[1] + yaw, 360.0f);
            rotation[1] = new_yaw < 0.0f ? new_yaw + 360.0f : new_yaw;
            Write(controller + ControlRotation, rotation);
        }
    }

    std::pair<float, float> Take() {
        std::scoped_lock lock{mutex};
        const std::pair<float, float> taken{pending_yaw, pending_pitch};
        pending_yaw = pending_pitch = 0.0f;
        return taken;
    }

    /// Eases the view towards standing or crouching. It only touches the camera while it is
    /// lowered, or coming back up, so the game's own placement of it is otherwise left alone.
    void Lower(float seconds) {
        const float target = crouch_held ? CrouchDepth : 0.0f;
        if (depth == 0.0f && target == 0.0f) {
            return;
        }
        const float step = CrouchSpeed * std::min(seconds, 0.1f);
        depth = depth < target ? std::min(depth + step, target) : std::max(depth - step, target);
        VAddr camera;
        std::array<float, 3> location;
        if (!Valid(player, player_class) || !Read(player + PlayerCamera, camera) || camera == 0 ||
            !Read(camera + RelativeLocation, location) || !std::isfinite(location[2])) {
            depth = 0.0f;
            return;
        }
        location[0] = CrouchForward * depth / CrouchDepth;
        location[2] = -depth;
        Write(camera + RelativeLocation, location);
    }

    static bool Valid(VAddr object, VAddr klass) {
        VAddr current;
        u32 flags;
        return object != 0 && Read(object + ObjectClass, current) && current == klass &&
               Read(object + ObjectFlags, flags) && (flags & ClassDefaultObject) == 0;
    }

    void Search() {
        bool found_controller = Valid(controller, controller_class);
        bool found_player = Valid(player, player_class);
        ForEachObject([&](VAddr object, VAddr klass) {
            auto [known, added] = kinds.try_emplace(klass, Other);
            if (added) {
                known->second = ClassIs(object, "BP_DGPlayerController_C") ? Controller
                                : ClassIs(object, "BP_DGPlayer_C")         ? Player
                                                                           : Other;
            }
            if (!found_controller && known->second == Controller) {
                controller = object;
                controller_class = klass;
                found_controller = true;
                LOG_INFO(Core_Vr, "Here They Lie 2D: player controller at {:#x}", object);
            }
            if (!found_player && known->second == Player) {
                player = object;
                player_class = klass;
                found_player = true;
                LOG_INFO(Core_Vr, "Here They Lie 2D: player at {:#x}", object);
            }
            return found_controller && found_player;
        });
    }

    std::mutex mutex;
    float pending_yaw{};
    float pending_pitch{};
    std::atomic_bool started{};
    VAddr controller{};
    VAddr controller_class{};
    VAddr player{};
    VAddr player_class{};
    enum Kind { Other, Controller, Player };
    std::unordered_map<VAddr, Kind> kinds;
    std::atomic_bool crouch_held{};
    float depth{};
};

} // namespace

MouseLook* Look() {
    static const bool htl = Common::ElfInfo::Instance().GameSerial() == "CUSA05344";
    static MouseLook look;
    return htl ? &look : nullptr;
}

void Add(float yaw_degrees, float pitch_degrees) {
    if (auto* look = Look()) {
        look->Add(yaw_degrees, pitch_degrees);
    }
}

void Crouch(bool held) {
    if (auto* look = Look()) {
        look->Crouch(held);
    }
}

} // namespace Core::HtlMouseLook
