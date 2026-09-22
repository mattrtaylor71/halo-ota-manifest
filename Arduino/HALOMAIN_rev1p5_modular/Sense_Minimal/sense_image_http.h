#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace sense_image_http {
// Borrow the signed request target from the caller's immutable URL. A String
// copy and printf's second full-path buffer otherwise occupy scarce internal
// memory while TLS/AES is active. The URL must outlive this view and its writes.
struct Target {
  char host[254]{};
  const char* path = nullptr;
  size_t path_length = 0;
  uint16_t port = 0;
  bool https = false;
};

inline bool parse(const char* url, size_t length, Target& out) {
  out = Target{};
  if (!url || !length || length > 4096 || memchr(url, '\0', length)) return false;
  const bool https = length >= 8 && !memcmp(url, "https://", 8);
  const bool http = length >= 7 && !memcmp(url, "http://", 7);
  if (!https && !http) return false;
  const size_t start = https ? 8 : 7;
  size_t end = start;
  while (end < length && url[end] != '/') ++end;
  size_t host_end = start;
  while (host_end < end && url[host_end] != ':') {
    const char c = url[host_end];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
    ++host_end;
  }
  const size_t host_length = host_end - start;
  if (!host_length || host_length >= sizeof(out.host)) return false;
  uint32_t port = https ? 443 : 80;
  if (host_end != end) {
    port = 0;
    if (host_end + 1 == end) return false;
    for (size_t i = host_end + 1; i < end; ++i) {
      if (url[i] < '0' || url[i] > '9') return false;
      port = port * 10 + unsigned(url[i] - '0');
      if (port > 65535) return false;
    }
    if (!port) return false;
  }
  // Never normalize, decode or rebuild a signed path/query.
  for (size_t i = end; i < length; ++i)
    if (uint8_t(url[i]) <= 0x20 || uint8_t(url[i]) == 0x7f) return false;
  memcpy(out.host, url + start, host_length);
  out.host[host_length] = '\0';
  out.path = end < length ? url + end : "/";
  out.path_length = end < length ? length - end : 1;
  out.port = uint16_t(port);
  out.https = https;
  return true;
}

// Coalesce small fields into bounded writes instead of allocating a complete
// header String or emitting a separate TLS record for every field fragment.
// This buffer lives on the existing upload worker's stack, not the DMA heap.
template <typename Client, typename Allowed>
class HeaderWriter {
 public:
  HeaderWriter(Client& client, Allowed& allowed) : client_(client), allowed_(allowed) {}
  bool append(const char* bytes, size_t length) {
    while (length) {
      const size_t room = sizeof(buffer_) - used_;
      const size_t count = length < room ? length : room;
      memcpy(buffer_ + used_, bytes, count);
      used_ += count; bytes += count; length -= count;
      if (used_ == sizeof(buffer_) && !finish()) return false;
    }
    return true;
  }
  bool field(const char* name, const char* value) {
    return append(name, strlen(name)) && append(value, strlen(value)) && append("\r\n", 2);
  }
  bool finish() {
    size_t offset = 0;
    while (offset < used_) {
      if (!allowed_()) return false;
      const size_t left = used_ - offset;
      const size_t sent = client_.write(
          reinterpret_cast<const uint8_t*>(buffer_ + offset), left);
      if (!sent || sent > left) return false;
      offset += sent;
    }
    used_ = 0;
    return true;
  }
 private:
  Client& client_;
  Allowed& allowed_;
  char buffer_[512];
  size_t used_ = 0;
};

template <typename Client, typename Allowed>
bool write_headers(Client& client, const Target& target, const char* content_type,
                   size_t content_length, const char* checksum, Allowed allowed) {
  char length[21];
  const int count = snprintf(length, sizeof(length), "%lu", (unsigned long)content_length);
  if (!target.path || !target.path_length || !content_type ||
      count <= 0 || size_t(count) >= sizeof(length)) return false;
  HeaderWriter<Client, Allowed> writer(client, allowed);
  return writer.append("PUT ", 4) && writer.append(target.path, target.path_length) &&
      writer.append(" HTTP/1.1\r\n", 11) &&
      writer.field("Host: ", target.host) &&
      writer.field("Content-Type: ", content_type) &&
      writer.field("Content-Length: ", length) &&
      (!checksum || (writer.field("If-None-Match: ", "*") &&
                     writer.field("x-amz-checksum-sha256: ", checksum))) &&
      writer.append("Connection: close\r\n\r\n", 21) && writer.finish();
}
} // namespace sense_image_http
