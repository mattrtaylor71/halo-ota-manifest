# OTA admission repair after the September 22 reproduction

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

On September22 the exact replacement Uno accepted a targeted USB-device reset.
A fresh HELP/STATUS probe then passed; one calibrated stroke woke the installed
Sense207/LCD206 pair, fresh Home/VALID identities passed, and both slept.
Evidence is under `/Users/MattTaylor/halo-ota208-20260922` in
`actuator-reset01`, `actuator-after-reset-probe.json` and
`actuator-recovered-health01`. No reflash was needed. This is recovery of this
occurrence, not proof that the intermittent USB fault is permanently fixed.
