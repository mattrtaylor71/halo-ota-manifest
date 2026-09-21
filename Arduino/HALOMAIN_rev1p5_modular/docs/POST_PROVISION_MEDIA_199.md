# Immediate capture after provisioning — candidate 6.4.199

## Problem and observed evidence

On September 21 the user completed office provisioning and recorded “add celery to the shopping list” before the existing 15-second owner-success grace elapsed. Recording succeeded (2.95 seconds, 94,208 bytes) and office Wi-Fi remained connected, but no matching cloud ingress or list insertion was found through 10 minutes 44 seconds after recording. The LCD repeatedly received `op_inflight` sleep denials, then its five-minute guardian consumed ten denials in under a second and forced local sleep using a stale relative maintenance arm.

Private evidence: `/Users/MattTaylor/halo-postprovision-audit-20260921/provision-voice-capture-01/TEST-RESULT.md` and the adjacent source-slice reproduction. The live Sense queue was not directly readable through LCD; the code reproduction establishes a conditional circular dependency matching the observed timing, not every internal state of that incident.

## Scoped correction

- Let the production owner loop finish an already successful provisioning session after its existing owner grace, even when media is queued. Require valid saved ownership, connected provisioning state, completed minimum/grace intervals and idle claim/scan/capture/HTTP/OTA transports. Do not run general provisioning update, owner-claim HTTP, reconnect, initial list fetch or OTA before the user-resource guard.
- Complete the corresponding post-AP bookkeeping with that cleanup so an already verified owner does not create a new claim-retry sleep blocker. Normal unfinished-claim recovery remains unchanged.
- Make the LCD guardian use the same existing passive Sense-idle wait and timed retry backoff as ordinary sleep. Recent actual Sense traffic keeps a denied handshake waiting; an asleep or silent peer releases the wait. Existing OTA/media/provisioning ownership protections and user wake behavior remain.

No change to Wi-Fi credentials, owner identity, stored media, daily schedule, OTA allowance, durable OTA debt, endpoints, capture formats or the 02:00 Pacific calculation.

## Schedule reasoning

Sense's normal loop already services bounded SNTP independently of the production-loop queued-media guard. LCD `clock_valid=0` alone therefore does not establish a Sense clock failure. Finishing setup permits the ordinary sleep/flush path to run. With fresh Sense time, final co-scheduling sends a newly calculated absolute nightly arm and current time to LCD. Preserve the fresh-clock requirement and existing fallback; do not trust an old relative arm as a successful next-02:00 result.

## Required validation

Run focused extracted-source cases plus the complete working-tree and exact-snapshot host gates and paired canonical build/artifact checks. Record results separately rather than treating this plan as acceptance.

Install through normal paired OTA with device settings/media retained. Before the user repeats the case, establish both new running versions and SDK validity, then open a fresh passive LCD log capture. The user provisions and immediately records a distinct shopping-list phrase after returning Home. Require:

1. Recording stats and completed setup cleanup, followed by the normal upload flush.
2. Cloud acceptance tied to the device/account/request and the corresponding exact list insertion.
3. A fresh received/stored maintenance arm for the next 02:00 Pacific and successful final SDK timer selection/committed sleep. For September 21 this is `nightly_20260922`, start epoch `1790067600`, LCD lead target `1790067585`.
4. No rapid guardian denial cascade, forced sleep through the upload, or unexpected visible background wake.

If an earlier legitimate media retry wins timer selection, inspect retention of the independent absolute calendar arm. Host coverage does not replace this hardware result. No scheduled OTA execution, physical cold-power or full-product acceptance is implied by this focused case.

## Status and receipts

Implementation candidate; build, installation and user retest are pending. Workspace: `/Users/MattTaylor/halo-postclaim199-20260921`. Published 198 and EOL 197 remain the recorded selectors until an actual new deployment is documented. Preserve all existing immutable release artifacts.
