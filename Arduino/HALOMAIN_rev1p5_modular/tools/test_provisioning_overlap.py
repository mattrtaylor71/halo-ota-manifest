#!/usr/bin/env python3
"""Native deterministic scheduler-interleaving regression, no firmware/network I/O.

Compiles the actual selected ProvisioningState accessors and actual Arduino
Preferences begin/end/string/bool/write methods, with a fake underlying NVS.
A nested callback at nvs_get_u8 models a worker preempting the main task while
isProvisioned holds its Preferences handle. It is deterministic, not a timing
stress test. List tests execute the real pre-request owner-admission slice.
"""
import argparse
from pathlib import Path
import hashlib
import json
import re
import subprocess
import shutil
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PREFS = Path.home() / 'Library/Arduino15/packages/esp32/hardware/esp32/3.3.8/libraries/Preferences/src/Preferences.cpp'


def definition(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        if text.startswith('//',end):
            end=text.index('\n',end);continue
        if text.startswith('/*',end):
            end=text.index('*/',end)+2;continue
        if text[end] in ('"',"'"):
            quote=text[end];end+=1
            while text[end]!=quote:
                end+=2 if text[end]=='\\' else 1
            end+=1;continue
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


PREFIX = r'''
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <utility>
#define LOG_ERROR(...) ((void)0)
#define LOG_INFO(...) ((void)0)
#define log_e(...) ((void)0)
#define log_v(...) ((void)0)
#define NVS_NAMESPACE "halo_prov"
#define KEY_OWNER_ID "owner_id"
#define KEY_OWNER_CODE "owner_code"
#define KEY_PROVISIONED "prov"
using esp_err_t=int;
constexpr int ESP_OK=0,NVS_READONLY=1,NVS_READWRITE=0;
static std::map<uint32_t,bool> handles;
static std::map<std::string,std::string> values;
static uint32_t next_handle=0;
static unsigned writes=0,open_failures=0;
static bool fail_open=false;
static std::function<void()> during_get_bool;
static int nvs_flash_init_partition(const char*){return 0;}
static int nvs_open(const char*,int mode,uint32_t* h){
  if(fail_open){++open_failures;return 1;}
  *h=++next_handle;handles[*h]=mode==NVS_READONLY;return 0;
}
static int nvs_open_from_partition(const char*,const char* ns,int m,uint32_t*h){return nvs_open(ns,m,h);}
static void nvs_close(uint32_t h){handles.erase(h);}
static int nvs_get_str(uint32_t h,const char*k,char*out,size_t*n){
  if(!handles.count(h)||!values.count(k))return 1;
  const auto &v=values.at(k);
  if(!out){*n=v.size()+1;return 0;}
  if(*n<v.size()+1)return 1;
  std::memcpy(out,v.c_str(),v.size()+1);*n=v.size()+1;return 0;
}
static int nvs_get_u8(uint32_t h,const char*k,uint8_t*out){
  if(during_get_bool){auto cb=std::move(during_get_bool);during_get_bool={};cb();}
  if(!handles.count(h)||!values.count(k))return 1;
  *out=values.at(k)=="1"?1:0;return 0;
}
static int nvs_set_str(uint32_t h,const char*k,const char*v){
  if(!handles.count(h)||handles.at(h))return 1;
  values[k]=v;++writes;return 0;
}
static int nvs_erase_key(uint32_t h,const char*k){
  if(!handles.count(h)||handles.at(h))return 1;
  values.erase(k);++writes;return 0;
}
static int nvs_commit(uint32_t h){return handles.count(h)?0:1;}
class Preferences {
  uint32_t _handle=0;bool _started=false,_readOnly=false;
public:
  ~Preferences(){end();}
  bool begin(const char*,bool=false,const char*=nullptr);
  void end();
  size_t getString(const char*,char*,size_t);
  uint8_t getUChar(const char*,uint8_t=0);
  bool getBool(const char*,bool=false);
  size_t putString(const char*,const char*);
  bool remove(const char*);
};
struct ProvisioningState {
 static bool isProvisioned();
 static bool loadOwnerId(char*,size_t);
 static bool loadOwnerCode(char*,size_t);
 static void saveOwnerId(const char*);
 static void clearOwnerCode();
};
static struct {
 template<class...A>void printf(const char*,A...){}
 void println(const char*){}
} Serial;
namespace sense_action_summary {struct ListAttempt{};}
// Owner/NVS boundary only; actual transport admission is covered separately.
struct SenseListTransportLease {explicit SenseListTransportLease(const char*){} explicit operator bool()const{return true;}};
struct SenseBackupWifiCall {explicit operator bool()const{return true;}};
static bool list_ensure_wifi_ready(){return true;}
static std::string list_failure,delete_failure;
static void list_refresh_fail(const char*r){list_failure=r;}
static void shopping_list_delete_failed(const char*,const char*r){delete_failure=r;}
'''.replace('const char*=nullptr', 'const char* =nullptr')

TESTS = r'''
static int failed=0;
static const char* owner="00000000-0000-4000-8000-000000000001";
static void reset(){
  values={{"prov","1"},{"owner_id",owner}};writes=0;fail_open=false;
  list_failure.clear();delete_failure.clear();during_get_bool={};
}
static void check(const char*name,bool ok){
  std::printf("{\"test\":\"%s\",\"pass\":%s}\n",name,ok?"true":"false");
  if(!ok)++failed;
}
int main(){
  char got[64]{};
  reset();load_owner_id_or_default(got,sizeof(got));
  check("warm_owner_read",std::strcmp(got,owner)==0&&handles.empty()&&writes==0);
  reset();got[0]=0;bool worker_ok=false;
  during_get_bool=[&]{load_owner_id_or_default(got,sizeof(got));worker_ok=std::strcmp(got,owner)==0;};
  bool provisioned=ProvisioningState::isProvisioned();
  check("overlap_owner_read_during_provisioned_read",provisioned&&worker_ok&&handles.empty()&&writes==0);
  reset();bool nested_write=false;
  during_get_bool=[&]{ProvisioningState::saveOwnerId("new-owner");nested_write=values.at("owner_id")=="new-owner";};
  provisioned=ProvisioningState::isProvisioned();
  check("overlap_owner_write_during_read",provisioned&&nested_write&&handles.empty()&&writes==1);
  reset();values.erase("owner_id");values["owner_code"]="pending-test";
  bool view=production_view_admission(),remove=production_delete_admission();
  check("pending_claim_never_admits_network",!view&&!remove&&list_failure=="owner_unavailable"&&delete_failure=="owner_unavailable"&&writes==0&&handles.empty());
  reset();fail_open=true;view=production_view_admission();remove=production_delete_admission();
  check("nvs_open_failure_never_admits_network",!view&&!remove&&list_failure=="owner_unavailable"&&delete_failure=="owner_unavailable"&&writes==0&&handles.empty());
  reset();values["owner_code"]="stale-test";load_owner_id_or_default(got,sizeof(got));
  check("stale_claim_clearing_preserved",std::strcmp(got,owner)==0&&!values.count("owner_code")&&writes==1&&handles.empty());
  reset();view=production_view_admission();remove=production_delete_admission();
  check("healthy_owner_still_admits_view_and_delete",view&&remove&&list_failure.empty()&&delete_failure.empty()&&writes==0&&handles.empty());
  return failed?1:0;
}
'''


def harness(source_root, preferences_source):
    provisioning=(source_root/'halo_ota_demo/firmware/shared/ProvisioningState.cpp').read_text()
    shopping=(source_root/'Sense_Minimal/sense_list.h').read_text()
    original_preferences=preferences_source.read_text()
    functions=[definition(original_preferences,s) for s in [
        'bool Preferences::begin(', 'void Preferences::end(',
        'size_t Preferences::getString(const char *key, char *value,',
        'uint8_t Preferences::getUChar(', 'bool Preferences::getBool(',
        'size_t Preferences::putString(const char *key, const char *value)',
        'bool Preferences::remove(',
    ]]
    if 'static Preferences prefs;' in provisioning:functions.append('static Preferences prefs;')
    functions += [definition(provisioning,s) for s in [
        'bool ProvisioningState::isProvisioned(',
        'bool ProvisioningState::loadOwnerId(',
        'bool ProvisioningState::loadOwnerCode(',
        'void ProvisioningState::saveOwnerId(',
        'void ProvisioningState::clearOwnerCode(',
    ]]
    functions.append(definition((source_root/'Sense_Minimal/Sense_Minimal.ino').read_text(), 'static void load_owner_id_or_default('))
    fetch=definition(shopping, 'static ListRequestResult fetch_shopping_list_from_api()' if 'static ListRequestResult fetch_shopping_list_from_api()' in shopping else 'static bool fetch_shopping_list_from_api()')
    preamble=fetch[fetch.index('{')+1:fetch.index('  String request_body =')]
    preamble=preamble.replace('return ListRequestResult::Failed;', 'return false;').replace('return ListRequestResult::Deferred;', 'return false;')
    functions.append('static bool production_view_admission(){'+preamble+'\nreturn true;\n}')
    deletion=definition(shopping, 'static ListRequestResult delete_item_from_api(' if 'static ListRequestResult delete_item_from_api(' in shopping else 'static void delete_item_from_api(')
    start=deletion.index('  char owner_id[64]')
    preamble=deletion[start:deletion.index('  String request_body =',start)]
    # Same owner boundary; void fail-return becomes false in this admission probe.
    preamble=preamble.replace('return;', 'return false;').replace('return ListRequestResult::Failed;', 'return false;')
    functions.append('static bool production_delete_admission(){const char*item_id="fixture-item";\n'+preamble+'\nreturn true;\n}')
    return PREFIX+'\n'+'\n'.join(functions)+'\n'+TESTS


def run_case(source_root, preferences_source, compiler, output_dir, label):
    source = output_dir / (label + '.cpp')
    source.write_text(harness(source_root, preferences_source))
    binary = output_dir / label
    subprocess.run([compiler, '-std=c++17', '-O0', '-Wall', '-Wextra',
                    '-Wno-unused-function', '-Wno-unused-variable',
                    str(source), '-o', str(binary)], check=True, timeout=30)
    run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
    rows = [json.loads(line) for line in run.stdout.splitlines()]
    if run.stderr:
        print(run.stderr, end='')
    if len(rows) != 7 or any(type(row.get('pass')) is not bool for row in rows):
        raise RuntimeError('Missing or malformed native test results')
    relative_sources = ('halo_ota_demo/firmware/shared/ProvisioningState.cpp',
                        'Sense_Minimal/sense_list.h', 'Sense_Minimal/Sense_Minimal.ino')
    return {'case': label, 'exit_code': run.returncode, 'tests': rows,
            'source_root': str(source_root),
            'source_sha256': {name: hashlib.sha256((source_root / name).read_bytes()).hexdigest()
                              for name in relative_sources},
            'harness_sha256': hashlib.sha256(source.read_bytes()).hexdigest()}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source-root', type=Path, default=ROOT)
    ap.add_argument('--preferences-source', type=Path, default=PREFS)
    ap.add_argument('--baseline-root', type=Path,
                    help='Optional original source snapshot; require the four known regression failures')
    ap.add_argument('--output', type=Path, help='Optional JSON receipt path')
    args = ap.parse_args()
    compiler = shutil.which('clang++') or shutil.which('g++')
    if not compiler or not args.preferences_source.is_file():
        ap.error('A C++ compiler and installed Arduino Preferences.cpp are required')
    results = []
    with tempfile.TemporaryDirectory(prefix='halo-provisioning-overlap-') as directory:
        output_dir = Path(directory)
        if args.baseline_root:
            results.append(run_case(args.baseline_root, args.preferences_source,
                                    compiler, output_dir, 'baseline'))
        results.append(run_case(args.source_root, args.preferences_source,
                                compiler, output_dir, 'current'))
    current = results[-1]
    passed = current['exit_code'] == 0 and all(row['pass'] for row in current['tests'])
    if args.baseline_root:
        baseline = results[0]
        expected = {'overlap_owner_read_during_provisioned_read',
                    'overlap_owner_write_during_read',
                    'pending_claim_never_admits_network',
                    'nvs_open_failure_never_admits_network'}
        observed = {row['test'] for row in baseline['tests'] if not row['pass']}
        passed = passed and baseline['exit_code'] == 1 and observed == expected
    receipt = {'status': 'PASS' if passed else 'FAIL',
               'scope': 'Host-only actual provisioning and Arduino Preferences methods with fake NVS; actual shopping pre-request owner-admission slices. No firmware build, hardware, or network.',
               'preferences_source': str(args.preferences_source),
               'preferences_sha256': hashlib.sha256(args.preferences_source.read_bytes()).hexdigest(),
               'results': results}
    if args.output:
        args.output.write_text(json.dumps(receipt, indent=2) + '\n')
    for result in results:
        for row in result['tests']:
            expected_old_failure = result['case'] == 'baseline' and not row['pass']
            label = 'REPRODUCED' if expected_old_failure else 'PASS' if row['pass'] else 'FAIL'
            print(label + ' ' + result['case'] + ': ' + row['test'])
    print(receipt['status'] + ' provisioning overlap and shopping owner admission')
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
