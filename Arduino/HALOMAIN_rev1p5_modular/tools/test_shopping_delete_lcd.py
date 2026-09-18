#!/usr/bin/env python3
"""Actual delete/projection/populate functions; queue, LVGL, storage and clock boundaries are doubled.

Checks immediate presentation separately from confirmed RAM/NVS mutation. No real
LVGL geometry, device timing or backend request is simulated as hardware proof.
"""
import argparse
from pathlib import Path
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
    screens = (lcd / 'lcd_ship_screens.h').read_text()
    projection = '\n'.join(definition(screens, s) for s in (
        'static int shopping_list_pending_delete_index()',
        'static int shopping_list_visible_count()',
        'static int shopping_list_visible_index(',
        'static void shopping_list_refresh_delete_view()'))
    populate = definition(screens, 'static void shopping_list_screen_populate() {')
    functions = '\n'.join(definition(screens, s) for s in (
        'static const char* shopping_list_delete_index(',
        'static void shopping_list_delete_failure_toast(',
        'static void shopping_list_expire_pending_delete(',
        'static int shopping_list_apply_delete_result('))
    ui = (lcd / 'lcd_ui_task.h').read_text()
    start = ui.index('        StaticJsonDocument<128> selected_id_doc;')
    selected = ui[start:ui.index('        Serial.printf("[LISTSTATE]', start)]
    # The actual transport predicates retain startup delivery and bounded failure.
    awake = definition((lcd / 'lcd_uart.h').read_text(), 'static bool tx_msg_requires_awake_proof(')
    assert '"INPUT_DELETE"' in awake
    ino = (lcd / 'LCD_Minimal.ino').read_text()
    assert 'post_list_delete_result(e->msg.id, false)' in ino
    assert 'post_list_delete_result(failed["id"] | "", false)' in (lcd / 'lcd_link_ack.h').read_text()
    assert 'post_list_delete_result(id, ok)' in (lcd / 'lcd_uart_rx.h').read_text()
    result_start = ui.index('        int removed = shopping_list_apply_delete_result(')
    result_end = ui.index('        resetActivityTimer();', result_start)
    apply_event = ui[result_start:result_end]
    scroll_start = ui.index('            int new_idx = shopping_list_scroll_idx + evt.data.scroll_delta;')
    scroll_end = ui.index('            // Track overscroll', scroll_start)
    scroll = ui[scroll_start:scroll_end]
    assert 'shopping_list_visible_index(0, 1)' in ui
    assert 'shopping_list_animate_card_removal' not in ui
    overlay = definition(screens, 'static void shopping_list_show_overlay() {')
    assert 'shopping_list_scroll_idx == shopping_list_pending_delete_index()' in overlay
    assert 'shopping_list_overlay_item_id' in screens
    return r'''
#include <ArduinoJson.h>
#include <cstdio>
#include <cstring>
#include <cassert>
#include <string>
#include <vector>
#include <memory>
struct List {int count;char items[50][64],item_ids[50][64],stores[50][48];} g_active;
struct tx_msg_t {char type[24];int delta;char id[40];bool has_delta,has_id;};
static void* app_state_mutex=(void*)1;
static bool lock_ok=true,queue_ok=true;
static uint32_t now_ms=100,shopping_list_pending_delete_ms;
static uint32_t millis(){return now_ms;}
static int enqueues,saves,toasts,shopping_list_scroll_idx,deleted_item_count;
static constexpr int MAX_DELETED_ITEMS=20,SCREEN_SHOPPING_LIST=16;
static int ui_screen_state=SCREEN_SHOPPING_LIST;
static char deleted_item_ids[MAX_DELETED_ITEMS][64],shopping_list_pending_delete_id[64];
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
static int xSemaphoreTake(void*,int){return lock_ok;}
static void xSemaphoreGive(void*){}
static void shopping_list_toast_show(const char*){++toasts;}
static void show_auto_hiding_status_message(const char*,unsigned long){++toasts;}
static bool uart_tx_enqueue(const tx_msg_t* m,const char*){++enqueues;assert(!strcmp(m->type,"INPUT_DELETE"));return queue_ok;}
static void save_list_to_storage(List*){++saves;}
static struct {template<class...T>void printf(const char*,T...) {}} Serial;
''' + render_stubs() + projection + '\n' + populate + '\n' + functions + r'''
static int apply(const char* id,bool ok){
 struct{struct{struct{char id[64];bool ok;}list_delete_result;}data;}evt={};
 strcpy(evt.data.list_delete_result.id,id);evt.data.list_delete_result.ok=ok;
''' + apply_event + r'''
 return removed;
}
static int scroll_next(int delta){
 struct{struct{int scroll_delta;}data;}evt={{delta}};
''' + scroll + r'''
 return new_idx;
}

static void reset(){
 g_active={};g_active.count=2;strcpy(g_active.items[0],"Olives");strcpy(g_active.item_ids[0],"6");
 strcpy(g_active.items[1],"Apples");strcpy(g_active.item_ids[1],"3");
 deleted_item_count=enqueues=saves=toasts=shopping_list_scroll_idx=0;
 shopping_list_pending_delete_id[0]=0;queue_ok=lock_ok=true;now_ms=100;
 ui_screen_state=SCREEN_SHOPPING_LIST;g_list_refresh_completed_once=true;refresh_state=REFRESH_IDLE;refresh_timeout_count=0;
 shopping_list_reveal_pending=false;allocations.clear();count_label.text.clear();title_label.text.clear();
 shopping_list_screen_populate();repaints=0;
}
static void retained(){assert(g_active.count==2&&saves==0&&deleted_item_count==0);}
static void visible(std::initializer_list<int> rows){
 std::vector<int> actual;
 for(int i=0;i<shopping_list_rendered_count;++i)if(shopping_list_items[i])actual.push_back(i);
 assert(actual==std::vector<int>(rows));
 assert(count_label.text==std::to_string(rows.size())+" ITEMS");
}
static void assert_headers(std::initializer_list<const char*> names){
 std::vector<std::string> actual,wanted;
 for(int i=0;i<shopping_list_rendered_count;++i)if(shopping_list_item_headers[i])actual.push_back(shopping_list_item_headers[i]->text);
 for(auto name:names)wanted.emplace_back(name);assert(actual==wanted);
}
static void group_fixture(){
 reset();g_active.count=5;
 const char* groups[]={"A","A","B","C","C"};
 for(int i=0;i<5;++i){snprintf(g_active.item_ids[i],64,"id%d",i);snprintf(g_active.items[i],64,"item%d",i);strcpy(g_active.stores[i],groups[i]);}
 shopping_list_screen_populate();repaints=0;
}
int main(){
 reset();queue_ok=false;assert(!shopping_list_delete_index(0)[0]);retained();assert(!shopping_list_pending_delete_id[0]);
 reset();lock_ok=false;assert(!shopping_list_delete_index(0)[0]);retained();assert(enqueues==0);
 reset();assert(!shopping_list_delete_index(0,"3")[0]);retained();assert(enqueues==0);
 reset();g_active.item_ids[0][0]=0;assert(!shopping_list_delete_index(0)[0]);retained();assert(enqueues==0);
 reset();memset(g_active.item_ids[0],'x',40);g_active.item_ids[0][40]=0;assert(!shopping_list_delete_index(0)[0]);retained();assert(enqueues==0);
 reset();assert(!strcmp(shopping_list_delete_index(0,"6"),"6"));retained();assert(enqueues==1&&!strcmp(shopping_list_pending_delete_id,"6"));
 assert(!shopping_list_delete_index(0)[0]&&!shopping_list_delete_index(1)[0]&&enqueues==1);retained();
 assert(apply("3",true)==-1);retained();assert(!strcmp(shopping_list_pending_delete_id,"6"));
 assert(apply("6",true)==0);assert(g_active.count==1&&saves==1&&!strcmp(g_active.item_ids[0],"3"));
 assert(deleted_item_count==1&&!strcmp(deleted_item_ids[0],"6"));assert(apply("6",true)==-1&&saves==1);
 reset();shopping_list_delete_index(0);assert(apply("6",false)==-1);retained();assert(!shopping_list_pending_delete_id[0]);
 reset();shopping_list_delete_index(0);now_ms+=59999;shopping_list_expire_pending_delete();assert(shopping_list_pending_delete_id[0]);
 ++now_ms;shopping_list_expire_pending_delete();retained();assert(!shopping_list_pending_delete_id[0]);
 assert(apply("6",true)==-1);retained();
 assert(!strcmp(shopping_list_delete_index(1),"3"));assert(apply("6",true)==-1);retained();
 reset();shopping_list_delete_index(0);now_ms+=60000;assert(apply("6",true)==-1);retained();
 reset();shopping_list_delete_index(0);strcpy(g_active.item_ids[0],"3");strcpy(g_active.item_ids[1],"6");
 assert(apply("6",true)==1&&g_active.count==1&&!strcmp(g_active.item_ids[0],"3"));
 reset();strcpy(g_active.item_ids[0],"6\"\\\n");
''' + selected + r'''
 StaticJsonDocument<128> decoded;assert(!deserializeJson(decoded,selected_id_json));assert(!strcmp(decoded.as<const char*>(),g_active.item_ids[0]));

 // Actual populate runs synchronously at accepted queue admission. Cache and
 // tombstones remain unchanged until a correlated positive backend result.
 reset();const List original=g_active;shopping_list_delete_index(0,"6");
 assert(repaints==1&&memcmp(&original,&g_active,sizeof(original))==0&&saves==0&&deleted_item_count==0);
 visible({1});assert(shopping_list_rendered_count==2&&shopping_list_scroll_idx==1);
 assert(scroll_next(-1)==1&&scroll_next(1)==1&&!shopping_list_reveal_pending);
 assert(apply("3",true)==-1&&repaints==1);visible({1});
 assert(apply("6",false)==-1&&repaints==2&&toasts==1);visible({0,1});retained();
 reset();queue_ok=false;shopping_list_delete_index(0);visible({0,1});assert(repaints==0&&saves==0);
 reset();shopping_list_delete_index(0);now_ms+=59999;shopping_list_expire_pending_delete();visible({1});
 ++now_ms;shopping_list_expire_pending_delete();visible({0,1});assert(toasts==1&&saves==0);
 assert(apply("6",true)==-1);visible({0,1});assert(saves==0);
 reset();shopping_list_delete_index(0);lock_ok=false;assert(apply("6",true)==-1);visible({0,1});assert(saves==0&&toasts==1);
 // A stale/fresh/reordered replacement projects by ID, never the old index.
 reset();shopping_list_delete_index(0);shopping_list_screen_populate();visible({1});
 strcpy(g_active.item_ids[0],"3");strcpy(g_active.item_ids[1],"6");shopping_list_screen_populate();visible({0});
 assert(shopping_list_pending_delete_index()==1);assert(apply("6",true)==1);visible({0});assert(saves==1&&deleted_item_count==1);
 assert(apply("6",true)==-1&&saves==1);
 reset();shopping_list_delete_index(0);strcpy(g_active.item_ids[0],"new");shopping_list_screen_populate();visible({0,1});
 assert(shopping_list_pending_delete_index()==-1);assert(apply("6",true)==-1&&saves==0);visible({0,1});
 // Selection crosses a hidden middle row in both directions. Last-row hide
 // returns to the prior visible row, and no data indices are compacted.
 group_fixture();shopping_list_scroll_idx=1;shopping_list_delete_index(2);visible({0,1,3,4});assert_headers({"A","C"});
 assert(scroll_next(1)==3);shopping_list_scroll_idx=3;assert(scroll_next(-1)==1);
 assert(apply("id2",false)==-1);visible({0,1,2,3,4});assert_headers({"A","B","C"});
 group_fixture();shopping_list_delete_index(0);visible({1,2,3,4});assert_headers({"A","B","C"});
 group_fixture();shopping_list_scroll_idx=4;shopping_list_delete_index(4);visible({0,1,2,3});assert(shopping_list_scroll_idx==3);
 assert_headers({"A","B","C"});
 // Last item: honest empty projection, rollback, then one confirmed mutation.
 reset();g_active.count=1;shopping_list_screen_populate();shopping_list_delete_index(0);visible({});assert(g_active.count==1&&saves==0);
 assert(apply("6",false)==-1);visible({0});shopping_list_delete_index(0);assert(apply("6",true)==0);visible({});assert(g_active.count==0&&saves==1);
 reset();ui_screen_state=0;shopping_list_delete_index(0);assert(repaints==0);apply("6",false);assert(repaints==0&&toasts==1);
 reset();shopping_list_reveal_pending=true;shopping_list_delete_index(0);assert(!shopping_list_reveal_pending);
 puts("PASS LCD projected delete: actual populate/encoder/header helpers; immediate view, cache retained, rollback and confirmed commit; enqueue retains row; only matching success removes once; failures/timeouts/late or wrong IDs retain; duplicate requests blocked; selected ID escapes");
}
'''


def render_stubs():
    return r'''
struct lv_obj_t {std::string text;int source=-1;};
struct lv_anim_t{};
using lv_opa_t=int;
static std::vector<std::unique_ptr<lv_obj_t>> allocations;
static lv_obj_t scroll_obj,title_label,count_label;
static lv_obj_t* shopping_list_scroll=&scroll_obj,*shopping_list_title_label=&title_label,*shopping_list_count_label=&count_label;
static lv_obj_t* shopping_list_items[50]={},*shopping_list_item_headers[50]={};
static int shopping_list_rendered_count=0,repaints=0,revealed=-1;
static uint32_t shopping_list_rendered_sig=0;
static bool shopping_list_touch_pull_consumed=false,shopping_list_touch_pull_armed=false,shopping_list_reveal_pending=false,g_list_refresh_completed_once=true;
static int refresh_state=0,refresh_timeout_count=0;
static constexpr int REFRESH_IDLE=0,REFRESH_WAKE_PENDING=1,REFRESH_INFLIGHT=2,REFRESH_FAILED=3,LIST_HONEST_FAIL_TIMEOUTS=3;
static constexpr int LV_ANIM_OFF=0,LV_PART_MAIN=0,LV_OPA_COVER=255,LV_OPA_TRANSP=0,LV_TEXT_ALIGN_CENTER=0,LV_OBJ_FLAG_SCROLLABLE=1,LV_LABEL_LONG_DOT=0;
static constexpr int COL_MUTED=0,COL_DARK=1,SHOPPING_LIST_CARD_W=272,SHOPPING_LIST_CARD_H=38,SHOPPING_LIST_CARD_RADIUS=12;
static int lv_font_montserrat_32,nunito_22,nunito_12,lv_font_montserrat_14,lv_font_montserrat_16;
static const char* LV_SYMBOL_WARNING="warning",*LV_SYMBOL_LIST="list";
static lv_obj_t* lv_obj_create(lv_obj_t*){allocations.emplace_back(new lv_obj_t);return allocations.back().get();}
static lv_obj_t* lv_label_create(lv_obj_t* p){return lv_obj_create(p);}
static void lv_label_set_text(lv_obj_t* p,const char* text){p->text=text;}
static void lv_obj_clean(lv_obj_t*){++repaints;}
static int lv_color_hex(int c){return c;}
static void shopping_list_style_card(lv_obj_t* card,int i){card->source=i;}
static void shopping_list_reveal_selection(int i,int){revealed=i;assert(i>=0&&i<50&&shopping_list_items[i]);}
static void shopping_list_render_skeleton(){}
static uint32_t shopping_list_content_sig(const List*){return 1;}
static lv_obj_t* shopping_list_add_store_header(const char* s){auto* p=lv_obj_create(shopping_list_scroll);p->text=s;return p;}
static void shopping_list_card_opa_anim_cb(void*,int32_t){}
static void lv_anim_path_ease_out(){}
#define NOOP(name) template<class... T>static void name(T...){}
NOOP(lv_obj_scroll_to_y) NOOP(lv_obj_set_style_text_font) NOOP(lv_obj_set_style_text_color)
NOOP(lv_obj_set_style_pad_top) NOOP(lv_obj_set_width) NOOP(lv_obj_set_style_text_align)
NOOP(lv_obj_set_size) NOOP(lv_obj_set_style_radius) NOOP(lv_obj_set_style_pad_left)
NOOP(lv_obj_set_style_pad_right) NOOP(lv_obj_set_style_pad_bottom) NOOP(lv_obj_set_style_border_width)
NOOP(lv_obj_set_style_bg_opa) NOOP(lv_obj_set_style_shadow_color) NOOP(lv_obj_clear_flag)
NOOP(lv_label_set_long_mode) NOOP(lv_anim_del) NOOP(lv_anim_init) NOOP(lv_anim_set_var)
NOOP(lv_anim_set_exec_cb) NOOP(lv_anim_set_values) NOOP(lv_anim_set_time) NOOP(lv_anim_set_delay)
NOOP(lv_anim_set_path_cb) NOOP(lv_anim_start)
#undef NOOP
static void shopping_list_screen_populate();
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[1])
    p.add_argument('--arduino-json', type=Path, default=Path.home() / 'Documents/Arduino/libraries/ArduinoJson/src')
    a = p.parse_args()
    compiler = shutil.which('clang++') or shutil.which('g++')
    assert compiler and (a.arduino_json / 'ArduinoJson.h').is_file()
    with tempfile.TemporaryDirectory(prefix='halo-lcd-delete-') as d:
        cpp = Path(d) / 'test.cpp'; exe = Path(d) / 'test'
        cpp.write_text(harness(a.source_root))
        subprocess.run([compiler, '-std=c++11', '-Wno-deprecated-declarations', '-I', str(a.arduino_json), str(cpp), '-o', str(exe)], check=True, timeout=30)
        subprocess.run([str(exe)], check=True, timeout=5)


if __name__ == '__main__':
    main()
