#!/usr/bin/env python3
"""Execute actual main-loop/refresh render boundaries with fake LVGL; no hardware."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BASELINE = '730d3c952de223c000e40a6e6a0c3a9e73c3f1a7'
REFRESH_BASELINE = 'df3f8a1a924083d38b6f0e8e1c9b894c1826b8d5'
LCD = 'LCD_Minimal/LCD_Minimal.ino'
UI = 'LCD_Minimal/lcd_ui_task.h'
UART_RX = 'LCD_Minimal/lcd_uart_rx.h'


def definition(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def old_source(path, revision=BASELINE):
    return subprocess.check_output([
        'git', 'show', revision + ':Arduino/HALOMAIN_rev1p5_modular/' + path
    ], cwd=ROOT, text=True)


def run(baseline=False):
    source = old_source(LCD) if baseline else (ROOT / LCD).read_text()
    loop = source.split('void loop() {', 1)[1]
    ui = old_source(UI) if baseline else (ROOT / UI).read_text()
    old_ui = old_source(UI)
    overlay_signature = ('if (!g_lcd_ota_binary_mode || !s_ota_overlay_pushed)' if baseline
                         else 'if (!g_lcd_ota_uart_receiving && !g_lcd_ota_binary_mode)')
    overlay = definition(ui, overlay_signature)
    # Preserve exact object creation and suspension. The deliberate static
    # installation phase is covered by test_lcd_ota_install_frame and native
    # LVGL pixel QA; it does not change these main-loop exclusion predicates.
    visual_start = '        // Create overlay on first entry'
    entry = definition(ui, 'if (!ota_overlay)')
    old_entry = definition(old_ui, 'if (!ota_overlay)')
    assert entry[entry.index(visual_start):] == old_entry[old_entry.index(visual_start):]
    motion_stop = '        halo_ui_motion_stop(lv_scr_act());'
    suspension = entry[entry.index(motion_stop):entry.index(visual_start)]
    expected_suspension = motion_stop + '\n' + (
        '        if (provision_ui_spinner) {\n'
        '          provision_ui_deferred_view = (int)provision_ui_view;\n'
        '          provision_ui_stop_spinner();\n'
        '        }\n' if baseline else
        '        provision_ui_suspend_for_ota();\n')
    assert suspension == expected_suspension
    direct_calls = loop.count('lv_timer_handler();')
    wrapped_calls = loop.count('lcd_main_lvgl_service();')
    assert (direct_calls, wrapped_calls) == ((15, 0) if baseline else (0, 15))
    # Execute the exact always-reached render block, including mutex release.
    render = definition(loop[loop.index('loop_continue:'):], 'if (lvgl_locked)')
    helper = '' if baseline else definition(source, 'static void lcd_main_lvgl_service()')
    harness = r'''
#include <atomic>
#include <cassert>
#include <cstdio>
static std::atomic<bool> g_lcd_ota_uart_receiving{false};
static bool g_lcd_ota_binary_mode = false;
INSTALL_FRAME_BOUNDARY
BASELINE_STATE
static unsigned renders = 0, unlocks = 0;
static void lv_timer_handler() { ++renders; }
static void example_lvgl_unlock() { ++unlocks; }
HELPER
static void main_render(bool lvgl_locked) { RENDER }
static void overlay_render() { OVERLAY }
int main() {
  constexpr bool baseline = BASELINE_MODE;
  main_render(true); assert(renders == 1 && unlocks == 1);
  overlay_render(); assert(renders == 2); // ordinary preflight/first overlay
  renders = 1;
  // Flash begins before binary mode; finalization also retains receiving=true.
  g_lcd_ota_uart_receiving = true;
  main_render(true);
  if (baseline) {
    assert(renders == 2 && unlocks == 2);
    g_lcd_ota_binary_mode = true; main_render(true);
    assert(renders == 3 && unlocks == 3);
    s_ota_overlay_pushed = false; overlay_render(); assert(renders == 4);
    puts("PASS baseline reproduces main-loop flash/binary render and delayed UI first-frame bypass");
    return 0;
  }
  assert(renders == 1 && unlocks == 2);
  overlay_render(); assert(renders == 1); // no erase/finalization flush
  g_lcd_ota_binary_mode = true; main_render(true);
  assert(renders == 1 && unlocks == 3);
  // Independently guard the binary flag, including a transition boundary.
  g_lcd_ota_uart_receiving = false; main_render(true);
  assert(renders == 1 && unlocks == 4);
  // A delayed first UI frame cannot bypass active transfer suppression.
  overlay_render(); assert(renders == 1);
  overlay_render(); assert(renders == 1);
  main_render(true); assert(renders == 1 && unlocks == 5);
  // Render resumes immediately after successful or failed transfer cleanup.
  g_lcd_ota_binary_mode = false;
  main_render(true); assert(renders == 2 && unlocks == 6);
  overlay_render(); assert(renders == 3);
  main_render(false); assert(renders == 3 && unlocks == 6);
  // A new transfer suppresses again; cleanup resumes without a sticky latch.
  g_lcd_ota_uart_receiving = true;
  main_render(true); overlay_render(); assert(renders == 3 && unlocks == 7);
  g_lcd_ota_uart_receiving = false;
  main_render(true); overlay_render(); assert(renders == 5 && unlocks == 8);
  puts("PASS current: all 15 loop sites guarded; active flash/binary suppression, mutex release, preflight overlay, delayed-first suppression and post-OTA resume");
}
'''.replace('HELPER', helper).replace('RENDER', render).replace('OVERLAY', overlay).replace('BASELINE_MODE', str(baseline).lower()).replace('BASELINE_STATE', 'static bool s_ota_overlay_pushed = false;')
    harness = harness.replace('INSTALL_FRAME_BOUNDARY', '' if baseline else '''
static bool install_frame=false;
static unsigned install_token=0;
static void* ota_overlay=nullptr;
static void lcd_ota_install_frame_submit(unsigned,void*){assert(false);}
''')
    with tempfile.TemporaryDirectory(prefix='halo-lcd-render-') as temp:
        cpp = Path(temp) / 'test.cpp'
        binary = Path(temp) / 'test'
        cpp.write_text(harness)
        subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


def run_refresh(baseline=False):
    def read(path):
        return old_source(path, REFRESH_BASELINE) if baseline else (ROOT / path).read_text()

    source, uart, ui = read(LCD), read(UART_RX), read(UI)
    # Verify both real UART entry points still reach the extracted state helper.
    # Rendering belongs to the existing UI-owner poll, not the UART callback.
    for message in ('PONG', 'SYNC_ACK'):
        branch = definition(uart, 'if (strcmp(type, "' + message + '") == 0)')
        assert 'refresh_sm_on_awake_proof("' + message + '");' in branch
    awake = definition(source, 'static void refresh_sm_on_awake_proof(const char* source)')
    assert 'if (refresh_state != REFRESH_WAKE_PENDING)' in awake
    assert 'refresh_sm_set_state(REFRESH_INFLIGHT, "awake_proof");' in awake
    poll = definition(ui, 'if (ui_screen_state == SCREEN_SHOPPING_LIST)')
    assert 'shopping_list_refresh_indicator_sync(false);' in poll
    assert ui.index('if (!example_lvgl_lock(50))') < ui.index(poll)
    helper = definition(source, 'static void refresh_sm_set_state(RefreshState state, const char* reason)')
    harness = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
enum RefreshState { REFRESH_IDLE, REFRESH_WAKE_PENDING, REFRESH_INFLIGHT,
                    REFRESH_COMPLETE, REFRESH_FAILED };
static RefreshState refresh_state = REFRESH_WAKE_PENDING;
static unsigned long now_ms = 100;
static unsigned long lcd_refresh_start_ms = 1, refresh_last_proof_ms = 2;
static unsigned long refresh_last_ui_pol_ms = 3, refresh_last_pulse_ms = 4;
static unsigned long refresh_last_wake_send_ms = 5, refresh_done_ms = 6;
static unsigned long last_proof_of_life_ms = 7;
static uint8_t refresh_pulse_count = 2;
static bool refresh_rx_stale_suppressed = true, refresh_wake_sent = true;
static bool is_glowing_animation = false;
static unsigned render_calls = 0, glow_calls = 0, log_calls = 0;
static const char* last_reason = nullptr;
static unsigned long last_log_ms = 0;
static unsigned long millis() { return now_ms; }
// Keep the old no-op glow semantics: it never sets is_glowing_animation.
// Counters detect the real pre-fix render without using or allocating LVGL.
[[maybe_unused]] static void start_glowing_animation(const char*) { ++glow_calls; }
[[maybe_unused]] static void lv_timer_handler() { ++render_calls; }
static void refresh_sm_log(const char* reason, unsigned long at) {
  ++log_calls; last_reason = reason; last_log_ms = at;
}
HELPER
int main() {
  // The observed PONG path enters from WAKE_PENDING while glow is off.
  refresh_sm_set_state(REFRESH_INFLIGHT, "awake_proof");
  assert(refresh_state == REFRESH_INFLIGHT);
  assert(lcd_refresh_start_ms == 100 && refresh_last_proof_ms == 100);
  assert(refresh_last_ui_pol_ms == 100 && last_proof_of_life_ms == 100);
  assert(refresh_last_pulse_ms == 0 && refresh_pulse_count == 0);
  assert(!refresh_rx_stale_suppressed && !refresh_wake_sent);
  assert(refresh_last_wake_send_ms == 0 && refresh_done_ms == 0);
  assert(log_calls == 1 && last_log_ms == 100 && !strcmp(last_reason, "awake_proof"));
  // Completion/failure set only the completion time; idle retains it.
  now_ms = 200; refresh_sm_set_state(REFRESH_COMPLETE, "list_received");
  assert(refresh_state == REFRESH_COMPLETE && refresh_done_ms == 200);
  now_ms = 300; refresh_sm_set_state(REFRESH_FAILED, "timeout");
  assert(refresh_state == REFRESH_FAILED && refresh_done_ms == 300);
  now_ms = 400; refresh_sm_set_state(REFRESH_IDLE, "complete_shown");
  assert(refresh_state == REFRESH_IDLE && refresh_done_ms == 300);
  assert(lcd_refresh_start_ms == 100 && refresh_last_proof_ms == 100);
  assert(refresh_last_ui_pol_ms == 100 && last_proof_of_life_ms == 100);
  assert(refresh_last_pulse_ms == 0 && refresh_pulse_count == 0);
  assert(!refresh_rx_stale_suppressed && !refresh_wake_sent);
  assert(refresh_last_wake_send_ms == 0);
  assert(log_calls == 4 && last_log_ms == 400 && !strcmp(last_reason, "complete_shown"));
  // Another refresh resets the timing even if the old animation flag is set.
  is_glowing_animation = true;
  refresh_last_pulse_ms = 9; refresh_pulse_count = 3;
  refresh_rx_stale_suppressed = true; refresh_wake_sent = true;
  refresh_last_wake_send_ms = 10;
  now_ms = 500; refresh_sm_set_state(REFRESH_INFLIGHT, "awake_proof");
  assert(refresh_state == REFRESH_INFLIGHT && refresh_done_ms == 0);
  assert(lcd_refresh_start_ms == 500 && refresh_last_proof_ms == 500);
  assert(refresh_last_ui_pol_ms == 500 && last_proof_of_life_ms == 500);
  assert(refresh_last_pulse_ms == 0 && refresh_pulse_count == 0);
  assert(!refresh_rx_stale_suppressed && !refresh_wake_sent);
  assert(refresh_last_wake_send_ms == 0 && log_calls == 5 && last_log_ms == 500);
  if (render_calls || glow_calls) {
    fprintf(stderr, "FAIL refresh UART boundary: render_calls=%u glow_calls=%u; expected no graphics work\n",
            render_calls, glow_calls);
    return 1;
  }
  puts("PASS refresh: real PONG/SYNC_ACK state path does no graphics work; timing resets, COMPLETE/FAILED/IDLE and UI-owner poll retained");
}
'''.replace('HELPER', helper)
    with tempfile.TemporaryDirectory(prefix='halo-lcd-refresh-') as temp:
        cpp, binary = Path(temp) / 'test.cpp', Path(temp) / 'test'
        cpp.write_text(harness)
        subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--baseline', action='store_true', help='Reproduce the pinned pre-fix render bypass')
    mode.add_argument('--refresh-baseline', action='store_true',
                      help='Test df3f8a1 against the no-UART-graphics requirement (must fail)')
    args = parser.parse_args()
    if args.refresh_baseline:
        run_refresh(baseline=True)
    else:
        run(args.baseline)
        if not args.baseline:
            run_refresh()
