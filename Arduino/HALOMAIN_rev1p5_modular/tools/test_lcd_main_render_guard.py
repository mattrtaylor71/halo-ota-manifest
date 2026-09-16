#!/usr/bin/env python3
"""Execute actual main-loop render/overlay call sites with fake LVGL; no hardware."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BASELINE = '730d3c952de223c000e40a6e6a0c3a9e73c3f1a7'
LCD = 'LCD_Minimal/LCD_Minimal.ino'
UI = 'LCD_Minimal/lcd_ui_task.h'


def definition(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def old_source(path):
    return subprocess.check_output([
        'git', 'show', BASELINE + ':Arduino/HALOMAIN_rev1p5_modular/' + path
    ], cwd=ROOT, text=True)


def run(baseline=False):
    source = old_source(LCD) if baseline else (ROOT / LCD).read_text()
    loop = source.split('void loop() {', 1)[1]
    ui = old_source(UI) if baseline else (ROOT / UI).read_text()
    old_ui = old_source(UI)
    overlay_signature = ('if (!g_lcd_ota_binary_mode || !s_ota_overlay_pushed)' if baseline
                         else 'if (!g_lcd_ota_uart_receiving && !g_lcd_ota_binary_mode)')
    overlay = definition(ui, overlay_signature)
    # Provisioning owns its suspension implementation; keep that scoped change
    # separate from the exact OTA creation/phase/label/progress preservation.
    visual_start = '        // Create overlay on first entry'
    visual_end = '      if (ota_overlay) {\n        lv_obj_move_foreground(ota_overlay);\n      }'
    assert ui[ui.index(visual_start):ui.index(visual_end)] == old_ui[old_ui.index(visual_start):old_ui.index(visual_end)]
    entry = definition(ui, 'if (!ota_overlay)')
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
    with tempfile.TemporaryDirectory(prefix='halo-lcd-render-') as temp:
        cpp = Path(temp) / 'test.cpp'
        binary = Path(temp) / 'test'
        cpp.write_text(harness)
        subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', action='store_true', help='Reproduce the pinned pre-fix render bypass')
    args = parser.parse_args()
    run(args.baseline)
