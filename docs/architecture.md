# Architecture and protocol notes

```text
Native Linux X-Plane
  └─ native plugin (src/)
       ├─ X-Plane SDK: ownship, datarefs, terrain, menu
       ├─ XPMP2: CSL instances, TCAS, model vertical offset
       ├─ UDP 49100 ← commands from BeyondATC
       ├─ UDP 49101 → heartbeat, ownship and per-plane feedback
       └─ UDP 49102 → native PTT state
                           ↓
Python supervisor (python/supervisor.py)
  ├─ verifies actual /proc/PID/exe and simulator socket ownership
  └─ loopback TCP → small Windows discovery/PTT companion (companion/)
                         ↓
                   private Wine prefix
                   ├─ C:\XPlaneNative simulator view
                   └─ official BeyondATC app + official Windows plugin
```

The Windows plugin is exposed to BeyondATC's installation checks, not loaded by
the native simulator. The native plugin independently implements the observed
interface. The companion is named `X-Plane.exe` for simulator discovery; it is
not the simulator and must not advertise one when native X-Plane is absent.
The mirror's scenery/model paths refer to the real installation.

The native/Wine boundary uses sockets, not named shared memory or mutexes.
Wine handles the official application's speech/network/account functionality.
The supervisor does not launch the paid application; `beyondatc-native launch`
checks/starts the service and then explicitly launches it.

## Responsibilities

BATC owns route generation, traffic population, sequencing, clearances and
separation decisions. The connector renders its commands and returns measurements.
It is not a second traffic controller. Native duplicate spawn IDs are
acknowledged without allocating another aircraft; two different IDs may still
occupy the same location.

## Wire conventions

Protocol **4**, with a 12-byte little-endian `BATC` header:

| Offset | Value |
|---|---|
| 0..3 | `BATC` magic |
| 4 | Opcode |
| 5 | Protocol version |
| 6..7 | Total packet length, unsigned 16-bit |
| 8..11 | Sequence, unsigned 32-bit |

Latitude/longitude are doubles in degrees. Traffic altitude is **MSL metres**;
speeds are metres/second. Fixed-size strings are null-terminated/padded.
`src/wire.hpp` is the implementation reference, not a claim of a stable vendor
specification. The plugin identifies as interface version `1.0.0` independently
of this project's package version `0.1.0`.

Important feedback distinction:

| Response | Meaning |
|---|---|
| `136`, plane altitude | Rendered aircraft base altitude, MSL metres |
| `132`, named `GroundElev` by the app | **Aircraft height above terrain, AGL metres**, not terrain MSL |
| Recorder `ground_msl_m` | Measured terrain elevation, MSL metres; this is not response 132 |

The model's vertical offset is added once by XPMP2 and removed once when
measuring the base. Terrain feedback is probed at the rendered coordinates.
Sub-centimetre world/local round-trip noise is reported as zero AGL. Missing
terrain omits AGL feedback but does not suppress valid plane-altitude feedback.

An earlier implementation confused terrain MSL with AGL; recorded landing values
exposed the mistake. Regression tests cover the corrected semantics, including
below-ground input, wheel contact and zero-elevation terrain.

## Placement and timing

Ground-marked aircraft follow fresh terrain rather than a common airport
elevation. Near-ground aircraft also have a terrain floor without changing
BATC's ground flag or valid above-terrain altitude. Latest terrain feedback gates
extra probes within 50 m of the ground, with an initial probe before first
feedback. Probe failure is logged and preserves commanded altitude; no elevation
is fabricated.

Position interpolation is bounded and does not extrapolate indefinitely.
Ownship telemetry targets about 20 Hz and per-plane feedback about 4 Hz; actual
cadence depends on simulator scheduling. A heartbeat older than three seconds
releases traffic, TCAS and PTT. PTT also has supervisor/companion release
watchdogs. Keep those timeouts rather than masking an unresponsive connection.

## Local trust boundary

All sockets bind or target loopback. This avoids LAN exposure, **not access by
other local processes**: there is no cryptographic peer authentication. Treat the
machine and its user session as trusted. Do not port-forward these interfaces
or expose them through a container/VM network bridge.

The command interface can write simulator datarefs and request scenery objects,
not just draw aircraft. It must not be used as an untrusted network service.

## Installation and diagnostic state

The installer and supervisor share a nonblocking file lock. The installer checks
stopped processes, validates recorded paths/hashes, journals replacements and
retains prior generations. It never silently kills a flight. Nix GC roots keep
the resources needed by installed/recovery generations available.

The optional recorder synchronizes ownship, commanded/interpolated/rendered
traffic, terrain and model-offset measurements. SDK access stays on the main
thread; bounded file writing runs on a separate thread. It is an incident aid,
not a flight recorder or proof of application-level traffic intent.
