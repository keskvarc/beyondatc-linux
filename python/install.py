"""Explicit, recoverable installation into the mutable simulator and Wine view."""

from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import uuid

import supervisor


CONFLICTS = (
    "OpenATC/64/lin.xpl",
    "LiveTraffic/lin_x64/LiveTraffic.xpl",
    "xPilot/lin_x64/xPilot.xpl",
)


class InstallError(RuntimeError):
    pass


def digest(path):
    if path.is_symlink() or not path.is_file():
        raise InstallError(f"Expected an ordinary file: {path}")
    return hashlib.sha256(path.read_bytes()).hexdigest()


def safe_directory(path):
    if path.is_symlink() or (path.exists() and not path.is_dir()):
        raise InstallError(f"Refusing non-directory or symlink: {path}")


def atomic_json(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex)
    with temporary.open("x") as output:
        os.chmod(temporary, 0o600)
        json.dump(data, output, indent=2)
        output.write("\n")
        output.flush()
        os.fsync(output.fileno())
    os.replace(temporary, path)
    with directory_fd(path.parent) as fd:
        os.fsync(fd)


@contextmanager
def directory_fd(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        yield fd
    finally:
        os.close(fd)


@contextmanager
def locked(state):
    safe_directory(state)
    state.mkdir(parents=True, exist_ok=True)
    fd = os.open(state / "lock", os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        if not stat.S_ISREG(os.fstat(fd).st_mode):
            raise InstallError("Installation lock is not a regular file")
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise InstallError("Another bridge or installer is active; stop it before configuring") from error
        yield
    finally:
        os.close(fd)


def assert_stopped(sim, acknowledged):
    if not acknowledged:
        raise InstallError("Close X-Plane and BeyondATC, then pass --simulator-stopped")
    pid = supervisor.native_pid((sim / "X-Plane-x86_64").resolve(strict=True))
    if pid is not None:
        raise InstallError(f"X-Plane is running (PID {pid}); no installation changes made")
    for proc in Path("/proc").iterdir():
        if not proc.name.isdecimal():
            continue
        try:
            if proc.stat().st_uid != os.getuid():
                continue
            arguments = (proc / "cmdline").read_bytes().split(b"\0")
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            continue
        if any(a.replace(b"\\", b"/").rsplit(b"/", 1)[-1].lower() == b"beyondatc.exe" for a in arguments):
            raise InstallError(f"BeyondATC is running (PID {proc.name}); close it before configuring")


def register_root(package, root):
    root.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        ["nix-store", "--add-root", str(root), "--indirect", "--realise", str(package)],
        check=True, stdin=subprocess.DEVNULL,
    )


class Installation:
    def __init__(self, sim, prefix, csl, state):
        self.sim = Path(sim).resolve(strict=True)
        self.prefix = Path(prefix).resolve(strict=True)
        self.csl = Path(csl).resolve()
        self.state = Path(state)
        self.plugins = self.sim / "Resources/plugins"
        self.native = self.plugins / "BeyondATC"
        self.view = self.prefix / "drive_c/XPlaneNative"
        self.vendor = self.prefix / "drive_c/BeyondATC/X-Plane Plugin/BeyondATC"
        self.marker = self.state / "installation.json"
        self.pending = self.state / "pending.json"
        self.install_list = supervisor.install_list_path(self.prefix)
        for path in (self.plugins, self.native, self.view, self.state):
            safe_directory(path)
        if self.install_list.is_symlink():
            raise InstallError(f"Refusing symlink install list: {self.install_list}")
        for relative in CONFLICTS:
            parent = (self.plugins / relative).parent
            while parent != self.plugins:
                safe_directory(parent)
                parent = parent.parent

    def read(self, path):
        if not path.exists() and not path.is_symlink():
            return None
        if path.is_symlink():
            raise InstallError(f"Refusing symlink manifest: {path}")
        try:
            data = json.loads(path.read_text())
            if (
                data["schema"] != 1
                or data["sim"] != str(self.sim)
                or data["prefix"] != str(self.prefix)
                or data["phase"] not in ("active", "installing", "restoring", "restored")
                or len(data["token"]) != 32
                or any(c not in "0123456789abcdef" for c in data["token"])
            ):
                raise ValueError("Invalid manifest header")
            expected = {str(self.plugins / name) for name in CONFLICTS}
            for name in ("native_sha256", "companion_sha256"):
                if len(data[name]) != 64 or any(c not in "0123456789abcdef" for c in data[name]):
                    raise ValueError("Invalid payload hash")
            if not Path(data["package"]).is_absolute() or not Path(data["resources"]).is_absolute():
                raise ValueError("Invalid package paths")
            for entry in data["disabled"]:
                original = Path(entry["original"])
                if str(original) not in expected or entry["backup"] not in (
                    str(original) + ".batc-prototype-disabled",
                    str(original) + ".beyondatc-native-disabled",
                ):
                    raise ValueError("Invalid disabled-plugin path")
                if len(entry["sha256"]) != 64 or any(c not in "0123456789abcdef" for c in entry["sha256"]):
                    raise ValueError("Invalid disabled-plugin hash")
                if type(entry["already_disabled"]) is not bool:
                    raise ValueError("Invalid disabled-plugin state")
            if data["phase"] == "installing":
                if set(data["previous_exists"]) != {str(self.native), str(self.view)}:
                    raise ValueError("Invalid previous installation paths")
                if not all(type(v) is bool for v in data["previous_exists"].values()):
                    raise ValueError("Invalid previous installation state")
                if data["previous"] is not None and not isinstance(data["previous"], dict):
                    raise ValueError("Invalid previous manifest")
                if data["install_list_before"] is not None and not isinstance(data["install_list_before"], str):
                    raise ValueError("Invalid previous install list")
            return data
        except (ValueError, KeyError, TypeError) as error:
            raise InstallError(f"Invalid installation record: {path}; inspect it before proceeding") from error

    def backup_paths(self, token):
        return (
            self.sim / "Output/beyondatc-native/backups" / token,
            self.prefix / "drive_c/.beyondatc-native-backups" / token,
        )

    def verify(self, record):
        if record["phase"] != "active":
            raise InstallError("The connector is not configured; run configure first")
        safe_directory(self.native)
        safe_directory(self.view)
        if digest(self.native / "64/lin.xpl") != record["native_sha256"]:
            raise InstallError("Installed native plugin was modified; inspect it before upgrading")
        if digest(self.view / "X-Plane.exe") != record["companion_sha256"]:
            raise InstallError("Installed discovery companion was modified; inspect it before upgrading")
        for name, expected in (("CSL", record["csl"]), ("Resources", record["resources"])):
            path = self.native / name
            if not path.is_symlink() or path.resolve() != Path(expected):
                raise InstallError(f"Installed {name} link changed; refusing replacement")
        for entry in record["disabled"]:
            original = Path(entry["original"])
            if original.exists() or original.is_symlink() or digest(Path(entry["backup"])) != entry["sha256"]:
                raise InstallError(f"Competing plugin changed: {entry['original']}")

    def current(self):
        if self.pending.exists():
            raise InstallError("Interrupted installation; close both apps and rerun configure to recover")
        record = self.read(self.marker)
        if record is None:
            raise InstallError("No managed installation; run configure or explicitly adopt the prototype")
        self.verify(record)
        return record

    def adopt(self, path):
        try:
            data = json.loads(Path(path).read_text())
            if Path(data["plugin_directory"]).resolve() != self.native:
                raise ValueError("Prototype belongs to another simulator")
            if digest(self.native / "64/lin.xpl") != data["native_sha256"]:
                raise ValueError("Prototype native binary does not match its record")
            if digest(self.view / "X-Plane.exe") != data["ptt"]["companion_sha256"]:
                raise ValueError("Prototype companion does not match its record")
            resources = self.native / "Resources"
            expected_resources = Path(path).resolve().parent / "vendor/XPMP2/Resources"
            if not resources.is_symlink() or resources.resolve() != expected_resources:
                raise ValueError("Prototype resources do not match its source")
            if not (self.native / "CSL").is_symlink() or (self.native / "CSL").resolve() != self.csl:
                raise ValueError("Prototype models do not match the configured CSL directory")
            entries = []
            for item in data["disabled"]:
                original = Path(item["original"])
                if (
                    original not in [self.plugins / name for name in CONFLICTS]
                    or item["backup"] != str(original) + ".batc-prototype-disabled"
                    or not item["renamed"]
                    or original.exists()
                    or original.is_symlink()
                ):
                    raise ValueError("Unexpected prototype plugin backup")
                entries.append({
                    "original": str(original), "backup": item["backup"],
                    "sha256": digest(Path(item["backup"])), "already_disabled": True,
                })
            return entries
        except (ValueError, KeyError, TypeError) as error:
            raise InstallError(f"Cannot adopt prototype: {error}") from error

    def restore_disabled(self, entries, rollback=False):
        for entry in entries:
            if rollback and entry["already_disabled"]:
                continue
            original, backup = Path(entry["original"]), Path(entry["backup"])
            original_present = original.exists() or original.is_symlink()
            backup_present = backup.exists() or backup.is_symlink()
            if original_present and backup_present:
                raise InstallError(f"Both original and backup exist; reconcile manually: {original}")
            if backup_present:
                if digest(backup) != entry["sha256"]:
                    raise InstallError(f"Competing-plugin backup changed: {backup}")
                backup.rename(original)
            elif not original_present or digest(original) != entry["sha256"]:
                raise InstallError(f"Neither expected original nor backup is available: {original}")

    def recover(self):
        record = self.read(self.pending)
        if record is None:
            return
        if record["phase"] == "restoring":
            self.finish_restore(record)
            return
        if record["phase"] != "installing":
            raise InstallError("Unexpected pending transaction phase")
        native_backup, wine_backup = self.backup_paths(record["token"])
        for target, backup in (
            (self.native, native_backup / "plugin"),
            (self.view, wine_backup / "view"),
        ):
            if backup.exists():
                if target.exists():
                    target.rename(backup.with_name(backup.name + "-aborted"))
                backup.rename(target)
            elif not record["previous_exists"][str(target)] and target.exists():
                target.rename(backup.with_name(backup.name + "-aborted"))
        self.restore_disabled(record["disabled"], rollback=True)
        if record["install_list_before"] is None:
            if self.install_list.exists():
                self.install_list.unlink()
        else:
            self.install_list.write_text(record["install_list_before"])
        if record["previous"] is None:
            if self.marker.exists():
                self.marker.unlink()
        else:
            atomic_json(self.marker, record["previous"])
        self.pending.unlink()
        print("beyondatc-native: recovered interrupted installation; previous state retained")

    def configure(self, package, acknowledged, upgrade=False, adopt=None):
        assert_stopped(self.sim, acknowledged)
        with locked(self.state):
            self.recover()
            previous = self.read(self.marker)
            package = Path(package).resolve(strict=True)
            native_source = package / "lib/beyondatc-native/64/lin.xpl"
            companion = package / "libexec/beyondatc-native/X-Plane.exe"
            resources = package / "share/beyondatc-native/Resources"
            native_hash, companion_hash = digest(native_source), digest(companion)
            if not resources.is_dir() or not self.vendor.is_dir():
                raise InstallError("Packaged resources or installed BeyondATC Windows plugin are missing")
            if next(self.csl.rglob("xsb_aircraft.txt"), None) is None:
                raise InstallError(f"No CSL model definitions found: {self.csl}")
            disabled = []
            if previous and previous["phase"] == "active":
                if adopt:
                    raise InstallError("An already managed installation cannot adopt another prototype")
                self.verify(previous)
                if previous["package"] == str(package) and previous["csl"] == str(self.csl):
                    print("beyondatc-native: installed package is current")
                    return previous
                if not upgrade:
                    raise InstallError("A new package is available; rerun configure --upgrade with both apps closed")
                disabled = [entry | {"already_disabled": True} for entry in previous["disabled"]]
            elif adopt:
                if previous:
                    raise InstallError("Prototype adoption is only valid before the first managed installation")
                disabled = self.adopt(adopt)
            elif self.native.exists() or self.view.exists():
                raise InstallError("Existing unmanaged connector/view; explicitly use --adopt-prototype")
            recorded = {entry["original"] for entry in disabled}
            for relative in CONFLICTS:
                original = self.plugins / relative
                backup = Path(str(original) + ".beyondatc-native-disabled")
                prototype_backup = Path(str(original) + ".batc-prototype-disabled")
                if str(original) in recorded:
                    continue
                if backup.exists() or backup.is_symlink():
                    raise InstallError(f"Unrecorded disabled plugin: {backup}")
                if prototype_backup.exists() or prototype_backup.is_symlink():
                    raise InstallError(f"Unrecorded disabled plugin: {prototype_backup}")
                if original.exists() or original.is_symlink():
                    disabled.append({
                        "original": str(original), "backup": str(backup),
                        "sha256": digest(original), "already_disabled": False,
                    })
            token = uuid.uuid4().hex
            native_backup, wine_backup = self.backup_paths(token)
            native_backup.mkdir(parents=True)
            wine_backup.mkdir(parents=True)
            root = self.state / "roots" / token
            register_root(package, root)
            record = {
                "schema": 1, "phase": "installing", "token": token,
                "sim": str(self.sim), "prefix": str(self.prefix),
                "package": str(package), "gc_root": str(root), "csl": str(self.csl),
                "resources": str(resources),
                "native_sha256": native_hash, "companion_sha256": companion_hash,
                "disabled": disabled, "previous": previous,
                "previous_exists": {str(self.native): self.native.exists(), str(self.view): self.view.exists()},
                "install_list_before": self.install_list.read_text() if self.install_list.exists() else None,
            }
            staged = native_backup / "new"
            (staged / "64").mkdir(parents=True)
            shutil.copy2(native_source, staged / "64/lin.xpl")
            (staged / "Resources").symlink_to(resources)
            (staged / "CSL").symlink_to(self.csl)
            atomic_json(self.pending, record)
            try:
                assert_stopped(self.sim, acknowledged)
                if self.native.exists():
                    self.native.rename(native_backup / "plugin")
                staged.rename(self.native)
                if self.view.exists():
                    self.view.rename(wine_backup / "view")
                for entry in disabled:
                    if not entry["already_disabled"]:
                        Path(entry["original"]).rename(entry["backup"])
                supervisor.prepare_view(self.sim, self.prefix, companion, self.vendor)
                record["phase"] = "active"
                record.pop("previous")
                record.pop("previous_exists")
                record.pop("install_list_before")
                atomic_json(self.marker, record)
                self.pending.unlink()
            except (OSError, RuntimeError):
                self.recover()
                raise
            print(f"beyondatc-native: configured; recovery copies retained under {native_backup} and {wine_backup}")
            return record

    def finish_restore(self, record):
        native_backup, wine_backup = self.backup_paths(record["token"])
        for target, destination in (
            (self.native, native_backup / "uninstalled-plugin"),
            (self.view, wine_backup / "uninstalled-view"),
        ):
            if target.exists():
                if destination.exists():
                    raise InstallError(f"Both active and saved paths exist: {target}")
                target.rename(destination)
        self.restore_disabled(record["disabled"])
        if self.install_list.exists():
            lines = self.install_list.read_text().splitlines()
            self.install_list.write_text("".join(line + "\n" for line in lines if line != "C:\\XPlaneNative"))
        record["phase"] = "restored"
        atomic_json(self.marker, record)
        self.pending.unlink()
        print("beyondatc-native: connector withdrawn and competing plugins restored; all payloads retained")

    def restore(self, acknowledged):
        assert_stopped(self.sim, acknowledged)
        with locked(self.state):
            self.recover()
            record = self.read(self.marker)
            if record and record["phase"] == "restored":
                print("beyondatc-native: last managed installation is already restored; no files changed")
                return
            if record is None or record["phase"] != "active":
                raise InstallError("No active managed installation to restore")
            self.verify(record)
            record["phase"] = "restoring"
            atomic_json(self.pending, record)
            self.finish_restore(record)
