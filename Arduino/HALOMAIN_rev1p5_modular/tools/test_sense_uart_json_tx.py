"""Native concurrency checks of the actual Sense JSON sender (no device I/O).

Default: verify current source. Use --source-root OLD_SNAPSHOT --expect-joined
to reproduce the former two-write race from that unmodified source. Hardware
Serial and FreeRTOS boundaries are faked; the sender, ownership admission and
ArduinoJson parser are the production definitions.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
ARDUINO_JSON = Path.home() / 'Documents/Arduino/libraries/ArduinoJson/src'


def definition(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def harness(root, expect_joined):
    source = (root / 'Sense_Minimal/sense_uart.h').read_text()
    locked = 'class UartJsonTxLock' in source
    if not expect_joined:
        assert locked, 'Production JSON message lock is missing'
        init = definition(source, 'static void initUarts()')
        assert init.index('uart_json_tx_init();') < init.index('lcdSerial.begin(')
        main = (root / 'Sense_Minimal/Sense_Minimal.ino').read_text()
        setup = definition(main, 'void setup()')
        assert setup.index('initUarts();') < setup.index('xTaskCreatePinnedToCore(op_worker_task')
    state = (source[source.index('static StaticSemaphore_t uart_json_tx_mutex_storage;'):
                    source.index('// ── TX/RX type tracking')]
             if locked else 'static void uart_json_tx_init() {}')
    prefix = r'''
#include <ArduinoJson.h>
#include <cassert>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
using namespace std::chrono_literals;
static constexpr bool fixed = FIXED, expect_joined = EXPECT_JOINED;
static bool g_lcd_ota_proxy_owns_uart, g_spool_owns_uart, g_img_spool_tx_active;
static std::atomic<bool> g_lcd_ota_mode_unconfirmed{false};
static std::atomic<unsigned> uart_tx_count{0}, last_uart_tx_ms{0}, now_ms{100};
static unsigned millis() { return ++now_ms; }
static void uart_note_tx_type(const char*) {}
static std::mutex schedule_mutex, wire_mutex;
static std::condition_variable schedule_cv;
static bool first_payload, second_payload, second_attempted, inject_timeout;
static bool coordinate;
static int change_owner;
static thread_local bool second_thread, own_message_lock;
static std::string wire;
static unsigned creates;
static std::atomic<unsigned> skip_logs{0};
struct StaticSemaphore_t { std::timed_mutex mutex; };
using SemaphoreHandle_t = StaticSemaphore_t*;
static constexpr int pdTRUE = 1;
static unsigned pdMS_TO_TICKS(unsigned n) { return n; }
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* storage) {
  ++creates; return storage;
}
static int xSemaphoreTake(SemaphoreHandle_t s, unsigned wait_ms) {
  assert(wait_ms > 0 && wait_ms <= 2000);
  if (second_thread && coordinate) {
    std::lock_guard<std::mutex> lock(schedule_mutex);
    second_attempted = true;
    if (change_owner == 1) g_lcd_ota_mode_unconfirmed = true;
    if (change_owner == 2) g_lcd_ota_proxy_owns_uart = true;
    if (change_owner == 3) g_spool_owns_uart = true;
    if (change_owner == 4) g_img_spool_tx_active = true;
    schedule_cv.notify_all();
  }
  if (inject_timeout || !s->mutex.try_lock_for(std::chrono::milliseconds(wait_ms))) return 0;
  assert(!own_message_lock); own_message_lock = true; return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t s) {
  assert(own_message_lock); own_message_lock = false; s->mutex.unlock();
}
static struct {
  template<class... A> void printf(const char*, A...) { assert(!own_message_lock); }
  void println(const char* s) {
    assert(!own_message_lock);
    if (strstr(s, "tx_lock_unavailable")) ++skip_logs;
  }
} Serial;
static struct {
  void print(const char* s) {
    if (fixed) assert(own_message_lock);
    {
      std::lock_guard<std::mutex> lock(wire_mutex); wire += s;
    }
    if (!coordinate || !strcmp(s, "\n")) return;
    std::unique_lock<std::mutex> lock(schedule_mutex);
    if (second_thread) {
      second_payload = true; schedule_cv.notify_all(); return;
    }
    first_payload = true; schedule_cv.notify_all();
    assert(schedule_cv.wait_for(lock, 2s, [] { return second_attempted || second_payload; }));
  }
  void flush() { if (fixed) assert(own_message_lock); }
} lcdSerial;
#define HALO_DEBUG_SENSITIVE 0
'''.replace('FIXED', str(locked).lower()).replace('EXPECT_JOINED', str(expect_joined).lower())
    cases = r'''
static const char* diag = R"({"ver":1,"type":"SENSE_DIAG","msg_id":19,"ts":5017})";
static const char* list = R"({"ver":1,"type":"UI_LIST","msg_id":20,"ts":5030,"items":[{"id":"fixture","text":"QA item"}]})";
static const char* query = R"({"ver":1,"type":"LCD_OTA_QUERY","msg_id":21,"ts":5040})";
static void clear_state() {
  wire.clear(); coordinate = false; first_payload = second_payload = second_attempted = false;
  g_lcd_ota_mode_unconfirmed = false;
  g_lcd_ota_proxy_owns_uart = g_spool_owns_uart = g_img_spool_tx_active = false;
  change_owner = 0; inject_timeout = false; uart_tx_count = 0;
}
static std::vector<std::string> received_types() {
  std::vector<std::string> result;
  size_t start = 0, end;
  while ((end = wire.find('\n', start)) != std::string::npos) {
    if (end != start) {
      JsonDocument parsed;
      assert(deserializeJson(parsed, wire.substr(start, end-start)) == DeserializationError::Ok);
      result.emplace_back(parsed["type"].as<const char*>());
    }
    start = end+1;
  }
  assert(start == wire.size()); return result;
}
static void collide(int ownership_change = 0) {
  clear_state(); coordinate = true; change_owner = ownership_change;
  std::thread a([] { uart_send_json(diag); });
  {
    std::unique_lock<std::mutex> lock(schedule_mutex);
    assert(schedule_cv.wait_for(lock, 2s, [] { return first_payload; }));
  }
  std::thread b([] { second_thread = true; uart_send_json(list); });
  a.join(); b.join(); coordinate = false;
}
int main() {
  if (fixed) {
    uart_send_json(diag); assert(wire.empty() && skip_logs == 1);
  }
  uart_json_tx_init(); uart_json_tx_init();
  if (fixed) assert(creates == 1);
  collide();
  if (expect_joined) {
    assert(wire == std::string(diag) + list + "\n\n");
    assert(received_types() == std::vector<std::string>{"SENSE_DIAG"});
    puts("PASS baseline reproduced joined diagnostic/list frames; real ArduinoJson silently loses UI_LIST");
    return 0;
  }
  assert(wire == std::string(diag) + "\n" + list + "\n");
  assert((received_types() == std::vector<std::string>{"SENSE_DIAG", "UI_LIST"}));
  for (int i = 0; i < 100; ++i) {
    collide(); assert(wire == std::string(diag) + "\n" + list + "\n");
    assert(uart_tx_count == 2);
  }
  puts("PASS 101 deterministic concurrent diagnostic/list sends preserve both complete frames");
  for (int owner = 1; owner <= 4; ++owner) {
    collide(owner); assert(wire == std::string(diag) + "\n");
    assert(uart_tx_count == 1);
  }
  clear_state();
  for (bool* owner : {&g_lcd_ota_proxy_owns_uart, &g_spool_owns_uart, &g_img_spool_tx_active}) {
    *owner = true; uart_send_json(list); uart_send_json(query, true); assert(wire.empty()); *owner = false;
  }
  g_lcd_ota_mode_unconfirmed = true;
  uart_send_json(list); uart_send_json(query); uart_send_json(list, true);
  uart_send_json("invalid JSON", true);
  std::string large_query = std::string(query) + std::string(256, ' ');
  uart_send_json(large_query.c_str(), true); assert(wire.empty());
  uart_send_json(query, true); assert(wire == std::string(query) + "\n");
  puts("PASS all binary/quarantine admissions, explicit mode probe bounds, and ownership recheck after contention");
  clear_state(); inject_timeout = true;
  const unsigned skipped = skip_logs;
  uart_send_json(list); assert(wire.empty() && uart_tx_count == 0 && skip_logs == skipped+1);
  inject_timeout = false; uart_send_json(list); assert(wire == std::string(list) + "\n");
  uart_send_json(nullptr); assert(uart_tx_count == 1);
  clear_state();
  std::string large = R"({"type":"UI_LIST","text":")" + std::string(12000, 'x') + R"("})";
  uart_send_json(large.c_str()); assert(wire == large + "\n");
  puts("PASS fail-closed uninitialized/timeout recovery, null admission, and unchanged12KB message bytes; USB logging outside lock");
}
'''
    return '\n'.join([prefix, state, definition(source, 'static void uart_send_json('), cases])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    parser.add_argument('--expect-joined', action='store_true')
    args = parser.parse_args()
    assert (ARDUINO_JSON / 'ArduinoJson.h').is_file(), 'Canonical ArduinoJson include directory required'
    with tempfile.TemporaryDirectory(prefix='halo-json-tx-') as directory:
        path = Path(directory); cpp = path / 'check.cpp'; binary = path / 'check'
        cpp.write_text(harness(args.source_root, args.expect_joined))
        subprocess.run([shutil.which('c++'), '-std=c++17', '-pthread', '-Wno-deprecated-declarations',
                        '-I', str(ARDUINO_JSON), str(cpp), '-o', str(binary)], check=True, timeout=30)
        subprocess.run([str(binary)], check=True, timeout=15)


if __name__ == '__main__':
    main()
