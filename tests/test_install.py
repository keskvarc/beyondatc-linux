import json
import io
import os
from pathlib import Path
import runpy
import subprocess
import tempfile
import unittest
from contextlib import redirect_stderr
from unittest.mock import patch

import install
import supervisor


class InstallationTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.sim = self.root / "X-Plane"
        self.prefix = self.root / "wine"
        self.csl = self.root / "models"
        self.state = self.root / "state"
        self.sim.mkdir()
        (self.sim / "X-Plane-x86_64").write_bytes(b"native simulator")
        (self.sim / "Resources/plugins").mkdir(parents=True)
        (self.csl / "BB_Airbus").mkdir(parents=True)
        (self.csl / "BB_Airbus/xsb_aircraft.txt").write_text("fixture models")
        vendor = self.prefix / "drive_c/BeyondATC/X-Plane Plugin/BeyondATC/win_x64"
        vendor.mkdir(parents=True)
        (vendor / "BeyondATC.xpl").write_bytes(b"unmodified vendor plugin")
        (self.prefix / "drive_c/BeyondATC/BeyondATC.exe").write_bytes(b"unmodified app")
        self.package = self.make_package("package-v1")
        for relative in install.CONFLICTS:
            path = self.sim / "Resources/plugins" / relative
            path.parent.mkdir(parents=True)
            path.write_bytes(relative.encode())
        self.instance = install.Installation(self.sim, self.prefix, self.csl, self.state)
        self.stop_check = patch("install.assert_stopped")
        self.stop_check.start()
        self.addCleanup(self.stop_check.stop)
        root_registration = patch("install.register_root", side_effect=self.register_root)
        root_registration.start()
        self.addCleanup(root_registration.stop)

    def register_root(self, package, root):
        root.parent.mkdir(parents=True, exist_ok=True)
        root.symlink_to(package)

    def make_package(self, name):
        package = self.root / name
        for relative in ("lib/beyondatc-native/64/lin.xpl", "libexec/beyondatc-native/X-Plane.exe"):
            path = package / relative
            path.parent.mkdir(parents=True)
            path.write_bytes((name + relative).encode())
        (package / "share/beyondatc-native/Resources").mkdir(parents=True)
        return package

    def check_disabled(self):
        for relative in install.CONFLICTS:
            original = self.sim / "Resources/plugins" / relative
            self.assertFalse(original.exists())
            self.assertEqual(Path(str(original) + ".beyondatc-native-disabled").read_bytes(), relative.encode())

    def check_restored(self):
        for relative in install.CONFLICTS:
            self.assertEqual((self.sim / "Resources/plugins" / relative).read_bytes(), relative.encode())

    def test_install_reuse_restore_and_retained_payloads(self):
        record = self.instance.configure(self.package, True)
        self.instance.verify(record)
        self.check_disabled()
        self.assertEqual(self.instance.configure(self.package, True)["token"], record["token"])
        self.assertEqual(
            (self.prefix / "drive_c/BeyondATC/BeyondATC.exe").read_bytes(), b"unmodified app"
        )
        self.instance.restore(True)
        self.check_restored()
        self.assertFalse(self.instance.native.exists())
        self.assertFalse(self.instance.view.exists())
        backup, wine_backup = self.instance.backup_paths(record["token"])
        self.assertTrue((backup / "uninstalled-plugin/64/lin.xpl").is_file())
        self.assertTrue((wine_backup / "uninstalled-view/X-Plane.exe").is_file())
        self.assertNotIn("C:\\XPlaneNative", self.instance.install_list.read_text())
        self.assertEqual(self.instance.read(self.instance.marker)["phase"], "restored")
        self.instance.restore(True)
        self.check_restored()

    def test_upgrade_is_explicit_and_keeps_previous_payload(self):
        first = self.instance.configure(self.package, True)
        newer = self.make_package("package-v2")
        with self.assertRaisesRegex(install.InstallError, "configure --upgrade"):
            self.instance.configure(newer, True)
        second = self.instance.configure(newer, True, upgrade=True)
        self.instance.verify(second)
        native_backup, wine_backup = self.instance.backup_paths(second["token"])
        self.assertEqual(install.digest(native_backup / "plugin/64/lin.xpl"), first["native_sha256"])
        self.assertEqual(install.digest(wine_backup / "view/X-Plane.exe"), first["companion_sha256"])
        self.check_disabled()

    def test_failure_rolls_back_original_files_and_install_list(self):
        self.instance.install_list.parent.mkdir(parents=True)
        self.instance.install_list.write_text("D:\\OtherSimulator\n")
        with patch("supervisor.prepare_view", side_effect=RuntimeError("view failed")):
            with self.assertRaisesRegex(RuntimeError, "view failed"):
                self.instance.configure(self.package, True)
        self.check_restored()
        self.assertFalse(self.instance.native.exists())
        self.assertFalse(self.instance.view.exists())
        self.assertFalse(self.instance.pending.exists())
        self.assertEqual(self.instance.install_list.read_text(), "D:\\OtherSimulator\n")

    def test_failed_upgrade_recovers_previous_managed_installation(self):
        first = self.instance.configure(self.package, True)
        newer = self.make_package("package-v2")
        with patch("supervisor.prepare_view", side_effect=OSError("interrupted view")):
            with self.assertRaisesRegex(OSError, "interrupted view"):
                self.instance.configure(newer, True, upgrade=True)
        recovered = self.instance.read(self.instance.marker)
        self.assertEqual(recovered["token"], first["token"])
        self.instance.verify(recovered)
        self.check_disabled()

    def test_interrupted_upgrade_recovers_on_next_configure(self):
        first = self.instance.configure(self.package, True)
        newer = self.make_package("package-v2")
        with patch("supervisor.prepare_view", side_effect=KeyboardInterrupt):
            with self.assertRaises(KeyboardInterrupt):
                self.instance.configure(newer, True, upgrade=True)
        self.assertTrue(self.instance.pending.is_file())
        record = self.instance.configure(self.package, True)
        self.assertEqual(record["token"], first["token"])
        self.instance.verify(record)
        self.check_disabled()

    def test_interrupted_restore_completes_on_recovery(self):
        self.instance.configure(self.package, True)
        restore_disabled = self.instance.restore_disabled

        def interrupt(entries, rollback=False):
            restore_disabled(entries[:1], rollback)
            raise KeyboardInterrupt

        with patch.object(self.instance, "restore_disabled", side_effect=interrupt):
            with self.assertRaises(KeyboardInterrupt):
                self.instance.restore(True)
        with install.locked(self.state):
            self.instance.recover()
        self.check_restored()
        self.assertEqual(self.instance.read(self.instance.marker)["phase"], "restored")
        self.assertFalse(self.instance.pending.exists())

    def test_modified_payload_and_backup_are_not_overwritten(self):
        record = self.instance.configure(self.package, True)
        (self.instance.native / "64/lin.xpl").write_bytes(b"user edit")
        with self.assertRaisesRegex(install.InstallError, "modified"):
            self.instance.restore(True)
        self.assertEqual((self.instance.native / "64/lin.xpl").read_bytes(), b"user edit")
        (self.instance.native / "64/lin.xpl").write_bytes(
            (self.package / "lib/beyondatc-native/64/lin.xpl").read_bytes()
        )
        Path(record["disabled"][0]["backup"]).write_bytes(b"changed backup")
        with self.assertRaisesRegex(install.InstallError, "Competing plugin changed"):
            self.instance.restore(True)
        self.assertTrue(self.instance.native.is_dir())

    def test_unknown_installation_and_symlink_parent_are_refused(self):
        self.instance.native.mkdir()
        (self.instance.native / "unrelated").write_text("preserve")
        with self.assertRaisesRegex(install.InstallError, "unmanaged"):
            self.instance.configure(self.package, True)
        self.assertEqual((self.instance.native / "unrelated").read_text(), "preserve")
        parent = self.sim / "Resources/plugins/OpenATC/64"
        parent.rename(parent.with_name("saved"))
        parent.symlink_to(parent.with_name("saved"))
        with self.assertRaisesRegex(install.InstallError, "symlink"):
            install.Installation(self.sim, self.prefix, self.csl, self.state)

    def test_broken_user_symlink_is_not_replaced_on_restore(self):
        record = self.instance.configure(self.package, True)
        original = Path(record["disabled"][0]["original"])
        original.symlink_to(self.root / "missing-user-file")
        with self.assertRaisesRegex(install.InstallError, "Competing plugin changed"):
            self.instance.restore(True)
        self.assertTrue(original.is_symlink())
        self.assertTrue(Path(record["disabled"][0]["backup"]).is_file())

    def test_unrecorded_prototype_backup_is_not_ignored(self):
        original = self.sim / "Resources/plugins" / install.CONFLICTS[0]
        original.rename(Path(str(original) + ".batc-prototype-disabled"))
        with self.assertRaisesRegex(install.InstallError, "Unrecorded disabled plugin"):
            self.instance.configure(self.package, True)
        self.assertFalse(self.instance.native.exists())

    def test_malformed_manifest_and_paths_are_refused(self):
        record = self.instance.configure(self.package, True)
        for change in (
            {"token": "../escape"},
            {"disabled": [record["disabled"][0] | {"original": "/tmp/unrelated"}]},
            {"native_sha256": "invalid"},
        ):
            install.atomic_json(self.instance.marker, record | change)
            with self.assertRaisesRegex(install.InstallError, "Invalid installation record"):
                self.instance.read(self.instance.marker)

    def test_lock_excludes_installer_and_supervisor(self):
        with install.locked(self.state):
            with self.assertRaisesRegex(install.InstallError, "Another bridge"):
                with install.locked(self.state):
                    self.fail("second lock was acquired")

    def cli(self, action):
        self.addCleanup(os.chdir, Path.cwd())
        main = runpy.run_path(str(Path(__file__).resolve().parents[1] / "python/beyondatc-native.py"))["main"]
        with patch.dict(os.environ, {
            "BATC_NATIVE_PACKAGE": str(self.package), "BATC_NATIVE_WINE": "/test/runtime/wine",
        }):
            return main([
                "--sim-root", str(self.sim), "--prefix", str(self.prefix),
                "--csl", str(self.csl), "--state-dir", str(self.state), action,
            ])

    def test_launcher_checks_service_and_preserves_private_prefix(self):
        self.instance.configure(self.package, True)
        with patch("subprocess.run") as service, patch("os.execve") as execute:
            self.assertEqual(self.cli("launch"), 0)
        self.assertEqual(service.call_count, 2)
        self.assertEqual(service.call_args.args[0], [
            "systemctl", "--user", "is-active", "--quiet", "beyondatc-native.service",
        ])
        executable, arguments, environment = execute.call_args.args
        self.assertEqual(str(executable), "/test/runtime/wine")
        self.assertEqual(arguments[-1], str(self.prefix / "drive_c/BeyondATC/BeyondATC.exe"))
        self.assertEqual(environment["WINEPREFIX"], str(self.prefix))
        self.assertIn("winemenubuilder.exe=d", environment["WINEDLLOVERRIDES"])

    def test_launcher_does_not_open_app_when_service_is_skipped(self):
        self.instance.configure(self.package, True)
        with (
            patch("subprocess.run", side_effect=[None, subprocess.CalledProcessError(3, "systemctl")]),
            patch("os.execve") as execute,
            redirect_stderr(io.StringIO()) as errors,
        ):
            self.assertEqual(self.cli("launch"), 1)
        execute.assert_not_called()
        self.assertIn("systemctl", errors.getvalue())

    def test_check_rejects_interrupted_and_restored_installations(self):
        self.instance.configure(self.package, True)
        self.assertEqual(self.cli("check"), 0)
        self.instance.pending.write_text("incomplete journal")
        with redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(self.cli("check"), 1)
        self.assertIn("Interrupted installation", errors.getvalue())
        self.instance.pending.unlink()
        self.instance.restore(True)
        with redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(self.cli("check"), 1)
        self.assertIn("not configured", errors.getvalue())

    def test_prototype_adoption_requires_exact_files_and_models(self):
        prototype = self.root / "prototype"
        resources = prototype / "vendor/XPMP2/Resources"
        resources.mkdir(parents=True)
        (self.instance.native / "64").mkdir(parents=True)
        (self.instance.native / "64/lin.xpl").write_bytes(b"prototype")
        (self.instance.native / "Resources").symlink_to(resources)
        (self.instance.native / "CSL").symlink_to(self.csl)
        self.instance.view.mkdir()
        (self.instance.view / "X-Plane.exe").write_bytes(b"old companion")
        disabled = []
        for relative in install.CONFLICTS:
            original = self.sim / "Resources/plugins" / relative
            backup = Path(str(original) + ".batc-prototype-disabled")
            original.rename(backup)
            disabled.append({"original": str(original), "backup": str(backup), "renamed": True})
        manifest = prototype / "installation.json"
        manifest.write_text(json.dumps({
            "plugin_directory": str(self.instance.native),
            "native_sha256": install.digest(self.instance.native / "64/lin.xpl"),
            "ptt": {"companion_sha256": install.digest(self.instance.view / "X-Plane.exe")},
            "disabled": disabled,
        }))
        record = self.instance.configure(self.package, True, adopt=manifest)
        self.instance.verify(record)
        self.assertEqual((self.instance.native / "Resources").resolve(), self.package / "share/beyondatc-native/Resources")
        self.instance.restore(True)
        self.check_restored()


class StopGuardTest(unittest.TestCase):
    def test_acknowledgement_and_process_guard(self):
        with self.assertRaisesRegex(install.InstallError, "simulator-stopped"):
            install.assert_stopped(Path("/unused"), False)
        with tempfile.TemporaryDirectory() as directory:
            sim = Path(directory)
            (sim / "X-Plane-x86_64").write_bytes(b"fixture")
            with patch.object(supervisor, "native_pid", return_value=1234):
                with self.assertRaisesRegex(install.InstallError, "PID 1234"):
                    install.assert_stopped(sim, True)


if __name__ == "__main__":
    unittest.main()
