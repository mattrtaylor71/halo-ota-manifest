#!/usr/bin/env python3
"""Exercise the production image store on a real temporary filesystem.

Only the filesystem error boundaries are injected. No devices, cloud calls,
firmware builds, or retained user files are touched.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

HARNESS = r'''
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cerrno>
#include <string>
#include <vector>
#include <filesystem>
#include <unistd.h>
#include <sys/stat.h>
static bool fail_write=false,fail_sync=false,fail_close=false;
static int rename_until_failure=-1;
static size_t injected_fwrite(const void* p,size_t s,size_t n,FILE* f){
  if(fail_write){errno=ENOSPC;return 0;}return ::fwrite(p,s,n,f);
}
static int injected_fsync(int fd){if(fail_sync){errno=EIO;return -1;}return ::fsync(fd);}
static int injected_fclose(FILE* f){int rc=::fclose(f);if(fail_close){errno=EIO;return -1;}return rc;}
static int injected_rename(const char* a,const char* b){
  if(rename_until_failure==0){errno=EIO;return -1;}
  if(rename_until_failure>0)--rename_until_failure;
  return ::rename(a,b);
}
#define fwrite injected_fwrite
#define fsync injected_fsync
#define fclose injected_fclose
#define rename injected_rename
#include "ImageSpoolStore.h"
#undef fwrite
#undef fsync
#undef fclose
#undef rename
using namespace halo_image;
namespace fs=std::filesystem;
static unsigned checks=0,failures=0;
static void check(bool ok,const char* what){++checks;if(!ok){++failures;std::fprintf(stderr,"FAIL: %s\n",what);}}
static uint32_t reference_crc(const uint8_t* p,size_t n){
  uint32_t c=0xffffffffu;
  for(size_t i=0;i<n;++i){c^=p[i];for(int k=0;k<8;++k)c=(c>>1)^(0xedb88320u&-(c&1));}
  return c^0xffffffffu;
}
static const std::string jpeg="\x01\x02\x03\x04\x05\x06\x07\x08";
static std::string root;
static bool allow_budget=true;
static bool within_budget(){return allow_budget;}
static std::string directory(const char* name){std::string p=root+"/"+name;fs::create_directory(p);return p;}
static Meta meta(unsigned n=1){
  Meta m{};m.schema=1;m.len=jpeg.size();m.crc32=reference_crc((const uint8_t*)jpeg.data(),jpeg.size());
  std::strcpy(m.checksum_sha256,"66840dda154e8a113c31dd0ad32f7f3a366a80e8136979d8f5a101d3d29d6f72");m.job_id=12;m.epoch=1789500000;m.retries=1;
  std::snprintf(m.owner_id,sizeof(m.owner_id),"owner-a");
  std::snprintf(m.device_id,sizeof(m.device_id),"halo-unit-a");
  std::strcpy(m.mode,"check-in");m.quantity=2;m.add_to_shopping_list=1; m.camera.actual_width=640;m.camera.actual_height=480;m.camera.scene_luma=-1;
  std::snprintf(m.request_id,sizeof(m.request_id),"%032u",n);return m;
}
static Result write_all(Store& s,const Meta& m){
  Result r=s.begin(m);if(r!=Result::Ok)return r;
  r=s.append(0,(const uint8_t*)jpeg.data(),jpeg.size());
  return r==Result::Ok?s.finish(1):r;
}
static unsigned count(Store& s,const Meta& m){
  Stats stats{};Meta first{};s.list(m.owner_id,m.device_id,&first,&stats);return stats.count;
}
int main(int argc,char** argv){
  if(argc!=2)return 2;root=argv[1];
  check(reference_crc((const uint8_t*)"123456789",9)==0xcbf43926u,"independent CRC32 reference vector");
  {
    std::string p=directory("roundtrip");Meta m=meta();
    {Store s(p.c_str());check(write_all(s,m)==Result::Ok,"complete JPEG and metadata commit");}
    Store rebooted(p.c_str());Meta got{};
    check(rebooted.lookup(m.request_id,m.owner_id,m.device_id,&got)==Result::Ok,"committed image survives store reconstruction");
    check(got.len==m.len&&got.crc32==m.crc32&&same(got,m),"replay retains exact image identity and capture options");
    check(count(rebooted,m)==1,"one committed job advertised after restart");
    char payload[512];check(rebooted.payload_path(m.request_id,payload,sizeof(payload)),"payload path available");
    FILE* f=std::fopen(payload,"rb");char bytes[8]{};size_t n=f?std::fread(bytes,1,sizeof(bytes),f):0;if(f)std::fclose(f);
    check(n==jpeg.size()&&!std::memcmp(bytes,jpeg.data(),jpeg.size()),"replayed JPEG bytes unchanged");
    check(rebooted.begin(m)==Result::AlreadyStored,"lost final ACK does not create another queued copy");
    check(count(rebooted,m)==1,"duplicate offer preserves one record");
    Meta changed=m;std::strcpy(changed.mode,"discard");
    check(rebooted.begin(changed)!=Result::Ok&&rebooted.begin(changed)!=Result::AlreadyStored,"same request cannot replace different metadata");
    check(rebooted.lookup(m.request_id,"other-owner",m.device_id,&got)!=Result::Ok,"new owner cannot retrieve old image");
    check(rebooted.erase(m.request_id,"other-owner",m.device_id,m.len,m.crc32)!=Result::Ok,"new owner cannot delete old image");
    check(rebooted.erase(m.request_id,m.owner_id,"other-device",m.len,m.crc32)!=Result::Ok,"different device cannot delete image");
    check(rebooted.erase(m.request_id,m.owner_id,m.device_id,m.len,m.crc32^1)!=Result::Ok,"wrong receipt checksum cannot delete image");
    check(count(rebooted,m)==1,"rejected receipts retain queued data");
    check(rebooted.erase(m.request_id,m.owner_id,m.device_id,m.len,m.crc32)==Result::Ok,"matching accepted-upload receipt deletes exact job");
    check(count(rebooted,m)==0,"accepted job no longer advertised");
  }
  {
    std::string p=directory("interrupted");Meta m=meta();
    {Store s(p.c_str());check(s.begin(m)==Result::Ok,"begin partial image");check(s.append(0,(const uint8_t*)jpeg.data(),4)==Result::Ok,"write partial payload");}
    Store rebooted(p.c_str());check(count(rebooted,m)==0,"interrupted payload never advertised as ready");
    check(write_all(rebooted,m)==Result::Ok,"same pending request can be retransmitted after restart");
    check(count(rebooted,m)==1,"retransmission produces exactly one committed job");
  }
  {
    std::string p=directory("framing");Meta m=meta();Store s(p.c_str());
    check(s.begin(m)==Result::Ok,"begin framing case");
    check(s.append(1,(const uint8_t*)jpeg.data(),4)!=Result::Ok,"out-of-order first frame refused");
    check(s.append(0,(const uint8_t*)jpeg.data(),4)==Result::Ok,"first proper frame accepted");
    check(s.append(0,(const uint8_t*)jpeg.data(),4)==Result::DuplicateChunk,"lost chunk ACK safely acknowledged again");
    const uint8_t changed[4]={9,9,9,9};
    check(s.append(0,changed,4)!=Result::DuplicateChunk,"same sequence with different bytes is not an acknowledged duplicate");
    check(s.finish(1)!=Result::Ok,"truncated payload cannot commit");
    check(count(s,m)==0,"truncated payload is not replayable");
  }
  {
    std::string p=directory("maximum_image");Meta m=meta();
    std::vector<uint8_t> image(512*1024);for(size_t i=0;i<image.size();++i)image[i]=(uint8_t)((i*31+i/101)&255);
    std::strcpy(m.checksum_sha256,"7f65d5200f02918a2fdff0649b0e10dde8b48b7034a96487122cf3756ca1ae76");m.len=image.size();m.crc32=reference_crc(image.data(),image.size());Store s(p.c_str());
    check(s.begin(m)==Result::Ok,"maximum supported image admitted");
    bool chunks_ok=true;uint16_t seq=0;
    for(size_t off=0;off<image.size();off+=512,++seq){
      chunks_ok&=s.append(seq,image.data()+off,512)==Result::Ok;
      if(seq==42)chunks_ok&=s.append(seq,image.data()+off,512)==Result::DuplicateChunk;
    }
    check(chunks_ok,"all1024 frames survive boundary sequence and duplicate ACK");
    check(s.finish(seq)==Result::Ok,"maximum image committed with whole-file checksum");
    Store rebooted(p.c_str());Meta got{};
    check(rebooted.lookup(m.request_id,m.owner_id,m.device_id,&got)==Result::Ok&&got.len==image.size(),"maximum image validates after restart");
  }
  {
    std::string p=directory("crc");Meta m=meta();m.crc32^=1;Store s(p.c_str());
    check(s.begin(m)==Result::Ok,"declared checksum recorded");
    check(s.append(0,(const uint8_t*)jpeg.data(),jpeg.size())==Result::Ok,"checksum case writes all bytes");
    check(s.finish(1)!=Result::Ok,"whole-payload checksum mismatch cannot commit");
    check(count(s,m)==0,"corrupt upload is not advertised");
  }
  {
    std::string p=directory("corrupt_after_commit");Meta m=meta();Store s(p.c_str());check(write_all(s,m)==Result::Ok,"seed committed payload");
    char path[512];s.payload_path(m.request_id,path,sizeof(path));FILE* f=std::fopen(path,"r+b");if(f){std::fputc(0xff,f);std::fclose(f);}
    Store rebooted(p.c_str());Meta got{};
    check(rebooted.lookup(m.request_id,m.owner_id,m.device_id,&got)!=Result::Ok,"storage corruption detected before replay");
    check(count(rebooted,m)==0,"corrupt stored payload excluded from replay list");
  }
  {
    std::string p=directory("full");Store s(p.c_str(),2);Meta one=meta(1),two=meta(2),three=meta(3);
    check(write_all(s,one)==Result::Ok&&write_all(s,two)==Result::Ok,"fill queue to configured capacity");
    check(s.begin(three)==Result::Full,"full queue explicitly rejects incoming image");
    Meta got{};check(s.lookup(one.request_id,one.owner_id,one.device_id,&got)==Result::Ok&&s.lookup(two.request_id,two.owner_id,two.device_id,&got)==Result::Ok,"full queue never evicts unacknowledged requests");
  }
  {
    std::string p=directory("metadata_marker");Meta m=meta();Store s(p.c_str());check(write_all(s,m)==Result::Ok,"seed commit marker case");
    std::string marker=p+"/"+m.request_id+".meta";std::rename(marker.c_str(),(marker+".part").c_str());
    Store rebooted(p.c_str());check(count(rebooted,m)==0,"data without final metadata marker never advertised");
  }
  for(int mode=0;mode<4;++mode){
    std::string name="io_failure_"+std::to_string(mode);std::string p=directory(name.c_str());Meta m=meta();Store s(p.c_str());
    check(s.begin(m)==Result::Ok,"begin storage failure case");
    if(mode==0)fail_write=true;
    Result appended=s.append(0,(const uint8_t*)jpeg.data(),jpeg.size());
    if(mode==0){check(appended!=Result::Ok,"failed payload write never acknowledged");fail_write=false;}
    else{
      check(appended==Result::Ok,"payload written before commit fault");
      if(mode==1)fail_sync=true;
      if(mode==2)fail_close=true;
      if(mode==3)rename_until_failure=1;
      check(s.finish(1)!=Result::Ok,"flush/close/metadata rename failure never claims committed storage");
      fail_sync=fail_close=false;rename_until_failure=-1;
    }
    Store rebooted(p.c_str());check(count(rebooted,m)==0,"failed commit has no advertised ready image");
    if(mode==3){
      check(rebooted.begin(m)==Result::AlreadyStored,"restart completes matching synced payload when final metadata rename was interrupted");
      check(count(rebooted,m)==1,"recovered metadata commit advertises one exact request");
    }
  }
  {
    std::string p=directory("commit_order");Store s(p.c_str());
    Meta one=meta(99),two=meta(2),three=meta(45),got{};Stats stats{};
    one.epoch=two.epoch=three.epoch=0;
    check(write_all(s,one)==Result::Ok&&write_all(s,two)==Result::Ok&&write_all(s,three)==Result::Ok,"commit offline commands with random identity order");
    check(s.list(one.owner_id,one.device_id,&got,&stats)==Result::Ok&&!std::strcmp(got.request_id,one.request_id),"oldest commit precedes lexicographically smaller request");
    const uint64_t first=got.ordinal;
    check(s.list(one.owner_id,one.device_id,&got,&stats,one.request_id)==Result::Ok&&!std::strcmp(got.request_id,two.request_id)&&got.ordinal>first,"cursor skips held old record while preserving FIFO");
    Store rebooted(p.c_str());
    check(rebooted.list(one.owner_id,one.device_id,&got,&stats,two.request_id)==Result::Ok&&!std::strcmp(got.request_id,three.request_id),"commit order survives restart");
    check(rebooted.list(one.owner_id,one.device_id,&got,&stats,three.request_id)==Result::Empty&&stats.count==3,"end cursor does not wrap and repeat old commands");
    Meta absent=meta(100);check(rebooted.list(one.owner_id,one.device_id,&got,&stats,absent.request_id)==Result::Empty,"unknown cursor cannot silently reorder replay");
    rebooted.set_budget(within_budget);allow_budget=false;
    check(rebooted.list(one.owner_id,one.device_id,&got,&stats)==Result::Io,"exhausted scan budget reports unavailable rather than empty");
    allow_budget=true;check(count(rebooted,one)==3,"scan timeout preserves all queued commands");
  }
  {
    std::string p=directory("attempt_marker");Store s(p.c_str());Meta m=meta();m.epoch=0;Meta got{};
    check(write_all(s,m)==Result::Ok,"offline capture can commit without clock");
    check(s.mark_attempt(m.request_id,m.owner_id,m.device_id,m.len,m.crc32,0,&got)==Result::Invalid,"untrusted time cannot authorize first POST");
    check(s.mark_attempt(m.request_id,"wrong-owner",m.device_id,m.len,m.crc32,1789500000,&got)!=Result::Ok,"first attempt remains owner bound");
    check(s.mark_attempt(m.request_id,m.owner_id,m.device_id,m.len,m.crc32^1,1789500000,&got)!=Result::Ok,"first attempt remains payload bound");
    check(s.mark_attempt(m.request_id,m.owner_id,m.device_id,m.len,m.crc32,1789500000,&got)==Result::Ok&&got.epoch==1789500000,"first upload time durably committed once");
    const uint64_t ordinal=got.ordinal;Store rebooted(p.c_str());
    check(rebooted.lookup(m.request_id,m.owner_id,m.device_id,&got)==Result::Ok&&got.epoch==1789500000&&got.ordinal==ordinal,"first attempt identity and age survive restart");
    check(rebooted.mark_attempt(m.request_id,m.owner_id,m.device_id,m.len,m.crc32,1789500000,&got)==Result::Ok,"lost attempt ACK safely repeats same epoch");
    check(rebooted.mark_attempt(m.request_id,m.owner_id,m.device_id,m.len,m.crc32,1789500001,&got)==Result::Conflict,"retry cannot renew backend dedupe horizon");
    check(rebooted.erase(m.request_id,m.owner_id,m.device_id,m.len,m.crc32)==Result::Ok&&fs::is_empty(p),"accepted upload removes exact image and attempt markers");
  }
  for(int fault=0;fault<2;++fault){
    std::string name="attempt_fault_"+std::to_string(fault);std::string p=directory(name.c_str());
    Store s(p.c_str());Meta m=meta();m.epoch=0;Meta got{};check(write_all(s,m)==Result::Ok,"seed first attempt fault");
    if(fault==0)fail_sync=true;else rename_until_failure=0;
    check(s.mark_attempt(m.request_id,m.owner_id,m.device_id,m.len,m.crc32,1789500000,&got)!=Result::Ok,"interrupted marker never authorizes POST");
    fail_sync=false;rename_until_failure=-1;Store rebooted(p.c_str());
    check(rebooted.lookup(m.request_id,m.owner_id,m.device_id,&got)==Result::Ok&&got.epoch==0,"incomplete attempt leaves original unattempted image");
    check(rebooted.mark_attempt(m.request_id,m.owner_id,m.device_id,m.len,m.crc32,1789500010,&got)==Result::Ok,"never attempted record recovers marker after power interruption");
  }
  {
    std::string p=directory("invalid");Store s(p.c_str());Meta m=meta();
    std::strcpy(m.request_id,"../escape");check(s.begin(m)!=Result::Ok,"path traversal request rejected");
    m=meta();m.camera.flash_enabled=2;check(s.begin(m)!=Result::Ok,"invalid camera flag rejected");
    m=meta();m.schema=2;check(s.begin(m)!=Result::Ok,"unknown image schema rejected");
    m=meta();m.len=0;check(s.begin(m)!=Result::Ok,"empty image rejected");
    m=meta();m.len=512*1024+2;check(s.begin(m)!=Result::Ok,"oversize image rejected");
    m=meta();m.owner_id[0]=0;check(s.begin(m)!=Result::Ok,"missing owner rejected");
    m=meta();m.quantity=0;check(s.begin(m)!=Result::Ok,"zero image quantity rejected");
    m=meta();std::memset(m.mode,'x',sizeof(m.mode));check(s.begin(m)!=Result::Ok,"unterminated image mode rejected");
    m=meta();std::memset(m.request_id,'a',sizeof(m.request_id));check(s.begin(m)!=Result::Ok,"unterminated request ID rejected");
    m=meta();m.retries=256;check(s.begin(m)!=Result::Ok,"invalid retry metadata rejected");
  }
  {
    Meta m=meta();std::strcpy(m.expiry,"2026-10-01");m.camera.profile=3;m.camera.flash_enabled=1;
    m.camera.jpeg_quality=12;m.camera.configured_framesize=8;m.camera.scene_green_ratio=-234;m.camera.xclk_hz=20000000;
    uint8_t encoded[kMetaBytes];encode(m,encoded);Meta got;
    check(decode(encoded,&got)&&same(got,m),"all signed camera/action/owner/SHA fields survive exact metadata codec");
    bool protected_all=true;
    for(size_t i=0;i<sizeof(encoded);++i){encoded[i]^=1;protected_all&=!decode(encoded,&got);encoded[i]^=1;}
    check(protected_all,"every stored metadata byte is checksum protected");
    std::string p=directory("identity_fields");Store s(p.c_str());check(write_all(s,m)==Result::Ok,"seed full image identity");
    for(unsigned field=0;field<6;++field){Meta changed=m;
      if(field==0)changed.quantity++;if(field==1)changed.add_to_shopping_list^=1;
      if(field==2)changed.camera.scene_luma++;if(field==3)std::strcpy(changed.expiry,"2026-10-02");
      if(field==4)changed.checksum_sha256[0]=changed.checksum_sha256[0]=='0'?'1':'0';
      if(field==5)changed.camera.xclk_hz++;
      check(s.begin(changed)==Result::Conflict,"same request cannot overwrite changed capture or backend checksum identity");}
    m=meta();m.checksum_sha256[0]='G';check(!valid(m),"nonhex backend checksum rejected");
    m=meta();m.checksum_sha256[64]='a';check(!valid(m),"unterminated backend checksum rejected");
  }
  {
    std::string p=directory("odd_jpeg");Store s(p.c_str());Meta m=meta();m.len=7;
    m.crc32=reference_crc((const uint8_t*)jpeg.data(),7);
    std::strcpy(m.checksum_sha256,"32bbe378a25091502b2baf9f7258c19444e7a43ee4593b08030acd790bd66e6a");
    check(s.begin(m)==Result::Ok&&s.append(0,(const uint8_t*)jpeg.data(),7)==Result::Ok&&s.finish(1)==Result::Ok,
      "image payloads allow odd byte lengths independently of PCM format");
  }
  std::printf("%s %u production image-store checks (%u failures)\n",failures?"FAIL":"PASS",checks,failures);
  return failures?1:0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    compiler = shutil.which('clang++') or shutil.which('g++')
    if not compiler:
        raise SystemExit('A host C++ compiler is required')
    with tempfile.TemporaryDirectory(prefix='halo-image-store-') as tmp:
        folder = Path(tmp)
        source = folder / 'test.cpp'
        source.write_text(HARNESS)
        binary = folder / 'test'
        subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', '-I', str(args.source_root / 'halo_common'),
                        str(source), '-o', str(binary)], check=True)
        data = folder / 'data'
        data.mkdir()
        subprocess.run([str(binary), str(data)], check=True)


if __name__ == '__main__':
    main()
