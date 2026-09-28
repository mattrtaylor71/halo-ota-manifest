#!/usr/bin/env python3
"""Exercise production shopping refresh admission and border feedback on the host.

Runs extracted production functions against deterministic LVGL/network boundaries.
This tests state and animation admission, not rendered pixels or cross-core timing.
Use --source-root with the private216 snapshot for the pre-fix negative control;
that run must fail the named manual-feedback assertion without crashing the host.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def definition(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def harness(root):
    source = (root / 'LCD_Minimal/lcd_ship_screens.h').read_text()
    functions = '\n'.join(definition(source, signature) for signature in (
        'static void shopping_list_ring_anim_reset(lv_obj_t* ring)',
        'static void shopping_list_ring_show_sweep()',
        'static void shopping_list_ring_finish()',
        'static void shopping_list_ring_show_error()',
        'static void shopping_list_ring_hide()',
        'static void shopping_list_refresh_indicator_sync(bool force)',
        'static void shopping_list_trigger_refresh(const char* reason) {',
    ))
    return r'''
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#define CHECK(condition, message) do { if (!(condition)) { \
  std::fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); \
  std::exit(1); } } while (false)
enum RefreshState {REFRESH_IDLE, REFRESH_WAKE_PENDING, REFRESH_INFLIGHT,
                   REFRESH_COMPLETE, REFRESH_FAILED};
static RefreshState refresh_state;
struct lv_obj_t { bool hidden; } ring_object;
struct lv_anim_t { void* var; void (*exec)(void*, int32_t); };
static constexpr int LV_OBJ_FLAG_HIDDEN=1, LV_OPA_COVER=255, LV_OPA_20=51;
static constexpr int LV_ANIM_REPEAT_INFINITE=-1;
static constexpr int SHOPPING_LIST_RING_WIDTH=5, SHOPPING_LIST_RING_SWEEP_DEG=70;
static constexpr uint32_t SHOPPING_LIST_RING_SWEEP_MS=1200;
static lv_obj_t* shopping_list_refresh_ring=&ring_object;
static int shopping_list_refresh_ui_state;
static bool shopping_list_refresh_show_ring, shopping_list_ring_hiding;
static bool shopping_list_ring_sweeping, shopping_list_ring_autohide;
static bool shopping_list_touch_in_press;
static unsigned now_ms, refresh_deadline, wake_requests, new_refreshes, ticks;
static unsigned sweep_starts, completion_starts, error_starts, fades, toasts;
static unsigned ring_resets;
static void shopping_list_ring_sweep_anim_cb(void*,int32_t){}
static void shopping_list_ring_close_anim_cb(void*,int32_t){}
static void shopping_list_ring_anim_opa_cb(void*,int32_t){}
static void shopping_list_ring_close_anim_ready(lv_anim_t*){}
static void shopping_list_ring_flash_anim_ready(lv_anim_t*){}
static void lv_anim_path_linear(){}
static void lv_anim_path_ease_out(){}
static bool lv_obj_has_flag(lv_obj_t* o,int){return o->hidden;}
static void lv_obj_clear_flag(lv_obj_t* o,int){o->hidden=false;}
static void lv_obj_move_foreground(lv_obj_t*){}
static void lv_obj_set_style_opa(lv_obj_t*,int,int){}
static void lv_arc_set_angles(lv_obj_t*,int,int){}
static void lv_anim_del(lv_obj_t*,void (*cb)(void*,int32_t)){
  if(cb==shopping_list_ring_sweep_anim_cb)++ring_resets;
}
static void lv_anim_init(lv_anim_t* a){*a={nullptr,nullptr};}
static void lv_anim_set_var(lv_anim_t* a,void* v){a->var=v;}
static void lv_anim_set_exec_cb(lv_anim_t* a,void (*cb)(void*,int32_t)){a->exec=cb;}
static void lv_anim_set_values(lv_anim_t*,int,int){}
static void lv_anim_set_time(lv_anim_t*,uint32_t){}
static void lv_anim_set_repeat_count(lv_anim_t*,int){}
static void lv_anim_set_path_cb(lv_anim_t*,void (*)()){}
static void lv_anim_set_ready_cb(lv_anim_t*,void (*)(lv_anim_t*)){}
static void lv_anim_set_playback_time(lv_anim_t*,uint32_t){}
static void lv_anim_start(lv_anim_t* a){
  CHECK(a->var==shopping_list_refresh_ring,"animation targets current ring");
  if(a->exec==shopping_list_ring_sweep_anim_cb)++sweep_starts;
  else if(a->exec==shopping_list_ring_close_anim_cb)++completion_starts;
  else if(a->exec==shopping_list_ring_anim_opa_cb)++error_starts;
  else CHECK(false,"known production animation callback");
}
static void shopping_list_ring_set_style(lv_obj_t*,uint32_t,int){}
static void shopping_list_keep_overlay_on_top(){}
static void shopping_list_scroll_cue_sync(){}
static void shopping_list_ring_fade_out(uint32_t,uint32_t){
  ++fades;shopping_list_ring_hiding=true;
}
static void shopping_list_toast_show(const char*){++toasts;}
static void request_sense_wake(const char*){++wake_requests;}
static void refresh_sm_set_wake_pending(const char*){
  ++new_refreshes;refresh_state=REFRESH_WAKE_PENDING;refresh_deadline=now_ms+20000;
}
static void ui_lvgl_tick(){++ticks;}
static struct {template<class... A>void printf(const char*,A...){}
               void println(const char*){}} Serial;
FUNCTIONS
static void reset(){
  refresh_state=REFRESH_IDLE;ring_object.hidden=true;
  shopping_list_refresh_ring=&ring_object;
  shopping_list_refresh_ui_state=-1;
  shopping_list_refresh_show_ring=false;
  shopping_list_ring_hiding=shopping_list_ring_sweeping=false;
  shopping_list_ring_autohide=shopping_list_touch_in_press=false;
  now_ms=100;refresh_deadline=0;
  wake_requests=new_refreshes=ticks=0;
  sweep_starts=completion_starts=error_starts=fades=toasts=ring_resets=0;
}
static void start_silent(RefreshState state){
  shopping_list_trigger_refresh("entry_revalidate");
  CHECK(new_refreshes==1&&wake_requests==1,"entry starts one background request");
  refresh_state=state;
  shopping_list_refresh_indicator_sync(false);
  CHECK(ring_object.hidden&&!shopping_list_refresh_show_ring&&sweep_starts==0,
        "entry refresh stays silent before an explicit user request");
}
int main(){
  for(RefreshState state:{REFRESH_WAKE_PENDING,REFRESH_INFLIGHT}){
    for(const char* reason:{"refresh_button","touch_pull","list_refresh","usb_refresh"}){
      reset();start_silent(state);
      const unsigned original_deadline=refresh_deadline;
      const unsigned original_ticks=ticks;
      now_ms+=8000;
      shopping_list_trigger_refresh(reason);
      CHECK(!ring_object.hidden&&shopping_list_refresh_show_ring&&
            shopping_list_ring_sweeping&&sweep_starts==1,
            "manual request promotes silent active refresh to visible ring");
      CHECK(refresh_state==state&&new_refreshes==1&&wake_requests==1&&
            refresh_deadline==original_deadline,
            "joining pending or inflight refresh preserves request and deadline");
      CHECK(ticks==original_ticks,"joining only changes UI state without nested render");
      const unsigned resets_after_join=ring_resets;
      for(int i=0;i<10;++i){
        ++now_ms;shopping_list_trigger_refresh(reason);
        shopping_list_refresh_indicator_sync(false);
      }
      CHECK(sweep_starts==1&&ring_resets==resets_after_join,
            "repeated manual requests never restart the running sweep");
      CHECK(new_refreshes==1&&wake_requests==1&&refresh_deadline==original_deadline,
            "repeated requests never extend the request or duplicate network work");
      shopping_list_trigger_refresh("entry_revalidate");
      CHECK(shopping_list_refresh_show_ring&&!ring_object.hidden&&sweep_starts==1,
            "duplicate background entry cannot demote user-visible active refresh");
      refresh_state=REFRESH_COMPLETE;
      shopping_list_refresh_indicator_sync(false);
      CHECK(completion_starts==1&&shopping_list_ring_autohide,
            "promoted refresh retains completion animation");
      refresh_state=REFRESH_IDLE;shopping_list_refresh_indicator_sync(false);
      CHECK(fades==0,"idle cannot interrupt completion animation ownership");
      // Deterministic boundary: LVGL finishes its close/fade, then the user reopens.
      ring_object.hidden=true;shopping_list_ring_hiding=false;
      shopping_list_ring_autohide=false;
      shopping_list_trigger_refresh("entry_revalidate");
      CHECK(!shopping_list_refresh_show_ring&&ring_object.hidden&&sweep_starts==1&&
            new_refreshes==2&&wake_requests==2,
            "new background entry resets prior manual visibility intent");
    }
  }
  puts("PASS silent pending/inflight refresh promotes on explicit request without duplicate work, deadline changes, nested render or sweep restart");
  for(RefreshState state:{REFRESH_WAKE_PENDING,REFRESH_INFLIGHT}){
    for(RefreshState outcome:{REFRESH_COMPLETE,REFRESH_FAILED}){
      reset();start_silent(state);
      const unsigned original_deadline=refresh_deadline;
      for(int i=0;i<5;++i)shopping_list_trigger_refresh("entry_revalidate");
      CHECK(ring_object.hidden&&!shopping_list_refresh_show_ring&&sweep_starts==0,
            "repeated silent entry stays silent");
      CHECK(wake_requests==1&&new_refreshes==1&&refresh_deadline==original_deadline,
            "repeated silent entry preserves request and deadline");
      refresh_state=outcome;shopping_list_refresh_indicator_sync(false);
      CHECK(ring_object.hidden&&completion_starts==0&&error_starts==0&&toasts==0,
            "silent background completion and failure do not surface feedback");
      reset();start_silent(state);shopping_list_trigger_refresh("refresh_button");
      refresh_state=outcome;shopping_list_refresh_indicator_sync(false);
      CHECK(completion_starts==(outcome==REFRESH_COMPLETE?1u:0u)&&
            error_starts==(outcome==REFRESH_FAILED?1u:0u)&&
            toasts==(outcome==REFRESH_FAILED?1u:0u),
            "explicit refresh gets exactly one appropriate completion or error");
    }
  }
  puts("PASS silent repeated entry, explicit completion/failure and next-entry intent reset");
  for(RefreshState state:{REFRESH_IDLE,REFRESH_COMPLETE,REFRESH_FAILED}){
    reset();refresh_state=state;shopping_list_trigger_refresh("refresh_button");
    CHECK(refresh_state==REFRESH_WAKE_PENDING&&new_refreshes==1&&wake_requests==1&&
          !ring_object.hidden&&sweep_starts==1,
          "ordinary manual refresh still starts one visible new request");
  }
  reset();start_silent(REFRESH_INFLIGHT);
  shopping_list_refresh_ring=nullptr;
  shopping_list_trigger_refresh("refresh_button");
  CHECK(shopping_list_refresh_show_ring&&new_refreshes==1&&wake_requests==1,
        "intent can promote safely while no ring object exists");
  shopping_list_refresh_ring=&ring_object;
  shopping_list_refresh_ui_state=-1;
  shopping_list_refresh_indicator_sync(true);
  CHECK(!ring_object.hidden&&sweep_starts==1,"rebuilt ring inherits promoted active intent");
  puts("PASS ordinary manual refresh and no-object/rebuilt-ring boundaries");
}
'''.replace('FUNCTIONS', functions)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='halo-shopping-feedback-') as temp:
        cpp, binary = Path(temp) / 'test.cpp', Path(temp) / 'test'
        cpp.write_text(harness(args.source_root.resolve()))
        subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
