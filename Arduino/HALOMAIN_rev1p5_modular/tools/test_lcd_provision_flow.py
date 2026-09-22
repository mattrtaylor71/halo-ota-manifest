#!/usr/bin/env python3
"""Exercise the actual LCD guide helper on the host; no hardware/network calls."""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1] / 'LCD_Minimal'
source = r'''
#include "lcd_provision_flow.h"
#include <assert.h>
#include <stdio.h>
int main() {
  LcdProvisionFlow f;
  assert(!f.status("connected", 0)); // A normal provisioned wake never opens success.
  f.begin();
  assert(!f.status("connected", 1) && f.step == f.GetApp); // Old-network heartbeat after reset.
  assert(f.step == f.GetApp && !f.scroll(-1));
  assert(f.scroll(20) && f.step == f.AppRoute);
  assert(f.scroll(1) && f.step == f.PairQr && !f.scroll(1));
  assert(f.scroll(-1) && f.scroll(-1));
  assert(f.status("app_connected", 10) && f.step == f.SelectWifi);
  assert(!f.scroll(1) && !f.scroll(-1));
  assert(!f.status("ap_setup", 11));
  assert(!f.status("app_connected", 12));
  assert(f.status("connecting", 20) && f.step == f.Connecting);
  assert(f.status("claiming", 40) && f.claiming);
  assert(!f.status("app_connected", 45) && f.step == f.Connecting);
  assert(!f.status("claiming", 1000));
  assert(f.status("connected", 2000) && f.step == f.Complete);
  assert(f.progress_sleep_pending(2000));
  const uint32_t completion_deadline = f.progress_sleep_deadline_ms.load();
  assert(!f.status("connected", 3000)); // Heartbeats do not extend the welcome screen.
  assert(!f.status("failed", 4000));
  assert(f.progress_sleep_deadline_ms.load() == completion_deadline);
  assert(!f.completion_due(6499) && f.completion_due(6500));
  assert(f.progress_sleep_pending(16499) && !f.progress_sleep_pending(16500));
  f.close();
  assert(!f.progress_sleep_pending(6500));
  assert(!f.active() && !f.status("connected", 6600));
  for (int page = 1; page <= 3; ++page) {
    f.begin();
    for (int i = 1; i < page; ++i) f.scroll(1);
    assert(f.status("app_connected", 0) && f.step == f.SelectWifi);
    f.close();
  }
  f.begin();
  assert(f.status("claiming", 0) && f.step == f.Connecting); // Catch up missed events.
  assert(!f.check_timeout(239999));
  assert(f.check_timeout(240000) && f.step == f.Failed);
  assert(!f.status("claiming", 240001)); // Repeated stuck status cannot hide the error.
  assert(f.status("connected", 240002) && f.step == f.Complete); // Real recovery wins.
  f.close(); f.begin();
  assert(f.status("error", 0) && f.step == f.Failed);
  f.retry(true);
  assert(f.step == f.PairQr && f.scrollable());
  f.status("connecting", 0xfffffff0);
  assert(!f.check_timeout(32));
  assert(f.check_timeout(0xfffffff0u + 240000u));
  f.close(); f.begin(); f.status("claiming", 0xffffffe0); f.status("connected", 0xfffffff0);
  assert(f.completion_due(0xfffffff0u + 4500u));
  assert(f.progress_sleep_pending(0xfffffff0u + 14499u));
  assert(!f.progress_sleep_pending(0xfffffff0u + 14500u));
  puts("PASS LCD guide: normal wake, scroll bounds, app latch from all pages, retries, heartbeat stability, 240s timeout, 4.5s success, timer rollover");
}
'''
compiler = shutil.which('clang++') or shutil.which('g++')
assert compiler
with tempfile.TemporaryDirectory(prefix='halo-provision-flow-') as folder:
    cpp, binary = Path(folder)/'test.cpp', Path(folder)/'test'
    cpp.write_text(source)
    subprocess.run([compiler, '-std=c++11', '-Wall', '-Wextra', '-Werror', '-I', str(root), str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
