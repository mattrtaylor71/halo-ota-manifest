#!/usr/bin/env python3
"""Exercise actual LCD delete helpers with fake queue, state, persistence and clock."""
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
    assert 'if (removed >= 0 && ui_screen_state == SCREEN_SHOPPING_LIST) shopping_list_animate_card_removal(removed)' in ui
    assert 'shopping_list_overlay_item_id' in screens
    return r'''
#include <ArduinoJson.h>
#include <cstdio>
#include <cstring>
#include <cassert>
struct List {int count;char items[7][64],item_ids[7][64],stores[7][48];} g_active;
struct tx_msg_t {char type[24];int delta;char id[40];bool has_delta,has_id;};
static void* app_state_mutex=(void*)1;
static bool lock_ok=true,queue_ok=true;
static unsigned long now_ms=100,shopping_list_pending_delete_ms;
static unsigned long millis(){return now_ms;}
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
''' + functions + r'''
static void reset(){
 g_active={};g_active.count=2;strcpy(g_active.items[0],"Olives");strcpy(g_active.item_ids[0],"6");
 strcpy(g_active.items[1],"Apples");strcpy(g_active.item_ids[1],"3");
 deleted_item_count=enqueues=saves=toasts=shopping_list_scroll_idx=0;
 shopping_list_pending_delete_id[0]=0;queue_ok=lock_ok=true;now_ms=100;
}
static void retained(){assert(g_active.count==2&&saves==0&&deleted_item_count==0);}
int main(){
 reset();queue_ok=false;assert(!shopping_list_delete_index(0)[0]);retained();assert(!shopping_list_pending_delete_id[0]);
 reset();lock_ok=false;assert(!shopping_list_delete_index(0)[0]);retained();assert(enqueues==0);
 reset();assert(!shopping_list_delete_index(0,"3")[0]);retained();assert(enqueues==0);
 reset();g_active.item_ids[0][0]=0;assert(!shopping_list_delete_index(0)[0]);retained();assert(enqueues==0);
 reset();memset(g_active.item_ids[0],'x',40);g_active.item_ids[0][40]=0;assert(!shopping_list_delete_index(0)[0]);retained();assert(enqueues==0);
 reset();assert(!strcmp(shopping_list_delete_index(0,"6"),"6"));retained();assert(enqueues==1&&!strcmp(shopping_list_pending_delete_id,"6"));
 assert(!shopping_list_delete_index(0)[0]&&!shopping_list_delete_index(1)[0]&&enqueues==1);retained();
 assert(shopping_list_apply_delete_result("3",true)==-1);retained();assert(!strcmp(shopping_list_pending_delete_id,"6"));
 assert(shopping_list_apply_delete_result("6",true)==0);assert(g_active.count==1&&saves==1&&!strcmp(g_active.item_ids[0],"3"));
 assert(deleted_item_count==1&&!strcmp(deleted_item_ids[0],"6"));assert(shopping_list_apply_delete_result("6",true)==-1&&saves==1);
 reset();shopping_list_delete_index(0);assert(shopping_list_apply_delete_result("6",false)==-1);retained();assert(!shopping_list_pending_delete_id[0]);
 reset();shopping_list_delete_index(0);now_ms+=59999;shopping_list_expire_pending_delete();assert(shopping_list_pending_delete_id[0]);
 ++now_ms;shopping_list_expire_pending_delete();retained();assert(!shopping_list_pending_delete_id[0]);
 assert(shopping_list_apply_delete_result("6",true)==-1);retained();
 assert(!strcmp(shopping_list_delete_index(1),"3"));assert(shopping_list_apply_delete_result("6",true)==-1);retained();
 reset();shopping_list_delete_index(0);now_ms+=60000;assert(shopping_list_apply_delete_result("6",true)==-1);retained();
 reset();shopping_list_delete_index(0);strcpy(g_active.item_ids[0],"3");strcpy(g_active.item_ids[1],"6");
 assert(shopping_list_apply_delete_result("6",true)==1&&g_active.count==1&&!strcmp(g_active.item_ids[0],"3"));
 reset();strcpy(g_active.item_ids[0],"6\"\\\n");
''' + selected + r'''
 StaticJsonDocument<128> decoded;assert(!deserializeJson(decoded,selected_id_json));assert(!strcmp(decoded.as<const char*>(),g_active.item_ids[0]));
 puts("PASS LCD delayed delete: enqueue retains row; only matching success removes once; failures/timeouts/late or wrong IDs retain; duplicate requests blocked; selected ID escapes");
}
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
