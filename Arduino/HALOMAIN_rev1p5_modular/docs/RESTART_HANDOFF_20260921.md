# Restart handoff — September 21, 2026

The user is restarting the laptop. Capture is closed, and no capture/service,
actuator, esptool or Arduino builder process was found during the restart check.
No device work needs to finish before restart. Resume from this checkout:

`/Users/MattTaylor/halo-camera-recovery-2026-09-15`

Branch: `codex/halo-production-baseline-197` (the branch name is historical).
Current firmware is **6.4.199**, not 197. Read `AGENTS.md`,
`PRODUCTION_BASELINE.json` and `docs/POST_PROVISION_MEDIA_199.md`.

## Installed and published state

- Both exact published 199 applications are installed. Sense runs app1/VALID;
  LCD runs app0/VALID. Source commit is
  `1f5b952284b0ca9732a23dad5e00d73c927b461d`, build
  `6.4.199-20260921T162738Z-1f5b952284b0`, tag `halo-v6.4.199`.
- Both complete 104-suite gates and canonical production builds passed before
  publication. EOL factory selection remains 197.
- Installation was application-only USB service, **not a successful OTA**.
  Old VALID 198 banks, NVS/settings, filesystems and stored OTA debt were retained
  and verified. No factory erase, allowance reset or queue clear occurred.
- New firmware must use unused 200+ after a fresh inventory, retain current
  ancestry, and follow the normal committed-source build/test/publication gates.
  Never overwrite published 199 or substitute old production source.

## Latest real test and next priority

The user reprovisioned and immediately said “Add celery to the shopping list.”
199's setup cleanup worked with a queued recording at the existing 15-second
owner grace. The old setup/queued-media circular dependency did not recur.

The two immediate voice POST attempts failed with explicit
`esp-aes: Failed to allocate memory`. The 90,112-byte recording was committed to
LCD SD, retained across sleep and automatically retried. That retry uploaded the
exact request/audio hash in 4.701 seconds, received HTTP 202 and then retired the
SD slot. Backend confirmed one worker attempt, transcript and Celery list row
79341. The total delay was about six minutes.

**Next priority: diagnose and fix the first upload's memory pressure immediately
after provisioning.** The failing wake could not reacquire the 16 KiB camera
reserve (largest block 12,788 bytes). The successful clean-boot retry had the
reserve available and released it before upload. Exact failed allocation size,
capabilities, site and competing allocation are not yet measured.

Do not misread positive `tls_err=48`: the Arduino library retained successful
socket descriptor 48 as its last-error value. Existing media secure writes
already cap chunks at 512 bytes. A speculative 16 KiB-to-stream chunk rewrite is
not established as a fix. The read-only report proposes a fixed failed-allocation
latch and owner-local memory/progress snapshots; do not allocate/log from the
allocation-failure hook itself.

Both automatic retry wakes stayed dark in all 109 captured LCD heartbeats. The
empty-queue confirmation cleared media retry and both boards committed sleep with
the next 02:00 Pacific schedule: Sense epoch1790067600, LCD lead1790067585.
This verifies scheduling/arming, not execution of a scheduled OTA.

## Separate unresolved findings

- Saved policy is RESOLVED for198/198 with no pending coordinator ID, but
  `halo/lcd_ota_due=1`. This stale hint makes policy entry refuse manual discovery
  before a manifest GET. 199 does not fix it. Preserve debt and interrupted-update
  protections; do not host-clear the hint or bypass exact target proof merely
  because a numerically newer application is now installed.
- One GPIO9 ISR during the first sleep teardown caused a brief abort/relight.
  A real brief touch versus an internal/controller edge is unproven. Standby
  occurred later and did not cause that abort. Preserve genuine touch handling.
- The actuator worked during installation but stalled on an extra wake attempt.
  That failed check is retained. No retry owners remained at restart check.
  Use canonical bounded `tools/tapctl.py`; do not reuse historical relay helpers
  or increase calibrated travel. The last observed actuator was USB21301.

## Evidence and recovery locations

All paths below are under `/Users/MattTaylor/halo-postclaim199-20260921`:

- `INSTALL-RESULT.json`: both application services, exact bytes and protection.
- `reprovision-voice199-01/TEST-RESULT.{json,md}`: finite user test and acceptance
  limits; raw Sense/LCD logs and closed capture receipt are adjacent.
- `reprovision-voice199-01/backend/AUTO-RETRY-FINDINGS.md`: independent cloud/list
  reconciliation and exact request/audio identity.
- `voice-transport-readonly/FINDINGS.md`: memory/transport source investigation.
- `policy-readonly/FINDINGS.md`: exact retained OTA guard and safe fix boundaries.
- `reprovision-voice199-01/lcd-sleep-audit/README.md`: ISR/timer findings.
- `service-sense01/service` and `service-lcd02/service`: private device backups.
- `snapshot`, `build`, `regression`, `promote001`: immutable release evidence.

Backups and raw logs are private. They may contain device/user configuration;
do not publish them. Documentation/results were committed locally through
`d5a83a4` before this handoff; no Git remote push was performed. Check disk space
again after restart before any build. Start a new capture for the next test—the
previous capture is deliberately closed.
