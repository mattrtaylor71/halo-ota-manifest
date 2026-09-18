#!/usr/bin/env python3
"""Bounded actuator control. A completed stroke is not proof of contact or wake.

Only the child opens USB; its supervisor bounds even a stuck macOS open/ioctl/
close. No reset, relay, flash, stronger stroke or automatic recovery is issued.
Each call retains a private receipt. USB transitions need accompanying Halo logs
for physical sleep/wake acceptance; an already present board is not a new wake.
"""
import argparse
import json
import os
from pathlib import Path
import re
import select
import signal
import subprocess
import sys
import tempfile
import termios
import time

ACTUATOR = ('03536373232351608112', 0x2341, 0x0043)
SENSE = ('98:A3:16:F8:1A:6C', 0x303A, 0x1001)
LCD = ('20:6E:F1:A1:2D:C4', 0x303A, 0x1001)
STROKE = 'PUSH:500,200,500'  # Calibrated maximum; never raise travel/speed.
WAKE_WAIT = 12
SETTLE = 2.0
BOUNDS = {'inventory': 8, 'probe': 25, 'stroke': 25, 'wake': 45}
SUCCESSES = {'INVENTORY', 'READY', 'STROKE_COMPLETE', 'WAKE_OBSERVED', 'ALREADY_AWAKE'}
_REVIEW_REQUIRED = None


class TapError(RuntimeError):
    def __init__(self, message, result=None):
        super().__init__(message)
        self.result = result


class Failure(Exception):
    def __init__(self, status, message):
        super().__init__(message)
        self.status = status


def inventory():
    from serial.tools import list_ports
    return [dict(port=p.device, serial=p.serial_number, vid=p.vid, pid=p.pid)
            for p in list_ports.comports()]


def exact_port(rows, identity):
    found = [r['port'] for r in rows
             if (r.get('serial'), r.get('vid'), r.get('pid')) == identity]
    if len(found) > 1:
        raise Failure('IDENTITY_AMBIGUOUS', 'Multiple ports match ' + identity[0])
    return found[0] if found else None


def raw_settings(current, flags=termios):
    """Remove inherited software AND hardware flow control without modem ioctls."""
    c = list(current); c[6] = list(current[6])
    c[0] = c[1] = c[3] = 0
    c[2] |= flags.CLOCAL | flags.CREAD
    clear = flags.HUPCL | flags.PARENB | flags.CSTOPB | flags.CSIZE
    for name in ('CRTSCTS', 'CCTS_OFLOW', 'CRTS_IFLOW', 'CDTR_IFLOW', 'CDSR_OFLOW', 'CCAR_OFLOW'):
        clear |= getattr(flags, name, 0)
    if sys.platform == 'darwin':
        # Darwin sys/termios.h: Python may omit the DTR/DSR/DCD flow-control
        # constants and CIGNORE even though the driver honors those bits.
        clear |= 0x001F0001
    c[2] = (c[2] & ~clear) | flags.CS8
    c[4] = c[5] = flags.B115200
    c[6][flags.VMIN] = c[6][flags.VTIME] = 0
    return c


def help_valid(reply):
    lines = reply.replace('\r', '').splitlines()
    return ('Commands:' in lines and
            '  PUSH:e,h,r  - extend/hold/retract ms' in lines and
            '  STATUS      - show state' in lines)


def status_valid(reply):
    lines = reply.replace('\r', '').splitlines()
    return (all(x in lines for x in ('=== STATUS ===', 'PWM: 0',
            'Target speed: 128', 'Hold time: 200ms', 'Stylus: OFF')) and
            any(re.fullmatch(r'Pot: \d+', x) for x in lines))


def unowned(port):
    for path in (port, port.replace('/dev/cu.', '/dev/tty.')):
        if os.path.exists(path):
            p = subprocess.run(['lsof', '-nP', path], capture_output=True, timeout=2)
            if p.returncode not in (0, 1) or p.stdout.strip():
                raise Failure('PORT_OWNED', 'Ownership unavailable or port busy: ' + path)


class RawPort:
    def __init__(self, port, emit):
        self.fd = None; self.emit = emit
        try:
            emit('open.before', port=port)
            self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            emit('open.after')
            emit('configure.before')
            current = termios.tcgetattr(self.fd)
            configured = raw_settings(current)
            emit('settings', before_cflag=current[2], after_cflag=configured[2])
            termios.tcsetattr(self.fd, termios.TCSANOW, configured)
            emit('configure.after')
        except BaseException:
            self.close()
            raise

    def read(self, seconds, complete=None):
        data = b''; end = time.monotonic() + seconds
        while time.monotonic() < end:
            if not select.select([self.fd], [], [], .05)[0]:
                continue
            try:
                chunk = os.read(self.fd, 4096)
            except BlockingIOError:
                continue
            if not chunk:
                raise Failure('SERIAL_EOF', 'Actuator disconnected')
            data += chunk
            if len(data) > 8192:
                raise Failure('SERIAL_OVERFLOW', 'Excess unsolicited actuator output')
            text = data.decode(errors='replace')
            if complete and text.endswith('\n') and complete(text):
                break
        return data.decode(errors='replace')

    def exchange(self, command, seconds, complete=None):
        if command not in ('HELP', 'STATUS', 'STOP', STROKE):
            raise ValueError('Unapproved actuator command')
        # Preserve discarded text; a previous reply cannot satisfy this request.
        self.emit('drain', text=self.read(.1))
        self.emit('write.before', command=command)
        data = (command + '\n').encode(); end = time.monotonic() + 2
        while data and time.monotonic() < end:
            if not select.select([], [self.fd], [], .05)[1]:
                continue
            try:
                count = os.write(self.fd, data)
            except BlockingIOError:
                continue
            if count <= 0:
                raise Failure('SERIAL_WRITE', 'Zero-byte write')
            data = data[count:]
        if data:
            raise Failure('SERIAL_WRITE', 'Write deadline exceeded')
        self.emit('write.after', command=command)
        reply = self.read(seconds, complete)
        self.emit('reply', command=command, text=reply)
        return reply

    def close(self):
        if self.fd is not None:
            try:
                self.emit('close.before')
            finally:
                os.close(self.fd); self.fd = None
            self.emit('close.after')


def episode(action, state, emit, scan=inventory, owner=unowned, connect=RawPort,
            clock=time.monotonic, sleep=time.sleep):
    """Actual worker logic, with inert dependency injection for host tests."""
    link = None
    try:
        rows = scan(); state['inventory_before'] = rows
        if action == 'inventory':
            state['status'] = 'INVENTORY'; return
        port = exact_port(rows, ACTUATOR)
        if not port:
            raise Failure('ACTUATOR_ABSENT', 'Exact replacement Uno is absent')
        if action == 'wake':
            if exact_port(rows, SENSE):
                state['status'] = 'ALREADY_AWAKE'; return
            if exact_port(rows, LCD):
                raise Failure('BOARD_STATE_UNSAFE', 'LCD present without Sense; no blind tap')
            sleep(SETTLE)
            rows = scan()
            if exact_port(rows, SENSE) or exact_port(rows, LCD):
                raise Failure('BOARD_STATE_CHANGED', 'Halo appeared before stroke')
            state['stable_usb_absence'] = True
        owner(port); link = connect(port, emit)
        emit('startup', text=link.read(3.5))
        if not help_valid(link.exchange('HELP', 3, help_valid)):
            raise Failure('NO_HELP', 'Fresh actuator HELP response missing')
        if not status_valid(link.exchange('STATUS', 2, status_valid)):
            raise Failure('NOT_READY', 'Actuator is not stopped at the unchanged calibration')
        state['ready'] = True
        if action == 'probe':
            state['status'] = 'READY'
        else:
            if action == 'wake':
                rows = scan()
                if exact_port(rows, SENSE) or exact_port(rows, LCD):
                    raise Failure('BOARD_STATE_CHANGED', 'Halo appeared during actuator readiness')
            state['stroke_attempted'] = True
            reply = link.exchange(STROKE, 8, lambda t: 'PUSH: Complete' in t.splitlines())
            if 'PUSH: Complete' not in reply.splitlines():
                raise Failure('NO_COMPLETION', 'Calibrated stroke did not acknowledge completion')
            state['stroke_complete'] = True
            if not status_valid(link.exchange('STATUS', 2, status_valid)):
                raise Failure('POST_STROKE_NOT_READY', 'Stopped state missing after stroke')
            state['status'] = 'STROKE_COMPLETE'
    except BaseException as exc:
        state['status'] = getattr(exc, 'status', 'SERIAL_ERROR'); state['error'] = repr(exc)
    finally:
        if link is not None:
            try:
                reply = link.exchange('STOP', 1, lambda t: 'CMD: STOP' in t.splitlines())
                state['stop_ack'] = 'CMD: STOP' in reply.splitlines()
                if not state['stop_ack'] and state['status'] in SUCCESSES:
                    state['status'] = 'STOP_UNCONFIRMED'
            except BaseException as exc:
                state['stop_error'] = repr(exc)
                if state['status'] in SUCCESSES: state['status'] = 'STOP_UNCONFIRMED'
            finally:
                try:
                    link.close(); state['descriptor_closed'] = True
                except BaseException as exc:
                    state['close_error'] = repr(exc); state['status'] = 'CLOSE_FAILED'
        emit('serial_finished', status=state['status'])
    if action == 'wake' and state['status'] == 'STROKE_COMPLETE':
        end = clock() + WAKE_WAIT
        while clock() < end:
            if exact_port(scan(), SENSE):
                state['sense_usb_transition'] = True
                state['status'] = 'WAKE_OBSERVED'; break
            sleep(.25)
        else:
            state['status'] = 'NO_HALO_WAKE'
        emit('wake_finished', status=state['status'])


def group_exists(pid):
    try: os.killpg(pid, 0); return True
    except ProcessLookupError: return False
    except OSError: return True  # Unknown is not proof of closure.


def supervise(argv, out, timeout):
    # An outer controller can terminate us while the USB worker is stuck. Reap
    # that worker before returning; a second signal must not abort this cleanup.
    previous = {}
    def interrupt(signum, frame):
        for sig in previous: signal.signal(sig, signal.SIG_IGN)
        raise KeyboardInterrupt()
    try:
        for sig in (signal.SIGTERM, signal.SIGINT):
            try:
                previous[sig] = signal.signal(sig, interrupt)
            except ValueError:  # A library caller may be on a non-main thread.
                break
        return _supervise(argv, out, timeout)
    finally:
        for sig, handler in previous.items(): signal.signal(sig, handler)


def _supervise(argv, out, timeout):
    """Never let a kill/wait failure skip the caller's other resource cleanup."""
    errors = []; timed_out = False
    def stop(child, sig):
        try: os.killpg(child.pid, sig)
        except ProcessLookupError: pass
        except OSError as exc: errors.append(repr(exc))
    with (out / 'worker.log').open('w') as log:
        child = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            child.wait(timeout=timeout)
        except (subprocess.TimeoutExpired, KeyboardInterrupt):
            timed_out = True
            stop(child, signal.SIGTERM)
            try: child.wait(timeout=2)
            except (subprocess.TimeoutExpired, OSError) as exc:
                errors.append(repr(exc)); stop(child, signal.SIGKILL)
                try: child.wait(timeout=2)
                except (subprocess.TimeoutExpired, OSError) as exc: errors.append(repr(exc))
        except OSError as exc:
            errors.append(repr(exc)); stop(child, signal.SIGKILL)
            try: child.wait(timeout=2)
            except (subprocess.TimeoutExpired, OSError) as exc: errors.append(repr(exc))
        if group_exists(child.pid):
            stop(child, signal.SIGKILL)
        try: result = json.loads((out / 'WORKER.json').read_text())
        except (OSError, ValueError): result = {'status': 'WORKER_RECEIPT_MISSING'}
        if not isinstance(result, dict): result = {'status': 'WORKER_RECEIPT_INVALID'}
        result.update(worker_pid=child.pid, worker_exit_code=child.poll(),
                      worker_reaped=child.poll() is not None, group_absent=not group_exists(child.pid),
                      controller_timeout=timed_out, supervisor_errors=errors, receipt_dir=str(out))
        if timed_out or child.returncode != 0 or not result['group_absent'] or errors:
            result['worker_status'] = result.get('status')
            result['status'] = 'CONTROLLER_TIMEOUT' if timed_out else 'CONTROLLER_FAILED'
        elif result.get('status') in ('READY', 'STROKE_COMPLETE', 'WAKE_OBSERVED') and not (
                result.get('descriptor_closed') and result.get('stop_ack')):
            result['worker_status'] = result['status']; result['status'] = 'CLOSURE_UNPROVED'
        try: (out / 'RESULT.json').write_text(json.dumps(result, indent=2) + '\n')
        except OSError as exc:
            result['receipt_error'] = repr(exc); result['status'] = 'RECEIPT_FAILED'
        return result


def run(action):
    global _REVIEW_REQUIRED
    if action != 'inventory' and _REVIEW_REQUIRED is not None:
        return dict(status='REVIEW_REQUIRED', previous_result=_REVIEW_REQUIRED,
                    receipt_dir=_REVIEW_REQUIRED['receipt_dir'])
    out = Path(tempfile.mkdtemp(prefix='halo-tapctl-'))
    result = supervise([sys.executable, '-B', str(Path(__file__).resolve()),
                        '--worker', action, '--out', str(out)], out, BOUNDS[action])
    if result['status'] in ('CONTROLLER_TIMEOUT', 'CONTROLLER_FAILED', 'CLOSURE_UNPROVED',
                            'RECEIPT_FAILED', 'CLOSE_FAILED', 'STOP_UNCONFIRMED',
                            'NO_COMPLETION', 'WORKER_ERROR', 'WORKER_RECEIPT_MISSING',
                            'WORKER_RECEIPT_INVALID'):
        _REVIEW_REQUIRED = result
    return result


def scanned_ports():
    result = run('inventory')
    if result['status'] != 'INVENTORY':
        raise TapError('USB inventory failed', result)
    return result['inventory_before']


def actuator_port(): return exact_port(scanned_ports(), ACTUATOR)
def ports():
    rows = scanned_ports()
    return exact_port(rows, LCD), exact_port(rows, SENSE)
def sense_up(): return ports()[1] is not None
def lcd_up(): return ports()[0] is not None
def any_up(): return any(ports())


class Tapper:
    """Compatibility API; each operation owns and closes its bounded worker."""
    def __init__(self, verbose=True):
        self.verbose = verbose; self.last_result = None; self.closed = False

    def close(self):
        self.closed = True  # There is no parent-owned serial descriptor.

    def _run(self, action):
        if self.closed: raise TapError('Tapper is closed')
        self.last_result = run(action)
        if self.verbose:
            print('[tap] ' + self.last_result['status'] + ' — ' + self.last_result['receipt_dir'], flush=True)
        return self.last_result['status']

    def sketch_alive(self): return self._run('probe') == 'READY'
    def stroke_once(self): return self._run('stroke') == 'STROKE_COMPLETE'

    def wake(self, attempts=1, require='sense'):
        if require not in ('any', 'sense', 'lcd') or not 1 <= attempts <= 6:
            raise ValueError('require must be any/sense/lcd and attempts must be 1..6')
        for _ in range(attempts):
            status = self._run('wake')
            if status in ('WAKE_OBSERVED', 'ALREADY_AWAKE'):
                return require != 'lcd' or lcd_up()
            if status != 'NO_HALO_WAKE':
                raise TapError('Actuator wake stopped: ' + status, self.last_result)
        return False

    def wait_asleep(self, timeout=120):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if not any_up():
                time.sleep(SETTLE)
                if not any_up(): return True
            time.sleep(.25)
        return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--worker', choices=BOUNDS, help=argparse.SUPPRESS)
    parser.add_argument('--out', type=Path, help=argparse.SUPPRESS)
    parser.add_argument('--action', choices=('probe', 'stroke', 'wake'), default='probe')
    args = parser.parse_args()
    if not args.worker:
        result = run(args.action); print(json.dumps(result, indent=2))
        return 0 if result['status'] in SUCCESSES else 1
    state = dict(status='RUNNING', action=args.worker, events=[], started_epoch=time.time(),
                 actuator_serial=ACTUATOR[0], stroke=STROKE, stroke_attempted=False,
                 stroke_complete=False, descriptor_closed=False, modem_line_operations=0)
    def emit(kind, **values):
        state['events'].append(dict(kind=kind, epoch=time.time(), **values))
        tmp = args.out / 'WORKER.tmp'; tmp.write_text(json.dumps(state, indent=2) + '\n')
        tmp.replace(args.out / 'WORKER.json')
    def interrupted(signum, frame):
        # Parent escalates if cleanup itself blocks; don't nest cleanup exceptions.
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        raise Failure('INTERRUPTED', 'Signal ' + str(signum))
    signal.signal(signal.SIGTERM, interrupted)
    try: episode(args.worker, state, emit)
    except BaseException as exc:
        state['status'] = 'WORKER_ERROR'; state['error'] = repr(exc)
    state['finished_epoch'] = time.time(); emit('finished', status=state['status'])
    # Domain failures retain their exact category; nonzero means worker malfunction.
    return 0


if __name__ == '__main__':
    sys.exit(main())
