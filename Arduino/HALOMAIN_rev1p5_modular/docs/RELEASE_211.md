# Release 6.4.211 — discard and dish confirmation subtitles

**Current published firmware: 6.4.211; device testing pending.** Discard and dish now use their requested Got it subtitles; 210 automatic check-in is retained. Paired canonical artifacts, all 114 exact-snapshot host suites and complete public downloads passed. The last verified installed pair remains 209 (Sense app1/LCD app0 SDK VALID). Source `731e4897c845e6bb891f277f0e82e210c380150c`, tag `halo-v6.4.211`; future versions require unused 212+ after inventory. Camera, network, OTA and storage behavior are unchanged; prior network/UI lease limits remain open. Factory 197/frozen 158 stay separate.

LCD Got it subtitles now say “Updating your kitchen...” for discard and “Macros available in app” for dish. Check-in wording and 210 automatic check-in behavior remain unchanged. The label resets for each screen presentation; no camera, network, OTA or storage implementation changes.

Source `731e4897c845e6bb891f277f0e82e210c380150c`, firmware tree `fc318755917c05805aeef921793190eaeb54118b`, build `6.4.211-20260922T194351Z-731e4897c845`. Local annotated tag object `2081558b1440f77a96e61871b7fb747bd14115d5`. Publication and device acceptance are separate; no 211 hardware actions or acceptance are claimed.

- [Materialization](/Users/MattTaylor/halo-confirmation211-20260922/snapshot/materialization.json) · SHA256 `bad7df59d9b0908792fa690978f222b3f320f84e00ebf0c691dd64905472c0d8`.
- [Exact snapshot gate](/Users/MattTaylor/halo-confirmation211-20260922/regression/RESULT.json) · SHA256 `48aac0c419b6ec9120417252218058e2e9c471bd5d9f7fe4f6ec1a728de910a7`.
- [Artifact checks](/Users/MattTaylor/halo-confirmation211-20260922/build/artifact-result-v2-sense-lcd.json) · SHA256 `e431166d57f5ed0bbaad4a9bf02892cec9d2c0bb4b4c68d6f7f6678c8bfdb6ad`.
- [Paired promotion](/Users/MattTaylor/halo-confirmation211-20260922/promote001/result.json) · SHA256 `88548fdd5b9882fbbf13a2eb382b3505bb00492e0fa181360b8a178b977c1c1a`.
- [Full public readback](/Users/MattTaylor/halo-confirmation211-20260922/PUBLIC-READBACK.json) · SHA256 `45d9bc478bca8bb49eb6126037c37cd31fb08296a7934fde793f1483093e6fbf`.
- [Source tag](/Users/MattTaylor/halo-confirmation211-20260922/SOURCE-TAG.json) · SHA256 `8fcf33e1caae9cf3537bfa966e8ce3bdfca7d0b3cf76a22d44c23fc56a50c80b`.
- [Retained 209 acceptance](/Users/MattTaylor/halo-release209-20260922/DEVICE-ACCEPTANCE-209.json) · SHA256 `b2b754396bd965a77fa584119f62108f8c523436400dacacd0efe170d822267f`.

211 installation and physical confirmation subtitles are pending; installed 209 evidence remains separate.
Got it acknowledges accepted local upload-queue ownership, not completed cloud delivery.
The prior manifest-wait UI lease issue and first-attempt network reliability remain open; no scheduled or full-product pass is claimed.

After OTA, verify actual running versions, the exact discard and dish Got it subtitles, then check-in to prove its original subtitle resets correctly and its quantity page remains skipped. Preserve discard choice behavior, queue acknowledgment, cloud delivery or retained retry, and native Home/sleep. Do not label a local acknowledgment as an upload completion.

Private complete source/artifact archive: `/Users/MattTaylor/halo-releases/6.4.211.tar.gz`; SHA256 `5f3a0c2074645cb57c64b097fa6a786d594e24b831164341e899965a1d129ae5`, 4166 members fully re-read and verified. This local archive was not uploaded. [Archive receipt](/Users/MattTaylor/halo-confirmation211-20260922/ARCHIVE-RECORD.json).
