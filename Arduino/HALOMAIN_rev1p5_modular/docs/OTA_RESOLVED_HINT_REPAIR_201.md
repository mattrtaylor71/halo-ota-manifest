# Completed OTA hint repair

The September 21 bench unit still ran paired 6.4.199 after the 200 publication. A new passive capture reproduced `lcd_ota_due from NVS` followed by `policy_target_valid`, with connected Wi-Fi and both boards SDK VALID. Earlier preserved NVS shows a valid RESOLVED 198 policy, empty canonical pending/deferred obligations, and a separate `halo/lcd_ota_due=1`. That hint makes the original admission guard refuse before fetching a manifest; a manual request displays “Update postponed.”

The original hint-writing event is not in the available capture. Both coordinator readiness-timeout paths can set the hint before starting an actual transfer. The fix reconciles this inconsistency without changing those scheduling paths.

## Repair contract

Only a checked canonical RESOLVED campaign can retire the hint. Coordinator storage must be trustworthy, with no pending/deferred work or completion target. The current Sense image must be SDK VALID and at least the resolved target version; equality also requires its exact image hash. The LCD must freshly report a healthy selected/running image at least its resolved version, bound to the same live nonce, owner, boot, partition and deadline. Both transports must be idle and safe; primary user work takes precedence. Bench and one-shot state cannot enter this repair.

The checked NVS setter rechecks the admission predicate around blocking storage work and verifies the erase. Only the hint changes: no policy record, counters, budget, calendar history, schedule or unsafe-transfer marker is reset. Interrupted or uncertain updates remain guarded. Automatic reconciliation ends without another manifest GET; a deliberate manual request may continue through the existing charged admission path.

Different newer board versions are permitted for this already-completed comparison, following the existing manual discovery recovery precedent. This allows a Sense-only USB repair followed by an ordinary LCD update. It does not allow a newer version number to resolve an unfinished older update.

## Bootstrap and validation

Shipping 199 has no supported command that safely retires this orphan hint. The bench therefore needs a reviewed inactive-bank Sense USB installation preserving NVS, provisioning, filesystems and the existing VALID bank. That installation is not evidence of a Sense OTA transfer. Afterward, test the normal Settings action, its LCD transfer, a second no-update request, and normal sleep/schedule closure. Do not fabricate a calendar obligation or use a factory image.

Evidence workspace: `/Users/MattTaylor/halo-ota-orphan201-20260921`. `diagnostic01` is the closed pre-repair passive capture; `initial-wake-receipt` records the successful actuator wake. Release and device results must be recorded from actual receipts; this implementation note alone claims neither publication nor device acceptance. The production daily schedule remains 02:00 Pacific. EOL firmware selection remains separate.
