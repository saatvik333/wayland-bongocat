#!/usr/bin/env python3
"""Verify automatic monitor selection against actual Wayland events."""
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import threading
import time

binary = str(Path("build/bongocat").resolve())
fixture = str(Path("build/compositor/server").resolve())


def wait_for(condition):
    deadline = time.monotonic() + 4
    while time.monotonic() < deadline:
        if condition():
            return
        time.sleep(0.02)
    raise AssertionError("focus test timed out")


def workspaces(focused):
    # The escaped name must not be mistaken for workspace fields.
    return {"WorkspacesChanged": {"workspaces": [
        {"id": 1, "idx": 1, "name": None, "output": "TEST-1",
         "is_active": True, "is_focused": focused == 1,
         "active_window_id": None},
        {"id": 2, "idx": 1, "name": 'x"id":1,"output":"TEST-1"}{[',
         "output": "TEST-2", "is_active": True, "is_focused": focused == 2,
         "active_window_id": None},
    ]}}


class FakeNiri:
    """niri event stream whose workspaces have no windows."""

    def __init__(self, path):
        self.focused = 1
        self.connection = None
        self.connections = 0
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.listener.bind(str(path))
        self.listener.listen()
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        while True:
            try:
                connection, _ = self.listener.accept()
            except OSError:
                return
            if connection.makefile("rb").readline() != b'"EventStream"\n':
                connection.close()
                continue
            # Lines larger than the client buffer must be skipped intact.
            connection.sendall(b'{"Ok":"Handled"}\n{"WindowsChanged":'
                               b'{"windows":["' + b"x" * 70000 + b'"]}}\n')
            # Publish the connection only once the initial state is complete.
            self.write(connection, workspaces(self.focused))
            self.connection = connection
            self.connections += 1

    @staticmethod
    def write(connection, event):
        data = (json.dumps(event, separators=(",", ":")) + "\n").encode()
        half = len(data) // 2
        connection.sendall(data[:half])
        time.sleep(0.05)  # Exercise events split across reads.
        connection.sendall(data[half:])

    def send(self, event):
        self.write(self.connection, event)

    def focus(self, workspace):
        self.focused = workspace
        self.send({"WorkspaceActivated": {"id": workspace, "focused": True}})

    def close(self):
        self.listener.close()
        if self.connection:
            self.connection.close()


def run_test(protocol=True, forced=False, niri=False):
    with tempfile.TemporaryDirectory(prefix="bongocat-focus-") as directory:
        root = Path(directory)
        env = dict(os.environ, XDG_RUNTIME_DIR=directory,
                   WAYLAND_DISPLAY="wayland-test", BONGOCAT_TEST_FOCUS="1")
        env.pop("NIRI_SOCKET", None)
        fake_niri = None
        if niri:
            env["NIRI_SOCKET"] = str(root / "niri.sock")
            fake_niri = FakeNiri(root / "niri.sock")
        if not protocol:
            env["BONGOCAT_TEST_NO_TOPLEVEL"] = "1"
        config = root / "cat.conf"
        log_path = root / "server.log"

        def configure(extra=""):
            config.write_text(
                "keyboard_device=/dev/input/event-bongocat-test-missing\n"
                "overlay_opacity=0\nfps=60\n" + extra +
                "[monitor:TEST-2]\noverlay_height=80\n")

        def command(name):
            result = subprocess.run([binary, "--" + name], env=env,
                                    capture_output=True, text=True, timeout=3)
            assert result.returncode == 0, result.stderr
            return result.stdout

        def text():
            return log_path.read_text()

        def mapped():
            monitors = set()
            for line in text().splitlines():
                if line.startswith("overlay "):
                    monitor = line.split()[1]
                    assert monitor not in monitors, line
                    monitors.add(monitor)
                elif line.startswith("destroy "):
                    monitors.discard(line.split()[1])
                assert len(monitors) <= 1, monitors
            return monitors

        def expect(monitor):
            wait_for(lambda: mapped() == {monitor})
            wait_for(lambda: "commit " + monitor in
                     text().rsplit("overlay " + monitor, 1)[-1])
            assert app.poll() is None

        def unchanged(monitor):
            time.sleep(0.15)
            expect(monitor)

        def step(phase):
            server.send_signal(signal.SIGUSR1)
            wait_for(lambda: f"focus-phase {phase}\n" in text())

        configure()
        app = None
        with log_path.open("w") as log, (root / "app.log").open("w") as app_log:
            server = subprocess.Popen([fixture], env=env, stdout=log, stderr=log)
            try:
                wait_for(lambda: (root / "wayland-test").exists())
                args = [binary, "-c", str(config)]
                if forced:
                    args += ["--monitor", "TEST-1"]
                app = subprocess.Popen(args, env=env,
                                       stdout=app_log, stderr=app_log)
                wait_for(lambda: (root / "bongocat.sock").exists())
                expect("TEST-1")
                if forced:
                    step(1)
                    step(2)
                    unchanged("TEST-1")
                    configure("monitor=TEST-2\n")
                    command("reload")
                    unchanged("TEST-1")
                elif protocol:
                    count = text().count("overlay ")
                    step(1)
                    unchanged("TEST-1")  # Changes are pending until done.
                    assert text().count("overlay ") == count
                    step(2)
                    expect("TEST-2")
                    wait_for(lambda: "commit TEST-2 2048x160" in text())
                    step(3)
                    unchanged("TEST-2")  # No focused window.
                    step(4)
                    unchanged("TEST-2")  # Inactive window moves.
                    step(5)
                    expect("TEST-1")
                    step(6)
                    unchanged("TEST-1")  # Spanning window retains current output.
                    step(7)
                    expect("TEST-2")
                    step(8)
                    wait_for(lambda: "visible TEST-2 0" in text())
                    before = text().count("visible TEST-2 1")
                    step(9)
                    wait_for(lambda: text().count("visible TEST-2 1") > before)

                    command("pause")
                    command("hide")
                    configure("follow_active_monitor=0\n")
                    command("reload")
                    expect("TEST-1")
                    assert "paused=yes" in command("status")
                    assert "hidden=yes" in command("status")
                    configure()
                    command("reload")
                    expect("TEST-2")
                    command("resume")
                    command("show")
                    configure("monitor=TEST-1\n")
                    command("reload")
                    unchanged("TEST-1")
                    configure()
                    command("reload")
                    expect("TEST-2")
                    step(10)
                    expect("TEST-1")  # Selected output disconnected.
                    step(11)
                    unchanged("TEST-1")
                    step(12)
                    expect("TEST-2")  # Reconnected output has a new wl_output.
                    step(13)
                    expect("TEST-1")  # Most recently activated window wins.
                    time.sleep(0.3)  # Drain configure and buffer-release work.
                    count = text().count("overlay ")
                    commits = text().count("commit ")
                    step(14)
                    unchanged("TEST-1")  # Metadata must not steal focus.
                    assert text().count("overlay ") == count
                    assert text().count("commit ") == commits, \
                        "metadata-only updates must not repaint overlays"
                    step(15)
                    expect("TEST-2")
                    step(16)
                    unchanged("TEST-2")  # Focused window closed.
                    step(17)
                    unchanged("TEST-2")  # Manager became unavailable.
                elif niri:
                    wait_for(lambda: fake_niri.connections == 1)
                    fake_niri.focus(2)
                    expect("TEST-2")  # Empty workspace on another monitor.
                    fake_niri.send(
                        {"WorkspaceActivated": {"id": 1, "focused": False}})
                    unchanged("TEST-2")  # Activation without focus.
                    # Fullscreen hiding keeps the stream for empty
                    # workspaces; it closes only when nothing needs it.
                    configure("follow_active_monitor=0\n")
                    command("reload")
                    expect("TEST-1")
                    configure()
                    command("reload")
                    expect("TEST-2")
                    assert fake_niri.connections == 1
                    configure("follow_active_monitor=0\n"
                              "disable_fullscreen_hide=1\n")
                    command("reload")
                    expect("TEST-1")
                    configure()
                    command("reload")
                    wait_for(lambda: fake_niri.connections == 2)
                    expect("TEST-2")  # Resubscribed with current focus.
                    fake_niri.focused = 1
                    fake_niri.send(workspaces(1))
                    expect("TEST-1")
                else:
                    configure("follow_active_monitor=0\n")
                    command("reload")
                    unchanged("TEST-1")
                    configure()
                    command("reload")
                    unchanged("TEST-1")
            except BaseException:
                print(text())
                print((root / "app.log").read_text())
                raise
            finally:
                if app and app.poll() is None:
                    app.terminate()
                    assert app.wait(timeout=3) == 0
                server.terminate()
                assert server.wait(timeout=3) == 0
                if fake_niri:
                    fake_niri.close()


run_test()
run_test(forced=True)
run_test(protocol=False)
run_test(protocol=False, niri=True)
print("Focus following, config precedence, fallback, scaling and hotplug tests passed.")
