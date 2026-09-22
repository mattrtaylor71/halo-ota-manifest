# Private bench candidate installation

Use this after the local build and artifact checks in
[Development and release](DEVELOPMENT_AND_RELEASE.md). USB installation needs no
publication. Production remains at the frozen release until a separate, explicit
release decision; this procedure never stages objects or changes latest manifests.

The existing service adapters supply the appropriate preservation and ownership
checks. Adapt their artifact and current-device bindings for each case; retain
their reviewed physical executor. Do not replace them with a generic app-only
`esptool write-flash` command or another flash implementation.

## Existing implementation to reuse

These are local, historical tools, not portable or ready-to-run installers for
the current device. Preserve the originals and create the reviewed adaptation in
a new private candidate workspace outside the checkout.

| Role | Existing path |
| --- | --- |
| Sense adapter | `/Users/MattTaylor/halo-ota208-20260922/service208-prep/service208.py` |
| Sense controller | `/Users/MattTaylor/halo-ota208-20260922/service208-prep/run_service208.py` |
| LCD adapter | `/Users/MattTaylor/halo-provision-memory202-20260921/service-lcd206-prep/service_lcd206.py` |
| LCD controller | `/Users/MattTaylor/halo-provision-memory202-20260921/service-lcd206-prep/run_service_lcd206.py` |
| Shared preservation adapter | `/Users/MattTaylor/halo-postclaim199-20260921/usb-service-prep/service199.py` |
| Sense physical executor | `/Users/MattTaylor/halo-manual195-20260918/tools/install_sense195_only.py` |
| LCD physical executor | `/Users/MattTaylor/halo-delete191-20260918/tools/install_lcd191_only.py` |

Reviewed SHA256 pins, in the same order:

```text
95b46d78535322c0bdec8bcf3158d9550d862c4ee42b5b4fa1270ba19fa4bf54  service208.py
20d6475f118b0402dbed09862abf5a9dae59b3da8156c087cd68608cbb8e0063  run_service208.py
eeba9cfd2d077e2d84ef95f2c61f7553ffc45638f244a3eca02a803fe312a7b1  service_lcd206.py
1dc6a26b410129492dfa877d922fbd0afb0f6a081bbf2226a0964e7b3160cc29  run_service_lcd206.py
b193b393b0c8fb91af8eb90d246e87d96daf80a187a347ce9a55bd8f75f09e05  service199.py
6a65d97441bba6ef5f9bffc611797a8169ade1f7e98ab7696663ddc145d4885f  install_sense195_only.py
fbd978127d61f03ea11e7f0e8610fa78ffefe5d27926bfe819aa621ccdf37fe1  install_lcd191_only.py
```

`service199.load_base()` verifies the historical executor, its helper closure and
runtime manifest. It adds the existing filesystem checksum barriers at three
checked anchors. Keep this mechanism and its refusal on changed dependencies;
do not patch around a hash mismatch. The service entry requires the pinned,
nonoptimized Python3.12 environment, not an arbitrary system Python.

## Minimal adaptation and admission

1. Start from the Sense208 adapter for Sense, or LCD206 adapter for LCD. The former
   is hard-bound to Sense207/app1 plus LCD206/app0, installing Sense208/app0. The
   latter is hard-bound to Sense205/app0 plus LCD201/app1, installing LCD206/app0.
   **Neither historical tuple is authority for a new installation.**
2. Bind real candidate materialization, full exact-snapshot passing regression
   result, canonical board `verified.json` and camera-aware artifact-check
   receipt. Reuse their validation of source, logs, artifacts, flags and partition
   bytes. Use actual catalog counts and board-proof receipts; do not manufacture
   old-version proof shapes or assume a fixed historical suite count.
3. Replace current proof/version/build/commit/tree and slot bindings with the
   actual bench pair. Set the candidate slot to the serviced board's verified
   inactive bank. Update plan kind, adapter identity key, provenance, expected
   result labels and matching controller imports/pins. The selected peer remains
   identity-only. Do not change the executor, write extents or selector algorithm.
4. Extend the adjacent offline adapter/controller tests for the new tuple. Retain
   rejection of wrong artifacts, ports/MACs, slots, CRC, stale identity, active
   work and altered dependencies; verify the appropriate slot transition and
   unchanged executor. Freeze the adaptation and plan hashes before execution.
5. The sole hardware owner obtains fresh paired Home/idle evidence through the
   existing capture controller: exact Sense FW_INFO and LCD nonce/length/CRC ID1,
   matching version/build, running slot equal to boot slot and SDK VALID. Refuse
   active OTA/binary/coordinator ownership or intervening user work/sleep/reset.
   Close and reap the capture before service. Retain the final five-second
   identity-age check **after** artifact hashing; this is a handoff bound, not a
   long awake lease. Refusal means stop and diagnose, not retry indefinitely.

The recorded bench MACs are Sense `98:A3:16:F8:1A:6C` and LCD
`20:6E:F1:A1:2D:C4`. A port name alone is insufficient. A different bench requires
explicit device binding and geometry/security/layout review, not a substituted
serial path. Obtain fresh identity again before servicing a second board, using
the first board's newly verified installed proof.

## What the service preserves

The executor verifies actual chip/MAC, flash geometry/security and partition
table. It reads NVS and both selector sectors, validates the selected current
image, and saves and verifies **both complete application banks before writing**.
It then writes only the full inactive slot and, after candidate/protected-range
verification, one derived alternate selector sector marked NEW. The prior
selected VALID bank and selector sector remain intact. It never guesses a
selector sequence from the release number.

NVS, ownership, Wi-Fi credentials, schedule, OTA debt/history, partition table,
bootloader and filesystem are not service write targets. NVS/table/selector bytes,
both bank hashes and the adapter's filesystem ranges receive the existing
readback/checksum barriers. This is verification of named ranges, **not a claim of
a whole-physical-flash reread**. Preserve the private bank/NVS backups and receipts;
they contain device data and do not belong in public release packages.

No erase-all, merged factory image, stock `boot_app0.bin`, policy fixture, allowance
refund or debt clearing belongs in this path. See
[Factory and recovery](PRODUCTION_FACTORY_RECOVERY.md) for the distinct recovery
and factory procedures. A retained VALID fallback is not permission to restore
an old NVS backup or erase later history.

## Failure, release and qualification

Keep one hardware owner, the existing locks, finite work/cleanup deadlines and
same-handle release. The historical executors reserve 180 seconds for work and
90 seconds for error release/cleanup. Do not split staging and application release
across fresh ROM connections: another reset before SDK validation can abort a NEW
candidate. Release through the retained handle and give its normal health gate
the opportunity to validate it.

On any failure or uncertain write, preserve attempted-write and readback receipts,
finish the existing bounded cleanup, and stop. Do not automatically rerun service,
re-arm selectors, restore backups or clear bookkeeping. Establish actual state
and review a separate recovery plan first.

Successful flash verification is followed by a finite ordinary health case:
exact new identity and SDK VALID, peer identity unchanged, expected Home/UI state,
the affected feature, retained-media handling where relevant, and native paired
sleep. The existing `service208-prep/health208.py` and binder are reusable examples
after their current/target/service-proof bindings are adapted. Keep unavailable
boot telemetry distinct from observed failures, and close/reap all capture owners.

Report USB installation, runtime acceptance and OTA acceptance separately. Service
does not resolve retained OTA debt or prove manual/scheduled OTA. Candidate success
does not publish anything; production promotion uses the exact qualified artifacts
and the separate explicit release workflow.
