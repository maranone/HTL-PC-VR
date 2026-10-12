// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Crash evidence: the last guest file reads, dumped when the guest asks for an absurd
// allocation (a corrupted size read from a file is the usual cause). Always on; a record is a
// few words, kept under one short lock.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>

#include "common/logging/log.h"
#include "common/thread.h"

namespace Common::Diagnostics {

struct ReadRecord {
    std::uint64_t serial{};
    std::uint64_t sequence{};
    std::uint64_t offset{};
    std::uint64_t requested{};
    std::int64_t result{};
    std::uint64_t buffer{};
    std::array<std::uint8_t, 16> head{};
    char file[48]{};
    char thread[24]{};
};

inline std::atomic<std::uint64_t>& Sequence() {
    static std::atomic<std::uint64_t> sequence{0};
    return sequence;
}

// Lock-free record of where the video decoder writes frames: no copy, no lock, so recording does
// not change the decoder's timing (a locked record made the crash disappear).
class FrameWrites {
public:
    static FrameWrites& Get() {
        static FrameWrites writes;
        return writes;
    }
    void Record(std::uint64_t address, std::uint64_t size) {
        const auto slot = next.fetch_add(1, std::memory_order_relaxed) % entries.size();
        entries[slot] = {Sequence().fetch_add(1, std::memory_order_relaxed), address, size};
    }
    struct Entry {
        std::uint64_t serial, address, size;
    };
    std::array<Entry, 128> entries{};
    std::atomic<std::uint64_t> next{0};
};

class ReadRing {
public:
    static ReadRing& Get() {
        static ReadRing ring;
        return ring;
    }

    void Record(const std::string& file, std::uint64_t offset, std::uint64_t requested,
                std::int64_t result, const void* buffer) {
        ReadRecord record;
        record.offset = offset;
        record.requested = requested;
        record.result = result;
        record.buffer = reinterpret_cast<std::uint64_t>(buffer);
        if (result > 0) {
            std::memcpy(record.head.data(), buffer,
                        std::min<std::size_t>(record.head.size(), static_cast<std::size_t>(result)));
        }
        Copy(record.file, file);
        thread_local const std::string thread_name = Common::GetCurrentThreadName();
        Copy(record.thread, thread_name);
        std::lock_guard lock(mutex);
        record.serial = ++serial;
        record.sequence = Sequence().fetch_add(1, std::memory_order_relaxed);
        records[record.serial % records.size()] = record;
        // Game:Main floods the main ring; other readers (AsyncIOSystem parses pak entries from
        // buffers it filled long before) keep their reads whole, to be compared at a crash.
        if (result > 0 && thread_name != "Game:Main") {
            auto& kept = others[others_next++ % others.size()];
            kept.record = record;
            kept.size = static_cast<std::uint32_t>(
                std::min<std::size_t>(kept.data.size(), static_cast<std::size_t>(result)));
            std::memcpy(kept.data.data(), buffer, kept.size);
        }
    }

    // Logs every kept read, oldest first. Called at most a few times per process.
    // `peek` reads guest memory safely; with it, each buffer's current bytes are compared to
    // what the read left there, which shows memory overwritten after the read.
    void Dump(const char* reason, std::uint64_t value,
              const std::function<bool(std::uint64_t, void*, std::size_t)>& peek = {}) {
        static std::atomic<int> dumps{0};
        if (dumps++ >= 3) return;
        std::array<ReadRecord, 256> copy;
        std::uint64_t last;
        {
            std::lock_guard lock(mutex);
            copy = records;
            last = serial;
        }
        LOG_CRITICAL(Kernel_Fs, "HTL read ring: {} ({:#x}); last {} guest reads follow", reason,
                     value, std::min<std::uint64_t>(last, copy.size()));
        const std::uint64_t first = last > copy.size() ? last - copy.size() + 1 : 1;
        for (std::uint64_t serial_i = first; serial_i <= last; ++serial_i) {
            const auto& r = copy[serial_i % copy.size()];
            std::string head;
            for (const auto byte : r.head) head += fmt::format("{:02x}", byte);
            const auto kept = static_cast<std::size_t>(
                std::clamp<std::int64_t>(r.result, 0, static_cast<std::int64_t>(r.head.size())));
            std::array<std::uint8_t, 16> now{};
            if (peek && kept != 0 && peek(r.buffer, now.data(), kept) &&
                std::memcmp(now.data(), r.head.data(), kept) != 0) {
                head += " CHANGED now ";
                for (std::size_t i = 0; i < kept; ++i) head += fmt::format("{:02x}", now[i]);
            }
            LOG_CRITICAL(Kernel_Fs,
                         "  read #{} seq {} [{}] {} offset {:#x} requested {:#x} result {} buffer {:#x} "
                         "head {}",
                         r.serial, r.sequence, r.thread, r.file, r.offset, r.requested, r.result, r.buffer,
                         head);
        }
        if (peek) {
            std::lock_guard lock(mutex);
            const std::size_t count = std::min<std::size_t>(others_next, others.size());
            LOG_CRITICAL(Kernel_Fs, "HTL read ring: last {} reads of other threads", count);
            std::array<std::uint8_t, 4096> now;
            for (std::size_t n = 0; n < count; ++n) {
                const auto& kept = others[(others_next - count + n) % others.size()];
                const auto& r = kept.record;
                std::string state = "unreadable";
                if (peek(r.buffer, now.data(), kept.size)) {
                    std::size_t first = kept.size, last_diff = 0, changed = 0;
                    for (std::size_t i = 0; i < kept.size; ++i) {
                        if (now[i] != kept.data[i]) {
                            first = std::min(first, i);
                            last_diff = i;
                            ++changed;
                        }
                    }
                    if (changed == 0) {
                        state = "intact";
                    } else {
                        const std::size_t from = first & ~std::size_t{15};
                        const std::size_t to = std::min<std::size_t>(from + 48, kept.size);
                        std::string was, is;
                        for (std::size_t i = from; i < to; ++i) {
                            was += fmt::format("{:02x}", kept.data[i]);
                            is += fmt::format("{:02x}", now[i]);
                        }
                        state = fmt::format("CHANGED {} bytes in [{:#x},{:#x}] at {:#x}: was {} now {}",
                                            changed, first, last_diff, r.buffer + from, was, is);
                    }
                }
                LOG_CRITICAL(Kernel_Fs, "  other read seq {} [{}] {} offset {:#x} size {:#x} buffer {:#x}: {}",
                             r.sequence, r.thread, r.file, r.offset, kept.size, r.buffer, state);
            }
        }
        for (const auto& w : FrameWrites::Get().entries) {
            if (w.size != 0) {
                LOG_CRITICAL(Kernel_Fs, "  video frame write seq {} at {:#x} size {:#x}", w.serial,
                             w.address, w.size);
            }
        }
    }

private:
    template <std::size_t N>
    static void Copy(char (&out)[N], const std::string& in) {
        const auto n = std::min(N - 1, in.size());
        std::memcpy(out, in.data() + (in.size() - n), n);
        out[n] = 0;
    }

    struct Kept {
        ReadRecord record;
        std::uint32_t size{};
        std::array<std::uint8_t, 4096> data{};
    };

    std::mutex mutex;
    std::uint64_t serial{};
    std::array<ReadRecord, 256> records{};
    std::array<Kept, 64> others{};
    std::uint64_t others_next{};
};

} // namespace Common::Diagnostics
