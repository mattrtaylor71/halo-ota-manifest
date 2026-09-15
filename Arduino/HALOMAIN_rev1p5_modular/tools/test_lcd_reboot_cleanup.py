"""Native actual cleanup/probe regression: forgotten session after LCD reset.

No hardware, network, or firmware build. Fake UART/clock boundaries run the
production nonce polling, proof predicate and original bounded cleanup helper.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_retry_peer_ready import definition

ROOT = Path(__file__).resolve().parents[1]


def harness():
    src = (ROOT / 'Sense_Minimal/sense_ota_lcd.h').read_text()
    extracted = '\n'.join(definition(src, name, structure) for name, structure in (
        ('struct LcdOtaCleanupResult', True),
        ('template<class Send, class Receive, class Remaining>', False),
        ('struct LcdOtaQuerySnapshot', True),
    ))
    ready = definition(src, 'static bool sense_lcd_reboot_cleanup_ready(')
    probe = definition(src, 'template<class Remaining>')
    fail = definition(src, 'auto fail =') + ';'
    return r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <atomic>
#include <string>
#include <vector>
static uint32_t now_ms, started, available, response_delay, persist_delay;
static unsigned sends, query_calls, confirms;
static bool ack_cleanup;
static uint32_t millis(){return now_ms;}
static uint32_t esp_random(){return 45;}
static void delay(uint32_t n){now_ms+=n;}
''' + extracted + r'''
static bool g_lcd_ota_proxy_owns_uart, g_lcd_ota_task_running, s_lcd_query_pending;
static std::atomic<bool> g_lcd_ota_mode_unconfirmed{true};
static std::atomic<bool> s_lcd_query_proof_held{false};
// Admission contention is covered by the dedicated photo/query harness. This
// fixture keeps the cleanup proof and original operation deadlines isolated.
struct UartJsonTxLock { bool held() const { return true; } };
static uint32_t s_lcd_reboot_cleanup_boot;
static LcdOtaQuerySnapshot reply;
enum {LCD_QUERY_WAITING,LCD_QUERY_READY,LCD_QUERY_TIMEOUT};
static bool sense_lcd_ota_query_start(const char* nonce,uint32_t n){
  assert(!strncmp(nonce,"recover_",8)&&n&&n<=5000);
  ++query_calls;s_lcd_query_pending=true;return true;
}
static void pump_uart_rx_once(){now_ms+=response_delay;response_delay=0;}
static int sense_lcd_ota_query_poll(LcdOtaQuerySnapshot& out,bool confirm){
  assert(!confirm);out=reply;s_lcd_query_pending=false;s_lcd_query_proof_held=true;return LCD_QUERY_READY;
}
static void sense_lcd_mode_confirm(){++confirms;g_lcd_ota_mode_unconfirmed=false;now_ms+=persist_delay;}
static uint32_t remaining(){
  const uint32_t elapsed=uint32_t(now_ms-started);
  return elapsed<available?available-elapsed:0;
}
struct LcdOtaTransportSnapshot {};
static LcdOtaTransportSnapshot sense_lcd_transport_snapshot(int){return {};}
static bool sense_lcd_transport_format(unsigned,const LcdOtaTransportSnapshot&,char*,size_t){return false;}
static unsigned s_lcd_terminal_store_failures;
static std::vector<std::string> terminals;
static void sense_lcd_terminal_store(const char* detail,int){terminals.emplace_back(detail);}
static void sense_lcd_terminal_flush(){}
static struct {void println(const char*){}} Serial;
''' + ready + '\n' + probe + r'''
static LcdOtaQuerySnapshot baseline(){
  now_ms=started=0;available=2400000;response_delay=50;persist_delay=0;
  sends=query_calls=confirms=0;s_lcd_reboot_cleanup_boot=0;
  ack_cleanup=false;terminals.clear();
  g_lcd_ota_mode_unconfirmed=true;g_lcd_ota_proxy_owns_uart=g_lcd_ota_task_running=false;
  LcdOtaQuerySnapshot before{};
  strcpy(before.fw,"6.4.139");strcpy(before.running_part,"app1");strcpy(before.boot_part,"app1");
  strcpy(before.running_state,"VALID");strcpy(before.coord_owner,"owner1");
  before.peer_boot_id=123;before.part_size=2621440;before.boot_ready=true;
  reply=before;reply.peer_boot_id=456;reply.correlated=reply.recovery_idle=true;
  reply.coord_owner[0]=0;reply.coord_lease_ms=0;
  return before;
}
static bool run(const LcdOtaQuerySnapshot& before){
  // Compile and run the actual proxy failure call site, including owner release,
  // original cleanup reservation and the subsequent mode/debt decision.
  const LcdOtaQuerySnapshot cleanup_baseline=before;
  const char* phase="chunk";
  int tls_client=0;
  const uint16_t session_id=32929;
  bool receiver_open=true,json_ready=false,mode_marked=true;
  auto remaining_ms=[](){return remaining();};
  auto phase_limit=[](uint32_t n){return n<remaining()?n:remaining();};
  auto close_stream=[](){};
  auto record=[](const char*,int){};
  auto send_control=[](const char* type,const char*,bool){assert(!strcmp(type,"LCD_OTA_ABORT"));++sends;return true;};
  auto receive_control=[](uint32_t n,bool){
    if(ack_cleanup){now_ms+=50;return "receiver_aborted";}
    now_ms+=n;return "control_ack_timeout";
  };
  struct {unsigned crc_error_count(){return 0;}unsigned frame_error_count(){return 0;}} protocol;
  g_lcd_ota_proxy_owns_uart=true;
''' + fail + r'''
  const char* result=fail("chunk_retry_exhausted",-1);
  assert(!strcmp(result,"chunk_retry_exhausted")&&!g_lcd_ota_proxy_owns_uart);
  assert(!s_lcd_query_proof_held);
  assert(uint32_t(now_ms-started)<=36000);
  return json_ready;
}
int main(){
  auto before=baseline();assert(run(before));
  assert(now_ms==31050&&query_calls==1&&confirms==2&&s_lcd_reboot_cleanup_boot==456);
  assert(!g_lcd_ota_mode_unconfirmed&&sends==3&&remaining()==2368950);
  assert(terminals.size()==1&&terminals.back().find("ack=0")!=std::string::npos&&
    terminals.back().find("reboot=1")!=std::string::npos&&terminals.back().find("ms=31050")!=std::string::npos);
  // A normal same-session cleanup ACK still wins immediately, with no query.
  before=baseline();ack_cleanup=true;assert(run(before));
  assert(now_ms==50&&sends==1&&!query_calls&&!s_lcd_reboot_cleanup_boot&&confirms==1);
  unsigned negatives=0;
#define REJECT(change) do{before=baseline();change;assert(!run(before));assert(!confirms&&g_lcd_ota_mode_unconfirmed&&!s_lcd_reboot_cleanup_boot);++negatives;}while(0)
  REJECT(reply.peer_boot_id=123); // same-boot wrong/forgotten session cannot be healed by a query
  REJECT(reply.peer_boot_id=0);REJECT(reply.correlated=false); // stale/wrong nonce
  REJECT(reply.recovery_idle=false); // newer active receive, binary mode or open handle
  REJECT(reply.boot_ready=false);REJECT(strcpy(reply.running_state,"PENDING_VERIFY"));
  REJECT(strcpy(reply.running_state,"INVALID"));REJECT(strcpy(reply.boot_part,"app0"));
  REJECT(strcpy(reply.fw,"6.4.140"));REJECT(reply.part_size--);
  REJECT(strcpy(reply.running_part,"app0");strcpy(reply.boot_part,"app0"));
  REJECT(strcpy(reply.coord_owner,"new-owner");reply.coord_lease_ms=1000);
  REJECT(strcpy(reply.coord_owner,"owner1");reply.coord_lease_ms=0);
  REJECT(reply.coord_lease_ms=1000);REJECT(strcpy(reply.coord_owner,"owner1");reply.coord_lease_ms=120001);
  REJECT(before.peer_boot_id=0);REJECT(before.boot_ready=false);
  REJECT(strcpy(before.running_state,"PENDING_VERIFY"));
  REJECT(response_delay=5000); // response at exact cleanup deadline is too late
  REJECT(available=31050;response_delay=5000); // original work deadline also wins
#undef REJECT
  before=baseline();strcpy(reply.coord_owner,"owner1");reply.coord_lease_ms=120000;
  assert(run(before)&&s_lcd_reboot_cleanup_boot==456);
  before=baseline();persist_delay=4950;assert(!run(before));
  assert(confirms==1&&g_lcd_ota_mode_unconfirmed&&!s_lcd_reboot_cleanup_boot&&now_ms==36000);
  before=baseline();started=now_ms=0xfffffff0U;assert(run(before));
  assert(uint32_t(now_ms-started)==31050&&remaining()==2368950);
  assert(negatives==20);
  puts("PASS actual proxy fail lambda: forgotten-session reboot proof; ordinary ACK; 20 negatives; one query; <=36s; millis wrap");
}
'''


class LcdRebootCleanupTests(unittest.TestCase):
    def test_production_cleanup_and_reboot_probe(self):
        with tempfile.TemporaryDirectory(prefix='halo-reboot-cleanup-') as tmp:
            cpp = Path(tmp) / 'test.cpp'
            binary = Path(tmp) / 'test'
            cpp.write_text(harness())
            subprocess.run([shutil.which('c++'), '-std=c++17', str(cpp), '-o', str(binary)],
                           check=True, timeout=30)
            subprocess.run([str(binary)], check=True, timeout=5)

    def test_query_omission_never_inherits_idle(self):
        source = (ROOT / 'Sense_Minimal/Sense_Minimal.ino').read_text()
        begin = source.index('} else if (strcmp(type, "LCD_OTA_QUERY_RESP") == 0)')
        end = source.index('} else if (strcmp(type, "LCD_OTA_BEGIN_ACK") == 0)', begin)
        parser = source[begin:end]
        self.assertIn('g_lcd_query_recovery_idle = false;', parser)
        self.assertIn('doc["recovery_idle"].is<bool>() && (doc["recovery_idle"] | false)', parser)


if __name__ == '__main__':
    unittest.main()
