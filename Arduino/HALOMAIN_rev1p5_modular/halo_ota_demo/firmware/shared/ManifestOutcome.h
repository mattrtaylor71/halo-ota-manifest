#pragma once
#include <stdint.h>
// Numeric observations only. A failed GET is not evidence of a received HTTP
// status or a specifically diagnosed TLS failure; tls_error is lastError().
enum class ManifestStage : uint8_t {
  None=0, InvalidUrl=1, DnsProbe=2, HttpBegin=3, Transport=4,
  HttpStatus=5, EmptyBody=6, JsonSyntax=7, JsonObject=8,
  VersionField=9, UrlField=10, ShaField=11, BuildField=12,
  DeadlineBefore=13, DeadlineAfter=14
};
struct ManifestOutcome {
  int32_t transport=0,tls_error=0;
  uint32_t elapsed_ms=0;
  int16_t http_status=0;
  ManifestStage stage=ManifestStage::None;
  uint8_t attempts=0,parser_code=0;
  uint8_t deadline_after=0,reserved[2]{};
};
static_assert(sizeof(ManifestOutcome)==20,"bounded manifest observations");
