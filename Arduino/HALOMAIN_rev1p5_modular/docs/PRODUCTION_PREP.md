# HALO — production prep

Written 2026-08-21, after the reliability pass. The firmware side is in good
shape: all five ship gates pass and the defects found this week are fixed and
verified on hardware. What follows is everything between "the firmware is good"
and "we can put these in users' hands", ordered by what blocks what.

`docs/SHIP_CHECKLIST.md` remains the per-build gate. This document is the
one-time work to stand production up.

---

## 0. Blocking — nothing ships past this

### 0.1 OTA channel — FIRMWARE DONE, bucket policy still needed

**Done 2026-08-21:**
- `OTA_DEFAULT_ENV` now defaults to `"prod"`; dev is opt-in via build flag.
- `publish_both.sh` derives the flag from `--channel`, so channel and compiled-in
  env cannot disagree.
- `ManifestClient.cpp` DNS sanity now resolves the host actually being fetched
  (it probed the literal dev host before).
- Verified on hardware:
  `[OTA_CFG] resolved_manifest_url=https://halo-ota-prod.s3.../halo/ota/prod/manifest_latest.json`,
  `DNS sanity host=halo-ota-prod... ok=1`, and a missing manifest degrades
  gracefully (`manifest_err`, no crash, no retry storm).

**STILL NEEDED — one AWS change and one publish:**

`halo-ota-prod` has **no bucket policy**, so devices get `403`. Dev works because
it has a public-read policy on the OTA prefix. Prod needs the equivalent:

```bash
aws s3api put-bucket-policy --bucket halo-ota-prod --profile trepo --policy '{
  "Version": "2012-10-17",
  "Statement": [{
    "Effect": "Allow",
    "Principal": "*",
    "Action": "s3:GetObject",
    "Resource": "arn:aws:s3:::halo-ota-prod/halo/ota/*"
  }]
}'
```

Not applied here on purpose: making a bucket world-readable is a deliberate
security-posture decision, and it is the same class of exposure as the key leak
found this week. It mirrors dev exactly, but it should be an explicit call.

Then publish a real prod manifest:
`./publish_both.sh --version X.Y.Z --channel prod --bucket halo-ota-prod --profile trepo`
and re-verify a device updates from it. Note the 3 stale objects already in that
bucket sit under a copy-pasted `halo/ota/dev/` prefix and should be cleared.

### 0.2 Revoked certificate follow-through

The leaked AWS IoT certificate `abf12dd3…` is **INACTIVE** and its key is out of
the source tree and the binaries. Remaining:

- Decide delete vs leave INACTIVE (leaving it preserves the record; it cannot
  authenticate either way).
- **Provision a new certificate before MQTT is ever re-enabled.** The firmware
  currently ships with `HALO_MQTT_DISABLED=1` and empty credentials, which is
  deliberate — do not paste a fresh key in until MQTT is actually needed.
- Audit the two other ACTIVE certs (`9294bae1…`, `cad6aa09…`): know where their
  private keys live and confirm it is not a repo.
- Enable IoT logging (`aws iot set-v2-logging-options`). Its absence is the only
  reason "was the leaked key ever used?" has no answer.

### 0.3 Delete or keep the public branch

`lcd-backlight-binary` on the public `mattrtaylor71/halo-ota-manifest` is where
the key was exposed. Revocation fixed the leak; removing the branch is tidying.
It is ~163 commits ahead of `master`, so check nothing lives only there.

---

## 1. Must happen before the first customer unit

### 1.1 Measure idle current

Never measured in this pass, and this pass **added** sleep-blocking conditions
(`spool_rx`, `spool_tx`) plus a per-boot SD probe. The entire product thesis is
"mostly off", and there is currently no number behind it.

Measure: deep-sleep draw both boards, average draw over a realistic day, and the
cost of one nightly maintenance wake. Compare against the battery to get a real
runtime figure.

### 1.2 Overnight soak with a real 02:00 maintenance wake

Everything verified this week was minutes-long and tap-driven. The nightly path
was proven by a *manual* `INPUT_OTA_CHECK`, not by the device waking itself at
02:00 and going back to sleep.

Confirm: one wake, one manifest check, correct apply/skip, back to sleep, and
that a *missed* night self-heals the next night.

### 1.3 First-unit provisioning on the pinned-CA path

The owner-claim path uses `setCACert(kAmazonRootCa1)` and has **never executed on
hardware**. What is verified: the pinned CA is in the binary, the claim endpoint's
chain validates against that root alone (openssl), and the *same* pinned-CA code
in `ManifestClient` works live. That is strong but indirect.

Provision a factory-fresh unit end to end. Pinning fails **closed**, so if this is
wrong, no device can ever be claimed.

### 1.4 Confirm the new telemetry actually lands somewhere

Two signals were added this week that only matter if someone sees them:

- `SD_HEALTH` / `sd` in `LCD_DIAG` — a dead SD card means every failed upload
  loses a photo.
- `PHOTO_LOST` (`SENSE_DIAG area=upload event=spool_failed`).

Verify both reach the dashboard, and put an alert on them. A dead card is silent
to the user by construction.

---

## 2. Known-open, with a recommendation

| Item | Recommendation |
|---|---|
| **Camera: ONE reliable capture per boot** | **Recommendation WITHDRAWN and replaced.** Earlier advice ("ship it, ~1 in 12") was based on a bad sample. Measured: first capture of a boot **6/6 ok**; subsequent captures in the same session **60% fail** (n=5). The first TLS permanently fragments the DMA region for that boot. **This is a product decision:** if a session is one capture, it is near-invisible; if users scan several items in a row, item two fails and retrying inside the session does not help. The fix is the spool-first decoupling `lcd_sdspool.h` already prescribes — the spool half is built and verified. |
| **Two independent LCD sleep paths** | Fix before adding features. Not a live defect, but it is *why* the spool bug existed, and it will cause the next one. Collapse into one gate. |
| **UART RX has no drop counter** | Cheap, do it. ~89ms of headroom at 115200; the counters already exist in `[UART_DIAG]` but are chatty-gated. Zero parse errors observed, so not currently biting. |
| **`ota_orch` logs ERROR on success** | Cosmetic, but it pollutes the errlog and will mislead whoever reads it first. |
| **Backend POSIX TZ at owner-claim** | Blocks correct nightly timing outside Pacific. Firmware side done. Installed fleet needs a backfill path — devices never re-claim. |

---

## 3. Release mechanics

**Always publish via `publish_both.sh --version X.Y.Z`.** It regenerates
`Version.h` (version, build date, git hash) and syncs it into both sketch dirs.
Building with `arduino-cli` directly leaves the metadata stale — the binaries
tested during this pass still identify as `6.1.815 / Aug 14 / 36036d5`.

**Consequence worth stating plainly:** the published artifacts are a *rebuild*
with a new version string, so they are not byte-identical to what was tested.
Re-run SHIP_CHECKLIST §1 and §2 against the published binaries, not the bench
ones.

Still to define:
- Version scheme for the first production release (6.1.x is a long dev line).
- **Rollback plan.** The nightly updater applies whatever the manifest says; there
  is no tested path for "we shipped a bad build". Decide before the first push.
- Staged rollout — the nightly design gives no cohort control today.

---

## 4. What is already done

Recorded so nobody re-verifies it:

- Ship gates §0–§4 pass; both boards flashed and running the current build.
- **SD spool fixed and verified end to end** — it had never worked for a real
  capture. Capture → forced upload failure → `SD spool OK saved=1` → drain →
  `PUT 200` → deleted.
- **SD health surfaced** — `[SD_HEALTH]` + `LCD_DIAG`, one report per boot,
  delivery verified 4/4 (was 1/19 when reported at pre-sleep).
- **Result-screen strand fixed** — 0 guardian force-sleeps across a 12-capture soak.
- **Nightly path proven on hardware** — pinned-CA TLS, both manifests,
  `proxy_result=up_to_date`, correct next-02:00 target.
- **Leaked credential closed** — certificate INACTIVE, key removed from 4 source
  files, 5 `MqttSecrets.local.cpp` copies and both binaries; `check_secrets.sh`
  added as gate §0.
- Core-0 LVGL violation contained under the existing lock.
