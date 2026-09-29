#!/usr/bin/env python3
"""Execute the actual LCD ADC owner/mailbox/sleep preparation with SDK doubles.

ADC return codes, calibration, clock, serial short writes and peer availability
are controlled boundaries. No device, network or filesystem-persistence calls.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_lcd_maintenance_sleep import definition

ROOT = Path(__file__).resolve().parents[1]
CASES = ('mean', 'cadence', 'adc_failure', 'channel_failure', 'calibration_failure',
         'read_failure', 'conversion_failure', 'saturated_low', 'saturated_high',
         'burst_deadline', 'sleep_sent', 'sleep_no_peer', 'sleep_busy',
         'sleep_short_write', 'cancel_activity', 'cancel_skipped_sleep',
         'coalesce', 'max_wire', 'teardown', 'headless', 'headless_busy',
         'tx_cadence', 'tx_no_control', 'tx_old_control', 'tx_forced_bypass')

SDK = r'''
#pragma once
#include <cstdint>
using esp_err_t=int;
constexpr int ESP_OK=0, ADC_UNIT_1=1, ADC_CHANNEL_0=0, ADC_ATTEN_DB_12=12, ADC_BITWIDTH_12=12;
using adc_oneshot_unit_handle_t=void*;
using adc_cali_handle_t=void*;
struct adc_oneshot_unit_init_cfg_t {int unit_id=0;};
struct adc_oneshot_chan_cfg_t {int atten=0,bitwidth=0;};
struct adc_cali_curve_fitting_config_t {int unit_id=0,chan=0,atten=0,bitwidth=0;};
static esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t*,adc_oneshot_unit_handle_t*);
static esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t,int,const adc_oneshot_chan_cfg_t*);
static esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t*,adc_cali_handle_t*);
static esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t);
static esp_err_t adc_cali_delete_scheme_curve_fitting(adc_cali_handle_t);
static esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t,int,int*);
static esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t,int,int*);
'''


def harness(root):
    adapter = root / 'LCD_Minimal/lcd_power.h'
    main = (root / 'LCD_Minimal/LCD_Minimal.ino').read_text()
    sleep = (root / 'LCD_Minimal/lcd_sleep.h').read_text()
    uart_task = (root / 'LCD_Minimal/lcd_uart_task.h').read_text()
    uart = (root / 'LCD_Minimal/lcd_uart.h').read_text()
    loop = definition(main, 'void loop() {')
    assert loop.index('lcd_power_owner_service(') < loop.index('example_lvgl_lock(')
    enter = definition(sleep, 'static void enterLightSleep() {')
    assert enter.index('lcd_power_prepare_sleep(') < enter.index('notify_sense_sleep()')
    assert enter.index('LcdPowerSleepScope power_sleep_scope;') < enter.index('bool force_sleep')
    assert enter.index('lcd_power_deinit();') > enter.index('lcd_sleep_touch_watch_end();')
    assert enter.index('lcd_power_deinit();') < enter.index('esp_deep_sleep_start();')
    service = uart_task[uart_task.index('lcd_power_uart_service('):uart_task.index('// Drain: hand a spooled image')]
    for guard in ('!binary_xfer_active', '!g_in_light_sleep', '!g_sleep_transition',
                  '!ota_locked', '!g_lcd_ota_uart_receiving', 'sense_ready_for_control_tx()',
                  '!yielded_early', '!deferred_awake_tx_pending()', 'uxQueueMessagesWaiting(uart_tx_queue) == 0'):
        assert guard in service
    rx = (root / 'LCD_Minimal/lcd_uart_rx.h').read_text()
    assert rx.count('lcd_power_note_control_reply();') == 2
    for kind in ('PONG', 'SYNC_ACK'):
        assert 'lcd_power_note_control_reply();' in definition(rx, f'if (strcmp(type, "{kind}") == 0) {{')
    assert 'if (s_lcd_power_sleep_pending) sense_sleep_intent_pending = true;' in loop
    activity = (root / 'LCD_Minimal/lcd_activity.h').read_text()
    for name in ('enter_ship_ota_sleep', 'enter_maintenance_sleep'):
        body = definition(activity, f'static void {name}() {{')
        assert body.index('lcd_maintenance_sleep_begin()') < body.index('lcd_power_headless_sample(')
        assert body.index('lcd_power_deinit();') < body.index('esp_deep_sleep_start();')
    return r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <esp_adc/adc_oneshot.h>
static uint64_t now_ms=1000;
static unsigned long millis(){return (unsigned long)now_ms;}
static int64_t esp_timer_get_time(){return int64_t(now_ms*1000);}
static struct {template<class... A>void printf(const char*,A...){}} Serial;
static unsigned delay_scale=1,delays=0;
static void delay(unsigned n){now_ms+=n*delay_scale;++delays;}
using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
static int lock_depth=0;
static void portENTER_CRITICAL(int*){assert(lock_depth==0);++lock_depth;}
static void portEXIT_CRITICAL(int*){assert(lock_depth==1);--lock_depth;}
static unsigned long last_user_activity_ms=50,last_scroll_activity_ms=60;
static uint32_t g_lcd_coord_boot_id=99;
static constexpr unsigned PROTOCOL_VERSION=1;
static uint32_t next_message=1,uart_tx_count=0;
static uint32_t get_next_msg_id(){return next_message++;}
static void uart_note_tx_type(const char*){}
static bool g_suppress_uart_json_tx=false,short_write=false;
static std::string wire;
static struct {
 size_t write(const uint8_t*s,size_t n){assert(!lock_depth);if(short_write&&n>1)--n;wire.append(reinterpret_cast<const char*>(s),n);return n;}
 void flush(){assert(!lock_depth);}
} senseSerial;
''' + definition(uart, 'static bool lcd_power_uart_write(') + r'''
static std::string mode;
static unsigned creates=0,configs=0,cal_creates=0,adc_deletes=0,cal_deletes=0,reads=0,conversions=0;
static esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t*c,adc_oneshot_unit_handle_t*out){
 assert(!lock_depth&&c->unit_id==ADC_UNIT_1);++creates;
 if(mode=="adc_failure")return -1;*out=(void*)1;return ESP_OK;
}
static esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t h,int channel,const adc_oneshot_chan_cfg_t*c){
 assert(!lock_depth&&h==(void*)1&&channel==ADC_CHANNEL_0&&c->atten==ADC_ATTEN_DB_12&&c->bitwidth==ADC_BITWIDTH_12);++configs;
 return mode=="channel_failure"?-1:ESP_OK;
}
static esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t*c,adc_cali_handle_t*out){
 assert(!lock_depth&&c->unit_id==ADC_UNIT_1&&c->chan==ADC_CHANNEL_0&&c->atten==ADC_ATTEN_DB_12&&c->bitwidth==ADC_BITWIDTH_12);++cal_creates;
 if(mode=="calibration_failure")return -1;*out=(void*)2;return ESP_OK;
}
static esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t h){assert(!lock_depth&&h==(void*)1);++adc_deletes;return ESP_OK;}
static esp_err_t adc_cali_delete_scheme_curve_fitting(adc_cali_handle_t h){assert(!lock_depth&&h==(void*)2);++cal_deletes;return ESP_OK;}
static esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t h,int channel,int*out){
 assert(!lock_depth&&h==(void*)1&&channel==ADC_CHANNEL_0);unsigned i=reads++%32;
 if(mode=="read_failure"&&i==7)return -1;
 *out=2000+int(i);if(mode=="saturated_low"&&i==7)*out=0;if(mode=="saturated_high"&&i==7)*out=4095;return ESP_OK;
}
static esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t h,int raw,int*out){
 assert(!lock_depth&&h==(void*)2);unsigned i=conversions++%32;
 if(mode=="conversion_failure"&&i==7)return -1;
 // Nonlinear conversion makes calibrate-mean-raw observably wrong.
 *out=1900+(raw-2000)*(raw-2000);return ESP_OK;
}
''' + '#include "' + str(adapter) + '"\n' + r'''
static void owner(bool allow=true){lcd_power_owner_service(allow);}
static void prepare(){assert(!lcd_power_prepare_sleep(true));}
static void tx(bool allow=true){lcd_power_note_control_reply();lcd_power_uart_service(allow);}
int main(int argc,char**argv){
 assert(argc==2);mode=argv[1];
 assert(!lcd_power_view().received);
 if(mode=="burst_deadline")delay_scale=5;
 if(mode=="sleep_busy"){
   prepare();for(unsigned n=0;n<3;++n){now_ms+=40;owner(false);assert(!lcd_power_prepare_sleep(true));}
   now_ms+=30;owner(false);assert(lcd_power_prepare_sleep(true));assert(reads==0&&creates==0&&wire.empty());
   // Rechecking cannot renew the same absolute preparation deadline.
   now_ms+=1000;assert(lcd_power_prepare_sleep(true));
 }else if(mode=="cancel_skipped_sleep"){
   prepare();owner(false);owner(false);assert(!s_lcd_power_sleep_pending);
 }else if(mode=="cancel_activity"){
   prepare();++last_user_activity_ms;owner(false);assert(!s_lcd_power_sleep_pending);
 }else{
   owner();auto v=lcd_power_view();assert(v.received&&v.sample.boot_id==99&&v.sample.sequence==1);
   if(mode=="adc_failure"||mode=="channel_failure"||mode=="calibration_failure"){
     assert(!halo_power::valid(v.sample)&&reads==0&&v.sample.system_supply_mv==0);
     assert(v.sample.status==(mode=="calibration_failure"?halo_power::Status::CalibrationUnavailable:halo_power::Status::AdcUnavailable));
     assert(adc_deletes==(mode=="adc_failure"?0u:1u));now_ms+=1000;owner();assert(creates==1);
   }else if(mode=="read_failure"||mode=="conversion_failure"||mode=="burst_deadline"){
     assert(v.sample.status==halo_power::Status::ReadError&&!halo_power::valid(v.sample)&&v.sample.system_supply_mv==0);
     assert(reads<=32&&v.sample.samples<32);if(mode!="burst_deadline")assert(v.sample.samples==7);
   }else if(mode=="saturated_low"||mode=="saturated_high"){
     assert(v.sample.status==halo_power::Status::Saturated&&v.sample.system_supply_mv==0&&reads==8&&conversions==7);
   }else{
     assert(halo_power::valid(v.sample)&&v.sample.samples==32&&reads==32&&conversions==32&&delays==31);
     unsigned sum=0;for(unsigned i=0;i<32;++i)sum+=1900+i*i;
     assert(v.sample.system_supply_mv==2*((sum+16)/32)&&v.sample.raw_min==2000&&v.sample.raw_max==2031);
     if(mode=="cadence"){
       now_ms=1999;owner();assert(reads==32);now_ms=2000;owner();assert(reads==64);
     }else if(mode=="sleep_sent"||mode=="sleep_no_peer"||mode=="sleep_short_write"){
       const auto began=now_ms;prepare();owner();assert(reads==64&&s_lcd_power_sleep_sequence==2);
       if(mode=="sleep_no_peer"){assert(lcd_power_prepare_sleep(false)&&wire.empty());}
       else{
         tx(false);assert(wire.empty()&&!lcd_power_prepare_sleep(true));
         short_write=mode=="sleep_short_write";tx();
         if(short_write){assert(!lcd_power_prepare_sleep(true)&&uart_tx_count==0);const auto partial=wire;tx();assert(wire==partial);now_ms=began+150;assert(lcd_power_prepare_sleep(true));}
         else{assert(lcd_power_prepare_sleep(true)&&uart_tx_count==1&&wire.find("\"q\":2")!=std::string::npos);}
       }
       {LcdPowerSleepScope scope;}assert(!s_lcd_power_sleep_pending);
     }else if(mode=="coalesce"){
       now_ms+=1000;owner();now_ms+=1000;owner();tx(false);assert(wire.empty());
       tx();assert(uart_tx_count==1&&wire.find("\"q\":3")!=std::string::npos);
       const auto saved=wire;tx();assert(wire==saved);
     }else if(mode=="max_wire"){
       s_lcd_power_sample.epoch_s=UINT64_MAX;s_lcd_power_sample.uptime_ms=UINT64_MAX;
       s_lcd_power_sample.boot_id=UINT32_MAX;s_lcd_power_sample.sequence=UINT32_MAX;
       next_message=UINT32_MAX;tx();assert(wire.size()<512&&wire.back()=='\n');
     }else if(mode=="teardown"){
       lcd_power_deinit();lcd_power_deinit();assert(adc_deletes==1&&cal_deletes==1);
     }else if(mode=="headless"||mode=="headless_busy"){
       lcd_power_headless_sample(mode=="headless");assert(!s_lcd_power_sleep_pending&&wire.empty());
       assert(reads==(mode=="headless"?64u:32u));
     }else if(mode=="tx_cadence"){
       tx();assert(uart_tx_count==1);
       now_ms=2000;owner();tx();assert(reads==64&&uart_tx_count==1);
       now_ms=3000;owner();tx();assert(reads==96&&uart_tx_count==1);
       now_ms=4000;owner();tx();assert(reads==128&&uart_tx_count==2&&wire.find("\"q\":4")!=std::string::npos);
     }else if(mode=="tx_no_control"){
       lcd_power_uart_service(true);assert(wire.empty());tx();assert(uart_tx_count==1);
     }else if(mode=="tx_old_control"){
       lcd_power_note_control_reply();now_ms+=2001;lcd_power_uart_service(true);assert(wire.empty());
       tx();assert(uart_tx_count==1);
       now_ms+=3000;owner();lcd_power_uart_service(true);assert(uart_tx_count==1);
     }else if(mode=="tx_forced_bypass"){
       tx();assert(uart_tx_count==1);const auto first=now_ms;prepare();owner();tx();
       assert(now_ms-first<3000&&uart_tx_count==2&&lcd_power_prepare_sleep(true));
     }
   }
 }
 assert(!lock_depth);printf("PASS %s reads=%u wire_bytes=%zu\n",argv[1],reads,wire.size());
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    args = parser.parse_args()
    compiler = shutil.which('c++')
    assert compiler, 'Host C++ compiler required'
    with tempfile.TemporaryDirectory(prefix='halo-lcd-power-') as temp:
        out = Path(temp)
        (out / 'esp_adc').mkdir()
        (out / 'esp_adc/adc_oneshot.h').write_text(SDK)
        (out / 'esp_heap_caps.h').write_text('#pragma once\n#include <cstddef>\nconstexpr int MALLOC_CAP_INTERNAL=1,MALLOC_CAP_8BIT=2;\nstatic size_t heap_caps_get_free_size(int){return 100000;}\nstatic size_t heap_caps_get_largest_free_block(int){return 90000;}\n')
        for name in ('adc_cali.h', 'adc_cali_scheme.h'):
            (out / 'esp_adc' / name).write_text('#pragma once\n#include "adc_oneshot.h"\n')
        cpp, exe = out / 'test.cpp', out / 'test'
        cpp.write_text(harness(args.source_root))
        subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-I', str(out), str(cpp), '-o', str(exe)], check=True, timeout=30)
        for case in CASES:
            subprocess.run([str(exe), case], check=True, timeout=10)
    print(f'PASS {len(CASES)} LCD power owner, ADC failure, freshness, coalescing and sleep-budget cases')


if __name__ == '__main__':
    main()
