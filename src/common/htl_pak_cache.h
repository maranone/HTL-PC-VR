// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Experimental read-through cache. Enabled only by a laboratory candidate, never by default.
// Only complete reads from immutable, read-only pak files are retained. A hit advances the
// original handle exactly as the underlying read would; failed seeks fall back to the loader.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Common::Diagnostics {
class PakReadCache {
public:
    explicit PakReadCache(std::uint64_t bytes) : limit(bytes) {}
    std::size_t Read(const std::filesystem::path& path, std::uint64_t offset,
                     std::size_t requested, void* destination,
                     const std::function<std::size_t()>& loader,
                     const std::function<bool(std::uint64_t)>& seek) {
        if (!limit || !requested || requested > 4 * 1024 * 1024) return loader();
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error || offset > size || requested > size - offset) return loader();
        const auto stamp = std::filesystem::last_write_time(path, error);
        if (error) return loader();
        const std::string key = path.string() + ":" + std::to_string(size) + ":" +
            std::to_string(stamp.time_since_epoch().count()) + ":" + std::to_string(offset) +
            ":" + std::to_string(requested);
        {
            std::lock_guard lock(mutex);
            if (auto found = entries.find(key); found != entries.end() && seek(offset + requested)) {
                std::memcpy(destination, found->second.data.data(), requested);
                order.splice(order.begin(), order, found->second.position);
                ++hits; hit_bytes += requested;
                return requested;
            }
        }
        ++misses;
        const auto result = loader();
        if (result != requested || requested > limit) return result;
        std::lock_guard lock(mutex);
        if (entries.contains(key)) return result;
        while (used + requested > limit && !order.empty()) {
            const auto found = entries.find(order.back());
            used -= found->second.data.size(); entries.erase(found); order.pop_back();
        }
        order.push_front(key);
        Entry entry{{}, order.begin()};
        const auto* data = static_cast<const std::uint8_t*>(destination);
        entry.data.assign(data, data + requested);
        used += requested; entries.emplace(key, std::move(entry));
        return result;
    }
    std::atomic<std::uint64_t> hits{}, misses{}, hit_bytes{};
    std::uint64_t Used() { std::lock_guard lock(mutex); return used; }
private:
    struct Entry { std::vector<std::uint8_t> data; std::list<std::string>::iterator position; };
    std::uint64_t limit{}, used{};
    std::mutex mutex;
    std::list<std::string> order;
    std::unordered_map<std::string, Entry> entries;
};
} // namespace Common::Diagnostics
