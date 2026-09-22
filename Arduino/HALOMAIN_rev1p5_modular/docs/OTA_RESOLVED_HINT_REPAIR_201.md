# Completed OTA hint repair

The September 21 bench unit still ran paired 6.4.199 after the 200 publication. A new passive capture reproduced `lcd_ota_due from NVS` followed by `policy_target_valid`, with connected Wi-Fi and both boards SDK VALID. Earlier preserved NVS shows a valid RESOLVED 198 policy, empty canonical pending/deferred obligations, and a separate `halo/lcd_ota_due=1`. That hint makes the original admission guard refuse before fetching a manifest; a manual request displays “Update postponed.”

The original hint-writing event is not in the available capture. Both coordinator readiness-timeout paths can set the hint before starting an actual transfer. The fix reconciles this inconsistency without changing those scheduling paths.

## Repair contract

Only a checked canonical RESOLVED campaign can retire the hint. Coordinator storage must be trustworthy, with no pending/deferred work or completion target. The current Sense image must be SDK VALID and at least the resolved target version; equality also requires its exact image hash. The LCD must freshly report a healthy selected/running image at least its resolved version, bound to the same live nonce, owner, boot, partition and deadline. Both transports must be idle and safe; primary user work takes precedence. Bench and one-shot state cannot enter this repair.

The checked NVS setter rechecks the admission predicate around blocking storage work and verifies the erase. Only the hint changes: no policy record, counters, budget, calendar history, schedule or unsafe-transfer marker is reset. Interrupted or uncertain updates remain guarded. Automatic reconciliation ends without another manifest GET; a deliberate manual request may continue through the existing charged admission path.

Different newer board versions are permitted for this already-completed comparison, following the existing manual discovery recovery precedent. This allows a Sense-only USB repair followed by an ordinary LCD update. It does not allow a newer version number to resolve an unfinished older update.

## Bootstrap and validation

Shipping 199 has no supported command that safely retires this orphan hint. The reviewed bootstrap used an inactive-bank Sense USB installation preserving NVS, provisioning, filesystems and the existing VALID bank. That installation is not evidence of a Sense OTA transfer. The finite validation below exercises the normal Settings action, its LCD transfer, a second no-update request, and normal sleep/schedule closure. Do not fabricate a calendar obligation or use a factory image.

Evidence workspace: `/Users/MattTaylor/halo-ota-orphan201-20260921`. `diagnostic01` is the closed pre-repair passive capture; `initial-wake-receipt` records the successful actuator wake. The implementation note above describes the contract; the completed release and device results are recorded below from actual receipts. The production daily schedule remains 02:00 Pacific. EOL firmware selection remains separate.

## Published release and device result — September 21

Source `8ca5b14868074a2eea792b519e5b942b0f7371da`, firmware tree `709687d3dc2018e02bc5eee13a884db4bc18a69c`, local tag `halo-v6.4.201`, build `6.4.201-20260921T233740Z-8ca5b1486807`. Both production latest manifests and complete binaries were published and read back. Continue future work from this source or reviewed descendants; allocate unused202+ after a fresh inventory check. EOL stays197 and historical158 recovery remains immutable. No Git remote push was performed.

| Check | Actual result |
| --- | --- |
| Working-source and exact-snapshot host gates | Both PASS:105 suites each, no skips, unchanged source |
| Focused actual-source hint suite |117 cases /1,658 checks PASS, including interrupted/unsafe/ambiguous state refusal |
| Canonical Sense and LCD builds | PASS with verified artifacts and production flags |
| Sense bootstrap |199/app1 retained;201 installed intoapp0, subsequently SDKVALID. Full NVS bytes unchanged by service; no filesystem, bootloader or policy fixture writes |
| Automatic repair | Verified `lcd_ota_due=0`, then `lcd_hint_retired authority=resolved credit=unchanged` at1790034266; no speculative manifest GET |
| Manual LCD update |199/app0→201/app1; first overall attempt succeeded, with two recovered packet retries; full2,032,864-byte image SHA matched; fresh SDKVALID and new hint cleared |
| Second manual check | Both201 manifests fetched; terminal `up_to_date` after8.832s; LCD cleared holds and coordinator state, returnedHome and both boards slept |
| Schedule | Next nightly start1790067600 (September22,02:00Pacific), LCDwake1790067585; unchanged production calculation |

Both manual cases sent exactly one USB `ota` command into the actual Settings action after fresh identity/Home checks. This does not independently qualify physical button geometry or the rendered text. Sense201 was USB installed, not OTA transferred. The pre-existing blocker prevented installing its own repair through normal discovery, so the service step was necessary on this bench unit. Fresh paired health was read again before the no-update test: Sense201/app0 and LCD201/app1, both selected/running SDKVALID.

Evidence under `/Users/MattTaylor/halo-ota-orphan201-20260921`:

- `release201/RELEASE-RECORD-v2.json`, `DEVICE-ACCEPTANCE-v2.json`, `PUBLIC-READBACK.json`, and `ARCHIVE-RECORD-v2.json` bind exact release identity, scoped hardware acceptance, public byte verification and the verified private archive.
- `regression001/RESULT.json` and `release201/regression/RESULT.json` are the distinct full gates; `release201/build/artifact-result-v2-sense-lcd.json` binds the paired artifacts.
- `service-sense01/service/result.json` records only the USB intervention; `post-service01` adds runtime SDK health and guarded hint retirement. Historical service status intentionally precedes later health observation.
- `manual-lcd201-02` contains the successful LCD transfer; `manual-no-update201-01` contains the subsequent check. Both controllers and captures closed/reaped with paired sleep observed.
- `manual-lcd201-01` stopped before sending an OTA request because the external harness required a cached peer-awake bit. Fresh identity already proved communication; the harness correction accepted either cached bit while retaining fresh nonce/CRC/build/SDK/selected-bank/Home checks. This is not a failed OTA attempt.

## Remaining limits

No new physical cold-power, interrupted-transfer, scheduled timer firing, provisioning, media reliability or full-product pass is claimed. The prior first-wake post-provision voice AES allocation problem is unchanged.

Diagnostic journal operations also logged `orphan_closure result=5` (Corrupt) and `context_conflict result=3` (Busy). These refer to retained diagnostic context, not the OTA hint or transfer result. Journal open/seal later succeeded and the actual OTA checks completed independently. This run does not establish healthy current-campaign diagnostic capture or universal journal corruption; preserve it as a separate diagnostic limitation.

The exact original stale-hint write remains unobserved. Unfinished campaign resolution still requires its existing exact-image checks; this repair is limited to a canonical completed record with no outstanding coordinator work. No allowance, calendar history or unresolved obligation was reset.

The private package revision2 corrects the case of the service receipt filename for restoration on case-sensitive filesystems. Firmware bytes, test results and publication are identical. The first sealed package and receipts remain preserved.
