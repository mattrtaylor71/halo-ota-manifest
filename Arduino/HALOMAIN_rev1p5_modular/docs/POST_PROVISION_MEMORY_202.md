# Post-provision memory investigation

Candidate work from production201 source `df018153ccd99900836732ca9eebe70bcb840485`; publication, installation and physical acceptance are not implied by this note. Preserve the201 OTA hint repair and all earlier fixes. Evidence workspace: `/Users/MattTaylor/halo-provision-memory202-20260921`.

## What is proved

The199 immediate post-provision voice case failed with ESP AES allocation errors twice, saved the exact recording, then delivered it on a clean automatic retry. Largest DMA/internal free block after setup was12,788bytes. The subsequent201 cucumber case delivered on its first POST despite the same reserve warning and a13,300-byte largest block. Both had about24KB aggregate DMA free but could not reserve one16,384-byte camera block. This establishes lost contiguous headroom; it does not identify the exact failed AES allocation or prove the camera would fail after its existing network recovery.

The scan response cache has a definite lifetime bug: it remains allocated after setup is finished. Arduino3.3.8 `String` retains capacity for assignment from an empty string, `clear()`, and even an empty temporary String. Assignment from a null C string invokes its invalidate/free path. Stop the setup HTTP server, explicitly release this unused scan cache, then retain the existing DNS/AP teardown and DMA admission order. Existing claim ownership, grace period, running-scan and user-priority guards remain intact. Before/after free/largest values quantify the actual contribution instead of assuming it cures all memory pressure.

## Bounded diagnosis

The supported ESP failed-allocation callback records four fixed observations: sequence, requested bytes, capability mask, active voice phase and a bounded hash of the allocator function name. One 32-bit compare/exchange drops a contended observation and sets a sticky loss marker with one store. Once loss occurs, reporting stays conservatively incomplete until reboot; the marker is not a dropped-event count. The hook performs no allocation, heap query, logging, wait or network operation. Overflow and loss are explicit; it never represents incomplete capture as complete.

Voice-owned snapshots record internal free, DMA free and largest DMA block before client construction, before/after TLS connect, before the first write/read and after client destruction. The owner reports them only after its TLS objects are destroyed. The byte counter measures bytes handed to TLS (including HTTP headers), not confirmed cloud delivery. Allocation failures from other tasks during the same phase are possible; the record is temporal evidence rather than a task-stack attribution.

The first-read point can be a zero-byte SDK connection probe, not reception of the HTTP response. SDK-internal reads can also occur outside the wrapper. Use the actual POST result and cloud correlation to establish acceptance; these phase samples only locate memory pressure in time.

No TLS allocator, cipher, SDK, payload size, timeout, retry count, Wi-Fi reset rule, user interruption, OTA guard, schedule, NVS or persistence behavior is changed. Existing512-byte background TLS writes remain intact. The observed AES log has several SDK allocation sites; do not call it a16KB AES request without the new evidence.

## Validation plan

Host tests exercise actual pinned String allocation/release semantics, setup lifecycle guards and negative controls, plus bounded diagnostic overflow/contention/owner lifetime and concurrent callbacks. Run the full source and exact-build snapshot gates before installing a candidate. Then capture a real re-provision and immediate voice note, correlate request/transcript/list completion, and inspect memory milestones. A same-wake camera capture is the separate next check. Do not clear saved uploads or force a radio reset to manufacture a healthy measurement.

The working-source gate passed all107 suites at `regression001/RESULT.json` in the evidence workspace. Focused diagnostics passed38 checks, including60,000 concurrent failure-hook calls. The scan-release test passed71 checks against pinned Arduino String allocation semantics and rejected both old empty-assignment and omitted-cleanup negative controls. These host checks do not establish that the physical AES failure is fixed. Candidate build, installation and physical results will be recorded separately.

The first candidate build stopped on the lock-free static assertion before any installation. Actual S3 assembly showed that byte atomic_flag and fetch-add can retry, while a strong32-bit compare/exchange emits one S32C1I. The revised gate uses the latter, with simple scalar loads/stores. The diagnostic test now compiles the actual gate with the pinned S3 compiler and rejects calls or branches in gate/load/store/release assembly. It also checks that a previous loss keeps later reports incomplete. Preserve the failed build under `release202`; the revised candidate uses `release202r2`.

Reference: [Espressif5.5.4 AES DMA allocation implementation](https://github.com/espressif/esp-idf/blob/v5.5.4/components/mbedtls/port/aes/dma/esp_aes_dma_core.c). The installed Arduino3.3.8 source and build proofs remain authoritative for compiled configuration.

## Exact build and physical result

The corrected candidate is source `0f42d531d58a3e62a37970463a72de669b0e53e3`, build `6.4.202-20260922T021005Z-0f42d531d58a`, under `release202r2` in the evidence workspace. Both full gates passed all 107 suites and the canonical Sense build/artifact checks passed. This is a private Sense bench candidate; LCD and public OTA remain 201. It is not a new production selection or paired release.

The standalone atomic probe omits the production `-mdisable-hardware-atomics` flag, so its no-calls result alone does not establish the deployed path. The exact linked ELF review at `elf-diagnostics-review/RECEIPT.json` closes that question for this binary: the gate is in internal DRAM at `0x3fcb7a2c`, and the SDK helper dispatches to one S32C1I, without entering its external-RAM critical section. ELF SHA256 is `e0c075a8050f31d8f24683197ffa3a884430ffcdf8b447fc729618d649fb2d05`. Future qualification must check actual flags, linked dispatch and gate placement, not rely solely on the standalone probe.

After the user placed the unit under the actuator, `service202-02` installed Sense 202 into app1 with full candidate readback and preserved the prior VALID 201 app0 bank, NVS, partition table and filesystem. Post-service capture verified Sense 202/app1/boot app1 SDK VALID, LCD 201 SDK VALID, registered diagnostics, normal Home and paired sleep. The initial misplaced-actuator attempt performed no firmware write.

`reprovision-voice202-01/TEST-RESULT.json` records one successful immediate post-provision voice case. The 90,112-byte recording was accepted on its first POST in 4.450 seconds; one cloud worker completed and inserted Cookies. Cloud transcript: “Add cookies to the shopping list.” Intended phrase confirmation is pending because the suggested test phrase was radishes. Release-to-list insertion was approximately 21.191 seconds, including the existing 12.682-second foreground/idle deferral before upload. This is one timing sample, not evidence that cleanup caused a speed improvement.

The cache release freed 704 DMA bytes from a 675-byte response. Largest free block stayed 13,300 bytes, and the 16,384-byte camera reserve still failed. Complete voice telemetry reported zero allocation failures and restored its pre-client memory values after TLS destruction. Thus the retention fix is physically verified, but it did **not** repair the contiguous camera reserve; this successful voice attempt does not prove the historical intermittent AES failure is cured.

The LCD stayed dark during upload; both boards slept and neither USB reopened during a 166-second observation. The next 02:00 Pacific maintenance arm was stored/verified, with LCD's 15-second lead. No scheduled transfer, same-wake camera, failed-upload persistence/retry or phone rendering pass is claimed. The next physical gap is a check-in capture immediately following provisioning, without an intervening sleep/reset. Separate diagnostic-journal persistence warnings remain distinct from the verified maintenance-arm acknowledgement.

## Immediate post-provision camera failure and scoped recovery candidate

`reprovision-camera202-01/TEST-RESULT.json` now proves that gap is a real failure.
Check-in job89 began5.58seconds after claim, before the15second app-completion grace
ended. The camera needed one16,384-byte internal DMA staging buffer; largest free
block was13,812bytes. Existing radio shutdown increased aggregate free heap from
24,056 to53,472bytes but did not change that largest block. Both camera init
attempts failed; no photo existed and no image upload was expected. Later setup
cleanup also left largest unchanged. This is fragmentation, not a failed upload.

A manual OTA preceded a POWERON reset in the same capture. The new boot held a
fresh camera reserve, then explicitly released it for provisioning. No OTA
reserve release preceded this camera failure in that boot. Interrupted manual
discovery recovery later delayed sleep until its deadline; its durable history
and NVS have been preserved. That separate confound does not account for the
recorded camera allocation failure. Both boards eventually slept.

The next private candidate preserves the provisioning/AP and TLS reserve rules
that avoid the earlier WPA/AES allocation failures. After existing camera
admission, reserve release and radio recovery, only a remaining sub16KiB block
selects direct PSRAM DMA. Normal adequate-memory captures retain internal DMA.
The pinned camera library sets the requested mode before returning
INVALID_STATE for an inactive sensor; the adapter accepts that exact case only
with verified mode readback and never calls the setter on a live camera. Fresh
initialization reselects the appropriate mode; KEEP_INIT reuse does not switch.

The fallback checks actual sensor PID after driver init, before starting the
grab worker or consuming frames. Only OV2640 is admitted because other sensors
can pad JPEGs beyond the pinned driver's EOI search window. This is not a claim
that all camera modules are qualified. Existing internal-DMA sensor support is
unchanged, and all failures retain bounded retries and full cleanup.

Direct DMA also requires exclusive cache-line ownership: the pinned library's
16-byte-aligned frame allocation is insufficient for this SDK's64-byte PSRAM
cache lines. A Sense-only linker wrapper, active only during fallback init,
rounds that specific framebuffer allocation's alignment/capacity up to64bytes
and prepares its cache before DMA. Other allocations retain their original
arguments. Exact archive/header/config hashes and actual linkage are required
by the canonical builder; no installed SDK bytes are edited.

Qualification is pending until the candidate is built and exercised on the
physical unit: reproduce setup→immediate check-in without sleep/reset, verify
OV2640/direct-DMA logs, decode actual JPEGs, confirm cloud delivery, repeat
captures, then check voice, normal capture, user priority and paired sleep.
Host coverage is not evidence of real image integrity or a production release.

The first203 candidate (`release203`, source858217b) passed both110-suite gates,
canonical compilation and artifact checks, but exact ELF review caught a target
defect before installation: the IRAM allocator wrapper called an outlined
`std::atomic<bool>::load` method in flash on its ordinary forwarding path.
`release203/HOLD.json` explicitly prohibits installing or publishing those bytes.
The scope flag now uses always-inline atomic compiler builtins; its focused
target probe uses the real SDK flags, including disabled hardware atomics.
The rebuilt candidate still requires a fresh full snapshot gate and exact ELF
review. No hardware was changed by the held build.

The corrected203r2 build is `6.4.203-20260922T031517Z-ee7f2f493e0e`,
source `ee7f2f493e0e883b25b809c1ca8d417d2ac7af8d`, firmware tree
`559a0cf3e19662902d8253dca3ad619ade350beb`. Its complete exact-snapshot
gate passed all110 suites; canonical Sense build and artifact checks passed.
Exact ELF SHA256 is
`99c1a6bc9b2b338fcbbc47f97e4bf8415c29658f502f59040febc8276f0a9cca`.
`elf203-review/actual203r2/MANUAL_REVIEW.json` verifies the wrapper's internal
code/data, inline scope-flag access, actual camera linkage, unconditional flag
clear and OV2640 guard. It also rechecks the202 diagnostic hook's internal
single-CAS path. These are compiled-path checks; physical JPEG integrity and
delivery are still pending.

The first203 installation attempt, `service203-01`, stopped before any firmware
write: the actuator worker stalled while configuring USB. No stroke was sent,
the passive capture opened neither board and `service_attempts` remained0.
The supervisor exited1; the worker's exit code/reap was not observed. A later
read-only check proves that worker PID24593 and its process group are absent;
the original timeout receipt remains unchanged. The next attempt waits for a
manual wake, then uses the existing fresh identity/idle/diagnostic-lease gates
and NVS-preserving inactive-bank service. Public paired201 and EOL197 remain
unchanged; this candidate is not a production release or an OTA test.

The manual wake in `service203-02` was detected, but again no flash write was
attempted. The exact diagnostic acknowledgement was truncated in the USB log;
the controller refused it and closed capture. `pre-service203-observation01`
then recorded paired sleep about25seconds after `diag`, despite continuing
diagnostic pings. Code review confirms that `diag` only refreshes the user idle
timestamp; it does not guard the Home-age or remote-sleep paths. Its advertised
five-minute duration is **not** a five-minute sleep-exclusion lease. Do not reuse
that premise for service admission. A separate bounded installation hold must
be verified and removed before camera/voice/sleep acceptance; preserve both
failed attempts and their zero-write results.

The replacement external tool is `service203-rom-prep`. It requires fresh
paired idle/SDK identity, rechecks its five-second handoff age after artifact
hashing, then uses the unchanged bounded Sense-only ROM executor. Actual ROM
MAC/security/stub ownership precedes every flash write; that chip's application
sleep code is no longer running. The LCD's GPIO wake line is not Sense power or
reset, so LCD sleep after takeover does not require a fabricated long awake
lease. Both full-bank backups and every NVS/current-bank/filesystem/readback
barrier remain. No diagnostic or test-mode command is sent. Its19 offline tests
and actual203r2 plan validation passed. The unused test-mode tooling draft was
never executed and is marked as such. Physical installation and subsequent
paired health still require their own observed receipts.

`service203-03` subsequently installed the exact203r2 Sense image into app0.
The complete candidate/selector readback and protected-range checks passed;
both banks were backed up and Sense202/app1, NVS and filesystem were preserved.
The executor released/reset Sense once on the same descriptor and closed all
ownership. Boot logs identify203/app0, the registered allocation hook, and
`esp_ota_mark_app_valid_cancel_rollback` success with current SDK readback.
Fresh peer queries reported LCD201/app1/bootapp1 SDK VALID. Sense prepared sleep
and its USB disappeared; LCD then completed paired deep sleep with its next
02:00 Pacific maintenance arm. The final Sense sleep line was truncated, so
do not represent that line as complete. Visible post-service Home and camera
acceptance are separate from this boot/readback result.

`postservice203-health01/HEALTH-RESULT.json` records these limits. Its capture
closed cleanly before `reprovision-camera203-01` was armed for the user's repeat
setup→immediate Check-in/Confirm case. No203 JPEG or upload pass is claimed yet.
Public paired201, EOL197 and retained OTA policy remain unchanged.

### Physical result: immediate post-provision Check-in on203

`reprovision-camera203-01/TEST-RESULT.json` now closes that pending case.
The user completed provisioning and selected Check-in about5.5seconds after
claim completion, without intervening reset/sleep. The camera selected the
PSRAM fallback with largest internal DMA block11764bytes, verified actual
OV2640 PID0x26, and captured a149100-byte1280×1024 JPEG in1527ms. This is a
physical pass of the previously failing immediate-camera path on this unit.

The first upload still failed: all15 PUT attempts emitted an AES allocation
failure. The exact failed allocation size/capabilities are not established;
post-error heap logging occurs after TLS cleanup and cannot establish them.
The camera had already been deinitialized. Do not claim the camera fix also
solves this separate encrypted-upload memory pressure.

Durable recovery passed. Halo saved job118, entered paired sleep, and uploaded
the saved image automatically on the retry's first PUT (HTTP200), about391s
after Confirm. The read-only cloud receipt under
`backend/object-check-20260922T035345008079Z/RESULT.json` verifies149100bytes,
SHA256 `f9c1ab026829e8fe7d2ebff516ec2cbf7707a2a96f39dbcd1f97a8f527fa0919`,
and full JPEG decode at1280×1024. This proves exact captured-image delivery;
downstream recognition/inventory processing was not checked.

Observed retry heartbeats kept backlight/panel off. A following short timer
wake cleared the pending hint to0, canceled the retry arm with peer ACK and
returned both boards to sleep; the02:00Pacific maintenance arm remained.
USB attachment does not cover the first instant of boot, so the display claim
is limited to captured telemetry. The passive capture issued no commands,
closed cleanly and was reaped with exit0. Final evidence hashes are in the
test receipt; raw logs contain credentials/signed URLs and remain private.

This is one physical camera/retry case, not repeated-capture, voice or OTA
qualification. Sense203 remains a private bench candidate paired with LCD201;
public paired201 and EOL197 are unchanged. Remaining investigation should
capture image-PUT allocation failures at the failing call, before TLS cleanup,
rather than infer their cause from the recovered heap afterward.

### Image PUT memory investigation and bounded mitigation

The failing first wake had25760bytes free before TLS connection; the successful
saved retry had31836. Largest DMA block was actually larger on the failed wake
(13300 versus12276), so those measurements do not prove a contiguous-block
threshold or the exact allocation that failed. The pinned AES implementation
has both small DMA-descriptor and up-to1600byte bounce-buffer allocation paths
with the same error text. Arduino stops TLS before returning a failed write,
making the old post-write heap diagnostic unsuitable for sizing the failure.

Review found no unclosed camera, claim TLS client or dedicated claim task:
claim transport reuses the upload worker, and its clients are destroyed before
publishing the result. The setup HTTP/DNS server and scan cache were already
released. Do not attribute the remaining roughly6KB difference to a proved
provisioning leak. A small Arduino WebServer argument-array cleanup issue is
separate and was not changed in this work.

There is avoidable image-request memory pressure. This case's signed path was
1740bytes. The old String copy requests1744bytes for the whole PUT, and
Print::printf additionally requests1756bytes while writing the request line
(pinned Arduino3.3.8 implementations). Standard allocator preference puts
small requests in internal memory when possible. The new image-only helper
borrows the original immutable URL's path and streams the same headers through
a512byte stack buffer, eliminating those duplicate heap-buffer requests. It
does not decode/rebuild signed query bytes. Partial writes resume at confirmed
bytes; zero/invalid writes stop the attempt. Every write checks the original
deadline, user cancellation and foreground state, and failed headers cannot
fall through into the image body. TLS validation, body chunk size, retry
budgets, durable custody, provisioning and OTA policy are unchanged.

Image PUT now uses the existing bounded allocation-failure recorder. Per-attempt
reports include phase, size/capabilities and heap snapshots; printing occurs
after TLS stop/destruction. The callback/ring are unchanged, allocations from
other tasks can also be observed, and overflow/contention marks evidence
incomplete. Voice diagnostics retain their existing behavior.

Evidence is under `image-put-memory-review` in the private case root. All111
working-tree host suites passed, including8891 new signed-wire, parser,
short-write and cancellation checks under ASan/UBSan. The pinned S3 helper
compile has no malloc/calloc/realloc/new references; its main header frame is
608bytes, excluding callees and the caller. This is not physical stack-margin
evidence. The mitigation is not yet proved to cure the AES failure: repeat
provision→immediate Check-in on the candidate and require first-attempt PUT200,
exact cloud-image integrity, user priority and dark saved recovery. Do not
publish or declare an AES fix solely from these host results.
