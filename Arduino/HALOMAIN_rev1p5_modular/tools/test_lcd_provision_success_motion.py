#!/usr/bin/env python3
"""Actual LVGL success entrance: render equivalence, cancel/delete/reentry and low-memory fallback."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from test_provisioning_display_status import definition
from test_lcd_provision_profile_icon import png

ROOT = Path(__file__).resolve().parents[1]


def harness(root):
    source = (root / 'LCD_Minimal/lcd_provision.h').read_text()
    theme = (root / 'LCD_Minimal/lcd_theme.h').read_text()
    design = (root / 'LCD_Minimal/lcd_ui_design.h').read_text()
    render = definition(source, 'static void provision_ui_render() {')
    cleanup = render[render.index('  provision_ui_success_stop();'):render.index('  const auto step =')]
    start = render.index('  } else if (step == LcdProvisionFlow::Complete) {')
    end = render.index('  } else if (step == LcdProvisionFlow::Failed)', start)
    success = render[render.index('\n', start) + 1:end]
    assert success.count('provision_ui_success_start();') == 1
    static_page = success.replace('    provision_ui_success_start();', '')
    colors = '\n'.join(re.findall(r'^#define COL_\w+\s+0x[0-9A-Fa-f]+', theme, re.M))
    funcs = '\n'.join(definition(theme, x) for x in ['static inline void quiet_clickable(',
        'static inline void trepo_shadow(', 'static inline void trepo_card('])
    funcs += '\n' + '\n'.join(definition(design, x) for x in ['static lv_obj_t* halo_ui_label(',
        'static lv_obj_t* halo_ui_card(', 'static lv_obj_t* halo_ui_badge('])
    funcs += '\n' + design[design.index('enum HaloUiIcon {'):design.index('};', design.index('enum HaloUiIcon {')) + 2]
    funcs += '\n' + definition(design, 'static void halo_ui_icon_draw(')
    funcs += '\n' + definition(design, 'static lv_obj_t* halo_ui_icon(')
    funcs += '\n' + '\n'.join(definition(source, x) for x in ['static lv_obj_t* provision_ui_label(',
        'static void provision_ui_overlay(', 'static void provision_ui_success_frame(',
        'static void provision_ui_success_stop(', 'static void provision_ui_success_start(',
        'static void hide_provisioning_screen() {', 'static void provision_ui_suspend_for_ota() {'])
    return r'''
#include "lvgl.h"
#include "lcd_provision_flow.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
LV_FONT_DECLARE(nunito_12);
LV_FONT_DECLARE(nunito_28);
static lv_obj_t *provision_screen=nullptr, *provision_qr=nullptr, *provision_status_label=nullptr,
 *provision_title_label=nullptr, *provision_ssid_label=nullptr, *provision_url_label=nullptr,
 *provision_ui_back_btn=nullptr, *provision_ui_retry_btn=nullptr;
static bool provision_ui_deferred=false,provision_ui_can_scroll=false,
 provision_ui_preview_active=false,provision_screen_visible=false;
static uint32_t provision_ui_preview_at_ms=0;
static LcdProvisionFlow provision_flow;
static void provision_qr_wait_clear(const char*) {}
''' + colors + '\n' + funcs + r'''
static constexpr int W=360,H=360;
static uint32_t pixels[W*H];
static unsigned checks=0;
static void check(bool ok,const char* why){++checks;if(!ok){fprintf(stderr,"FAIL %s\n",why);std::exit(1);}}
static void flush(lv_disp_drv_t* d,const lv_area_t* a,lv_color_t* data){
 for(int y=a->y1;y<=a->y2;++y)for(int x=a->x1;x<=a->x2;++x)pixels[y*W+x]=lv_color_to32(*data++);
 lv_disp_flush_ready(d);
}
static void frame(const char* name){lv_refr_now(nullptr);if(!name)return;
 FILE* f=fopen(name,"wb");check(f!=nullptr,"open render");fprintf(f,"P6\n%d %d\n255\n",W,H);
 for(auto p:pixels){fputc(p>>16,f);fputc(p>>8,f);fputc(p,f);}fclose(f);
}
static void tick(unsigned ms){for(unsigned i=0;i<ms;i+=10){lv_tick_inc(10);lv_timer_handler();}}
static void no_layers(lv_obj_t* object){
 check(_lv_obj_get_layer_type(object)==LV_LAYER_TYPE_NONE,"no opacity/transform draw layer");
 for(uint32_t i=0;i<lv_obj_get_child_cnt(object);++i)no_layers(lv_obj_get_child(object,i));
}
static void page(bool animate){
 if(!provision_screen){provision_screen=lv_obj_create(lv_layer_top());provision_ui_overlay(provision_screen);}
 provision_flow.step=LcdProvisionFlow::Complete;
''' + cleanup + static_page + r'''
 lv_obj_clear_flag(provision_screen,LV_OBJ_FLAG_HIDDEN);
 if(animate)provision_ui_success_start();
}
int main(){
 lv_init();static lv_color_t buffer[W*12];static lv_disp_draw_buf_t draw;
 lv_disp_draw_buf_init(&draw,buffer,nullptr,W*12);static lv_disp_drv_t d;lv_disp_drv_init(&d);
 d.hor_res=W;d.ver_res=H;d.draw_buf=&draw;d.flush_cb=flush;lv_disp_drv_register(&d);
 page(false);frame("final-reference.ppm");std::vector<uint32_t> expected(pixels,pixels+W*H);
 page(true);check(lv_anim_count_running()==1,"one finite animation");frame("entrance.ppm");
 check(!std::equal(expected.begin(),expected.end(),pixels),"entrance differs from final page");no_layers(provision_screen);
 tick(120);frame("middle.ppm");check(!std::equal(expected.begin(),expected.end(),pixels),"intermediate render exists");no_layers(provision_screen);
 tick(260);frame("settled.ppm");check(lv_anim_count_running()==0,"animation finishes");
 check(std::equal(expected.begin(),expected.end(),pixels),"final pixels exactly match unchanged static page");
 for(int mode=0;mode<3;++mode){
  page(true);tick(80);
  if(mode==0)hide_provisioning_screen();
  if(mode==1)provision_ui_suspend_for_ota();
  if(mode==2){lv_obj_del(provision_screen);provision_screen=nullptr;}
  check(lv_anim_count_running()==0,"hide OTA or delete cancels root animation");tick(500);
  page(false);frame(nullptr);check(std::equal(expected.begin(),expected.end(),pixels),"reentry after cleanup unchanged");
 }
 page(true);tick(80);page(true);check(lv_anim_count_running()==1,"rebuild replaces in-flight animation");tick(400);
 frame(nullptr);check(std::equal(expected.begin(),expected.end(),pixels),"rebuild settles correctly");
 // Allocate all presentation properties first, then force real LVGL heap pressure.
 page(false);provision_ui_success_frame(provision_screen,LV_OPA_COVER);
 lv_mem_monitor_t memory;lv_mem_monitor(&memory);check(memory.free_biggest_size>2048,"room for pressure fixture");
 std::vector<void*> pressure;
 while(memory.free_biggest_size>=1024){
  void* block=lv_mem_alloc(512);check(block!=nullptr,"pressure allocation");
  pressure.push_back(block);lv_mem_monitor(&memory);
 }
 check(memory.free_biggest_size<1024,"actual low-memory branch");
 provision_ui_success_start();check(lv_anim_count_running()==0,"low memory skips optional motion");
 for(void* block:pressure)lv_mem_free(block);
 frame(nullptr);check(std::equal(expected.begin(),expected.end(),pixels),"low memory keeps complete page visible");
 hide_provisioning_screen();tick(500);lv_mem_monitor_t first,last;lv_mem_monitor(&first);
 for(int n=0;n<40;++n){page(true);tick(n%2?400:70);hide_provisioning_screen();tick(400);}
 lv_mem_monitor(&last);check(first.free_size==last.free_size,"repeated entrance/cancellation retains no allocations");
 check(lv_anim_count_running()==0,"all animations closed");
 printf("PASS %u actual-LVGL motion/lifetime/memory checks\n",checks);
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-root', type=Path, default=ROOT)
    p.add_argument('--lvgl', type=Path, default=Path.home() / 'Documents/Arduino/libraries/lvgl')
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args();root, out, lvgl = a.source_root.resolve(), a.out.resolve(), a.lvgl.resolve()
    out.mkdir(parents=True, exist_ok=False)
    version = (lvgl / 'lvgl.h').read_text()
    assert '#define LVGL_VERSION_MAJOR 8' in version and '#define LVGL_VERSION_MINOR 3' in version
    (out / 'motion.cpp').write_text(harness(root))
    (out / 'lv_conf.h').write_text('#pragma once\n#define LV_COLOR_DEPTH 16\n#define LV_COLOR_16_SWAP 1\n'
        '#define LV_MEM_SIZE (96U * 1024U)\n#define LV_USE_THEME_DEFAULT 0\n'
        '#define LV_ASSERT_HANDLER abort();\n#define LV_ASSERT_HANDLER_INCLUDE <stdlib.h>\n')
    fonts = root / 'halo_ota_demo/firmware/halo_lcd_prod'
    (out / 'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.12)\nproject(provision_motion C CXX)\n'
        'set(LV_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE STRING "")\n'
        f'add_subdirectory("{lvgl}" lvgl-build)\n'
        f'add_executable(motion motion.cpp "{fonts}/nunito_12.c" "{fonts}/nunito_28.c")\n'
        f'target_include_directories(motion PRIVATE "{root}/LCD_Minimal")\n'
        'target_compile_features(motion PRIVATE cxx_std_17)\ntarget_link_libraries(motion PRIVATE lvgl)\n')
    with (out / 'build.log').open('x') as log:
        subprocess.run(['cmake','-S',str(out),'-B',str(out/'build')],stdout=log,stderr=subprocess.STDOUT,check=True,timeout=30)
        subprocess.run(['cmake','--build',str(out/'build'),'--target','motion','-j','4'],stdout=log,stderr=subprocess.STDOUT,check=True,timeout=120)
    run = subprocess.run([str(out/'build/motion')],cwd=out,capture_output=True,text=True,timeout=30)
    (out/'render.log').write_text(run.stdout+run.stderr);print(run.stdout,end='');run.check_returncode()
    for name in ['final-reference','entrance','middle','settled']:
        rgb=(out/(name+'.ppm')).read_bytes().split(b'\n',3)[3];png(out/(name+'.png'),360,360,rgb)
    files=['LCD_Minimal/lcd_provision.h','LCD_Minimal/lcd_provision_flow.h','LCD_Minimal/lcd_theme.h','LCD_Minimal/lcd_ui_design.h']
    (out/'RESULT.json').write_text(json.dumps({'status':'PASS','scope':'Actual LVGL8.3 software render/lifetime/memory only; no firmware or panel qualification','source_sha256':{n:hashlib.sha256((root/n).read_bytes()).hexdigest() for n in files},'result':run.stdout.strip()},indent=2)+'\n')

if __name__=='__main__':main()
