#!/usr/bin/env python3
"""Render actual Home battery code, icons and fonts with production-config LVGL.

Hardware inputs are doubles; the Home creation block, theme/icon helpers,
presentation model, draw callback and service gating are production code.
This is not ADC, power-source calibration, panel or firmware acceptance.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

from host_paths import arduino_user
from test_lcd_provision_profile_icon import png
from test_provisioning_display_status import definition

ROOT = Path(__file__).resolve().parents[1]


def harness(root):
    menu = (root / 'LCD_Minimal/lcd_ship_screens.h').read_text()
    ino = (root / 'LCD_Minimal/LCD_Minimal.ino').read_text()
    theme = (root / 'LCD_Minimal/lcd_theme.h').read_text()
    design = (root / 'LCD_Minimal/lcd_ui_design.h').read_text()
    home = definition(menu, 'static void show_ship_main_menu_impl() {')
    creation = definition(home, 'if (!ship_menu_screen) {')
    assert creation.count('lcd_home_power_register(ship_menu_screen);') == 1
    assert 'lv_scr_load(ship_menu_screen);\n  lcd_home_power_service();' in home
    activity = (root / 'LCD_Minimal/lcd_activity.h').read_text()
    reset = definition(activity, 'static void ui_reset_lvgl_objects() {')
    assert 'ship_menu_screen = NULL;\n  lcd_home_power_reset();' in reset
    task = (root / 'LCD_Minimal/lcd_ui_task.h').read_text()
    assert task.index('if (g_lcd_ota_uart_receiving || g_lcd_ota_binary_mode)') < task.index('lcd_home_power_service();')
    assert task.index('if (ota_overlay) {', task.index('// OTA screen no longer active:')) < task.index('lcd_home_power_service();')
    power = (root / 'LCD_Minimal/lcd_home_power.h').read_text()
    for signature in ('static void lcd_home_power_draw(', 'static void lcd_home_power_service('):
        body = definition(power, signature)
        for call in ('adc_oneshot_read(', 'lcd_power_owner_service(', 'resetActivityTimer(',
                     'lcd_set_idle_screen_dark(', 'uart_send_', 'lv_timer_create(',
                     'lv_anim_start(', 'malloc(', 'lv_obj_create('):
            assert call not in body, (signature, call)
    assert 'HWCDC::isPlugged()' in definition(power, 'static bool home_battery_usb_attached()')
    colors = '\n'.join(re.findall(r'^#define COL_\w+\s+0x[0-9A-Fa-f]+', theme, re.M))
    geometry = '\n'.join(line for line in ino.splitlines()
                         if line.startswith('#define SHIP_MENU_') or line.startswith('#define SHIP_MAIN_MENU_'))
    functions = '\n'.join(definition(theme, s) for s in (
        'static inline void quiet_clickable(', 'static inline void trepo_shadow(', 'static inline void trepo_card('))
    functions += '\n' + design[design.index('enum HaloUiIcon {'):design.index('};', design.index('enum HaloUiIcon {')) + 2]
    functions += '\n' + '\n'.join(definition(design, s) for s in (
        'static void halo_ui_icon_draw(', 'static lv_obj_t* halo_ui_icon('))
    functions += '\n' + '\n'.join(definition(menu, s) for s in (
        'static void ship_main_menu_style_button(', 'static void ship_main_menu_set_ai_hold_active(',
        'static void ship_main_menu_add_dish_icon(', 'static void ship_main_menu_add_mic_icon(',
        'static lv_obj_t* ship_menu_bar(', 'static void ship_main_menu_add_plus(lv_obj_t* btn, lv_color_t color) {',
        'static void ship_main_menu_add_minus(lv_obj_t* btn, lv_color_t color) {',
        'static void ship_main_menu_add_dots(lv_obj_t* btn, lv_color_t color) {',
        'static void ship_style_plain_screen('))
    return r'''
#include "lvgl.h"
#include "SystemPower.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <vector>
#define ARDUINO_USB_MODE 1
#define ARDUINO_USB_CDC_ON_BOOT 1
#define EXAMPLE_LCD_H_RES 360
#define EXAMPLE_LCD_V_RES 360
LV_FONT_DECLARE(nunito_12);
LV_IMG_DECLARE(dish_icon_img);
static uint32_t clock_ms=1000;
static uint32_t millis(){return clock_ms;}
static bool usb_attached=false;
class HWCDC {public: static bool isPlugged(){return usb_attached;}};
static lv_obj_t *ship_menu_screen=nullptr,*ship_main_menu_buttons[4]={},
 *ship_main_menu_ai_button=nullptr,*ship_main_menu_ai_label=nullptr,*provision_screen=nullptr;
static constexpr int SCREEN_HOME=1;
static int ui_screen_state=SCREEN_HOME,g_backlight_duty=255;
static bool g_ui_initialized=true,g_panel_enabled=true,g_lvgl_running=true,
 g_idle_screen_dark=false,g_sleep_transition=false,g_in_light_sleep=false,
 g_lcd_maintenance_headless=false,g_background_wake_dark=false,
 g_ota_screen_active=false,ota_locked=false,provision_intro_visible=false;
static const uint32_t SHIP_HOME_SYMBOL_GOLD=0xA66A00;
static halo_power::View input;
static unsigned view_calls=0;
static halo_power::View lcd_power_view(){++view_calls;return input;}
''' + colors + '\n' + geometry + r'''
#include "lcd_home_power.h"
''' + functions + r'''
static constexpr int W=360,H=360;
static uint32_t pixels[W*H];
static unsigned checks=0,flushes=0;
static void check(bool ok,const char* why){++checks;if(!ok){fprintf(stderr,"FAIL %s\n",why);std::exit(1);}}
static void flush(lv_disp_drv_t* display,const lv_area_t* area,lv_color_t* data){
 ++flushes;for(int y=area->y1;y<=area->y2;++y)for(int x=area->x1;x<=area->x2;++x)
  pixels[y*W+x]=lv_color_to32(*data++);
 lv_disp_flush_ready(display);
}
static void frame(const char* name=nullptr){lv_refr_now(nullptr);if(!name)return;
 FILE* file=fopen(name,"wb");check(file!=nullptr,"open render");fprintf(file,"P6\n%d %d\n255\n",W,H);
 for(auto p:pixels){fputc(p>>16,file);fputc(p>>8,file);fputc(p,file);}fclose(file);
}
static unsigned objects(lv_obj_t* obj){unsigned n=1;
 for(unsigned i=0;i<lv_obj_get_child_cnt(obj);++i)n+=objects(lv_obj_get_child(obj,i));return n;
}
static void no_layers(lv_obj_t* obj){check(_lv_obj_get_layer_type(obj)==LV_LAYER_TYPE_NONE,"no extra rendering layer");
 for(unsigned i=0;i<lv_obj_get_child_cnt(obj);++i)no_layers(lv_obj_get_child(obj,i));
}
static void create(){
''' + creation + r'''
 lv_scr_load(ship_menu_screen);lv_obj_update_layout(ship_menu_screen);
}
static void reading(unsigned mv,bool usb=false){clock_ms+=1000;usb_attached=usb;
 input={};input.received=true;input.sample.status=halo_power::Status::Ok;
 input.sample.boot_id=1;input.sample.sequence=clock_ms;input.sample.uptime_ms=clock_ms;
 input.sample.system_supply_mv=mv;input.sample.raw_min=1000;input.sample.raw_max=1200;
 input.sample.samples=halo_power::kSamples;
}
static void force(unsigned mv,bool usb=false){s_home_power_model=home_battery::Model{};reading(mv,usb);lcd_home_power_service();}
static uint32_t color(uint32_t value){return lv_color_to32(lv_color_hex(value));}
static uint32_t arc_pixel(int degrees){double angle=degrees*3.141592653589793/180;
 int x=int(std::round(180+174*std::cos(angle))),y=int(std::round(180+174*std::sin(angle)));
 return pixels[y*W+x];
}
static void preserve(const std::vector<uint32_t>& baseline){
 for(int y=0;y<H;++y)for(int x=0;x<W;++x)if(pixels[y*W+x]!=baseline[y*W+x]){
  check(x>=20&&x<=339&&y>=260&&y<=359,"draw changes only lower indicator band");
  check((x-180)*(x-180)+(y-180)*(y-180)<=180*180,"indicator and glow fit physical circular aperture");
 }
 const int positions[5][2]={{134,24},{24,134},{244,134},{134,244},{134,134}};
 for(auto &pos:positions)for(int y=pos[1];y<pos[1]+96;++y)for(int x=pos[0];x<pos[0]+96;++x)
  check(pixels[y*W+x]==baseline[y*W+x],"all five original cards icons and shadows unchanged");
}
int main(int argc,char**){
 lv_init();static lv_color_t buffer[W*12];static lv_disp_draw_buf_t draw;
 lv_disp_draw_buf_init(&draw,buffer,nullptr,W*12);static lv_disp_drv_t display;lv_disp_drv_init(&display);
 display.hor_res=W;display.ver_res=H;display.draw_buf=&draw;display.flush_cb=flush;lv_disp_drv_register(&display);
 lv_obj_t* base=lv_scr_act();create();
 check(lv_obj_remove_event_cb(ship_menu_screen,lcd_home_power_draw),"remove callback for unchanged baseline");
 frame("baseline.ppm");std::vector<uint32_t> baseline(pixels,pixels+W*H);
 const unsigned object_count=objects(ship_menu_screen);
 lv_mem_monitor_t before_registration,after_registration;lv_mem_monitor(&before_registration);
 if(argc==1)lcd_home_power_register(ship_menu_screen);
 lv_mem_monitor(&after_registration);
 force(3920);frame("battery-78.ppm");
 check(s_home_power_display.mode==home_battery::Mode::Battery&&s_home_power_display.percent==78,"actual policy supplies78 percent");
 check(arc_pixel(90)==color(COL_GREEN),"battery arc visible");
 check(arc_pixel(40)==color(COL_TERT),"unfilled arc remains muted");preserve(baseline);
 unsigned label_ink=0;for(int y=340;y<350;++y)for(int x=150;x<210;++x)
  if(pixels[y*W+x]!=baseline[y*W+x])++label_ink;
 check(label_ink>60,"estimated percentage text visible below More shadow");
 struct Case {unsigned mv,pct;uint32_t ink;int sample;const char* name;};
 const Case cases[]={{4200,100,COL_GREEN,35,"battery-100.ppm"},
  {3700,25,COL_GOLD,140,"battery-25.ppm"},{3600,10,COL_RED,145,"battery-10.ppm"},
  {3300,0,COL_TERT,145,"battery-0.ppm"}};
 for(auto& c:cases){force(c.mv);frame(c.name);check(s_home_power_display.percent==c.pct,"percentage fixture");
  check(arc_pixel(c.sample)==color(c.ink),"threshold color or empty arc");preserve(baseline);}
 force(4650,true);frame("external-power.ppm");
 check(s_home_power_display.mode==home_battery::Mode::ExternalPower,"high rail gives external power");
 check(arc_pixel(135)==baseline[303*W+57],"external power replaces entire arc outside bolt glow");
 unsigned bolt_ink=0;for(int y=338;y<359;++y)for(int x=171;x<189;++x)
  if(pixels[y*W+x]!=baseline[y*W+x])++bolt_ink;
 check(bolt_ink>40,"green glowing lightning bolt visible");preserve(baseline);
 force(3056,true);frame("unknown-usb-low-rail.ppm");
 check(s_home_power_display.mode==home_battery::Mode::Unknown,"USB low rail does not masquerade as low battery");
 check(arc_pixel(90)==color(COL_TERT),"unknown has neutral arc");preserve(baseline);
 force(4650,true);frame();input.age_ms=5001;clock_ms+=300;lcd_home_power_service();frame("stale.ppm");
 check(s_home_power_display.mode==home_battery::Mode::Unknown,"stale external source removes bolt");
 check(arc_pixel(90)==color(COL_TERT),"stale state is neutral");
 force(3920);frame();const unsigned calls_before=view_calls,flush_before=flushes;
 lcd_home_power_service();frame();check(view_calls==calls_before&&flushes==flush_before,"same cadence produces no reads or redraws");
 clock_ms+=250;lcd_home_power_service();frame();check(view_calls==calls_before+1&&flushes==flush_before,"same presentation does not invalidate");
 bool* denied_true[]={&g_idle_screen_dark,&g_sleep_transition,&g_in_light_sleep,&g_lcd_maintenance_headless,
  &g_background_wake_dark,&g_ota_screen_active,&ota_locked,&provision_intro_visible};
 for(auto flag:denied_true){*flag=true;clock_ms+=300;unsigned n=view_calls;lcd_home_power_service();
  check(view_calls==n,"dark sleep OTA or provisioning does not read or render");*flag=false;}
 bool* required_true[]={&g_ui_initialized,&g_panel_enabled,&g_lvgl_running};
 for(auto flag:required_true){*flag=false;unsigned n=view_calls;lcd_home_power_service();
  check(view_calls==n,"unavailable panel does not read or render");*flag=true;}
 g_backlight_duty=0;unsigned n=view_calls;lcd_home_power_service();check(view_calls==n,"zero backlight stays idle");g_backlight_duty=255;
 ui_screen_state=2;n=view_calls;lcd_home_power_service();check(view_calls==n,"non-Home route stays idle");ui_screen_state=SCREEN_HOME;
 lv_scr_load(base);n=view_calls;lcd_home_power_service();check(view_calls==n,"hidden Home stays idle");lv_scr_load(ship_menu_screen);
 provision_screen=lv_obj_create(lv_layer_top());n=view_calls;lcd_home_power_service();check(view_calls==n,"visible setup overlay stays idle");
 lv_obj_del(provision_screen);provision_screen=nullptr;
 for(unsigned i=0;i<12;++i){force(i%3==0?4650:i%3==1?3920:3056,i%3!=1);frame();}
 lv_mem_monitor_t first,last;lv_mem_monitor(&first);
 for(unsigned i=0;i<300;++i){force(i%3==0?4650:i%3==1?3920:3056,i%3!=1);frame();}
 lv_mem_monitor(&last);check(first.free_size==last.free_size,"300 state transitions retain no allocations");
 check(objects(ship_menu_screen)==object_count,"renderer creates no widgets");check(lv_anim_count_running()==0,"glow starts no animation");no_layers(ship_menu_screen);
 // The baseline removed/readded its callback after all cards. Normalize once
 // to production's callback-before-cards allocation order, then compare every
 // subsequent exact lifecycle without allowing a heap-growth tolerance.
 force(3920);frame();lv_mem_monitor(&first);
 lv_scr_load(base);lv_obj_del(ship_menu_screen);ship_menu_screen=nullptr;
 lcd_home_power_reset();create();force(3920);frame();lv_mem_monitor(&last);
 printf("Reference callback reattachment vs normal creation allocation delta=%d bytes\n",int(first.free_size)-int(last.free_size));
 first=last;
 for(unsigned i=0;i<20;++i){lv_scr_load(base);lv_obj_del(ship_menu_screen);ship_menu_screen=nullptr;
  lcd_home_power_reset();create();force(3920);frame();lv_mem_monitor(&last);
  check(first.free_size==last.free_size,"each normal Home rebuild has exactly stable heap");}
 lv_mem_monitor(&last);check(first.free_size==last.free_size,"20 Home rebuilds free callback and recreate without growth");
 check(objects(ship_menu_screen)==object_count,"Home rebuild count stable");check(lv_mem_test()==LV_RES_OK,"LVGL heap remains intact");
 printf("PASS actual Home power renderer: %u checks; %u unchanged objects; callback_bytes=%u; heap_after_warmup=%u; model_bytes=%zu; presentation_bytes=%zu\n",
  checks,object_count,unsigned(before_registration.free_size-after_registration.free_size),unsigned(last.free_size),sizeof(s_home_power_model),
  sizeof(s_home_power_display)+sizeof(s_home_power_checked)+sizeof(s_home_power_last_check_ms));
 return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    parser.add_argument('--lvgl', type=Path, default=arduino_user() / 'libraries/lvgl')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    root, out, lvgl = args.source_root.resolve(), args.out.resolve(), args.lvgl.resolve()
    if not shutil.which('cmake') or not (lvgl / 'lvgl.h').is_file():
        raise SystemExit('CMake and local LVGL 8.3 sources are required')
    version = (lvgl / 'lvgl.h').read_text()
    assert '#define LVGL_VERSION_MAJOR 8' in version and '#define LVGL_VERSION_MINOR 3' in version
    (out / 'home.cpp').write_text(harness(root))
    shutil.copyfile(root / 'LCD_Minimal/lv_conf.h', out / 'lv_conf.h')
    assets = root / 'halo_ota_demo/firmware/halo_lcd_prod'
    (out / 'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.12)\nproject(home_power C CXX)\n'
        'set(LV_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE STRING "")\n'
        f'add_subdirectory("{lvgl}" lvgl-build)\n'
        f'add_executable(home home.cpp "{assets}/nunito_12.c" "{assets}/dish_icon.c")\n'
        f'target_include_directories(home PRIVATE "{root}/LCD_Minimal" "{root}/halo_ota_demo/firmware/shared")\n'
        'target_compile_features(home PRIVATE cxx_std_17)\ntarget_link_libraries(home PRIVATE lvgl)\n')
    with (out / 'build.log').open('w') as log:
        subprocess.run(['cmake', '-S', str(out), '-B', str(out / 'build')], stdout=log, stderr=subprocess.STDOUT, check=True, timeout=30)
        subprocess.run(['cmake', '--build', str(out / 'build'), '--target', 'home', '-j', '4'], stdout=log, stderr=subprocess.STDOUT, check=True, timeout=150)
    result = subprocess.run([str(out / 'build/home')], cwd=out, capture_output=True, text=True, timeout=30)
    (out / 'render.log').write_text(result.stdout + result.stderr)
    print(result.stdout, end='')
    result.check_returncode()
    for image in sorted(out.glob('*.ppm')):
        data = image.read_bytes().split(b'\n', 3)[3]
        png(image.with_suffix('.png'), 360, 360, data)
    negative = out / 'negative-control'
    negative.mkdir()
    old = subprocess.run([str(out / 'build/home'), 'no-indicator'], cwd=negative, capture_output=True, text=True, timeout=15)
    (negative / 'render.log').write_text(old.stdout + old.stderr)
    assert old.returncode == 1 and 'FAIL battery arc visible' in old.stderr
    files = ['LCD_Minimal/lcd_home_power.h', 'LCD_Minimal/lcd_ship_screens.h', 'LCD_Minimal/lcd_activity.h',
             'LCD_Minimal/lcd_ui_task.h', 'LCD_Minimal/LCD_Minimal.ino', 'LCD_Minimal/lcd_theme.h',
             'LCD_Minimal/lcd_ui_design.h', 'LCD_Minimal/lv_conf.h',
             'halo_ota_demo/firmware/shared/HomeBattery.h', 'halo_ota_demo/firmware/shared/SystemPower.h',
             'halo_ota_demo/firmware/halo_lcd_prod/nunito_12.c', 'halo_ota_demo/firmware/halo_lcd_prod/dish_icon.c']
    (out / 'RESULT.json').write_text(json.dumps({'status': 'PASS', 'negative_control': 'missing Home indicator rejected',
        'renderer': str(lvgl), 'scope': 'Actual LVGL production-config software renderer and actual Home helpers; hardware inputs doubled. No physical display, ADC calibration or firmware acceptance.',
        'summary': result.stdout.strip(), 'images': [p.name for p in sorted(out.glob('*.png'))],
        'source_sha256': {f: hashlib.sha256((root / f).read_bytes()).hexdigest() for f in files}}, indent=2) + '\n')
    print('PASS missing-indicator negative control; artifacts: ' + str(out))


if __name__ == '__main__':
    main()
