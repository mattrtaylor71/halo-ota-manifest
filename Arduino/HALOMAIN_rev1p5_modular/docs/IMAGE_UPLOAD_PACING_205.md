# Private Sense205 image upload experiment

Sense205 is installed on the bench with LCD201. The ordinary Check-in smoke
passed on the first upload attempt, but the decisive same-boot phone
provisioning → immediate Check-in case is pending. Do not call the original
post-provision AES failure fixed from the ordinary smoke. Public paired OTA
remains201 and EOL remains197;205 has not been published.

## Why this change

Private204 captured the user's immediate post-provision image successfully but
failed all15 first-wake PUT attempts. It saved the exact145109-byte image, then
automatically uploaded it on the first attempt after a299-second sleep. The
exact captured SHA matched the cloud object, which fully decoded.118 captured
LCD heartbeats across retry/cleanup showed backlight0 and panel0. See
[204 results](IMAGE_UPLOAD_MEMORY_204_AUDIT.md).

The allocation trace caught512-byte DMA allocation failures during encryption,
alongside internal DMA failures consistent with Wi-Fi transmit pressure. The
pinned SDK/ELF review identifies the AES bounce allocation path; caller PCs and
actual transient heap low-water remain unmeasured. The204 buffer removal saved
memory but did not cure the first-wake case.

205 adds only a requested delay of up to2ms between image-body writes, bounded
by the original deadline, with the existing cancellation and foreground guards
before and after. The pinned RTOS tick is1ms. There is no delay before the first
body chunk, after the final chunk, or in response handling. This reduces burst
rate; it does not reserve DMA memory or guarantee recovery. A synchronous SDK
write retains its existing interruption limits. The added wait itself requests
at most2ms, although scheduler latency is not a hard realtime bound.

512-byte chunks, TLS validation, request bytes, partial-write accounting,
immutable image identity, persistence, delivery receipts, retry limits, voice,
OTA and Wi-Fi driver configuration are unchanged. No heap buffer was added.

## Source, artifact and offline qualification

- Source: `0600f1fa5f73aa683c6eea6f7fb30bedd4bd8754`.
- Firmware tree: `914522dfcf12cc21839f83d9e15e23dec85ddfa9`.
- Build: `6.4.205-20260922T060624Z-0600f1fa5f73`.
- Sense BIN:1872976 bytes; SHA256
  `2186fafaf7cc1f9edaeabeab5d6eb18b3916f96ebe9fbb583eaacf4fc10034fc`.
- ELF SHA256:
  `69206827083b6d8149fdae3906ce72f77be553703358ef0ba5727c0797057725`.
- Evidence root: `/Users/MattTaylor/halo-provision-memory202-20260921`.

Both working-tree and exact materialized-source gates passed all112 suites.
The actual-PUT focused harness passed102 candidate scenarios/1435 assertions;
comparison with unpaced204 passed184 scenarios/2390 assertions. It checks exact
wire bytes, partial writes, cancellation/foreground arrival during the pause,
no subsequent write after user input, one-ms deadline clamping and wraparound,
no first/final-chunk wait, and balanced HTTP/DMA/trace cleanup. A bounded
transmit-queue model makes the old path fail and the paced path pass; its drain
rate is an adversarial test double, not a measured Wi-Fi throughput claim.

The canonical shipping-profile Sense build and unchanged camera-aware artifact
checker passed. Static RAM remains159508 bytes and RTC slow memory2528 bytes;
the application grew64 bytes. No LCD205 was built. See `release205/QUALIFICATION.json`,
`release205/regression/RESULT.json` and `pacing205-tests/regression112/RESULT.json`.

The first artifact-check invocation followed a stale general documentation path
and refused the legitimate camera wrapper's second build property. Its empty
output directory was retained; the already qualified203/204 camera-aware checker
then passed without edits or bypasses. `release205/CHECKER-SELECTION.json`
records this, and [build instructions](BUILD_AND_RELEASE.md) now select the
correct pinned checker.

## Installation and ordinary hardware smoke

`service205-03` installed205 in Sense app0 while preserving VALID204/app1,
NVS, filesystem, partition table and the selected old selector sector. Both
complete banks were backed up before replacing203/app0. Candidate and alternate
NEW-selector readbacks passed. Service/capture workers closed and reaped.
The release boot log shows205, the memory hook registered, SDK mark-valid with
current readback, LCD201 VALID/ready, Wi-Fi connection and sleep. This is USB
service, not an OTA transfer or physical power-cycle qualification.

Earlier host-only refusals are retained: `service205-01` found sleeping boards
before USB opens; `service205-02` had a parent Python environment import failure
after passive capture. Neither sent a device command or attempted flash. Both
closed. The inherited Python launcher variable was removed for the successful
run; firmware and installation guards were unchanged.

After the user's actuator reset, fresh HELP/STATUS, calibrated strokes and STOP
returned successfully, with physical paired wakes. This is finite recovery
evidence, not a permanent actuator repair. `bench205-smoke01` missed the short
awake window and stopped without commands. A single host sequence then handled
wake → observed boot-work completion → fresh identity → one Check-in/Confirm.

`bench205-smoke02/RESULT.json` records job28: camera captured146717 bytes at
1280x1024 in1107ms; first PUT returned200, body time2764ms, with
`failures=0 loss_seen=0 complete=1`. Queue drained, retry arm cleared with ACK,
both boards slept, and the retained next timer was02:00Pacific. The full capture
`postservice205-smoke01` closed and its process reaped normally.

The cloud object was uniquely correlated by known owner/device prefix, exact
146717-byte size and PUT completion time. Read-only retrieval/checksum/JPEG
validation is in `bench205-cloud01`. Its expected SHA comes from the S3
FULL_OBJECT checksum; this successful fresh path did not print an independent
camera-buffer SHA. Do not label that as the same independent capture-hash proof
as the failed-and-saved204 case. Backend recognition semantics remain separate.

The fresh passive `reprovision-camera205-01` capture is armed for the user's
normal app provisioning followed immediately by Check-in/Confirm. First-attempt
PUT200 without allocation failures in that same wake is required to close the
immediate-upload regression. User interruption on205 and ordinary voice remain
physically unqualified; host coverage and prior-version results are not new
physical passes. No new manual/scheduled OTA transfer is claimed.
