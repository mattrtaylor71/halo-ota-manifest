# Provisioning and first-upload memory audit

Private candidate work following Sense205 / LCD206. Public paired201 and EOL197
remain unchanged. This note distinguishes the observed failure, the scoped
mitigation, and the larger memory opportunity that has not been implemented.

## Observed failure

The actual phone setup and immediate Check-in in
`/Users/MattTaylor/halo-provision-memory202-20260921/reprovision-lcd206-01`
completed, but the first account claim and first image PUT each failed with
`esp-aes: Failed to allocate memory`. Both second attempts succeeded in the
same wake. The JPEG reached S3 and decoded completely; the LCD followed its
normal idle and sleep sequence. Recovery is established, first-attempt
reliability is not.

| Boundary | Observed memory or result |
| --- | --- |
| First claim, AP+STA and app status polling active | HTTP -1 after 577 ms, AES allocation error |
| Second claim, same AP+STA resources still active | HTTP 200 after 930 ms |
| Before camera initialization | Internal heap 23,652 B; largest DMA block 11,764 B; PSRAM 7,825,520 B free |
| After camera stops Wi-Fi | Internal heap 53,072 B; largest DMA block still 11,764 B |
| Camera | Existing PSRAM fallback succeeds; 154,506-byte, 1280 × 1024 JPEG |
| Before PUT connect | Internal heap 27,832 B; DMA free 20,384 B; largest DMA block 8,692 B |
| After first PUT connect | Internal heap 16,764 B; DMA free 9,316 B; largest DMA block 5,108 B |
| First PUT | One recorded 512 B / DMA allocation failure during writing; incomplete upload |
| Second PUT | HTTP 200, complete diagnostic window, zero recorded allocation failures |

These are checkpoints, not the instantaneous lowest free heap. The later
512-byte failure cannot establish the size of the earlier claim failure,
which did not yet have its own allocation trace. Retry success does not prove
an AP teardown or a leak repair: both claims ran before teardown. The function
hash identifies an allocator entry point, not its caller.

## Resource ownership audit

- Claim and media transports share the existing upload worker. Claim clients
  and their guards are destroyed before the result mailbox becomes ready.
  There are not two concurrent claim TLS clients.
- Image presign HTTP/TLS objects are destroyed before PUT starts. Only the
  owned response URL, content type and immutable-object proof survive; there
  is no borrowed pointer to a destroyed response or overlapping presign TLS
  connection.
- The scan response cache is already released during actual setup teardown.
  The provisioning camera reserve is already released and cannot be
  reacquired by the claim TLS cleanup. MQTT is disabled and creates no client,
  queue or task. Repeating these prior fixes cannot recover more memory.
- The camera is deinitialized before the upload, and its retained JPEG uses
  PSRAM. Large voice/capture buffers also prefer PSRAM. The failed encryption
  allocation requires hardware-compatible internal DMA memory instead.
- Permanent task stacks include the 16 KB main loop, 16 KB operation worker,
  12 KB upload worker and 4,000 B audio worker, plus RTOS/SDK tasks. The camera
  grab worker is temporary. Individual compiler stack frames are not a
  measured safe task-stack bound; reducing these stacks is not justified.
- The pinned Arduino3.3.8 / IDF5.5.4 default allocation policy prefers internal
  RAM for allocations of 4,096 B or less. The existing mbedTLS override uses
  ordinary `calloc`, so moving large TLS buffers to PSRAM still leaves many
  small TLS allocations competing with Wi-Fi/AES in internal RAM. The roughly
  11 KB connect delta also includes socket/driver memory: it is not all
  reclaimable through the mbedTLS allocator.
- The outer Arduino secure-client object contains a 2,096 B context allocated
  with C++ `new`, which the mbedTLS hook does not move. Wi-Fi TX buffers, AES
  bounce buffers and descriptors also retain their SDK allocation paths.

## Scoped candidate

Install one stable mbedTLS allocator dispatcher at boot. A lexical scope on
the exact task owning a claim, image presign/reconciliation or image PUT
prefers `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT` even for small mbedTLS allocations.
The scope begins before constructing the TLS client and ends after its
destruction. Cancellation and early returns unwind the same scope. Outside
that task/scope, ordinary allocation remains in force. PSRAM allocation
failure falls back to the prior allocator; overflow is rejected and zero-size
allocation keeps the platform convention. Freeing does not depend on an
active scope.

This follows the supported external-memory policy in
[the pinned SDK allocator](https://github.com/espressif/esp-idf/blob/v5.5.4/components/mbedtls/port/esp_mem.c).
The [AES DMA path](https://github.com/espressif/esp-idf/blob/v5.5.4/components/mbedtls/port/aes/dma/esp_aes_dma_core.c)
still allocates its own hardware buffers. No DMA request is redirected to
arbitrary PSRAM and the global hooks are never swapped around live requests.

Claim diagnostics now record allocation failures, phase and memory checkpoints
through the same bounded hook used by image/voice. Claim and image reports also
show actual dispatcher call/byte counts after TLS teardown. These byte totals
are cumulative allocation requests, not live memory or a claim of bytes saved.
There are no heap queries, logging, waiting or new allocation inside the failed
allocation callback. The callback observes process-wide failures during the
owner interval, not only that task.

Camera/audio drivers, OTA routing/policy, task-stack sizes, user-input priority,
TLS verification, retry limits and durable capture identity are unchanged.

## Larger opportunity retained for separate OTA qualification

The exact Sense205 ELF reserves **65,536 B of internal BSS** for
`SenseOtaApplier::applyToOtaPartition::marker_check_buffer`, plus a separate
4,096 B streaming buffer. The first buffer remains reserved on ordinary
wakes, although it is only needed to inspect the first 64 KB of an OTA image.
It is a substantial fixed contributor to the tight memory budget, not a
newly observed leak. Evidence is retained in
`memory207-audit/static-memory205.txt` under the private evidence root.

Moving the marker buffer to owned PSRAM only during OTA is a stronger potential
RAM improvement. It has not been combined with this candidate: it adds a new
allocation-failure path to the firmware writer and needs direct marker,
partial-transfer, restart, cleanup and physical OTA validation. The existing
marker-before-write, hash and boot-selection rules must remain intact.

## Acceptance boundary

Host tests must exercise the real dispatcher, task ownership, nested and refused
scopes, zeroing, overflow, PSRAM failure, cross-scope frees and cleanup on every
transport exit. Actual claim and PUT harnesses must preserve cancellation,
response completion and custody. The full host gate and canonical Sense build
are required before a bench candidate is installed.

Hardware acceptance remains an actual phone provisioning → immediate capture
case, repeated with first-attempt claim and PUT success, complete allocation
reports, measured internal headroom and confirmed cloud JPEG. Then verify
ordinary image/voice, input interruption/resume and paired sleep. Host doubles
cannot prove RF behavior, actual memory savings or elimination of AES failures.

## Offline result

All **113 working-tree regression suites passed** without skipped suites or
runtime-source changes during the gate. Focused tests exercised 1,200 competing
allocator-scope claims, cross-translation-unit state, actual claim transport,
actual full PUT, and image presign/reconciliation. Removing either image scope
fails its regression negative control. These remain host tests with simulated
SDK boundaries, not a measurement of device headroom.

Evidence: `memory207-audit/regression-working01/RESULT.json`,
`scoped-tls207-tests/allocator03/RESULT.json`, `tls207-image-tests`, and
`allocation-audit207/claim-ownership-review.md` under the private evidence root.

One separate previously identified claim-policy issue remains: after all four
incomplete transport attempts, the advertised post-AP retry window does not
actually submit another claim. That retry bookkeeping needs a separate scoped
repair; this allocation change does not claim to fix exhausted-claim recovery.

## Built and installed on the private bench

Sense207 source `fcf6fb220f1f4983557669e8e3b552f910e079c1`, build
`6.4.207-20260922T073628Z-fcf6fb220f1f`, passed the second complete113-suite gate
in its immutable snapshot, canonical Sense compilation and artifact checks.
Binary1,874,752 B / SHA256
`a6a46832b39dd7b05b677657b7af1e17818b5a234909b7357f1c77e120076e0e`.
Independent exact-ELF inspection confirms the registered dispatcher, shared
internal owner and scoped call sites. Static internal RAM increases32 B; the
upload-worker frame increases32 B and claim transport frame176 B. Task stack
allocations are unchanged. These costs are not measured memory savings.

`service207-01` installed207/app1, preserving the complete205/app0 fallback,
NVS, partition table, filesystem and selected oldVALID selector sector. Only
the inactive application and alternateNEW selector were written; readbacks,
backups and independent44-reference service review passed. The release log
confirms scoped allocator rc0/PSRAM1, registered failure hook, SDK mark-valid
with current readback, and Sense sleep. LCD206/app0 was retained.

`postservice207-health01/RESULT.json` separately passes one fresh actuator
wake, exact paired207/206 SDKVALID identities, unlocked Home and both deep-sleep
logs. Capture and actuator ownership closed cleanly. This is not a phone
provisioning, capture/upload or OTA-transfer pass. The original memory-failure
scenario still needs its own physical repeat.

A preceding host wrapper successfully woke205/206 but then looked for a
`tapctl --out` file that its parent CLI does not create. The controller never
started and no flash was attempted. Its stdout JSON was retained and verified;
the corrected wrapper consumes stdout. Preserve `wake-service01` evidence.
This host receipt error is separate from firmware behavior.

Public paired201 and EOL197 remain unchanged. Continue firmware work from207's
reviewed source or descendants; do not publish this memory mitigation as
physically qualified until the immediate post-provision claim/capture case is
observed.

## First physical repeat on Sense207 / LCD206

The user's September 22 phone provisioning followed immediately by Check-in
passed in `reprovision-memory207-01`. This is one physical case, not repeated
reliability or a new full-product/OTA qualification.

- Account claim attempt 1 returned HTTP 200 with a complete response in 1,039 ms.
  Its allocation-failure window was complete, with zero failures or lost
  events. The dispatcher recorded 158 small external allocation requests and
  zero external allocation failures. The counter line truncates before
  `default_fallbacks`; that particular value is unavailable.
- Check-in began 0.801 s after the setup screen returned to Home. The existing
  camera PSRAM fallback captured 158,011 B at 1280×1024. The camera reserve warning
  still exists; this change does not eliminate all camera memory constraints.
- Image presign and PUT both succeeded on attempt 1. The PUT's complete
  allocation-failure window had zero failures/lost events; its allocator report
  had zero external failures and zero default fallbacks.
- The active PUT took 4.03 s; the complete pre-sleep upload flush took 5.58 s.
  Confirm to PUT completion was 17.99 s, including the existing foreground
  interaction/idle period. The upload still honors the user's active session.
- Internal heap just after PUT connection was 27,672 B, compared with 16,764 B
  on the previous Sense205 case. DMA free was 20,008 B versus 9,316 B; its largest
  block was 10,228 B versus 5,108 B. These are matched checkpoints across two
  cases, not instantaneous low-water measurements or exact causal savings.
- S3 contained one matching owner/device/time/size object. Its FULL_OBJECT
  SHA256 matched a conditional GET, and the JPEG decoded fully. SHA256:
  `33896360ab13bc7db58456751f5cf2f31c66f86d6f3126ff537e07fc955d74e6`.
  This confirms cloud image custody; backend recognition/inventory semantics
  and an independent camera-buffer hash were not checked.
- LCD showed completion for 4.49 s, returned to Home, accepted Check-in/Confirm,
  and darkened at its normal idle timeout. It stayed dark during upload, then
  both boards logged deep sleep. No USB reopen occurred in the following 316 s
  of passive capture. This does not establish an overnight no-wake result.

Evidence is sealed in `CASE-RESULT.json`, `cloud/RESULT.json`, raw serial bytes
and timestamped logs. Capture 76728 was stopped normally and reaped with exit 0;
all descriptors closed, no injected commands, one USB open per board. The
Sense raw file retains a 17-byte final partial line; early boot before USB
enumeration is outside this capture. Preserve these limitations and the
truncated claim counter rather than treating the log as lossless.

The observed first-attempt failures did not recur, and usable internal memory
improved substantially in this case. Repeat provisioning, ordinary voice and
input-interruption/resume coverage remain separate acceptance work. The
exhausted-claim retry issue and permanent 64 KB OTA buffer remain unchanged.
No new firmware was flashed or published for this read-only verification.
