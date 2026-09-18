#!/usr/bin/env python3
"""Actual LCD BEGIN/UI submission/DMA handoff with deterministic time and SDK doubles.

No hardware. The native LVGL pixel review is separate; this test executes the
production mailbox, UI submission function, UART barrier and real BSP DMA logic.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from test_manual_ota_clock import definition

ROOT = Path(__file__).resolve().parents[1]


def harness(root, negative=None):
    ota = (root / 'LCD_Minimal/lcd_ota_uart.h').read_text()
    ui = (root / 'LCD_Minimal/lcd_ui_task.h').read_text()
    bsp = (root / 'LCD_Minimal/lcd_bsp.c').read_text()
    begin = definition(ota, 'static void lcd_ota_handle_begin(JsonObject& doc) {')
    gate = definition(begin, 'if (!lcd_ota_quiesce_before_begin())')
    assert all(x not in gate for x in ['esp_ota_', 'lcd_diag_', 'lcd_ota_nvs_'])
    assert '"render_not_quiescent"' in gate and 'return;' in gate
    assert all(begin.index('lcd_ota_quiesce_before_begin()') < begin.index(x)
               for x in ['lcd_diag_prepare_attempt(', 'lcd_ota_nvs_load_progress(', 'esp_ota_begin('])
    assert 'lcd_lvgl_wait_tx_done' not in definition(bsp, 'bool lcd_lvgl_quiesce_for_flash(')
    assert 's_flush_dma_pending' not in definition(bsp, 'void lcd_bsp_check_flush_timeout(')
    # Existing receive guard stays before all panel/widget work. Static text is
    # a distinct phase and does not advertise an unrendered percentage.
    assert ui.index('if (g_lcd_ota_uart_receiving || g_lcd_ota_binary_mode)') < ui.index('lcd_allow_visible_ui("ota_screen")')
    assert 'install_frame ? "Keep Halo powered on"' in ui
    assert 'install_frame ? "Something new is coming"' in ui
    assert 'if (install_frame || manual_result)' in ui
    assert 'bool is_transfer = !install_frame &&' in ui
    handoff = definition(ota, 'static bool lcd_ota_quiesce_before_begin(')
    if negative:
        handoff = definition((negative/'LCD_Minimal/lcd_ota_uart.h').read_text(), 'static bool lcd_ota_quiesce_before_begin(')
    functions = '\n'.join([
        definition(bsp, 'bool lcd_lvgl_quiesce_for_flash('),
        definition(bsp, 'static bool example_notify_lvgl_flush_ready('),
        definition(bsp, 'static void example_lvgl_flush_cb('),
        definition(ui, 'static bool lcd_ota_install_frame_visible('),
        definition(ui, 'static void lcd_ota_install_frame_submit('), handoff])
    return r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include "lcd_ota_install_frame.h"
using namespace std::chrono;
static std::atomic<unsigned> checks{0};
static void check(bool ok,const char* why){++checks;if(!ok){fprintf(stderr,"FAIL %s\n",why);std::exit(2);}}
static std::atomic<uint32_t> now_ms{1000};
static uint32_t millis(){return now_ms.load();}
static uint64_t esp_timer_get_time(){return uint64_t(millis())*1000;}
#define pdMS_TO_TICKS(x) (x)
static void vTaskDelay(uint32_t){std::this_thread::sleep_for(microseconds(100));}
template<class F>static void until(F condition,const char* why){auto end=steady_clock::now()+seconds(2);while(!condition()){if(steady_clock::now()>end){fprintf(stderr,"STALLED %s\n",why);std::exit(3);}std::this_thread::sleep_for(microseconds(100));}check(true,why);}
static std::timed_mutex owner;
static thread_local bool owns=false;
static bool example_lvgl_lock(int ms){owns=owner.try_lock_for(milliseconds(ms));return owns;}
static void example_lvgl_unlock(){assert(owns);owns=false;owner.unlock();}
static void *lvgl_mux=(void*)1,*amoled_panel_io_handle=(void*)1,*amoled_panel_handle=(void*)1;
using esp_lcd_panel_io_handle_t=void*;
using esp_lcd_panel_handle_t=void*;
struct esp_lcd_panel_io_event_data_t{};
struct lv_disp_drv_t{void* user_data;};
struct lv_area_t{int x1,x2,y1,y2;};
using lv_color_t=int;
struct lv_obj_t{};
static lv_obj_t overlay;
static lv_disp_drv_t driver{(void*)1};
static lv_area_t area{0,3,0,3};
static lv_disp_drv_t *s_flush_pending_drv=nullptr;
static uint32_t s_flush_dma_pending=0,flush_outstanding_count=0;
static unsigned long s_flush_start_ms=0;
static uint32_t s_flush_submit_ok=0,s_flush_submit_fail=0,flush_fail_count=0;
static int s_flush_soft_fault=0;
static bool display_reset_requested=false;
static std::atomic<bool> g_sleep_transition{false};
static void lv_disp_flush_ready(lv_disp_drv_t*){}
using esp_err_t=int;
#define ESP_OK 0
#define SH8601_PANEL_IO_QSPI_TRANS_QUEUE_DEPTH 10
#define FLUSH_FAIL_RESET_THRESHOLD 10
static int submit_error=0;
static int esp_lcd_panel_draw_bitmap(void*,int,int,int,int,void*){return submit_error;}
static struct{template<class...T>void printf(const char*,T...){}void println(const char*){}}Serial;
enum {LCD_OTA_IDLE,LCD_OTA_RECEIVING};
static std::atomic<int> s_lcd_ota_state{LCD_OTA_IDLE};
static std::atomic<bool> g_lcd_ota_uart_receiving{false};
static bool g_lcd_ota_binary_mode=false;
static uint32_t s_lcd_ota_started_ms=1000,s_lcd_ota_budget_ms=10000;
static uint16_t s_lcd_ota_session_id=7;
static bool g_ota_screen_active=true,g_lcd_maintenance_headless=false,g_background_wake_dark=false,
 g_ui_initialized=true,g_panel_enabled=true,g_lvgl_running=true;
static int g_backlight_duty=255;
static std::atomic<unsigned> restores{0},erases{0},frames{0};
struct LcdNvsDeadline{static LcdNvsDeadline fromAttempt(uint32_t,uint32_t){return {};}};
static void lcd_ota_uart_restore_ui(const LcdNvsDeadline&,bool persisted){assert(!persisted);++restores;}
static void lcd_bsp_get_flush_submit_stats(uint32_t*ok,uint32_t*fail,int*,int*){if(ok)*ok=s_flush_submit_ok;if(fail)*fail=s_flush_submit_fail;}
static bool invalidated=false;
static void lv_obj_invalidate(lv_obj_t*o){assert(owns&&o==&overlay);invalidated=true;}
static void lv_refr_now(void*);
FUNCTIONS
// A full LVGL refresh with four strips. The first three DMA completions are
// immediate; the final completion is controlled by the test, independently of
// LVGL's bookkeeping. Fail/skip modes exercise the actual BSP callback.
static std::atomic<unsigned> frame_mode{0};
static std::atomic<bool> frame_return_gate{true},frame_waiting{false};
static void lv_refr_now(void*){
 assert(owns&&invalidated);invalidated=false;++frames;
 for(unsigned i=0;i<4;++i){
  lv_color_t color=1;
  if(frame_mode==1&&i==2)submit_error=1;
  if(frame_mode==2&&i==2)g_sleep_transition=true;
  example_lvgl_flush_cb(&driver,&area,&color);
  submit_error=0;
  if(__atomic_load_n(&s_flush_dma_pending,__ATOMIC_ACQUIRE)&&i<3)
   example_notify_lvgl_flush_ready(nullptr,nullptr,&driver);
 }
 frame_waiting=true;
 while(!frame_return_gate)vTaskDelay(1);
}
static void complete(){if(__atomic_load_n(&s_flush_dma_pending,__ATOMIC_ACQUIRE))example_notify_lvgl_flush_ready(nullptr,nullptr,&driver);}
static void ui_once(uint32_t token){check(example_lvgl_lock(50),"UI acquires owner");lcd_ota_install_frame_submit(token,&overlay);example_lvgl_unlock();}
static void begin(){if(lcd_ota_quiesce_before_begin())++erases;}
static void reset(uint32_t now=1000,uint32_t budget=10000){
 complete();check(__atomic_load_n(&s_flush_dma_pending,__ATOMIC_ACQUIRE)==0,"all DMA closed");
 g_lcd_ota_install_frame.cancel(g_lcd_ota_install_frame.pending(millis()));
 now_ms=now;s_lcd_ota_started_ms=now;s_lcd_ota_budget_ms=budget;s_lcd_ota_session_id=7;
 s_lcd_ota_state=LCD_OTA_IDLE;g_lcd_ota_uart_receiving=false;g_lcd_ota_binary_mode=false;
 g_ota_screen_active=true;g_lcd_maintenance_headless=false;g_background_wake_dark=false;
 g_ui_initialized=true;g_panel_enabled=true;g_lvgl_running=true;g_backlight_duty=255;g_sleep_transition=false;
 frame_mode=0;frame_return_gate=true;frame_waiting=false;invalidated=false;
}
static uint32_t await_request(){until([]{return g_lcd_ota_install_frame.pending(millis())!=0;},"request published");return g_lcd_ota_install_frame.pending(millis());}
int main(){
 if(NEGATIVE_ENABLED){reset();begin();check(erases==1&&frames==0,"old handoff accepts flash without static frame");puts("PASS NEGATIVE186: reproduced stale checking frame at erase admission");return 0;}
 // A timed-out or wrong-session ACK cannot authorize a later BEGIN.
 LcdOtaInstallFrame m;auto a=m.request(4,0xfffffff0u,30);check(m.active(a,0),"clock wrap stays bounded");
 check(!m.active(a,14),"exact deadline expired");check(!m.acknowledge(a,14),"late acknowledgement refused");
 m.cancel(a);auto b=m.request(4,20,20);check(a!=b,"same session has new token");
 check(!m.acknowledge(a,21)&&!m.acknowledged(b,4,21),"old token cannot ack new BEGIN");
 check(m.acknowledge(b,21)&&!m.acknowledged(b,5,21),"session identity required");m.cancel(a);
 check(m.acknowledged(b,4,21),"old cancellation cannot cancel new request");m.cancel(b);
 check(!m.acknowledged(b,4,21),"cancellation removes accepted token");
 // Actual complete handoff. Final frame DMA remains pending after LVGL returns.
 reset();std::thread uart(begin);auto token=await_request();std::thread ui([=]{ui_once(token);});
 until([]{return g_lcd_ota_uart_receiving.load();},"UART inhibits after submitted frame");ui.join();
 check(frames==1&&erases==0,"frame submitted but DMA prevents erase");
 s_flush_pending_drv=nullptr;flush_outstanding_count=0; // synthetic flush-ready
 std::this_thread::sleep_for(milliseconds(2));check(erases==0,"synthetic ready cannot authorize flash");
 complete();uart.join();check(erases==1&&g_lcd_ota_uart_receiving,"real completion authorizes flash under inhibition");
 check(s_lcd_ota_started_ms==1000&&s_lcd_ota_budget_ms==10000,"attempt budget unchanged");
 // Timeout while UI still holds owner, then late UI completion. No late flag
 // write can revive inhibition or fulfill a following request with same session.
 reset();frame_return_gate=false;auto before=erases.load();std::thread late_uart(begin);token=await_request();
 std::thread late_ui([=]{ui_once(token);});until([]{return frame_waiting.load();},"UI inside old frame");
 now_ms=1500;late_uart.join();check(erases==before&&!g_lcd_ota_uart_receiving&&s_lcd_ota_state==LCD_OTA_IDLE,"timeout rejects before flash and clears state");
 frame_return_gate=true;late_ui.join();complete();check(!g_lcd_ota_uart_receiving,"late UI never reenables inhibition");
 reset(1600);std::thread next(begin);auto fresh=await_request();check(fresh!=token,"next BEGIN has fresh token");
 check(!g_lcd_ota_install_frame.acknowledge(token,millis()),"late old ACK ignored");
 std::thread nextui([=]{ui_once(fresh);});until([]{return g_lcd_ota_uart_receiving.load();},"new frame independently acknowledged");nextui.join();complete();next.join();check(erases==before+1,"next genuine frame accepted");
 // The 500 ms is shared, not renewed when UI uses 400 ms. Original attempt
 // remaining can be still smaller; wrap does not create an extra budget.
 for(unsigned small=0;small<2;++small){
  reset(small?0xfffffff0u:2000,small?80:10000);frame_return_gate=false;before=erases;
  uint32_t start=millis();std::thread t(begin);token=await_request();std::thread u([=]{ui_once(token);});
  until([]{return frame_waiting.load();},"frame refresh ready to return");
  now_ms=start+(small?60:400);frame_return_gate=true;
  until([]{return g_lcd_ota_uart_receiving.load();},"late frame acknowledged within original budget");u.join();
  now_ms=start+(small?80:500);t.join();check(erases==before&&!g_lcd_ota_uart_receiving,"DMA cannot use fresh 500 ms after frame");complete();
 }
 // No UI task, skipped render or failed submit must refuse; no NVS erase mock
 // is reachable before acceptance. Initial visibility does not wake a panel.
 for(unsigned mode=0;mode<5;++mode){
  reset(3000,50);before=erases;std::thread t(begin);token=await_request();
  if(mode){if(mode==1)frame_mode=1;if(mode==2)frame_mode=2;if(mode==3)g_panel_enabled=false;if(mode==4)g_backlight_duty=0;
   std::thread u([=]{ui_once(token);});u.join();}
  check(!g_lcd_ota_install_frame.acknowledged(token,7,millis()),"failed/skipped/not visible frame has no ACK");
  now_ms=3050;t.join();check(erases==before&&!g_lcd_ota_uart_receiving,"frame refusal has no flash side effect");complete();
 }
 // Headless/background/offscreen paths retain the old real DMA barrier and
 // never publish a frame request or touch UI visibility.
 for(unsigned dark=0;dark<3;++dark){
  reset();before=frames;if(dark==0)g_lcd_maintenance_headless=true;if(dark==1)g_background_wake_dark=true;if(dark==2)g_ota_screen_active=false;
  auto old=erases.load();begin();check(erases==old+1&&frames==before,"dark BEGIN skips UI frame");
  check(!g_lcd_ota_install_frame.pending(millis()),"dark BEGIN publishes no UI request");
 }
 reset(0,0);before=erases;begin();check(erases==before&&!g_lcd_ota_uart_receiving,"zero budget cannot admit flash");
 printf("PASS %u checks: actual mailbox/UI submit/UART barrier/BSP DMA, deadlines, wrap, late owner, dark and retry\n",checks.load());
}
'''.replace('FUNCTIONS', functions).replace('NEGATIVE_ENABLED', 'true' if negative else 'false')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--source-root', type=Path, default=ROOT)
    p.add_argument('--negative-source-root', type=Path)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--sanitize', action='store_true')
    a = p.parse_args()
    root, out = a.source_root.resolve(), a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    results = []
    for name, negative in [('current', None)] + ([('negative', a.negative_source_root)] if a.negative_source_root else []):
        cpp, exe = out/(name+'.cpp'), out/name
        cpp.write_text(harness(root, negative))
        cmd = ['c++', '-std=c++17', '-pthread', '-Wall', '-Wextra', '-I', str(root/'LCD_Minimal'), str(cpp), '-o', str(exe)]
        if a.sanitize:
            cmd += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        with (out/(name+'-build.log')).open('w') as f:
            built = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, timeout=60)
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=15) if built.returncode == 0 else None
        log = out/(name+'.log')
        log.write_text((run.stdout+run.stderr) if run else 'BUILD FAILED\n')
        results.append({'case': name, 'build_exit': built.returncode, 'run_exit': run.returncode if run else None,
                        'output': log.read_text(), 'harness_sha256': hashlib.sha256(cpp.read_bytes()).hexdigest()})
    paths = [root/'LCD_Minimal'/x for x in ['lcd_ota_uart.h','lcd_ui_task.h','lcd_ota_install_frame.h','lcd_bsp.c']]
    result = {'status': 'PASS' if all(x['build_exit']==0 and x['run_exit']==0 for x in results) else 'FAIL',
              'source_root': str(root), 'source_sha256': {str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in paths},
              'test_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), 'sanitizers':a.sanitize,
              'negative_source_root':str(a.negative_source_root) if a.negative_source_root else None,
              'cases':results, 'limits':['LVGL refresh is an SDK-boundary double; actual production UI submit and BSP DMA functions execute.',
              'Pixel/layout and full UI rendering are separately tested with native LVGL; no hardware latency or panel appearance claim.',
              'UART fails closed at its deadline even if an existing blocked LVGL call outlives that bound.']}
    (out/'RESULT.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))
    raise SystemExit(0 if result['status']=='PASS' else 1)

if __name__ == '__main__':
    main()
