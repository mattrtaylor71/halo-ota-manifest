#!/usr/bin/env python3
"""Exercise actual LCD owner/DMA handoff and UI guards with host-only I/O.

Optional --baseline-root extracts the pre-fix receive handoff from that source
snapshot and reproduces erase overlapping an already-owned render. No hardware.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def definition(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--baseline-root', type=Path)
    args = parser.parse_args()
    bsp = (ROOT / 'LCD_Minimal/lcd_bsp.c').read_text()
    ota = (ROOT / 'LCD_Minimal/lcd_ota_uart.h').read_text()
    ui = (ROOT / 'LCD_Minimal/lcd_ui_task.h').read_text()
    loop = (ROOT / 'LCD_Minimal/LCD_Minimal.ino').read_text().split('void loop() {', 1)[1]
    begin = definition(ota, 'static void lcd_ota_handle_begin(JsonObject& doc) {')
    assert begin.index('lcd_ota_quiesce_before_begin()') < begin.index('lcd_diag_prepare_attempt(') < begin.index('esp_ota_begin(')
    assert begin.index('lcd_ota_quiesce_before_begin()') < begin.index('lcd_ota_nvs_load_progress(')
    refused = definition(begin, 'if (!lcd_ota_quiesce_before_begin())')
    assert '"render_not_quiescent"' in refused and 'resp["json_ready"] = true;' in refused and 'return;' in refused
    assert all(x not in refused for x in ('esp_ota_', 'lcd_ota_nvs_', 'lcd_diag_'))
    quiesce = definition(bsp, 'bool lcd_lvgl_quiesce_for_flash(')
    assert 'lcd_lvgl_wait_tx_done' not in quiesce and 'delay(10)' not in quiesce
    timeout = definition(bsp, 'void lcd_bsp_check_flush_timeout(')
    assert 's_flush_dma_pending' not in timeout  # force-ready cannot invent DMA completion
    fallback = definition(loop, 'if (s_flush_reset_cycles >= 5 && flush_ok == 0)')
    assert fallback.index('example_lvgl_unlock()') < fallback.index('enterLightSleep()') < fallback.index('return;')
    assert loop.index('example_lvgl_lock(50)') < loop.index('lcd_bsp_display_reset_requested()')
    assert loop.index('if (g_lcd_ota_uart_receiving || g_lcd_ota_binary_mode)') < loop.index('lcd_bsp_display_reset_requested()')
    ui_guard = definition(ui, 'if (g_lcd_ota_uart_receiving || g_lcd_ota_binary_mode)')
    assert ui.index('lcd_freeze_wdt_feed()') < ui.index(ui_guard) < ui.index('lcd_panel_set_power(true)')
    query = definition(ota, 'static void lcd_ota_handle_query(')
    idle_expression = query.split('doc["recovery_idle"] = ', 1)[1].split(';', 1)[0]
    old_handoff = '' 
    if args.baseline_root:
        old = (args.baseline_root / 'LCD_Minimal/lcd_ota_uart.h').read_text()
        old_begin = definition(old, 'static void lcd_ota_handle_begin(JsonObject& doc) {')
        assert 'lcd_lvgl_quiesce_for_flash' not in old_begin and 'example_lvgl_lock' not in old_begin
        start = old_begin.index('    s_lcd_ota_state = LCD_OTA_RECEIVING;')
        end = old_begin.index('    s_lcd_ota_last_aborted_session = 0;', start)
        old_handoff = old_begin[start:end]
    harness = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <algorithm>
#include "lcd_ota_install_frame.h"
using namespace std::chrono;
static auto epoch=steady_clock::now();
static uint32_t millis(){return duration_cast<milliseconds>(steady_clock::now()-epoch).count();}
static uint64_t esp_timer_get_time(){return duration_cast<microseconds>(steady_clock::now()-epoch).count();}
#define pdMS_TO_TICKS(x) (x)
static void vTaskDelay(uint32_t ms){std::this_thread::sleep_for(milliseconds(ms));}
static std::timed_mutex owner;
static std::atomic<unsigned> held{0};
static bool example_lvgl_lock(int ms){bool yes=owner.try_lock_for(milliseconds(ms));if(yes)++held;return yes;}
static void example_lvgl_unlock(){--held;owner.unlock();}
static void* lvgl_mux=(void*)1;
using esp_lcd_panel_io_handle_t=void*;
using esp_lcd_panel_handle_t=void*;
struct esp_lcd_panel_io_event_data_t{};
static void* amoled_panel_io_handle=(void*)1;
static void* amoled_panel_handle=(void*)1;
struct lv_disp_drv_t{void* user_data;};
struct lv_area_t{int x1,x2,y1,y2;};
using lv_color_t=int;
static lv_disp_drv_t driver{(void*)1};
static lv_area_t area{0,3,0,3};
static lv_disp_drv_t* s_flush_pending_drv=nullptr;
static uint32_t s_flush_dma_pending=0,flush_outstanding_count=0;
static unsigned long s_flush_start_ms=0;
static uint32_t s_flush_submit_ok=0,s_flush_submit_fail=0,flush_fail_count=0;
static int s_flush_soft_fault=0;
static bool display_reset_requested=false,g_sleep_transition=false,g_background_wake_dark=false;
static unsigned ready_calls=0;
static void lv_disp_flush_ready(lv_disp_drv_t*){++ready_calls;}
using esp_err_t=int;
#define ESP_OK 0
#define SH8601_PANEL_IO_QSPI_TRANS_QUEUE_DEPTH 10
#define FLUSH_FAIL_RESET_THRESHOLD 10
static int submit_error=0;
static bool complete_before_submit_return=false;
static bool example_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t,esp_lcd_panel_io_event_data_t*,void*);
static int esp_lcd_panel_draw_bitmap(void*,int,int,int,int,void*){
  if(complete_before_submit_return&&!submit_error)example_notify_lvgl_flush_ready(nullptr,nullptr,&driver);
  return submit_error;
}
static struct{template<class...T>void printf(const char*,T...){}void println(const char*){}}Serial;
enum{LCD_OTA_IDLE,LCD_OTA_RECEIVING};
static std::atomic<int> s_lcd_ota_state{LCD_OTA_IDLE};
static std::atomic<bool> g_lcd_ota_uart_receiving{false};
static bool g_lcd_ota_binary_mode=false;
static unsigned s_lcd_ota_handle=0;
static void* s_lcd_ota_protocol=nullptr;
static bool g_img_rx_active=false,g_img_rx_binary_mode=false,g_spool_tx_pending=false,g_spool_tx_active=false,g_suppress_uart_json_tx=false;
static bool query_recovery_idle(){return IDLE_EXPRESSION;}
static uint32_t s_lcd_ota_started_ms=0,s_lcd_ota_budget_ms=1000,s_lcd_ota_session_id=42;
static unsigned erased=0,clear_writes=0,restore_calls=0,ui_actions=0;
struct LcdNvsDeadline{static LcdNvsDeadline fromAttempt(uint32_t,uint32_t){return {};}};
static bool g_lcd_ota_show_progress=false,ota_locked=false,ota_check_pending=false,ota_check_requested=false,
 sense_ota_active=false,sense_ota_apply_required=false,g_lcd_maintenance_active=false,g_ota_mode_active=false,
 g_manual_ota_override=false,g_manual_ota_ui_active=false,g_ota_screen_active=false,g_lcd_maintenance_headless=false,
 provision_return_home_pending=false;
static int g_lcd_ota_progress_pct=0;
static uint32_t ota_stay_awake_until_ms=0,g_ota_lock_window_until_ms=0,g_lcd_maintenance_deadline_ms=0,
 g_manual_ota_override_until_ms=0;
static void lcd_clear_persisted_maintenance_state(const char*,const LcdNvsDeadline&){++clear_writes;}
static void resetActivityTimer(){}
static void lcd_ota_arm_recovery_grace(){++restore_calls;}
FUNCTIONS
static void ui_iteration(){
  for(unsigned once=0;once<1;++once){
    assert(example_lvgl_lock(50));
    UI_GUARD
    ++ui_actions;
    example_lvgl_unlock();
  }
}
static void begin_flash(){if(lcd_ota_quiesce_before_begin())++erased;}
static void reset_attempt(){
  assert(held==0&&__atomic_load_n(&s_flush_dma_pending,__ATOMIC_ACQUIRE)==0);
  g_lcd_ota_uart_receiving=false;g_lcd_ota_binary_mode=false;s_lcd_ota_state=LCD_OTA_IDLE;
  s_lcd_ota_started_ms=millis();s_lcd_ota_budget_ms=1000;
}
int main(){
  assert(query_recovery_idle());
  for(bool* owner : {&g_img_rx_active,&g_img_rx_binary_mode,&g_spool_tx_pending,&g_spool_tx_active,&g_suppress_uart_json_tx}){
    *owner=true;assert(!query_recovery_idle());*owner=false;
  }
  s_lcd_ota_state=LCD_OTA_RECEIVING;assert(!query_recovery_idle());s_lcd_ota_state=LCD_OTA_IDLE;
  s_lcd_ota_handle=1;assert(!query_recovery_idle());s_lcd_ota_handle=0;
  s_lcd_ota_protocol=(void*)1;assert(!query_recovery_idle());s_lcd_ota_protocol=nullptr;
  g_lcd_ota_binary_mode=true;assert(!query_recovery_idle());g_lcd_ota_binary_mode=false;
  g_lcd_ota_uart_receiving=true;assert(!query_recovery_idle());g_lcd_ota_uart_receiving=false;
  puts("PASS actual QUERY idle predicate independently rejects every active owner");
  // Optional immutable pre-fix handoff: erase advances while another owner
  // still holds an in-flight render. The current implementation below cannot.
  if(BASELINE_ENABLED){
    assert(example_lvgl_lock(50));
    std::thread prior([]{ OLD_HANDOFF ++erased; });prior.join();
    assert(erased==1&&held==1);example_lvgl_unlock();erased=0;reset_attempt();
    puts("PASS extracted baseline handoff reproduces erase while render owns LVGL");
  }
  // Renderer is already owned when BEGIN arrives, with DMA still in flight.
  assert(example_lvgl_lock(50));
  lv_color_t color=1;example_lvgl_flush_cb(&driver,&area,&color);
  assert(__atomic_load_n(&s_flush_dma_pending,__ATOMIC_ACQUIRE)==1);
  std::thread begin([]{begin_flash();});
  while(!g_lcd_ota_uart_receiving.load())std::this_thread::yield();
  vTaskDelay(10);assert(erased==0&&held==1);
  example_lvgl_unlock(); // owner can finish before its asynchronous DMA does
  vTaskDelay(10);assert(erased==0);
  example_notify_lvgl_flush_ready(nullptr,nullptr,&driver);
  begin.join();assert(erased==1&&held==0&&s_lcd_ota_state==LCD_OTA_RECEIVING);
  ui_iteration();assert(ui_actions==0); // no panel/widget/layout calls
  g_lcd_ota_uart_receiving=false;g_lcd_ota_binary_mode=true;ui_iteration();assert(ui_actions==0);
  g_lcd_ota_binary_mode=false;lcd_ota_uart_restore_ui();reset_attempt();
  ui_iteration();assert(ui_actions==1); // ordinary/manual UI works again
  puts("PASS real owner then DMA completion required, transfer owner released, UI resumes");
  // Force-ready clearing of old bookkeeping is NOT DMA drain proof.
  example_lvgl_flush_cb(&driver,&area,&color);
  s_flush_pending_drv=nullptr;flush_outstanding_count=0;
  unsigned before=erased,writes=clear_writes;
  s_lcd_ota_budget_ms=25;s_lcd_ota_started_ms=millis();begin_flash();
  assert(erased==before&&clear_writes==writes&&!g_lcd_ota_uart_receiving&&s_lcd_ota_state==LCD_OTA_IDLE&&held==0);
  example_notify_lvgl_flush_ready(nullptr,nullptr,&driver);reset_attempt();
  puts("PASS fake flush-ready refuses before erase/NVS and clears receiving flags");
  // Mutex acquisition timeout must also be bounded and release logical state.
  assert(example_lvgl_lock(50));s_lcd_ota_budget_ms=25;s_lcd_ota_started_ms=millis();
  std::thread blocked([]{begin_flash();});blocked.join();
  assert(erased==before&&clear_writes==writes&&!g_lcd_ota_uart_receiving&&held==1);
  example_lvgl_unlock();reset_attempt();
  // No extension of an expired attempt, no uninitialized BSP acceptance.
  s_lcd_ota_budget_ms=0;begin_flash();assert(erased==before&&!g_lcd_ota_uart_receiving);
  reset_attempt();amoled_panel_io_handle=nullptr;begin_flash();assert(erased==before&&!g_lcd_ota_uart_receiving);
  amoled_panel_io_handle=(void*)1;reset_attempt();
  // An unusually fast real completion can precede the submit call's return.
  complete_before_submit_return=true;example_lvgl_flush_cb(&driver,&area,&color);
  complete_before_submit_return=false;
  assert(__atomic_load_n(&s_flush_dma_pending,__ATOMIC_ACQUIRE)==0&&s_flush_pending_drv==nullptr);
  // Failed DMA submission retires only its own counted work and allows retry.
  submit_error=1;example_lvgl_flush_cb(&driver,&area,&color);submit_error=0;
  assert(__atomic_load_n(&s_flush_dma_pending,__ATOMIC_ACQUIRE)==0);
  begin_flash();assert(erased==before+1&&held==0&&g_lcd_ota_uart_receiving);
  puts("PASS join/deadline/init refusal; no stuck flag; subsequent manual BEGIN succeeds");
}
'''
    functions = '\n'.join((quiesce,
        definition(bsp,'static bool example_notify_lvgl_flush_ready('),
        definition(bsp,'static void example_lvgl_flush_cb('),
        definition(ota,'static void lcd_ota_uart_restore_ui('),
        definition(ota,'static bool lcd_ota_quiesce_before_begin(')))
    harness = harness.replace('FUNCTIONS', functions).replace('UI_GUARD',ui_guard).replace('IDLE_EXPRESSION',idle_expression)
    harness = harness.replace('BASELINE_ENABLED', str(bool(args.baseline_root)).lower()).replace('OLD_HANDOFF',old_handoff)
    with tempfile.TemporaryDirectory(prefix='halo-lcd-quiesce-') as temp:
        cpp=Path(temp)/'test.cpp';exe=Path(temp)/'test';cpp.write_text(harness)
        subprocess.run(['c++','-std=c++17','-pthread','-Wall','-Wextra','-I',str(ROOT/'LCD_Minimal'),str(cpp),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=10)
    print('PASS actual BEGIN order/rejection, watchdog placement and main-loop owner guards')

if __name__=='__main__':main()
