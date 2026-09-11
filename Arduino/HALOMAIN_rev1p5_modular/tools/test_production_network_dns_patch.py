"""Actual pinned Arduino function regression; temporary files only, no network."""
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location('dns_patch', Path(__file__).with_name('production_network_dns_patch.py'))
fix = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(fix)


class DnsPatchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        data = subprocess.check_output(['arduino-cli', 'config', 'get', 'directories.data'], text=True, timeout=10).strip()
        cls.installed = Path(data) / fix.SDK_RELATIVE
        cls.installed_before = cls.installed.read_bytes()
        cls.original = cls.installed_before
        if fix.sha(cls.original) == fix.PATCHED_SHA:
            for before, after in reversed(fix.EDITS):
                cls.original = cls.original.replace(after, before)
        assert fix.sha(cls.original) == fix.ORIGINAL_SHA

    @classmethod
    def tearDownClass(cls):
        assert cls.installed.read_bytes() == cls.installed_before, 'Test changed installed SDK'

    def test_exact_transform_idempotent_and_unknown_refused(self):
        fixed = fix.corrected(self.original)
        self.assertEqual(fix.sha(fixed), fix.PATCHED_SHA)
        self.assertEqual(fix.corrected(fixed), fixed)
        with self.assertRaises(ValueError):
            fix.corrected(self.original + b'// unrelated change\n')
        restored = fixed
        for before, after in reversed(fix.EDITS):
            restored = restored.replace(after, before)
        self.assertEqual(restored, self.original)

    def test_apply_and_build_binding_on_temporary_sdk_only(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            source = root / fix.SDK_RELATIVE
            source.parent.mkdir(parents=True)
            source.write_bytes(self.original)
            evidence = root / 'patch'
            result = fix.apply(source, evidence)
            self.assertTrue(result['changed'])
            self.assertEqual((evidence / 'NetworkManager.before.cpp').read_bytes(), self.original)
            self.assertFalse(fix.apply(source, root / 'again')['changed'])
            out = root / 'build'
            out.mkdir()
            with patch.object(fix.subprocess, 'check_output', return_value=str(root)):
                prepared = fix.prepare('/synthetic/arduino-cli', out)
            compile_dir = out / 'compile'
            compile_dir.mkdir()
            obj = compile_dir / 'NetworkManager.cpp.o'
            obj.write_bytes(b'synthetic object: not physical compiler proof')
            commands = compile_dir / 'compile_commands.json'
            row = {'directory': str(root), 'file': str(source), 'arguments': ['c++', '-c', str(source), '-o', str(obj)]}
            commands.write_text(json.dumps([row]))
            bound = fix.verify_compiled(prepared, compile_dir)
            self.assertEqual(bound['source'], fix.ref(source))
            source.write_bytes(self.original)
            with self.assertRaisesRegex(ValueError, 'changed during compilation'):
                fix.verify_compiled(prepared, compile_dir)
            with patch.object(fix.subprocess, 'check_output', return_value=str(root)), self.assertRaisesRegex(ValueError, '--apply first'):
                fix.prepare('/synthetic/arduino-cli', out)

    def test_wrong_compiler_source_refused(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            source = root / 'NetworkManager.cpp'
            source.write_bytes(fix.corrected(self.original))
            (root / 'compile_commands.json').write_text(json.dumps([{'file': str(root / 'other/NetworkManager.cpp')}]))
            with self.assertRaisesRegex(ValueError, 'different NetworkManager'):
                fix.verify_compiled({'source': fix.ref(source)}, root)

    def test_actual_hostbyname_cache_locked_and_blocking_dns_unlocked(self):
        prefix = r'''
#include <cstdint>
#include <cstring>
#include <string>
#include <stdexcept>
#include <netdb.h>
#include <netinet/in.h>
#define CONFIG_LWIP_IPV6 1
#define log_d(...) do{}while(0)
#define log_e(...) do{}while(0)
using err_t=int; using Network_Interface_ID=int; constexpr int ERR_OK=0, ESP_NETIF_ID_MAX=1, IPv6=6;
static int depth=0, clears=0, queries=0; static bool v4=true,v6=false,fail=false;
static void check(bool b){if(!b)throw std::runtime_error("DNS core lock contract");}
#define LOCK_TCPIP_CORE() do{check(depth==0);++depth;}while(0)
#define UNLOCK_TCPIP_CORE() do{check(depth==1);--depth;}while(0)
void dns_clear_cache(){check(depth==1);++clears;}
struct IPAddress {IPAddress(uint32_t=0){} IPAddress(int,const uint8_t*){} bool fromString(const char*s){return !strcmp(s,"192.0.2.1");} std::string toString(){return "192.0.2.1";}};
struct NetworkInterface {bool hasGlobalIPv6(){return v6;}bool hasIP(){return v4;}};
NetworkInterface*getNetifByID(int){static NetworkInterface iface;return &iface;}
int lwip_getaddrinfo(const char*,const char*,const addrinfo*h,addrinfo**out){
 check(depth==0);++queries;if(fail)return EAI_FAIL;
 static addrinfo r{};static sockaddr_in a{};static sockaddr_in6 a6{};
 r.ai_family=h->ai_family==AF_INET6?AF_INET6:AF_INET;r.ai_addr=r.ai_family==AF_INET6?reinterpret_cast<sockaddr*>(&a6):reinterpret_cast<sockaddr*>(&a);*out=&r;return 0;
}
void lwip_freeaddrinfo(addrinfo*){check(depth==0);}
struct NetworkManager {int hostByName(const char*,IPAddress&);};
'''
        suffix = r'''
int main(){try{
 NetworkManager n;IPAddress ip;
 check(n.hostByName("list.example",ip)==1 && clears==1 && queries==1);
 check(n.hostByName("list.example",ip)==1 && clears==1 && queries==2);
 v4=false;v6=true;check(n.hostByName("list.example",ip)==1 && clears==2 && queries==3);
 int q=queries;check(n.hostByName("192.0.2.1",ip)==1 && queries==q && clears==2);
 fail=true;check(n.hostByName("missing.example",ip)!=1 && depth==0);
 return 0;}catch(...){return 1;}}
'''
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            for label, raw, expected in [('original', self.original, 1), ('patched', fix.corrected(self.original), 0)]:
                text = raw.decode()
                function = text[text.index('int NetworkManager::hostByName('):text.index('\nuint8_t *NetworkManager::macAddress')]
                cpp = root / (label + '.cpp')
                cpp.write_text(prefix + function + suffix)
                binary = root / label
                compiled = subprocess.run([shutil.which('c++'), '-std=c++17', str(cpp), '-o', str(binary)], capture_output=True, text=True, timeout=20)
                self.assertEqual(compiled.returncode, 0, compiled.stderr)
                self.assertEqual(subprocess.run([str(binary)], timeout=5).returncode, expected, label)


if __name__ == '__main__':
    unittest.main()
