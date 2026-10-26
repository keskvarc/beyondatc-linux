# Installation, updates and recovery

## Requirements

- An x86_64 Linux machine with native X-Plane 12 and a systemd user session.
- Nix with `nix-command` and `flakes` enabled. NixOS is not required.
- A legally obtained BeyondATC installation with its **Experimental X-Plane
  support**, plus a working account/license and its bundled Windows X-Plane
  plugin. Only 1.11.0.Experimental/protocol 4 was exercised here.
- A Wine-compatible desktop session with working audio/microphone permissions.
  The Nix package supplies Wine; GPU/audio/session support still comes from the
  host.
- A separately obtained, XPMP2-compatible CSL model library containing
  `xsb_aircraft.txt` definitions. Obtain models from their legitimate distributor
  and follow their license. Models are not downloaded by this project.

Back up simulator preferences and valuable Wine-prefix state before experimenting.
Use your ordinary user account, not root.

## 1. Build and install the command

From a checkout:

```sh
nix build
nix profile install .
beyondatc-native --help
```

Keep the checkout, `flake.lock` and the selected package revision for reproducible
updates. The package includes a desktop entry, though desktop discovery of Nix
profile applications varies by distribution.

## 2. Install the official application into a private prefix

The default prefix is `$XDG_DATA_HOME/beyondatc/wine`, normally
`~/.local/share/beyondatc/wine`. This is separate from other Wine applications.
Use **the same Wine runtime** for setup, the application and the companion:

```sh
mkdir -p "$HOME/.local/share/beyondatc/wine"
nix shell .#wine --command \
  env WINEPREFIX="$HOME/.local/share/beyondatc/wine" \
  wine /absolute/path/to/official-installer.exe
```

Obtain the installer directly from BeyondATC, perform login/update through the official app, and
select its Experimental channel if that version is still available. Do not copy
someone else's installed application or authenticated prefix.

The current helper expects this layout:

```text
PREFIX/drive_c/BeyondATC/BeyondATC.exe
PREFIX/drive_c/BeyondATC/X-Plane Plugin/BeyondATC/
```

Choose `C:\BeyondATC` in the installer if offered. If the current vendor installer
uses a different layout or version, stop and inspect before adapting the code;
do not assume that any Experimental build is compatible.

**Close BeyondATC again before continuing.** This project does not automate app
purchase, authentication, channel selection or paid-app modifications.

## 3. Configure paths

Create `~/.config/beyondatc-native/config.json` (or use `$XDG_CONFIG_HOME`).
Replace all example paths with your own **absolute** paths:

```json
{
  "simRoot": "/absolute/path/to/X-Plane 12",
  "cslRoot": "/absolute/path/to/your/CSL/models",
  "prefix": "/home/YOUR_USER/.local/share/beyondatc/wine"
}
```

`~`, environment variables and shell expressions inside JSON are not expanded.
`prefix` is optional if the default is correct. Keep CSL models outside
`Resources/plugins/BeyondATC`, which the installer manages.

Alternatively, put `--sim-root`, `--csl`, `--prefix` and optionally `--state-dir`
**before** the subcommand. A service does not inherit arguments from an earlier
interactive invocation: use the configuration file for normal service use.

## 4. Configure the connector, with both applications closed

Finish the flight, close X-Plane and BeyondATC, and stop any existing bridge:

```sh
# Only needed if a bridge service was previously installed:
systemctl --user stop beyondatc-native.service

beyondatc-native configure --simulator-stopped
```

On a first install, a missing service is expected; do not treat that as a reason
to skip inspecting another active connector.

**`configure` disables known competing injector binaries** (OpenATC, LiveTraffic
and xPilot) by renaming them and recording hashes/recovery paths. This is
intentional: only one injector should own traffic/TCAS. It does not stop running
applications or disable every possible third-party injector. Review your plugin
list; remove other conflicts yourself with the simulator stopped.

The helper:

1. Checks the actual simulator/app process state and takes an exclusive lock.
2. Installs the native plugin at `Resources/plugins/BeyondATC/64/lin.xpl`.
3. Links its packaged XPMP2 resources and your existing CSL directory.
4. Creates the separate Wine-facing `C:\XPlaneNative` view, exposing the official
   Windows plugin for app discovery without loading it into native X-Plane.
5. Saves a transaction record, recovery copies and a Nix garbage-collection root.

Unknown existing plugins, modified binaries, symlinks or interrupted operations
are not silently overwritten. Legacy `--adopt-prototype` is for an exact matching
early prototype manifest only, not a way to force an unrelated installation.

In X-Plane's **Settings > Graphics**, turn **Draw parked aircraft** off. Otherwise
X-Plane and BeyondATC may fill the same stands. Scenery-authored static aircraft
require the scenery's own no-static option. This project does not edit preferences.

## 5. Install the user service

From the same checkout/revision used above:

```sh
mkdir -p "$HOME/.config/systemd/user"
ln -s "$(nix path-info .#default)/share/systemd/user/beyondatc-native.service" \
  "$HOME/.config/systemd/user/beyondatc-native.service"
systemctl --user daemon-reload
systemctl --user enable --now beyondatc-native.service
```

`ln -s` deliberately refuses to overwrite an existing unit. If another package or
Home Manager owns that unit, use its lifecycle instead. Do not replace a unit
while its bridge is serving a flight.

The service waits for the real Linux simulator and its command socket; it does
not start X-Plane or BeyondATC. It requires the user session's desktop/audio
environment to be available to Wine. Check the journal if your desktop does not
import that environment into user services.

## 6. Fly

Start X-Plane normally, then:

```sh
beyondatc-native launch
```

In X-Plane, assign `beyondatc/push_to_talk` to your chosen key/button. In BeyondATC,
set its PTT binding to **Down Arrow**. They need not be the same physical key.
Wine supplies microphone input and playback; the connector does not record audio.

Check **Plugins > BeyondATC Native** for a fresh heartbeat, loaded models,
ownship telemetry and TCAS ownership.

## Updates

Package updates and mutable plugin installation are separate. Updating a Nix
profile does not replace the simulator's plugin.

With both apps closed:

```sh
systemctl --user stop beyondatc-native.service
# Select/review the new source revision before building it.
nix build
nix profile upgrade beyondatc-native
beyondatc-native configure --upgrade --simulator-stopped
```

Check `nix profile list`: the profile entry's name may differ. If its tracked URL
does not select your reviewed revision, use `nix profile install .` for that
revision rather than assuming an upgrade uses the current directory.

A manually linked unit contains an immutable package path. Replace **only your
own old symlink**, after verifying it points to this project's packaged service,
with the new `nix path-info .#default` service path. Then:

```sh
systemctl --user daemon-reload
systemctl --user start beyondatc-native.service
beyondatc-native status
```

Vendor application updates are independent and may change the protocol, layout
or compatibility. Stop both apps and the bridge first; preserve the old prefix
according to the vendor's instructions. This project does not patch the paid app.

## Removal and recovery

With both apps closed:

```sh
systemctl --user stop beyondatc-native.service
beyondatc-native restore --simulator-stopped
systemctl --user disable beyondatc-native.service
```

`restore` withdraws this managed connector and re-enables only the competing
plugins recorded by its installer. It keeps the paid application, login/settings,
prefix, models and all recovery copies. Remove your own service symlink and Nix
profile entry separately if no longer needed.

State normally lives at `~/.local/state/beyondatc-native`:

| Path | Purpose |
|---|---|
| `installation.json` | Installed payload hashes, paths and generation |
| `pending.json` | In-progress installation/restoration journal, if present |
| `roots/` | Retained indirect Nix roots, do not delete while recovery may need them |

Native recovery directories are at
`X-Plane/Output/beyondatc-native/backups/TOKEN/`; Wine-view recovery directories
are at `PREFIX/drive_c/.beyondatc-native-backups/TOKEN/`. Backups stay on their
respective filesystems and outside the plugin scanner.

Interrupted configuration rolls back on the next stopped-app `configure`;
interrupted restoration completes on the next recovery operation. If files no
longer match their recorded hashes, inspect the discrepancy. Do not delete the
manifest, move backups into `Resources/plugins`, or use adoption to bypass it.
Backups and GC roots are intentionally not automatically pruned.
