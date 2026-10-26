#!/usr/bin/env python3
"""Run isolated runtime regressions against the protocol fixture."""
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


def wait_for(condition, seconds=4):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if condition():
            return
        time.sleep(0.03)
    raise AssertionError("condition did not become true")


def children(pid):
    path = Path(f"/proc/{pid}/task/{pid}/children")
    return [int(child) for child in path.read_text().split()]


def context_switches(pid):
    status = Path(f"/proc/{pid}/status").read_text()
    for line in status.splitlines():
        if line.startswith("voluntary_ctxt_switches:"):
            return int(line.split()[1])
    raise AssertionError("no context switch count")


with tempfile.TemporaryDirectory(prefix="bongocat-integration-") as directory:
    root = Path(directory)
    env = dict(os.environ, XDG_RUNTIME_DIR=directory,
               WAYLAND_DISPLAY="wayland-test")
    env.pop("NIRI_SOCKET", None)
    config = root / "cat.conf"
    config.write_text("keyboard_device=/dev/input/event-bongocat-test-missing\n"
                      "monitor=TEST-1,TEST-2\noverlay_opacity=0\nfps=1\n"
                      "test_animation_interval=1\n[monitor:TEST-2]\n"
                      "cat_height=60\ndisable_fullscreen_hide=1\n"
                      "[global]\ncat_height=40\n")
    compositor_log = (root / "compositor.log").open("w+")
    app_log = (root / "app.log").open("w+")
    server = subprocess.Popen([fixture], env=env, stdout=compositor_log,
                              stderr=compositor_log)
    app = None

    def command(name, success=True):
        result = subprocess.run([binary, "--" + name], env=env,
                                capture_output=True, text=True, timeout=3)
        assert (result.returncode == 0) == success, \
            (name, result.stdout, result.stderr)
        return result.stdout

    def compositor_text():
        compositor_log.flush()
        return (root / "compositor.log").read_text()

    def full_damage_after(skip, monitor, visible):
        """Whether the monitor's first commit with the given visibility after
        `skip` commits was fully damaged; None before it arrives."""
        lines = compositor_text().splitlines()
        commits = []
        for previous, line in zip(lines, lines[1:]):
            if line.startswith("commit "):
                commits.append((previous.split(), line.split()))
        for shown, (_, name, size, _, damage) in commits[skip:]:
            if name == monitor and shown == ["visible", monitor, str(int(visible))]:
                width, height = map(int, size.split("x"))
                return int(damage) == width * height
        return None

    def phase(number):
        server.send_signal(signal.SIGUSR1)
        wait_for(lambda: f"phase {number}\n" in compositor_text())
        time.sleep(0.2)
        assert app.poll() is None and server.poll() is None

    def start(*extra):
        process = subprocess.Popen([binary, "-c", str(config), *extra],
                                   env=env, stdout=app_log, stderr=app_log)
        wait_for(lambda: (root / "bongocat.sock").exists())
        wait_for(lambda: children(process.pid))
        return process

    try:
        wait_for(lambda: (root / "wayland-test").exists())
        app = start("-w")
        helper = children(app.pid)[0]
        assert "paused=no" in command("status")
        assert "devices=0" in command("status")
        competing = subprocess.run([binary, "-c", str(config)], env=env,
                                   capture_output=True, text=True, timeout=3)
        assert competing.returncode != 0
        assert "already running" in competing.stderr
        # Visibility changes repaint everything, whichever buffer was shown.
        # TEST-2 ignores the fixture's fullscreen window; TEST-1 is covered.
        for name in ("hide", "show", "hide", "show"):
            before = compositor_text().count("\ncommit ")
            command(name)
            shown = name == "show"
            wait_for(lambda: full_damage_after(before, "TEST-2", shown)
                     is not None)
            assert full_damage_after(before, "TEST-2", shown), name
            time.sleep(0.5)  # Let the fixture release both buffers
        command("pause")
        assert "paused=yes" in command("status")
        command("resume")
        assert "paused=no" in command("status")

        # Invalid, missing and replaced configurations reload transactionally.
        config.write_text("fps=invalid\n")
        command("reload", success=False)
        config.unlink()
        command("reload", success=False)
        assert app.poll() is None
        replacement = root / "replacement"
        replacement.write_text("monitor=TEST-1,TEST-2\ncat_height=45\n"
                               "keyboard_device=/dev/input/event-missing\n")
        replacement.replace(config)
        command("reload")

        # Idle means asleep: no timers in the renderer or the helper.
        wait_for(lambda: children(app.pid))
        helper = children(app.pid)[0]
        time.sleep(0.5)
        before = (context_switches(app.pid), context_switches(helper))
        time.sleep(1.5)
        after = (context_switches(app.pid), context_switches(helper))
        assert after[0] - before[0] <= 1 and after[1] == before[1], \
            (before, after)

        # A killed helper is restarted.
        os.kill(helper, signal.SIGKILL)
        wait_for(lambda: children(app.pid) and children(app.pid)[0] != helper,
                 seconds=7)
        helper = children(app.pid)[0]

        # Resolution and scale change, unplug, replug, then queue pressure.
        for number in range(1, 5):
            phase(number)
        command("status")

        # Stop server reads and fill the renderer's Wayland socket. Controls
        # must keep responding while the flush waits for POLLOUT.
        os.kill(server.pid, signal.SIGSTOP)
        resume = threading.Timer(2, lambda: os.kill(server.pid, signal.SIGCONT))
        resume.start()
        try:
            for _ in range(2500):
                with socket.socket(socket.AF_UNIX,
                                   socket.SOCK_SEQPACKET) as control:
                    control.settimeout(1.5)
                    control.connect(str(root / "bongocat.sock"))
                    control.sendall(b"reload")
                    assert control.recv(512).startswith(b"0 ")
        finally:
            os.kill(server.pid, signal.SIGCONT)
            resume.cancel()
        time.sleep(0.2)
        assert app.poll() is None

        # A missing explicit monitor waits; extreme offsets are clipped.
        config.write_text("monitor=MISSING\n")
        command("reload")
        config.write_text("monitor=TEST-1\ncat_x_offset=-2147483648\n"
                          "cat_y_offset=2147483647\n")
        command("reload")
        assert app.poll() is None

        # Signals exit cleanly, keep the lock inode and reap the helper.
        inode = (root / "bongocat.pid").stat().st_ino
        for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGQUIT,
                    signal.SIGHUP):
            app.send_signal(sig)
            assert app.wait(timeout=3) == 0
            assert (root / "bongocat.pid").stat().st_ino == inode
            assert not (root / "bongocat.sock").exists()
            assert not Path(f"/proc/{helper}").exists()
            app = start()
            helper = children(app.pid)[0]

        # --toggle stops a running cat, then starts one.
        command("stop", success=False)  # Not a public option
        subprocess.run([binary, "--toggle"], env=env, check=True, timeout=3)
        assert app.wait(timeout=3) == 0
        app = subprocess.Popen([binary, "-c", str(config), "--toggle"],
                               env=env, stdout=app_log, stderr=app_log)
        wait_for(lambda: (root / "bongocat.sock").exists())
        subprocess.run([binary, "--toggle"], env=env, check=True, timeout=3)
        assert app.wait(timeout=3) == 0

        text = compositor_text()
        assert "overlay TEST-1" in text and "overlay TEST-2" in text
        assert "phase 4" in text and "commit TEST-1" in text
        # 800 px at 1.25x, 1024 px at 2x, then 640 px at 1.5x after phase 1.
        assert "commit TEST-1 1000x" in text and "commit TEST-2 2048x" in text
        assert "commit TEST-1 960x" in text
        assert "visible TEST-1 0" in text and "visible TEST-1 1" in text
        assert text.count("overlay TEST-2") >= 2  # Replugged
        print("Runtime controls, helper restart, idle wakeups, signals, "
              "reloads, output lifecycle and buffer tests passed.")
    finally:
        if app and app.poll() is None:
            app.terminate()
            try:
                app.wait(timeout=3)
            except subprocess.TimeoutExpired:
                app.kill()
                app.wait()
        server.terminate()
        server.wait(timeout=3)
        if (app and app.returncode) or server.returncode:
            print((root / "app.log").read_text())
            print(compositor_text())
        app_log.close()
        compositor_log.close()
