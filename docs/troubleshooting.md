# Troubleshooting and diagnostics

## Start with status, not a traffic reset

```sh
beyondatc-native check
beyondatc-native status
journalctl --user -u beyondatc-native.service --no-pager -n 60
```

`check` validates the managed installation without starting anything. `status`
also asks systemd for service state, so an inactive service returns a nonzero
exit code even if the installation itself is intact.

In X-Plane, open **Plugins > BeyondATC Native**. The menu shows connection and
heartbeat age, traffic/TCAS, models, ownship telemetry age, PTT, packet/error
counters and the latest issue. **Write status snapshot to Log.txt** adds a
snapshot without enabling detailed recording.

## Connection problems

| Symptom | Check |
|---|---|
| Plugin/menu missing | `Log.txt` plugin-load errors, installed binary, Linux architecture |
| Service skipped/inactive | `beyondatc-native check`, configuration paths, installed user unit and journal |
| Waiting for simulator | The real `X-Plane-x86_64` process must own loopback UDP 49100 |
| No discovery under Wine | Same prefix/runtime, `C:\XPlaneNative`, companion journal and `discovery.log` |
| TCAS not owned | Other traffic injectors/AI controllers, not just model availability |
| Models zero/spawn refused | Valid CSL definitions, model compatibility, 63-aircraft limit |
| Periodic disconnect | Heartbeat age and command rejects; do not remove the watchdog |

No firewall opening is required. Do not run a second bridge or bind a diagnostic
listener to command/response ports; that can steal traffic.

## Push-to-talk and audio

Use X-Plane's `beyondatc/push_to_talk`, with BATC's binding set to Down Arrow.
The menu should show a press/release while connected. The companion forwards
key events inside the same Wine prefix even when X-Plane retains focus.

Microphone permissions, input devices and speech playback belong to Wine/the
official application and the desktop audio session. The connector does not
translate speech. A passing F24 integration test confirms an isolated Wine input
path, not that your microphone or production Down Arrow binding works.

## Traffic visibility, height and runway obstructions

Turn off X-Plane's **Draw parked aircraft**. Do not run competing injectors.
Objects baked into custom scenery need the scenery's own static-aircraft option.

For incorrect heights, capture commanded altitude, ground flag, rendered base,
terrain, model offset and measurement ages together. A terrain probe miss for
an unrelated distant aircraft does not explain a touchdown defect.

For a stopped aircraft, compare fresh command speed/location with rendered
motion. If the app commands a stationary position on a runway and the renderer
matches, that is different from stale commands or a frozen native instance.
Do not delete the aircraft just to make an incident disappear before collecting
evidence. The connector does not guarantee safe sequencing or runway clearance.

## Detailed incident recording

Enable **Record flight diagnostics (5 Hz; max 32 MiB)** in the plugin menu.
It is off at startup and session-only. Disable it immediately after an incident
to preserve the window.

Files are under the simulator:

```text
Output/beyondatc-native/diagnostics/flight.jsonl
Output/beyondatc-native/diagnostics/flight.jsonl.1
Output/beyondatc-native/diagnostics/flight.jsonl.2
Output/beyondatc-native/diagnostics/flight.jsonl.3
```

Each is capped at 8 MiB; `.1` is the newest archive. New recordings append and
eventually rotate older data. Samples include timestamps, ownship location/
speed/radio/transponder state, aircraft IDs/types/callsigns, commanded,
interpolated and rendered positions, model offset, terrain, ages and counters.
They exclude microphone audio, ATC transcripts, account data, ownship
registration and arbitrary raw packets.

These are still **private flight-location/callsign records**. Inspect/redact
before sharing. The queue is bounded; dropped records visibly increment a
counter, and disk errors stop recording without stopping the connector. Forced
termination can leave a partial trailing JSON line. Never describe such a trace
as complete.

Normal `[BATC Native]` messages remain in `Log.txt`. `last-player.bin` is only the
latest ownship snapshot and includes telemetry/registration fields; it is not
history and should not be attached casually. Official app logs may contain
account/session details and ATC transcripts.

## Experimental application crashes

Known failures were observed in the vendor app's runway-name normalization
(duplicate center-runway keys) and departure SID generation (missing runway).
This repository does not include paid-app patchers or modified assemblies.
Linux/Wine is unsupported by the vendor; report your environment honestly and do
not claim a supported-Windows reproduction.

A native build passing does not prove that the simulator, Wine, the app or a
particular aircraft/scenery combination will run correctly. Preserve the first
exception and relevant timestamps; cleanup errors afterward may be secondary.

## Reporting

Use the [contribution guide](../CONTRIBUTING.md). Include the source revision,
OS, Wine, X-Plane/BATC versions, model/scenery, precise steps and a small redacted
incident excerpt. Do not upload your Wine prefix, vendor binaries, authentication
files, purchased models, audio, whole app logs or packet captures by default.
