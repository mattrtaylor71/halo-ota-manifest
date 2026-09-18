#!/usr/bin/env python3
"""Inert actuator controller tests. No USB, relay, motor, firmware or cloud access."""
import ast
import json
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
import tapctl as t

HELP = ('Commands:\r\n  PUSH:e,h,r  - extend/hold/retract ms\r\n'
        '  STATUS      - show state\r\n')
STATUS = '=== STATUS ===\r\nPWM: 0\r\nTarget speed: 128\r\nHold time: 200ms\r\nStylus: OFF\r\nPot: 398\r\n'


def row(identity, port):
    return dict(serial=identity[0], vid=identity[1], pid=identity[2], port=port)


ACT = row(t.ACTUATOR, '/dev/cu.test-actuator')
SENSE = row(t.SENSE, '/dev/cu.test-sense')
LCD = row(t.LCD, '/dev/cu.test-lcd')


class FakePort:
    def __init__(self):
        self.commands = []; self.closed = False; self.replies = {}
    def read(self, seconds): return ''
    def exchange(self, command, seconds, complete=None):
        self.commands.append(command)
        reply = self.replies.get(command, {'HELP': HELP, 'STATUS': STATUS,
            t.STROKE: 'PUSH: Complete\r\n', 'STOP': 'CMD: STOP\r\n'}[command])
        if isinstance(reply, Exception): raise reply
        return reply
    def close(self): self.closed = True


class EpisodeTests(unittest.TestCase):
    def episode(self, action='wake', scans=None, port=None):
        port = port or FakePort(); scans = list(scans or [[ACT]])
        state = dict(status='RUNNING', stroke_attempted=False, stroke_complete=False)
        now = [0]; seen = []
        def scan(): return scans.pop(0) if len(scans) > 1 else scans[0]
        def sleep(seconds): now[0] += seconds
        t.episode(action, state, lambda *a, **kw: seen.append((a, kw)),
            scan=scan, owner=lambda p: None, connect=lambda p, e: port,
            clock=lambda: now[0], sleep=sleep)
        return state, port, seen

    def test_probe_does_not_move_and_closes(self):
        state, port, _ = self.episode('probe')
        self.assertEqual(state['status'], 'READY')
        self.assertEqual(port.commands, ['HELP', 'STATUS', 'STOP'])
        self.assertTrue(port.closed and state['descriptor_closed'])

    def test_stroke_has_fixed_cap_and_both_status_checks(self):
        state, port, _ = self.episode('stroke')
        self.assertEqual(t.STROKE, 'PUSH:500,200,500')
        self.assertEqual(port.commands, ['HELP', 'STATUS', t.STROKE, 'STATUS', 'STOP'])
        self.assertEqual(state['status'], 'STROKE_COMPLETE')
        self.assertNotIn('sense_usb_transition', state)

    def test_complete_without_wake_is_not_success(self):
        state, port, _ = self.episode()
        self.assertEqual(state['status'], 'NO_HALO_WAKE')
        self.assertTrue(state['stroke_complete'] and port.closed)

    def test_actual_sense_transition_required(self):
        state, _, _ = self.episode(scans=[[ACT], [ACT], [ACT], [ACT, SENSE]])
        self.assertEqual(state['status'], 'WAKE_OBSERVED')
        self.assertTrue(state['sense_usb_transition'])

    def test_lcd_only_after_stroke_is_not_wake(self):
        state, _, _ = self.episode(scans=[[ACT], [ACT], [ACT], [ACT, LCD]])
        self.assertEqual(state['status'], 'NO_HALO_WAKE')

    def test_already_awake_and_partial_start_never_tap(self):
        for board, expected in ((SENSE, 'ALREADY_AWAKE'), (LCD, 'BOARD_STATE_UNSAFE')):
            state, port, _ = self.episode(scans=[[ACT, board]])
            self.assertEqual(state['status'], expected)
            self.assertEqual(port.commands, [])
            self.assertFalse(state['stroke_attempted'])

    def test_wake_during_readiness_never_taps(self):
        state, port, _ = self.episode(scans=[[ACT], [ACT], [ACT, SENSE]])
        self.assertEqual(state['status'], 'BOARD_STATE_CHANGED')
        self.assertNotIn(t.STROKE, port.commands)
        self.assertTrue(port.closed)

    def test_no_help_and_bad_calibration_are_distinct(self):
        for cmd, reply, expected in [('HELP', '', 'NO_HELP'),
                ('STATUS', STATUS.replace('128', '255'), 'NOT_READY')]:
            port = FakePort(); port.replies[cmd] = reply
            state, port, _ = self.episode('stroke', port=port)
            self.assertEqual(state['status'], expected)
            self.assertNotIn(t.STROKE, port.commands)
            self.assertTrue(port.closed)

    def test_no_completion_and_stop_failure_are_not_contact_failures(self):
        port = FakePort(); port.replies[t.STROKE] = 'PUSH: Starting...\r\n'
        state, _, _ = self.episode('stroke', port=port)
        self.assertEqual(state['status'], 'NO_COMPLETION')
        self.assertTrue(port.closed)
        port = FakePort(); port.replies['STOP'] = ''
        state, _, _ = self.episode('stroke', port=port)
        self.assertEqual(state['status'], 'STOP_UNCONFIRMED')

    def test_close_error_never_claims_closure(self):
        port = FakePort()
        with patch.object(port, 'close', side_effect=OSError('close failed')):
            state, _, _ = self.episode('stroke', port=port)
        self.assertEqual(state['status'], 'CLOSE_FAILED')
        self.assertFalse(state.get('descriptor_closed', False))

    def test_exact_identity_no_fallback_or_duplicate(self):
        self.assertIsNone(t.exact_port([dict(ACT, serial='old-controller')], t.ACTUATOR))
        with self.assertRaises(t.Failure): t.exact_port([ACT, ACT], t.ACTUATOR)

    def test_status_parser_does_not_accept_substrings(self):
        self.assertTrue(t.status_valid(STATUS))
        for changed in (STATUS.replace('PWM: 0', 'PWM: 01'),
                        STATUS.replace('Target speed: 128', 'Target speed: 1280'),
                        STATUS.replace('Stylus: OFF', 'Stylus: ON')):
            self.assertFalse(t.status_valid(changed))

    def test_unapproved_commands_cannot_write(self):
        link = t.RawPort.__new__(t.RawPort)
        for cmd in ('PUSH:501,200,500', 'SPEED:100', 'EXTEND', 'STYLUS', 'RETRACT'):
            with self.assertRaises(ValueError): link.exchange(cmd, 1)

    def test_darwin_unexported_flow_control_flags_and_cignore_are_cleared(self):
        c = [0, 0, 0x001F0001, 0, 0, 0, [0] * 32]
        with patch.object(t.sys, 'platform', 'darwin'):
            result = t.raw_settings(c)
        self.assertEqual(result[2] & 0x001F0001, 0)

    def test_inherited_hardware_flow_control_cleared_without_mutating_input(self):
        class Flags:
            HUPCL=1; PARENB=2; CSTOPB=4; CSIZE=8; CLOCAL=16; CREAD=32; CS8=64
            CRTSCTS=128; CCTS_OFLOW=256; CRTS_IFLOW=512; CDTR_IFLOW=1024
            CDSR_OFLOW=2048; CCAR_OFLOW=4096; B115200=115200; VMIN=0; VTIME=1
        c = [9, 9, 8191, 9, 0, 0, [9, 9]]
        result = t.raw_settings(c, Flags)
        self.assertEqual(result[2], Flags.CLOCAL | Flags.CREAD | Flags.CS8)
        self.assertEqual(result[0:2], [0, 0]); self.assertEqual(result[3], 0)
        self.assertEqual(result[4:6], [115200, 115200]); self.assertEqual(result[6], [0, 0])
        self.assertEqual(c[6], [9, 9]); self.assertEqual(c[2], 8191)


class SupervisorTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.out = Path(self.tmp.name)
    def tearDown(self): self.tmp.cleanup()

    def test_real_child_timeout_is_reaped(self):
        r = t.supervise([sys.executable, '-c', 'import time; time.sleep(60)'], self.out, .1)
        self.assertEqual(r['status'], 'CONTROLLER_TIMEOUT')
        self.assertTrue(r['worker_reaped'] and r['group_absent'])
        self.assertFalse(r.get('descriptor_closed', False))

    def test_real_sigterm_ignoring_child_is_killed_and_reaped(self):
        code = 'import signal,time;signal.signal(signal.SIGTERM,signal.SIG_IGN);time.sleep(60)'
        r = t.supervise([sys.executable, '-c', code], self.out, .3)
        self.assertEqual(r['worker_exit_code'], -signal.SIGKILL)
        self.assertTrue(r['worker_reaped'] and r['group_absent'])
        self.assertEqual(r['status'], 'CONTROLLER_TIMEOUT')

    def test_outer_termination_reaps_worker_before_return(self):
        ready = self.out/'ready'
        worker = ('from pathlib import Path; import time; '
                  'Path(' + repr(str(ready)) + ').write_text("ready"); time.sleep(60)')
        code = ('import sys; from pathlib import Path; '
                'sys.path.insert(0,' + repr(str(Path(t.__file__).parent)) + '); '
                'import tapctl; tapctl.supervise([sys.executable,"-c",' + repr(worker) + '],'
                'Path(' + repr(str(self.out)) + '),60)')
        owner = subprocess.Popen([sys.executable, '-c', code])
        try:
            end = time.monotonic() + 3
            while not ready.exists() and time.monotonic() < end: time.sleep(.01)
            self.assertTrue(ready.exists())
            owner.terminate(); owner.wait(timeout=6)
            result = json.loads((self.out/'RESULT.json').read_text())
            self.assertEqual(result['status'], 'CONTROLLER_TIMEOUT')
            self.assertTrue(result['worker_reaped'] and result['group_absent'])
        finally:
            if owner.poll() is None: owner.kill(); owner.wait(timeout=3)

    def test_permission_and_unreapable_errors_do_not_escape(self):
        class Child:
            pid=987654; returncode=None
            def wait(self, timeout): raise subprocess.TimeoutExpired('fake', timeout)
            def poll(self): return None
        with patch.object(t.subprocess, 'Popen', return_value=Child()), \
                patch.object(t.os, 'killpg', side_effect=PermissionError('denied')):
            r = t.supervise(['fake'], self.out, .1)
        self.assertEqual(r['status'], 'CONTROLLER_TIMEOUT')
        self.assertFalse(r['worker_reaped']); self.assertFalse(r['group_absent'])
        self.assertTrue(any('PermissionError' in x for x in r['supervisor_errors']))

    def test_success_without_closed_descriptor_rejected(self):
        (self.out/'WORKER.json').write_text(json.dumps(dict(status='READY', stop_ack=True)))
        r = t.supervise([sys.executable, '-c', 'pass'], self.out, 3)
        self.assertEqual(r['status'], 'CLOSURE_UNPROVED')

    def test_timeout_blocks_automatic_reuse(self):
        bad = dict(status='CONTROLLER_TIMEOUT', receipt_dir=str(self.out))
        with patch.object(t, '_REVIEW_REQUIRED', None), patch.object(t, 'supervise', return_value=bad) as run, \
                patch.object(t.tempfile, 'mkdtemp', return_value=str(self.out)):
            self.assertEqual(t.run('probe')['status'], 'CONTROLLER_TIMEOUT')
            self.assertEqual(t.run('stroke')['status'], 'REVIEW_REQUIRED')
            self.assertEqual(run.call_count, 1)

    def test_default_wake_does_not_retry_contact_failure(self):
        with patch.object(t, 'run', return_value={'status':'NO_HALO_WAKE','receipt_dir':'inert'}) as run:
            tap = t.Tapper(verbose=False)
            self.assertFalse(tap.wake())
            self.assertEqual(run.call_count, 1)
            self.assertEqual(tap.last_result['status'], 'NO_HALO_WAKE')
            tap.close()
            with self.assertRaises(t.TapError): tap.stroke_once()

    def test_legacy_soak_aborts_without_waiting_or_clearing_review_latch(self):
        # Compile the actual function without importing the executable soak script,
        # whose top level starts a serial reader and performs device operations.
        source = Path(t.__file__).with_name('soak_cycles.py')
        tree = ast.parse(source.read_text())
        function = next(node for node in tree.body
                        if isinstance(node, ast.FunctionDef) and node.name == 'wait_for_rig')
        latch = dict(status='CONTROLLER_TIMEOUT', receipt_dir='inert-review-receipt')
        tap = t.Tapper(verbose=False)
        messages = []
        namespace = dict(_tap=[tap], time=time, tapctl=t, rig_stalls=[0],
                         print=lambda *args, **kwargs: messages.append(' '.join(map(str, args))))
        exec(compile(ast.Module(body=[function], type_ignores=[]), str(source), 'exec'), namespace)
        with patch.object(t, '_REVIEW_REQUIRED', latch), \
                patch.object(t, 'supervise', side_effect=AssertionError('must not launch worker')), \
                patch.object(time, 'sleep', side_effect=AssertionError('must not wait')), \
                patch.object(tap, 'close', side_effect=AssertionError('must not recreate controller')):
            self.assertFalse(namespace['wait_for_rig']())
            self.assertIs(t._REVIEW_REQUIRED, latch)
        self.assertIs(namespace['_tap'][0], tap)
        self.assertEqual(namespace['rig_stalls'], [0])
        self.assertEqual(tap.last_result['status'], 'REVIEW_REQUIRED')
        self.assertTrue(any('inert-review-receipt' in msg for msg in messages))

    def test_controller_failure_is_typed_error_not_contact_failure(self):
        with patch.object(t, 'run', return_value={'status':'CONTROLLER_TIMEOUT','receipt_dir':'inert'}):
            with self.assertRaises(t.TapError) as cm: t.Tapper(verbose=False).wake()
            self.assertEqual(cm.exception.result['status'], 'CONTROLLER_TIMEOUT')


if __name__ == '__main__':
    unittest.main()
