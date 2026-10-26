#include "wire.hpp"
#include "placement.hpp"
#include "menu.hpp"
#include "XPMPAircraft.h"
#include "XPMPMultiplayer.h"
#include "XPLMDataAccess.h"
#include "XPLMGraphics.h"
#include "XPLMInstance.h"
#include "XPLMPlugin.h"
#include "XPLMProcessing.h"
#include "XPLMScenery.h"
#include "XPLMUtilities.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sys/socket.h>
#include <unistd.h>

namespace {
using Clock = std::chrono::steady_clock;
double now() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }
batc::DiagnosticRecorder recorder;
void log(const std::string& text) {
    XPLMDebugString(("[BATC Native] " + text + "\n").c_str());
    if (recorder.status().active) recorder.submit(batc::message_record(now(), text));
}
std::string last_issue;
void report_issue(const std::string& text) { last_issue = text; log(text); }
int udp = -1, sim_version = 0;
uint32_t sequence = 1, session = 0, app_pid = 0;
double last_heartbeat = 0, last_status = 0, last_state = 0, last_capture = 0, last_report = 0;
bool aircraft_loaded = false, controls_tcas = false;
bool ptt_pressed = false;
double last_ptt = 0;
XPLMCommandRef ptt_command = nullptr;
std::string plugin_dir;
std::filesystem::path diagnostic_dir;
std::unique_ptr<batc::PluginMenu> plugin_menu;
std::optional<batc::PlayerState> last_player;
double last_menu = 0, last_diagnostic = 0;
std::string reported_recording_error;
uint64_t reported_drops = 0, placement_misses = 0, feedback_misses = 0, spawn_refusals = 0, loop_errors = 0;
XPLMProbeRef probe = nullptr;
uint64_t received = 0, sent = 0, rejected = 0;
batc::TerrainWarning terrain_warning, placement_warning;

void send_ptt() {
    if (udp < 0) return;
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(49102);
    peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const unsigned char packet[]{'B', 'P', 'T', 'T', static_cast<unsigned char>(ptt_pressed)};
    if (sendto(udp, packet, sizeof(packet), 0, reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) != ssize_t(sizeof(packet)))
        throw std::runtime_error("PTT relay send failed: " + std::string(strerror(errno)));
    last_ptt = now();
}

int push_to_talk(XPLMCommandRef, XPLMCommandPhase phase, void*) {
    if (phase == xplm_CommandContinue) return 0;
    ptt_pressed = phase == xplm_CommandBegin && app_pid && now() - last_heartbeat <= 3;
    if (phase == xplm_CommandBegin && !ptt_pressed) log("PTT ignored: BeyondATC is not connected");
    else log(std::string("PTT ") + (ptt_pressed ? "pressed" : "released"));
    try { send_ptt(); }
    catch (const std::runtime_error& error) { report_issue(error.what()); }
    return 0;
}

struct Ref {
    XPLMDataRef ref = nullptr;
    std::string name;
    explicit Ref(const std::string& n, bool required = true) : name(n) {
        if (!n.empty()) ref = XPLMFindDataRef(n.c_str());
        if (!ref && required) throw std::runtime_error("missing required dataref: " + n);
    }
    double read(int index = 0) const {
        if (!ref) throw std::runtime_error("unavailable dataref: " + name);
        int types = XPLMGetDataRefTypes(ref);
        if (types & xplmType_Double) return XPLMGetDatad(ref);
        if (types & xplmType_Float) return XPLMGetDataf(ref);
        if (types & xplmType_Int) return XPLMGetDatai(ref);
        if (types & xplmType_FloatArray) {
            float value;
            if (XPLMGetDatavf(ref, &value, index, 1) != 1) throw std::runtime_error("unreadable dataref array: " + name);
            return value;
        }
        if (types & xplmType_IntArray) {
            int value;
            if (XPLMGetDatavi(ref, &value, index, 1) != 1) throw std::runtime_error("unreadable dataref array: " + name);
            return value;
        }
        throw std::runtime_error("unsupported numeric dataref: " + name);
    }
    std::string text() const {
        char value[256]{};
        int length = XPLMGetDatab(ref, value, 0, sizeof(value) - 1);
        if (length < 0 || length >= int(sizeof(value))) throw std::runtime_error("unreadable text dataref: " + name);
        return std::string(value, strnlen(value, sizeof(value)));
    }
};
std::map<std::string, std::unique_ptr<Ref>> refs;
Ref& get_ref(const std::string& path) {
    auto it = refs.find(path);
    if (it == refs.end()) it = refs.emplace(path, std::make_unique<Ref>(path)).first;
    return *it->second;
}
std::array<std::unique_ptr<Ref>, 4> custom;
const std::array<std::string, 3> coordinate_refs{
    "sim/flightmodel/position/latitude", "sim/flightmodel/position/longitude", "sim/flightmodel/position/elevation"};
const std::array<std::string, 14> analog_refs{
    "sim/flightmodel/position/y_agl",
    "sim/cockpit2/gauges/indicators/altitude_ft_pilot",
    "sim/flightmodel2/position/pressure_altitude",
    "sim/flightmodel/position/true_psi", "sim/flightmodel/position/mag_psi",
    "sim/flightmodel/position/indicated_airspeed", "sim/flightmodel/position/true_airspeed",
    "sim/flightmodel/position/groundspeed", "sim/cockpit2/controls/parking_brake_ratio",
    "sim/aircraft/parts/acf_gear_deploy", "sim/weather/aircraft/wind_now_direction_degt",
    "sim/weather/aircraft/wind_now_speed_msc", "sim/cockpit2/radios/actuators/audio_volume_com1",
    "sim/cockpit2/radios/actuators/audio_volume_com2"};
const std::array<std::string, 6> discrete_refs{
    "sim/cockpit2/radios/actuators/com1_frequency_hz_833",
    "sim/cockpit2/radios/actuators/com1_standby_frequency_hz_833",
    "sim/cockpit2/radios/actuators/com2_frequency_hz_833",
    "sim/cockpit2/radios/actuators/com2_standby_frequency_hz_833",
    "sim/cockpit2/radios/actuators/transponder_code", "sim/cockpit2/radios/actuators/transponder_mode"};

void send(const batc::Bytes& bytes) {
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(49101);
    peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    auto result = sendto(udp, bytes.data(), bytes.size(), 0, reinterpret_cast<sockaddr*>(&peer), sizeof(peer));
    if (result != ssize_t(bytes.size())) throw std::runtime_error("UDP send failed: " + std::string(strerror(errno)));
    ++sent;
}
float terrain(double lat, double lon, float altitude) {
    double x, y, z, out_lat, out_lon, out_alt;
    XPLMWorldToLocal(lat, lon, altitude, &x, &y, &z);
    XPLMProbeInfo_t info{};
    info.structSize = sizeof(info);
    if (XPLMProbeTerrainXYZ(probe, float(x), float(y), float(z), &info) != xplm_ProbeHitTerrain)
        throw std::runtime_error("terrain probe missed");
    XPLMLocalToWorld(info.locationX, info.locationY, info.locationZ, &out_lat, &out_lon, &out_alt);
    return float(out_alt);
}

struct Plane : XPMP2::Aircraft {
    batc::Position previous, target;
    std::optional<float> feedback_ground;
    double changed = 0, interval = 0.05, last_feedback = 0, last_packet = 0;
    explicit Plane(const batc::Spawn& s)
        : XPMP2::Aircraft(s.type, s.airline, s.livery, 0, s.model),
          previous(s.position), target(s.position), changed(now()), last_packet(changed) {
        bDrawLabel = false;
        SetGearRatio((s.position.flags & 1) ? 1 : 0);
        UpdatePosition(0, 0);
    }
    batc::Position displayed(double time) const {
        auto p = target;
        double t = std::clamp((time - changed) / interval, 0.0, 1.0);
        auto blend = [t](double a, double b) { return a + (b - a) * t; };
        p.lat = blend(previous.lat, target.lat);
        double longitude_delta = std::remainder(target.lon - previous.lon, 360.0);
        p.lon = std::remainder(previous.lon + longitude_delta * t, 360.0);
        p.altitude = float(blend(previous.altitude, target.altitude));
        p.heading = float(previous.heading + std::remainder(target.heading - previous.heading, 360.0f) * t);
        p.pitch = float(blend(previous.pitch, target.pitch));
        p.roll = float(blend(previous.roll, target.roll));
        return p;
    }
    void update(const batc::Position& p) {
        static bool jetway_warning = false;
        if ((p.flags & 12) && !jetway_warning) {
            log("traffic jetway requests are not implemented");
            jetway_warning = true;
        }
        double time = now();
        previous = displayed(time);
        double distance = std::hypot((target.lat - p.lat) * 111000,
            std::remainder(target.lon - p.lon, 360.0) * 111000 * std::cos(p.lat * 0.017453292519943));
        if (distance > 1000) previous = p;
        interval = std::clamp(time - last_packet, 0.025, 0.2);
        changed = last_packet = time;
        target = p;
    }
    void UpdatePosition(float elapsed, int) override {
        auto p = displayed(now());
        try {
            p = batc::grounded_position(p, terrain, feedback_ground, last_feedback != 0);
        } catch (const std::runtime_error& error) {
            ++placement_misses;
            if (placement_warning.due(now()))
                report_issue("cannot check terrain placement for aircraft " + std::to_string(GetModeS_ID()) +
                    " at " + std::to_string(p.lat) + "," + std::to_string(p.lon) +
                    ": " + error.what() + "; retaining commanded altitude");
        }
        SetLocation(p.lat, p.lon, p.altitude * 3.280839895, (p.flags & 1) != 0);
        SetHeading(p.heading); SetPitch(p.pitch); SetRoll(p.roll);
        SetThrustRatio(float(p.n1) / 100);
        SetEngineRotRpm(float(p.n1) * 60);
        SetTireRotRpm(p.speed * 60 / 3.141592654f);
        SetTireRotAngle(std::fmod(GetTireRotAngle() + p.speed * elapsed * 114.591559f, 360.0f));
    }
};
std::map<uint32_t, std::unique_ptr<Plane>> planes;

struct GroundObject {
    XPLMInstanceRef instance = nullptr;
    XPLMObjectRef object = nullptr;
    std::string path;
    double last_seen = 0;
    ~GroundObject() {
        if (instance) XPLMDestroyInstance(instance);
        if (object) XPLMUnloadObject(object);
    }
};
std::map<uint32_t, std::unique_ptr<GroundObject>> objects;

batc::DiagnosticStatus diagnostic_status(double time) {
    batc::DiagnosticStatus s;
    s.enabled = udp >= 0;
    s.connected = s.enabled && app_pid && time - last_heartbeat <= 3;
    s.aircraft_loaded = aircraft_loaded;
    s.tcas = s.enabled && XPMPHasControlOfAIAircraft();
    s.ptt = ptt_pressed;
    if (app_pid) s.heartbeat_age = time - last_heartbeat;
    if (last_player) s.player_age = time - last_state;
    s.aircraft = planes.size(); s.objects = objects.size(); s.models = XPMPGetNumberOfInstalledModels();
    s.received = received; s.sent = sent; s.rejected = rejected;
    s.placement_misses = placement_misses; s.feedback_misses = feedback_misses;
    s.spawn_refusals = spawn_refusals; s.loop_errors = loop_errors;
    s.last_issue = last_issue;
    return s;
}
void refresh_menu() {
    if (plugin_menu) plugin_menu->refresh(diagnostic_status(now()), recorder.status());
}
void record_sample(double time) {
    std::vector<batc::TrafficSample> traffic;
    traffic.reserve(planes.size());
    for (const auto& [id, plane] : planes) {
        batc::TrafficSample sample;
        sample.id = id; sample.mode_s = plane->GetModeS_ID();
        sample.type = plane->acIcaoType; sample.callsign = plane->acCallSign;
        sample.commanded = plane->target; sample.interpolated = plane->displayed(time);
        const auto& draw = plane->GetLocation();
        sample.model_offset = plane->GetVertOfs();
        XPLMLocalToWorld(draw.x, draw.y - sample.model_offset, draw.z,
                        &sample.rendered_base[0], &sample.rendered_base[1], &sample.rendered_base[2]);
        sample.rendered_attitude = {draw.heading, draw.pitch, draw.roll};
        sample.ground = plane->feedback_ground;
        sample.packet_age = time - plane->last_packet;
        if (plane->last_feedback) sample.feedback_age = time - plane->last_feedback;
        traffic.push_back(std::move(sample));
    }
    recorder.submit(batc::sample_record(time, diagnostic_status(time), recorder.status(), last_player, traffic));
}
void report_recording_health() {
    auto state = recorder.status();
    if (!state.error.empty() && state.error != reported_recording_error) {
        reported_recording_error = state.error;
        report_issue("diagnostic recording stopped: " + state.error);
    }
    if (state.dropped != reported_drops) {
        reported_drops = state.dropped;
        report_issue("diagnostic records dropped: " + std::to_string(state.dropped) + "; trace is incomplete");
    }
}
void diagnostics_tick(double time) {
    if (time - last_menu >= 1) {
        report_recording_health();
        refresh_menu();
        last_menu = time;
    }
    if (recorder.status().active && time - last_diagnostic >= 0.2) {
        record_sample(time);
        last_diagnostic = time;
    }
}
void menu_action(batc::MenuAction action) {
    try {
        if (action == batc::MenuAction::recording) {
            if (recorder.status().active) {
                log("diagnostic recording stopped by menu");
                recorder.stop();
                report_recording_health();
            } else {
                if (udp < 0) throw std::runtime_error("enable the plugin before starting diagnostic recording");
                recorder.start(diagnostic_dir);
                reported_recording_error.clear();
                reported_drops = 0;
                log("diagnostic recording started (5 Hz, max 32 MiB): " + diagnostic_dir.string());
                record_sample(now());
            }
        } else {
            log("status snapshot:");
            for (const auto& line : batc::status_lines(diagnostic_status(now()))) log(line);
            auto state = recorder.status();
            log("diagnostic recording=" + std::string(state.active ? "ON" : "off") +
                " dropped=" + std::to_string(state.dropped) + " directory=" + diagnostic_dir.string());
            if (!state.error.empty()) log("diagnostic recording error: " + state.error);
        }
    } catch (const std::exception& error) {
        report_issue("menu action failed: " + std::string(error.what()));
    }
    refresh_menu();
}

void lookup_object(const char* path, void* out) {
    auto& selected = *static_cast<std::string*>(out);
    if (selected.empty()) selected = path;
}
void object_update(batc::Reader& r) {
    uint32_t id = r.integer();
    double lat = r.coordinate(), lon = r.coordinate();
    float heading = r.real();
    bool engine = (r.byte() & 1) != 0;
    std::string path = r.text(64);
    r.end();
    if (std::abs(lat) > 90 || std::abs(lon) > 180) throw std::runtime_error("invalid ground object coordinates");
    auto it = objects.find(id);
    if (it == objects.end() || it->second->path != path) {
        std::string resolved;
        XPLMLookupObjects(path.c_str(), float(lat), float(lon), lookup_object, &resolved);
        if (resolved.empty()) throw std::runtime_error("ground object library path not found: " + path);
        auto o = std::make_unique<GroundObject>();
        o->path = path;
        o->object = XPLMLoadObject(resolved.c_str());
        if (!o->object) throw std::runtime_error("ground object failed to load: " + resolved);
        const char* datarefs[]{"sim/graphics/animation/ground_traffic/engine_running",
            "sim/graphics/animation/ground_traffic/engine_rpm_rat", nullptr};
        o->instance = XPLMCreateInstance(o->object, datarefs);
        if (!o->instance) throw std::runtime_error("ground object instance failed");
        objects[id] = std::move(o);
        it = objects.find(id);
    }
    auto& object = *it->second;
    double x, y, z;
    XPLMWorldToLocal(lat, lon, terrain(lat, lon, 0), &x, &y, &z);
    XPLMDrawInfo_t draw{};
    draw.structSize = sizeof(draw);
    draw.x = float(x); draw.y = float(y); draw.z = float(z); draw.heading = heading;
    float values[]{float(engine), engine ? 1.0f : 0.0f};
    XPLMInstanceSetPosition(object.instance, &draw, values);
    object.last_seen = now();
}
void clear() { planes.clear(); objects.clear(); }
void release() {
    ptt_pressed = false;
    try { send_ptt(); }
    catch (const std::runtime_error& error) { log(error.what()); }
    clear();
    XPMPMultiplayerDisable();
    controls_tcas = false;
    app_pid = 0;
    last_player.reset();
}

void set_dataref(batc::Reader& r) {
    auto type = r.byte();
    auto bits = r.integer();
    std::string path = r.text(256);
    r.end();
    auto& value = get_ref(path);
    if (!XPLMCanWriteDataRef(value.ref)) throw std::runtime_error("dataref is read-only: " + path);
    int types = XPLMGetDataRefTypes(value.ref);
    double number;
    if (type == 0) {
        float f;
        std::memcpy(&f, &bits, sizeof(f));
        number = f;
    } else if (type == 1) {
        int32_t i;
        std::memcpy(&i, &bits, sizeof(i));
        number = i;
    } else throw std::runtime_error("unknown dataref wire type");
    if (!std::isfinite(number)) throw std::runtime_error("non-finite dataref write: " + path);
    if (types & xplmType_Int) {
        if (number < INT32_MIN || number > INT32_MAX) throw std::runtime_error("integer dataref write is out of range: " + path);
        XPLMSetDatai(value.ref, int(number));
    }
    else if (types & xplmType_Float) XPLMSetDataf(value.ref, float(number));
    else if (types & xplmType_Double) XPLMSetDatad(value.ref, number);
    else throw std::runtime_error("unsupported writable dataref: " + path);
    log("radio/transponder write " + path + " = " + std::to_string(number));
}
void command(const batc::Bytes& bytes) {
    batc::Reader r(bytes);
    uint8_t op = bytes[4];
    if (op == 127) {
        auto min = r.byte(), max = r.byte();
        uint32_t pid = r.integer();
        r.end();
        if (min > batc::protocol || max < batc::protocol) throw std::runtime_error("incompatible heartbeat protocol range");
        if (app_pid != pid) {
            clear();
            if (!app_pid) {
                const char* error = XPMPMultiplayerEnable();
                if (error && *error) report_issue(std::string("TCAS acquisition: ") + error);
            }
            app_pid = pid;
            log("BeyondATC handshake, process " + std::to_string(pid));
        }
        last_heartbeat = now();
        controls_tcas = XPMPHasControlOfAIAircraft();
        send(batc::heartbeat(sim_version, session, batc::u32(bytes.data() + 8)));
        return;
    }
    if (now() - last_heartbeat > 3) throw std::runtime_error("command arrived without a live heartbeat");
    if (op == 9) { r.end(); clear(); return; }
    if (op == 10) { set_dataref(r); return; }
    if (op == 11) {
        for (auto& item : custom) {
            auto path = r.text(256);
            item.reset();
            if (!path.empty()) {
                auto candidate = std::make_unique<Ref>(path, false);
                if (candidate->ref) item = std::move(candidate);
                else log("requested aircraft override is unavailable: " + path);
            }
        }
        r.end();
        return;
    }
    if (op == 12) { object_update(r); return; }
    uint32_t id = r.integer();
    if (op == 13) { r.end(); objects.erase(id); return; }
    if (op == 1) {
        auto s = batc::spawn(bytes);
        if (planes.count(id)) { send(batc::ack(129, id)); return; }
        if (planes.size() >= 63 || XPMPGetNumberOfInstalledModels() == 0) {
            ++spawn_refusals;
            report_issue("spawn refused: traffic capacity or CSL models unavailable");
            send(batc::ack(129, id, false)); return;
        }
        try {
            planes.emplace(id, std::make_unique<Plane>(s));
            send(batc::ack(129, id));
            log("spawned " + std::to_string(id) + " " + s.type + " " + s.airline);
        } catch (const XPMP2::XPMP2Error& error) {
            ++spawn_refusals;
            report_issue(std::string("spawn failed: ") + error.what());
            send(batc::ack(129, id, false));
        }
        return;
    }
    if (op == 3) { r.end(); planes.erase(id); send(batc::ack(130, id)); return; }
    auto it = planes.find(id);
    if (it == planes.end()) { send(batc::ack(134, id)); return; }
    auto& plane = *it->second;
    if (op == 2) plane.update(batc::update(r));
    else if (op == 4) {
        plane.SetLightsBeacon(r.byte() != 0); plane.SetLightsLanding(r.byte() != 0);
        plane.SetLightsTaxi(r.byte() != 0); plane.SetLightsNav(r.byte() != 0);
        plane.SetLightsStrobe(r.byte() != 0); r.end();
    } else if (op == 5) {
        plane.SetGearRatio(r.byte() ? 1 : 0);
        plane.SetFlapRatio(std::clamp(r.real(), 0.0f, 1.0f));
        plane.SetSpeedbrakeRatio(r.byte() ? 1 : 0); plane.SetReversDeployRatio(r.byte() ? 1 : 0);
        r.end();
    } else if (op == 6) {
        plane.acCallSign = plane.label = r.text(16); r.end();
    } else if (op == 8) {
        uint8_t mode = r.byte(); r.end();
        if (mode > xpmpTransponderMode_ModeS_TARA) throw std::runtime_error("invalid SSR mode");
        plane.acRadar.mode = static_cast<XPMPTransponderMode>(mode);
    } else throw std::runtime_error("unsupported command opcode " + std::to_string(op));
}

batc::PlayerState player() {
    batc::PlayerState p;
    for (size_t i = 0; i < coordinate_refs.size(); ++i) p.coordinates[i] = get_ref(coordinate_refs[i]).read();
    for (size_t i = 0; i < analog_refs.size(); ++i) p.analog[i] = float(get_ref(analog_refs[i]).read());
    for (size_t i = 0; i < discrete_refs.size(); ++i) p.discrete[i] = uint32_t(get_ref(discrete_refs[i]).read());
    for (size_t i = 0; i < custom.size(); ++i) p.discrete[6 + i] = custom[i] ? uint32_t(custom[i]->read()) : 0;
    int transmit = int(get_ref("sim/cockpit2/radios/actuators/audio_com_selection").read());
    p.switches = {
        uint8_t(get_ref("sim/flightmodel/failures/onground_any").read() != 0),
        uint8_t(get_ref("sim/flightmodel/engine/ENGN_running").read(0) != 0),
        uint8_t(get_ref("sim/flightmodel/engine/ENGN_running").read(1) != 0),
        uint8_t(get_ref("sim/cockpit2/radios/actuators/com1_power").read() != 0),
        uint8_t(get_ref("sim/cockpit2/radios/actuators/com2_power").read() != 0),
        uint8_t(get_ref("sim/cockpit2/radios/actuators/audio_selection_com1").read() != 0),
        uint8_t(get_ref("sim/cockpit2/radios/actuators/audio_selection_com2").read() != 0),
        uint8_t(transmit == 6), uint8_t(transmit == 7),
        uint8_t(get_ref("sim/cockpit2/radios/indicators/transponder_id").read() != 0)};
    p.icao = get_ref("sim/aircraft/view/acf_ICAO").text();
    p.registration = get_ref("sim/aircraft/view/acf_tailnum").text();
    return p;
}
void sim_time() {
    static auto year_ref = std::make_unique<Ref>("sim/time/local_date_year", false);
    static bool reported_fallback = false;
    int year;
    if (year_ref->ref) year = int(year_ref->read());
    else {
        if (!reported_fallback) { log("simulator has no calendar-year dataref; reporting current calendar year explicitly"); reported_fallback = true; }
        auto current = std::time(nullptr);
        year = std::localtime(&current)->tm_year + 1900;
    }
    int day = int(get_ref("sim/time/zulu_date_days").read());
    int local_day = int(get_ref("sim/time/local_date_days").read());
    if (day - local_day > 180) --year;
    else if (local_day - day > 180) ++year;
    if (year < 1900 || year > 9999 || day < 0 || day > 365) throw std::runtime_error("invalid simulator calendar");
    std::tm date{};
    date.tm_year = year - 1900; date.tm_mon = 0; date.tm_mday = day + 1;
    timegm(&date);
    batc::Writer w(133, sequence++);
    w.byte(uint8_t(date.tm_wday)); w.word(uint16_t(day + 1)); w.word(uint16_t(year));
    w.integer(uint32_t(get_ref("sim/time/zulu_time_sec").read()));
    w.integer(uint32_t(get_ref("sim/time/local_time_sec").read()));
    w.byte(year_ref->ref ? 1 : 2);
    send(w.finish());
}
float loop(float, float, int, void*) {
    try {
        for (int i = 0; i < 1000; ++i) {
            uint8_t buffer[2048];
            sockaddr_in from{};
            socklen_t size = sizeof(from);
            auto length = recvfrom(udp, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &size);
            if (length < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                throw std::runtime_error("UDP receive failed: " + std::string(strerror(errno)));
            }
            ++received;
            if (from.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) { ++rejected; continue; }
            try { command(batc::Bytes(buffer, buffer + length)); }
            catch (const std::runtime_error& error) {
                ++rejected;
                if (rejected <= 10 || rejected % 100 == 0) report_issue(std::string("command rejected: ") + error.what());
            }
        }
        double time = now();
        if (app_pid && time - last_heartbeat > 3) {
            report_issue("BeyondATC heartbeat expired; releasing traffic, TCAS and PTT");
            release();
        }
        if (time - last_ptt >= 0.05) send_ptt();
        if (!app_pid) { diagnostics_tick(time); return 0.025f; }
        if (time - last_status >= 1) {
            controls_tcas = XPMPHasControlOfAIAircraft();
            uint32_t flags = controls_tcas ? 0 : 2;
            if (XPMPGetNumberOfInstalledModels() == 0) flags |= 1;
            send(batc::status(uint32_t(planes.size()), 63, flags));
            sim_time();
            last_status = time;
        }
        if (aircraft_loaded && time - last_state >= 0.05) {
            auto p = player();
            auto packet = p.packet(sequence++);
            send(packet);
            last_player = p;
            last_state = time;
            if (time - last_capture >= 1) {
                auto temporary = plugin_dir + "/last-player.bin.tmp";
                std::ofstream capture(temporary, std::ios::binary | std::ios::trunc);
                capture.write(reinterpret_cast<const char*>(packet.data()), packet.size());
                capture.close();
                if (!capture) throw std::runtime_error("cannot write telemetry capture");
                std::filesystem::rename(temporary, plugin_dir + "/last-player.bin");
                last_capture = time;
            }
        }
        for (auto& item : planes) {
            auto& plane = *item.second;
            if (time - plane.last_feedback < 0.25) continue;
            const auto& draw = plane.GetLocation();
            double lat, lon, altitude;
            XPLMLocalToWorld(draw.x, draw.y - plane.GetVertOfs(), draw.z, &lat, &lon, &altitude);
            batc::TrafficFeedback feedback{float(altitude), std::nullopt};
            try {
                feedback.ground = terrain(lat, lon, float(altitude));
            } catch (const std::runtime_error& error) {
                ++feedback_misses;
                if (terrain_warning.due(time))
                    report_issue("ground feedback unavailable for aircraft " + std::to_string(item.first) +
                        " at " + std::to_string(lat) + "," + std::to_string(lon) +
                        " MSL=" + std::to_string(altitude) + ": " + error.what() +
                        "; aircraft altitude feedback remains active");
            }
            for (const auto& response : feedback.packets(item.first)) send(response);
            plane.feedback_ground = feedback.ground;
            plane.last_feedback = time;
        }
        for (auto it = objects.begin(); it != objects.end();) {
            if (time - it->second->last_seen > 10) it = objects.erase(it); else ++it;
        }
        if (time - last_report >= 10) {
            log("rx=" + std::to_string(received) + " tx=" + std::to_string(sent) +
                " rejected=" + std::to_string(rejected) + " aircraft=" + std::to_string(planes.size()) +
                " TCAS=" + std::to_string(controls_tcas) + " player=" + std::to_string(aircraft_loaded));
            last_report = time;
        }
    } catch (const std::exception& error) {
        ++loop_errors;
        static std::string previous;
        if (previous != error.what()) { previous = error.what(); report_issue("flight-loop error: " + previous); }
    }
    diagnostics_tick(now());
    return 0.025f;
}
}

PLUGIN_API int XPluginStart(char* name, char* signature, char* description) {
    strcpy(name, "BeyondATC Native");
    strcpy(signature, "local.beyondatc.native");
    strcpy(description, "Native Linux adapter for the BeyondATC Experimental UDP interface.");
    XPLMEnableFeature("XPLM_USE_NATIVE_PATHS", 1);
    char path[4096]{};
    XPLMGetPluginInfo(XPLMGetMyID(), nullptr, path, nullptr, nullptr);
    plugin_dir = std::filesystem::path(path).parent_path().parent_path().string();
    XPLMGetSystemPath(path);
    diagnostic_dir = std::filesystem::path(path) / "Output/beyondatc-native/diagnostics";
    int sdk, host;
    XPLMGetVersions(&sim_version, &sdk, &host);
    session = uint32_t(std::time(nullptr)) ^ uint32_t(getpid());
    try {
        const char* error = XPMPMultiplayerInit("BeyondATC Native", (plugin_dir + "/Resources").c_str(), nullptr, "A320", "BATCN");
        if (error && *error) throw std::runtime_error(error);
        error = XPMPLoadCSLPackage((plugin_dir + "/CSL").c_str());
        if (error && *error) report_issue(std::string("CSL loading: ") + error);
        probe = XPLMCreateProbe(xplm_ProbeY);
        if (!probe) throw std::runtime_error("cannot create terrain probe");
        ptt_command = XPLMCreateCommand("beyondatc/push_to_talk", "BeyondATC: Push to Talk (hold)");
        if (!ptt_command) throw std::runtime_error("cannot create push-to-talk command");
        plugin_menu = std::make_unique<batc::PluginMenu>(menu_action);
        plugin_menu->create();
        refresh_menu();
        log("native adapter 0.1.0 started; real simulator version=" + std::to_string(sim_version) +
            " models=" + std::to_string(XPMPGetNumberOfInstalledModels()) + " protocol=4");
        return 1;
    } catch (const std::exception& error) {
        log(std::string("startup failed: ") + error.what());
        plugin_menu.reset();
        if (probe) XPLMDestroyProbe(probe);
        probe = nullptr;
        XPMPMultiplayerCleanup();
        return 0;
    }
}
PLUGIN_API int XPluginEnable() {
    udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp < 0) {
        report_issue("cannot create UDP socket: " + std::string(strerror(errno)));
        refresh_menu();
        return 0;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET; address.sin_port = htons(49100);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int buffer = 4 * 1024 * 1024;
    setsockopt(udp, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));
    if (bind(udp, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ||
        fcntl(udp, F_SETFL, O_NONBLOCK) < 0) {
        report_issue("cannot bind command socket: " + std::string(strerror(errno)));
        close(udp); udp = -1;
        refresh_menu();
        return 0;
    }
    XPLMRegisterCommandHandler(ptt_command, push_to_talk, 1, nullptr);
    XPLMRegisterFlightLoopCallback(loop, 0.025f, nullptr);
    log("listening on loopback UDP 49100; telemetry destination 49101");
    log("PTT command: beyondatc/push_to_talk; Down Arrow relay on loopback UDP 49102");
    refresh_menu();
    return 1;
}
PLUGIN_API void XPluginDisable() {
    XPLMUnregisterFlightLoopCallback(loop, nullptr);
    XPLMUnregisterCommandHandler(ptt_command, push_to_talk, 1, nullptr);
    if (recorder.status().active) log("diagnostic recording stopped: plugin disabled");
    recorder.stop();
    report_recording_health();
    release();
    if (udp >= 0) close(udp);
    udp = -1;
    refresh_menu();
}
PLUGIN_API void XPluginStop() {
    recorder.stop();
    report_recording_health();
    plugin_menu.reset();
    clear();
    refs.clear();
    for (auto& item : custom) item.reset();
    if (probe) XPLMDestroyProbe(probe);
    probe = nullptr;
    XPMPMultiplayerCleanup();
}
PLUGIN_API void XPluginReceiveMessage(XPLMPluginID, int message, void* parameter) {
    if (message == XPLM_MSG_PLANE_LOADED && reinterpret_cast<intptr_t>(parameter) == 0) {
        aircraft_loaded = true;
        last_player.reset();
        log("real user aircraft loaded");
    }
}
