#!/usr/bin/env python3
"""Execute the real fixed-size Home power estimator with ASan/UBSan."""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CODE = r'''
#include "halo_ota_demo/firmware/shared/HomeBattery.h"
#include <cassert>
#include <cstdio>
#include <cstring>
using namespace home_battery;
static halo_power::View sample(unsigned mv, unsigned sequence=1, unsigned boot=1) {
  halo_power::View v; v.received=true; v.age_known=true;
  auto& s=v.sample;s.status=halo_power::Status::Ok;s.boot_id=boot;
  s.sequence=sequence;s.uptime_ms=sequence*1000ULL;s.samples=32;
  s.raw_min=1000;s.raw_max=3000;s.system_supply_mv=mv;return v;
}
int main() {
  unsigned assertions=0;
  #define CHECK(x) do{assert(x);++assertions;}while(0)
  CHECK(band(100)==Band::Green);CHECK(band(26)==Band::Green);
  CHECK(band(25)==Band::Yellow);CHECK(band(11)==Band::Yellow);
  CHECK(band(10)==Band::Red);CHECK(band(0)==Band::Red);
  unsigned last=0;
  for(unsigned mv=0;mv<=6600;++mv) {
    auto pct=estimate_percent(mv);CHECK(pct<=100);CHECK(pct>=last);last=pct;
  }
  CHECK(estimate_percent(3300)==0);CHECK(estimate_percent(3600)==10);
  CHECK(estimate_percent(3700)==25);CHECK(estimate_percent(4200)==100);
  CHECK(estimate_percent(4100)==97);
  CHECK(estimate_percent(4149)==99);CHECK(estimate_percent(4150)==100);
  CHECK(estimate_percent(4151)==100);CHECK(estimate_percent(4160)==100);
  for(unsigned mv=3300;mv<4150;++mv)CHECK(estimate_percent(mv)<100);
  for(unsigned mv=4150;mv<=4250;++mv)CHECK(estimate_percent(mv)==100);
  Model m;
  CHECK(m.update({},false).mode==Mode::Unknown);
  auto v=sample(3800);
  CHECK(m.update(v,false).percent==50);
  const auto unchanged=m.update(v,false);
  for(unsigned i=0;i<10000;++i) CHECK(m.update(v,false)==unchanged);
  // Missing, invalid and stale input cannot manufacture battery or USB status.
  v.age_ms=5001;CHECK(m.update(v,false).mode==Mode::Unknown);
  v.age_ms=5000;CHECK(m.update(v,false).percent==50);
  v.age_known=false;CHECK(m.update(v,false).mode==Mode::Unknown);
  v.age_known=true;v.sample.samples=31;CHECK(m.update(v,false).mode==Mode::Unknown);
  v=sample(5000);v.sample.status=halo_power::Status::ReadError;
  CHECK(m.update(v,true).mode==Mode::Unknown);
  CHECK(m.update(sample(0),false).mode==Mode::Unknown);
  CHECK(m.update(sample(6000),true).mode==Mode::Unknown);
  // External threshold uses actual fresh rail; USB data alone is never enough.
  CHECK(m.update(sample(3056),true).mode==Mode::Unknown);
  CHECK(m.update(sample(3056),false).mode==Mode::Unknown);
  CHECK(m.update(sample(4200,2),true).mode==Mode::Unknown);
  CHECK(m.update(sample(4399,3),false).mode==Mode::Unknown);
  CHECK(m.update(sample(4400,4),false).mode==Mode::ExternalPower);
  CHECK(m.update(sample(4301,5),false).mode==Mode::ExternalPower);
  CHECK(m.update(sample(4300,6),false).mode==Mode::Unknown);
  CHECK(m.update(sample(4646,7),true).mode==Mode::ExternalPower);
  CHECK(m.update(sample(3800,8),false).percent==50);
  // A source change must discard the old voltage filter immediately.
  CHECK(m.update(sample(3300,9),true).mode==Mode::Unknown);
  CHECK(m.update(sample(3600,10),false).percent==10);
  // Same boot's older sequence or regressing time cannot update the display.
  CHECK(m.update(sample(4200,9),false).mode==Mode::Unknown);
  v=sample(4200,11);v.sample.uptime_ms=1;
  CHECK(m.update(v,false).mode==Mode::Unknown);
  CHECK(m.update(sample(3600,10),false).percent==10);
  // Boot or >5s gap starts afresh, without taking many UI redraws to catch up.
  CHECK(m.update(sample(4200,1,2),false).percent==100);
  CHECK(m.update(sample(4150,1,3),false).percent==100);
  CHECK(m.update(sample(4149,1,4),false).percent==99);
  CHECK(m.update(sample(3700,20,2),false).percent==25);
  m.reset();CHECK(m.update(sample(3900),false).percent==75);
  // A single noisy low sample is median filtered; sustained change converges.
  for(unsigned q=2;q<7;++q)CHECK(m.update(sample(3900,q),false).percent==75);
  CHECK(m.update(sample(3300,7),false).percent==75);
  for(unsigned q=8;q<130;++q)m.update(sample(3600,q),false);
  CHECK(m.update(sample(3600,130),false).percent==10);
  CHECK(m.update(sample(3600,130),false).band==Band::Red);
  // Stale external power must clear the bolt, not preserve hysteresis forever.
  CHECK(m.update(sample(4600,131),false).mode==Mode::ExternalPower);
  v=sample(4600,131);v.age_ms=5001;CHECK(m.update(v,false).mode==Mode::Unknown);
  CHECK(m.update(sample(4350,132),false).mode==Mode::Unknown);
  // Presentation never modifies the cloud's honest system-supply semantics.
  char json[halo_power::kJsonCapacity];CHECK(halo_power::json(sample(3900),json,sizeof(json)));
  CHECK(strstr(json,"\"battery_percent\":null")!=nullptr);
  CHECK(strstr(json,"\"power_source\":\"unknown\"")!=nullptr);
  printf("PASS %u assertions; Model=%zu B; estimated display only, no physical SOC calibration\n",assertions,sizeof(Model));
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix="halo-home-battery-") as name:
        out = Path(name)
        (out / "test.cpp").write_text(CODE)
        compiler = shutil.which("clang++")
        assert compiler, "clang++ required"
        subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-I" + str(ROOT),
                        str(out / "test.cpp"), "-o", str(out / "test")], check=True)
        subprocess.run([str(out / "test")], check=True, timeout=20)


if __name__ == "__main__":
    main()
