#pragma once

// Versioned, bounded image-only storage. No Arduino, SD mounting or UART here.
// The caller owns the task-level SD lease for each complete method. No FILE or
// directory handle survives a call, so mount teardown cannot race a live file.
// Payload SHA256 is an immutable sender commitment; this store checks bytes
// with CRC32. Sense must also verify SHA256 before backend admission.
// .meta is the commit marker; .part/.meta.part are never advertised as jobs.
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace halo_image {
static constexpr uint32_t kSchema = 1;
static constexpr uint32_t kMaxBytes = 512UL * 1024UL;
static constexpr size_t kMetaBytes = 314;
static constexpr size_t kPathBytes = 192;
enum class Result { Ok, AlreadyStored, DuplicateChunk, Empty, Full, Invalid,
  Conflict, NotFound, Io, Corrupt, Incomplete, Sequence, Overflow };
struct CameraMeta {
  uint32_t profile=0, flash_enabled=0, jpeg_quality=0;
  uint32_t actual_width=0, actual_height=0, configured_framesize=0;
  int32_t scene_luma=0, scene_green_ratio=0;
  uint32_t xclk_hz=0;
};
struct Meta {
  uint32_t schema=kSchema, len=0, crc32=0, job_id=0, epoch=0, retries=0;
  uint64_t ordinal=0; // LCD-local commit order, never supplied by the peer.
  uint32_t quantity=1, add_to_shopping_list=0;
  CameraMeta camera;
  char owner_id[64]={}, device_id[32]={}, request_id[33]={};
  char checksum_sha256[65]={};
  char mode[16]={}, expiry[16]={};
};
struct Stats { uint32_t count = 0, invalid = 0, incomplete = 0; };

inline const char* result_name(Result r) {
  switch (r) {
    case Result::Ok: return "ok"; case Result::AlreadyStored: return "already_stored";
    case Result::DuplicateChunk: return "duplicate_chunk"; case Result::Empty: return "empty";
    case Result::Full: return "full"; case Result::Invalid: return "invalid";
    case Result::Conflict: return "conflict"; case Result::NotFound: return "not_found";
    case Result::Io: return "io"; case Result::Corrupt: return "corrupt";
    case Result::Incomplete: return "incomplete"; case Result::Sequence: return "sequence";
    case Result::Overflow: return "overflow";
  }
  return "unknown";
}
// IEEE CRC32, initial state ~0u and final XOR ~0u. Nibble table keeps CPU and
// storage overhead bounded without allocating a whole image in LCD RAM.
inline uint32_t crc32_update(uint32_t state, const uint8_t* data, size_t n) {
  static const uint32_t table[16] = {0,0x1db71064,0x3b6e20c8,0x26d930ac,
    0x76dc4190,0x6b6b51f4,0x4db26158,0x5005713c,0xedb88320,0xf00f9344,
    0xd6d6a3e8,0xcb61b38c,0x9b64c2b0,0x86d3d2d4,0xa00ae278,0xbdbdf21c};
  for (size_t i=0; i<n; ++i) { state ^= data[i]; state=(state>>4)^table[state&15]; state=(state>>4)^table[state&15]; }
  return state;
}
inline uint32_t crc32(const uint8_t* data, size_t n) { return crc32_update(~0u,data,n)^~0u; }
inline bool request_valid(const char* s) {
  if (!s || strnlen(s,33)!=32) return false;
  for (size_t i=0;i<32;++i) if (!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f'))) return false;
  return true;
}
inline bool bounded_text(const char* s, size_t cap, bool required) {
  if (!s || strnlen(s,cap)==cap || (required && !s[0])) return false;
  for (size_t i=0;s[i];++i) if ((unsigned char)s[i]<32 || (unsigned char)s[i]>126) return false;
  return true;
}
inline bool sha256_valid(const char* s) {
  if(!s||strnlen(s,65)!=64)return false;
  for(unsigned i=0;i<64;++i)if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return false;
  return true;
}
inline bool mode_valid(const char* mode) {
  return !strcmp(mode,"discard")||!strcmp(mode,"check-in")||!strcmp(mode,"check-out")||
    !strcmp(mode,"check_out")||!strcmp(mode,"dish")||!strcmp(mode,"dish-log")||!strcmp(mode,"dish_log");
}
inline bool camera_valid(const CameraMeta& c) {
  return c.profile<=255 && c.flash_enabled<=1 && c.jpeg_quality<=255 &&
    c.actual_width<=65535 && c.actual_height<=65535 && c.configured_framesize<=65535 &&
    c.scene_luma>=-32768 && c.scene_luma<=32767 && c.scene_green_ratio>=-32768 && c.scene_green_ratio<=32767;
}
inline bool camera_same(const CameraMeta& a,const CameraMeta& b) {
  return a.profile==b.profile && a.flash_enabled==b.flash_enabled && a.jpeg_quality==b.jpeg_quality &&
    a.actual_width==b.actual_width && a.actual_height==b.actual_height && a.configured_framesize==b.configured_framesize &&
    a.scene_luma==b.scene_luma && a.scene_green_ratio==b.scene_green_ratio && a.xclk_hz==b.xclk_hz;
}
inline bool valid(const Meta& m) {
  return m.schema==kSchema && m.len>0 && m.len<=kMaxBytes && m.job_id>0 && m.retries<=255 &&
    sha256_valid(m.checksum_sha256) && m.quantity>=1 && m.quantity<=65535 && m.add_to_shopping_list<=1 && camera_valid(m.camera) &&
    bounded_text(m.owner_id,sizeof(m.owner_id),true) && bounded_text(m.device_id,sizeof(m.device_id),true) &&
    bounded_text(m.request_id,sizeof(m.request_id),true) && request_valid(m.request_id) &&
    bounded_text(m.mode,sizeof(m.mode),true) && mode_valid(m.mode) && bounded_text(m.expiry,sizeof(m.expiry),false);
}
inline bool same(const Meta& a,const Meta& b) {
  // Retries are diagnostic; identity, capture options and bytes cannot change.
  return a.schema==b.schema && a.len==b.len && a.crc32==b.crc32 && a.job_id==b.job_id && a.epoch==b.epoch &&
    a.quantity==b.quantity && a.add_to_shopping_list==b.add_to_shopping_list && camera_same(a.camera,b.camera) &&
    !strcmp(a.owner_id,b.owner_id) && !strcmp(a.device_id,b.device_id) && !strcmp(a.request_id,b.request_id) &&
    !strcmp(a.checksum_sha256,b.checksum_sha256) && !strcmp(a.mode,b.mode) && !strcmp(a.expiry,b.expiry);
}
inline void put32(uint8_t*& p,uint32_t v) { for(unsigned i=0;i<4;++i)*p++=(uint8_t)(v>>(8*i)); }
inline uint32_t get32(const uint8_t*& p) { uint32_t v=0;for(unsigned i=0;i<4;++i)v|=(uint32_t)*p++<<(8*i);return v; }
inline void put64(uint8_t*& p,uint64_t v) { for(unsigned i=0;i<8;++i)*p++=(uint8_t)(v>>(8*i)); }
inline uint64_t get64(const uint8_t*& p) { uint64_t v=0;for(unsigned i=0;i<8;++i)v|=(uint64_t)*p++<<(8*i);return v; }
inline void encode(const Meta& m,uint8_t out[kMetaBytes]) {
  memset(out,0,kMetaBytes);uint8_t* p=out;memcpy(p,"HLIMAGE1",8);p+=8;
  put32(p,m.schema);put32(p,m.len);put32(p,m.crc32);put32(p,m.job_id);put32(p,m.epoch);put32(p,m.retries);
  put64(p,m.ordinal);put32(p,m.quantity);put32(p,m.add_to_shopping_list);
  put32(p,m.camera.profile);put32(p,m.camera.flash_enabled);put32(p,m.camera.jpeg_quality);
  put32(p,m.camera.actual_width);put32(p,m.camera.actual_height);put32(p,m.camera.configured_framesize);
  put32(p,(uint32_t)m.camera.scene_luma);put32(p,(uint32_t)m.camera.scene_green_ratio);put32(p,m.camera.xclk_hz);
  memcpy(p,m.owner_id,strlen(m.owner_id));p+=64;memcpy(p,m.device_id,strlen(m.device_id));p+=32;
  memcpy(p,m.request_id,32);p+=33;memcpy(p,m.checksum_sha256,64);p+=65;memcpy(p,m.mode,strlen(m.mode));p+=16;memcpy(p,m.expiry,strlen(m.expiry));p+=16;
  put32(p,crc32(out,kMetaBytes-4));
}
inline bool decode(const uint8_t in[kMetaBytes],Meta* m) {
  if(!m||memcmp(in,"HLIMAGE1",8))return false;
  const uint8_t* tail=in+kMetaBytes-4;if(get32(tail)!=crc32(in,kMetaBytes-4))return false;
  const uint8_t* p=in+8;Meta v;
  v.schema=get32(p);v.len=get32(p);v.crc32=get32(p);v.job_id=get32(p);v.epoch=get32(p);v.retries=get32(p);
  v.ordinal=get64(p);v.quantity=get32(p);v.add_to_shopping_list=get32(p);
  v.camera.profile=get32(p);v.camera.flash_enabled=get32(p);v.camera.jpeg_quality=get32(p);
  v.camera.actual_width=get32(p);v.camera.actual_height=get32(p);v.camera.configured_framesize=get32(p);
  v.camera.scene_luma=(int32_t)get32(p);v.camera.scene_green_ratio=(int32_t)get32(p);v.camera.xclk_hz=get32(p);
  memcpy(v.owner_id,p,64);p+=64;memcpy(v.device_id,p,32);p+=32;memcpy(v.request_id,p,33);p+=33;
  memcpy(v.checksum_sha256,p,65);p+=65;memcpy(v.mode,p,16);p+=16;memcpy(v.expiry,p,16);
  if(!valid(v))return false;*m=v;return true;
}

class Store {
 public:
  explicit Store(const char* root,uint32_t max_slots=40) : max_slots_(max_slots>40?40:max_slots) {
    if(root&&strlen(root)<sizeof(root_)-48)strcpy(root_,root);
  }
  void set_progress(void (*fn)()) { progress_=fn; }
  void set_budget(bool (*fn)()) { budget_=fn; }
  bool active() const { return active_; }
  const Meta& active_meta() const { return active_meta_; }
  uint32_t received() const { return got_; }
  uint16_t next_sequence() const { return next_; }
  bool payload_path(const char* request,char* out,size_t cap) const { return path(request,".jpg",out,cap); }

  Result begin(const Meta& m) {
    if(!valid(m)||!root_[0]||max_slots_==0)return Result::Invalid;
    if(active_)return Result::Conflict;
    if(mkdir(root_,0700)!=0&&errno!=EEXIST)return Result::Io;
    bool retired_same=false;
    Result cleanup=recover_deletes(m.owner_id,m.device_id,m.request_id,&retired_same);
    if(cleanup!=Result::Ok)return cleanup;
    // Do not reuse a request identity while retiring its accepted old record.
    if(retired_same)return Result::Conflict;
    Meta old; Result existing=lookup(m.request_id,m.owner_id,m.device_id,&old);
    if(existing==Result::Incomplete)return Result::Conflict;
    if(existing==Result::Io)return existing;
    if(existing==Result::Ok)return same(m,old)?Result::AlreadyStored:Result::Conflict;
    char commit[kPathBytes],stage[kPathBytes],part[kPathBytes],jpeg[kPathBytes];
    if(!path(m.request_id,".meta",commit,sizeof(commit))||!path(m.request_id,".meta.part",stage,sizeof(stage))
       ||!path(m.request_id,".part",part,sizeof(part))||!payload_path(m.request_id,jpeg,sizeof(jpeg)))return Result::Invalid;
    // A committed but corrupt or differently-owned record is never overwritten.
    if(exists(commit))return Result::Conflict;
    if(exists(stage)) {
      if(read_meta(stage,&old)!=Result::Ok||!same(m,old))return Result::Conflict;
      // Power stopped between payload rename and metadata commit: validate it
      // and finish that exact existing image without requiring another copy.
      if(exists(jpeg)) {
        if(check_payload(jpeg,m)!=Result::Ok)return Result::Corrupt;
        Meta committed=m;Result order=assign_ordinal(&committed);if(order!=Result::Ok)return order;
        uint8_t bytes[kMetaBytes];encode(committed,bytes);if(write_synced(stage,bytes,sizeof(bytes))!=Result::Ok)return Result::Io;
        if(rename(stage,commit)!=0)return Result::Io;
        return lookup(m.request_id,m.owner_id,m.device_id,&old)==Result::Ok?Result::AlreadyStored:Result::Corrupt;
      }
      // A partial transfer may restart only for its same bound request. No
      // committed image, unrelated partial or legacy photo is evicted.
    } else {
      if(exists(part)||exists(jpeg))return Result::Conflict;
      uint32_t slots=0; Result r=occupied(&slots);if(r!=Result::Ok)return r;
      if(slots>=max_slots_)return Result::Full;
      uint8_t bytes[kMetaBytes];encode(m,bytes);
      if(write_synced(stage,bytes,sizeof(bytes))!=Result::Ok)return Result::Io;
    }
    FILE* f=fopen(part,"wb");if(!f)return Result::Io;
    if(fclose(f)!=0)return Result::Io;
    active_meta_=m;active_=true;got_=0;next_=0;rolling_=~0u;last_len_=0;last_crc_=0;
    return Result::Ok;
  }
  Result append(uint16_t seq,const uint8_t* data,size_t n) {
    if(!active_)return Result::Incomplete;
    if(!data||n==0||n>512)return Result::Invalid;
    if(next_>0&&seq==(uint16_t)(next_-1)&&n==last_len_&&crc32(data,n)==last_crc_)return Result::DuplicateChunk;
    if(seq!=next_)return Result::Sequence;
    if(n>active_meta_.len-got_)return Result::Overflow;
    char p[kPathBytes];if(!path(active_meta_.request_id,".part",p,sizeof(p)))return Result::Invalid;
    FILE* f=fopen(p,"ab");if(!f)return Result::Io;
    bool ok=fwrite(data,1,n,f)==n;if(fclose(f)!=0)ok=false;
    if(!ok)return Result::Io;
    rolling_=crc32_update(rolling_,data,n);got_+=(uint32_t)n;next_++;
    last_len_=n;last_crc_=crc32(data,n);return Result::Ok;
  }
  Result finish(uint16_t end_seq) {
    if(!active_)return Result::Incomplete;
    if(end_seq!=next_)return Result::Sequence;
    if(got_!=active_meta_.len)return Result::Incomplete;
    if((rolling_^~0u)!=active_meta_.crc32)return Result::Corrupt;
    char part[kPathBytes],jpeg[kPathBytes],stage[kPathBytes],commit[kPathBytes];
    path(active_meta_.request_id,".part",part,sizeof(part));payload_path(active_meta_.request_id,jpeg,sizeof(jpeg));
    path(active_meta_.request_id,".meta.part",stage,sizeof(stage));path(active_meta_.request_id,".meta",commit,sizeof(commit));
    FILE* f=fopen(part,"r+b");if(!f)return Result::Io;
    bool synced=fflush(f)==0;if(synced&&fsync(fileno(f))!=0)synced=false;
    if(fclose(f)!=0)synced=false;if(!synced)return Result::Io;
    Result r=check_payload(part,active_meta_);if(r!=Result::Ok)return r;
    if(exists(jpeg)||exists(commit))return Result::Conflict;
    if(rename(part,jpeg)!=0)return Result::Io;
    Result order=assign_ordinal(&active_meta_);if(order!=Result::Ok)return order;
    uint8_t bytes[kMetaBytes];encode(active_meta_,bytes);
    if(write_synced(stage,bytes,sizeof(bytes))!=Result::Ok)return Result::Io;
    // The image is committed last, with its local commit ordinal.
    if(rename(stage,commit)!=0)return Result::Io;
    Meta observed; r=lookup(active_meta_.request_id,active_meta_.owner_id,active_meta_.device_id,&observed);
    if(r!=Result::Ok||!same(active_meta_,observed))return r==Result::Ok?Result::Corrupt:r;
    active_=false;return Result::Ok;
  }
  void abort() { active_=false; } // Leave partial bytes/metadata for diagnosis/retry.

  Result lookup(const char* request,const char* owner,const char* device,Meta* out) const {
    if(!request_valid(request)||!owner||!device||!out)return Result::Invalid;
    char p[kPathBytes];if(!path(request,".meta",p,sizeof(p)))return Result::Invalid;
    char deleted[kPathBytes];path(request,".delete",deleted,sizeof(deleted));
    struct stat st;if(stat(deleted,&st)==0)return Result::Incomplete;
    if(errno!=ENOENT)return Result::Io;
    Meta m;Result r=read_meta(p,&m);if(r!=Result::Ok)return r;
    if(strcmp(request,m.request_id)||m.ordinal==0)return Result::Corrupt;
    if(strcmp(owner,m.owner_id)||strcmp(device,m.device_id))return Result::Conflict;
    payload_path(request,p,sizeof(p));r=check_payload(p,m);if(r!=Result::Ok)return r;
    r=apply_attempt(&m);if(r!=Result::Ok)return r;
    *out=m;return Result::Ok;
  }
  Result list(const char* owner,const char* device,Meta* oldest,Stats* stats,const char* after_request=nullptr) const {
    if(!owner||!device||!oldest||!stats)return Result::Invalid;
    if(after_request&&after_request[0]&&!request_valid(after_request))return Result::Invalid;
    *stats=Stats{};
    Result cleanup=recover_deletes(owner,device);if(cleanup!=Result::Ok)return cleanup;
    uint64_t after_ordinal=0;bool missing_cursor=false;
    if(after_request&&after_request[0]){
      char p[kPathBytes];path(after_request,".meta",p,sizeof(p));Meta cursor;
      Result r=read_meta(p,&cursor);
      if(r==Result::Io)return r;
      if(r==Result::Ok&&cursor.ordinal&&!strcmp(cursor.request_id,after_request)&&
         !strcmp(cursor.owner_id,owner)&&!strcmp(cursor.device_id,device))after_ordinal=cursor.ordinal;
      else missing_cursor=true;
    }
    *stats=Stats{};DIR* d=opendir(root_);if(!d)return errno==ENOENT?Result::Empty:Result::Io;
    bool have=false;struct dirent* e;Result scan=Result::Ok;
    while((scan=next_entry(d,&e))==Result::Ok&&e) {
      if(budget_&&!budget_()){closedir(d);return Result::Io;}
      char request[33];const char* suffix=nullptr;
      if(!file_request(e->d_name,request,&suffix))continue;
      if(!strcmp(suffix,".meta.part")){stats->incomplete++;continue;}
      if(strcmp(suffix,".meta"))continue;
      Meta m;Result r=lookup(request,owner,device,&m);
      if(r==Result::Conflict)continue; // Other owner's retained image.
      if(r==Result::Io){closedir(d);return r;}
      if(r!=Result::Ok){stats->invalid++;continue;}
      stats->count++;
      if(missing_cursor||m.ordinal<=after_ordinal)continue;
      if(!have||m.ordinal<oldest->ordinal||(m.ordinal==oldest->ordinal&&strcmp(m.request_id,oldest->request_id)<0)){*oldest=m;have=true;}
      tick();
    }
    const bool ok=closedir(d)==0;if(!ok||scan!=Result::Ok)return Result::Io;
    return have?Result::Ok:Result::Empty;
  }
  // A image made without a trusted clock cannot be POSTed until this
  // first-attempt epoch is durably committed. Use a separate commit-once file:
  // FATFS does not guarantee POSIX rename-over-existing replacement semantics.
  Result mark_attempt(const char* request,const char* owner,const char* device,
                      uint32_t len,uint32_t crc,uint32_t epoch,Meta* out) {
    if(epoch<1577836800UL||!out)return Result::Invalid;
    Meta m;Result r=lookup(request,owner,device,&m);if(r!=Result::Ok)return r;
    if(m.len!=len||m.crc32!=crc)return Result::Conflict;
    if(m.epoch){if(m.epoch!=epoch)return Result::Conflict;*out=m;return Result::Ok;}
    m.epoch=epoch;uint8_t bytes[kMetaBytes];encode(m,bytes);
    char stage[kPathBytes],commit[kPathBytes];path(request,".attempt.part",stage,sizeof(stage));path(request,".attempt",commit,sizeof(commit));
    if(exists(commit))return Result::Conflict;
    if(write_synced(stage,bytes,sizeof(bytes))!=Result::Ok)return Result::Io;
    if(rename(stage,commit)!=0)return Result::Io;
    Meta observed;r=lookup(request,owner,device,&observed);
    if(r!=Result::Ok||!same(m,observed))return r==Result::Ok?Result::Corrupt:r;
    *out=observed;return Result::Ok;
  }
  // Read-only engineering inventory across owners. No identifiers or payload
  // leave this method. The caller prints only aggregate health counters.
  Result inventory(Stats* stats) const {
    if(!stats)return Result::Invalid;*stats=Stats{};
    DIR* d=opendir(root_);if(!d)return errno==ENOENT?Result::Empty:Result::Io;
    struct dirent* e;Result result=Result::Ok;
    while((result=next_entry(d,&e))==Result::Ok&&e){
      if(budget_&&!budget_()){result=Result::Io;break;}
      char request[33];const char* suffix;if(!file_request(e->d_name,request,&suffix))continue;
      if(!strcmp(suffix,".meta.part")||!strcmp(suffix,".attempt.part")){stats->incomplete++;continue;}
      char p[kPathBytes];
      if(!strcmp(suffix,".meta")){
        path(request,".meta",p,sizeof(p));Meta m;Result r=read_meta(p,&m);
        if(r==Result::Ok&&(strcmp(request,m.request_id)||!m.ordinal))r=Result::Corrupt;
        if(r==Result::Ok){payload_path(request,p,sizeof(p));r=check_payload(p,m);}
        if(r==Result::Ok)r=apply_attempt(&m);
        if(r==Result::Io){result=r;break;}
        if(r==Result::Ok)stats->count++;else stats->invalid++;
      }else{
        path(request,".meta",p,sizeof(p));if(exists(p))continue;
        path(request,".meta.part",p,sizeof(p));if(!exists(p))stats->incomplete++;
      }
      tick();
    }
    if(closedir(d)!=0)result=Result::Io;
    return result;
  }
  Result erase(const char* request,const char* owner,const char* device,uint32_t len,uint32_t crc) {
    if(!request_valid(request)||!owner||!device)return Result::Invalid;
    if(active_&&!strcmp(request,active_meta_.request_id))return Result::Conflict;
    char p[kPathBytes],stage[kPathBytes];path(request,".delete",p,sizeof(p));
    Meta receipt;Result r=read_meta(p,&receipt);
    if(r==Result::NotFound) {
      r=lookup(request,owner,device,&receipt);if(r!=Result::Ok)return r;
      if(receipt.len!=len||receipt.crc32!=crc)return Result::Conflict;
      // Only a fully validated record and its bound cloud receipt may create
      // accepted-delete authority. A staging file alone never authorizes it.
      uint8_t bytes[kMetaBytes];encode(receipt,bytes);
      path(request,".delete.part",stage,sizeof(stage));
      if(write_synced(stage,bytes,sizeof(bytes))!=Result::Ok)return Result::Io;
      if(rename(stage,p)!=0)return Result::Io;
      Meta observed;r=read_meta(p,&observed);
      if(r!=Result::Ok)return r;
      if(!same(receipt,observed)||receipt.ordinal!=observed.ordinal)return Result::Corrupt;
      receipt=observed;
    } else if(r!=Result::Ok)return r;
    if(strcmp(request,receipt.request_id)||!receipt.ordinal)return Result::Corrupt;
    if(strcmp(owner,receipt.owner_id)||strcmp(device,receipt.device_id)||
       receipt.len!=len||receipt.crc32!=crc)return Result::Conflict;
    return finish_delete(receipt);
  }

 private:
  char root_[kPathBytes]={};uint32_t max_slots_;bool active_=false;Meta active_meta_;
  uint32_t got_=0,rolling_=~0u,last_crc_=0;uint16_t next_=0;size_t last_len_=0;
  void (*progress_)()=nullptr;
  bool (*budget_)()=nullptr;
  void tick() const { if(progress_)progress_(); }
  bool path(const char* request,const char* suffix,char* out,size_t cap) const {
    if(!root_[0]||!request_valid(request)||!suffix||!out)return false;
    const int n=snprintf(out,cap,"%s/%s%s",root_,request,suffix);return n>0&&(size_t)n<cap;
  }
  static bool exists(const char* p) { struct stat st;return stat(p,&st)==0; }
  // A null directory entry is EOF only when readdir leaves errno clear.
  // Reset immediately before each call: metadata/payload I/O inside a scan
  // may set errno even when the next directory entry is read successfully.
  static Result next_entry(DIR* d,struct dirent** out) {
    errno=0;*out=readdir(d);return *out||errno==0?Result::Ok:Result::Io;
  }
  static bool file_request(const char* name,char request[33],const char** suffix) {
    if(!name||strlen(name)<33)return false;memcpy(request,name,32);request[32]=0;
    if(!request_valid(request))return false;*suffix=name+32;
    return !strcmp(*suffix,".meta")||!strcmp(*suffix,".jpg")||!strcmp(*suffix,".part")||!strcmp(*suffix,".meta.part")||!strcmp(*suffix,".attempt")||!strcmp(*suffix,".attempt.part")||!strcmp(*suffix,".delete")||!strcmp(*suffix,".delete.part");
  }
  // Accepted-delete recovery runs only under the caller's existing SD lease.
  // Validate every surviving file before removing any; conflicting/corrupt data
  // remains held. The committed receipt is removed last, so every interrupted
  // cleanup still has durable authority when the next admission/list resumes.
  Result finish_delete(const Meta& receipt) const {
    if(!valid(receipt)||!receipt.ordinal)return Result::Corrupt;
    char p[kPathBytes];
    const char* metadata[]={".meta",".meta.part",".attempt",".attempt.part",".delete.part"};
    for(const char* suffix:metadata){
      path(receipt.request_id,suffix,p,sizeof(p));Meta observed;Result r=read_meta(p,&observed);
      if(r==Result::NotFound)continue;if(r!=Result::Ok)return r;
      Meta expected=receipt;
      // A first-attempt marker overlays an immutable base epoch of zero.
      if((!strcmp(suffix,".meta")||!strcmp(suffix,".meta.part"))&&observed.epoch==0)expected.epoch=0;
      if(!same(expected,observed)||receipt.ordinal!=observed.ordinal)return Result::Conflict;
    }
    const char* payloads[]={".jpg",".part"};
    for(const char* suffix:payloads){
      path(receipt.request_id,suffix,p,sizeof(p));Result r=check_payload(p,receipt);
      if(r!=Result::Ok&&r!=Result::Incomplete)return r;
    }
    const char* removed[]={".jpg",".part",".meta.part",".attempt.part",".attempt",".meta",".delete.part"};
    for(const char* suffix:removed){
      path(receipt.request_id,suffix,p,sizeof(p));
      if(remove(p)!=0&&errno!=ENOENT)return Result::Io;
    }
    path(receipt.request_id,".delete",p,sizeof(p));
    if(remove(p)!=0)return Result::Io;
    return Result::Ok;
  }
  Result recover_deletes(const char* owner,const char* device,const char* target=nullptr,bool* retired_target=nullptr) const {
    DIR* d=opendir(root_);if(!d)return errno==ENOENT?Result::Ok:Result::Io;
    struct dirent* entry;Result result=Result::Ok;uint32_t receipts=0;
    while((result=next_entry(d,&entry))==Result::Ok&&entry){
      if(budget_&&!budget_()){result=Result::Io;break;}
      char request[33];const char* suffix;
      if(!file_request(entry->d_name,request,&suffix)||strcmp(suffix,".delete"))continue;
      if(++receipts>40){result=Result::Io;break;}
      char p[kPathBytes];path(request,".delete",p,sizeof(p));Meta receipt;
      Result r=read_meta(p,&receipt);
      if(r==Result::Io){result=r;break;}
      if(r!=Result::Ok||strcmp(request,receipt.request_id)||!receipt.ordinal)continue;
      if(strcmp(owner,receipt.owner_id)||strcmp(device,receipt.device_id))continue;
      if(target&&retired_target&&!strcmp(target,request))*retired_target=true;
      r=finish_delete(receipt);
      if(r==Result::Io){result=r;break;}
      // Invalid or conflicting survivors must stay held, never be guessed safe.
      tick();
    }
    if(closedir(d)!=0)result=Result::Io;
    return result;
  }
  Result occupied(uint32_t* count) const {
    *count=0;DIR* d=opendir(root_);if(!d)return Result::Io;
    // Count one stem, including invalid/uncommitted data. Bound memory to the
    // admission cap; exceeding it rejects new records, never deletes old ones.
    char seen[40][33]={};uint32_t n=0;struct dirent* e;Result r=Result::Ok;
    while((r=next_entry(d,&e))==Result::Ok&&e) {
      if(budget_&&!budget_()){r=Result::Io;break;}
      char req[33];const char* suffix;if(!file_request(e->d_name,req,&suffix))continue;
      bool found=false;for(uint32_t i=0;i<n;++i)if(!strcmp(seen[i],req)){found=true;break;}
      if(found)continue;if(n>=40){*count=40;break;}strcpy(seen[n++],req);*count=n;
    }
    if(closedir(d)!=0)r=Result::Io;return r;
  }
  static Result write_synced(const char* p,const uint8_t* bytes,size_t n) {
    FILE* f=fopen(p,"wb");if(!f)return Result::Io;
    bool ok=fwrite(bytes,1,n,f)==n;if(ok&&fflush(f)!=0)ok=false;
    if(ok&&fsync(fileno(f))!=0)ok=false;if(fclose(f)!=0)ok=false;
    return ok?Result::Ok:Result::Io;
  }
  static Result read_meta(const char* p,Meta* out) {
    FILE* f=fopen(p,"rb");if(!f)return errno==ENOENT?Result::NotFound:Result::Io;
    uint8_t bytes[kMetaBytes];const size_t n=fread(bytes,1,sizeof(bytes),f);
    const int extra=fgetc(f);const bool err=ferror(f)!=0;const bool closed=fclose(f)==0;
    if(err||!closed)return Result::Io;
    return n==sizeof(bytes)&&extra==EOF&&decode(bytes,out)?Result::Ok:Result::Corrupt;
  }
  Result assign_ordinal(Meta* m) const {
    DIR* d=opendir(root_);if(!d)return Result::Io;uint64_t largest=0;Result result=Result::Ok;
    struct dirent* e;
    while((result=next_entry(d,&e))==Result::Ok&&e){
      if(budget_&&!budget_()){result=Result::Io;break;}
      char request[33];const char* suffix;if(!file_request(e->d_name,request,&suffix)||strcmp(suffix,".meta"))continue;
      char p[kPathBytes];path(request,".meta",p,sizeof(p));Meta v;Result r=read_meta(p,&v);
      if(r==Result::Io){result=r;break;}
      if(r==Result::Ok&&!strcmp(request,v.request_id)&&v.ordinal>largest)largest=v.ordinal;
      tick();
    }
    if(closedir(d)!=0)result=Result::Io;if(result!=Result::Ok)return result;
    if(largest==UINT64_MAX)return Result::Overflow;m->ordinal=largest+1;return Result::Ok;
  }
  Result apply_attempt(Meta* m) const {
    char p[kPathBytes];path(m->request_id,".attempt",p,sizeof(p));Meta a;
    Result r=read_meta(p,&a);if(r==Result::NotFound)return Result::Ok;if(r!=Result::Ok)return r;
    // The base metadata is immutable; only an unknown first-attempt epoch can
    // be filled once. A conflicting marker quarantines the record.
    if(m->epoch!=0||a.epoch<1577836800UL||a.ordinal!=m->ordinal)return Result::Corrupt;
    Meta expected=*m;expected.epoch=a.epoch;if(!same(expected,a))return Result::Corrupt;
    *m=a;return Result::Ok;
  }
  Result check_payload(const char* p,const Meta& m) const {
    FILE* f=fopen(p,"rb");if(!f)return errno==ENOENT?Result::Incomplete:Result::Io;
    uint8_t buf[1024];uint32_t state=~0u,total=0;bool ok=true;
    bool expired=false;
    while(true){if(budget_&&!budget_()){expired=true;ok=false;break;}const size_t n=fread(buf,1,sizeof(buf),f);if(n){if(n>m.len-total){ok=false;break;}total+=(uint32_t)n;state=crc32_update(state,buf,n);tick();}if(n<sizeof(buf)){if(ferror(f))ok=false;break;}}
    if(fclose(f)!=0)ok=false;
    if(expired)return Result::Io;
    return ok&&total==m.len&&(state^~0u)==m.crc32?Result::Ok:Result::Corrupt;
  }
};
} // namespace halo_image
