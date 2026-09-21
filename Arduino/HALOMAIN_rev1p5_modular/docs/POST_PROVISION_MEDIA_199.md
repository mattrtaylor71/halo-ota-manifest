# Immediate capture after provisioning — published 6.4.199

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

Install through normal paired OTA with device settings/media retained. Before the user repeats the case, establish both new running versions and SDK validity, then open a fresh passive LCD log capture. The user provisions, immediately records a distinct shopping-list phrase after returning Home, then leaves the device untouched. Immediate voice followed by staying on the shopping list during the remaining setup grace is not claimed fixed; that still defers cleanup until list activity ends. Require:

1. Recording stats and completed setup cleanup, followed by the normal upload flush.
2. Cloud acceptance tied to the device/account/request and the corresponding exact list insertion.
3. A fresh received/stored maintenance arm for the next 02:00 Pacific and successful final SDK timer selection/committed sleep. For September 21 this is `nightly_20260922`, start epoch `1790067600`, LCD lead target `1790067585`.
4. No rapid guardian denial cascade, forced sleep through the upload, or unexpected visible background wake.

If an earlier legitimate media retry wins timer selection, inspect retention of the independent absolute calendar arm. Host coverage does not replace this hardware result. No scheduled OTA execution, physical cold-power or full-product acceptance is implied by this focused case.

## Status and receipts

Published for both boards; paired installation and the user retest remain pending at this documentation checkpoint. EOL factory selection remains 197. No new private archive or device acceptance is claimed. Preserve prior immutable releases, including 198. Future work must descend from this source and allocate unused 200+ after a fresh inventory check.

| Identity | Value |
| --- | --- |
| Source | `1f5b952284b0ca9732a23dad5e00d73c927b461d` |
| Firmware tree | `a3a43b5208b6b3fe9b82a126cd02786220c69ec8` |
| Build | `6.4.199-20260921T162738Z-1f5b952284b0` |
| Local source tag | `halo-v6.4.199` |
| Sense application | 1,869,008 bytes; `f4618ec34281261b8ad4793dd77011ed6a97f711038b7dcc1ecbab0b769393b6` |
| LCD application | 2,030,128 bytes; `ac688e5d8dff56e39fffd5b96fa0f7a39dfd8caed2728d95b248139be9c30fcd` |

Receipts under `/Users/MattTaylor/halo-postclaim199-20260921`:

- `snapshot/materialization.json`: exact committed source and build inputs.
- `working-host001/RESULT.json` and `regression/RESULT.json`: both complete 104-suite gates passed without skips.
- `build/artifact-result-v2-sense-lcd.json` and board `artifacts/verified.json`: paired canonical build/artifact checks passed.
- `promote001/result.json`: paired production promotion verified. Full public binary readbacks `get-003.body` and `get-005.body` match the application hashes above; final latest-manifest readbacks are `get-010.body` and `get-008.body`.

Machine-readable receipt hashes and current source selection are in [RELEASE_BASELINE.json](../RELEASE_BASELINE.json) and [PRODUCTION_BASELINE.json](../PRODUCTION_BASELINE.json). Later installation/retest evidence must be recorded separately before changing the pending acceptance status.

## Installation block observed September21

The assembled test unit remains Sense198/app0 and LCD198/app1, both SDK VALID. Two Settings-action requests did not start a transfer; a matching fresh cloud report records `policy_deferred` while the retained nightly_20260920 target198 record is RESOLVED. The exact legacy coordinator predicate is not visible through existing LCD/cloud diagnostics. No policy, quota, NVS or media was cleared. Sense USB was requested for application-only service;199 user acceptance remains pending. See `/Users/MattTaylor/halo-postclaim199-20260921/install-01/INSTALLATION-STATUS.md` and its raw/cloud receipts.

A later ordinary wake on198 successfully refreshed and armed the next02:00 calendar timer. This does not qualify the failing immediate post-provision path or199 hardware acceptance. Remaining on the shopping list during the setup grace deliberately retains foreground priority; the focused user retest should record and then leave the unit untouched.
