# 6.4.171 corner-case fixes — built, host-tested, not installed

Continue from artifact source `aa221c2ef8871e428747c32ac61216993dae58bc` or reviewed descendants. Build ID: `6.4.171-20260917T014934Z-aa221c2ef887`. The 171 candidate retains all 170 dark retry/user-priority changes and all prior provisioning, camera and OTA fixes. The corrected foreground **test fixture** is in descendant `847a856`; it changes no device code.

## Three reproduced defects and fixes

- **Wi-Fi clock zero:** an association starting at `millis()==0` never timed out because zero also meant absent. The existing inflight flag now owns validity in all three active paths. The new test fails on the old source and passes the fix.
- **Interrupted accepted-upload deletion:** a payload could be removed while metadata cleanup failed, permanently consuming a slot. Voice/image stores now sync, commit and read back an identity-bound `.delete` marker before removing anything. Owner-bound list/admission resumes cleanup; the marker is removed last. Mismatched/corrupt survivors stay held. Original-code negative controls reproduce 14 failures.
- **User input during SD retry work:** background CRC/list/cleanup could continue until its 12-second budget after touch. Both budget callbacks now honor replay cancellation. All saved-media controls (LIST/FETCH/ATTEMPT/DELETE) claim replay ownership; an incoming capture BEGIN remains SAVE so its only copy reaches durable custody. Tests reproduce 54 old assertions per media and pass the fix.

## Completed evidence

The consolidated [host receipt](/Users/MattTaylor/halo-wifi-recovery-2026-09-16/device171/candidate171-001/HOST-VALIDATION.json) binds **45 suites** to canonical 171 runtime. The 39 unchanged regression runs match snapshot inputs except the three explicitly generated version headers; the corrected foreground fixture and five corner suites execute the canonical snapshot. Intermediate failed fixture and missing-argument invocation receipts remain preserved and are explicitly superseded by successful reruns.

- 1,024 paired message schedules, 4,096 timer/provenance cases, 64 dark-boot/user-input sequences and 1,005 retry-policy transitions.
- 1,106 storage cases, including 628 filesystem interruption/reopen cases, 218 budget checkpoints, receipt rejection and automatic capacity recovery.
- Network cancellation at request/response/receipt boundaries, 128 partial-write cases, task ownership, mutex release and reconnect rollover.
- 108 actual camera HTTP-drain timing cases, plus three mutation controls that the test rejects. This covers the drain branch, not physical camera/DMA behavior.
- Full LCD handler/Store tests for user input during LIST, FETCH, ATTEMPT, DELETE and cleanup; 5,760 assertions per media. The foreground suite passes 916 per media, retaining old163 negative controls.

The earlier baseline170 campaign separately passed 24,000 seeded transport cases and 4,275 storage cases. These are host injections, not that many physical device runs.

Both canonical production builds and artifact checks pass. Sense binary: 1,846,368 bytes (unchanged size from170); LCD: 2,030,992 bytes (+2,832). Static RAM and RTC are unchanged. UART task stack remains 12,288 bytes; inspected application-only chains grow to 7,840 bytes voice / 8,416 bytes image. SDK/filesystem/interrupt overhead and runtime high-water remain unmeasured; see the pinned stack review.

## Device state and next step

A fresh read-only USB capture confirms Sense169/app1 SDK VALID and LCD169/app0 SDK VALID. LCD reports provisioning step3, `app_connected=0`, with sleep suppressed. No reset, tap, flash, credentials/NVS/SD mutation, OTA publication or Git remote push occurred in this campaign.

Complete actual setup using Trepo and the device QR code. Then rebind/review the application-only installer to171, preserving both169 fallback banks. The old170 binding must not be reused for171. Validate one online image/voice upload, offline retention, a dark scheduled retry after connectivity returns, touch during retry, ordinary sleep/wake and measured stack high-water. Physical FAT power-loss, fridge RF and USB-free acceptance remain pending.

Legacy/corrupt orphan files without a trustworthy accepted-delete marker remain held; this fix does not infer acceptance and erase them. Cancellation happens at the next safe checkpoint after the current filesystem/SDK call returns.

Authoritative candidate receipt: [CANDIDATE171.json](/Users/MattTaylor/halo-wifi-recovery-2026-09-16/device171/candidate171-001/CANDIDATE171.json). Candidate171 is **not installed or published**. Frozen158 remains immutable recovery; public162 is the separately last-verified OTA release. Allocate unused172+ for new firmware bytes.
