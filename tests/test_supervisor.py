import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import unittest

import supervisor


class SupervisorTest(unittest.TestCase):
    def test_ptt_press_hold_release_and_timeout(self):
        ptt = supervisor.PttState()
        peer = ("127.0.0.1", 49100)
        self.assertEqual(ptt.command(10), b"\0")
        self.assertTrue(ptt.receive(b"BPTT\1", peer, 10))
        self.assertEqual(ptt.command(10.05), b"\1")
        self.assertTrue(ptt.receive(b"BPTT\1", peer, 10.2))
        self.assertEqual(ptt.command(10.4), b"\1")
        self.assertEqual(ptt.command(10.51), b"\0")
        self.assertTrue(ptt.receive(b"BPTT\1", peer, 11))
        self.assertTrue(ptt.receive(b"BPTT\0", peer, 11.1))
        self.assertEqual(ptt.command(11.1), b"\0")

    def test_ptt_rejects_other_sources_and_malformed_packets(self):
        ptt = supervisor.PttState()
        for peer in (("127.0.0.1", 49103), ("127.0.0.2", 49100), ("192.0.2.1", 49100)):
            self.assertFalse(ptt.receive(b"BPTT\1", peer, 10))
        self.assertEqual(ptt.command(10), b"\0")
        for packet in (b"", b"BPTT", b"BPTT\2", b"BPTT\1\0", b"BATC\1"):
            with self.assertRaisesRegex(RuntimeError, "Invalid PTT packet"):
                ptt.receive(packet, ("127.0.0.1", 49100), 10)
        self.assertEqual(ptt.command(10), b"\0")

    def test_links_are_idempotent_and_refuse_replacement(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source"
            source.write_text("unchanged")
            target = root / "target"
            supervisor.link(source, target)
            supervisor.link(source, target)
            self.assertEqual(target.read_text(), "unchanged")
            with self.assertRaises(RuntimeError):
                supervisor.link(root / "different", target)
            self.assertEqual(source.read_text(), "unchanged")

    def test_socket_ownership_and_wrong_process(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            udp.bind(("127.0.0.1", 0))
            port = udp.getsockname()[1]
            self.assertTrue(supervisor.owns_plugin_socket(os.getpid(), port))
            child = subprocess.Popen(["sleep", "10"])
            try:
                self.assertFalse(supervisor.owns_plugin_socket(child.pid, port))
            finally:
                child.terminate()
                child.wait(timeout=5)
        self.assertFalse(supervisor.owns_plugin_socket(os.getpid(), port))

    def test_native_pid_uses_executable_not_process_name(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "native-simulator-test"
            shutil.copy2(shutil.which("sleep"), executable)
            self.assertIsNone(supervisor.native_pid(executable))
            child = subprocess.Popen(["sleep", "10"], executable=executable)
            try:
                self.assertEqual(supervisor.native_pid(executable), child.pid)
            finally:
                child.terminate()
                child.wait(timeout=5)
        self.assertIsNone(supervisor.native_pid(Path("/no/such/executable")))

    def test_wildcard_udp_bind_is_not_the_plugin(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            udp.bind(("0.0.0.0", 0))
            self.assertFalse(supervisor.owns_plugin_socket(os.getpid(), udp.getsockname()[1]))


if __name__ == "__main__":
    unittest.main()
