// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/types.h"
#include "core/vr/htl_hands.h"
#include "core/vr/htl_unreal.h"

namespace Core::HtlHands {

namespace {

using namespace Core::HtlUnreal;

// Components keep their world transform at +0x110 (rotation) and +0x120 (translation).
constexpr u64 ComponentRotation = 0x110;
constexpr u64 ComponentTranslation = 0x120;
// BP_DGPlayer's FirstPersonCameraComponent.
constexpr u64 PlayerCamera = 0x9e0;

// In Player_A_AnimBP: a HIKReachRotator on the right wrist (effector 4) that only nudges it,
// and the HIKSolve that solves it. Each node starts with its exposed-input handler: copy records
// (array at +0x10, count at +0x18) and a bound function (+0x20) that refresh its inputs.
constexpr u64 ReachNode = 0x1d30;
constexpr u64 SolveNode = 0x1cb0;
constexpr u64 ReachSize = 0xb0;
constexpr u64 SolveSize = 0x80;
constexpr u64 HandlerCount = 0x18;
constexpr u64 HandlerFunction = 0x20;
constexpr u64 ReachAlpha = 0x48;       // Alpha, ReachT, ReachR
constexpr u64 ReachTranslation = 0x54; // FVector
constexpr u64 ReachOrientation = 0x60; // FRotator
constexpr u64 ReachEffector = 0x6c;    // then EffectorSpace, BoneTranslationMode
constexpr u64 ReachOrientationMode = 0x8a;
constexpr u64 SolveAlpha = 0x4c;
constexpr u8 RightWrist = 4;
constexpr u8 CharacterSpace = 1; // the mesh's own space
constexpr u8 Replace = 1;

// The torch hand's HIKReachRotator (left wrist, effector 3), already in the mesh's space and
// replacing. The graph only evaluates its branch while DGPlayerAnimInstance's bTorchIsOn is set,
// which the game copies from DGPlayer's own when that changes: with the torch off, the arm hangs.
constexpr u64 TorchNode = 0xa18;
constexpr u8 LeftWrist = 3;
constexpr u64 AnimTorchOn = 0x601;
constexpr u64 PlayerTorchOn = 0xec8;

struct Vec {
    float x, y, z;
};
struct Quat {
    float x, y, z, w;
};

// The node's Orientation turns the wrist from this pose, in the mesh's space (measured: with no
// Orientation, the right wrist bone ends up turned so, wherever the hand reaches).
constexpr Quat WristAtRest{-0.92842174f, 0.16872270f, 0.32567304f, 0.05918489f};

Vec Rotate(const Quat& q, const Vec& v) {
    const Vec t{2.0f * (q.y * v.z - q.z * v.y), 2.0f * (q.z * v.x - q.x * v.z),
                2.0f * (q.x * v.y - q.y * v.x)};
    return {v.x + q.w * t.x + (q.y * t.z - q.z * t.y), v.y + q.w * t.y + (q.z * t.x - q.x * t.z),
            v.z + q.w * t.z + (q.x * t.y - q.y * t.x)};
}

Quat Multiply(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

Quat Conjugate(const Quat& q) {
    return {-q.x, -q.y, -q.z, q.w};
}

/// The rotation that takes the unit axes to `x`, `y` and `z` (orthonormal).
Quat FromAxes(const Vec& x, const Vec& y, const Vec& z) {
    const float trace = x.x + y.y + z.z;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        return {(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, 0.25f * s};
    }
    if (x.x > y.y && x.x > z.z) {
        const float s = std::sqrt(1.0f + x.x - y.y - z.z) * 2.0f;
        return {0.25f * s, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s};
    }
    if (y.y > z.z) {
        const float s = std::sqrt(1.0f + y.y - x.x - z.z) * 2.0f;
        return {(y.x + x.y) / s, 0.25f * s, (z.y + y.z) / s, (z.x - x.z) / s};
    }
    const float s = std::sqrt(1.0f + z.z - x.x - y.y) * 2.0f;
    return {(z.x + x.z) / s, (z.y + y.z) / s, 0.25f * s, (x.y - y.x) / s};
}

/// Unreal's FRotator (pitch, yaw, roll; degrees) for a rotation, as FQuat::Rotator makes it.
std::array<float, 3> ToRotator(const Quat& q) {
    constexpr float Degrees = 57.295779513f;
    const float singularity = q.z * q.x - q.w * q.y;
    const float yaw_y = 2.0f * (q.w * q.z + q.x * q.y);
    const float yaw_x = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    const float yaw = std::atan2(yaw_y, yaw_x) * Degrees;
    if (std::abs(singularity) > 0.4999995f) {
        const float sign = singularity > 0.0f ? 1.0f : -1.0f;
        const float roll = std::remainder(sign * yaw - 2.0f * std::atan2(q.x, q.w) * Degrees, 360.0f);
        return {sign * 90.0f, yaw, roll};
    }
    return {std::asin(2.0f * singularity) * Degrees, yaw,
            std::atan2(-2.0f * (q.w * q.x + q.y * q.z), 1.0f - 2.0f * (q.x * q.x + q.y * q.y)) *
                Degrees};
}

/// From the head's axes (x right, y up, -z forward) to Unreal's (x forward, y right, z up).
Vec FromHeadAxes(const Vec& v) {
    return {-v.z, v.x, v.y};
}

class Driver {
public:
    void Set(const Vec& from_head, const Quat& turn) {
        {
            std::scoped_lock lock{mutex};
            target = from_head;
            target_turn = turn;
            active = true;
        }
        Start();
    }

    void Clear() {
        std::scoped_lock lock{mutex};
        active = false;
    }

    void SetLeft(const Vec& from_head) {
        {
            std::scoped_lock lock{mutex};
            left_target = from_head;
            left_active = true;
        }
        Start();
    }

    void ClearLeft() {
        std::scoped_lock lock{mutex};
        left_active = false;
    }

private:
    void Start() {
        if (started.exchange(true)) {
            return;
        }
        if (Common::ElfInfo::Instance().GameSerial() != "CUSA05344") {
            return;
        }
        std::thread{[this] { Run(); }}.detach();
    }

    void Run() {
        using namespace std::chrono_literals;
        auto next_search = std::chrono::steady_clock::now();
        while (true) {
            std::this_thread::sleep_for(4ms);
            Vec from_head, left_from_head;
            Quat turn;
            bool right, left;
            {
                std::scoped_lock lock{mutex};
                from_head = target;
                turn = target_turn;
                right = active;
                left_from_head = left_target;
                left = left_active;
            }
            if (!right) {
                Release();
            }
            if (!left) {
                ReleaseLeft();
            }
            if (!right && !left) {
                continue;
            }
            if (!Valid()) {
                Release();
                ReleaseLeft();
                instance = 0;
                const auto now = std::chrono::steady_clock::now();
                if (now < next_search) {
                    continue;
                }
                next_search = now + 1s;
                if (!Search()) {
                    continue;
                }
            }
            Drive(right ? &from_head : nullptr, turn, left ? &left_from_head : nullptr);
        }
    }

    /// The live Player_A_AnimBP instance (not its class default), through GUObjectArray.
    bool Search() {
        bool adopted = false;
        ForEachObject([&](VAddr object, VAddr klass) {
            u32 class_name;
            if (klass != player_class &&
                (player_class != 0 || !Read(klass + ObjectName, class_name) ||
                 !NameIs(class_name, "Player_A_AnimBP_C"))) {
                return false;
            }
            player_class = klass;
            adopted = Adopt(object);
            return true;
        });
        return adopted;
    }

    bool Adopt(VAddr object) {
        VAddr mesh, actor, camera;
        u8 effector;
        if (!Read(object + ObjectOuter, mesh) || !Read(mesh + ObjectOuter, actor) ||
            !Read(actor + PlayerCamera, camera) || !ClassIs(camera, "DGCameraComponent") ||
            !Read(object + ReachNode + ReachEffector, effector) || effector != RightWrist) {
            return false;
        }
        u8 torch_effector;
        torch_hand = Read(object + TorchNode + ReachEffector, torch_effector) &&
                     torch_effector == LeftWrist;
        instance = object;
        mesh_component = mesh;
        camera_component = camera;
        player = actor;
        LOG_INFO(Core_Vr, "Here They Lie: the right hand follows the controller (animation {:#x})",
                 instance);
        return true;
    }

    bool Valid() {
        VAddr klass;
        u32 flags;
        return instance != 0 && Read(instance + ObjectClass, klass) && klass == player_class &&
               Read(instance + ObjectFlags, flags) && (flags & ClassDefaultObject) == 0;
    }

    /// The hands where the controllers are (the right one, or the left one, or both: null for a
    /// hand left to the game).
    void Drive(const Vec* right, const Quat& turn, const Vec* left) {
        Quat camera_rotation, mesh_rotation;
        Vec camera_at, mesh_at;
        if (!Read(camera_component + ComponentRotation, camera_rotation) ||
            !Read(camera_component + ComponentTranslation, camera_at) ||
            !Read(mesh_component + ComponentRotation, mesh_rotation) ||
            !Read(mesh_component + ComponentTranslation, mesh_at)) {
            return;
        }
        // From the head's axes (metres) to Unreal's (centimetres), then from the camera to the
        // world, then into the mesh.
        const Quat to_mesh = Conjugate(mesh_rotation);
        const auto into_mesh = [&](const Vec& v) {
            return Rotate(to_mesh, Rotate(camera_rotation, FromHeadAxes(v)));
        };
        const Vec offset = Rotate(to_mesh, {camera_at.x - mesh_at.x, camera_at.y - mesh_at.y,
                                            camera_at.z - mesh_at.z});
        const auto point = [&](const Vec& from_head) {
            const Vec local = into_mesh({from_head.x * 100.0f, from_head.y * 100.0f,
                                         from_head.z * 100.0f});
            return Vec{offset.x + local.x, offset.y + local.y, offset.z + local.z};
        };
        if (left != nullptr) {
            DriveLeft(point(*left));
        }
        if (right == nullptr) {
            return;
        }
        const Vec reach_to = point(*right);

        // The wrist as if it held the controller: the fingers along the grip's -z, the thumb
        // along its y and the back of the hand along its x (the right hand's grip axes). The
        // right wrist bone has the fingers along its -x, the back of the hand along its y and
        // the thumb along its -z.
        const Vec forward = into_mesh(Rotate(turn, {0.0f, 0.0f, -1.0f}));
        const Vec up = into_mesh(Rotate(turn, {0.0f, 1.0f, 0.0f}));
        const Vec back = into_mesh(Rotate(turn, {1.0f, 0.0f, 0.0f}));
        const Quat wrist = FromAxes({-forward.x, -forward.y, -forward.z}, back,
                                    {-up.x, -up.y, -up.z});
        const auto orientation = ToRotator(Multiply(wrist, Conjugate(WristAtRest)));
        if (!std::isfinite(reach_to.x) || !std::isfinite(reach_to.y) || !std::isfinite(reach_to.z) ||
            !std::isfinite(orientation[0]) || !std::isfinite(orientation[1]) ||
            !std::isfinite(orientation[2])) {
            return;
        }

        const VAddr reach = instance + ReachNode;
        const VAddr solve = instance + SolveNode;
        if (driven != instance) {
            Release();
            Read(reach, saved_reach.data(), saved_reach.size());
            Read(solve, saved_solve.data(), saved_solve.size());
            driven = instance;
            // The game refreshes both nodes' inputs from its own variables every frame: no more.
            Write(reach + HandlerCount, u32{0});
            Write(reach + HandlerFunction, VAddr{0});
            Write(solve + HandlerCount, u32{0});
        }
        const std::array<float, 3> alpha{1.0f, 1.0f, 1.0f}; // reach to the point, and turn
        Write(reach + ReachAlpha, alpha.data(), sizeof(alpha));
        Write(reach + ReachTranslation, reach_to);
        Write(reach + ReachOrientation, orientation.data(), sizeof(orientation));
        const std::array<u8, 2> space{CharacterSpace, Replace};
        Write(reach + ReachEffector + 1, space.data(), space.size());
        Write(reach + ReachOrientationMode, u8{0});
        Write(solve + SolveAlpha, 1.0f);
    }

    /// The torch hand: while the torch is lit, its node reaches for the point (the torch still
    /// aims by the pad's tilt); unlit, the arm hangs as the game has it. With
    /// SHADPS4_HTL_LEFT_HAND_UNLIT=1 the graph keeps the torch branch unlit too, and the empty
    /// hand follows the controller as well.
    void DriveLeft(const Vec& reach_to) {
        static const bool unlit = [] {
            const char* value = std::getenv("SHADPS4_HTL_LEFT_HAND_UNLIT");
            return value != nullptr && *value == '1';
        }();
        u8 lit;
        if (!torch_hand || !std::isfinite(reach_to.x) || !std::isfinite(reach_to.y) ||
            !std::isfinite(reach_to.z)) {
            return;
        }
        if (!unlit && (!Read(player + PlayerTorchOn, lit) || (lit & 1) == 0)) {
            ReleaseLeft();
            return;
        }
        const VAddr reach = instance + TorchNode;
        if (left_driven != instance) {
            ReleaseLeft();
            Read(reach, saved_torch.data(), saved_torch.size());
            left_driven = instance;
            Write(reach + HandlerCount, u32{0});
            Write(reach + HandlerFunction, VAddr{0});
        }
        Write(reach + ReachTranslation, reach_to);
        u8 torch_on;
        if (Read(instance + AnimTorchOn, torch_on) && (torch_on & 1) == 0) {
            Write(instance + AnimTorchOn, static_cast<u8>(torch_on | 1));
        }
    }

    /// Gives the torch hand back, its branch again following the torch.
    void ReleaseLeft() {
        if (left_driven == 0) {
            return;
        }
        u8 player_on, anim_on;
        if (Valid() && left_driven == instance) {
            Write(left_driven + TorchNode, saved_torch.data(), saved_torch.size());
            if (Read(player + PlayerTorchOn, player_on) && Read(instance + AnimTorchOn, anim_on)) {
                Write(instance + AnimTorchOn, static_cast<u8>((anim_on & ~1) | (player_on & 1)));
            }
        }
        left_driven = 0;
    }

    /// Gives the nodes back to the game as they were.
    void Release() {
        if (driven == 0) {
            return;
        }
        if (Valid() && driven == instance) {
            Write(driven + ReachNode, saved_reach.data(), saved_reach.size());
            Write(driven + SolveNode, saved_solve.data(), saved_solve.size());
        }
        driven = 0;
    }

    std::mutex mutex;
    Vec target{};
    Quat target_turn{0.0f, 0.0f, 0.0f, 1.0f};
    bool active{};
    Vec left_target{};
    bool left_active{};
    std::atomic_bool started{};

    VAddr player_class{};
    VAddr instance{};
    VAddr mesh_component{};
    VAddr camera_component{};
    VAddr player{};
    bool torch_hand{};
    VAddr driven{};
    VAddr left_driven{};
    std::array<u8, ReachSize> saved_torch{};
    std::array<u8, ReachSize> saved_reach{};
    std::array<u8, SolveSize> saved_solve{};
};

Driver& Instance() {
    static Driver driver;
    return driver;
}

} // namespace

void SetRightHand(const float (&position)[3], const float (&orientation)[4]) {
    Instance().Set({position[0], position[1], position[2]},
                   {orientation[0], orientation[1], orientation[2], orientation[3]});
}

void ClearRightHand() {
    Instance().Clear();
}

void SetLeftHand(const float (&position)[3]) {
    Instance().SetLeft({position[0], position[1], position[2]});
}

void ClearLeftHand() {
    Instance().ClearLeft();
}

void PollTestHand() {
    static const bool test = [] {
        const char* value = std::getenv("SHADPS4_HTL_TEST_HAND");
        if (value == nullptr || *value == '\0') {
            return false;
        }
        // x, y, z, then pitch, yaw and roll (degrees about the head's x, y and z).
        std::array<float, 6> values{};
        char* end = const_cast<char*>(value) - 1;
        for (size_t i = 0; i < values.size() && (i == 0 || *end == ','); ++i) {
            values[i] = std::strtof(end + 1, &end);
        }
        const auto about = [](float x, float y, float z, float degrees) {
            const float half = degrees * 0.00872664626f;
            const float s = std::sin(half);
            return Quat{x * s, y * s, z * s, std::cos(half)};
        };
        const Quat turn = Multiply(about(0, 1, 0, values[4]),
                                   Multiply(about(1, 0, 0, values[3]), about(0, 0, 1, values[5])));
        SetRightHand({values[0], values[1], values[2]}, {turn.x, turn.y, turn.z, turn.w});
        return true;
    }();
    static const bool test_left = [] {
        const char* value = std::getenv("SHADPS4_HTL_TEST_LEFT_HAND");
        if (value == nullptr || *value == '\0') {
            return false;
        }
        std::array<float, 3> values{};
        char* end = const_cast<char*>(value) - 1;
        for (size_t i = 0; i < values.size() && (i == 0 || *end == ','); ++i) {
            values[i] = std::strtof(end + 1, &end);
        }
        SetLeftHand({values[0], values[1], values[2]});
        return true;
    }();
    (void)test;
    (void)test_left;
}

} // namespace Core::HtlHands
