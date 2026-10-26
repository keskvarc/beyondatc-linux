#include "diagnostics.hpp"
#include "placement.hpp"

#include <cassert>
#include <csignal>
#include <fstream>
#include <iterator>
#include <sys/resource.h>

namespace fs = std::filesystem;

static std::string contents(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    assert(file);
    return {std::istreambuf_iterator<char>(file), {}};
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    auto root = fs::path(argv[1]) / ("diagnostics-test-" + std::to_string(getpid()));
    assert(fs::create_directory(root));
    batc::DiagnosticStatus status;
    assert(batc::status_lines(status)[0] == "Connection: plugin disabled");
    status.enabled = true;
    assert(batc::status_lines(status)[0] == "Connection: waiting for BeyondATC");
    status.connected = status.aircraft_loaded = status.tcas = true;
    status.heartbeat_age = 0.2; status.player_age = 0.025; status.models = 2027; status.aircraft = 4;
    assert(batc::status_lines(status)[0] == "Connection: connected; heartbeat 0.2 s ago");
    assert(batc::status_lines(status)[3] == "Ownship telemetry: 0.0 s ago");
    status.last_issue = "terrain\nprobe\tmissed";
    assert(batc::status_lines(status)[7] == "Last issue: terrain?probe?missed");

    batc::PlayerState player;
    player.coordinates = {33.64769955115317, -84.4382841009146, 310.761976508952};
    player.registration = "DO-NOT-RECORD";
    player.discrete = {118005, 121900, 122800, 120000, 2105, 2, 0, 0, 0, 0};
    player.switches[0] = 1;
    player.analog[3] = std::nanf("");
    std::vector<batc::TrafficSample> traffic;
    // KATL ground/cruise positions and the KAUS ASA352 airborne-marked rollout.
    for (auto [id, lat, lon, altitude, ground, flags] : std::array<std::array<double, 6>, 5>{{
        {8, 33.642990175582355, -84.42525817547777, 310.89599609375, 302.2900695800781, 1},
        {25, 33.63808116522605, -84.41908377714864, 310.89599609375, 297.6044921875, 1},
        {53, 33.63894083786963, -84.43719734993, 178.15122985839844, 308.3912353515625, 1},
        {28, 33.56377797127688, -84.27047718213653, 2274.873046875, 232.22918701171875, 0},
        {40, 30.193988565867574, -97.6778932822948, 33.43608474731445, 152.46011352539062, 0}
    }}) {
        batc::TrafficSample sample;
        sample.id = uint32_t(id); sample.mode_s = uint32_t(id) + 1000;
        sample.type = "A319"; sample.callsign = "TEST\"\\\n";
        sample.commanded.lat = lat; sample.commanded.lon = lon;
        sample.commanded.altitude = float(altitude); sample.commanded.flags = uint8_t(flags);
        sample.interpolated = sample.commanded;
        auto placed = batc::grounded_position(sample.interpolated, [ground](double, double, float) {
            return float(ground);
        }, float(ground), true);
        sample.rendered_base = {lat, lon, placed.altitude};
        sample.ground = float(ground);
        sample.model_offset = 1.25f;
        sample.packet_age = 0.05; sample.feedback_age = 0.1;
        traffic.push_back(sample);
        assert(sample.commanded.altitude == float(altitude));
        assert(sample.rendered_base[2] == (flags ? float(ground) : std::max(float(altitude), float(ground))));
    }
    batc::RecordingState recording;
    recording.active = true;
    auto sample = batc::sample_record(123.25, status, recording, player, traffic);
    assert(sample.find("DO-NOT-RECORD") == std::string::npos);
    assert(sample.find("\"heading_deg\":null") != std::string::npos);
    assert(sample.find("TEST\\\"\\\\\\u000a") != std::string::npos);
    auto fixture = fs::path(argv[1]) / "native-diagnostics.jsonl";
    {
        std::ofstream file(fixture);
        file << sample << batc::sample_record(124, {}, {}, std::nullopt, {})
             << batc::message_record(124.1, "\"quoted\"\n\t\\\x01\xff");
        assert(file);
    }
    {
        batc::DiagnosticRecorder recorder;
        assert(!recorder.status().active);
        assert(!recorder.submit(sample));
        assert(!fs::exists(root / "logs"));
        recorder.start(root / "logs");
        assert(recorder.status().active);
        assert(recorder.submit(sample));
        assert(!recorder.submit(std::string(batc::DiagnosticRecorder::record_limit + 1, 'x')));
        assert(recorder.status().dropped == 1);
        assert(recorder.submit(std::string(batc::DiagnosticRecorder::record_limit - 1, 'x') + '\n'));
        assert(recorder.submit(batc::message_record(125, "recording stopped")));
        recorder.stop();
        assert(!recorder.status().active && recorder.status().error.empty());
        auto old = contents(root / "logs/flight.jsonl");
        assert(old.compare(0, sample.size(), sample) == 0);
        assert(old.find("recording stopped") != std::string::npos);
        assert((fs::status(root / "logs/flight.jsonl").permissions() & fs::perms::all) ==
               (fs::perms::owner_read | fs::perms::owner_write));
        recorder.start(root / "logs");
        assert(recorder.status().dropped == 0);
        assert(recorder.submit(batc::message_record(126, "new recording")));
        recorder.stop();
        assert(contents(root / "logs/flight.jsonl").substr(0, old.size()) == old);
    }
    {
        batc::DiagnosticRecorder recorder;
        recorder.start(root / "burst");
        auto record = batc::message_record(127, "burst");
        constexpr size_t count = 50000;
        assert(count * record.size() < batc::RotatingDiagnosticLog::file_limit);
        size_t accepted = 0;
        for (size_t i = 0; i < count; ++i) accepted += recorder.submit(record);
        recorder.stop();
        assert(accepted > 0 && recorder.status().dropped + accepted == count);
        auto data = contents(root / "burst/flight.jsonl");
        assert(data.size() == accepted * record.size());
        assert(size_t(std::count(data.begin(), data.end(), '\n')) == accepted);
        assert(recorder.status().error.empty());
    }
    {
        batc::RotatingDiagnosticLog file(root / "rotation");
        std::string record(64 * 1024 - 1, 'x');
        record += '\n';
        for (size_t i = 0; i < batc::RotatingDiagnosticLog::file_limit / record.size(); ++i) file.append(record);
        assert(fs::file_size(root / "rotation/flight.jsonl") == batc::RotatingDiagnosticLog::file_limit);
        assert(!fs::exists(root / "rotation/flight.jsonl.1"));
        file.append(record);
        assert(fs::file_size(root / "rotation/flight.jsonl") == record.size());
        assert(fs::file_size(root / "rotation/flight.jsonl.1") == batc::RotatingDiagnosticLog::file_limit);
        for (size_t i = 0; i < 5 * batc::RotatingDiagnosticLog::file_limit / record.size(); ++i) file.append(record);
        file.close();
        size_t count = 0, bytes = 0;
        for (const auto& entry : fs::directory_iterator(root / "rotation")) {
            ++count; bytes += entry.file_size();
            assert(entry.file_size() <= batc::RotatingDiagnosticLog::file_limit);
        }
        assert(count == 4 && bytes <= 32 * 1024 * 1024);
    }
    {
        fs::create_directory(root / "invalid");
        fs::create_directory(root / "invalid/flight.jsonl");
        batc::DiagnosticRecorder recorder;
        bool failed = false;
        try { recorder.start(root / "invalid"); }
        catch (const std::runtime_error&) { failed = true; }
        assert(failed && !recorder.status().active && !recorder.status().error.empty());
        fs::create_symlink(fixture, root / "invalid/flight.jsonl.1");
        fs::remove(root / "invalid/flight.jsonl");
        failed = false;
        try { recorder.start(root / "invalid"); }
        catch (const std::runtime_error&) { failed = true; }
        assert(failed && !recorder.status().error.empty());
    }
    {
        struct rlimit original{};
        assert(getrlimit(RLIMIT_FSIZE, &original) == 0);
        auto limit = original; limit.rlim_cur = 512;
        auto old_signal = std::signal(SIGXFSZ, SIG_IGN);
        assert(setrlimit(RLIMIT_FSIZE, &limit) == 0);
        batc::DiagnosticRecorder recorder;
        recorder.start(root / "full");
        assert(recorder.submit(sample));
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (recorder.status().active && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        auto state = recorder.status();
        recorder.stop();
        assert(setrlimit(RLIMIT_FSIZE, &original) == 0);
        std::signal(SIGXFSZ, old_signal);
        assert(!state.active && state.error.find("diagnostic write failed") != std::string::npos);
        assert(fs::file_size(root / "full/flight.jsonl") <= 512);
    }
    fs::remove_all(root);
}
