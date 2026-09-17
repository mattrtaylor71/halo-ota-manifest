#!/usr/bin/env python3
"""Execute the production pure media retry policy with fault/reboot sequences."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include "halo_common/MediaRetryPolicy.h"
#include <assert.h>
#include <stdio.h>
using namespace halo_media_retry;
int main() {
  for (unsigned p=0;p<8;++p) for(unsigned b=0;b<4;++b) for(unsigned i=0;i<2;++i) {
    State s; s.pending=p; s.backoff=b; s.image_first=i;
    State restored; assert(decode(encode(s),restored));
    assert(restored.pending==p && restored.backoff==b && restored.image_first==bool(i));
    for(unsigned bit=6;bit<32;++bit) {
      State bad; assert(!decode(encode(s)^(1U<<bit),bad));
      assert(bad.pending==kAllStores); // unknown storage is never assumed empty
    }
  }
  State s; assert(!decode(0,s)); assert(s.pending==kAllStores);
  inventory(s,VoiceFlash,false); inventory(s,VoiceSd,false); inventory(s,ImageSd,false);
  assert(interval(s,false)==0);
  saved(s,VoiceSd); saved(s,ImageSd);
  const unsigned delays[]={300,900,3600,21600,21600,21600};
  for(unsigned expected:delays) {
    assert(interval(s,false)==expected);
    // A failed LIST/connection/ACK performs no authoritative inventory clear.
    sleep_committed(s,false);
    State rebooted; assert(decode(encode(s),rebooted)); s=rebooted;
    assert(s.pending==(VoiceSd|ImageSd));
  }
  assert(interval(s,true)==60); sleep_committed(s,true); assert(s.backoff==0);
  for(unsigned wake=0;wake<80;++wake) {
    const auto kind=s.image_first?ImageSd:VoiceSd;
    assert(kind==(wake%2?ImageSd:VoiceSd));
    attempted(s,kind); // even permanently rejected work yields to the other kind
    State rebooted; assert(decode(encode(s),rebooted)); s=rebooted;
  }
  inventory(s,VoiceSd,false); assert(s.pending==ImageSd);
  inventory(s,ImageSd,false); assert(interval(s,false)==0);
  saved(s,VoiceFlash); assert(interval(s,false)==300);
  for(unsigned earlier=1;earlier<22000;earlier+=7) for(unsigned media:delays) {
    const unsigned actual=earlier<media?earlier:media;
    assert(selected(actual,media,earlier)==(media<earlier));
    if(media==earlier)assert(!selected(actual,media,earlier));
    assert(!selected(actual+1,media,earlier)); // SDK/policy selected a different timer
  }
  assert(!selected(300,0,600)); assert(!selected(300,300,300));
  puts("PASS media policy: persisted state/corruption, bounded offline backoff, recovery, fair turns, empty-stop, OTA/safety timer precedence");
}
'''
with tempfile.TemporaryDirectory(prefix="halo-media-retry-") as directory:
    source = Path(directory) / "test.cpp"
    binary = Path(directory) / "test"
    source.write_text(SOURCE)
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-I", str(ROOT), str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
