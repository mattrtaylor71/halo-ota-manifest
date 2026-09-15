/* Durable image admission. Include after UploadJob, PresignReply, HTTP and camera helpers.
 * All attempts use the captured account/request/options. Never fall back to legacy
 * presign: that allocates another cloud job after an uncertain upload response.
 */
#ifndef SENSE_IMAGE_UPLOAD_H
#define SENSE_IMAGE_UPLOAD_H
#include <mbedtls/sha256.h>

static bool sense_image_hex(const char* s, size_t n) {
  if (!s || strnlen(s, n + 1) != n) return false;
  for (size_t i = 0; i < n; ++i)
    if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
  return true;
}

static bool sense_image_id(const char* s, size_t cap) {
  const size_t n = s ? strnlen(s, cap) : 0;
  if (!n || n == cap) return false;
  for (size_t i = 0; i < n; ++i)
    if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
          (s[i] >= '0' && s[i] <= '9') || s[i] == '-' || s[i] == '_')) return false;
  return true;
}

static bool sense_image_operation_token(const UploadJob& job, char out[65]) {
  if (job.is_voice || !job.image_len || job.image_len > 512U * 1024U ||
      !sense_image_id(job.image.owner_id, sizeof(job.image.owner_id)) ||
      !sense_image_id(job.image.device_id, sizeof(job.image.device_id)) ||
      !sense_image_hex(job.image.request_id, 32) ||
      !sense_image_hex(job.image.checksum_sha256, 64)) return false;
  // Length-prefix each field so concatenation cannot alias another owner/device.
  char framed[192];
  const int n = snprintf(framed, sizeof(framed), "halo-image-v1:%u:%s%u:%s32:%s",
      (unsigned)strlen(job.image.owner_id), job.image.owner_id,
      (unsigned)strlen(job.image.device_id), job.image.device_id, job.image.request_id);
  uint8_t hash[32];
  if (n < 0 || (size_t)n >= sizeof(framed) ||
      mbedtls_sha256((const unsigned char*)framed, (size_t)n, hash, 0) != 0) return false;
  for (unsigned i = 0; i < 32; ++i) snprintf(out + i * 2, 3, "%02x", hash[i]);
  return true;
}

static bool sense_image_checksum_base64(const char* hex, char out[45]) {
  if (!sense_image_hex(hex, 64)) return false;
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  uint8_t bytes[32];
  for (unsigned i = 0; i < 32; ++i) {
    const unsigned a = hex[i*2] <= '9' ? hex[i*2]-'0' : hex[i*2]-'a'+10;
    const unsigned b = hex[i*2+1] <= '9' ? hex[i*2+1]-'0' : hex[i*2+1]-'a'+10;
    bytes[i] = (uint8_t)((a << 4) | b);
  }
  unsigned o = 0;
  for (unsigned i = 0; i < 32; i += 3) {
    const uint32_t v = ((uint32_t)bytes[i] << 16) |
        (i+1 < 32 ? (uint32_t)bytes[i+1] << 8 : 0) | (i+2 < 32 ? bytes[i+2] : 0);
    out[o++] = alphabet[(v >> 18) & 63]; out[o++] = alphabet[(v >> 12) & 63];
    out[o++] = i+1 < 32 ? alphabet[(v >> 6) & 63] : '=';
    out[o++] = i+2 < 32 ? alphabet[v & 63] : '=';
  }
  out[o] = 0; return true;
}

static bool sense_image_contract_request(const UploadJob& job, bool reconcile, JsonDocument& doc) {
  char token[65];
  if (!sense_image_operation_token(job, token) ||
      strnlen(job.mode, sizeof(job.mode)) == sizeof(job.mode) ||
      strnlen(job.expiry_date, sizeof(job.expiry_date)) == sizeof(job.expiry_date)) return false;
  const char* type = !strcmp(job.mode,"check-in") ? "grocery" :
      !strcmp(job.mode,"discard") ? "discard" : !strcmp(job.mode,"dish") ? "dish" : nullptr;
  if (!type) return false;
  doc.clear();
  doc["owner"] = job.image.owner_id; doc["user_id"] = job.image.owner_id;
  doc["device_id"] = job.image.device_id; doc["type"] = type; doc["action"] = "IN";
  doc["content_type"] = "image/jpeg";
  doc["quantity"] = job.quantity ? job.quantity : 1;
  doc["add_to_shopping_list"] = !strcmp(type,"discard") && job.add_to_shopping_list;
  if (job.expiry_date[0]) doc["product_expiration"] = job.expiry_date;
  append_camera_meta_json(doc, job.camera_meta);
  doc["upload_operation"]["version"] = 1;
  doc["upload_operation"]["token"] = token;
  doc["upload_operation"]["checksum_sha256"] = job.image.checksum_sha256;
  doc["upload_operation"]["content_length"] = (uint32_t)job.image_len;
  if (reconcile) doc["upload_operation"]["reconcile_only"] = true;
  return !doc.overflowed() && measureJson(doc) <= 2048;
}

static bool sense_image_object_key(const UploadJob& job, const char* key, const char* cloud_job) {
  if (!key || !sense_image_hex(cloud_job,32) || strnlen(key,1025) > 1024) return false;
  char prefix[128], suffix[38];
  snprintf(prefix,sizeof(prefix),"images/%s/%s/",job.image.owner_id,job.image.device_id);
  snprintf(suffix,sizeof(suffix),"/%s.jpg",cloud_job);
  const size_t n = strlen(key), a = strlen(prefix), b = strlen(suffix);
  if (n != a + 10 + b || strncmp(key,prefix,a) || strcmp(key+n-b,suffix)) return false;
  for (unsigned i=0;i<10;++i) {
    const char c=key[a+i];
    if (i==4 || i==7) { if(c!='/') return false; }
    else if(c<'0'||c>'9') return false;
  }
  return true;
}

static bool sense_image_put_url(const char* url, const char* key) {
  if (!url || strnlen(url,4097)>4096 || strncmp(url,"https://",8)) return false;
  const char* path=strchr(url+8,'/'); if(!path) return false;
  const size_t host_len=(size_t)(path-(url+8));
  static const char* suffixes[]={".s3.amazonaws.com",".s3.us-east-1.amazonaws.com"};
  bool host_ok=false;
  for(const char* suffix:suffixes){const size_t n=strlen(suffix);
    if(host_len>n && !strncmp(path-n,suffix,n))host_ok=true;}
  if(!host_ok)return false;
  for(const char* p=url+8;p<path;++p)
    if(!((*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')||*p=='.'||*p=='-'))return false;
  const size_t n=strlen(key);
  return !strncmp(path+1,key,n) && path[n+1]=='?' && path[n+2] &&
      strstr(path+n+2,"X-Amz-Algorithm=AWS4-HMAC-SHA256") != nullptr;
}

static bool sense_image_contract_reply(const UploadJob& job, bool reconcile, int code,
                                       JsonDocument& doc, PresignReply& out) {
  char token[65], checksum[45];
  if (code!=200 || doc.overflowed() || !sense_image_operation_token(job,token) ||
      !sense_image_checksum_base64(job.image.checksum_sha256,checksum)) return false;
  const JsonVariantConst proof=doc["upload_operation"];
  if (!doc["upload_protocol"].is<unsigned>() || doc["upload_protocol"].as<unsigned>()!=1 ||
      !proof["version"].is<unsigned>() || proof["version"].as<unsigned>()!=1 ||
      !proof["token"].is<const char*>() || strcmp(proof["token"].as<const char*>(),token) ||
      !proof["checksum_sha256"].is<const char*>() ||
      strcmp(proof["checksum_sha256"].as<const char*>(),job.image.checksum_sha256) ||
      !proof["content_length"].is<uint32_t>() || proof["content_length"].as<uint32_t>()!=job.image_len ||
      !doc["job_id"].is<const char*>() || !doc["s3_key"].is<const char*>() ||
      !sense_image_object_key(job,doc["s3_key"].as<const char*>(),doc["job_id"].as<const char*>()) ||
      !doc["content_type"].is<const char*>() || strcmp(doc["content_type"].as<const char*>(),"image/jpeg") ||
      !doc["upload_stored"].is<bool>()) return false;
  const bool stored=doc["upload_stored"].as<bool>();
  if (stored != reconcile) return false;
  if (reconcile && (out.job_id.isEmpty() || out.s3_key.isEmpty() ||
      out.job_id != doc["job_id"].as<const char*>() || out.s3_key != doc["s3_key"].as<const char*>())) return false;
  if (!stored) {
    const JsonVariantConst headers=doc["put_headers"];
    if (!headers.is<JsonObjectConst>() || headers.size()!=3 ||
        !headers["If-None-Match"].is<const char*>() || strcmp(headers["If-None-Match"].as<const char*>(),"*") ||
        !headers["Content-Type"].is<const char*>() || strcmp(headers["Content-Type"].as<const char*>(),"image/jpeg") ||
        !headers["x-amz-checksum-sha256"].is<const char*>() ||
        strcmp(headers["x-amz-checksum-sha256"].as<const char*>(),checksum) ||
        !doc["put_url"].is<const char*>() ||
        !sense_image_put_url(doc["put_url"].as<const char*>(),doc["s3_key"].as<const char*>()) ||
        !doc["expires_in"].is<unsigned>() || !doc["expires_in"].as<unsigned>() ||
        doc["expires_in"].as<unsigned>()>900) return false;
  }
  PresignReply next;
  next.job_id=doc["job_id"].as<const char*>(); next.s3_key=doc["s3_key"].as<const char*>();
  next.content_type="image/jpeg"; next.immutable_image=true; next.upload_stored=stored;
  next.operation_token=token; next.checksum_sha256_b64=checksum;
  next.expected_image_bytes=(uint32_t)job.image_len;
  if (!stored) { next.put_url=doc["put_url"].as<const char*>(); next.ttl_s=doc["expires_in"].as<unsigned>(); }
  out=next; return true;
}

static bool sense_image_presign_request(const UploadJob& job, PresignReply& out,
                                        uint32_t deadline_ms, bool reconcile) {
  char token_guard[65];
  if (deadline_expired(deadline_ms) || !sense_image_operation_token(job,token_guard)) return false;
  char owner[64]={},device[32]={}; load_owner_id_or_default(owner,sizeof(owner));
  load_runtime_device_id(device,sizeof(device));
  if(strcmp(owner,job.image.owner_id)||strcmp(device,job.image.device_id)) return false;
  DynamicJsonDocument request(2048);
  if (!sense_image_contract_request(job,reconcile,request)) return false;
  String body; serializeJson(request,body);
  const String url=String(CHECKIN_API_BASE_URL)+CHECKIN_PRESIGN_ENDPOINT;
  int code=0; String response;
  const bool sent=http_post_json_with_retries(url.c_str(),body,code,response,
      reconcile ? "IMAGE_RECONCILE" : "IMAGE_PRESIGN",API_KEY,BEARER_TOKEN,job.job_id,deadline_ms);
  Serial.printf("[IMAGE_UPLOAD] %s http=%d bytes=%u\n",reconcile?"reconcile":"presign",code,(unsigned)response.length());
  if(!sent || deadline_expired(deadline_ms) || response.length()>6144) return false;
  DynamicJsonDocument reply(6144);
  if(deserializeJson(reply,response)) return false;
  return sense_image_contract_reply(job,reconcile,code,reply,out);
}

static bool sense_image_get_presign(const UploadJob& job, PresignReply& out, uint32_t deadline_ms) {
  return sense_image_presign_request(job,out,deadline_ms,false);
}
static bool sense_image_reconcile_stored(const UploadJob& job, PresignReply& out, uint32_t deadline_ms) {
  return sense_image_presign_request(job,out,deadline_ms,true);
}
#endif
