// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Optional, bounded telemetry. No disk I/O or GPU calls are performed by log/audio producers.
#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>

namespace Common::Diagnostics {
using Json = nlohmann::json;
class Session {
public:
    static Session& Get() { static Session value; return value; }
    bool Enabled() const { return !directory.empty(); }
    double Seconds() const {
        return offset + std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }
    void Record(Json event) {
        if (!Enabled()) return;
        event["t"] = Seconds();
        event["frame"] = frame.load(std::memory_order_relaxed);
        Work([this, event = std::move(event)] { stream << event.dump() << '\n'; });
    }
    bool Work(std::function<void()> job) {
        if (!Enabled()) return false;
        std::lock_guard lock(mutex);
        if (jobs.size() >= 2048) { ++dropped; return false; }
        jobs.push_back(std::move(job));
        wake.notify_one();
        return true;
    }
    void Drain() {
        if (!Enabled()) return;
        while (capture_pending.load()) capture_pending.wait(true);
        std::unique_lock lock(mutex);
        drained.wait(lock, [&] { return jobs.empty() && !working; });
    }
    bool FrameDue(unsigned id) {
        frame.store(id, std::memory_order_relaxed);
        if (!Enabled()) return false;
        const double now = Seconds();
        const double current_rate = rate.load();
        if (current_rate <= 0 || !attachments.load() || now < next_frame) return false;
        next_frame = now + 1.0 / current_rate;
        return true;
    }
    bool TraceRequested() { return trace_requested.exchange(false); }
    bool LabEnabled() const { return lab_enabled; }
    Json Command() {
        std::lock_guard lock(command_mutex);
        if (commands.empty()) return {};
        Json command = std::move(commands.front()); commands.pop_front(); return command;
    }
    void RuntimeStatus(const Json& values) {
        if (!LabEnabled()) return;
        Json status = values;
        status["t"] = Seconds(); status["frame"] = frame.load();
        status["ready"] = lab_ready.load();
        Record({{"type", "resident"}, {"status", status}});
        Work([this, status = std::move(status)] {
            const auto target = directory / "runtime-status.json";
            { std::ofstream output(target.string() + ".tmp"); output << status.dump(); }
            std::filesystem::rename(target.string() + ".tmp", target);
        });
    }
    std::filesystem::path Incoming() const { return directory / "incoming"; }
    void Audio(std::vector<float> samples, unsigned channels, unsigned sample_rate,
               double begin, unsigned port) {
        if (!Enabled() || !attachments.load()) return;
        Work([this, samples = std::move(samples), channels, sample_rate, begin, port] {
            const auto name = "audio_" + std::to_string(port) + "_" + std::to_string(audio_serial++) + ".f32";
            const auto target = Incoming() / name;
            { std::ofstream out(target.string() + ".tmp", std::ios::binary);
              out.write(reinterpret_cast<const char*>(samples.data()), samples.size() * sizeof(float)); }
            std::filesystem::rename(target.string() + ".tmp", target);
            Json event{{"type", "audio"}, {"t", begin}, {"frame", frame.load()},
                       {"file", name}, {"channels", channels}, {"rate", sample_rate}, {"port", port}};
            stream << event.dump() << '\n';
        });
    }
    std::atomic<unsigned> frame{};
    std::atomic<bool> capture_pending{};
    std::atomic<unsigned long long> dropped{};
    std::atomic<bool> attachments{true};
    std::atomic<bool> pause_at_pad{};
    std::atomic<bool> resident_paused{};
    std::atomic<bool> lab_ready{};
private:
    Session() {
        const char* value = std::getenv("SHADPS4_DIAGNOSTICS_DIR");
        if (!value || !*value) return;
        directory = std::filesystem::path(value);
        lab_enabled = std::getenv("SHADPS4_HTL_LAB") != nullptr;
        if (const char* epoch = std::getenv("SHADPS4_DIAGNOSTICS_START_US")) {
            offset = (std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count()
                      - std::strtod(epoch, nullptr) / 1000000.0);
        }
        try {
            std::filesystem::create_directories(Incoming());
            stream.open(directory / "telemetry.jsonl", std::ios::app);
            if (!stream) { directory.clear(); return; }
            thread = std::jthread([this](std::stop_token stop) {
                for (;;) {
                    if (lab_enabled && Seconds() >= next_lab_control) {
                        next_lab_control = Seconds() + 0.1;
                        try {
                            std::ifstream control(directory / "runtime-command.json");
                            if (control) {
                                Json command = Json::parse(control);
                                const std::string id = command.value("id", std::string{});
                                if (command.contains("command") && command["command"].is_string() &&
                                    !id.empty() && id.size() <= 80 && id != last_command) {
                                    std::lock_guard lock(command_mutex);
                                    if (commands.size() < 16) commands.push_back(std::move(command));
                                    last_command = id;
                                }
                            }
                        } catch (...) { ++dropped; }
                    }
                    if (Seconds() >= next_control) {
                        next_control = Seconds() + 1.0;
                        try {
                            std::ifstream control(directory / "capture-control.json");
                            if (control) {
                                const Json values = Json::parse(control);
                                rate.store(std::clamp(values.value("rate", 4.0), 0.0, 4.0));
                                attachments.store(values.value("attachments", true));
                                const auto request = values.value("trace", 0ull);
                                if (request > last_trace) { trace_requested.store(true); last_trace = request; }
                            }
                        } catch (...) { ++dropped; }
                    }
                    std::function<void()> job;
                    {
                        std::unique_lock lock(mutex);
                        wake.wait_for(lock, std::chrono::milliseconds(200), [&] { return !jobs.empty() || stop.stop_requested(); });
                        if (jobs.empty()) {
                            stream.flush();
                            drained.notify_all();
                            if (stop.stop_requested()) break;
                            continue;
                        }
                        job = std::move(jobs.front()); jobs.pop_front(); working = true;
                    }
                    try {
                        job();
                        if (stream.tellp() > 32 * 1024 * 1024) {
                            stream.close();
                            std::error_code error;
                            for (int i = 3; i >= 1; --i) {
                                const auto old = directory / (i == 1 ? "telemetry.jsonl" : "telemetry.jsonl." + std::to_string(i - 1));
                                const auto target = directory / ("telemetry.jsonl." + std::to_string(i));
                                std::filesystem::remove(target, error);
                                std::filesystem::rename(old, target, error);
                            }
                            stream.open(directory / "telemetry.jsonl", std::ios::trunc);
                        }
                    } catch (...) { ++dropped; }
                    {
                        std::lock_guard lock(mutex);
                        working = false;
                        if (jobs.empty()) { stream.flush(); drained.notify_all(); }
                    }
                }
            });
        } catch (...) { directory.clear(); }
    }
    ~Session() {
        if (thread.joinable()) { thread.request_stop(); wake.notify_all(); thread.join(); }
    }
    std::filesystem::path directory;
    std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
    double offset{}, next_frame{}, next_control{};
    std::atomic<double> rate{4.0};
    unsigned long long last_trace{};
    unsigned long long audio_serial{};
    std::atomic<bool> trace_requested{};
    std::ofstream stream;
    std::mutex mutex;
    std::condition_variable wake, drained;
    std::deque<std::function<void()>> jobs;
    bool working{};
    bool lab_enabled{};
    double next_lab_control{};
    std::string last_command;
    std::mutex command_mutex;
    std::deque<Json> commands;
    std::jthread thread;
};
} // namespace Common::Diagnostics
