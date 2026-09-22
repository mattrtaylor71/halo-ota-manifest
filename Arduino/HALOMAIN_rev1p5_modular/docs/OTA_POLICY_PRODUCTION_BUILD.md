# Production OTA build

Start from `PRODUCTION_BASELINE.json` and `RELEASE_BASELINE.json.current_working_source`. Follow [Build and release](BUILD_AND_RELEASE.md) for the current published 211 source and future freshly allocated 212+ versions. The retained158 record is historical recovery only. Publication/build evidence and physical acceptance remain separate. Preserve exact source/build/publication provenance as described in [Release baseline](RELEASE_BASELINE.md). `halo_ota_demo/publish_both.sh` delegates to the verified prebuilt-artifact publisher; it does not compile firmware.

Prepare a release snapshot from clean committed source, then compile that snapshot:

```sh
python3 -B tools/prepare_production_release.py --version <allocated-version> --epoch <explicit-UTC-seconds> --out /absolute/path/to/new-snapshot
python3 -B /absolute/path/to/new-snapshot/source/tools/build_ota_policy_production.py --out /absolute/path/to/new-build
```

Allocate unused versions from the current release record and immutable artifact inventory; do not reuse historical candidate numbers. For a final production version, prepare its explicit version and omit `--private-canary`. The preparer requires the committed path manifest and clean source scope, records full Git commit/tree and changes only three generated metadata headers in the external snapshot. Its build ID is deterministic for the explicit source/version/epoch. Repository metadata remains clearly precommit/source-only; the publisher rejects provisional proofs. Never publish a direct build of those provisional headers.

Use `--board sense` or `--board lcd` for one target, or `--plan` to inspect the exact commands. Each output directory must be new. The entry verifies the partition-table hashes and records the compiler command, log and bounded process completion. Each compiler gets 600 seconds plus at most 10 seconds for cleanup.

The canonical entry supplies these flags automatically:

| Setting | Sense | LCD |
|---|---:|---:|
| Durable OTA policy | 1 | Sense owns policy |
| Durable diagnostics | 1 | 1 |
| Diagnostic admission export | 1 | Sense owns export |
| LCD sleep witness | 1 | 1 |
| Idle-network recovery | 1 | Sense owns guard |
| Scheduled one-shot test command | 0 | 0 |
| Accelerated bench profile | 0 | 0 |
| Local diagnostic credential provisioning | 0 | 0 |
| Idle-network probe | 0 | Not enabled |
| UI review / layout audit | Unchanged | 1 / 1 |

It supplies no private channel, S3 route, capture fixture, snapshot-retirement or spool-disable override. Existing production configuration and spool defaults remain in use. Version headers and firmware metadata are not rewritten. Policy and diagnostics are enabled through this build entry across compilation units; low-level compatibility builds that omit these flags retain their existing source fallbacks and do not qualify as this production build.

The explicit `--private-canary` option appends only the fixed dev-bucket channel/prefix for `halo/ota/canary/production-release-20260909`. All shipping limits and disabled bench/one-shot/probe/fault controls stay the same. Such artifacts are labeled canary and cannot be presented as default-route production builds. The production example above omits this option. Select it only for an explicitly scoped private-route candidate; it is not the normal release workflow.

The exact Sense target is `esp32:esp32:XIAO_ESP32S3:PSRAM=opi,USBMode=hwcdc,CDCOnBoot=default`. The LCD target is `esp32:esp32:esp32s3:PartitionScheme=custom,FlashSize=8M,USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi`. The configured LCD image layout is 8 MiB. Preserve the checked board libraries, pin assignments and partition tables when reproducing a qualified build.

The production path uses the durable shipping limits. A stored full target binds both boards to their versioned manifests. Paired update orchestration updates LCD first, then Sense. SDK image validation requires real local readiness and checked selected/running image state. Fresh clock and peer qualification are separate OTA admission checks. Fast retry, deferred maintenance and remaining work retain their persisted limits across reboots; a new version alone does not clear unresolved debt.

Compilation does not establish battery continuity, unattended shipping-limit maintenance, or on-device qualification of a newly reconciled source tree. Those runtime results must be recorded separately before installation or release.

The qualified shipping profile retains the fixed HTTPS diagnostic admission endpoint already used by the M8 shipping build. This optional exporter is separate from the firmware download route and requires an existing scoped runtime credential; without one it is disabled. No key enters the build, and local credential provisioning remains off. Owner claim does not install the B1 key. Fresh units therefore skip this optional admission export before HTTP. Ordinary reporting, retained Diagnostic/H4 export, and OTA policy/download do not depend on that key.

The tracked MQTT-disabled configuration supplies the existing public CA with empty client certificate/key. The canonical builder rejects `MqttSecrets.local.h` and `MqttSecrets.local.cpp` overrides before compiling. This packages the tested configuration reproducibly without enabling MQTT or committing credentials. Compile jobs are capped at two.

The Sense camera fragmentation recovery additionally requires the pinned
ESP32-S3 camera archive, public header and qio_opi SDK configuration.
`production_camera_driver_guard.py` records their exact bytes before and after
compilation, verifies actual header dependencies and retained camera symbols,
and rejects an unknown SDK. The Sense link adds only
`-Wl,--wrap=heap_caps_aligned_alloc` through `compiler.c.elf.extra_flags`, leaving
the SDK linker defaults intact. It verifies the scoped allocation wrapper is
retained from the Sense sketch and referenced by the camera archive. This
protects the pre-init mode-setting and64-byte cache-line contract described in
[post-provision memory investigation](POST_PROVISION_MEMORY_202.md); it does not
edit the installed SDK or qualify physical JPEG capture. Keep
`sdk-camera-before.json` and `sdk-camera-compiled.json` with candidate evidence.

The qualified Arduino ESP32 3.3.8 Network library also requires the tracked DNS-cache lock correction. On first IP acquisition, its `NetworkManager::hostByName` calls raw `dns_clear_cache` without the TCPIP core lock; clearing an outstanding DNS entry can remove a UDP PCB and panic. The correction adds the lock only around cache clearing, leaving blocking `lwip_getaddrinfo` outside it. No firmware network retry, timing, DNS policy or assertion setting changes.

Apply the exact reviewed SDK correction once, using a new evidence directory:

```sh
python3 -B tools/production_network_dns_patch.py --apply --sdk-source "$(arduino-cli config get directories.data)/packages/esp32/hardware/esp32/3.3.8/libraries/Network/src/NetworkManager.cpp" --out /absolute/path/to/new-sdk-patch-receipt
python3 -B tools/test_production_network_dns_patch.py
```

The helper accepts only the pinned upstream file or its exact corrected bytes, preserves the prior file in the receipt directory, and records both hashes. It changes the installed SDK explicitly; canonical compilation does not silently patch it. The builder refuses unpatched or unknown bytes, saves `sdk-dns-patch.json` plus a source copy, and verifies the actual `NetworkManager.cpp` translation unit/object after compilation in `sdk-dns-compiled.json`. Include these receipts with each build's provenance. Do not upgrade or locally edit that SDK during a build. The native regression extracts the real pinned function: the upstream code reproduces the unlocked cache-clear failure; corrected code locks cleanup while IPv4/IPv6 lookups, literal addresses and failure paths preserve their existing behavior. This is host evidence; the startup shopping path still needs device validation.
