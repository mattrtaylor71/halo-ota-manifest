#!/usr/bin/env python3
"""Build the production OTA path with declared shipping limits and fixed board settings."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import time

BASE_FLAGS = '-DARDUINO_HOST_OS="{runtime.os}" -DARDUINO_FQBN="{build.fqbn}" -DESP32=ESP32 -DCORE_DEBUG_LEVEL={build.code_debug} {build.loop_core} {build.event_core} {build.defines} {build.extra_flags.{build.mcu}} {build.zigbee_mode}'
FQBNS = {
    'sense': 'esp32:esp32:XIAO_ESP32S3:PSRAM=opi,USBMode=hwcdc,CDCOnBoot=default',
    'lcd': 'esp32:esp32:esp32s3:PartitionScheme=custom,FlashSize=8M,USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi',
}
PARTITIONS = {
    'sense': 'ab86e9d1252698d0ac9740404aa7ec803cbb3813730138052fd25ec356593a6b',
    'lcd': '549aeb5a3a1fec5a7c2448cabe1fab5049e0eb088d1ac67f3eefb0258126b95d',
}
ADMISSION_URL = 'https://r2uuurb7qj.execute-api.us-east-1.amazonaws.com/v1/private/ota/admission'
CANARY_PREFIX = 'halo/ota/canary/production-release-20260909'


def shipping_flags(board, private_canary=False):
    """The qualified M8 shipping profile, shared across every translation unit."""
    flags = BASE_FLAGS + ' -fstack-usage -DHALO_DURABLE_DIAGNOSTICS=1'
    if board == 'sense':
        flags += (' -DHALO_DURABLE_OTA_POLICY=1 -DHALO_DIAGNOSTIC_ADMISSION=1'
                  ' -DHALO_LCD_SLEEP_WITNESS=1 -DHALO_IDLE_NETWORK_RECOVERY=1'
                  ' -DHALO_OTA_BENCH_PROFILE=0 -DHALO_OTA_ONE_SHOT=0'
                  ' -DHALO_DIAG_AUTH_PROVISIONING=0 -DHALO_IDLE_NETWORK_PROBE=0'
                  ' -DHALO_DIAG_B1_URL="' + ADMISSION_URL + '"')
    elif board == 'lcd':
        flags += (' -DHALO_LCD_SLEEP_WITNESS=1 -DHALO_OTA_ONE_SHOT=0'
                  ' -DHALO_OTA_BENCH_PROFILE=0 -DHALO_DIAG_AUTH_PROVISIONING=0'
                  ' -DHALO_UI_REVIEW=1 -DLAYOUT_AUDIT=1')
    else:
        raise ValueError('Unknown production board')
    if private_canary:
        flags += (' -DOTA_CHANNEL_ENABLED -DOTA_CHANNEL="dev"'
                  ' -DOTA_S3_BUCKET="halo-ota-dev" -DOTA_S3_REGION="us-east-1"'
                  ' -DOTA_S3_PREFIX="' + CANARY_PREFIX + '"')
    else:
        # Ordinary report metadata must agree with the unchanged default-prod
        # resolver. CHANNEL_ENABLED remains absent in the production profile.
        flags += ' -DOTA_CHANNEL="prod"'
    return flags


def command(board, source, build, compiler, private_canary=False):
    source, build = Path(source).resolve(), Path(build).resolve()
    sketch = 'halo_ota_demo/firmware/halo_%s_prod/halo_%s_prod.ino' % (board, board)
    table = source / Path(sketch).parent / 'partitions.csv'
    assert hashlib.sha256(table.read_bytes()).hexdigest() == PARTITIONS[board], 'Partition layout changed; review the production configuration'
    flags = shipping_flags(board)
    assert all(token not in flags for token in ('OTA_S3_', 'OTA_CHANNEL_ENABLED', 'RETENTION_FIXTURE', 'RETIRE_SNAPSHOT', 'SPOOL_ENABLED=0', 'SPOOL_DRAIN_WAKE_ENABLED=0'))
    flags = shipping_flags(board, private_canary)
    for relative in ('halo_ota_demo/firmware/shared/MqttSecrets.local.h',
                     'halo_ota_demo/firmware/halo_sense_prod/MqttSecrets.local.h',
                     'halo_ota_demo/firmware/halo_sense_prod/MqttSecrets.local.cpp'):
        assert not (source / relative).exists(), 'Production build must use tracked disabled-MQTT defaults, not local credentials'
    return [str(compiler), 'compile', '--fqbn', FQBNS[board], '--build-path', str(build), '--build-property', 'build.extra_flags=' + flags, sketch, '--jobs', '2']


def save(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + '\n')


def close_owned_group(child):
    """Close only the dedicated compiler group, including descendants left by an exited parent."""
    def present():
        child.poll()
        try:
            os.killpg(child.pid, 0)
            return True
        except ProcessLookupError:
            return False

    actions = []
    if not present():
        return {'group_absent': True, 'signals': actions}
    for sig in (signal.SIGTERM, signal.SIGKILL):
        try:
            os.killpg(child.pid, sig)
            actions.append({'signal': int(sig), 'epoch': time.time()})
        except ProcessLookupError:
            pass
        deadline = time.monotonic() + 5
        while present() and time.monotonic() < deadline:
            time.sleep(0.05)
        if not present():
            break
    child.poll()
    return {'group_absent': not present(), 'signals': actions}


def min_free_gib(value):
    """Keep an explicit, bounded host reserve; the normal admission stays 8 GiB."""
    try:
        value = int(value)
    except (TypeError, ValueError):
        raise argparse.ArgumentTypeError("Minimum free space must be an integer GiB value")
    if not 4 <= value <= 1024:
        raise argparse.ArgumentTypeError("Minimum free space must be between 4 and 1024 GiB")
    return value


def run(board, source, out, compiler, private_canary=False, min_free_gib=8):
    out.mkdir(parents=True, exist_ok=False)
    space = {'path': str(out), 'minimum_free_gib': min_free_gib,
             'minimum_free_bytes': min_free_gib * 1024 ** 3,
             'before_free_bytes': shutil.disk_usage(out).free, 'after_free_bytes': None}
    space['admitted'] = space['before_free_bytes'] >= space['minimum_free_bytes']
    save(out / 'disk-space.json', space)
    assert space['admitted'], 'Free disk space is below the configured host build reserve'
    argv = command(board, source, out / 'compile', compiler, private_canary)
    save(out / 'command.json', {'argv': argv, 'cwd': str(source), 'profile': 'shipping', 'route_profile': 'private-canary' if private_canary else 'production', 'policy': board == 'sense', 'diagnostics': True, 'one_shot': False, 'bench_profile': False, 'private_route_override': private_canary, 'idle_network_recovery': board == 'sense', 'diagnostic_admission': board == 'sense', 'auth_provisioning': False, 'idle_network_probe': False, 'admission_endpoint': ADMISSION_URL if board == 'sense' else None})
    started = time.time()
    child = None
    error = None
    with (out / 'compile.log').open('xb') as log:
        try:
            mask = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGTERM, signal.SIGINT})
            try:
                child = subprocess.Popen(argv, cwd=source, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
                save(out / 'started.json', {'pid': child.pid, 'owner_pid': os.getpid(), 'epoch': started, 'timeout_s': 600, 'cleanup_s': 10})
            finally:
                signal.pthread_sigmask(signal.SIG_SETMASK, mask)
            print(json.dumps({'board': board, 'compiler_pid': child.pid, 'owner_pid': os.getpid(), 'out': str(out)}), flush=True)
            child.wait(timeout=600)
        except BaseException as exc:
            error = repr(exc)
        finally:
            previous = {sig: signal.signal(sig, signal.SIG_IGN) for sig in (signal.SIGTERM, signal.SIGINT)}
            try:
                cleanup = close_owned_group(child) if child is not None else {'group_absent': True, 'signals': []}
                result = {'board': board, 'exit_code': child.poll() if child else None, 'reaped': child is not None and child.poll() is not None, 'group_absent': cleanup['group_absent'], 'cleanup_signals': cleanup['signals'], 'error': error, 'elapsed_s': time.time() - started, 'finished_epoch': time.time()}
            finally:
                for sig, handler in previous.items():
                    signal.signal(sig, handler)
            save(out / 'result.json', result)
            space['after_free_bytes'] = shutil.disk_usage(out).free
            save(out / 'disk-space.json', space)
    assert result['reaped'] and result['group_absent'] and result['exit_code'] == 0 and not error, result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--board', choices=('sense', 'lcd', 'both'), default='both')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--plan', action='store_true', help='Print the exact commands without compiling')
    parser.add_argument('--private-canary', action='store_true', help='Use the fixed private canary manifest route; all shipping policy and disabled test features remain unchanged')
    parser.add_argument('--min-free-gib', type=min_free_gib, default=8, help='Host free-space reserve checked before each board (default: 8 GiB; minimum: 4 GiB)')
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1]
    compiler = shutil.which('arduino-cli')
    assert compiler, 'arduino-cli is required'
    compiler = str(Path(compiler).resolve())
    boards = ('sense', 'lcd') if args.board == 'both' else (args.board,)
    if args.plan:
        print(json.dumps({b: command(b, source, args.out.resolve() / b / 'compile', compiler, args.private_canary) for b in boards}, indent=2))
        return
    def interrupted(sig, frame):
        raise InterruptedError('Production build interrupted')
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    for board in boards:
        run(board, source, args.out.resolve() / board, compiler, args.private_canary, args.min_free_gib)


if __name__ == '__main__':
    main()
