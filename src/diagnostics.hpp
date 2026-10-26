#pragma once

#include "wire.hpp"

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fcntl.h>
#include <iomanip>
#include <locale>
#include <memory>
#include <mutex>
#include <sstream>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace batc {
struct DiagnosticStatus {
    bool enabled = false, connected = false, aircraft_loaded = false, tcas = false, ptt = false;
    std::optional<double> heartbeat_age, player_age;
    size_t aircraft = 0, objects = 0, models = 0;
    uint64_t received = 0, sent = 0, rejected = 0;
    uint64_t placement_misses = 0, feedback_misses = 0, spawn_refusals = 0, loop_errors = 0;
    std::string last_issue;
};
struct RecordingState {
    bool active = false;
    uint64_t dropped = 0;
    std::string error;
};
inline std::string menu_text(const std::string& text) {
    std::string result;
    for (unsigned char c : text.substr(0, 85)) result += c >= 32 && c < 127 ? char(c) : '?';
    return result;
}
inline std::string seconds(double value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(1) << value;
    return out.str();
}
inline std::array<std::string, 8> status_lines(const DiagnosticStatus& s) {
    return {
        !s.enabled ? "Connection: plugin disabled" : !s.connected ? "Connection: waiting for BeyondATC" :
            "Connection: connected; heartbeat " + seconds(s.heartbeat_age.value()) + " s ago",
        "Traffic: " + std::to_string(s.aircraft) + "/63; TCAS: " + (s.tcas ? "owned" : "not owned"),
        "CSL models: " + std::to_string(s.models) + "; ground objects: " + std::to_string(s.objects),
        !s.aircraft_loaded ? "Ownship: waiting for aircraft" : !s.connected ? "Ownship: waiting for connection" :
            s.player_age ? "Ownship telemetry: " + seconds(*s.player_age) + " s ago" : "Ownship: no telemetry sent",
        std::string("Push to talk: ") + (s.ptt ? "pressed" : "released"),
        "UDP: RX " + std::to_string(s.received) + "; TX " + std::to_string(s.sent) +
            "; rejected " + std::to_string(s.rejected) + "; loop errors " + std::to_string(s.loop_errors),
        "Terrain misses: placement " + std::to_string(s.placement_misses) +
            "; feedback " + std::to_string(s.feedback_misses) + "; spawn refusals " + std::to_string(s.spawn_refusals),
        "Last issue: " + (s.last_issue.empty() ? "none" : menu_text(s.last_issue))
    };
}

inline void json_text(std::ostream& out, const std::string& value) {
    constexpr char hex[] = "0123456789abcdef";
    out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << char(c);
        else if (c < 32 || c >= 127) out << "\\u00" << hex[c >> 4] << hex[c & 15];
        else out << char(c);
    }
    out << '"';
}
inline void json_number(std::ostream& out, double value) {
    if (std::isfinite(value)) out << value;
    else out << "null";
}
template <typename T>
inline void json_optional(std::ostream& out, const std::optional<T>& value) {
    if (value) json_number(out, *value);
    else out << "null";
}
inline std::ostringstream record_start(const char* event, double time) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(17) << "{\"schema\":1,\"utc_ms\":"
        << std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()
        << ",\"monotonic_s\":" << time << ",\"event\":";
    json_text(out, event);
    return out;
}
inline std::string message_record(double time, const std::string& message) {
    auto out = record_start("message", time);
    out << ",\"message\":";
    json_text(out, message.substr(0, 512));
    return out.str() + "}\n";
}
inline void position_json(std::ostream& out, const Position& p) {
    out << "{\"lat\":"; json_number(out, p.lat);
    out << ",\"lon\":"; json_number(out, p.lon);
    out << ",\"altitude_msl_m\":"; json_number(out, p.altitude);
    out << ",\"heading_deg\":"; json_number(out, p.heading);
    out << ",\"pitch_deg\":"; json_number(out, p.pitch);
    out << ",\"roll_deg\":"; json_number(out, p.roll);
    out << ",\"speed_mps\":"; json_number(out, p.speed);
    out << ",\"vertical_speed_mps\":"; json_number(out, p.vertical_speed);
    out << ",\"on_ground\":" << bool(p.flags & 1) << '}';
}
struct TrafficSample {
    uint32_t id = 0, mode_s = 0;
    std::string type, callsign;
    Position commanded, interpolated;
    std::array<double, 3> rendered_base{};
    std::array<float, 3> rendered_attitude{};
    float model_offset = 0;
    std::optional<float> ground;
    double packet_age = 0;
    std::optional<double> feedback_age;
};
inline std::string sample_record(double time, const DiagnosticStatus& s, const RecordingState& recording,
                                const std::optional<PlayerState>& player, const std::vector<TrafficSample>& traffic) {
    auto out = record_start("sample", time);
    out << std::boolalpha << ",\"status\":{\"enabled\":" << s.enabled << ",\"connected\":" << s.connected
        << ",\"aircraft_loaded\":" << s.aircraft_loaded << ",\"tcas\":" << s.tcas << ",\"ptt\":" << s.ptt
        << ",\"heartbeat_age_s\":"; json_optional(out, s.heartbeat_age);
    out << ",\"player_age_s\":"; json_optional(out, s.player_age);
    out << ",\"aircraft\":" << s.aircraft << ",\"objects\":" << s.objects << ",\"models\":" << s.models
        << ",\"rx\":" << s.received << ",\"tx\":" << s.sent << ",\"rejected\":" << s.rejected
        << ",\"placement_misses\":" << s.placement_misses << ",\"feedback_misses\":" << s.feedback_misses
        << ",\"spawn_refusals\":" << s.spawn_refusals << ",\"loop_errors\":" << s.loop_errors
        << ",\"recording_dropped\":" << recording.dropped << ",\"last_issue\":";
    json_text(out, s.last_issue.substr(0, 512));
    out << "},\"player\":";
    if (player) {
        const auto& p = *player;
        out << "{\"lat\":"; json_number(out, p.coordinates[0]);
        out << ",\"lon\":"; json_number(out, p.coordinates[1]);
        out << ",\"altitude_msl_m\":"; json_number(out, p.coordinates[2]);
        out << ",\"heading_deg\":"; json_number(out, p.analog[3]);
        out << ",\"indicated_altitude_ft\":"; json_number(out, p.analog[1]);
        out << ",\"ias_knots\":"; json_number(out, p.analog[5]);
        out << ",\"groundspeed_mps\":"; json_number(out, p.analog[7]);
        out << ",\"on_ground\":" << bool(p.switches[0])
            << ",\"com1_frequency_hz_833\":" << p.discrete[0]
            << ",\"com2_frequency_hz_833\":" << p.discrete[2]
            << ",\"transponder_code\":" << p.discrete[4] << ",\"transponder_mode\":" << p.discrete[5] << '}';
    } else out << "null";
    out << ",\"traffic\":[";
    bool first = true;
    for (const auto& p : traffic) {
        if (!first) out << ',';
        first = false;
        out << "{\"id\":" << p.id << ",\"mode_s\":" << p.mode_s << ",\"type\":";
        json_text(out, p.type);
        out << ",\"callsign\":"; json_text(out, p.callsign);
        out << ",\"commanded\":"; position_json(out, p.commanded);
        out << ",\"interpolated\":"; position_json(out, p.interpolated);
        out << ",\"rendered_base\":{\"lat\":"; json_number(out, p.rendered_base[0]);
        out << ",\"lon\":"; json_number(out, p.rendered_base[1]);
        out << ",\"altitude_msl_m\":"; json_number(out, p.rendered_base[2]);
        out << ",\"heading_deg\":"; json_number(out, p.rendered_attitude[0]);
        out << ",\"pitch_deg\":"; json_number(out, p.rendered_attitude[1]);
        out << ",\"roll_deg\":"; json_number(out, p.rendered_attitude[2]);
        out << "},\"model_offset_m\":"; json_number(out, p.model_offset);
        out << ",\"ground_msl_m\":"; json_optional(out, p.ground);
        out << ",\"command_age_s\":"; json_number(out, p.packet_age);
        out << ",\"feedback_age_s\":"; json_optional(out, p.feedback_age);
        out << '}';
    }
    return out.str() + "]}\n";
}

class RotatingDiagnosticLog {
    std::filesystem::path directory;
    int fd = -1;
    size_t size = 0;
    std::filesystem::path path(unsigned generation = 0) const {
        return directory / ("flight.jsonl" + (generation ? "." + std::to_string(generation) : ""));
    }
    void open() {
        fd = ::open(path().c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) throw std::runtime_error("cannot open diagnostic log: " + std::string(strerror(errno)));
        struct stat info{};
        if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_nlink != 1 ||
            info.st_size < 0 || uint64_t(info.st_size) > file_limit || fchmod(fd, 0600)) {
            ::close(fd); fd = -1;
            throw std::runtime_error("diagnostic log must be a private regular file no larger than 8 MiB");
        }
        size = size_t(info.st_size);
    }
public:
    static constexpr size_t file_limit = 8 * 1024 * 1024;
    static constexpr unsigned file_count = 4;
    explicit RotatingDiagnosticLog(const std::filesystem::path& dir) : directory(dir) {
        std::filesystem::create_directories(directory);
        std::filesystem::permissions(directory, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
        for (unsigned i = 0; i < file_count; ++i) {
            auto status = std::filesystem::symlink_status(path(i));
            if (std::filesystem::exists(status) &&
                (!std::filesystem::is_regular_file(status) || std::filesystem::file_size(path(i)) > file_limit))
                throw std::runtime_error("unexpected diagnostic archive: " + path(i).string());
        }
        open();
    }
    RotatingDiagnosticLog(const RotatingDiagnosticLog&) = delete;
    RotatingDiagnosticLog& operator=(const RotatingDiagnosticLog&) = delete;
    ~RotatingDiagnosticLog() { if (fd >= 0) ::close(fd); }
    void close() {
        int previous = fd;
        fd = -1;
        if (previous >= 0 && ::close(previous))
            throw std::runtime_error("cannot close diagnostic log: " + std::string(strerror(errno)));
    }
    void append(const std::string& record) {
        if (record.size() > file_limit) throw std::runtime_error("diagnostic record exceeds file limit");
        if (size + record.size() > file_limit) {
            close();
            std::filesystem::remove(path(file_count - 1));
            for (unsigned i = file_count - 1; i > 0; --i)
                if (std::filesystem::exists(path(i - 1))) std::filesystem::rename(path(i - 1), path(i));
            open();
        }
        size_t written = 0;
        while (written < record.size()) {
            auto result = ::write(fd, record.data() + written, record.size() - written);
            if (result < 0 && errno == EINTR) continue;
            if (result <= 0) throw std::runtime_error("diagnostic write failed: " + std::string(strerror(errno)));
            written += size_t(result);
        }
        size += record.size();
    }
};

class DiagnosticRecorder {
    mutable std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::string> queue;
    std::thread writer;
    RecordingState state;
    bool stopping = false;
public:
    static constexpr size_t queue_limit = 32, record_limit = 128 * 1024;
    ~DiagnosticRecorder() { stop(); }
    RecordingState status() const {
        std::lock_guard<std::mutex> lock(mutex);
        return state;
    }
    void start(const std::filesystem::path& directory) {
        stop();
        {
            std::lock_guard<std::mutex> lock(mutex);
            state = {};
            stopping = false;
        }
        try {
            auto file = std::make_unique<RotatingDiagnosticLog>(directory);
            {
                std::lock_guard<std::mutex> lock(mutex);
                state.active = true;
            }
            writer = std::thread([this, file = std::move(file)] {
                try {
                    for (;;) {
                        std::deque<std::string> batch;
                        {
                            std::unique_lock<std::mutex> lock(mutex);
                            ready.wait(lock, [this] { return stopping || !queue.empty(); });
                            batch.swap(queue);
                            if (batch.empty() && stopping) break;
                        }
                        for (const auto& record : batch) file->append(record);
                    }
                    file->close();
                } catch (const std::exception& error) {
                    std::lock_guard<std::mutex> lock(mutex);
                    state.active = false;
                    state.error = error.what();
                    queue.clear();
                }
            });
        } catch (const std::exception& error) {
            std::lock_guard<std::mutex> lock(mutex);
            state.active = false;
            state.error = error.what();
            throw;
        }
    }
    bool submit(std::string record) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!state.active) return false;
        if (record.size() > record_limit || queue.size() >= queue_limit) {
            ++state.dropped;
            return false;
        }
        queue.push_back(std::move(record));
        ready.notify_one();
        return true;
    }
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            state.active = false;
            stopping = true;
        }
        ready.notify_one();
        if (writer.joinable()) writer.join();
    }
};
}
