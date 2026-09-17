#pragma once
#include <atomic>
#include <stdint.h>
#include <string.h>

// One owner-loop producer/consumer and one upload-worker transport. No manager,
// NVS or socket is shared across tasks. The slot stays owned until TLS cleanup
// has completed and the owner has consumed (or discarded) its result.
namespace provision_claim {
static constexpr uint32_t kAttemptMs = 20000;
static constexpr size_t kResponseBytes = 768;
struct Request {
  uint32_t generation = 0;
  uint32_t queued_ms = 0;
  char body[256] = {};
  char owner_code[32] = {};
  char previous_owner[64] = {};
  bool previous_owner_set = false;
};
struct Result {
  int http_code = -1;
  bool complete = false;
  bool overflow = false;
  uint32_t elapsed_ms = 0;
  char response[kResponseBytes + 1] = {};
};
class Job {
 public:
  bool busy() const { return state_.load() != Idle; }
  void cancel() { generation_.fetch_add(1); }
  bool submit(const Request& request) {
    if (busy()) return false;
    request_ = request;
    request_.generation = generation_.load();
    result_ = Result{};
    state_.store(Queued);
    return true;
  }
  const Request* take() {
    unsigned expected = Queued;
    return state_.compare_exchange_strong(expected, Running) ? &request_ : nullptr;
  }
  bool cancelled(const Request& request, uint32_t now) const {
    return request.generation != generation_.load() ||
           uint32_t(now - request.queued_ms) >= kAttemptMs;
  }
  void finish(const Result& result) { result_ = result; state_.store(Ready); }
  bool consume(Request& request, Result& result, bool& current) {
    if (state_.load() != Ready) return false;
    request = request_;
    result = result_;
    current = request.generation == generation_.load();
    request_ = Request{};
    result_ = Result{};
    state_.store(Idle);
    return true;
  }
 private:
  enum { Idle, Queued, Running, Ready };
  std::atomic<unsigned> state_{Idle};
  std::atomic<uint32_t> generation_{1};
  Request request_;
  Result result_;
};
}
