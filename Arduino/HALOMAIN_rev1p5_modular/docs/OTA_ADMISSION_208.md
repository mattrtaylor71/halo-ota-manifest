# OTA admission repair after the September 22 reproduction

**Final September 22 outcome: 208 is published and installed on both boards.** Both 114-suite gates, paired canonical artifact checks and full public binary readbacks passed. Sense 208 was USB bootstrapped with NVS and207 fallback preserved; one ordinary LCD 206→208 OTA and one same-version request passed SDK VALID, terminal cleanup, natural Home and paired sleep. The first transfer's pre-sleep report proved canonical RESOLVED. The no-update Sense comparison line was interleaved and independently corroborated by exact versions and the executed terminal path. Local annotated tag `halo-v6.4.208` pins source `38cd151056242aba29bd07503ba78157e7cb0a97`; no new archive or remote Git push was performed. See [the final 208 handoff](RELEASE_208.md).

This is finite manual acceptance, not Sense self-OTA, a physical scheduled 208 pass or full-product qualification. The [207 functional campaign](FUNCTIONAL_207_20260922.md) retains its separate camera, voice/list and semantic limits. The cause, test plan, actuator incidents and staged-only notes below are **historical checkpoints**; their 207/pending statements describe those earlier times. The final publication/acceptance section at the end supersedes them.

## Historical candidate plan

Candidate work for the next unused release, 6.4.208. Public firmware remains207
until separately recorded publication. Original failure evidence and its limits
are in [the 207 functional campaign](FUNCTIONAL_207_20260922.md).

## Reproduced cause

The bench retained a closed, nondeferred DISCOVERY record with completed201
comparison evidence, no active reservation, no pending calendar owner, and an
LCD-due hint. Both current images were SDK VALID and newer than that completed
comparison. Manual admission nevertheless refused with `missing=0x2880`:
the existing orphan-hint repair accepted only RESOLVED, while DISCOVERY recovery
required a due admitted calendar owner.

At the genuine02:00 wake, the timer and Wi-Fi worked. A second ordering gap kept
recovery blocked: the old hint queued `lcd_due` before the timer notice arrived.
Readiness correctly retained that request's reason/deadline and recognized the
timer, but credit adoption recognized only the `nightly`/`lcd_timer` reason
strings. It therefore never created the due calendar owner.

## Narrow changes

- Extend the checked orphan-hint authority to canonical inactive, nondeferred
  DISCOVERY carrying a valid completed-comparison target, only for explicit
  manual intent. Keep the existing local/peer VALID, completed-version floors,
  fresh locked peer, storage, user-idle, UART and original-deadline checks.
  Retirement preserves the durable record, accounting, history and schedule;
  ordinary manual admission supplies its normal bounded grant afterward.
- Allow existing checked calendar-credit adoption to recognize an accepted,
  correlated, same-boot bound timer origin despite the older queued reason.
  Preserve pending-first handling, due/timezone/history checks, checked save,
  and original deadlines. This does not invent a timer event or clear debt.

Active or unfinished targets, empty/legacy discovery, one-shot/bench state,
pending/deferred/completion obligations and uncertain storage retain their
existing refusal/recovery behavior. Camera, voice, Wi-Fi, provisioning, UI and
production02:00 scheduling are outside this patch.

## Validation and device plan

The focused tests execute production admission/credit functions with host SDK,
UART and persistence doubles. The calendar test retains an old-gate negative
control, follows a timer notice joining an existing `lcd_due` request, and tests
due admission, checked save/reload, replay, deadline expiry and invalid evidence.
The manual test covers the captured closed-DISCOVERY state and must retain all
existing orphan-hint refusal tests. The full registered regression gate and
canonical paired artifact checks are required before publication.

Device acceptance preserves the failed NVS state: install the fixed Sense image
into its inactive application slot, verify fresh health, then request ordinary
manual OTA and observe real LCD transfer, target hash/boot validation, cleanup
and sleep. A subsequent same-version check is separate evidence. This is an
assisted Sense installation, not a Sense self-OTA or a physical scheduled pass.
No erased-NVS, forced allowance, synthetic completion or changed production
schedule is part of this plan. Actual execution results will be recorded after
they exist.

## Actuator recovery

On September 22 the exact replacement Uno accepted a targeted USB-device reset.
A fresh HELP/STATUS probe then passed; one calibrated stroke woke the installed
Sense207/LCD 206 pair, fresh Home/VALID identities passed, and both slept.
Evidence is under `/Users/MattTaylor/halo-ota208-20260922` in
`actuator-reset01`, `actuator-after-reset-probe.json` and
`actuator-recovered-health01`. No reflash was needed. This is recovery of this
occurrence, not proof that the intermittent USB fault is permanently fixed.

## Historical September 22 build and installation checkpoint

The fix is committed at `38cd151056242aba29bd07503ba78157e7cb0a97`, firmware
tree `5a1c3632d5ea6e8cb5e855689c24fa1fa48e4191`. Both complete **114-suite**
gates passed, including the exact materialized release snapshot. Both canonical
builds and the camera-aware artifact checker passed. Build identity:
`6.4.208-20260922T100632Z-38cd15105624`.

| Image | Bytes | SHA256 |
| --- | ---: | --- |
| Sense | 1,874,944 | `2f254cdaa2e92ab8d727f2bfdaa6e4a2a9159d8091c2e1ed5ce3cfa048a63951` |
| LCD | 2,032,960 | `421abd322036615ec9a1d810b3fe77724c2a188fbfca476371c4fbf63120a128` |

The first service controller stopped before commands or flash because the unit
slept before capture attached. The second obtained fresh identity and installed
Sense 208 into app0. It verified the candidate bytes and preserved NVS, the
current207 app1 fallback, partition table, filesystem and old VALID selector.
LCD remains206/app0. Native208 boot was observed marking the image VALID,
communicating with LCD, deferring automatic `lcd_due` as `policy_not_due`, and
later sleeping. The full requested Home/nonce health check remains incomplete.

Independent strict decoding of the preserved NVS confirms that the original
generation52 closed DISCOVERY policy blob is unchanged, with completed201
comparison, `lcd_ota_due=1`, unsafe=0 and no pending/deferred/completion owner.
The next calendar schedule advanced normally to September23 at02:00 Pacific.
No debt, quota, target or schedule was forced to make a test pass.

Only the four immutable208 release objects were staged and read back. **Latest
manifests remain207;208 is not promoted.** Version208 is now reserved by those
immutable bytes; a changed build must use a freshly inventoried unused209+.
Current release selectors still describe published207. Continue development
from38cd151 or descendants so these repairs are retained.

### Historical remaining physical test and actuator failure

The actuator subsequently failed during the post-install wake: it returned
HELP/STATUS and PUSH progress through retraction, but no completion or STOP
acknowledgement. Halo actually woke and slept; the health controller correctly
stopped before issuing queries because actuator completion was unproved.
All descriptor owners and processes eventually closed and were reaped.

A subsequent live-serial USB probe timed out before reset. A separately reviewed
helper bound the same Uno through fresh registry serial/VID/PID/location/address
and repeated that identity after open; its single exact-device reset succeeded.
The following nonmoving probe still received no HELP. A bounded avrdude reset
and bootloader/signature probe also failed synchronization; **no actuator flash
write occurred**. This recurrence does not establish an electrical root cause
or permanent hardware failure. No hub reset, stronger stroke or blind repeated
movement was used.

The real manual208 transfer and same-version check have **not run**. Resume with
the prepared health208 controller after a manual wake or restored actuator,
then promote the exact staged208 pair, run the one-request manual208 observer,
and require native `lcd_hint_retired authority=closed_discovery`, charged manual
discovery, verified LCD208/app1 postboot, settlement and sleep. A successful
transfer alone does not prove the stale-record branch ran. The physical timer
path also remains unqualified; the new host regression reproduces the original
calendar ordering failure and tests its guarded correction.

Private evidence root: `/Users/MattTaylor/halo-ota208-20260922`. The release
checkpoint `release208/QUALIFICATION-PENDING.json` hashes the gates, artifacts,
stage, service and failure receipts. `service208-02/INDEPENDENT-SERVICE-OTA-REVIEW.json`
records the preserved-state audit. `service208-prep/HEALTH208.md` and
`ota-prep/README.md` contain the prepared next-test commands. Raw NVS/serial
evidence remains private and is not a public release asset.


## September 22 publication and first manual acceptance

This later checkpoint supersedes the staged-only and actuator-blocked status above. After the user physically restarted the actuator, fresh paired health passed. The exact staged 208 resources were conditionally promoted from 207 and both full public binaries matched canonical proofs.

One ordinary manual action on Sense 208/LCD 206 retired the original closed-DISCOVERY hint with credit unchanged, received the normal charged grant and transferred the exact LCD 208 image into app1. Both boards were SDK VALID; Home, cleared transport ownership and paired sleep were captured. The exact pre-sleep report at 1790097934 (ingested 1790097936, boot 2262) records RESOLVED phase 8/generation 57, storage READY, reserved 0 and the 208 target. `manual208-01/FIRST-OTA-ACCEPTANCE.json` binds the closed capture, target bytes and independent policy review. Sense 208 remains a USB bootstrap, not a self-OTA pass. The subsequent same-version request also passed matched `up_to_date`, natural Home and paired sleep. Its literal Sense comparison line was interleaved; exact current/fetched versions and the executed terminal path corroborate that result. No post-case canonical phase is inferred for this new read-only discovery. Local annotated tag `halo-v6.4.208` pins source38cd151. No new archive or remote push was performed. Calendar adoption has host regression coverage, not a new physical scheduled pass.

`QUALIFICATION-PENDING` remains an immutable historical checkpoint; `release208/RELEASE-RECORD.json` and `release208/DEVICE-ACCEPTANCE.json` now supersede it. Current release selectors now require source 38cd151 or descendants and freshly inventoried unused 209+ versions. See [the 208 handoff](RELEASE_208.md).
