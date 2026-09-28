#!/usr/bin/env python3
"""Render the actual provisioning badge callback with host LVGL 8.3.

This is a focused software-render check, not a firmware build or panel test.
The existing card/theme helpers and new draw callback execute without doubles.
"""
import argparse
import hashlib
import json
from pathlib import Path
from host_paths import arduino_user
import re
import shutil
import struct
import subprocess
import zlib

from test_provisioning_display_status import definition

ROOT = Path(__file__).resolve().parents[1]


def harness(root):
    provision = (root / 'LCD_Minimal/lcd_provision.h').read_text()
    theme = (root / 'LCD_Minimal/lcd_theme.h').read_text()
    design = (root / 'LCD_Minimal/lcd_ui_design.h').read_text()
    start = provision.index('    lv_obj_t* avatar = halo_ui_card(profile,')
    end = provision.index('\n', provision.index('lv_obj_add_event_cb(avatar,', start))
    creation = provision[start:end]
    attach = creation[creation.index('    lv_obj_add_event_cb'):]
    creation = creation[:creation.index('    lv_obj_add_event_cb')]
    functions = '\n'.join(definition(theme, signature) for signature in (
        'static inline void quiet_clickable(', 'static inline void trepo_shadow(',
        'static inline void trepo_card('))
    functions += '\n' + definition(design, 'static lv_obj_t* halo_ui_card(')
    functions += '\n' + definition(provision, 'static void provision_ui_profile_draw(')
    colors = '\n'.join(re.search(r'^#define ' + name + r'\s+0x[0-9A-Fa-f]+', theme, re.M).group(0)
                       for name in ('COL_DARK', 'COL_GOLD', 'COL_WHITE'))
    return r'''
#include "lvgl.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
''' + colors + '\n' + functions + r'''
static constexpr int W=80,H=55;
static uint32_t pixels[W*H];
static void flush(lv_disp_drv_t* display,const lv_area_t* area,lv_color_t* data){
  for(int y=area->y1;y<=area->y2;++y)for(int x=area->x1;x<=area->x2;++x)
    pixels[y*W+x]=lv_color_to32(*data++);
  lv_disp_flush_ready(display);
}
static void ppm(const char* path){
  FILE* f=fopen(path,"wb");if(!f)std::exit(2);fprintf(f,"P6\n%d %d\n255\n",W,H);
  for(auto pixel:pixels){fputc((pixel>>16)&255,f);fputc((pixel>>8)&255,f);fputc(pixel&255,f);}fclose(f);
}
static bool check(bool ok,const char* message){if(!ok)fprintf(stderr,"FAIL %s\n",message);return ok;}
int main(int argc,char**){
  lv_init();static lv_color_t buffer[W*10];static lv_disp_draw_buf_t draw;
  lv_disp_draw_buf_init(&draw,buffer,nullptr,W*10);static lv_disp_drv_t display;lv_disp_drv_init(&display);
  display.hor_res=W;display.ver_res=H;display.draw_buf=&draw;display.flush_cb=flush;
  lv_disp_t* screen=lv_disp_drv_register(&display);lv_obj_t* profile=lv_scr_act();
  lv_obj_remove_style_all(profile);lv_obj_set_style_bg_color(profile,lv_color_hex(COL_WHITE),0);
  lv_obj_set_style_bg_opa(profile,LV_OPA_COVER,0);
''' + creation + r'''
  lv_refr_now(screen);ppm("before.ppm");std::vector<uint32_t> before(pixels,pixels+W*H);
  if(argc==1){
''' + attach + r'''
  }
  lv_obj_invalidate(avatar);lv_refr_now(screen);ppm("after.ppm");
  lv_area_t a;lv_obj_get_coords(avatar,&a);const uint32_t ink=lv_color_to32(lv_color_hex(COL_DARK));
  auto pixel=[&](int x,int y){return pixels[(a.y1+y)*W+a.x1+x];};
  bool ok=check(pixel(12,8)==ink,"head is visible")&&check(pixel(12,16)==ink,"shoulders are visible");
  ok=check(pixel(12,12)==before[(a.y1+12)*W+a.x1+12],"head and shoulders remain separated")&&ok;
  unsigned changed=0;
  for(int y=0;y<H;++y)for(int x=0;x<W;++x)if(pixels[y*W+x]!=before[y*W+x]){
    ++changed;ok=check(x>=a.x1+6&&x<=a.x1+18&&y>=a.y1+5&&y<=a.y1+19,"badge outline and layout unchanged")&&ok;
  }
  ok=check(changed>=70&&changed<150,"person silhouette has visible bounded area")&&ok;
  ok=check(lv_obj_get_child_cnt(avatar)==0,"draw callback adds no child objects")&&ok;
  lv_mem_monitor_t first,last;lv_mem_monitor(&first);
  for(unsigned i=0;i<50;++i){lv_obj_invalidate(avatar);lv_refr_now(screen);}
  lv_mem_monitor(&last);ok=check(first.free_size==last.free_size,"repeated draws retain no allocations")&&ok;
  printf("%s actual LVGL profile badge: %u changed pixels, unchanged border/layout, no child objects or redraw growth\n",ok?"PASS":"FAIL",changed);
  return ok?0:1;
}
'''


def png(path, width, height, rgb):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    rows = b''.join(b'\0' + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
                     + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


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
    (out / 'profile.cpp').write_text(harness(root))
    (out / 'lv_conf.h').write_text('#pragma once\n#define LV_COLOR_DEPTH 16\n#define LV_COLOR_16_SWAP 1\n'
                                 '#define LV_MEM_SIZE (96U * 1024U)\n#define LV_USE_THEME_DEFAULT 0\n')
    (out / 'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.12)\nproject(profile_icon C CXX)\n'
        'set(LV_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE STRING "")\n'
        f'add_subdirectory("{lvgl}" lvgl-build)\n'
        'add_executable(profile profile.cpp)\ntarget_compile_features(profile PRIVATE cxx_std_17)\n'
        'target_link_libraries(profile PRIVATE lvgl)\n')
    with (out / 'build.log').open('w') as log:
        subprocess.run(['cmake', '-S', str(out), '-B', str(out / 'build')], stdout=log, stderr=subprocess.STDOUT, check=True, timeout=30)
        subprocess.run(['cmake', '--build', str(out / 'build'), '--target', 'profile', '-j', '4'], stdout=log, stderr=subprocess.STDOUT, check=True, timeout=120)
    result = subprocess.run([str(out / 'build/profile')], cwd=out, capture_output=True, text=True, timeout=10)
    (out / 'render.log').write_text(result.stdout + result.stderr)
    print(result.stdout, end='');result.check_returncode()
    for name in ('before', 'after'):
        data = (out / (name + '.ppm')).read_bytes().split(b'\n', 3)[3]
        png(out / (name + '.png'), 80, 55, data)
        scale = 8
        enlarged = b''.join(b''.join(data[(y * 80 + x) * 3:(y * 80 + x + 1) * 3] * scale
                                    for x in range(80)) for y in range(55) for _ in range(scale))
        png(out / (name + '-8x.png'), 80 * scale, 55 * scale, enlarged)
    negative = out / 'negative-control';negative.mkdir()
    old = subprocess.run([str(out / 'build/profile'), 'no-person-callback'], cwd=negative, capture_output=True, text=True, timeout=10)
    (negative / 'render.log').write_text(old.stdout + old.stderr)
    assert old.returncode == 1 and 'head is visible' in old.stderr
    files = ['LCD_Minimal/lcd_provision.h', 'LCD_Minimal/lcd_theme.h', 'LCD_Minimal/lcd_ui_design.h']
    (out / 'RESULT.json').write_text(json.dumps({'status': 'PASS', 'negative_control': 'empty original badge rejected',
        'renderer': str(lvgl), 'scope': 'Actual LVGL software rendering; no physical display qualification',
        'source_sha256': {f: hashlib.sha256((root / f).read_bytes()).hexdigest() for f in files}}, indent=2) + '\n')
    print('PASS empty-badge negative control; render artifacts: ' + str(out))


if __name__ == '__main__':
    main()
