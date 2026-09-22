#!/usr/bin/env python3
"""Run actual LCD scan-choice functions with native rendering/transport stubs.

Checks user actions and serialized protocol data, shared-screen transitions,
input deadlines, and quantity motion cancellation. No hardware or network use.
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def definition(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def harness(root):
    lcd = root / 'LCD_Minimal'
    flow = (lcd / 'lcd_ship_flow.h').read_text()
    route = (lcd / 'lcd_ship_route.h').read_text()
    main = (lcd / 'LCD_Minimal.ino').read_text()
    ui = (lcd / 'lcd_ui_task.h').read_text()
    motion = (lcd / 'lcd_ui_motion.h').read_text()
    # Include the production input gate, rather than invoking the adjustment
    # helper for modes in which the real event consumer suppresses scrolling.
    scroll = definition(ui, '          if (ui_screen_state == SCREEN_EXPIRY_CHOICE)')
    timeout = definition(main, '  if (ui_screen_state == SCREEN_EXPIRY_CHOICE &&\n      ship_expiry_choice_shown_time > 0 &&')
    timeout_ms = re.search(r'EXPIRY_SCREEN_TIMEOUT_MS\s*=\s*(\d+)', main).group(1)
    funcs = '\n'.join(definition(flow, s) for s in (
        'static void ship_anim_set_y(',
        'static void ship_anim_set_text_opa(',
        'static void ship_init_expiry_choice(',
    ))
    route_funcs = '\n'.join(definition(route, s) for s in (
        'static bool ship_mode_is_discard(',
        'static bool ship_choice_mode_is_discard(',
        'static void ship_configure_scan_choice_screen(',
        'static void ship_send_discard_choice(',
        'static void expiry_choice_update_quantity_label(',
        'static void expiry_choice_adjust_quantity(',
        'static bool expiry_choice_handle_touch(',
    ))
    # Presentation helpers may evolve without copying their implementation
    # into this test. Keep their complete source, in production order.
    quantity_helpers = []
    for source in (flow, route):
        for match in re.finditer(r'^static (?:int|void|bool|lv_coord_t) (ship_choice_quantity_\w+)\([^;]*?\)\s*\{', source, re.M):
            quantity_helpers.append(definition(source, match.group(0)[:-1].rstrip()))
    return PREFIX.replace('@TIMEOUT@', timeout_ms) + '\n'.join(quantity_helpers) + '\n' + funcs + '\n' + route_funcs + '\n' + definition(motion, 'static void halo_ui_motion_stop(') + r'''
static void consume_scroll(int delta) {
  struct { struct { int scroll_delta; } data; } evt{{delta}};
  for (int once=0;once<1;++once) {
''' + scroll + r'''
  }
}
static void consume_timeout() {
''' + timeout + r'''
}
''' + CASES


PREFIX = r'''
#include <ArduinoJson.h>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
using String = std::string;
using lv_opa_t = uint8_t;
using lv_coord_t = int;
struct lv_font_t { int line_height; };
static const lv_font_t nunito_12{12},nunito_14{14},nunito_16{16},nunito_18{18},nunito_22{22},nunito_24{24},nunito_32{32},nunito_44{44},nunito_60{60},lv_font_montserrat_12{12},lv_font_montserrat_14{14};
static constexpr unsigned COL_DARK=0x1a1a1a,COL_WHITE=0xffffff,COL_CREAM=0xf5e9d8,COL_GREEN=0x1f4d2b,COL_TEAL=0x296065,COL_MUTED=0x8a7e72,COL_TEXT2=0x444444,COL_GOLD=0xffd54f;
static constexpr int LV_OBJ_FLAG_HIDDEN=1,LV_OBJ_FLAG_CLICKABLE=2,LV_OBJ_FLAG_SCROLLABLE=4,LV_OBJ_FLAG_EVENT_BUBBLE=8,LV_OBJ_FLAG_OVERFLOW_VISIBLE=16;
static constexpr int LV_PART_MAIN=0,LV_PART_INDICATOR=1,LV_OPA_TRANSP=0,LV_OPA_COVER=255,LV_OPA_30=76,LV_LABEL_LONG_CLIP=1,LV_LABEL_LONG_WRAP=2,LV_ALIGN_CENTER=1;
static constexpr int HALO_ICON_CHEVRON_UP=1,HALO_ICON_CHEVRON_DOWN=2;
struct lv_obj_t {
  int x=0,y=0,w=0,h=0,flags=0,text_opa=255,radius=0;
  unsigned bg=0,ink=0;const lv_font_t* font=nullptr;
  lv_obj_t* parent=nullptr;std::string text;std::vector<lv_obj_t*> children;
};
struct lv_area_t {int x1,y1,x2,y2;};
static std::deque<lv_obj_t> objects;
static lv_obj_t* make(lv_obj_t* parent,int x,int y,int w,int h){
  objects.emplace_back();auto* o=&objects.back();o->parent=parent;o->x=x;o->y=y;o->w=w;o->h=h;if(parent)parent->children.push_back(o);return o;
}
static void lv_obj_set_pos(lv_obj_t* o,int x,int y){o->x=x;o->y=y;}
static void lv_obj_set_x(lv_obj_t* o,int x){o->x=x;}
static void lv_obj_set_y(lv_obj_t* o,int y){o->y=y;}
static int lv_obj_get_y(lv_obj_t* o){return o->y;}
static void lv_obj_set_width(lv_obj_t* o,int w){o->w=w;}
static void lv_obj_set_height(lv_obj_t* o,int h){o->h=h;}
static void lv_obj_set_size(lv_obj_t* o,int w,int h){o->w=w;o->h=h;}
static void lv_obj_add_flag(lv_obj_t* o,int f){assert(o);o->flags|=f;}
static void lv_obj_clear_flag(lv_obj_t* o,int f){assert(o);o->flags&=~f;}
static bool lv_obj_has_flag(lv_obj_t* o,int f){assert(o);return o->flags&f;}
static unsigned lv_color_hex(unsigned c){return c;}
static void lv_label_set_text(lv_obj_t* o,const char* t){assert(o);o->text=t;}
static void lv_label_set_long_mode(lv_obj_t*,int){}
static void lv_obj_set_style_text_font(lv_obj_t* o,const lv_font_t* f,int){o->font=f;}
static void lv_obj_set_style_text_color(lv_obj_t* o,unsigned c,int){o->ink=c;}
static void lv_obj_set_style_bg_color(lv_obj_t* o,unsigned c,int){o->bg=c;}
static void lv_obj_set_style_text_opa(lv_obj_t* o,int p,int){o->text_opa=p;}
static void lv_obj_set_style_arc_opa(lv_obj_t*,int,int){}
static void lv_obj_set_style_radius(lv_obj_t* o,int r,int){o->radius=r;}
static void lv_obj_set_style_border_width(lv_obj_t*,int,int){}
static void lv_obj_set_style_border_color(lv_obj_t*,unsigned,int){}
static void lv_obj_set_style_pad_all(lv_obj_t*,int,int){}
static void lv_obj_set_style_shadow_width(lv_obj_t*,int,int){}
static void lv_obj_set_style_shadow_ofs_x(lv_obj_t*,int,int){}
static void lv_obj_set_style_shadow_ofs_y(lv_obj_t*,int,int){}
static void lv_obj_update_layout(lv_obj_t*){}
static void lv_obj_center(lv_obj_t* o){if(o->parent){o->x=(o->parent->w-o->w)/2;o->y=(o->parent->h-o->h)/2;}}
static lv_obj_t* lv_obj_get_child(lv_obj_t* o,unsigned i){return i<o->children.size()?o->children[i]:nullptr;}
static unsigned lv_obj_get_child_cnt(lv_obj_t* o){return o->children.size();}
static void lv_obj_get_coords(lv_obj_t* o,lv_area_t* a){int x=o->x,y=o->y;for(auto* p=o->parent;p;p=p->parent){x+=p->x;y+=p->y;}*a={x,y,x+o->w-1,y+o->h-1};}
static lv_obj_t* halo_ui_page(){return make(nullptr,0,0,360,360);}
static lv_obj_t* halo_ui_label(lv_obj_t* p,const char* t,const lv_font_t* f,unsigned c,int x,int y,int w){auto* o=make(p,x,y,w,f->line_height);o->text=t;o->font=f;o->ink=c;return o;}
static lv_obj_t* halo_ui_card(lv_obj_t* p,int x,int y,int w,int h,unsigned fill=COL_WHITE,int radius=18){auto* o=make(p,x,y,w,h);o->bg=fill;o->radius=radius;return o;}
static lv_obj_t* halo_ui_badge(lv_obj_t* p,const char* t,int y,int w){auto* o=halo_ui_card(p,(360-w)/2,y,w,25,COL_GOLD,12);halo_ui_label(o,t,&nunito_12,COL_DARK,0,0,w-8);return o;}
static lv_obj_t* halo_ui_ring(lv_obj_t* p,int x,int y,int size,int,unsigned){return make(p,x,y,size,size);}
static lv_obj_t* halo_ui_icon(lv_obj_t* p,int,int x,int y,int size,unsigned){return make(p,x,y,size,size);}
static lv_obj_t* halo_ui_button(lv_obj_t* p,int x,int y,int w,int h,const char* t,unsigned fill=COL_GREEN,unsigned ink=COL_WHITE){auto* o=halo_ui_card(p,x,y,w,h,fill,16);auto* l=halo_ui_label(o,t,&nunito_16,ink,0,0,w-8);lv_obj_center(l);return o;}
using lv_anim_exec_xcb_t=void(*)(void*,int32_t);
struct lv_anim_t {void* var=nullptr;lv_anim_exec_xcb_t exec=nullptr;int from=0,to=0;unsigned time=0;};
static std::vector<lv_anim_t> animations;
static void lv_anim_init(lv_anim_t* a){*a={};}
static void lv_anim_set_var(lv_anim_t* a,void* v){a->var=v;}
static void lv_anim_set_exec_cb(lv_anim_t* a,lv_anim_exec_xcb_t f){a->exec=f;}
static void lv_anim_set_values(lv_anim_t* a,int f,int t){a->from=f;a->to=t;}
static void lv_anim_set_time(lv_anim_t* a,unsigned t){a->time=t;}
static int lv_anim_path_ease_out(const lv_anim_t*){return 0;}
static int lv_anim_path_linear(const lv_anim_t*){return 0;}
static void lv_anim_set_path_cb(lv_anim_t*,int(*)(const lv_anim_t*)){}
static bool lv_anim_del(void* v,lv_anim_exec_xcb_t cb){const auto size=animations.size();animations.erase(std::remove_if(animations.begin(),animations.end(),[=](const lv_anim_t& a){return a.var==v&&(!cb||a.exec==cb);}),animations.end());return animations.size()!=size;}
static lv_anim_t* lv_anim_start(lv_anim_t* a){lv_anim_del(a->var,a->exec);animations.push_back(*a);a->exec(a->var,a->from);return &animations.back();}
static void finish_animations(){auto pending=animations;animations.clear();for(const auto& a:pending)a.exec(a.var,a.to);}
static void half_animations(){for(const auto& a:animations)a.exec(a.var,(a.from+a.to)/2);}
static unsigned long now_ms=1000;
static unsigned long millis(){return now_ms;}
static unsigned long ship_expiry_choice_shown_time=1000;
static constexpr unsigned long EXPIRY_SCREEN_TIMEOUT_MS=@TIMEOUT@;
static constexpr unsigned PROTOCOL_VERSION=1;
static constexpr int SCREEN_HOME=0,SCREEN_EXPIRY_CHOICE=1,SCREEN_SHIP_LOGGED=2,SCREEN_SHIP_EXPIRY=3;
static int ui_screen_state=SCREEN_EXPIRY_CHOICE,expiry_choice_quantity=1;
static int ship_choice_quantity_base_y=122;
static bool expiry_submitted=false,g_ship_ui_finalized=false;
static unsigned lcd_msg_id_counter=1,g_ship_ui_job_id=37,g_ship_ui_finalized_job_id=0;
static char g_ship_ui_mode[24]="check-in";
static int shown_count=0,last_shown=0,date_routes=0,activity_resets=0;
static std::vector<std::string> sent,wakes;
static void request_sense_wake(const char* s){wakes.emplace_back(s);}
static void uart_send_json(const char* s){sent.emplace_back(s);}
static void ui_show(int next,const char*){++shown_count;last_shown=next;}
#define UI_SHOW(next,reason) ui_show(next,reason)
static void ship_show_expiry_screen(){++date_routes;}
static void lv_timer_handler(){}
static void example_lvgl_unlock(){}
static void resetActivityTimer(){++activity_resets;}
static struct {template<class...T>void printf(const char*,T...){}void println(const char*){}} Serial;
static lv_obj_t *ship_expiry_choice_screen=nullptr,*ship_expiry_choice_timeout_ring=nullptr,*ship_choice_badge=nullptr,*ship_expiry_choice_qty_prefix=nullptr,*expiry_choice_quantity_label=nullptr,*ship_expiry_choice_prompt=nullptr,*ship_choice_question=nullptr;
static lv_obj_t *ship_expiry_choice_skip_btn=nullptr,*ship_expiry_choice_skip_label=nullptr,*ship_expiry_choice_add_btn=nullptr,*ship_expiry_choice_add_label=nullptr,*ship_choice_quantity_card=nullptr,*ship_choice_quantity_up=nullptr,*ship_choice_quantity_down=nullptr;
static void ship_anim_set_y(void*,int32_t);
static void ship_anim_set_text_opa(void*,int32_t);
static void expiry_choice_update_quantity_label();
static bool ship_choice_mode_is_discard();
'''


CASES = r'''
static bool visible(lv_obj_t* o){return o&&!lv_obj_has_flag(o,LV_OBJ_FLAG_HIDDEN);}
static void choose(const char* mode){
  strcpy(g_ship_ui_mode,mode);ui_screen_state=SCREEN_EXPIRY_CHOICE;
  expiry_submitted=g_ship_ui_finalized=false;g_ship_ui_finalized_job_id=0;
  shown_count=date_routes=activity_resets=0;sent.clear();wakes.clear();
  now_ms=ship_expiry_choice_shown_time=1000;
  ship_configure_scan_choice_screen();
}
static void check_payload(unsigned quantity){
  assert(sent.size()==1&&wakes.size()==1&&shown_count==1);
  StaticJsonDocument<512> doc;assert(!deserializeJson(doc,sent.front()));
  assert(!strcmp(doc["type"],"INPUT_EXPIRY_DATE"));
  assert(doc["expiry_date"].is<const char*>()&&!strcmp(doc["expiry_date"],""));
  assert(doc["quantity"].as<unsigned>()==quantity);
  assert(expiry_submitted&&g_ship_ui_finalized&&g_ship_ui_finalized_job_id==g_ship_ui_job_id);
  assert(ship_expiry_choice_shown_time==0&&last_shown==SCREEN_SHIP_LOGGED&&date_routes==0);
}
static void discard_payload(bool add){
  assert(sent.size()==1&&wakes.size()==1&&shown_count==1);
  StaticJsonDocument<512> doc;assert(!deserializeJson(doc,sent.front()));
  assert(!strcmp(doc["type"],"INPUT_DISCARD_OPTIONS"));
  assert(doc["add_to_shopping_list"].is<bool>()&&doc["add_to_shopping_list"].as<bool>()==add);
  assert(!doc.containsKey("expiry_date"));
  assert(expiry_submitted&&g_ship_ui_finalized&&ship_expiry_choice_shown_time==0&&date_routes==0);
}
int main(){
  ship_init_expiry_choice();
  // Both halves of the expanded Confirm control have identical semantics.
  for(const auto x:{90,180,270}){
    choose("check-in");expiry_choice_quantity=7;
    assert(expiry_choice_handle_touch(x,260));check_payload(7);
    assert(expiry_choice_handle_touch(x,260));check_payload(7);
    consume_timeout();check_payload(7);
  }
  puts("PASS Confirm emits one empty-date quantity payload across its full width");

  choose("check-in");
  assert(visible(ship_expiry_choice_skip_btn)&&!visible(ship_expiry_choice_add_btn));
  // Put the hidden old Add-Date object on the number to make stale-coordinate
  // hit testing observable independently of its current cached layout.
  lv_obj_set_pos(ship_expiry_choice_add_btn,100,100);lv_obj_set_size(ship_expiry_choice_add_btn,160,90);
  assert(expiry_choice_handle_touch(180,145));
  assert(expiry_choice_handle_touch(20,260));
  assert(sent.empty()&&shown_count==0&&date_routes==0&&!expiry_submitted);
  puts("PASS hidden legacy date control and touches outside Confirm do nothing");

  choose("check-in");expiry_choice_quantity=1;expiry_choice_update_quantity_label();
  consume_scroll(-9);assert(expiry_choice_quantity==1&&animations.empty());
  consume_scroll(1);half_animations();consume_scroll(7);consume_scroll(-2);
  assert(expiry_choice_quantity==7&&expiry_choice_quantity_label->text=="7");
  assert(!animations.empty());for(const auto& a:animations)assert(a.time<=200);
  finish_animations();const int settled_y=expiry_choice_quantity_label->y;
  assert(expiry_choice_quantity_label->text_opa==255);
  assert(ship_expiry_choice_shown_time==1000);
  expiry_choice_handle_touch(180,260);check_payload(7);
  consume_scroll(10);assert(expiry_choice_quantity==7);
  puts("PASS rapid quantity turns settle on latest value; lower bound and submitted gate hold");

  // Previously stale label motion could move the discard question after a
  // mode change; run the animation queue after each reconfiguration.
  choose("check-in");expiry_choice_quantity=98;expiry_choice_update_quantity_label();
  consume_scroll(5);assert(expiry_choice_quantity_label->text=="103");
  choose("discard");const int question_y=expiry_choice_quantity_label->y;
  assert(visible(ship_expiry_choice_skip_btn)&&visible(ship_expiry_choice_add_btn));
  assert(!visible(ship_choice_quantity_card)&&!visible(ship_choice_quantity_up)&&!visible(ship_choice_quantity_down));
  assert(!visible(ship_expiry_choice_prompt));
  assert(expiry_choice_quantity_label->text=="Add it to your\nshopping list?");
  finish_animations();assert(expiry_choice_quantity_label->y==question_y&&expiry_choice_quantity_label->text_opa==255);
  consume_scroll(20);assert(expiry_choice_quantity==103);
  choose("check-in");expiry_choice_quantity=7;expiry_choice_update_quantity_label();finish_animations();
  assert(visible(ship_choice_quantity_card)&&visible(ship_choice_quantity_up)&&visible(ship_choice_quantity_down));
  assert(visible(ship_expiry_choice_prompt)&&!visible(ship_expiry_choice_add_btn));
  assert(expiry_choice_quantity_label->text=="7"&&expiry_choice_quantity_label->y==settled_y);
  puts("PASS mode reuse cancels old motion and restores quantity controls/visibility");

  for(const bool add:{true,false}){
    choose("discard");
    auto* button=add?ship_expiry_choice_add_btn:ship_expiry_choice_skip_btn;
    lv_area_t area;lv_obj_get_coords(button,&area);
    // User-requested large side-by-side panels, not stacked short controls.
    assert(area.y1<=155&&area.y2>=290&&area.x2-area.x1>=120);
    assert(add?(area.x2<180):(area.x1>180));
    assert(expiry_choice_handle_touch(add?110:245,170));discard_payload(add);
    assert(expiry_choice_handle_touch(add?110:245,275));discard_payload(add);
  }
  choose("discard");assert(expiry_choice_handle_touch(180,220));assert(sent.empty());
  puts("PASS large side-by-side discard panels dispatch left=true/right=false once; gap inert");

  for(const bool discard:{false,true}){
    choose(discard?"discard":"check-in");expiry_choice_quantity=9;
    now_ms=1000+EXPIRY_SCREEN_TIMEOUT_MS;consume_timeout();assert(sent.empty());
    ++now_ms;consume_timeout();if(discard)discard_payload(false);else check_payload(9);
    now_ms+=60000;consume_timeout();assert(sent.size()==1);
  }
  puts("PASS original deadline retained; timeouts submit selected quantity or discard skip once");

  choose("check-in");consume_scroll(1);assert(!animations.empty());
  halo_ui_motion_stop(ship_expiry_choice_screen);assert(animations.empty());
  puts("PASS navigation motion cleanup removes quantity animations");
  return 0;
}
'''


def automatic_checkin_harness(root):
    """Run the actual Sense capture branch and LCD routing, with hardware doubles.

    The legacy choice tests above intentionally remain: an older Sense may
    still send WAITING_INPUT, and discard still requires the shared screen.
    """
    sense = (root / 'Sense_Minimal/Sense_Minimal.ino').read_text()
    scan = (root / 'Sense_Minimal/sense_scan.h').read_text()
    route = (root / 'LCD_Minimal/lcd_ship_route.h').read_text()
    worker = sense[sense.index('// Set as current job and mark foreground'):]
    setup = definition(worker, 'if (strcmp(job.mode, "check-in") == 0)')
    legacy = definition(sense, 'if (strcmp(type, "INPUT_EXPIRY_DATE") == 0)')
    capture = definition(worker, 'if (is_check_mode)')
    lcd = '\n'.join(definition(route, signature) for signature in (
        'static bool ship_mode_is_check(', 'static bool ship_mode_is_discard(',
        'static bool ship_mode_is_dish(', 'static void ship_route_log(',
        'static ScreenId route_ship_ui(',
    ))
    return AUTO_PREFIX + lcd + '\n' + definition(scan, 'static void scan_send_terminal_status(') + r'''
static void begin_checkin() {
  current_job=job; current_job.active=true;
''' + setup + r'''
}
static void legacy_expiry(unsigned quantity, const char* expiry) {
  const char* type="INPUT_EXPIRY_DATE";
  StaticJsonDocument<256> doc;doc["quantity"]=quantity;doc["expiry_date"]=expiry;
''' + legacy + r'''
}
static void capture_checkin() {
  const bool is_check_mode=true;
  Frame* fb=nullptr;
  UploadJob::CameraUploadMeta capture_meta{};
  uint8_t* job_buf=nullptr;size_t job_len=0;bool skip_upload=false;
''' + capture + r'''
  assert(skip_upload);
}
''' + AUTO_CASES


AUTO_PREFIX = r'''
#include <ArduinoJson.h>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#define HALO_CAMERA_KEEP_INIT 0
#define pdMS_TO_TICKS(x) (x)
static constexpr unsigned CAMERA_UI_CAPTURE_DELAY_MS=0;
enum {OP_RECORDING,OP_DONE};
struct Job {char mode[24]="check-in",expiry_date[16]="";uint16_t quantity=1;bool add_to_shopping_list=false,active=false;uint32_t job_id=71;int state=OP_RECORDING;};
struct UploadJob {struct CameraUploadMeta {int marker=0;};};
static Job job,current_job,*active_checkin_job=nullptr,*active_discard_job=nullptr;
static char pending_expiry_date[16]="";
static uint16_t pending_quantity=1;
static bool expiry_date_response_received=false,scan_terminal_sent=false;
static unsigned clock_ms=1000,wait_calls=0,queue_calls=0,frame_returns=0,free_calls=0;
static bool camera_ok=true,capture_ok=true,alloc_ok=true,queue_ok=true,inject_late=false;
static uint8_t frame_bytes[16]={0xff,0xd8},copy_bytes[16];
struct Frame {uint8_t* buf=frame_bytes;size_t len=sizeof(frame_bytes);int width=1280,height=1024;} frame;
static Job queued;
static std::vector<std::string> phases,events;
static unsigned millis(){return clock_ms;}
static void vTaskDelay(unsigned ms){clock_ms+=ms;++wait_calls;}
static struct {template<class...T>void printf(const char*,T...){}void println(const char*){}} Serial;
static struct {unsigned getFreeHeap(){return 100000;}} ESP;
static void camera_timeline_reset(const char*){}
static void camera_timeline_event(const char*,int32_t){}
static void camera_timeline_complete(bool,void*,const char*){}
static void diag_note_stage(const char*,int){}
static void diag_record_error(const char*,int,const char*){}
static void diag_record_error_persistent(const char*,int,const char*){}
static void uart_send_sense_diag(const char*,const char*,const char*,int,const char*){}
static void diag_record_action_event(const char*,const char*,const char*,const char*,int){}
static bool init_camera(){return camera_ok;}
static void deinit_camera(){}
static bool warmup_and_capture(Frame*& out,bool){out=capture_ok?&frame:nullptr;return capture_ok;}
static void legacy_expiry(unsigned,const char*);
static void capture_camera_meta_snapshot(UploadJob::CameraUploadMeta* out,Frame*){out->marker=27;}
static uint8_t* allocate_upload_buffer(size_t n,bool* psram){assert(n==sizeof(copy_bytes));*psram=true;if(inject_late)legacy_expiry(88,"2031-02-03");return alloc_ok?copy_bytes:nullptr;}
static void esp_camera_fb_return(Frame*){++frame_returns;}
static void recorded_free(void* p){assert(p==copy_bytes);++free_calls;}
#define free recorded_free
static unsigned upload_queue_count(){return 0;}
static void presign_set_error_text(const char*){}
static void scan_ui_inflight_set(bool,const char*){}
static void flow_step(uint32_t,const char*){}
static bool scan_ui_status_emit(const char* phase,const char*,const char*,uint32_t,bool){phases.emplace_back(phase);events.emplace_back(phase);return true;}
static bool queue_upload_job(uint32_t id,const char* mode,const char* expiry,uint16_t qty,bool add,const UploadJob::CameraUploadMeta* meta,uint8_t* buf,size_t len,uint8_t,bool,uint32_t){
 ++queue_calls;events.emplace_back("QUEUE");assert(meta->marker==27&&buf==copy_bytes&&len==sizeof(copy_bytes));
 assert(!std::memcmp(buf,frame_bytes,len));queued={};queued.job_id=id;strcpy(queued.mode,mode);strcpy(queued.expiry_date,expiry);queued.quantity=qty;queued.add_to_shopping_list=add;return queue_ok;
}
enum ScreenId {SCREEN_UNKNOWN,SCREEN_SHIP_HOLD_STILL,SCREEN_SHIP_EXPIRY_CHOICE,SCREEN_SHIP_LOGGED,SCREEN_SHIP_ERROR,SCREEN_SHIP_PROCESSING};
struct ui_status_t {const char* op;const char* mode;const char* phase;const char* text;const char* ui_policy;uint32_t job_id;};
static bool g_ship_ui_finalized=false;static uint32_t g_ship_ui_finalized_job_id=0;
static const char* ship_infer_phase(const char* phase,const char*){return phase;}
'''


AUTO_CASES = r'''
static void reset(){
 job={};current_job={};queued={};active_checkin_job=&current_job;active_discard_job=nullptr;
 pending_quantity=39;strcpy(pending_expiry_date,"2030-12-31");expiry_date_response_received=true;
 scan_terminal_sent=false;camera_ok=capture_ok=alloc_ok=queue_ok=true;inject_late=false;
 clock_ms=1000;wait_calls=queue_calls=frame_returns=free_calls=0;phases.clear();events.clear();
 g_ship_ui_finalized=false;g_ship_ui_finalized_job_id=0;
}
static ScreenId route(const char* phase,const char* mode="check-in",uint32_t id=71){ui_status_t s={"SCAN",mode,phase,"","",id};return route_ship_ui(&s);}
static void default_metadata(){assert(current_job.quantity==1&&!current_job.expiry_date[0]&&!current_job.add_to_shopping_list&&active_checkin_job==nullptr);}
static unsigned count(const char* phase){unsigned n=0;for(const auto& p:phases)n+=p==phase;return n;}
int main(){
 reset();legacy_expiry(99,"2032-01-02");begin_checkin();default_metadata();
 inject_late=true;capture_checkin();default_metadata();
 assert(queue_calls==1&&queued.quantity==1&&!queued.expiry_date[0]&&!queued.add_to_shopping_list);
 assert(queued.job_id==job.job_id&&frame_returns==1&&free_calls==0&&wait_calls==0);
 assert(count("WAITING_INPUT")==0&&count("ERROR")==0&&count("DONE")==1);
 assert(events.size()>=2&&events[events.size()-2]=="QUEUE"&&events.back()=="DONE");
 assert(route("DONE")==SCREEN_SHIP_LOGGED&&g_ship_ui_finalized_job_id==job.job_id);
 for(const char* late:{"DONE","WAITING_INPUT","CAPTURING","UPLOADING"})assert(route(late)==SCREEN_UNKNOWN);
 legacy_expiry(700,"2033-04-05");assert(queue_calls==1);default_metadata();
 ++job.job_id;begin_checkin();default_metadata();capture_checkin();
 assert(queue_calls==2&&queued.job_id==72&&queued.quantity==1&&!queued.expiry_date[0]);
 puts("PASS actual Sense capture queues quantity1 once without confirmation/wait; late legacy input/status cannot create a job or contaminate next capture");
 for(int failure=0;failure<4;++failure){
  reset();begin_checkin();if(failure==0)camera_ok=false;if(failure==1)capture_ok=false;if(failure==2)alloc_ok=false;if(failure==3)queue_ok=false;
  capture_checkin();assert(count("ERROR")==1&&count("DONE")==0&&count("WAITING_INPUT")==0&&wait_calls==0);
  assert(queue_calls==(failure==3?1u:0u)&&free_calls==(failure==3?1u:0u));
  assert(route("ERROR")==SCREEN_SHIP_ERROR&&route("DONE")==SCREEN_UNKNOWN);
 }
 puts("PASS actual camera-init/capture/allocation/queue failures remain errors with no false Got it or ownership leak");
 reset();assert(route("WAITING_INPUT","discard")==SCREEN_SHIP_EXPIRY_CHOICE);
 assert(route("DONE","discard")==SCREEN_SHIP_LOGGED);
 reset();assert(route("WAITING_INPUT")==SCREEN_SHIP_EXPIRY_CHOICE);
 puts("PASS discard choice route and older-Sense WAITING_INPUT compatibility retained");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--arduino-json', type=Path, default=Path.home() / 'Documents/Arduino/libraries/ArduinoJson/src')
    args = parser.parse_args()
    compiler = shutil.which('clang++') or shutil.which('g++')
    if not compiler or not (args.arduino_json / 'ArduinoJson.h').is_file():
        parser.error('Requires a C++ compiler and canonical ArduinoJson include directory')
    with tempfile.TemporaryDirectory(prefix='halo-scan-choice-') as directory:
        source = Path(directory) / 'test.cpp'
        binary = Path(directory) / 'test'
        for generate in (harness, automatic_checkin_harness):
            source.write_text(generate(args.source_root))
            subprocess.run([compiler, '-std=c++17', '-Wno-deprecated-declarations', '-I', str(args.arduino_json), str(source), '-o', str(binary)], check=True, timeout=30)
            subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == '__main__':
    main()
