#!/usr/bin/env python3
"""Bounded SH-UR01A COM/NC actuator USB recovery; never sends a stroke."""
import argparse
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import termios
import time

RELAY = ('74658ffdc01cf1119f07c9e40f0f12f8', 0x10C4, 0xEA60)
ACTUATOR = ('0353637333235110A2A3', 0x2341, 0x0043)
ACKS = {'AT': 'OK', 'AT+CH1=1': 'OK+CH1=1', 'AT+CH1=0': 'OK+CH1=0'}


def exact_ack(reply, command):
    lines = [line.strip() for line in reply.splitlines()]
    return ACKS[command] in lines and not any(line.startswith('ERROR') for line in lines)


def inventory():
    from serial.tools import list_ports
    return [dict(port=p.device, serial=p.serial_number, vid=p.vid, pid=p.pid)
            for p in list_ports.comports()]


def matches(row, identity):
    return (row['serial'], row['vid'], row['pid']) == identity


def present(rows):
    found = [p for p in rows if matches(p, ACTUATOR)]
    if len(found) > 1:
        raise RuntimeError('ambiguous actuator identity')
    return bool(found)


def unowned(port):
    # A tty owner also owns the corresponding callout device.
    paths = {port, port.replace('/dev/cu.', '/dev/tty.')}
    for path in sorted(paths):
        if not os.path.exists(path):
            continue
        check = subprocess.run(['lsof', '-nP', path], capture_output=True, timeout=2)
        if check.returncode not in (0, 1) or check.stdout.strip():
            raise RuntimeError('port owned or ownership unavailable: ' + path)


class Interrupted(Exception):
    pass


def interrupt_handler(state, signum):
    state['interrupted_signal'] = signum
    if not state.get('restoring'):
        raise Interrupted('signal ' + str(signum))
    # A repeated stop request must not interrupt the bounded restore attempt.


class Relay:
    def __init__(self, port):
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            c = termios.tcgetattr(self.fd)
            c[0] = c[1] = c[3] = 0
            c[2] |= termios.CLOCAL | termios.CREAD
            c[2] &= ~(termios.HUPCL | termios.PARENB | termios.CSTOPB | termios.CSIZE | getattr(termios, 'CRTSCTS', 0))
            c[2] |= termios.CS8
            c[4] = c[5] = termios.B9600
            c[6][termios.VMIN] = c[6][termios.VTIME] = 0
            termios.tcsetattr(self.fd, termios.TCSANOW, c)
            self.read(.2)
        except BaseException:
            os.close(self.fd)
            raise

    def read(self, seconds):
        data = b''
        end = time.monotonic() + seconds
        while time.monotonic() < end and len(data) < 4096:
            if not select.select([self.fd], [], [], .05)[0]:
                continue
            try:
                chunk = os.read(self.fd, 4096 - len(data))
            except BlockingIOError:
                continue
            if not chunk:
                break
            data += chunk
        return data.decode(errors='replace')

    def exchange(self, command):
        if command not in ACKS:
            raise ValueError('unapproved relay command')
        data = (command + '\r\n').encode()
        end = time.monotonic() + 2
        while data and time.monotonic() < end:
            if not select.select([], [self.fd], [], .05)[1]:
                continue
            try:
                count = os.write(self.fd, data)
            except BlockingIOError:
                continue
            data = data[count:]
        if data:
            raise RuntimeError('relay write timed out')
        return self.read(1)

    def close(self):
        os.close(self.fd)


def cycle(state, emit, scan, check_owner, connect, clock, sleep, off_seconds, return_seconds, degraded_start):
    """Actual cycle logic; dependencies are replaced only by inert host tests."""
    relay = None
    try:
        rows = scan()
        relays = [p for p in rows if matches(p, RELAY)]
        if len(relays) != 1:
            raise RuntimeError('one exact relay required')
        before = present(rows)
        state['actuator_present_before'] = before
        if not before and not degraded_start:
            raise RuntimeError('actuator BSD serial absent; explicit --degraded-start required')
        for row in rows:
            if matches(row, RELAY) or matches(row, ACTUATOR):
                check_owner(row['port'])
        relay = connect(relays[0]['port'])
        reply = relay.exchange('AT')
        emit('at_reply', text=reply)
        if not exact_ack(reply, 'AT'):
            raise RuntimeError('exact AT acknowledgement missing')
        state['off_attempted'] = True  # Set before even a partial write.
        emit('off_intent')
        reply = relay.exchange('AT+CH1=1')
        emit('off_reply', text=reply)
        if not exact_ack(reply, 'AT+CH1=1'):
            raise RuntimeError('exact off acknowledgement missing')
        end = clock() + off_seconds
        absent_samples = 0
        while clock() < end:
            absent_samples += int(not present(scan()))
            sleep(.2)
        state['off_absent_samples'] = absent_samples
        state['actuator_disappeared'] = before and absent_samples >= 2
    except BaseException as exc:
        state['error'] = repr(exc)
    finally:
        state['restoring'] = True
        if relay is not None:
            try:
                if state.get('off_attempted'):
                    state['restore_ack'] = False
                    for attempt in (1, 2):
                        try:
                            reply = relay.exchange('AT+CH1=0')
                            emit('restore_reply', attempt=attempt, text=reply)
                            if exact_ack(reply, 'AT+CH1=0'):
                                state['restore_ack'] = True
                                break
                        except BaseException as exc:
                            emit('restore_error', attempt=attempt, error=repr(exc))
            finally:
                try:
                    relay.close()
                    state['relay_descriptor_closed'] = True
                except BaseException as exc:
                    state['close_error'] = repr(exc)
        state['restoring'] = False
    if not state.get('error') and state.get('restore_ack'):
        try:
            end = clock() + return_seconds
            stable = 0
            while clock() < end and stable < 2:
                stable = stable + 1 if present(scan()) else 0
                sleep(.25)
            state['actuator_returned'] = stable >= 2
        except BaseException as exc:
            state['error'] = repr(exc)
    if state.get('off_attempted') and not state.get('restore_ack'):
        state['status'] = 'RESTORE_UNCONFIRMED'
    elif state.get('error') or state.get('interrupted_signal') or state.get('close_error'):
        state['status'] = 'STOPPED_FOR_REVIEW'
    elif not state.get('actuator_returned'):
        state['status'] = 'USB_RETURN_NOT_PROVED'
    else:
        state['status'] = 'BSD_DISAPPEAR_RETURN_PROVED' if state.get('actuator_present_before') else 'BSD_SERIAL_RETURNED_AFTER_RESTORE'
        if state.get('actuator_present_before') and not state.get('actuator_disappeared'):
            state['status'] = 'USB_DISAPPEARANCE_NOT_PROVED'
    state['finished_epoch'] = time.time()
    emit('finished', status=state['status'])
    return state['status'] in ('BSD_SERIAL_RETURNED_AFTER_RESTORE', 'BSD_DISAPPEAR_RETURN_PROVED')


def restoration_unconfirmed(result, forced_stop, child_reaped):
    return (bool(result.get('off_attempted') and not result.get('restore_ack')) or
            not child_reaped or
            (forced_stop and not result.get('restore_ack')) or
            result.get('worker_status', result.get('status')) == 'WORKER_RECEIPT_UNAVAILABLE')


def worker(args):
    state = dict(status='RUNNING', relay_serial=RELAY[0], actuator_serial=ACTUATOR[0],
                 wiring='user-confirmed COM/NC actuator USB power', off_seconds=args.off_seconds,
                 degraded_start=args.degraded_start, events=[], started_epoch=time.time(),
                 actuator_commands=0, physical_power_off_proved=False, sketch_alive_proved=False)
    def emit(kind, **values):
        state['events'].append(dict(epoch=time.time(), kind=kind, **values))
        try:
            tmp = args.out / 'WORKER.tmp'
            tmp.write_text(json.dumps(state, indent=2) + '\n')
            tmp.replace(args.out / 'WORKER.json')
        except OSError as exc:
            state['receipt_error'] = repr(exc)
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda n, frame: interrupt_handler(state, n))
    return 0 if cycle(state, emit, inventory, unowned, Relay, time.monotonic, time.sleep,
                      args.off_seconds, args.return_seconds, args.degraded_start) else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--off-seconds', type=int, default=10, choices=range(1, 11))
    parser.add_argument('--return-seconds', type=int, default=30, choices=range(1, 31))
    parser.add_argument('--degraded-start', action='store_true', help='explicitly allow the known actuator BSD serial to be absent before cycling')
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.worker:
        return worker(args)
    os.umask(0o077)
    args.out.mkdir(parents=False, exist_ok=False)
    argv = [sys.executable, '-B', str(Path(__file__).resolve()), '--worker', '--out', str(args.out),
            '--off-seconds', str(args.off_seconds), '--return-seconds', str(args.return_seconds)]
    if args.degraded_start:
        argv.append('--degraded-start')
    stops = []
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda n, frame: stops.append(n))
    with (args.out / 'worker.log').open('w') as log:
        child = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + args.off_seconds + args.return_seconds + 30
        stop_at = None
        while child.poll() is None:
            now = time.monotonic()
            if stop_at is None and (stops or now >= deadline):
                child.terminate();stop_at = now
            elif stop_at is not None and now - stop_at >= 8:
                child.kill()
                break
            time.sleep(.1)
        try:
            child.wait(timeout=2)
        except subprocess.TimeoutExpired:
            pass
    try:
        result = json.loads((args.out / 'WORKER.json').read_text())
    except (OSError, ValueError):
        result = dict(status='WORKER_RECEIPT_UNAVAILABLE')
    result.update(worker_pid=child.pid, worker_exit_code=child.poll(), worker_reaped=child.poll() is not None,
                  supervisor_signals=stops, supervisor_finished_epoch=time.time())
    if stop_at is not None or child.returncode != 0 or result.get('receipt_error'):
        result['worker_status'] = result['status']
        result['status'] = 'STOPPED_FOR_REVIEW'
    result['restore_unconfirmed'] = restoration_unconfirmed(result, stop_at is not None, child.poll() is not None)
    (args.out / 'RESULT.json').write_text(json.dumps(result, indent=2) + '\n')
    print(result['status'])
    return 0 if result['status'] in ('BSD_SERIAL_RETURNED_AFTER_RESTORE', 'BSD_DISAPPEAR_RETURN_PROVED') else 1


if __name__ == '__main__':
    sys.exit(main())
