#!/usr/bin/env python3
"""Run actual LCD storage budgets/handlers/Stores with user input during CRC.

Only serial, clock, SD lease/mount and one remove fault are mocked. Real files,
actual replay/SAVE ownership, Store traversal/CRC and complete LCD handlers run.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

import test_lcd_image_transport as image
import test_lcd_voice_transport as voice

ROOT = Path(__file__).resolve().parents[1]


def replace_once(source, old, new):
    assert source.count(old) == 1, old
    return source.replace(old, new, 1)


def harness(media):
    name = media.title()
    payload = 'jpeg' if media == 'image' else 'pcm'
    source = (image if media == 'image' else voice).HARNESS.split('int main(){', 1)[0]
    source = source.replace('#include <cstdio>', '#include <cstdio>\n#include <cerrno>\n#include <map>')
    source = replace_once(source, f'#include "halo_common/{name}SpoolStore.h"', r'''
static bool fail_remove=false;
static int injected_remove(const char* p){if(fail_remove){errno=EIO;return -1;}return ::remove(p);}
#define remove injected_remove
''' + f'#include "halo_common/{name}SpoolStore.h"\n#undef remove')
    source = replace_once(source, 'static void lcd_freeze_wdt_feed(){++tick_ms;}', r'''
static unsigned progress_calls=0,touch_after=0;
static void (*progress_hook)()=nullptr;
static void lcd_freeze_wdt_feed(){
  ++tick_ms;++progress_calls;
  if(progress_hook&&progress_calls==touch_after){auto hook=progress_hook;progress_hook=nullptr;hook();}
}''')
    source = replace_once(source, f'static const std::vector<uint8_t> {payload}{{1,2,3,4,5,6,7,8}};',
                          f'static const std::vector<uint8_t> {payload}(8194,42);')
    if media == 'image':
        source = source.replace('66840dda154e8a113c31dd0ad32f7f3a366a80e8136979d8f5a101d3d29d6f72',
                                hashlib.sha256(bytes([42]) * 8194).hexdigest())
    main = MAIN.replace('MEDIA', media).replace('UPPER', media.upper()).replace('PAYLOAD', payload)
    main = main.replace('LCD_'+media+'_SAVE', 'LCD_MEDIA_SAVE').replace('LCD_'+media+'_IDLE', 'LCD_MEDIA_IDLE')
    return source + main


MAIN = r'''
static void fresh_case(){
  progress_hook=nullptr;touch_after=progress_calls=0;fail_remove=false;
  reset_case();g_lcd_media_queued_intents=0;g_lcd_media_user_session=false;lcd_media_voice_end();
}
static void touch(){
  check(lease_depth>0,"injected accepted user input occurs during leased Store validation");
  lcd_media_note_user_input();
}
static void arm_touch(unsigned at){progress_calls=0;touch_after=at;progress_hook=touch;}
static uint16_t send_body(){
  uint16_t seq=0;
  for(size_t offset=0;offset<PAYLOAD.size();offset+=512){
    const size_t end=std::min(offset+512,PAYLOAD.size());
    frame(MSG_IMG_CHUNK,seq++,std::vector<uint8_t>(PAYLOAD.begin()+offset,PAYLOAD.begin()+end));
    check(lcd_MEDIA_receive_loop(),"SAVE body accepted");
  }
  return seq;
}
static void save(const halo_MEDIA::Meta& m){
  begin(m);check(g_lcd_media_mode==LCD_MEDIA_SAVE&&g_MEDIA_rx,"actual BEGIN admits SAVE owner");
  const auto seq=send_body();frame(MSG_IMG_END,seq);check(!lcd_MEDIA_receive_loop(),"actual END finishes SAVE");
  check(last_reply()["ok"]==1&&released()&&lease_depth==0&&g_lcd_media_mode==LCD_MEDIA_IDLE,"SAVE releases real handler owners");
}
static std::map<std::string,std::vector<uint8_t>> disk(const halo_MEDIA::Meta& m){
  char payload[256];check(g_MEDIA_store.payload_path(m.request_id,payload,sizeof(payload)),"payload path available");
  const auto root=std::filesystem::path(payload).parent_path();
  std::map<std::string,std::vector<uint8_t>> out;
  for(const auto& entry:std::filesystem::directory_iterator(root))if(entry.is_regular_file()){
    FILE* f=fopen(entry.path().c_str(),"rb");check(f!=nullptr,"host audit opens file");
    std::vector<uint8_t> bytes;int c;while((c=fgetc(f))!=EOF)bytes.push_back((uint8_t)c);fclose(f);
    out[entry.path().filename().string()]=bytes;
  }
  return out;
}
static void control(const halo_MEDIA::Meta& m,const char* type){
  JsonDocument d;d["type"]=type;d["MEDIA_schema"]=1;d["owner_id"]=m.owner_id;d["device_id"]=m.device_id;
  d["request_id"]=m.request_id;d["len"]=m.len;d["crc32"]=m.crc32;d["epoch"]=1789500000U;
  check(lcd_MEDIA_uart(d),"actual control handler recognized");
}
static void idle_user_request(){
  check(released()&&lease_depth==0&&g_lcd_media_mode==LCD_MEDIA_IDLE,"cancelled storage releases UART, sleep, SD and replay owners");
  check(g_lcd_media_user_session&&!lcd_media_try_claim(true,false),"remaining wake keeps user priority over new replay");
  check(lcd_media_voice_begin(),"next user recording is admitted immediately");lcd_media_voice_end();
  check(lcd_media_queue_begin("INPUT_MENU_SELECT"),"next camera/menu intent can queue without storage owner");
  lcd_media_queue_end("INPUT_MENU_SELECT");
}
int main(){
  for(bool lookup:{false,true})for(unsigned at:{1U,2U,4U}){
    fresh_case();auto m=fixture();save(m);const auto before=disk(m);arm_touch(at);
    const uint32_t started=tick_ms;
    control(m,lookup?"UPPER_SPOOL_FETCH":"UPPER_SPOOL_LIST_REQ");
    const auto r=last_reply();
    check(r["ok"]==0,"touch cancels actual list/lookup instead of completing replay validation");
    check(progress_calls==at&&tick_ms-started==at,"Store stops at first budget checkpoint after user input");
    check(disk(m)==before,"cancelled list/lookup preserves exact committed media files");
    idle_user_request();
    // Fresh custody is allowed even though user-session pause remains latched.
    progress_hook=nullptr;save(fixture(2));
  }
  std::puts("CASE six actual LIST/FETCH CRC interleavings: next budget yield, retained files, and new user/SAVE work");

  for(bool attempt:{false,true})for(unsigned at:{1U,4U,10U,12U}){
    fresh_case();auto m=fixture();if(attempt)m.epoch=0;save(m);const auto before=disk(m);
    arm_touch(at);const uint32_t started=tick_ms;
    const char* type=attempt?"UPPER_SPOOL_ATTEMPT":"UPPER_SPOOL_DELETE";control(m,type);
    check(last_reply()["ok"]==0,"touch cancels saved ATTEMPT/DELETE CRC validation");
    check(progress_calls==at&&tick_ms-started==at,"saved control yields at next budget checkpoint");
    const auto after=disk(m);
    for(const auto& file:before){const auto found=after.find(file.first);
      check(found!=after.end()&&found->second==file.second,"cancelled saved control retains original media/meta exactly");}
    const auto marker=std::string(m.request_id)+(attempt?".attempt":".delete");
    check(after.count(marker)==(at>9?1U:0U),"pause before marker retains original; pause after marker retains committed authority");
    idle_user_request();
    control(m,type);check(last_reply()["ok"]==0&&last_reply()["reason"]=="busy"&&disk(m)==after,
                         "rest-of-wake user priority refuses saved control without touching retained state");
    progress_hook=nullptr;save(fixture(2));
  }
  std::puts("CASE eight actual ATTEMPT/DELETE interleavings: before/after durable marker, held data, refused replay, fresh SAVE");

  for(unsigned at:{1U,2U,4U}){
    fresh_case();auto m=fixture();save(m);
    fail_remove=true;control(m,"UPPER_SPOOL_DELETE");fail_remove=false;
    check(last_reply()["ok"]==0&&released(),"simulated remove failure leaves accepted-delete recovery pending");
    const auto before=disk(m);check(before.count(std::string(m.request_id)+".delete")==1,"actual erase committed accepted-delete authority");
    arm_touch(at);const uint32_t started=tick_ms;control(m,"UPPER_SPOOL_LIST_REQ");
    check(last_reply()["ok"]==0,"touch cancels actual accepted-delete cleanup list");
    check(progress_calls==at&&tick_ms-started==at,"cleanup payload validation yields at next budget checkpoint");
    check(disk(m)==before,"cancelled cleanup retains both accepted marker and original payload/meta");
    idle_user_request();
    // Existing accepted cleanup can complete under a later foreground SAVE.
    progress_hook=nullptr;save(fixture(2));
    const auto after=disk(m);check(!after.count(std::string(m.request_id)+".delete"),"subsequent SAVE safely retires accepted marker");
  }
  std::puts("CASE accepted-marker cleanup: next budget yield, retained authority/data, released owners, subsequent SAVE cleanup");

  for(unsigned at:{2U,3U,5U}){
    fresh_case();auto m=fixture();begin(m);const auto seq=send_body();arm_touch(at);
    frame(MSG_IMG_END,seq);check(!lcd_MEDIA_receive_loop(),"SAVE finishes despite user input during CRC");
    check(g_lcd_media_user_session&&progress_calls>at,"user input occurred and SAVE continued validation");
    check(output_frames.back().type==MSG_IMG_ACK&&output_frames.back().bytes.size()==40,"SAVE retains exact durable commit proof");
    idle_user_request();check(retained(m),"fresh capture has durable readable custody after touch");
  }
  std::puts("CASE touch during SAVE checksum preserves fresh custody and commit acknowledgement");

  fresh_case();g_MEDIA_sd_work_started_ms=tick_ms;g_MEDIA_sd_work_budget_ms=12000;
  check(lcd_media_try_claim(false,false),"SAVE owner admitted for timeout boundary");
  lcd_media_note_user_input();check(lcd_MEDIA_sd_budget(),"SAVE ignores replay cancellation predicate");
  tick_ms+=11999;check(lcd_MEDIA_sd_budget(),"original budget allows just below limit");
  ++tick_ms;check(!lcd_MEDIA_sd_budget(),"original 12 second cap still applies to SAVE");lcd_media_release();
  std::printf("%s %u actual LCD MEDIA storage priority checks (%u failures)\n",failures?"FAIL":"PASS",checks,failures);
  delete g_MEDIA_proto;g_MEDIA_proto=nullptr;return failures?1:0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    compiler = shutil.which('clang++') or shutil.which('g++')
    if not compiler:
        parser.error('C++ compiler required')
    results = []
    for media in ('voice', 'image'):
        with tempfile.TemporaryDirectory(prefix='halo-lcd-storage-priority-') as directory:
            work = Path(directory);cpp=work/'main.cpp';exe=work/'test';cpp.write_text(harness(media))
            shutil.copy2(cpp,args.out/(media+'.cpp'))
            command=[compiler,'-std=c++17','-Wno-deprecated-declarations',
                     '-fsanitize=address,undefined','-fno-omit-frame-pointer',
                     '-I',str(args.source_root),'-I',str(args.source_root/'halo_ota_demo/firmware/halo_sense_prod/libraries/ArduinoJson/src'),
                     str(cpp),'-o',str(exe)]
            built=subprocess.run(command,capture_output=True,text=True,timeout=40)
            run=subprocess.run([str(exe)],cwd=work,capture_output=True,text=True,timeout=20) if built.returncode==0 else built
            output=built.stdout+built.stderr+run.stdout+run.stderr
            (args.out/(media+'.log')).write_text(output);print(output,end='')
            results.append({'media':media,'compile_exit':built.returncode,'exit_code':run.returncode,'log':str(args.out/(media+'.log'))})
    files=['LCD_Minimal/lcd_voice_spool.h','LCD_Minimal/lcd_image_spool.h','LCD_Minimal/lcd_media_foreground.h',
           'halo_common/VoiceSpoolStore.h','halo_common/ImageSpoolStore.h']
    (args.out/'results.json').write_text(json.dumps({'status':'PASS' if all(r['exit_code']==0 for r in results) else 'FAIL',
        'scope':'Actual handlers, budget callbacks, ownership and POSIX Store; scripted user input, clock, UART/SD boundaries; no hardware',
        'results':results,'source_sha256':{p:hashlib.sha256((args.source_root/p).read_bytes()).hexdigest() for p in files}},indent=2)+'\n')
    raise SystemExit(0 if all(r['exit_code']==0 for r in results) else 1)


if __name__ == '__main__':
    main()
