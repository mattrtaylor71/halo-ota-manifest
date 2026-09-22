#pragma once
#include <atomic>
#include <stdint.h>
#include <stddef.h>

// Evidence only. No allocation, logging, heap query or waiting in the SDK
// failure hook. A contended hook drops its observation instead of blocking.
// The owner prints bounded snapshots only after its TLS client is stopped or
// destroyed. Failures cover the process during that owner interval, not a task.
#if defined(ARDUINO_ARCH_ESP32) && !defined(HALO_MEMORY_DIAGNOSTICS_TEST)
#include <esp_heap_caps.h>
#endif
namespace sense_memory {
enum Phase : uint32_t { Idle, VoiceAttempt, Connect, Write, Read, ImagePutAttempt };
enum Point : uint8_t { BeforeClient, BeforeConnect, AfterConnect, FirstWrite,
                       FirstRead, AfterClient, BeforeAttempt, PointCount };
#if defined(ARDUINO_ARCH_ESP32) || defined(HALO_MEMORY_DIAGNOSTICS_TEST)
// One 32-bit compare/exchange, never atomic_flag's byte-CAS retry loop.
// The standalone probe verifies the primitive; production flags can call the
// SDK helper. Exact-ELF review must verify its internal-memory dispatch too.
struct TryGate {
  std::atomic<uint32_t> value{0};
  bool test_and_set(std::memory_order order = std::memory_order_acquire) {
    uint32_t expected = 0;
    return !value.compare_exchange_strong(expected, 1, order, std::memory_order_relaxed);
  }
  void clear(std::memory_order order = std::memory_order_release) { value.store(0, order); }
};
struct Failure { uint32_t sequence, bytes, caps, phase, function_hash; };
struct Failures {
  TryGate guard;
  // Sticky loss marker: a single store avoids a fetch_add CAS retry loop.
  // Once contention occurs, reports remain conservatively incomplete until boot.
  std::atomic<uint32_t> phase{Idle}, dropped{0};
  uint32_t sequence = 0;
  Failure ring[4]{};
  static uint32_t function_hash(const char* name) {
    uint32_t h = 2166136261u;
    for (unsigned i = 0; name && i < 48 && name[i]; ++i)
      h = (h ^ uint8_t(name[i])) * 16777619u;
    return h;
  }
  void record(size_t bytes, uint32_t caps, const char* name) {
    if (guard.test_and_set(std::memory_order_acquire)) {
      dropped.store(1, std::memory_order_relaxed); return;
    }
    const uint32_t seq = ++sequence;
    ring[(seq - 1u) % 4u] = {seq, uint32_t(bytes), caps,
      phase.load(std::memory_order_relaxed), function_hash(name)};
    guard.clear(std::memory_order_release);
  }
  bool copy(Failure* out, uint32_t& seq, uint32_t& lost) {
    if (guard.test_and_set(std::memory_order_acquire)) return false;
    for (unsigned i = 0; i < 4; ++i) out[i] = ring[i];
    seq = sequence; lost = dropped.load(std::memory_order_relaxed);
    guard.clear(std::memory_order_release); return true;
  }
};
inline Failures failures;
inline void failed_alloc(size_t bytes, uint32_t caps, const char* function_name) {
  failures.record(bytes, caps, function_name);
}
inline bool registered = false;
inline void begin() {
  if (registered) return;
  const auto rc = heap_caps_register_failed_alloc_callback(failed_alloc);
  registered = rc == ESP_OK;
  Serial.printf("[MEM_DIAG] hook_registered=%u rc=%d\n", registered ? 1u : 0u, int(rc));
}
struct Trace;
// Accessed only by the existing single HTTP/upload owner; the failure hook
// never touches this stack pointer. It reads only the atomic scalar phase.
inline Trace* current = nullptr;
struct Trace {
  uint32_t heap[PointCount][3]{};
  uint32_t job, attempt = 0, start_sequence = 0, start_dropped = 0, sent = 0;
  const bool image;
  uint8_t mask = 0;
  int result = 0;
  bool owns = false, initial_valid = false;
  Trace(uint32_t j, bool is_image) : job(j), image(is_image) {}
  Trace(const Trace&) = delete;
  Trace& operator=(const Trace&) = delete;
  bool start(uint32_t a) {
    // The caller must finish only after stop(), then start the next attempt.
    // Never displace a live owner or implicitly print while its TLS is alive.
    if (current || owns) return false;
    attempt = a; mask = 0; result = 0; sent = 0;
    current = this; owns = true;
    Failure ignored[4]; initial_valid = failures.copy(ignored, start_sequence, start_dropped);
    failures.phase.store(image ? ImagePutAttempt : VoiceAttempt, std::memory_order_relaxed);
    // PUT reuses one client object: point 6 is after stop, before configuring
    // this attempt. Voice point 0 remains before client construction.
    sample(image ? BeforeAttempt : BeforeClient);
    return true;
  }
  void sample(Point p) {
    if (p >= PointCount || (mask & (1u << p))) return;
    heap[p][0] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    heap[p][1] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    heap[p][2] = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    mask |= 1u << p;
  }
  void response(int code) { result = code; }
  void finish() {
    if (!owns) return;
    sample(AfterClient);
    failures.phase.store(Idle, std::memory_order_relaxed); current = nullptr; owns = false;
    Failure observed[4]; uint32_t last = 0, dropped = 0;
    const bool valid = failures.copy(observed, last, dropped) && initial_valid;
    const char* label = image ? "IMAGE_PUT_MEM" : "VOICE_MEM";
    Serial.printf("[%s] job=%lu attempt=%lu http=%d tls_bytes=%lu mask=%u hook=%u failures=%lu loss_seen=%lu complete=%u\n",
      label, (unsigned long)job, (unsigned long)attempt, result, (unsigned long)sent,
      unsigned(mask), registered ? 1u : 0u,
      (unsigned long)(valid ? uint32_t(last - start_sequence) : 0),
      (unsigned long)(valid ? dropped : 1),
      registered && valid && uint32_t(last - start_sequence) <= 4u && dropped == 0 ? 1u : 0u);
    for (unsigned i = 0; i < PointCount; ++i) if (mask & (1u << i))
      Serial.printf("[%s] job=%lu attempt=%lu point=%u internal=%lu dma_free=%lu dma_largest=%lu\n",
        label, (unsigned long)job, (unsigned long)attempt, i, (unsigned long)heap[i][0],
        (unsigned long)heap[i][1], (unsigned long)heap[i][2]);
    if (valid) for (const auto& x : observed) {
      const uint32_t delta = x.sequence - start_sequence;
      if (!delta || delta > uint32_t(last - start_sequence)) continue;
      Serial.printf("[ALLOC_FAIL] job=%lu seq=%lu bytes=%lu caps=%08lx phase=%lu fn=%08lx owner=%s attempt=%lu\n",
        (unsigned long)job, (unsigned long)x.sequence, (unsigned long)x.bytes,
        (unsigned long)x.caps, (unsigned long)x.phase, (unsigned long)x.function_hash,
        label, (unsigned long)attempt);
    }
  }
  ~Trace() { finish(); }
};
struct VoiceTrace : Trace {
  VoiceTrace(uint32_t j, uint32_t a) : Trace(j, false) { start(a); }
};
struct ImagePutTrace : Trace {
  explicit ImagePutTrace(uint32_t j) : Trace(j, true) {}
};
inline void point(Point p, Phase phase) {
  if (!current) return;
  failures.phase.store(phase, std::memory_order_relaxed); current->sample(p);
}
inline void wrote(size_t bytes) { if (current) current->sent += uint32_t(bytes); }
#else
inline void begin() {}
struct VoiceTrace { VoiceTrace(uint32_t, uint32_t) {} void response(int) {} };
struct ImagePutTrace {
  explicit ImagePutTrace(uint32_t) {}
  bool start(uint32_t) { return false; }
  void finish() {}
  void response(int) {}
};
inline void point(Point, Phase) {}
inline void wrote(size_t) {}
#endif
} // namespace sense_memory
