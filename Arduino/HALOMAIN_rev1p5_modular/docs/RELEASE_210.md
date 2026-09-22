# Release 6.4.210 — direct check-in acknowledgment

**Current published firmware: 6.4.210; device testing pending.** Check-in now skips the quantity page and proceeds to the existing Got it screen after queue acceptance. Paired canonical artifacts, all 114 exact-snapshot host suites and complete public downloads passed. The last verified installed pair remains 209 (Sense app1/LCD app0 SDK VALID). Source `6eb91c7bed8bc89eed83ca785108b1f0a53deac3`, tag `halo-v6.4.210`; future versions require unused 211+ after inventory. Camera, network, OTA and storage behavior are unchanged; prior network/UI lease limits remain open. Factory 197/frozen 158 stay separate.

Sense check-in skips quantity/expiry input, queues quantity 1 with no manual expiry, then uses the existing Got it screen. LCD runtime, camera capture, network, OTA and storage implementations are unchanged.

Source `6eb91c7bed8bc89eed83ca785108b1f0a53deac3`, firmware tree `8fb9086a91025d55438844d62444f3338828147f`, build `6.4.210-20260922T191011Z-6eb91c7bed8b`. Local annotated tag object `9d086b80f70a16f216a058a7f487ed00e97f7be8`. Publication and device acceptance are separate; no 210 hardware actions or acceptance are claimed.

- [Materialization](/Users/MattTaylor/halo-checkin210-20260922/snapshot/materialization.json) · SHA256 `a56cbc510411272098beee9c324496fe1fc4e51f9b192fb3fb164d204d557005`.
- [Exact snapshot gate](/Users/MattTaylor/halo-checkin210-20260922/regression/RESULT.json) · SHA256 `5964318fc5f57d3078d120e9cf8a94c04958eae82897d069198dd2a379a9b186`.
- [Artifact checks](/Users/MattTaylor/halo-checkin210-20260922/build/artifact-result-v2-sense-lcd.json) · SHA256 `52bfefcc198e066fc582ec500f5e40cbf1b871758e9cd86a1ca4d7bb15542f4d`.
- [Paired promotion](/Users/MattTaylor/halo-checkin210-20260922/promote001/result.json) · SHA256 `edea767a7153a40f366451134a91f99d205e928f17835b10ed0afaf8f4945343`.
- [Full public readback](/Users/MattTaylor/halo-checkin210-20260922/PUBLIC-READBACK.json) · SHA256 `b700acdc65ad301ca60456d9d513a2f3b1f90895b9cc895046187bc7b787c742`.
- [Source tag](/Users/MattTaylor/halo-checkin210-20260922/SOURCE-TAG.json) · SHA256 `b446ef85c5d17fe1eebb462347fe6bc377466c0498085eee99ed90af7aab4622`.
- [Retained 209 acceptance](/Users/MattTaylor/halo-release209-20260922/DEVICE-ACCEPTANCE-209.json) · SHA256 `b2b754396bd965a77fa584119f62108f8c523436400dacacd0efe170d822267f`.

210 installation and physical check-in behavior are pending; installed 209 evidence remains separate.
Got it acknowledges accepted local upload-queue ownership, not completed cloud delivery.
The prior manifest-wait UI lease issue and first-attempt network reliability remain open; no scheduled or full-product pass is claimed.

After OTA, verify actual running versions, one check-in without a quantity page, one queue acknowledgment followed by Home, cloud delivery or retained retry, and native sleep. Preserve discard choice behavior. Do not label a local acknowledgment as an upload completion.

The private source/history, exact binaries, build proofs, host evidence and publication receipts are archived at `/Users/MattTaylor/halo-releases/6.4.210.tar.gz` (SHA256 `f45c09b252e7f756651e2f5775912444ba23d530e065ba29797f0c1bc9795f5a`). All 2905 members were read back and verified; the archive was not uploaded. Local source and release documentation are committed; no remote Git push was performed.
