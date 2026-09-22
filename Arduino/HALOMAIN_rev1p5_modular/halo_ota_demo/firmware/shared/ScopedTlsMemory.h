#pragma once
#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Install calloc/free once as the mbedTLS platform hooks. Only a lexical scope
// on its owning task prefers PSRAM; unrelated TLS users retain ordinary calloc.
// This does not intercept the SDK's explicit hardware-DMA allocations.
namespace halo_tls_memory {
struct Counters {
  uint32_t external_calls, external_requested_bytes;
  uint32_t small_external_calls, small_external_requested_bytes;
  uint32_t external_failures, default_fallbacks;
};
namespace detail {
inline std::atomic<bool> enabled{false};
inline std::atomic<TaskHandle_t> owner{nullptr};
// Only the exclusive owner task accesses depth. Release/acquire transfers it
// to the next owner; no lock is held across any allocator call.
inline uint32_t depth = 0;
inline std::atomic<uint32_t> external_calls{0}, external_requested_bytes{0};
inline std::atomic<uint32_t> small_external_calls{0}, small_external_requested_bytes{0};
inline std::atomic<uint32_t> external_failures{0}, default_fallbacks{0};
inline void add(std::atomic<uint32_t>& counter, uint32_t amount) {
  // One owner writes; readers may take a nontransactional cumulative snapshot.
  // These counters wrap modulo 2^32. They do not represent live or peak memory.
  counter.store(counter.load(std::memory_order_relaxed) + amount,
                std::memory_order_relaxed);
}
} // namespace detail

// Boot-only, before clients/tasks can enter scopes; never change global hooks
// or this enablement while a connection is active.
inline void initialize(bool psram_ready) {
  detail::enabled.store(psram_ready, std::memory_order_release);
}

class Scope {
  TaskHandle_t task_ = nullptr;
public:
  Scope() {
    if (!detail::enabled.load(std::memory_order_acquire)) return;
    const TaskHandle_t me = xTaskGetCurrentTaskHandle();
    if (!me) return;
    TaskHandle_t expected = nullptr;
    if (detail::owner.load(std::memory_order_acquire) == me) {
      if (detail::depth == UINT32_MAX) return;
      ++detail::depth;
    } else {
      // One bounded claim, without waiting/retrying on another task's scope.
      if (!detail::owner.compare_exchange_strong(expected, me,
              std::memory_order_acq_rel, std::memory_order_relaxed)) return;
      detail::depth = 1;
    }
    task_ = me;
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
  Scope(Scope&&) = delete;
  Scope& operator=(Scope&&) = delete;
  ~Scope() {
    // Scopes must be destroyed on their original task. A refused foreign
    // scope never clears another task's ownership.
    if (!task_ || xTaskGetCurrentTaskHandle() != task_ ||
        detail::owner.load(std::memory_order_acquire) != task_) return;
    if (--detail::depth == 0)
      detail::owner.store(nullptr, std::memory_order_release);
  }
  bool active() const { return task_ != nullptr; }
};

inline void* calloc(size_t count, size_t size) {
  if (size && count > SIZE_MAX / size) return nullptr;
  const size_t bytes = count * size;
  // Preserve the platform's ordinary zero-size convention and every call
  // outside a matching owner; PSRAM availability is only a preference.
  if (!bytes || !detail::enabled.load(std::memory_order_acquire))
    return ::calloc(count, size);
  const TaskHandle_t me = xTaskGetCurrentTaskHandle();
  if (!me || detail::owner.load(std::memory_order_acquire) != me)
    return ::calloc(count, size);
  detail::add(detail::external_calls, 1);
  detail::add(detail::external_requested_bytes, uint32_t(bytes));
  if (bytes <= 4096) {
    detail::add(detail::small_external_calls, 1);
    detail::add(detail::small_external_requested_bytes, uint32_t(bytes));
  }
  void* result = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (result) return result;
  detail::add(detail::external_failures, 1);
  detail::add(detail::default_fallbacks, 1);
  return ::calloc(count, size);
}

// Both ordinary calloc and caps calloc belong to the ESP-IDF unified heap.
// Freeing is independent of scope/task/lifetime; mbedTLS owns zeroization.
inline void free(void* pointer) { heap_caps_free(pointer); }

inline Counters snapshot() {
  return {detail::external_calls.load(std::memory_order_relaxed),
          detail::external_requested_bytes.load(std::memory_order_relaxed),
          detail::small_external_calls.load(std::memory_order_relaxed),
          detail::small_external_requested_bytes.load(std::memory_order_relaxed),
          detail::external_failures.load(std::memory_order_relaxed),
          detail::default_fallbacks.load(std::memory_order_relaxed)};
}
} // namespace halo_tls_memory
