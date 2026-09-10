#pragma once
#include "DiagnosticAdmissionEnvelope.h"
#include <mbedtls/sha256.h>
namespace halo_admission {
static constexpr char AUTH_PREFIX[]="HALO_B1_POST_V1\nPOST\n/v1/private/ota/admission\n";
static constexpr size_t PRIVATE_ENVELOPE_MAX=1024;
inline void wipe(void*p,size_t n){volatile uint8_t*b=static_cast<volatile uint8_t*>(p);while(n--)*b++=0;}
inline bool key_id_ok(const char*id){if(!id)return false;const size_t n=strnlen(id,33);if(!n||n>32)return false;for(size_t i=0;i<n;++i){const char c=id[i];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'))return false;}return true;}
// Fixed32-byte key, no allocation and no key copied into an envelope/receipt.
// The exact domain has a trailing LF and deliberately excludes the C-string NUL.
class Hmac {
  mbedtls_sha256_context sha_;uint8_t outer_[64]{};bool good_=false;
public:
  Hmac(const uint8_t(&key)[32]){
    mbedtls_sha256_init(&sha_);uint8_t inner[64];
    for(unsigned i=0;i<64;++i){const uint8_t k=i<32?key[i]:0;inner[i]=k^0x36;outer_[i]=k^0x5c;}
    good_=mbedtls_sha256_starts(&sha_,0)==0&&mbedtls_sha256_update(&sha_,inner,64)==0;
    wipe(inner,sizeof(inner));
  }
  ~Hmac(){mbedtls_sha256_free(&sha_);wipe(outer_,sizeof(outer_));}
  bool append(const char*p,size_t n){return good_=good_&&mbedtls_sha256_update(&sha_,reinterpret_cast<const uint8_t*>(p),n)==0;}
  bool finish(uint8_t(&out)[32]){uint8_t inner[32]{};const bool ok=good_&&mbedtls_sha256_finish(&sha_,inner)==0&&mbedtls_sha256_starts(&sha_,0)==0&&mbedtls_sha256_update(&sha_,outer_,64)==0&&mbedtls_sha256_update(&sha_,inner,32)==0&&mbedtls_sha256_finish(&sha_,out)==0;good_=false;wipe(inner,sizeof(inner));return ok;}
};
struct AuthBuffer {uint8_t*data;size_t capacity,used;Hmac&hmac;
 static bool append(void*v,const char*p,size_t n){auto&b=*static_cast<AuthBuffer*>(v);if(n>b.capacity-b.used||!b.hmac.append(p,n))return false;memcpy(b.data+b.used,p,n);b.used+=n;return true;}
};
inline bool authenticated_envelope(const uint8_t(&context)[256],const uint8_t(&record)[64],Hash256 hash,
    const EnvelopeIdentity&id,const uint8_t(&key)[32],uint8_t*body,size_t capacity,size_t&length,char(&signature)[65]) {
  length=0;signature[0]=0;if(!body||capacity>PRIVATE_ENVELOPE_MAX)return false;
  Hmac hmac(key);if(!hmac.append(AUTH_PREFIX,sizeof(AUTH_PREFIX)-1))return false;
  AuthBuffer b{body,capacity,0,hmac};size_t n=0;uint8_t digest[32]{};
  const bool ok=write_envelope(context,record,hash,id,&b,AuthBuffer::append,n)&&n==b.used&&hmac.finish(digest);
  if(ok){halo_diag::hex_into(signature,digest,32);length=n;}wipe(digest,sizeof(digest));return ok;
}
} // namespace halo_admission
