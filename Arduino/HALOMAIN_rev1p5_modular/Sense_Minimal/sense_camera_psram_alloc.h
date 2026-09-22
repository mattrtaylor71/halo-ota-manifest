#pragma once
#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <esp_attr.h>
#include <esp_cache.h>
#include <esp_heap_caps.h>
#include <sdkconfig.h>

// Included by the Sense sketch only. The canonical Sense linker wraps the SDK
// allocator; the camera owner enables this only during fallback camera init.
inline std::atomic<bool> g_camera_psram_dma_allocation{false};
static_assert(CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE == 64,
              "Camera allocation contract requires the pinned 64-byte cache line");

extern "C" void* __real_heap_caps_aligned_alloc(size_t alignment, size_t size,
                                               uint32_t caps);

extern "C" void* IRAM_ATTR __wrap_heap_caps_aligned_alloc(size_t alignment,
                                                        size_t size,
                                                        uint32_t caps) {
  // Preserve every ordinary allocation, including zero-size semantics. The
  // pinned camera archive requests precisely these caps and 16-byte alignment.
  if (!g_camera_psram_dma_allocation.load(std::memory_order_relaxed) ||
      alignment != 16 || caps != (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) || !size)
    return __real_heap_caps_aligned_alloc(alignment, size, caps);

  constexpr size_t line = 64;
  static_assert((line & (line - 1)) == 0, "Cache line must be a power of two");
  if (size > SIZE_MAX - (line - 1)) return nullptr;
  const size_t rounded = (size + line - 1) & ~(line - 1);
  void* allocation = __real_heap_caps_aligned_alloc(line, rounded, caps);
  if (!allocation) return nullptr;

  // cam_hal keeps a +16 byte framebuffer offset and rounds invalidation
  // outwards. Own every touched cache line, including the allocation's tail.
  // Flush and invalidate reused dirty lines before DMA can write this memory.
  if (esp_cache_msync(allocation, rounded,
                      ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE |
                      ESP_CACHE_MSYNC_FLAG_TYPE_DATA) != ESP_OK) {
    heap_caps_free(allocation);
    return nullptr;
  }
  return allocation;
}
