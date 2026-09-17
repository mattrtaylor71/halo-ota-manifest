#!/usr/bin/env python3
"""Inert tests of the actual relay-cycle owner; no serial import or hardware."""
import importlib.util
from pathlib import Path
import signal

spec = importlib.util.spec_from_file_location('relay_reset', Path(__file__).with_name('actuator_relay_reset.py'))
m = importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
checks = 0

def check(value):
    global checks
    assert value
    checks += 1

for command, reply in m.ACKS.items():
    check(m.exact_ack(reply + '\r\n', command))
    check(not m.exact_ack('prefix' + reply, command))
    check(not m.exact_ack(reply + '\nERROR\n', command))
check(not m.exact_ack('OK+CH1=1\n', 'AT+CH1=0'))
check(not m.exact_ack('OK\n', 'AT+CH1=0'))

class Fake:
    def __init__(self, fault=None, degraded=False):
        self.fault=fault;self.degraded=degraded;self.now=0;self.commands=[];self.closed=False;self.off=False;self.restored=False;self.state={}
    def clock(self):return self.now
    def sleep(self,seconds):self.now+=seconds
    def scan(self):
        if self.off and self.fault=='scan':raise OSError('inventory fault')
        if self.off and self.fault=='signal':m.interrupt_handler(self.state,signal.SIGTERM)
        rows=[dict(port='/dev/relay',serial=m.RELAY[0],vid=m.RELAY[1],pid=m.RELAY[2])]
        if self.fault!='no_return' and (self.restored or (not self.off and not self.degraded)):
            rows.append(dict(port='/dev/actuator',serial=m.ACTUATOR[0],vid=m.ACTUATOR[1],pid=m.ACTUATOR[2]))
        return rows
    def owner(self,port):
        if self.fault=='owned':raise RuntimeError('owned')
    def connect(self,port):
        if self.fault=='connect':raise OSError('open failed')
        return self
    def exchange(self,command):
        self.commands.append(command)
        if command=='AT':return 'wrong' if self.fault=='at' else 'OK\n'
        if command=='AT+CH1=1':
            self.off=True
            if self.fault=='off_write':raise OSError('partial off write')
            return 'OK\n' if self.fault=='off_ack' else 'OK+CH1=1\n'
        if self.fault=='restore_all' or (self.fault=='restore_first' and self.commands.count(command)==1):raise OSError('restore I/O error')
        if self.fault=='restore_stale':return 'OK+CH1=1\n'
        if self.fault=='restore_signal':m.interrupt_handler(self.state,signal.SIGTERM)
        self.off=False;self.restored=True;return 'OK+CH1=0\n'
    def close(self):
        self.closed=True
        if self.fault=='close':raise OSError('close failed')
    def run(self,allow_degraded=False):
        def emit(kind,**kw):pass
        success=m.cycle(self.state,emit,self.scan,self.owner,self.connect,self.clock,self.sleep,10,3,allow_degraded)
        return success

f=Fake();check(f.run());check(f.now>=10);check(f.closed);check(f.commands==['AT','AT+CH1=1','AT+CH1=0']);check(f.state['restore_ack']);check(f.state['actuator_disappeared'])
for fault in ['owned','connect','at']:
    f=Fake(fault);check(not f.run());check('AT+CH1=1' not in f.commands)
for fault in ['off_write','off_ack','scan','signal']:
    f=Fake(fault);check(not f.run());check(f.state['restore_ack']);check(f.commands[-1]=='AT+CH1=0');check(f.closed)
f=Fake('restore_first');check(f.run());check(f.commands.count('AT+CH1=0')==2)
for fault in ['restore_all','restore_stale']:
    f=Fake(fault);check(not f.run());check(f.commands.count('AT+CH1=0')==2);check(f.state['status']=='RESTORE_UNCONFIRMED');check(f.closed)
f=Fake('restore_signal');check(not f.run());check(f.state['restore_ack']);check(f.closed)
f=Fake('close');check(not f.run());check(f.state['restore_ack'])
f=Fake(degraded=True);check(not f.run());check(not f.commands)
f=Fake(degraded=True);check(f.run(True));check(f.state['status']=='BSD_SERIAL_RETURNED_AFTER_RESTORE');check(not f.state['actuator_disappeared'])
# A return failure still closes after acknowledged NC restoration.
f=Fake();original=f.scan
f.scan=lambda: [p for p in original() if not (f.restored and m.matches(p,m.ACTUATOR))]
check(not f.run());check(f.state['status']=='USB_RETURN_NOT_PROVED');check(f.state['restore_ack']);check(f.closed)
check(set(f.commands)<=set(m.ACKS))
f=Fake();check(f.run(True));check(f.state['status']=='BSD_DISAPPEAR_RETURN_PROVED')
check(m.restoration_unconfirmed({},True,True))
check(m.restoration_unconfirmed({'status':'WORKER_RECEIPT_UNAVAILABLE'},False,True))
check(not m.restoration_unconfirmed({'restore_ack':True},True,True))
check(m.restoration_unconfirmed({'restore_ack':True},True,False))
print('PASS %d inert relay recovery checks; no device or actuator commands executed' % checks)
