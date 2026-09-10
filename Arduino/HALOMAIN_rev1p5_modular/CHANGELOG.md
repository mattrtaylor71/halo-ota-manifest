# Firmware changelog

## 6.4.112 — successor candidate, not yet released

A verified early LCD timer notice could be rejected because retained OTA debt had already queued the boot check as `coord_recovery`. The queue preserves its first reason and deadline, but the retry readiness check incorrectly required that reason to equal `lcd_timer`. The correction uses the accepted timer origin and currently queried peer identity, while preserving the original deadline, stored arm identity and all retry accounting.

The native regression fails on the preceding source and passes with the correction. It covers the five queue reasons, ten invalid-origin conditions, clock/storage/expiry/deadline boundaries, and the actual canonical reservation/codec operations. The earlier post-transfer cleanup/arm regression also passes. Successor 109–112 builds and hardware acceptance are pending; 108 was never released.

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
