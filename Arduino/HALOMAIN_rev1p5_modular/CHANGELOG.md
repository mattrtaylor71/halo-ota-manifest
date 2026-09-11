# Firmware changelog

## 6.4.117 — scoped UI/manual OTA fixes, September 10, 2026

Built 116/117 from committed source `1e624b5e8d2f44f8f1c91cbe455df5f2f8d7a66f`. Replace the microphone bitmap background with the existing vector icon and the unsupported Settings separator with ASCII; center shopping labels and reveal the selected row while turning the knob. Manual Software update uses one acknowledged request, a stable checking/result screen, and no sleep/identity/timer re-dispatch. Explicit manual discovery can replenish a later UTC day only after RESOLVED work; failed debt and same-day bounds remain intact. USB `ota` exercises the same Settings action.

Canonical 116/117 builds and focused native/action/LVGL checks passed. The real 116 seven-item list scrolled 0→6/235→0. One Settings-action manual request installed exact 117 on both boards; saved full hashes, SDK VALID, Home, cleared OTA flags and natural sleep passed. Production 117 pointers were promoted and verified. The final same-version request correctly produced `policy_daily_limit`, held the result for eight seconds, returned Home without re-dispatch, and naturally slept. Fresh paired 117 SDK VALID/build evidence passed. Release tag `halo-v6.4.117` and the normal development checkout identify this accepted release; exact bindings are in `RELEASE_BASELINE.json`. No new scheduled or extended-soak qualification is claimed.

## 6.4.115 — published September 10, 2026 for manual OTA testing

Version-only rebuild of the released 6.4.114 runtime from committed production baseline `79ee6ee8695738dc90513da3cc797007edc07d11`. Only generated version/build metadata differs in executable source; production flags, partition layouts, static RAM and RTC usage are unchanged. Both canonical builds and uploaded binary/manifest checks passed; both production latest pointers now serve 6.4.115. The user will perform the manual installation, which has not yet been qualified for these new bytes. No device firmware, scheduling or retained allowance was changed by publication. See `published_manual_test_bumps` in `RELEASE_BASELINE.json` for exact receipts and the saved test-allowance limitation.

## 6.4.114 — released September 10, 2026

A retry could wake within its15-second lead interval while the separate LCD timer-origin notice was still unavailable. The existing fresh clock and peer handshake were ready, but policy cancelled the entire opportunity before its due time. The correction lets a valid persisted shipping ARMED record wait inside that lead interval with a current correlated peer. It preserves the actual missing-origin diagnostic, original deadlines and all target/accounting gates. It returns false before due, so this wait grants no early transfer or reservation.

The native test now starts from the observed due−12s timing with an absent origin rather than assuming that notice was accepted. Exact 113 private and 114 production builds and the portable 114 package have passed verification. Manual 113→114 installation passed, with both exact 114 images SDK VALID, Home/idle and resolved accounting. Scheduled114 installation also passed; final paired health, accounting, Pacific timers and natural sleep were verified and production latest promoted. See RELEASE_BASELINE.json for scope and exact receipts. Version112 was not released; its on-device transfer failures and missed retry remain recorded below.

## 6.4.112 — superseded candidate, never released

A verified early LCD timer notice could be rejected because retained OTA debt had already queued the boot check as `coord_recovery`. The queue preserves its first reason and deadline, but the retry readiness check incorrectly required that reason to equal `lcd_timer`. The correction uses the accepted timer origin and currently queried peer identity, while preserving the original deadline, stored arm identity and all retry accounting.

The native regression fails on the preceding source and passes with the correction. It covers the five queue reasons, ten invalid-origin conditions, clock/storage/expiry/deadline boundaries, and the actual canonical reservation/codec operations. The earlier post-transfer cleanup/arm regression also passes. Exact production112 build and one clean scheduled109→112 installation are the user-directed release gate. Additional fault/repeat runs and a separate final manual reinstall are deferred under the one-hour shipping scope. Candidate108 was never released.

The actual September10 scheduled109→112 run failed during LCD transfer. Its armed retry woke but returned to sleep on109 without a second reservation, with an early readiness snapshot reporting `origin:false`. The queue-label fix is therefore insufficient for hardware recovery. The one-hour deadline was missed; production latest is unchanged, and this candidate has no release tag or production acceptance. Exact cause of the transfer failure remains unconfirmed.

Deadline closure restored Pacific scheduling while preserving valid109 firmware, provisioning and failed-campaign accounting. Saved ring records identify an HTTP no-data abort at835,080 bytes and a later connection-loss abort at304,405 bytes; final policy is DEFERRED/gen8 with no second reservation. The recurring soak is paused. The bounded-readiness correction was subsequently implemented and natively tested in candidate 114.

## 6.4.108 — superseded candidate, never released

The shipping102-to103 calendar case downloaded and acknowledged16924LCD bytes, then stalled; its immediate second HTTP request failed. Both boards retained valid102 images. The prepared five-minute retry closed without a verified arm and was deferred until the next daily wake despite remaining budget. The failed run did not capture the fresh query payload, so its exact rejected predicate is unknown. Source review found two deterministic gates that reject a healthy old LCD: a changed image-begin count and the requirement for a live coordinator lease that ordinary OTA_LOCK clears.

The committed correction preserves confirmed transfer cleanup, fresh nonce/current-boot proof, exact invocation-local old-image identity, selected/running VALID state, competing-owner rejection, user-work guards and existing deadlines/accounting. It permits an exactly empty owner/zero-lease tuple after cleanup rather than treating it as evidence of a failed image. No TLS/network root cause is claimed from the saved socket snapshot.

Reserved validation:105 bootstrap,106 isolated LCD integrity-failure plus autonomous retry,107 clean scheduled repeat,108 exact production manual bridge and Pacific scheduling. All four pairs now pass canonical artifact and independent resource checks from source commit9d38091f2b23341f5a12f423efa5fa71cf0f6978. Controlled 105 setup passed, but the 105→106 scheduled case failed: the transfer stalled at 1,383,690 bytes, a five-minute retry was confirmed and woke both boards, then policy deferred without a second network reservation. Further source correction is required; later acceptance, publication, release tag and original-checkout adoption remain pending. Existing102/103/104 bytes are unchanged;104 is not a production release.

## 6.4.104 — superseded candidate, never released

Versions 6.4.102/103 are staging canaries with shipping policy and a fixed private OTA route. The final 6.4.104 image uses the normal production route; canary validation and exact final-image validation remain separately recorded.

Prepared from the tested M8 source instead of rebuilding the older development checkout without its fixes. The integration includes the bounded shared Ordinary/Diagnostic/Admission POST transport, reduced diagnostic/report stack use, and one checked watchdog enrolment acknowledgement while preserving the global watchdog policy. Durable OTA accounting, clock provenance, paired target checks and normal calendar scheduling remain part of the release source.

The canonical production builder now selects the previously compiled M8 shipping feature profile explicitly and rejects local MQTT credential overrides. The tracked MQTT-disabled configuration contains the existing public CA and empty client credentials; it does not enable MQTT. Bench, one-shot, local diagnostic credential provisioning, idle-network probe and intentional fault controls are excluded from the shipping profile.

Evidence already available: three earlier **bench** scheduled paired installs completed in 318, 303 and 309 seconds, each subsequently SDKVALID/Home with full resolved policy. Shipping 98 and default-off shipping 99 compiled. These do not establish shipping 6.4.104 acceptance. The 101 pair's later supported bench stop and restored Pacific calendar timers are separately archived.

Pending: actual 6.4.104 source commit/tag, both binaries and served manifests, finite shipping acceptance, factory/rollback instructions, production promotion and adoption by the standard development checkout. See `RELEASE_BASELINE.json` and `docs/PRODUCTION_RELEASE_ACCEPTANCE.md`. Optional M9 report-expiry wake and durable outbox work is not included.

For every later version, record the concrete behavior changed, the source commit/tag, exact paired artifact manifest, tests performed and material limitations. Change this entry to released only from actual completion/publication evidence; do not rename a candidate or reuse an immutable version to conceal changed bytes.
