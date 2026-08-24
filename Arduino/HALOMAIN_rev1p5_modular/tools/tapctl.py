#!/usr/bin/env python3
"""tapctl — the one tap primitive every HALO test should use.

Earned the hard way. Each rule below exists because its absence cost a test run:

  * NEVER strokes harder than the sketch default (500/200/500). A parameter sweep
    up to 1400/600/1400 was driving the stylus into the panel; longer extend time
    on this rig means more travel, not a better tap. Matt saw it and stopped it.
    Harder is capped here so no caller can reintroduce it.

  * tap.py is never invoked. It reopens the port per call (3s Uno reset), it has
    hung >100s twice, and it prints "Sending: TAP" whether or not anything is
    listening. This holds the port open and talks to the sketch directly.

  * Only a board ENUMERATING counts as a wake. The sketch's "PUSH: Complete" and
    "STYLUS: ON" are printed even with the stylus nowhere near the glass - that
    misled a whole night's soak.

  * Distinguishes SKETCH DEAD from MISSED CONTACT. They look identical from the
    host, and conflating them nearly got LCD touch-wake firmware blamed for a
    dead Arduino. If the sketch stops answering, stop tapping and say so.

  * "Asleep" is judged by the SENSE port only. The LCD does not reliably
    re-enumerate USB after a wake, so absence-of-LCD-USB is NOT sleep - assuming
    it was produced a 5/5 "wake rate" against an already-awake device.
"""
import serial, time, glob

ACT_PORT   = "/dev/cu.usbmodem21301"
STROKE     = "PUSH:500,200,500"     # the sketch default. Do not raise.
WAKE_WAIT  = 12                     # generous: boot + USB enumeration
SETTLE     = 2.0

class TapError(RuntimeError):
    pass

def ports():
    g = glob.glob("/dev/cu.usbmodem*")
    return (next((p for p in g if p.endswith("m101")), None),
            next((p for p in g if p.endswith("1101")), None))

def sense_up():   return ports()[1] is not None
def lcd_up():     return ports()[0] is not None
def any_up():     return sense_up() or lcd_up()

class Tapper:
    def __init__(self, verbose=True):
        self.verbose = verbose
        self.s = serial.Serial()
        self.s.port = ACT_PORT; self.s.baudrate = 115200; self.s.timeout = 0.2
        self.s.dtr = False; self.s.rts = False
        self.s.open()
        time.sleep(3.5)              # one-time boot after the open-reset
        self.s.reset_input_buffer()

    def _say(self, m):
        if self.verbose: print(f"    [tap] {m}", flush=True)

    def close(self):
        try:
            self.s.write(b"STOP\n"); self.s.flush(); time.sleep(0.3)
            self.s.close()
        except Exception:
            pass

    def sketch_alive(self):
        """A live sketch answers. Silence means the Uno is not running it."""
        self.s.reset_input_buffer()
        self.s.write(b"HELP\n"); self.s.flush()
        t0 = time.time(); r = ""
        while time.time() - t0 < 3:
            n = self.s.in_waiting
            if n: r += self.s.read(n).decode(errors="replace")
            else: time.sleep(0.05)
        return len(r) > 0

    def stroke_once(self):
        self.s.reset_input_buffer()
        self.s.write((STROKE + "\n").encode()); self.s.flush()
        t0 = time.time(); r = ""
        while time.time() - t0 < 8:
            n = self.s.in_waiting
            if n: r += self.s.read(n).decode(errors="replace")
            else: time.sleep(0.05)
            if "Complete" in r: break
        return "Complete" in r

    def wake(self, attempts=6, require="any"):
        """Tap until the required board actually enumerates. Returns True/False.

        require="lcd" matters more than it looks: the LCD does not reliably
        re-enumerate USB after a wake, so "a board is up" is often the SENSE
        only. A test that needs to inject commands must ask for the LCD
        explicitly or it will sail past this check and then fail on a None port.

        Raises TapError if the sketch is dead — that needs a human, and
        continuing to tap at it just wastes the run.
        """
        def satisfied():
            if require == "lcd":   return lcd_up()
            if require == "sense": return sense_up()
            return any_up()
        if satisfied():
            self._say(f"already awake ({require})")
            return True
        if not self.sketch_alive():
            raise TapError("actuator sketch is not running (no reply) — "
                           "power-cycle the Uno / re-upload actuator_test.ino")
        for i in range(1, attempts + 1):
            self.stroke_once()
            t0 = time.time()
            while time.time() - t0 < WAKE_WAIT:
                if satisfied():
                    self._say(f"woke on stroke {i} after {time.time()-t0:.1f}s")
                    return True
                time.sleep(0.25)
            self._say(f"stroke {i}: no wake")
            if not self.sketch_alive():
                raise TapError(f"sketch died after {i} strokes — power-cycle the Uno")
        return False

    def wait_asleep(self, timeout=120):
        """Sleep is judged by the SENSE port only (see module docstring)."""
        t0 = time.time()
        while time.time() - t0 < timeout:
            if not sense_up():
                time.sleep(SETTLE)
                if not sense_up():
                    return True
            time.sleep(1)
        return False

if __name__ == "__main__":
    t = Tapper()
    try:
        print("  sketch alive:", t.sketch_alive())
        print("  wake:", t.wake())
        print("  ports:", ports())
    except TapError as e:
        print("  TAP ERROR:", e)
    finally:
        t.close()
