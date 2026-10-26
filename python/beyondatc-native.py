#!/usr/bin/env python3
"""Run the paid Windows application against a managed native connector."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

from install import Installation, InstallError, locked
import supervisor


def settings(args):
    config_path = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "beyondatc-native/config.json"
    config = json.loads(config_path.read_text()) if config_path.exists() else {}
    if not isinstance(config, dict) or any(
        not isinstance(config[key], str) for key in ("simRoot", "cslRoot", "prefix") if key in config
    ):
        raise InstallError(f"Invalid path configuration: {config_path}")
    sim = args.sim_root or config.get("simRoot")
    csl = args.csl or config.get("cslRoot")
    prefix = args.prefix or config.get("prefix") or (
        Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share")) / "beyondatc/wine"
    )
    state = args.state_dir or (
        Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local/state")) / "beyondatc-native"
    )
    if not sim or not csl:
        raise InstallError("Set simRoot/cslRoot in beyondatc-native/config.json or pass --sim-root and --csl")
    if not all(Path(p).is_absolute() for p in (sim, csl, prefix, state)):
        raise InstallError("Simulator, CSL, prefix and state paths must be absolute")
    return Installation(sim, prefix, csl, state)


def wine_environment(prefix):
    overrides = os.environ.get("WINEDLLOVERRIDES", "")
    return os.environ | {
        "WINEPREFIX": str(prefix),
        "WINEDEBUG": os.environ.get("WINEDEBUG", "-fixme"),
        "WINEDLLOVERRIDES": (overrides + ";" if overrides else "") + "winemenubuilder.exe=d",
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sim-root", type=Path)
    parser.add_argument("--csl", type=Path)
    parser.add_argument("--prefix", type=Path)
    parser.add_argument("--state-dir", type=Path)
    sub = parser.add_subparsers(dest="action", required=True)
    configure = sub.add_parser("configure", help="Install/adopt the connector with both applications closed")
    configure.add_argument("--simulator-stopped", action="store_true")
    configure.add_argument("--upgrade", action="store_true")
    configure.add_argument("--adopt-prototype", type=Path)
    restore = sub.add_parser("restore", help="Withdraw the connector and re-enable competing plugins")
    restore.add_argument("--simulator-stopped", action="store_true")
    sub.add_parser("supervise", help="Watch native X-Plane and relay discovery/PTT without launching BeyondATC")
    sub.add_parser("launch", help="Start the bridge user service and the official BeyondATC application")
    sub.add_parser("status", help="Report installed payload, native simulator and user-service state")
    sub.add_parser("check", help="Verify the configured payload without starting anything")
    args = parser.parse_args(argv)
    try:
        installation = settings(args)
        package = Path(os.environ["BATC_NATIVE_PACKAGE"])
        wine = Path(os.environ["BATC_NATIVE_WINE"])
        if args.action == "configure":
            installation.configure(package, args.simulator_stopped, args.upgrade, args.adopt_prototype)
        elif args.action == "restore":
            installation.restore(args.simulator_stopped)
        elif args.action == "check":
            installation.current()
        elif args.action == "status":
            record = installation.current()
            pid = supervisor.native_pid(installation.sim / "X-Plane-x86_64")
            print(f"Installed package: {record['package']}")
            print(f"Available package: {package}")
            print(f"Native simulator PID: {pid if pid is not None else 'not running'}")
            print(f"Native command socket owned: {bool(pid and supervisor.owns_plugin_socket(pid))}")
            subprocess.run(["systemctl", "--user", "--no-pager", "--lines=0", "status", "beyondatc-native.service"], check=True)
        elif args.action == "supervise":
            with locked(installation.state):
                record = installation.current()
                companion = Path(record["package"]) / "libexec/beyondatc-native/X-Plane.exe"
                supervisor.main([
                    "--sim-root", str(installation.sim), "--prefix", str(installation.prefix),
                    "--wine", str(wine), "--companion", str(companion),
                    "--vendor-plugin", str(installation.vendor),
                ])
        else:
            installation.current()
            executable = installation.prefix / "drive_c/BeyondATC/BeyondATC.exe"
            if not executable.is_file():
                raise InstallError(f"Install the official BeyondATC application first: {executable}")
            subprocess.run(["systemctl", "--user", "start", "beyondatc-native.service"], check=True)
            subprocess.run(["systemctl", "--user", "is-active", "--quiet", "beyondatc-native.service"], check=True)
            installation.current()
            os.chdir(executable.parent)
            os.execve(wine, [str(wine), str(executable)], wine_environment(installation.prefix))
    except (InstallError, OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"beyondatc-native: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
