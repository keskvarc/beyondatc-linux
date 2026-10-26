#!/usr/bin/env python3
import argparse
import os
from pathlib import Path
import pwd
import shutil
import signal
import socket
import subprocess
import threading
import time


def link(source, target):
    if target.is_symlink():
        if target.resolve() != source.resolve():
            raise RuntimeError(f"Refusing to replace an unrelated symlink: {target}")
    elif target.exists():
        raise RuntimeError(f"Refusing to replace an existing path: {target}")
    else:
        target.symlink_to(source)


def mirror(source, target, exclude=()):
    target.mkdir(parents=True, exist_ok=True)
    for entry in source.iterdir():
        if entry.name not in exclude:
            link(entry, target / entry.name)


def install_list_path(prefix):
    user = pwd.getpwuid(os.getuid()).pw_name
    return prefix / "drive_c/users" / user / "AppData/Local/x-plane_install_12.txt"


def prepare_view(sim, prefix, companion, vendor_plugin):
    view = prefix / "drive_c" / "XPlaneNative"
    mirror(sim, view, ("Resources", "X-Plane.exe"))
    mirror(sim / "Resources", view / "Resources", ("plugins",))
    plugins = view / "Resources" / "plugins"
    mirror(sim / "Resources" / "plugins", plugins, ("BeyondATC",))
    batc = plugins / "BeyondATC"
    batc.mkdir(exist_ok=True)
    for entry in vendor_plugin.iterdir():
        if entry.name != "CSL":
            link(entry, batc / entry.name)
    link(sim / "Resources" / "plugins" / "BeyondATC" / "CSL", batc / "CSL")
    executable = view / "X-Plane.exe"
    if executable.exists() and executable.read_bytes() != companion.read_bytes():
        raise RuntimeError(f"Refusing to overwrite a different executable: {executable}")
    if not executable.exists():
        shutil.copy2(companion, executable)
    install_list = install_list_path(prefix)
    previous = install_list.read_text() if install_list.exists() else ""
    root = "C:\\XPlaneNative"
    if root not in previous.splitlines():
        if previous:
            backup = install_list.with_name(install_list.name + ".before-beyondatc-native")
            if not backup.exists():
                shutil.copy2(install_list, backup)
        install_list.parent.mkdir(parents=True, exist_ok=True)
        install_list.write_text(previous.rstrip("\r\n") + ("\n" if previous else "") + root + "\n")
    return view


def native_pid(executable):
    for proc in Path("/proc").iterdir():
        if not proc.name.isdecimal():
            continue
        try:
            if (proc / "exe").readlink() == executable:
                return int(proc.name)
        except (FileNotFoundError, ProcessLookupError):
            continue
        except PermissionError:
            # Other users' processes are neither readable nor candidates.
            continue
    return None


def owns_plugin_socket(pid, port=49100):
    proc = Path("/proc") / str(pid)
    try:
        inodes = set()
        for fd in (proc / "fd").iterdir():
            try:
                name = fd.readlink().as_posix()
            except FileNotFoundError:
                continue
            if name.startswith("socket:["):
                inodes.add(name[8:-1])
        for line in (proc / "net" / "udp").read_text().splitlines()[1:]:
            fields = line.split()
            if fields[1] == f"0100007F:{port:04X}" and fields[9] in inodes:
                return True
    except (FileNotFoundError, ProcessLookupError):
        return False
    return False


class PttState:
    def __init__(self):
        self.pressed = False
        self.updated = None

    def receive(self, packet, peer, now):
        if peer != ("127.0.0.1", 49100):
            return False
        if packet not in (b"BPTT\0", b"BPTT\1"):
            raise RuntimeError("Invalid PTT packet from the native plugin")
        self.pressed = packet[-1] == 1
        self.updated = now
        return True

    def command(self, now):
        return bytes([int(self.pressed and self.updated is not None and now - self.updated < 0.3)])


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--sim-root", type=Path, required=True)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--wine", type=Path, required=True)
    parser.add_argument("--companion", type=Path, required=True)
    parser.add_argument("--vendor-plugin", type=Path, required=True)
    parser.add_argument("--prepare-only", action="store_true")
    args = parser.parse_args(argv)
    sim = args.sim_root.resolve(strict=True)
    executable = (sim / "X-Plane-x86_64").resolve(strict=True)
    view = prepare_view(sim, args.prefix, args.companion, args.vendor_plugin)
    print(f"Wine-facing simulator view: {view}", flush=True)
    if args.prepare_only:
        return
    stop = threading.Event()
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda _sig, _frame: stop.set())
    env = os.environ | {
        "WINEPREFIX": str(args.prefix),
        "WINEDEBUG": "-all",
        "WINEDLLOVERRIDES": "winemenubuilder.exe=d",
    }
    print("Waiting for the actual Linux X-Plane process and its native plugin socket.", flush=True)
    while not stop.is_set():
        pid = native_pid(executable)
        if pid is None or not owns_plugin_socket(pid):
            stop.wait(0.5)
            continue
        with socket.socket() as listener, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as ptt_socket:
            ptt_socket.bind(("127.0.0.1", 49102))
            ptt_socket.setblocking(False)
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            listener.settimeout(10)
            with (view / "discovery.log").open("a") as output:
                child = subprocess.Popen(
                    [str(args.wine), "C:\\XPlaneNative\\X-Plane.exe", str(listener.getsockname()[1])],
                    cwd=view, env=env, stdout=output, stderr=subprocess.STDOUT,
                )
                try:
                    connection, _ = listener.accept()
                    connection.settimeout(1)
                    ptt = PttState()
                    print(f"Advertising real Linux X-Plane PID {pid}; discovery companion PID {child.pid}.", flush=True)
                    with connection:
                        while not stop.is_set() and native_pid(executable) == pid and owns_plugin_socket(pid):
                            if child.poll() is not None:
                                raise RuntimeError(f"Discovery companion exited unexpectedly: {child.returncode}")
                            for _ in range(256):
                                try:
                                    packet, peer = ptt_socket.recvfrom(64)
                                except BlockingIOError:
                                    break
                                if not ptt.receive(packet, peer, time.monotonic()):
                                    print(f"Rejected PTT packet from unexpected peer {peer}.", flush=True)
                            connection.sendall(ptt.command(time.monotonic()))
                            stop.wait(0.05)
                    print("Real simulator/plugin stopped; withdrawing Windows discovery.", flush=True)
                finally:
                    try:
                        child.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        child.terminate()
                        child.wait(timeout=5)
        stop.wait(1)


if __name__ == "__main__":
    main()
