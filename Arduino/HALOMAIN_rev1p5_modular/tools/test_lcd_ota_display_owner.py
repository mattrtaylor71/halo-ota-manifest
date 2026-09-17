"""Execute actual UART OTA_LOCK and UI wake paths with a forced interleaving.

Host-only: a simulated SPI call blocks the UI just after publication of the
overlay. The old UART path then enters that same device concurrently. The
corrected path publishes intent and leaves all panel work on the LVGL owner.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_manual_ota_clock import definition

ROOT = Path(__file__).resolve().parents[1]


def harness(uart, baseline=False):
    ui = (ROOT / 'LCD_Minimal/lcd_ui_task.h').read_text()
    main = (ROOT / 'LCD_Minimal/LCD_Minimal.ino').read_text()
    anim = (ROOT / 'LCD_Minimal/lcd_anim.h').read_text()
    start = uart.index('    if (!coord[0]) lcd_allow_visible_ui("ota_lock");')
    end = uart.index('    Serial.println("[OTA] lock received - blocking LCD OTA");', start)
    legacy = uart[start:end]
    prelude_start = ui.index('    if (g_ota_screen_active) {')
    prelude_end = ui.index('      // Dark designer system surface.', prelude_start)
    prelude = ui[prelude_start:prelude_end]
    receive_guard = definition(ui, 'if (g_lcd_ota_uart_receiving || g_lcd_ota_binary_mode)')
    render_start = ui.index('      // Render the existing overlay during preflight.')
    render = definition(ui[render_start:], 'if (!g_lcd_ota_uart_receiving && !g_lcd_ota_binary_mode)')
    if not baseline:
        assert 'lcd_set_idle_screen_dark(' not in legacy
        assert 'lcd_panel_set_power(' not in legacy
        assert 'example_lvgl_lock(' not in legacy
    prefix = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
using namespace std::chrono;
static std::mutex sync_mutex;
static std::condition_variable changed;
static bool published=false,ui_in_panel=false,release_ui=false,race_enabled=false;
static std::thread::id ui_id,uart_id;
static std::timed_mutex lvgl;
static thread_local bool owns_lvgl=false;
static unsigned panel_calls=0,owner_violations=0,concurrent_calls=0,renders=0,backlights=0;
static std::atomic<unsigned> panel_busy{0};
struct ScreenFlag {
 std::atomic<bool> value{false};
 operator bool()const{return value.load();}
 void operator=(bool v){
  value.store(v);
  if(v&&race_enabled&&std::this_thread::get_id()==uart_id){
   std::unique_lock<std::mutex> lock(sync_mutex);published=true;changed.notify_all();
   assert(changed.wait_for(lock,seconds(2),[]{return ui_in_panel;}));
  }
 }
}g_ota_screen_active;
static bool g_background_wake_dark=true,g_idle_screen_dark=true,g_sleep_transition=false;
static bool g_in_light_sleep=false,g_ota_mode_active=false,g_lvgl_running=false,g_panel_enabled=false;
static bool g_lcd_initialized=true,g_lcd_ota_uart_receiving=false,g_lcd_ota_binary_mode=false;
static int g_backlight_duty=0;
static bool ota_locked=false,ota_check_requested=false,ota_check_pending=false;
static bool g_lcd_ota_recovery_grace=false,g_lcd_maintenance_active=false;
static uint32_t ota_lock_at_ms=0,g_lcd_maintenance_boot_grace_until_ms=0;
static uint32_t g_lcd_maintenance_deadline_ms=0,ota_stay_awake_until_ms=0,g_ota_lock_window_until_ms=0;
static std::atomic<bool> g_lcd_sleep_commit_gate{false};
static constexpr uint32_t OTA_LOCK_TIMEOUT_MS=180000,LCD_OTA_LOCK_STAY_AWAKE_MS=180000;
struct LcdCoordCriticalGuard{};
static uint32_t millis(){return 8000;}
static void delay(unsigned){}
#define pdMS_TO_TICKS(n) (n)
static void vTaskDelay(unsigned){}
static bool lv_is_initialized(){return true;}
static void lcd_lvgl_wait_tx_done(unsigned){}
static bool example_lvgl_lock(int ms){bool ok=lvgl.try_lock_for(milliseconds(ms));owns_lvgl=ok;return ok;}
static void example_lvgl_unlock(){assert(owns_lvgl);owns_lvgl=false;lvgl.unlock();}
static void lcd_timer_receiver_wait_release(const char*){}
static void lcd_set_backlight_binary(bool on,const char*){++backlights;g_backlight_duty=on?128:0;}
static void lv_timer_handler(){assert(owns_lvgl&&g_panel_enabled);++renders;}
// This suite owns the independent ordinary render/owner boundary. The actual
// token/frame-submit handshake is exercised by test_lcd_ota_install_frame.py.
static bool install_frame=false;
static uint32_t install_token=0;
static void* ota_overlay=nullptr;
static void lcd_ota_install_frame_submit(uint32_t,void*){assert(false);}
static struct {template<class...T>void printf(const char*,T...){}void println(const char*){}}Serial;
static void lcd_panel_set_power(bool on){
 assert(on);++panel_calls;if(!owns_lvgl)++owner_violations;
 if(panel_busy.fetch_add(1))++concurrent_calls;
 if(race_enabled&&std::this_thread::get_id()==ui_id){
  std::unique_lock<std::mutex> lock(sync_mutex);ui_in_panel=true;changed.notify_all();
  assert(changed.wait_for(lock,seconds(2),[]{return release_ui;}));
 }
 --panel_busy;
}
'''
    functions = '\n'.join([
        definition(main, 'static void lcd_allow_visible_ui('),
        definition(anim, 'static void lcd_set_idle_screen_dark('),
        'static void uart_legacy_lock(){const char*coord="";\n' + legacy + '\n}',
        'static void ui_iteration(){for(unsigned once=0;once<1;++once){assert(example_lvgl_lock(50));\n'
        + receive_guard + '\n' + prelude + '\n' + render + '\n}\nexample_lvgl_unlock();}}',
    ])
    cases = r'''
static void reset(){
 assert(panel_busy==0&&!owns_lvgl);
 g_background_wake_dark=true;g_idle_screen_dark=true;g_sleep_transition=false;g_in_light_sleep=false;
 g_ota_mode_active=false;g_lvgl_running=false;g_panel_enabled=false;g_backlight_duty=0;
 g_lcd_ota_uart_receiving=false;g_lcd_ota_binary_mode=false;g_ota_screen_active.value=false;
 published=ui_in_panel=release_ui=false;race_enabled=false;
 panel_calls=owner_violations=concurrent_calls=renders=backlights=0;
}
int main(){
 reset();race_enabled=true;
 std::thread user([]{
  ui_id=std::this_thread::get_id();
  {std::unique_lock<std::mutex>lock(sync_mutex);assert(changed.wait_for(lock,seconds(2),[]{return published;}));}
  ui_iteration();
 });
 std::thread uart([]{
  uart_id=std::this_thread::get_id();uart_legacy_lock();
  {std::lock_guard<std::mutex>lock(sync_mutex);release_ui=true;}changed.notify_all();
 });
 uart.join();user.join();
#if BASELINE
 assert(owner_violations==1&&concurrent_calls==1&&panel_calls==2);
 puts("REPRODUCED actual baseline: UART enters the same panel while UI owns its first wake/render");
 return 0;
#else
 assert(owner_violations==0&&concurrent_calls==0&&panel_calls==1&&renders==1);
 assert(g_panel_enabled&&g_lvgl_running&&!g_idle_screen_dark&&g_backlight_duty==128);
 puts("PASS actual interleaving: UART publishes only; UI owns exactly one dark-to-visible panel wake");
 // BEGIN can inhibit the UI after publication. No late panel mutation/render
 // is allowed until existing cleanup releases the receiving/binary flags.
 for(unsigned binary=0;binary<2;++binary){
  reset();g_lcd_ota_uart_receiving=!binary;g_lcd_ota_binary_mode=binary;
  uart_legacy_lock();assert(panel_calls==0);ui_iteration();
  assert(panel_calls==0&&renders==0&&!owns_lvgl);
  g_lcd_ota_uart_receiving=g_lcd_ota_binary_mode=false;ui_iteration();
  assert(panel_calls==1&&renders==1&&owner_violations==0&&g_panel_enabled);
 }
 // Already-visible manual UI does not power-cycle when legacy LOCK arrives.
 reset();g_background_wake_dark=g_idle_screen_dark=false;
 g_panel_enabled=g_lvgl_running=true;g_backlight_duty=128;
 uart_legacy_lock();ui_iteration();assert(panel_calls==0&&renders==1&&owner_violations==0);
 puts("PASS actual UI controls: receive/binary inhibition, cleanup wake, already-visible manual overlay unchanged");
#endif
}
'''
    return '\n'.join(['#define BASELINE '+str(int(baseline)), prefix, functions, cases])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--baseline-ref')
    args = parser.parse_args()
    if args.baseline_ref:
        uart = subprocess.check_output(['git', 'show', args.baseline_ref + ':./LCD_Minimal/lcd_uart_rx.h'], cwd=ROOT, text=True)
    else:
        uart = (ROOT / 'LCD_Minimal/lcd_uart_rx.h').read_text()
    with tempfile.TemporaryDirectory(prefix='halo-ota-display-owner-') as directory:
        cpp,binary=Path(directory)/'test.cpp',Path(directory)/'test'
        cpp.write_text(harness(uart, bool(args.baseline_ref)))
        subprocess.run([shutil.which('c++'),'-std=c++17','-pthread',str(cpp),'-o',str(binary)],check=True,timeout=30)
        subprocess.run([str(binary)],check=True,timeout=8)


if __name__ == '__main__':
    main()
