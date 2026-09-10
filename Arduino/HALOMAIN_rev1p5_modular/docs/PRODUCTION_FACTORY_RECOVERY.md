# Production 6.4.104 factory and recovery package

This document describes the exact shipping artifacts built from commit `a54bb86b5e8e4fc96a4bb8f25ba8d08581489387`, with build ID `6.4.104-20260910T014009Z-a54bb86b5e8e`. The production route and reported channel are both `prod`; bench, one-shot, probe and fault controls are disabled. Artifact qualification has passed. Device acceptance and production promotion are separate receipts in `RELEASE_BASELINE.json`; this document does not claim those actions have happened.

The local package is `production-release104-recovery-package001` in the release evidence archive. `MANIFEST.json` lists every file's size and SHA-256; `SHA256SUMS` verifies the portable files. Each board has its exact application, bootloader, partition table, `boot_app0.bin`, 8 MiB merged factory image, `flash_args`, partition CSV, ELF and map. The provenance directory contains the committed source materialization and build proofs. Absolute references inside those proofs identify the separately retained evidence archive; they are not substitutes for the package's relative file hashes.

The application hashes are:

| Board | Bytes | SHA-256 |
| --- | ---: | --- |
| Sense | 1,803,008 | `78620893e66e63150570088464cef6cab790351da14accc694c325f900ad59fa` |
| LCD | 1,877,264 | `5f85c91a207d4da4c7bce46617596453cdf31826169fe16f7bd3bd1ddd724d01` |

## Identify the hardware before service

Use the pinned esptool 5.2.0 with the correct board's explicitly bound serial device. Record chip/MAC, physical flash ID and size, security state, and the current partition table before selecting a write plan. A port name alone does not identify a board. The observed test pair has Sense MAC `98:A3:16:F8:1A:6C` and LCD MAC `20:6E:F1:A1:2D:C4`; a factory station must bind each new unit's own identity instead.

| Property | Sense | LCD |
| --- | --- | --- |
| Chip | ESP32-S3 | ESP32-S3 |
| Physical flash on observed pair | 8 MiB | 16 MiB |
| Compiled image flash setting | 8 MiB, DIO, 80 MHz | 8 MiB, DIO, 80 MHz |
| End of partition-table allocation | `0x400000` (4 MiB) | `0x800000` (8 MiB) |
| NVS | `0x9000`, length `0x5000` | `0x9000`, length `0x5000` |
| OTA selector | `0xe000`, length `0x2000` | `0xe000`, length `0x2000` |
| app0 / app1 | `0x10000` / `0x1f0000` | `0x10000` / `0x290000` |
| Application slot size | `0x1e0000` | `0x280000` |

Both observed devices have secure boot, flash encryption and secure-download mode disabled. These are unsigned, unencrypted application images; HTTPS and manifest SHA-256 verification do not make them secure-boot-signed images. Refuse this service procedure if actual chip, physical geometry, security state or existing table differs from the reviewed unit plan. In particular, do not reduce an LCD full-device backup to its configured 8 MiB: the observed physical device requires a 16 MiB backup, including its upper tail.

Read-only service commands below still enter ROM/reset the application. Run them only after the active OTA/capture owner has closed, and keep their results with the service case:

```sh
esptool --chip esp32s3 --port "$PORT" get-security-info
esptool --chip esp32s3 --port "$PORT" flash-id
esptool --chip esp32s3 --port "$PORT" read-mac
```

Archive a private full image before recovery. Select exactly one size from the freshly measured geometry; these examples describe the observed boards:

```sh
umask 077
esptool --chip esp32s3 --port "$SENSE_PORT" read-flash 0x0 0x800000 sense-full-before.bin
esptool --chip esp32s3 --port "$LCD_PORT" read-flash 0x0 0x1000000 lcd-full-before.bin
shasum -a 256 sense-full-before.bin lcd-full-before.bin
```

Backups contain customer credentials and ownership data. Store them privately, separately from release artifacts, and bind each to its board identity and capture time. The archived 101 test-pair backups are preserved in `production101-backup-run001`; never flash those device-specific backups onto another unit.

## Existing provisioned unit: preserve NVS by default

Use normal paired OTA discovery and its supported manual action or daily maintenance opportunity. Publish the verified prebuilt pair with `halo_ota_demo/tools/ota/publish_pair.py`; do not rebuild at publication. The existing updater writes the alternate application slots, validates images and changes the selectors through its checked lifecycle. Retain NVS, ownership, Wi-Fi, authentication, policy debt, diagnostic journals, filesystems and the prior application bank. A new firmware version is not permission to erase or refill the policy ledger.

Do not use `erase-flash`, a merged factory image, or the stock `boot_app0.bin` on a provisioned device as an ordinary update. Those procedures overwrite selector and/or data state. There is deliberately no universal app-only USB command here: the correct inactive bank and selector transition depend on current full images, actual VALID state and unresolved paired policy. A service plan must verify both boards before writing either selector, preserve the current banks and unrelated bytes, and verify complete expected readback before application release.

For a controlled USB installation that selects a candidate as NEW, retain both serial descriptors through final selector readback and coordinated application release. Do not end a staging process and later reconnect through ROM to release those NEW images. A further reset before SDK validation can abort a candidate and select the previous VALID bank. In the controlled102 test, both candidates were observed ABORTED after the separate-stage/release sequence; the same intact images reached VALID after selector-only re-arming and a retained-descriptor paired release0.1078seconds apart. The earlier electrical interval was not captured, so this does not identify which individual reset or delay caused the abort. Archive the actual policy and preserve current NVS during such service; re-arming a selector is not an autonomous OTA retry result.

The separately archived fixture tools prepare declared new test cases. The planned initial setup uses a clean-policy fixture; exhausted-yesterday hardware testing remains deferred and is not a passed release-acceptance case. These fixtures are not customer recovery commands and must never be applied inside an active retry/failure case.

## Fresh factory unit only

Use this path only for a genuinely new unit or an explicitly authorized factory reset with the old state archived. It destroys existing provisioning and OTA history. Verify package hashes first, select each board's own directory, and keep both boards from starting their application until the pair has been written and verified.

```sh
cd /absolute/path/to/production-release104-recovery-package001
shasum -a 256 -c SHA256SUMS

# Factory Sense: exact 8 MiB merged image, including erased NVS and initial selector.
esptool --chip esp32s3 --port "$SENSE_PORT" --after no-reset erase-flash
esptool --chip esp32s3 --port "$SENSE_PORT" --after no-reset write-flash --flash-mode keep --flash-freq keep --flash-size keep 0x0 sense/halo_sense_prod.ino.merged.bin
esptool --chip esp32s3 --port "$SENSE_PORT" --after no-reset verify-flash 0x0 sense/halo_sense_prod.ino.merged.bin

# Factory LCD: physical 16 MiB is erased; the qualified image/layout occupies 8 MiB.
esptool --chip esp32s3 --port "$LCD_PORT" --after no-reset erase-flash
esptool --chip esp32s3 --port "$LCD_PORT" --after no-reset write-flash --flash-mode keep --flash-freq keep --flash-size keep 0x0 lcd/halo_lcd_prod.ino.merged.bin
esptool --chip esp32s3 --port "$LCD_PORT" --after no-reset verify-flash 0x0 lcd/halo_lcd_prod.ino.merged.bin
```

The commands above document the qualified component payloads and esptool options; they are not a verified paired station runner. The `--after no-reset` option applies to each invocation and does not establish that both descriptors remain held across separate commands. A station must implement and verify coordinated ownership/release before this procedure is treated as hardware-qualified.

The merged files were checked byte-for-byte against bootloader at `0x0`, the exact board partition table at `0x8000`, `boot_app0.bin` at `0xe000`, and application at `0x10000`. Their NVS and app1 ranges are erased. Do not substitute a generic partition table, change flash mode/size, or mix board components. Independent hardware acceptance of the factory commands remains a separate gate; packaging checks are offline.

Boot both boards after readback. Use the existing HALO app/setup portal and a current owner code to provision WLAN and ownership. Claim may supply a timezone; otherwise the compiled Pacific rule is `PST8PDT,M3.2.0,M11.1.0`. Verify the actual accepted timezone and next local 02:00 due time, rather than assuming a timezone from a host label. The optional B1 admission key is not installed by owner claim; missing B1 authentication safely skips that optional exporter. Ordinary reporting, retained diagnostics/H4, and OTA discovery/download remain independent of it. MQTT remains disabled with empty client credentials.

Provisioning serial output can contain setup credentials and owner codes. Keep raw captures restricted and redact them before sharing. Verify paired identity/version/build, local readiness and Home, running/selected VALID state, normal reporting, and eventual sleep. A software/USB reset does not prove an electrical cold boot or power-loss recovery.

## Rollback and a bad published release

For an unvalidated OTA image, the SDK and `HealthGate` manage the PENDING_VERIFY validation/rollback lifecycle. Preserve the previous valid bank and current policy attribution. An already VALID image is outside that automatic pending-image rollback path.

Restoring an older `manifest_latest.json` may stop discovery of a bad new target, but it does not force installed devices to downgrade: durable admission requires a verified newer target. Use a newly versioned corrective release with the same paired prebuilt validation and current policy checks. Archive both previous latest bodies and current attempted/readback receipts before changing pointers. The publisher uses conditional writes and makes no automatic rollback attempt after an uncertain or partial promotion; inspect both actual latest objects first.

A same-device full-flash backup is a disaster-recovery artifact, not an automatic update mechanism. Restoring it also restores the archived provisioning and ledger state and can discard later history. Require a concrete paired recovery plan and explicit state-loss disposition; do not restore old bytes as a way to retry an exhausted OTA or copy a backup between units. After any repair, verify both banks/selectors, current policy and both application identities before returning the unit to service.

## Evidence and release boundaries

- Exact 104 pair: `production-release104-build001/RELEASE-PAIR.json`, SHA-256 `9dccd7babe361ad6017645df84ef45842a8e33d1d966615f904f76990b729308`.
- Independent 104 artifact peer: `production-release104-build001/SESSION-RELEASE-PAIR-PEER.json` (the record binds exact source, arguments, artifacts, partitions, resources and closed owners).
- Actual prior hardware layout/security/full backups: `production101-backup-run001/result.json` and `SESSION-BACKUP-NVS-PEER.json`.
- Publishing and byte-identical canary bridge: [Paired release](../halo_ota_demo/tools/ota/PAIRED_RELEASE.md).
- Acceptance limits and final deployment state: [Production acceptance](PRODUCTION_RELEASE_ACCEPTANCE.md) and [Release baseline](RELEASE_BASELINE.md).

Do not turn historical notes in `PRODUCTION_PREP.md` into current release blockers or deployment claims. The finite current receipts determine what passed, what remains pending and which exact bytes may be promoted.
