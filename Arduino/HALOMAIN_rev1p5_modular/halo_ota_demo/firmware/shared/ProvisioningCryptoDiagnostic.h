#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>

// Manual owner-loop diagnostic only. Never call from a Wi-Fi callback or setup.
// These public vectors exercise the mbedTLS primitives used by the pinned
// IDF 5.5.4 WPA crypto_mbedtls.c adapter, with the current global allocator.
// They do not inspect AP credentials, change allocation policy or prove that
// the authenticator selected the right PMK/nonce/peer for a real handshake.
namespace provision_crypto_diag {

struct Check {
  int rc;
  bool matches;
};

struct Result {
  Check hmac_sha1;
  Check pbkdf2_sha1;
  bool ok() const {
    return hmac_sha1.rc == 0 && hmac_sha1.matches &&
           pbkdf2_sha1.rc == 0 && pbkdf2_sha1.matches;
  }
};

inline Check check_hmac_sha1() {
  // RFC 2202 section 3, case 7: multi-block data and a key longer than SHA1's
  // block. Split updates mirror the WPA adapter's vector-HMAC path.
  static const unsigned char data[] =
      "Test Using Larger Than Block-Size Key and Larger Than One Block-Size Data";
  static const unsigned char expected[] = {
      0xe8,0xe9,0x9d,0x0f,0x45,0x23,0x7d,0x78,0x6d,0x6b,
      0xba,0xa7,0x96,0x5c,0x78,0x08,0xbb,0xff,0x1a,0x91};
  unsigned char key[80];
  memset(key, 0xaa, sizeof(key));
  unsigned char output[sizeof(expected)] = {};
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
  int rc = info ? mbedtls_md_setup(&ctx, info, 1) : MBEDTLS_ERR_MD_FEATURE_UNAVAILABLE;
  if (!rc) rc = mbedtls_md_hmac_starts(&ctx, key, sizeof(key));
  if (!rc) rc = mbedtls_md_hmac_update(&ctx, data, 31);
  if (!rc) rc = mbedtls_md_hmac_update(&ctx, data + 31, sizeof(data) - 1 - 31);
  if (!rc) rc = mbedtls_md_hmac_finish(&ctx, output);
  mbedtls_md_free(&ctx);
  return {rc, rc == 0 && memcmp(output, expected, sizeof(expected)) == 0};
}

inline Check check_pbkdf2_sha1() {
  // RFC 6070 section 2, 4096-iteration/25-byte case. This spans two PBKDF2
  // output blocks, as does a WPA2 PMK, without touching the device's password.
  static const unsigned char password[] = "passwordPASSWORDpassword";
  static const unsigned char salt[] = "saltSALTsaltSALTsaltSALTsaltSALTsalt";
  static const unsigned char expected[] = {
      0x3d,0x2e,0xec,0x4f,0xe4,0x1c,0x84,0x9b,0x80,0xc8,0xd8,0x36,0x62,
      0xc0,0xe4,0x4a,0x8b,0x29,0x1a,0x96,0x4c,0xf2,0xf0,0x70,0x38};
  unsigned char output[sizeof(expected)] = {};
  const int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(
      MBEDTLS_MD_SHA1, password, sizeof(password) - 1, salt, sizeof(salt) - 1,
      4096, sizeof(output), output);
  return {rc, rc == 0 && memcmp(output, expected, sizeof(expected)) == 0};
}

inline Result run() {
  Result result;
  result.hmac_sha1 = check_hmac_sha1();
  result.pbkdf2_sha1 = check_pbkdf2_sha1();
  return result;
}

template<class Logger>
inline Result run_and_log(Logger& logger) {
  const Result result = run();
  logger.printf("[PROVISION_CRYPTO_DIAG] hmac_sha1_rc=%d hmac_sha1_ok=%d "
                "pbkdf2_sha1_rc=%d pbkdf2_sha1_ok=%d all_ok=%d\n",
                result.hmac_sha1.rc, result.hmac_sha1.matches ? 1 : 0,
                result.pbkdf2_sha1.rc, result.pbkdf2_sha1.matches ? 1 : 0,
                result.ok() ? 1 : 0);
  return result;
}

}  // namespace provision_crypto_diag
