#!/usr/bin/env python3
"""Run the actual driver shutdown against active, absent and faulty I2C devices."""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    # There must be no remaining input/event route that can start a waveform.
    for path in (ROOT / 'LCD_Minimal').glob('*'):
        if path.suffix in ('.ino', '.h', '.c', '.cpp'):
            text = path.read_text()
            assert 'haptic_pulse' not in text, path
            assert 'EVT_HAPTIC_TICK' not in text, path
    activity = (ROOT / 'LCD_Minimal/lcd_activity.h').read_text()
    assert 'init_touch_once();\n  haptic_init();' in activity
    uart = (ROOT / 'LCD_Minimal/lcd_uart_task.h').read_text()
    assert 'strcmp(usb_buf, "haptics") == 0' in uart
    harness = r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdarg>
#include <cstring>
using i2c_port_t = int;
constexpr int I2C_NUM_0 = 0, ESP_OK = 0;
#define pdMS_TO_TICKS(ms) (ms)
static uint8_t regs[256];
static unsigned writes, reads, fail_write, fail_read;
struct Log { char last[256]{};
  void printf(const char* format, ...) {
    va_list a; va_start(a, format); vsnprintf(last,sizeof(last),format,a); va_end(a);
  }
} Serial;
static int i2c_master_write_to_device(int port, uint8_t addr, const uint8_t* p,
                                    size_t n, unsigned timeout) {
  assert(port==0 && addr==0x5A && n==2 && timeout==20);
  static constexpr uint8_t expected[][2]={{1,0x40},{0x0c,0},{2,0}};
  assert(p[0]==expected[writes%3][0] && p[1]==expected[writes%3][1]);
  ++writes;
  if(fail_write & (1U<<((writes-1)%3))) return -1;
  regs[p[0]]=p[1]; return ESP_OK;
}
static int i2c_master_write_read_device(int port,uint8_t addr,const uint8_t* p,
                                      size_t n,uint8_t* out,size_t len,unsigned timeout) {
  assert(port==0 && addr==0x5A && n==1 && len==1 && timeout==20);
  assert(*p==1 || *p==0x0c || *p==2); ++reads;
  if(fail_read & (1U<<((reads-1)%3))) return -1;
  *out=regs[*p]; return ESP_OK;
}
#include "LCD_Minimal/lcd_haptics.h"
static void active() {
  memset(regs,0,sizeof(regs));regs[1]=0;regs[0x0c]=1;regs[2]=127;
  writes=reads=fail_write=fail_read=0;
}
int main() {
  active();haptic_init();assert(writes==3 && reads==3);
  assert(regs[1]==0x40 && regs[0x0c]==0 && regs[2]==0);
  assert(strstr(Serial.last,"standby_verified=1"));
  // Diagnostic reads cannot mutate state or re-enable output.
  assert(haptic_report());assert(writes==3 && reads==6);
  // Repeat boot initialization is still exclusively stop/standby operations.
  haptic_init();assert(writes==6 && haptic_report());
  // Any failed stop write from a previously active driver remains unverified.
  for(unsigned mask=1;mask<8;++mask) {
    active();fail_write=mask;haptic_init();assert(writes==3 && reads==3);
    assert(strstr(Serial.last,"standby_verified=0"));
  }
  // Missing/partial readback is not proof of physical standby.
  for(unsigned mask=1;mask<8;++mask) {
    active();fail_read=mask;haptic_init();assert(writes==3 && reads==3);
    assert(strstr(Serial.last,"standby_verified=0"));
  }
  active();haptic_init();regs[1]=0xc0;assert(!haptic_report());
  regs[1]=0x45;assert(!haptic_report()); // Standby with retained real-time playback mode.
  puts("PASS: disabled input paths, warm boot shutdown, read-only observation, I2C failures");
}
'''
    compiler = shutil.which('c++')
    assert compiler, 'C++ compiler required'
    with tempfile.TemporaryDirectory(prefix='halo-haptics-') as tmp:
        src = Path(tmp) / 'main.cpp'
        exe = Path(tmp) / 'test'
        src.write_text(harness)
        subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT), str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
