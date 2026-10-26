import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import unittest


@unittest.skipUnless(os.environ.get("BATC_TEST_SHIM"), "Wine integration runs in the package check")
class WinePttTest(unittest.TestCase):
    def run_bridge(self, release, watchdog=False):
        wine = Path(os.environ["BATC_TEST_WINE"])
        helper = Path(os.environ["BATC_TEST_SHIM"])
        # Wine's background processes can inherit stdout; wait only for the probe.
        with tempfile.TemporaryFile(mode="w+") as log, socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            listener.settimeout(15)
            process = subprocess.Popen(
                [str(wine), str(helper), str(listener.getsockname()[1])],
                stdout=log, stderr=subprocess.STDOUT,
            )
            try:
                connection, _ = listener.accept()
                with connection:
                    connection.sendall(b"\1")
                    time.sleep(0.15)
                    if release:
                        connection.sendall(b"\0")
                        time.sleep(0.1)
                    if not watchdog:
                        connection.shutdown(socket.SHUT_WR)
                    process.wait(timeout=5)
                    log.seek(0)
                    output = log.read()
                self.assertEqual(process.returncode, 7 if watchdog else 0, output)
                self.assertIn("key=135 down, observed=1", output)
                self.assertIn("key=135 released, observed=0", output)
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)

    def test_press_and_release(self):
        self.run_bridge(release=True)

    def test_disconnect_releases_held_key(self):
        self.run_bridge(release=False)

    def test_stalled_supervisor_releases_held_key(self):
        self.run_bridge(release=False, watchdog=True)


if __name__ == "__main__":
    unittest.main()
