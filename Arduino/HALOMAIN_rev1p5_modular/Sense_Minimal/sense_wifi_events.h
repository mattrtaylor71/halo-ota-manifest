#ifndef SENSE_WIFI_EVENTS_H
#define SENSE_WIFI_EVENTS_H

#include <stddef.h>
#include <stdint.h>

// The SDK callback only copies into this bounded queue. The caller holds its
// short critical section for push/pop; logging and Wi-Fi work happen outside.
namespace sense_wifi_events {
struct Event {
  int32_t id;
  uint32_t at_ms;
  uint16_t reason;
};

template <size_t Capacity> class Queue {
 public:
  static_assert(Capacity > 0, "Wi-Fi event queue must have capacity");
  void push(const Event& event) {
    if (count_ == Capacity) {
      // Keep the newest reason/status sequence and report lost diagnostics.
      read_ = (read_ + 1) % Capacity;
      --count_;
      if (dropped_ != UINT32_MAX) ++dropped_;
    }
    events_[write_] = event;
    write_ = (write_ + 1) % Capacity;
    ++count_;
  }
  bool pop(Event& event) {
    if (!count_) return false;
    event = events_[read_];
    read_ = (read_ + 1) % Capacity;
    --count_;
    return true;
  }
  uint32_t take_dropped() {
    const uint32_t result = dropped_;
    dropped_ = 0;
    return result;
  }

 private:
  Event events_[Capacity] = {};
  size_t read_ = 0, write_ = 0, count_ = 0;
  uint32_t dropped_ = 0;
};
}  // namespace sense_wifi_events
#endif
