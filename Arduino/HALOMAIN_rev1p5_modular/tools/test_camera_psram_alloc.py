#!/usr/bin/env python3
"""Compile the production camera allocator wrapper against SDK boundary doubles."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HEADER = "Sense_Minimal/sense_camera_psram_alloc.h"
STUBS = {
    "esp_attr.h": "#pragma once\n#define IRAM_ATTR\n",
    "sdkconfig.h": "#pragma once\n#define CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE 64\n",
    "esp_heap_caps.h": """#pragma once
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_SPIRAM 0x400
#define MALLOC_CAP_8BIT 0x004
extern "C" void heap_caps_free(void*);
""",
    "esp_cache.h": """#pragma once
#include <stddef.h>
#define ESP_OK 0
#define ESP_CACHE_MSYNC_FLAG_INVALIDATE 1
#define ESP_CACHE_MSYNC_FLAG_DIR_C2M 4
#define ESP_CACHE_MSYNC_FLAG_TYPE_DATA 16
extern "C" int esp_cache_msync(void*, size_t, int);
""",
}
NATIVE = r'''
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include "wrapper.h"
static unsigned checks = 0;
static void check(bool condition) {
  ++checks;
  if (!condition) { std::fprintf(stderr, "FAIL check %u\n", checks); std::exit(1); }
}
struct Call { unsigned kind; size_t alignment, size; uint32_t caps; void* ptr; int flags; };
static Call calls[8];
static unsigned count, residue;
static bool alloc_fail;
static int sync_result;
alignas(64) static unsigned char arena[264000];
static void reset() { count = residue = 0; alloc_fail = false; sync_result = 0; }
extern "C" void* __real_heap_caps_aligned_alloc(size_t align, size_t size, uint32_t caps) {
  void* p = alloc_fail || !size || size > sizeof(arena) - 64 ? nullptr :
            static_cast<void*>(arena + (align == 16 ? residue : 0));
  calls[count++] = {1, align, size, caps, p, 0}; return p;
}
extern "C" int esp_cache_msync(void* p, size_t size, int flags) {
  calls[count++] = {2, 0, size, 0, p, flags}; return sync_result;
}
extern "C" void heap_caps_free(void* p) { calls[count++] = {3, 0, 0, 0, p, 0}; }
static bool owned(uintptr_t base, size_t allocation_size, uintptr_t start, size_t len) {
  const uintptr_t first = start & ~uintptr_t(63);
  const uintptr_t end = (start + len + 63) & ~uintptr_t(63);
  return first >= base && end <= base + allocation_size;
}
static void geometry(uintptr_t base, size_t allocation_size, size_t payload) {
  const uintptr_t fb = base + 16 - (base & 15);
  check(owned(base, allocation_size, fb, payload));
  check(owned(base, allocation_size, fb, payload < 32 ? payload : 32));
  const size_t tail = payload < 1025 ? payload : 1025;
  check(owned(base, allocation_size, fb + payload - tail, tail));
}
int main() {
  constexpr uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  constexpr size_t maximum = std::numeric_limits<size_t>::max();
  // Outside the camera-init scope, arguments and return values are untouched.
  for (size_t align : {size_t(0), size_t(16), size_t(32), size_t(64)}) {
    for (size_t size : {size_t(0), size_t(17), maximum}) {
      reset(); g_camera_psram_dma_allocation.store(false);
      void* p = __wrap_heap_caps_aligned_alloc(align, size, caps);
      check(count == 1 && calls[0].alignment == align && calls[0].size == size);
      check(calls[0].caps == caps && p == calls[0].ptr);
    }
  }
  g_camera_psram_dma_allocation.store(true);
  // Every nonmatching alignment/capability remains a byte-for-byte forward.
  for (size_t align : {size_t(0), size_t(16), size_t(32), size_t(64)}) {
    for (uint32_t cap : {uint32_t(0), caps, caps | 8u, uint32_t(MALLOC_CAP_8BIT)}) {
      if (align == 16 && cap == caps) continue;
      reset(); void* p = __wrap_heap_caps_aligned_alloc(align, 17, cap);
      check(count == 1 && calls[0].alignment == align && calls[0].size == 17);
      check(calls[0].caps == cap && p == calls[0].ptr);
    }
  }
  reset(); check(__wrap_heap_caps_aligned_alloc(16, 0, caps) == nullptr);
  check(count == 1 && calls[0].alignment == 16 && calls[0].size == 0);
  // Exercise both sides of each rounding boundary, including the real SXGA
  // recv_size + DMA half-buffer + driver's 16-byte framebuffer offset.
  const size_t frame_size = 262144 + 1024 + 16;
  for (unsigned raw_residue : {0u, 16u, 32u, 48u}) {
    for (size_t size = 17; size <= 512; ++size) {
      reset(); residue = raw_residue;
      void* p = __wrap_heap_caps_aligned_alloc(16, size, caps);
      const size_t rounded = (size + 63) & ~size_t(63);
      check(count == 2 && calls[0].kind == 1 && calls[1].kind == 2);
      check(calls[0].alignment == 64 && calls[0].size == rounded && calls[0].caps == caps);
      check(p == calls[0].ptr && (uintptr_t(p) & 63) == 0);
      check(calls[1].ptr == p && calls[1].size == rounded && calls[1].flags == 21);
      geometry(uintptr_t(p), rounded, size - 16);
    }
    reset(); residue = raw_residue;
    void* p = __wrap_heap_caps_aligned_alloc(16, frame_size, caps);
    check(count == 2 && calls[0].alignment == 64 && calls[0].size == 263232);
    geometry(uintptr_t(p), calls[0].size, frame_size - 16);
  }
  // Negative controls: legacy 16-aligned layouts lose ownership at either the
  // prefix or tail. Check every possible 16-byte residue in a 64-byte line.
  unsigned unsafe_prefix = 0, unsafe_full = 0;
  for (unsigned old_residue : {0u, 16u, 32u, 48u}) {
    const uintptr_t base = uintptr_t(arena) + old_residue;
    if (!owned(base, frame_size, base + 16, 32)) ++unsafe_prefix;
    if (!owned(base, frame_size, base + 16, frame_size - 16)) ++unsafe_full;
  }
  check(unsafe_prefix == 2 && unsafe_full == 3);
  // Allocation failure must not synchronize or free a null pointer.
  reset(); alloc_fail = true;
  check(__wrap_heap_caps_aligned_alloc(16, frame_size, caps) == nullptr && count == 1);
  // Sync failure frees the exact base, only after alloc -> sync; no unusable
  // pointer escapes to the driver and its ordinary free remains compatible.
  reset(); sync_result = -1;
  check(__wrap_heap_caps_aligned_alloc(16, frame_size, caps) == nullptr);
  check(count == 3 && calls[0].kind == 1 && calls[1].kind == 2 && calls[2].kind == 3);
  check(calls[2].ptr == calls[0].ptr && calls[1].ptr == calls[0].ptr);
  // Overflow is rejected before allocation; largest already aligned value is
  // forwarded without wraparound (the allocator double rejects its huge size).
  for (size_t delta = 0; delta < 63; ++delta) {
    reset(); check(__wrap_heap_caps_aligned_alloc(16, maximum - delta, caps) == nullptr);
    check(count == 0);
  }
  reset(); check(__wrap_heap_caps_aligned_alloc(16, maximum - 63, caps) == nullptr);
  check(count == 1 && calls[0].alignment == 64 && calls[0].size == maximum - 63);
  // Scope closes cleanly after fallback, including matching camera arguments.
  reset(); g_camera_psram_dma_allocation.store(false);
  __wrap_heap_caps_aligned_alloc(16, frame_size, caps);
  check(count == 1 && calls[0].alignment == 16 && calls[0].size == frame_size);
  std::printf("PASS camera PSRAM allocator: %u checks; all four legacy base residues; SDK boundary doubles\n", checks);
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=ROOT)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    source = (args.source_root.resolve() / HEADER).read_text()
    compiler = shutil.which("clang++") or shutil.which("g++")
    assert compiler, "Native C++ compiler required"
    sdk = Path.home() / "Library/Arduino15/packages/esp32/tools/esp32s3-libs/3.3.8"
    target = Path.home() / "Library/Arduino15/packages/esp32/tools/esp-x32/2601/bin/xtensa-esp32s3-elf-g++"
    flags = sdk / "flags/cpp_flags"
    assert target.is_file() and flags.is_file(), "Pinned S3 compiler and actual SDK flags required"
    assert "-mdisable-hardware-atomics" in flags.read_text()
    variants = {
        "production": source,
        "alignment_negative": source.replace("__real_heap_caps_aligned_alloc(line, rounded, caps)",
                                               "__real_heap_caps_aligned_alloc(alignment, rounded, caps)"),
        "rounding_negative": source.replace("const size_t rounded = (size + line - 1) & ~(line - 1);",
                                              "const size_t rounded = size;"),
        "sync_flags_negative": source.replace("ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE |",
                                                "ESP_CACHE_MSYNC_FLAG_DIR_C2M |"),
        "failure_cleanup_negative": source.replace("heap_caps_free(allocation);", "(void)allocation;"),
    }
    results = {}
    with tempfile.TemporaryDirectory(prefix="halo-camera-psram-alloc-") as directory:
        tmp = Path(directory)
        for name, content in STUBS.items():
            (tmp / name).write_text(content)
        (tmp / "test.cpp").write_text("#include <initializer_list>\n" + NATIVE)
        for name, content in variants.items():
            assert name == "production" or content != source, name + " failed to mutate header"
            (tmp / "wrapper.h").write_text(content)
            executable = tmp / name
            build = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                    "-fsanitize=address,undefined", "-I", str(tmp), str(tmp / "test.cpp"),
                                    "-o", str(executable)], text=True, capture_output=True, timeout=40)
            assert build.returncode == 0, build.stdout + build.stderr
            run = subprocess.run([str(executable)], text=True, capture_output=True, timeout=30)
            results[name] = {"exit_code": run.returncode, "output": run.stdout + run.stderr}
        # Compile the actual header with production's SDK flags (especially
        # -mdisable-hardware-atomics), not a flag-free synthetic std::atomic
        # primitive. Boundary declarations are doubled; exact ELF placement
        # and resolved callees still require the canonical artifact review.
        (tmp / "wrapper.h").write_text(source)
        (tmp / "esp_attr.h").write_text('#pragma once\n#define IRAM_ATTR __attribute__((section(".iram1.alloc_test")))\n')
        probe = '#include "wrapper.h"\nextern "C" void probe_scope_store(bool value) { g_camera_psram_dma_allocation.store(value); }\n'
        assembly = tmp / "target.s"
        subprocess.run([str(target), "@" + str(flags), "-Os", "-S", "-x", "c++",
                        "-I", str(tmp), "-o", str(assembly), "-"], input=probe,
                       text=True, capture_output=True, check=True, timeout=30)
        target_assembly = assembly.read_text()
        wrapper_asm = target_assembly.split("__wrap_heap_caps_aligned_alloc:", 1)[1].split(
            "\t.size\t__wrap_heap_caps_aligned_alloc", 1)[0]
        store_asm = target_assembly.split("probe_scope_store:", 1)[1].split(
            "\t.size\tprobe_scope_store", 1)[0]
        assert re.search(r"\bl8ui\b", wrapper_asm), "Scope load must be an inline byte load"
        assert re.search(r"\bs8i\b", store_asm), "Scope store must be an inline byte store"
        assert not re.search(r"\bcall\w*\b", store_asm), "Scope store contains an out-of-line call"
        assert "__atomic_" not in wrapper_asm + store_asm
        assert "CameraPsramAllocationFlag4load" not in wrapper_asm
        assert "_ZNKSt6atomicIbE4load" not in wrapper_asm, "Flash std::atomic load returned"
        literals = set(re.findall(r"^\s*\.word\s+([A-Za-z_]\w*)\s*$", target_assembly, re.M))
        assert literals <= {"g_camera_psram_dma_allocation", "__real_heap_caps_aligned_alloc",
                            "esp_cache_msync", "heap_caps_free"}, literals
    passed = results["production"]["exit_code"] == 0 and all(
        data["exit_code"] != 0 for name, data in results.items() if name != "production")
    result = {
        "status": "PASS" if passed else "FAIL",
        "scope": "Actual header with native ASan/UBSan and SDK boundary doubles; allocation forwarding, "
                 "overflow, cache line ownership, sync ordering/flags, cleanup and four detected mutations. "
                 "Actual header target assembly under SDK cpp_flags verifies inline byte flag load/store. "
                 "No physical DMA/cache, final target link or camera-init lifecycle claim.",
        "source_sha256": hashlib.sha256(source.encode()).hexdigest(),
        "target_cpp_flags": {"path": str(flags), "sha256": hashlib.sha256(flags.read_bytes()).hexdigest()},
        "target_assembly_sha256": hashlib.sha256(target_assembly.encode()).hexdigest(),
        "variants": results,
    }
    if args.out:
        args.out.mkdir(parents=True, exist_ok=True)
        (args.out / "RESULT.json").write_text(json.dumps(result, indent=2) + "\n")
        (args.out / "target.s").write_text(target_assembly)
    print(results["production"]["output"], end="")
    print("PASS four mutation controls" if passed else json.dumps(result, indent=2))
    print("PASS actual SDK flags: inline target byte load/store; no atomic helper")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
