#!/usr/bin/env python3
"""Seeded host stress of real LCD media handlers/UI and Sense custody/admission.

No serial, network, firmware edits, or device operations. Runtime functions are
included/extracted by the existing actual-source harnesses; only host boundaries
and test drivers are composed here. This is finite fault coverage, not exhaustive
hardware/RTOS/power-cut assurance. --out must be a new evidence directory.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import test_lcd_media_foreground as foreground
import test_media_busy_custody as busy
import test_voice_uart_ownership as uart

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OLD = Path('/Users/MattTaylor/halo-device-analytics-2026-09-10/offline-backup-20260915/candidate163-002/snapshot/source')
SEEDS = [0x163A11CE, 0x5EED2031, 0xFFFFFFFF, 0x9E3779B9]

LCD = r'''
static uint32_t random_state,seed_value,case_number;
static unsigned coverage[6]={},subcoverage[24]={};
static uint32_t rnd(){uint32_t x=random_state;x^=x<<13;x^=x>>17;x^=x<<5;return random_state=x;}
static unsigned hook_at,hook_seen,hook_action;
static uint32_t gesture_hold;
static void stress_hook(uint8_t type){
 if(type!=MSG_IMG_CHUNK&&type!=MSG_IMG_END)return;
 if(hook_seen++!=hook_at)return;
 if(hook_action==1)camera();
 if(hook_action==2){press();tick_ms+=gesture_hold;held();tick_ms+=37;release_touch();}
 drain_queue();check(delivered.empty(),"foreground callbacks cannot transmit before terminal JSON proof");
}
static std::vector<uint8_t> saved_bytes(const NAMESPACE::Meta& m){
 char path[NAMESPACE::kPathBytes];g_MEDIA_store.payload_path(m.request_id,path,sizeof(path));
 FILE* f=std::fopen(path,"rb");check(f!=nullptr,"committed payload still exists");std::vector<uint8_t>b;
 if(f){int c;while((c=std::fgetc(f))!=EOF)b.push_back((uint8_t)c);std::fclose(f);}return b;
}
static void bound_abort(const NAMESPACE::Meta& m){JsonDocument a;deserializeJson(a,abort_json(m));lcd_MEDIA_uart(a);}
static void save_chunks(const NAMESPACE::Meta& m,bool duplicate,bool wrong_abort,bool lost_ack){
 begin(m);check(g_MEDIA_rx,"fresh SAVE admitted");
 uint16_t seq=0;
 for(size_t off=0;off<PAYLOAD.size();off+=MAX_CHUNK_SIZE,++seq){
  std::vector<uint8_t> chunk(PAYLOAD.begin()+off,PAYLOAD.begin()+std::min(PAYLOAD.size(),off+MAX_CHUNK_SIZE));
  if(wrong_abort&&(seq%2)==0){auto wrong=m;wrong.job_id++;input_events.push_back({UartOtaProtocol::JSON,0,0,{},abort_json(wrong)});lcd_MEDIA_receive_loop();check(g_MEDIA_rx,"stale abort cannot terminate SAVE");}
  frame(MSG_IMG_CHUNK,seq,chunk);check(lcd_MEDIA_receive_loop(),"multi-chunk payload progresses");
  if(duplicate&&(seq%2)==0){frame(MSG_IMG_CHUNK,seq,chunk);check(lcd_MEDIA_receive_loop(),"exact duplicate chunk is idempotent");}
 }
 drop_commit_ack=lost_ack;frame(MSG_IMG_END,seq);check(!lcd_MEDIA_receive_loop()&&released(),"SAVE terminal releases owners");drop_commit_ack=false;
 check(retained(m)&&saved_bytes(m)==PAYLOAD,"committed payload is byte-exact");
 check(output_frames.back().type==MSG_IMG_ACK&&output_frames.back().seq==seq&&output_frames.back().bytes.size()==40,"terminal ACK has exact sequence and full custody proof");
 const uint8_t* p=output_frames.back().bytes.data();check(NAMESPACE::get32(p)==m.len&&NAMESPACE::get32(p)==m.crc32&&!memcmp(p,m.request_id,32),"terminal proof binds payload and immutable request");
 begin(m);auto r=last_reply();check(r["stored"]==1&&released()&&count(m)==1,"lost/duplicate terminal ACK recovered without a second slot");
}
int main(int argc,char**argv){
 seed_value=random_state=(uint32_t)strtoul(argv[1],nullptr,0);unsigned iterations=(unsigned)strtoul(argv[2],nullptr,0);
 for(case_number=0;case_number<iterations;++case_number){
  const unsigned start_failures=failures;reset_case();reset_ui();
  unsigned kind=case_number%6;coverage[kind]++;
  const size_t n=2*(1+rnd()%2048);PAYLOAD.resize(n);for(auto& b:PAYLOAD)b=(uint8_t)rnd();
  const bool wrap=(rnd()&1)!=0;tick_ms=wrap?UINT32_MAX-(1+rnd()%700):(1000+rnd()%100000);
  auto m=fixture(case_number+1);m.job_id=1+rnd()%60000;
  if(kind==0){
   save_chunks(m,rnd()&1,rnd()&1,rnd()&1);subcoverage[0]++;
  }else if(kind==1){
   unsigned fault=rnd()%5;subcoverage[1+fault]++;begin(m);
   std::vector<uint8_t> chunk(PAYLOAD.begin(),PAYLOAD.begin()+std::min(PAYLOAD.size(),MAX_CHUNK_SIZE));
   if(fault==0){frame(MSG_IMG_CHUNK,1,chunk);lcd_MEDIA_receive_loop();}
   if(fault==1){frame(MSG_IMG_CHUNK,0,chunk);lcd_MEDIA_receive_loop();chunk[0]^=1;frame(MSG_IMG_CHUNK,0,chunk);lcd_MEDIA_receive_loop();}
   if(fault==2){uint16_t s=0;for(size_t off=0;off<PAYLOAD.size();off+=MAX_CHUNK_SIZE,++s){std::vector<uint8_t>b(PAYLOAD.begin()+off,PAYLOAD.begin()+std::min(PAYLOAD.size(),off+MAX_CHUNK_SIZE));if(!off)b[0]^=1;frame(MSG_IMG_CHUNK,s,b);lcd_MEDIA_receive_loop();}frame(MSG_IMG_END,s);lcd_MEDIA_receive_loop();}
   if(fault==3){frame(MSG_IMG_END,0);lcd_MEDIA_receive_loop();}
   if(fault==4){tick_ms=g_MEDIA_started_ms+LCD_LIMIT;g_MEDIA_last_frame_ms=tick_ms-1;lcd_MEDIA_receive_loop();}
   check(released()&&count(m)==0,"invalid sequence/body/deadline never advertises durable success");
  }else if(kind==2){
   save_chunks(m,false,false,false);fetch(m);output_frames.clear();senseSerial.output.clear();json_pins.clear();
   unsigned mode=rnd()%6,proof=rnd()%3;subcoverage[6+mode]++;subcoverage[12+proof]++;
   peer=(Peer[]){Peer::Accept,Peer::WrongChunk,Peer::DropChunk,Peer::DropEnd,Peer::WrongEnd,Peer::AbortChunk}[mode];
   auto_abort=proof==0;abort_text=abort_json(m);if(proof==2){auto wrong=m;wrong.crc32^=1;abort_text=abort_json(wrong);}
   if(mode==5){auto_abort=false;abort_text=abort_json(m);}
   hook_action=rnd()%3;hook_at=rnd()%((PAYLOAD.size()+511)/512+1);hook_seen=0;gesture_hold=rnd()%1500;
   frame_hook=stress_hook;const uint32_t start=tick_ms;lcd_MEDIA_send_file();frame_hook=nullptr;
   check((uint32_t)(tick_ms-start)<10000&&pin_count==0,"loss/cancel/cleanup bounded and file closed");
   check(saved_bytes(m)==PAYLOAD,"every replay outcome preserves original exact SD bytes");
   if(hook_action==2&&hook_seen>hook_at)check(listening==0&&on_it==0&&queue_items.empty(),"busy physical voice never gives false local feedback or delayed edges");
   if(g_MEDIA_waiting_abort){
    check(g_suppress_uart_json_tx,"unconfirmed peer stays JSON quarantined");drain_queue();check(delivered.empty(),"queued user intent held while abort proof missing");
    auto wrong=m;wrong.job_id++;bound_abort(wrong);check(g_MEDIA_waiting_abort,"late wrong identity cannot end quarantine");
    tick_ms=g_MEDIA_started_ms+LCD_LIMIT-1;lcd_MEDIA_quarantine_tick();check(g_spool_tx_active,"original deadline has not expired early");
    tick_ms++;lcd_MEDIA_quarantine_tick();check(!g_spool_tx_active&&g_suppress_uart_json_tx&&g_MEDIA_waiting_abort,"wrap-safe original deadline releases only sleep custody");
    drain_queue();check(delivered.empty(),"retiring sleep custody never releases ordinary JSON");
    bound_abort(m);check(released(),"late matching proof releases quarantine");
   }
   check(released(),"every test supplies eventual exact mode proof");
   drain_queue();if(hook_action==1&&hook_seen>hook_at)check(delivered.size()==1&&delivered[0]=="INPUT_MENU_SELECT","queued camera survives and dispatches exactly once");
   bound_abort(m);check(released()&&saved_bytes(m)==PAYLOAD,"duplicate terminal proof is harmless and retains data");
  }else if(kind==3){
   // Host unsigned long is not ESP32's 32-bit type: UI timing cases use no wrap.
   tick_ms=100000;unsigned mode=rnd()%3;subcoverage[15+mode]++;
   if(mode==1)begin(m);if(mode==2){save_chunks(m,false,false,false);fetch(m);}
   queue_full=rnd()&1;press();tick_ms+=rnd()%750;held();tick_ms+=rnd()%2000;release_touch();
   if(mode){check(listening==0&&on_it==0&&queue_items.empty(),"SAVE/REPLAY consumes rejected whole gesture without false feedback");if(mode==2){peer=Peer::Accept;auto_abort=true;lcd_MEDIA_send_file();}else bound_abort(m);}
   else if(queue_full)check(on_it==0&&queue_items.empty()&&!g_lcd_media_voice_gesture&&g_lcd_media_queued_intents==0,"queue-full voice never claims accepted input");
   else {drain_queue();check(g_lcd_media_queued_intents==0&&!g_lcd_media_voice_gesture,"idle gesture retires all intent owners");}
  }else if(kind==4){
   tick_ms=10000;const unsigned total=8+rnd()%25;const auto drops=deferred_ring_dropped;
   proof_ready=false;
   for(unsigned i=0;i<total;++i){enqueue_hook=[](){check(!lcd_media_try_claim(true,false),"queue-intent publication prevents overlapping replay claim");};camera();if(rnd()%2)drain_queue();}
   while(!queue_items.empty())drain_queue();
   check(deferred_ring_count==4&&deferred_ring_dropped-drops==total-4,"actual bounded deferred queue evicts oldest with counted overflow");
   check(g_lcd_media_queued_intents==0&&lcd_media_deferred_intent_pending()&&!lcd_media_try_claim(true,lcd_media_deferred_intent_pending()),"deferred camera still blocks replay after queue count retires");
   deferred_ring_clear();check(lcd_media_try_claim(true,false),"retiring all queued intents restores admission");lcd_media_release();
  }else{
   unsigned refusal=rnd()%5;subcoverage[18+refusal]++;
   if(refusal==0)ota_busy=true;if(refusal==1)g_lcd_sleep_commit_gate=true;if(refusal==2)image_valid=false;if(refusal==3)mount_ok=false;if(refusal==4)camera();
   begin(m);auto r=last_reply();check(r["ok"]==0&&!g_MEDIA_rx&&!g_spool_tx_active,"OTA/sleep/SDK/mount/foreground guard refuses SAVE");
   check(count(m)==0,"refused admission creates no committed slot");
  }
  check(pin_count==0&&lease_depth==0&&!media_critical_depth,"case exits with all SD pins and critical sections closed");
  if(failures!=start_failures){fprintf(stderr,"REPLAY seed=%u case=%u kind=%u wrap=%u payload=%zu\n",seed_value,case_number,kind,wrap,n);return 1;}
 }
 printf("STRESS %s seed=%u cases=%u checks=%u coverage=",failures?"FAIL":"PASS",seed_value,iterations,checks);for(auto x:coverage)printf("%u,",x);printf(" subcoverage=");for(auto x:subcoverage)printf("%u,",x);puts("");return failures?1:0;
}
'''

BUSY = r'''
static uint32_t random_state;
static uint32_t rnd(){uint32_t x=random_state;x^=x<<13;x^=x>>17;x^=x<<5;return random_state=x;}
int main(int argc,char**argv){
 unsigned seed=random_state=(uint32_t)strtoul(argv[1],nullptr,0),iterations=(unsigned)strtoul(argv[2],nullptr,0);
 unsigned coverage[4]={};
 for(unsigned i=0;i<iterations;++i){
  const bool voice=rnd()&1;auto j=prepare(voice);now_ms=(rnd()&1)?UINT32_MAX-(rnd()%1000):1000;
  const unsigned context=rnd()%4;current_task=context==1?2:1;uart_dispatch_depth=context==2?1:0;if(context==3)g_sense_main_task_handle=0;
  const unsigned scenario=i%4;coverage[scenario]++;g_upload_persist_ready=false;
  busy_count=rnd()%35;unsigned expected_busy=busy_count;
  if(scenario==1){io_count=1;script_success=true;}
  if(scenario==2){io_count=2;script_success=false;}
  if(scenario==3){busy_forever=true;expected_busy=479;}
  const auto initial=files;const uint32_t started=millis();bool saved=upload_persist_handle_failure(j,"seeded_transport");
  check(saved==(scenario<2),"actual failure handler distinguishes busy, eventual storage success, I/O exhaustion and deadline");
  check((uint32_t)(millis()-started)<120000&&g_media_custody_waiters==0,"one wrap-safe operation deadline and no leaked RAM custody");
  check(storage_calls==(scenario==3?480:expected_busy+1+(scenario==1||scenario==2)),"busy events do not consume the genuine I/O attempt budget");
  check(busy_pumps==(context==0?expected_busy:0)&&busy_collects==(context==0?0:expected_busy),"only main depth-zero dispatches pending gesture; worker and nested paths collect");
  check(files==initial&&writes==0,"busy/I/O denial does not overwrite unrelated SPIFFS");
 }
 printf("STRESS PASS seed=%u cases=%u checks=%u coverage=%u,%u,%u,%u\n",seed,iterations,checks,coverage[0],coverage[1],coverage[2],coverage[3]);
}
'''

REFUSAL = r'''
int main(int argc,char**argv){
 uart_json_tx_init();uint32_t state=(uint32_t)strtoul(argv[1],nullptr,0);unsigned n=(unsigned)strtoul(argv[2],nullptr,0),coverage[10]={};
 for(unsigned i=0;i<n;++i){
  state^=state<<13;state^=state>>17;state^=state<<5;prepare();begin_mutation=(i+state)%10;coverage[begin_mutation]++;begin_count=0;
  if(state&1)now=UINT32_MAX-(state%1000);bool busy=true;
  bool saved=sense_MEDIA_spool_store(stored,millis()+20000,&busy);
  check(!saved&&begin_count==1,"negative READY never invents committed custody");
  check(busy==(begin_mutation==0),"strict typed full tuple is required for foreground-busy retry");
  check(!uart_rx_mutex->held&&!uart_json_tx_mutex->held&&!g_img_spool_tx_active,"all negative READY paths release actual shared UART lease");
 }
 printf("STRESS PASS cases=%u checks=%u coverage=",n,checks);for(auto c:coverage)printf("%u,",c);puts("");
}
'''

UART = r'''
static uint32_t random_state;
static uint32_t rnd(){uint32_t x=random_state;x^=x<<13;x^=x>>17;x^=x<<5;return random_state=x;}
int main(int argc,char**argv){
 uart_json_tx_init();const unsigned seed=random_state=(uint32_t)strtoul(argv[1],nullptr,0),iterations=(unsigned)strtoul(argv[2],nullptr,0);unsigned coverage[8]={};
 const std::string input="{\"ver\":1,\"type\":\"INPUT_MENU_SELECT\",\"menu_item\":\"Dish\",\"msg_id\":17,\"ts\":1}";
 const std::string good="{\"ver\":1,\"type\":\"VOICE_XFER_READY\",\"voice_schema\":1,\"request_id\":\"wanted\",\"ok\":1}";
 const std::string wrong="{\"ver\":1,\"type\":\"VOICE_XFER_READY\",\"voice_schema\":1,\"request_id\":\"old\",\"ok\":1}";
 for(unsigned i=0;i<iterations;++i){
  reset();now=(rnd()&1)?UINT32_MAX-(1+rnd()%50):1000+rnd()%1000;unsigned mode=i%8;coverage[mode]++;
  size_t cut=1+rnd()%(input.size()-1);
  if(mode==0||mode==1){
   lcdSerial.bytes=input.substr(0,cut);pump_uart_rx_once();check(dispatches==0&&uart_rx_frame_len==cut,"fragment has no premature callback");
   if(mode==0){lcdSerial.bytes=input.substr(cut)+"\n"+wrong+"\n"+good+"\n"+std::string("\0COBS",5);
    {SenseVoiceUartLease lease(millis()+1000);JsonDocument r;check(lease.held()&&sense_voice_spool_read(r,"VOICE_XFER_READY","wanted",millis()+1000),"matching response after fragmented ordinary frame");
     check(dispatches==0&&lcdSerial.bytes==std::string("\0COBS",5),"reader retains callback and never consumes following binary bytes");}
    lcdSerial.bytes.clear();pump_uart_rx_once();check(dispatched.size()==2&&dispatched[0]==input&&dispatched[1]==wrong,"seeded fragmentation preserves ordinary input and stale reply exactly");
   }else{
    {SenseVoiceUartLease lease(millis()+25);JsonDocument r;check(!sense_voice_spool_read(r,"VOICE_XFER_READY","wanted",millis()+25),"partial response timeout bounded across wrap");}
    lcdSerial.bytes=input.substr(cut)+"\n";pump_uart_rx_once();check(dispatched.size()==1&&dispatched[0]==input,"timeout retains incomplete ordinary frame for successor owner");
   }
  }else if(mode==2){
   const unsigned count=1+rnd()%8;for(unsigned n=0;n<count;++n)lcdSerial.bytes+=wrong+"\n";lcdSerial.bytes+=good+"\n";
   {SenseVoiceUartLease lease(millis()+1000);JsonDocument r;check(sense_voice_spool_read(r,"VOICE_XFER_READY","wanted",millis()+1000),"duplicate wrong identities cannot impersonate owned reply");}
   pump_uart_rx_once();check(dispatched.size()==count,"unmatched duplicate control lines preserved without executing under raw lease");
  }else if(mode==3){
   std::string retained;while(retained.size()+input.size()+1<=UART_RX_RING_SIZE)retained+=input+"\n";
   {UartRxLock rx;for(char c:retained)uart_ring_push(c);}const auto dropped=uart_rx_dropped_since_frame;
   lcdSerial.bytes=wrong+"\n"+good+"\n";
   {SenseVoiceUartLease lease(millis()+1000);JsonDocument r;check(sense_voice_spool_read(r,"VOICE_XFER_READY","wanted",millis()+1000),"owned response survives saturated ordinary ring");
    check(uart_rx_ring_count==retained.size()&&uart_rx_dropped_since_frame-dropped==wrong.size()+1,"full ring drops only whole new frame with exact count");}
   pump_uart_rx_once();check(dispatched.size()==retained.size()/(input.size()+1),"all prior admitted frames survive capacity refusal");
  }else if(mode==4){
   lcdSerial.bytes=input+"\n";bool boundary=false;
   on_unlock=[&](SemaphoreHandle_t s){if(s!=uart_rx_mutex||!uart_dispatch_depth.load())return;on_unlock=nullptr;boundary=true;
    SenseVoiceUartLease voice(millis()+100);SenseImgSpoolLease photo(42);check(!voice.held()&&!photo.held(),"unlocked admitted callback rejects both new binary owners");};
   on_dispatch=[](){SenseVoiceUartLease voice(millis()+100);check(!voice.held(),"callback depth excludes binary voice claim until callback returns");};
   pump_uart_rx_once();check(boundary&&dispatches==1&&uart_dispatch_depth==0,"callback owner released after one complete dispatch");
  }else if(mode==5||mode==6){
   const uint32_t timeout=1+rnd()%100;LcdOtaQuerySnapshot out{};
   check(sense_lcd_ota_query_start("owned",timeout),"async query admitted");response("stale");check(sense_lcd_ota_query_poll(out)==LCD_QUERY_WAITING&&!confirms,"wrong query correlation cannot release media guard");
   if(mode==5){response("owned");check(sense_lcd_ota_query_poll(out)==LCD_QUERY_READY&&confirms==1,"bound query releases guard");}
   else {now+=timeout;response("owned");SenseVoiceUartLease voice(millis()+100);check(voice.held(),"expired query yields admission");g_lcd_ota_mode_unconfirmed=true;check(sense_lcd_ota_query_poll(out)==LCD_QUERY_TIMEOUT&&!confirms&&g_lcd_ota_mode_unconfirmed,"late expired query never clears successor quarantine");}
  }else{
   lcdSerial.bytes.assign(2000,'x');on_read=[](){++now;};const uint32_t timeout=1+rnd()%50;
   {SenseVoiceUartLease lease(millis()+timeout);JsonDocument r;check(!sense_voice_spool_read(r,"VOICE_XFER_READY","wanted",millis()+timeout)&&reads==timeout,"continuous input consumes only original bounded reader budget");}
  }
  check(!uart_rx_mutex->held&&!uart_json_tx_mutex->held&&!g_img_spool_tx_active&&uart_dispatch_depth==0,"UART stress case closes all local owners");
 }
 printf("STRESS PASS seed=%u cases=%u checks=%u coverage=",seed,iterations,checks);for(auto c:coverage)printf("%u,",c);puts("");
}
'''

def lcd_harness(root, media):
    code = foreground.harness(root, media).split('int main(){')[0]
    payload = 'pcm' if media == 'voice' else 'jpeg'
    code = code.replace('static const std::vector<uint8_t> '+payload, 'static std::vector<uint8_t> '+payload)
    # Keep the production headers unchanged; only the peer fixture gets a real
    # per-payload SHA for randomized images.
    if media == 'image':
        code = '#include <CommonCrypto/CommonDigest.h>\n' + code
        old = 'std::strcpy(m.checksum_sha256,'
        start = code.index(old)
        end = code.index(';', start)+1
        code = code[:start] + 'unsigned char h[32];CC_SHA256(jpeg.data(),(CC_LONG)jpeg.size(),h);for(unsigned i=0;i<32;++i)std::snprintf(m.checksum_sha256+2*i,3,"%02x",h[i]);' + code[end:]
    main = LCD.replace('NAMESPACE', 'halo_'+media).replace('MEDIA', media).replace('PAYLOAD', payload).replace('LCD_LIMIT', 'LCD_'+media.upper()+'_XFER_MS')
    return code+main

def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def pin(path): return {'path':str(path.resolve()),'sha256':sha(path)}
def sources(root):
    result={}
    for folder in ('LCD_Minimal','Sense_Minimal','halo_common'):
        for p in sorted((root/folder).rglob('*')):
            if p.is_file() and p.suffix in ('.h','.cpp','.ino'):
                result[str(p.relative_to(root))]=sha(p)
    for name in ('stress_media_transport.py','test_lcd_media_foreground.py','test_lcd_voice_transport.py','test_lcd_image_transport.py','test_media_busy_custody.py','test_voice_persistence.py','test_sense_image_spool.py','test_voice_uart_ownership.py','test_manual_ota_clock.py'):
        p=ROOT/'tools'/name;result['test_dependency:'+name]=sha(p)
    return result

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root',type=Path,default=ROOT)
    parser.add_argument('--negative-source-root',type=Path,default=DEFAULT_OLD)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--iterations',type=int,default=192)
    args=parser.parse_args()
    if not 12<=args.iterations<=1000:parser.error('iterations must be12..1000')
    args.out.mkdir(parents=True,exist_ok=False)
    before=sources(args.source_root);start=time.time();runs=[];result={'status':'RUNNING','host_only':True,'started_epoch':start,'source_root':str(args.source_root.resolve()),'seeds':SEEDS,'cases_per_seed_per_suite':args.iterations,'sources_before':before,'runs':runs}
    def save(): (args.out/'RESULT.json').write_text(json.dumps(result,indent=2)+'\n')
    save()
    compiler=shutil.which('clang++') or shutil.which('c++')
    specs=[('lcd-voice',lambda:lcd_harness(args.source_root,'voice')),('lcd-image',lambda:lcd_harness(args.source_root,'image')),('sense-custody',lambda:busy.custody_harness(args.source_root).split('int main(){')[0]+BUSY)]
    specs += [('sense-uart-fragments',lambda:uart.harness(args.source_root).split('int main(){')[0]+UART)]
    specs += [('sense-'+m+'-negative-ready',lambda m=m:busy.refusal_harness(args.source_root,m).split('int main(){')[0]+REFUSAL.replace('MEDIA',m)) for m in ('voice','image')]
    try:
        for name,make in specs:
            unit=args.out/name;unit.mkdir();src=unit/'harness.cpp';src.write_text(make());binary=unit/'test'
            argv=[compiler,'-std=c++17','-pthread','-Wno-deprecated-declarations','-I',str(args.source_root),'-I',str(args.source_root/'halo_ota_demo/firmware/halo_sense_prod/libraries/ArduinoJson/src'),str(src),'-o',str(binary)]
            r=subprocess.run(argv,capture_output=True,text=True,timeout=60);(unit/'compile.log').write_text(r.stdout+r.stderr)
            if r.returncode: raise RuntimeError(name+' compilation failed (not a runtime finding)')
            for seed in SEEDS:
                with tempfile.TemporaryDirectory(prefix='halo-media-seeded-') as td:
                    r=subprocess.run([str(binary),str(seed),str(args.iterations)],cwd=td,capture_output=True,text=True,timeout=60)
                log=unit/(str(seed)+'.log');log.write_text(r.stdout+r.stderr)
                runs.append({'suite':name,'seed':seed,'cases':args.iterations,'returncode':r.returncode,'log':pin(log),'harness':pin(src)})
                save()
                if r.returncode:raise RuntimeError(name+' deterministic failure; inspect seed '+str(seed))
        # A real pre-fix source must compile then demonstrate the historical
        # false Listening/On-it and delayed START+END collapse. No compile-error
        # or synthetic runtime mutation can count as this negative control.
        unit=args.out/'old-source-negative';unit.mkdir();src=unit/'harness.cpp';src.write_text(foreground.harness(args.negative_source_root,'voice',negative=True));binary=unit/'test'
        r=subprocess.run([compiler,'-std=c++17','-Wno-deprecated-declarations','-I',str(args.negative_source_root),'-I',str(args.negative_source_root/'halo_ota_demo/firmware/halo_sense_prod/libraries/ArduinoJson/src'),str(src),'-o',str(binary)],capture_output=True,text=True,timeout=60)
        (unit/'compile.log').write_text(r.stdout+r.stderr)
        if r.returncode:raise RuntimeError('negative control failed compilation; not a valid negative result')
        with tempfile.TemporaryDirectory(prefix='halo-media-old-negative-') as td:r=subprocess.run([str(binary)],cwd=td,capture_output=True,text=True,timeout=30)
        log=unit/'run.log';log.write_text(r.stdout+r.stderr)
        proved=r.returncode==1 and 'REPRODUCED old START/END delivered in same drain after 30 seconds' in r.stdout
        result['negative_control']={'status':'REPRODUCED' if proved else 'FAILED','source_root':str(args.negative_source_root),'returncode':r.returncode,'log':pin(log),'harness':pin(src)}
        if not proved:raise RuntimeError('historical runtime did not reproduce expected behavioral negative')
        result['sources_after']=sources(args.source_root)
        if before!=result['sources_after']:raise RuntimeError('source/test dependencies changed during stress run')
        result.update(status='PASS_HOST_SEEDED_TRANSPORT_STRESS',total_cases=sum(x['cases'] for x in runs),limitations=['Finite seeded host tests, not all possible faults or a physical test.','Actual production LCD handlers, foreground UI/queue branches, FILE/store logic and Sense custody/READY functions execute; UART frame events, RTOS/semaphores, visuals, time, and SD mount are controlled host boundaries.','The mock frame protocol does not validate COBS/electrical corruption. Host filesystem flush is not ESP/SD power-loss proof; independent storage stress covers syscall crash boundaries.','UI gesture timing uses non-wrapping host unsigned long; explicit uint32 transfer/cleanup and Sense operation deadlines exercise millis wrap.','Deferred queue overflow intentionally drops oldest with a counter; coverage verifies existing bounded policy, not lossless capacity.','No device, backend, network, source runtime, build, publication or firmware change.'])
    except Exception as e:
        result.update(status='FAIL_OR_TEST_SETUP_ERROR',error=str(e));raise
    finally:
        result['finished_epoch']=time.time();save()
    print(json.dumps({'status':result['status'],'total_cases':result['total_cases'],'result':pin(args.out/'RESULT.json')}))
if __name__=='__main__':main()
