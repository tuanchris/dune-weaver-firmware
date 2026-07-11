# Hardware-in-the-loop test fixtures: drive a real FluidNC board over
# serial and assert on its behavior.  See hil/README.md.
#
#   python3 -m pytest hil -v
#
# Environment:
#   HIL_PORT    serial port (default /dev/cu.usbserial-8310)
#   HIL_MOTION  set to 1 to enable tests that move the machine
#
# The whole session is skipped cleanly when no board is reachable, so
# this suite is safe to invoke unconditionally. Close any serial
# monitor first; the port is exclusive.

import os
import re
import time

import pytest
import serial

PORT = os.environ.get("HIL_PORT", "/dev/cu.usbserial-8310")
BAUD = 115200


class Board:
    def __init__(self, ser):
        self.ser = ser

    def drain(self, quiet=0.3, limit=10.0):
        """Read until the line has been quiet for `quiet` seconds."""
        out = []
        last = time.time()
        end = time.time() + limit
        while time.time() < end:
            d = self.ser.read(4096)
            if d:
                out.append(d.decode(errors="replace"))
                last = time.time()
            elif time.time() - last > quiet:
                break
        return "".join(out)

    def send(self, line):
        self.ser.write((line + "\n").encode())

    def cmd(self, line, timeout=5.0):
        """Send a command; return (text, status) where status is 'ok',
        'error:N', or None on timeout."""
        self.send(line)
        buf = ""
        end = time.time() + timeout
        while time.time() < end:
            d = self.ser.read(4096)
            if not d:
                continue
            buf += d.decode(errors="replace")
            for raw in buf.splitlines():
                token = raw.strip()
                if token == "ok":
                    return buf, "ok"
                if re.match(r"error:\d+$", token):
                    return buf, token
        return buf, None

    def status(self, timeout=3.0):
        """Realtime '?' report; returns the text between < and >."""
        self.ser.write(b"?")
        buf = ""
        end = time.time() + timeout
        while time.time() < end:
            d = self.ser.read(4096)
            if d:
                buf += d.decode(errors="replace")
                m = re.search(r"<([^>]*)>", buf)
                if m:
                    return m.group(1)
        return None

    def state(self):
        s = self.status()
        return s.split("|")[0] if s else None

    def wait_state(self, want, timeout):
        end = time.time() + timeout
        while time.time() < end:
            if self.state() == want:
                return True
            time.sleep(0.5)
        return False

    def wait_ready(self, attempts=5):
        """Wait out the boot home, then probe with $I until the command
        processor responds.  startup_line0 homes on every boot and line
        commands aren't processed until the machine is back at Idle --
        only the realtime '?' status works during homing -- so poll the
        state first (DWG's home + recenter takes ~25 s)."""
        end = time.time() + 60.0
        while time.time() < end:
            s = self.status(timeout=2.0)
            if s and s.startswith("Idle"):
                break
            time.sleep(1.0)
        for _ in range(attempts):
            self.drain(quiet=0.5, limit=3.0)
            _, status = self.cmd("$I", timeout=3.0)
            if status == "ok":
                return True
        return False

    def restart(self, limit=30.0):
        """$Bye, wait through the silent reboot gap for the banner,
        capture the boot log, and wait until commands respond again."""
        self.send("$Bye")
        buf = ""
        end = time.time() + limit
        while time.time() < end and "Grbl " not in buf:
            d = self.ser.read(4096)
            if d:
                buf += d.decode(errors="replace")
        buf += self.drain(quiet=2.0, limit=15.0)
        self.wait_ready()
        return buf


@pytest.fixture(scope="session")
def board():
    try:
        ser = serial.Serial(PORT, BAUD, timeout=0.2)
    except (serial.SerialException, OSError) as e:
        pytest.skip(f"board not available on {PORT}: {e}")
    b = Board(ser)
    # The board may still be booting (e.g. right after a flash); be patient.
    if not b.wait_ready():
        ser.close()
        pytest.skip(f"no FluidNC responding on {PORT}")
    yield b
    ser.close()


@pytest.fixture(scope="session")
def leds_configured(board):
    _, status = board.cmd("$LED/Brightness")
    if status != "ok":
        pytest.skip("board config has no leds: section")
    return True


@pytest.fixture(scope="session")
def playlist_configured(board):
    # Harmless probe: reports "No playlist active" when the module exists
    _, status = board.cmd("$Playlist/Skip")
    if status != "ok":
        pytest.skip("board config has no playlist: section")
    return True


motion = pytest.mark.skipif(
    os.environ.get("HIL_MOTION") != "1",
    reason="set HIL_MOTION=1 to run tests that move the machine",
)
