#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace batc {
using Bytes = std::vector<uint8_t>;
constexpr uint8_t protocol = 4;
constexpr size_t header_size = 12;

inline uint32_t u32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline void put32(Bytes& b, size_t offset, uint32_t value) {
    for (int i = 0; i < 4; ++i) b.at(offset + i) = uint8_t(value >> (i * 8));
}

struct Reader {
    const Bytes& bytes;
    size_t offset = header_size;
    explicit Reader(const Bytes& b) : bytes(b) {
        if (b.size() < header_size || std::memcmp(b.data(), "BATC", 4) ||
            b[5] != protocol || (size_t(b[6]) | size_t(b[7]) << 8) != b.size())
            throw std::runtime_error("invalid BATC header, version or length");
    }
    void require(size_t count) const {
        if (count > bytes.size() - offset) throw std::runtime_error("truncated BATC payload");
    }
    uint8_t byte() { require(1); return bytes[offset++]; }
    uint32_t integer() { require(4); auto v = u32(bytes.data() + offset); offset += 4; return v; }
    float real() {
        auto bits = integer();
        float v;
        std::memcpy(&v, &bits, sizeof(v));
        if (!std::isfinite(v)) throw std::runtime_error("non-finite BATC float");
        return v;
    }
    double coordinate() {
        uint64_t bits = integer();
        bits |= uint64_t(integer()) << 32;
        double v;
        std::memcpy(&v, &bits, sizeof(v));
        if (!std::isfinite(v)) throw std::runtime_error("non-finite BATC double");
        return v;
    }
    std::string text(size_t length) {
        require(length);
        auto p = reinterpret_cast<const char*>(bytes.data() + offset);
        auto n = strnlen(p, length);
        offset += length;
        return std::string(p, n);
    }
    void end() const {
        if (offset != bytes.size()) throw std::runtime_error("unexpected BATC payload length");
    }
};

struct Writer {
    Bytes bytes;
    explicit Writer(uint8_t opcode, uint32_t sequence = 0)
        : bytes{'B', 'A', 'T', 'C', opcode, protocol, 0, 0, 0, 0, 0, 0} {
        put32(bytes, 8, sequence);
    }
    void byte(uint8_t v) { bytes.push_back(v); }
    void word(uint16_t v) { byte(uint8_t(v)); byte(uint8_t(v >> 8)); }
    void integer(uint32_t v) {
        size_t at = bytes.size();
        bytes.resize(at + 4);
        put32(bytes, at, v);
    }
    void real(float v) {
        if (!std::isfinite(v)) throw std::runtime_error("non-finite outgoing float");
        uint32_t bits;
        std::memcpy(&bits, &v, sizeof(bits));
        integer(bits);
    }
    void coordinate(double v) {
        if (!std::isfinite(v)) throw std::runtime_error("non-finite outgoing coordinate");
        uint64_t bits;
        std::memcpy(&bits, &v, sizeof(bits));
        integer(uint32_t(bits));
        integer(uint32_t(bits >> 32));
    }
    void text(const std::string& value, size_t count) {
        size_t at = bytes.size();
        bytes.resize(at + count, 0);
        std::memcpy(bytes.data() + at, value.data(), std::min(value.size(), count - 1));
    }
    Bytes finish() {
        if (bytes.size() > 65535) throw std::runtime_error("oversized outgoing BATC packet");
        bytes[6] = uint8_t(bytes.size());
        bytes[7] = uint8_t(bytes.size() >> 8);
        return bytes;
    }
};

struct Position {
    double lat = 0, lon = 0;
    float altitude = 0, heading = 0, pitch = 0, roll = 0, speed = 0, vertical_speed = 0;
    uint8_t flags = 0, n1 = 0;
    void validate() const {
        if (std::abs(lat) > 90 || std::abs(lon) > 180)
            throw std::runtime_error("invalid aircraft coordinates");
    }
};
struct Spawn {
    uint32_t id;
    std::string type, airline, livery, model;
    Position position;
};
inline Spawn spawn(const Bytes& bytes) {
    Reader r(bytes);
    Spawn s;
    s.id = r.integer();
    s.type = r.text(8);
    s.airline = r.text(4);
    s.livery = r.text(16);
    s.position.lat = r.coordinate();
    s.position.lon = r.coordinate();
    s.position.altitude = r.real();
    s.position.heading = r.real();
    s.position.flags = r.byte() ? 1 : 0;
    s.model = r.text(64);
    r.end();
    s.position.validate();
    return s;
}
inline Position update(Reader& r) {
    Position p;
    p.lat = r.coordinate();
    p.lon = r.coordinate();
    p.altitude = r.real();
    p.heading = r.real();
    p.pitch = r.real();
    p.roll = r.real();
    p.speed = r.real();
    p.vertical_speed = r.real();
    p.flags = r.byte();
    p.n1 = r.byte();
    r.end();
    p.validate();
    return p;
}

struct PlayerState {
    std::array<double, 3> coordinates{};
    std::array<float, 14> analog{};
    std::array<uint32_t, 10> discrete{};
    std::array<uint8_t, 10> switches{};
    std::string icao, registration;
    Bytes packet(uint32_t seq) const {
        Writer w(135, seq);
        for (auto v : coordinates) w.coordinate(v);
        for (auto v : analog) w.real(v);
        for (auto v : discrete) w.integer(v);
        for (auto v : switches) w.byte(v);
        w.byte(0);
        w.text(icao, 8);
        w.text(registration, 16);
        return w.finish();
    }
};

inline Bytes heartbeat(int simulator_version, uint32_t session, uint32_t sequence = 0) {
    Writer w(255, sequence);
    w.byte(protocol);
    w.text("1.0.0", 16);
    w.integer(uint32_t(simulator_version));
    w.integer(session);
    return w.finish();
}
inline Bytes status(uint32_t count, uint32_t capacity, uint32_t flags) {
    Writer w(131);
    w.integer(count);
    w.integer(capacity);
    w.integer(flags);
    w.text("1.0.0", 16);
    return w.finish();
}
inline Bytes scalar(uint8_t opcode, uint32_t id, float value) {
    Writer w(opcode);
    w.integer(id);
    w.real(value);
    return w.finish();
}
struct TrafficFeedback {
    float altitude;
    std::optional<float> ground;
    std::optional<float> height_agl() const {
        if (!ground) return std::nullopt;
        const float height = altitude - *ground;
        // World/local round trips leave millimetres of apparent wheel clearance.
        return height <= 0.01f ? 0.0f : height;
    }
    std::vector<Bytes> packets(uint32_t id) const {
        std::vector<Bytes> result{scalar(136, id, altitude)};
        // BATC's "GroundElev" response carries AGL metres, not terrain MSL.
        if (auto height = height_agl()) result.push_back(scalar(132, id, *height));
        return result;
    }
};
struct TerrainWarning {
    double next = 0;
    bool due(double time) {
        if (time < next) return false;
        next = time + 5;
        return true;
    }
};
inline Bytes ack(uint8_t opcode, uint32_t id, bool success = true) {
    Writer w(opcode);
    w.integer(id);
    if (opcode == 129) w.byte(success);
    return w.finish();
}
}
