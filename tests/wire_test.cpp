#include "wire.hpp"
#include "placement.hpp"

#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>

static batc::Bytes load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot read fixture " + path);
    return {std::istreambuf_iterator<char>(f), {}};
}
static void save(const std::string& path, const batc::Bytes& packet) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(packet.data()), packet.size());
    if (!f) throw std::runtime_error("cannot write fixture " + path);
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::string root = argv[1];
    for (const std::string name : {"BuildSpawn", "BuildUpdate", "BuildObjectUpdate", "BuildObjectRemove",
        "BuildRemove", "BuildRemoveAll", "BuildLights", "BuildGear", "BuildCallsign", "BuildSsrMode",
        "BuildHeartbeat", "BuildSetDataref", "BuildSetPlayerDatarefs", "BuildSetDatarefInt", "BuildSetDatarefFloat"}) {
        auto b = load(root + "/" + name + ".bin");
        batc::Reader r(b);
        assert(batc::u32(b.data() + 8) == 42);
        for (size_t i = 0; i < b.size(); ++i) {
            auto truncated = b;
            truncated.resize(i);
            bool rejected = false;
            try { batc::Reader invalid(truncated); }
            catch (const std::runtime_error&) { rejected = true; }
            assert(rejected);
        }
    }
    auto s = batc::spawn(load(root + "/BuildSpawn.bin"));
    assert(s.id == 7 && s.type == "BB_A320" && s.airline == "BB_" && s.model == "BB_A320_AFR");
    assert(std::abs(s.position.lat - 35.877639) < 1e-9);
    assert(std::abs(s.position.lon + 78.787472) < 1e-9);
    assert(s.position.altitude == 435.25f && s.position.flags == 1);
    auto packet = load(root + "/BuildUpdate.bin");
    batc::Reader r(packet);
    assert(r.integer() == 7);
    auto p = batc::update(r);
    assert(p.flags == 3 && p.n1 == 7 && p.altitude == 435.25f);
    assert(p.heading == 237.5f && p.pitch == 0.5f && p.roll == 0.5f);
    auto object = load(root + "/BuildObjectUpdate.bin");
    batc::Reader object_packet(object);
    assert(object_packet.integer() == 7);
    assert(std::abs(object_packet.coordinate() - 35.877639) < 1e-9);
    assert(std::abs(object_packet.coordinate() + 78.787472) < 1e-9);
    assert(object_packet.real() == 90 && object_packet.byte() == 0);
    assert(object_packet.text(64) == "lib/airport/vehicles/test.obj");
    object_packet.end();
    // KATL traffic capture includes aircraft both above and below the scenery.
    for (auto [commanded, ground] : std::array<std::array<float, 2>, 8>{{
        {310.895996094f, 302.290069580f}, {310.895996094f, 297.604492188f},
        {310.895996094f, 303.885894775f}, {310.895996094f, 309.341796875f},
        {190.669754028f, 297.800537109f}, {178.151229858f, 308.279876709f},
        {184.071487427f, 308.681121826f}, {5.0f, 0.0f}}}) {
        auto captured = p;
        captured.altitude = commanded;
        int probes = 0;
        auto probe = [&](double lat, double lon, float altitude) {
            ++probes;
            assert(lat == captured.lat && lon == captured.lon && altitude == commanded);
            return ground;
        };
        auto placed = batc::grounded_position(captured, probe, ground, true);
        assert(probes == 1 && placed.altitude == ground);
        assert(placed.lat == captured.lat && placed.lon == captured.lon);
        assert(placed.flags == captured.flags && placed.heading == captured.heading);
        captured.flags &= ~1;
        placed = batc::grounded_position(captured, probe, ground, true);
        assert(probes == 2 && placed.altitude == std::max(commanded, ground) && placed.flags == captured.flags);
        captured.flags |= 1;
        bool missed = false;
        try {
            batc::grounded_position(captured, [](double, double, float) -> float {
                throw std::runtime_error("terrain probe missed");
            }, ground, true);
        } catch (const std::runtime_error& error) {
            missed = std::string(error.what()) == "terrain probe missed";
        }
        assert(missed && captured.altitude == commanded);
    }
    // KAUS ASA352 remained airborne through a rollout 119 metres below terrain.
    for (auto [commanded, ground, flags] : std::array<std::array<float, 3>, 3>{{
        {158.989089966f, 160.512985229f, 0},
        {33.436084747f, 152.460113525f, 0},
        {33.216777802f, 152.459411621f, 1}
    }}) {
        auto captured = p;
        captured.lat = 30.193988565867574;
        captured.lon = -97.6778932822948;
        captured.altitude = commanded;
        captured.flags = uint8_t(flags);
        int probes = 0;
        auto placed = batc::grounded_position(captured, [&](double lat, double lon, float altitude) {
            ++probes;
            assert(lat == captured.lat && lon == captured.lon && altitude == commanded);
            return ground;
        }, ground, true);
        assert(probes == 1 && placed.altitude == ground && captured.altitude == commanded);
        assert(placed.flags == captured.flags && placed.heading == captured.heading);
    }
    for (float height : {0.0f, 10.0f, 50.0f, 50.001f, 2000.0f}) {
        auto airborne = p;
        airborne.flags &= ~1;
        airborne.altitude = 150.0f + height;
        int probes = 0;
        auto probe = [&](double, double, float) { ++probes; return 150.0f; };
        auto placed = batc::grounded_position(airborne, probe, 150.0f, true);
        assert(probes == (height <= 50.0f ? 1 : 0) && placed.altitude == airborne.altitude);
        placed = batc::grounded_position(airborne, probe, std::nullopt, true);
        assert(probes == (height <= 50.0f ? 1 : 0) && placed.altitude == airborne.altitude);
        probes = 0;
        placed = batc::grounded_position(airborne, probe, std::nullopt, false);
        assert(probes == 1 && placed.altitude == airborne.altitude);
    }
    // A cached elevation only gates probing; placement uses the current location's terrain.
    auto airborne = p;
    airborne.flags &= ~1;
    airborne.altitude = 100;
    auto placed = batc::grounded_position(airborne, [](double, double, float) { return 90.0f; }, 150.0f, true);
    assert(placed.altitude == 100 && placed.flags == airborne.flags);
    placed = batc::grounded_position(airborne, [](double, double, float) { return 110.0f; }, 150.0f, true);
    assert(placed.altitude == 110 && placed.flags == airborne.flags);
    for (float altitude : {76.6890684928934f, 132.6642f, 435.25f}) {
        batc::TrafficFeedback feedback{altitude, std::nullopt};
        auto responses = feedback.packets(7);
        assert(responses.size() == 1 && responses[0][4] == 136);
        batc::Reader altitude_packet(responses[0]);
        assert(altitude_packet.integer() == 7 && altitude_packet.real() == altitude);
        altitude_packet.end();
        feedback.ground = 0.0f;
        responses = feedback.packets(7);
        assert(responses.size() == 2 && responses[1][4] == 132);
        batc::Reader ground_packet(responses[1]);
        assert(ground_packet.integer() == 7 && ground_packet.real() == altitude);
        ground_packet.end();
    }
    // Replayed KAUS feedback distinguishes airport elevation from aircraft clearance.
    for (auto [altitude, ground, expected] : std::array<std::array<float, 3>, 7>{{
        {371.814605713f, 135.797988892f, 236.016616821f},
        {158.991973877f, 160.512985229f, 0.0f},
        {33.438381195f, 152.460113525f, 0.0f},
        {152.461196899f, 152.459411621f, 0.0f},
        {0.01f, 0.0f, 0.0f},
        {0.010001f, 0.0f, 0.010001f},
        {150.0f, 0.0f, 150.0f}
    }}) {
        batc::TrafficFeedback feedback{altitude, ground};
        auto responses = feedback.packets(40);
        assert(responses.size() == 2);
        batc::Reader msl(responses[0]);
        assert(responses[0][4] == 136 && msl.integer() == 40 && msl.real() == altitude);
        msl.end();
        batc::Reader agl(responses[1]);
        assert(responses[1][4] == 132 && agl.integer() == 40 && std::abs(agl.real() - expected) < 0.0001f);
        agl.end();
    }
    batc::TerrainWarning warning;
    int warning_count = 0;
    for (int tick = 0; tick < 40; ++tick)
        for (int aircraft = 0; aircraft < 21; ++aircraft)
            warning_count += warning.due(tick * 0.25);
    assert(warning_count == 2);
    assert(!warning.due(9.999));
    assert(warning.due(10));
    packet = load(root + "/BuildSetDatarefFloat.bin");
    batc::Reader d(packet);
    assert(d.byte() == 0 && d.real() == 0.5f);
    assert(d.text(256) == "sim/cockpit2/radios/actuators/com1_frequency_hz_833");
    d.end();
    batc::PlayerState own;
    own.coordinates = {35.877639, -78.787472, 132.6642};
    for (size_t i = 0; i < own.analog.size(); ++i) own.analog[i] = float(i) + 0.25f;
    own.discrete = {118005, 121900, 122800, 120000, 2105, 2, 0, 0, 0, 0};
    own.switches = {1, 1, 1, 1, 1, 1, 0, 1, 0, 0};
    own.icao = "FA8X";
    own.registration = "N-TEST";
    assert(own.packet(42).size() == 167);
    save(root + "/native-player.bin", own.packet(42));
    save(root + "/native-heartbeat.bin", batc::heartbeat(124000, 123456, 42));
    save(root + "/native-status.bin", batc::status(2, 63, 0));
    batc::Writer time(133, 42);
    time.byte(6); time.word(297); time.word(2026); time.integer(72000); time.integer(57600); time.byte(1);
    assert(time.bytes.size() == 26);
    save(root + "/native-time.bin", time.finish());
    std::cout << "Vendor command fixtures decoded; truncated frames rejected; native response fixtures generated.\n";
}
