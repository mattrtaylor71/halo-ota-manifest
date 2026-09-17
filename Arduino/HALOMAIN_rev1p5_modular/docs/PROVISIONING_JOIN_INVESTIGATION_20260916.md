# Provisioning join investigation — September 16, 2026

Current installed diagnostic is private173, source `0a9576b5ff56d0120407df1f2848ba298cb40d7a`, build `6.4.173-20260917T043640Z-0a9576b5ff56`. Both canonical builds/artifact checks and seven focused suites pass with172's49-suite lineage retained. Sense/app1 and LCD/app0 are SDK VALID;172 fallback banks/NVS are preserved. Samsung authentication fails before and after an explicit same-credential AP restart. No authentication fix or publication is claimed. Continue from173 or reviewed descendants, preserving171/172 fixes; unused174 diagnostics are in development. Read `docs/PROVISIONING_JOIN_INVESTIGATION_20260916.md` and `RELEASE_BASELINE.json.current_working_source`. Keep the separately recorded late-arrival setup sleep race unresolved.

## Confirmed icon defect and source fix

The user's IMG_8787 photo matches the source: step2 created the gold profile badge
but never drew a person in it. The badge now draws a head and shoulders using the
existing dark ink, without another object, font, image buffer or layout change.
The actual LVGL8.3.6 software-render test passes: visible silhouette, unchanged
border/layout, zero child objects and no retained allocation over50 redraws.
The old empty-badge negative control fails. This is not panel qualification.

## Current phone-join evidence

The user saw the exact setup SSID advertised by Sense169 and re-scanned the
current QR. The iOS system alert named that same network and said it could not
join. A direct iPhone Settings join using the password captured at this session's
startup returned "Incorrect password." This bypasses the app but does not prove
which side rejected authentication or that the live driver configuration still
matches the startup password.

Passive capture003 spans the reproduction. Sense continued reporting ap_setup;
no AP-client connect/IP event was captured. Normal STA disconnected diagnostics
are not evidence that the setup AP itself is down. No board reset, firmware
installation, Wi-Fi scan or radio test command was used for that capture.

The installed169 sender writes the original JSON to UART before redacting a
separate copy for USB logging. LCD password buffers are65 bytes; generated
passwords are12 alphanumeric characters. No sender/cache/parser mismatch was
found. A separate host campaign exercised the real generator and148px LVGL QR:
Apple Vision decoded32/32 exactly and the actual Swift parser preserved all32
SSID/password pairs. OpenCV missed one code but decoded no incorrect payload.
These are synthetic software pixels, not a decode of the actual device screen.

After preserving the failed session, one INPUT_RESET_WIFI was sent through the
LCD, using the existing setup-reset path on the already-unprovisioned unit.
Capture004 records AP teardown, fresh credentials and successful AP startup.
This also clears setup identity fields by the existing reset contract; they were
already unset. It is a recovery experiment, not proof of a fix. Phone association
and full setup after this fresh session remain to be established.

## Separate reproduced ownership gap

An old normal STA connection attempt could survive entry into provisioning.
When its guard deadline expired, wifi_guard_poll could scan or force STA-only,
disrupting the setup AP. The poll now retires that obsolete owner and returns
while provisioning owns the radio. It does not change credentials, radio mode,
failure counters or provisioning's own connection attempt.

The actual-function regression passes1543 checks across status/deadline/rollover
and ownership boundaries; frozen171 fails553 of them. Four existing Wi-Fi/media
network suites pass. This conditional defect has not been established as the
cause of the reported join failure: observed169 logs show inflight=0.

## Private evidence

Raw captures and startup logs can contain credentials; keep them local and out
of release archives/publication. Evidence root:
`/Users/MattTaylor/halo-provisioning-review-2026-09-16/join-failure-20260916`.

- `profile-icon-render001/RESULT.json`: actual badge render and negative control.
- `qr-roundtrip001/run002/RESULT.json`:32-case QR/Apple decoder/Swift parser proof.
- `capture001` and `capture002`: pre-retry diagnostics; the guide bypasses the old
  QR widget's plaintext logging, so these do not recover the actual QR password.
- `capture003`: original phone/direct-join failure capture, closed normally.
- `capture004`: fresh setup-network recovery experiment.
- `/Users/MattTaylor/halo-wifi-recovery-2026-09-16/provisioning169-review/OWNER-FIX-REVIEW.json`:
  ownership reproducer, negative control and four unchanged regression suites.

Root also reran test_lcd_provision_flow.py and
test_provisioning_display_status.py successfully. No new device acceptance or
public OTA release is claimed.

## Follow-up source for172: setup sleep and live driver diagnostics

The phone also failed to join the later2752 session. Capture005 proves an
intervening power-on reset; its cause is not established. The Mac remains on
Garage Member and must not be switched for this test: the user will connect
the phone. Capture005 ended with LCD USB disappearance/OSError at302 seconds;
capture006 independently confirms Sense169 remained alive advertising2752.

The LCD five-minute guardian bypassed ordinary setup gating. An unsynchronized
link then admitted a sleep handshake despite fresh Sense traffic, and repeated
provisioning denials exhausted the ten-denial escape. These paths were unchanged
168 to169. The new guard protects live provisioning at sleep entry, throughout
the handshake, and before UI teardown, while retaining the8-second stale-peer
escape. The actual-function suite passes15/15; installed169 fails12/15. Its
22-call negative control uses the real deny-max10 and produces20 requests and
2 teardown admissions; the fixed source produces neither. Seven existing
sleep/liveness/provisioning suites also pass. This explains disappearing setup
UI, not the earlier authentication failure.

Sense now logs read-only SDK AP configuration at startup and on
INPUT_PROVISION_DIAG: driver/manager credential equality, auth/cipher/PMF,
channel, IP and clients. No password is printed by this diagnostic. AP client
disconnect reasons preserve16bits and are distinguished from home-STA reasons.
The detailed output currently requires Sense USB, although the command can
arrive through LCD forwarding.224 host checks pass under ASan/UBSan and four
focused regressions pass. SDK getters are not an atomic snapshot; rejection
before association may produce no AP-disconnect event.

Evidence: join-failure-20260916/lcd-sleep-guard001/RESULT.json and
/Users/MattTaylor/halo-wifi-recovery-2026-09-16/provisioning169-review/driver-diag002.
Two bounded native JTAG observation attempts were closed without explicit halt,
reset or flash commands; the live memory read refused because the CPU was not
halted. Capture006 confirms continuous uptime afterward. No password was
recovered that way; further diagnosis uses the firmware's SDK getters.

These changes retain all171 media/retry fixes and e58a4fb profile/STA ownership
fixes. The final172 build,49 host suites and installation/paired SDK health pass.
Actual phone association and setup completion remain pending. Preserve169 fallback banks and NVS.
Do not claim a fixed authentication failure until the phone test establishes it.

A separate bounded source review found that a new or revived setup session
arriving after the last guard, during LCD UI teardown, can still reach sleep.
The UART provisioning handlers do not cancel that transition. The candidate is
retained for the current phone/AP diagnosis; this late-arrival case remains open
and no complete provisioning/sleep qualification is claimed.

## Actual172 release evidence

Candidate: `/Users/MattTaylor/halo-provisioning-review-2026-09-16/device172/candidate172-001`.
`RELEASE_BASELINE.json.current_working_source` pins the exact receipts:

- `RELEASE-PAIR.json`: SHA256 `d21894556dd65033f764a60c27ebdb9de9890c8603d5a36b3078f339213d412c`.
- `host-final001/RESULT.json`:49 passing suites; SHA256 `dd60bae093853a4a2ecef7eeb8977409c282068a8a1f62bfaa59daf40ce2771c`.
- `build/artifact-result-v2-sense-lcd.json`: both canonical shipping artifacts passed.
- `SOURCE-SCOPE.json`, `STACK-REVIEW.json` and `../tools/QUALIFICATION-LIMITS172.json`:
  exact37-file delta versus169, compiler frame evidence and retained known risk.
- `../tools/BINDING172.json`: exact actual pair bound; no hardware action by binder.

Sense image grows8224bytes versus169, static RAM280bytes and RTC unchanged;
LCD grows7264bytes, static RAM32bytes and RTC40bytes. Both fit existing slots.
Diagnostic compiler frame is544bytes, not a measured runtime stack margin.
Historical171 artifacts and acceptance records remain unchanged.


## Installed172 checkpoint

`/Users/MattTaylor/halo-provisioning-review-2026-09-16/device172/INSTALLATION172-REVIEW.json` verifies the
saved complete banks, exact candidate writes/selectors, protected NVS/partition
bytes and service closure. SHA256:
`11e2f02f3ef5629c68abfa8f50ebf8a18e3be5db0372971823fb45db30b5a832`.
Separate `health172-observed001/RESULT.json` confirms Sense/app0 and LCD/app1
selected SDK VALID, including the LCD nonce/CRC check. SHA256:
`bff0a571947755f8378c298af0dceff1727414f4d59ac1815bc19bf33d7262d8`.
The immutable sealed pair retains its original build-time checkpoint; these
separate receipts establish the later installation and health facts.

The actual driver reports SSID/password equality with the manager for session20AE.
The user's iPhone Settings attempt nevertheless returned "Incorrect password."
An independent Samsung SM_A366U (USB RFCY322W52W through Mac mini) reproduced the
failure with the exact startup password programmed by ADB. Its target scan was
RSSI-48, WPA2-PSK/CCMP. For Halo BSSID9a:a3:16:f8:1a:6c, Android received M1 at
04:23:43UTC and sent M2, then reported reason15 timeout/WRONG_KEY about5.1seconds
later; no target M3 was captured. The later M1/M3 belongs to Garage Member
BSSIDa4:f8:ff:8c:a1:46 during restoration, not successful Halo authentication.

This verifies a failed target handshake despite driver/manager credential
readback equality. It does not establish PMK correctness or identify the cause;
no PMK causal claim or confirmed authentication fix is made. phone-join001 is
closed; android-join001/RESULT.json and wifi-logcat.raw retain the independent
attempt. Mac Wi-Fi remained unchanged. The warm AP-reset experiment in
phone-join002/Android002 is still in progress; its outcome is not inferred here.

Separately, setup-sleep172-observed001/RESULT.json (SHA256
`a1f22e2fcf690e9a7d13937867cef25c513b2e55519bcba1fe799575eac453ec`)
confirms live setup stayed awake beyond330seconds. This is one USB-connected
live-peer run with read-only diagnostic queries every15seconds and no touch or
scroll input; it does not cover the known late/revived-session teardown race.


## Private173: independent Galaxy same-credential A/B

The Samsung SM_A366U (USB serial RFCY322W52W) is controlled through SSH to the
Mac mini and USB ADB. Neither Mac's network was changed. The Galaxy joined using
exact startup credentials entered automatically, bypassing QR and manual typing.
On173, the target BSSID9a:a3:16:f8:1a:6c sent M1; the phone sent M2; the attempt
ended with reason15/WRONG_KEY after4.094seconds, without target M3. A manual
AP-only disable/re-enable then verified config_same=1 and ip_same=1. The exact
same credentials failed again after4.049seconds. Later successful handshakes in
those captures belong to Garage Member and are not Halo passes.

This disproves that particular AP-only restart as a sufficient recovery. It does
not prove a wrong PMK, crypto fault or receipt of M2 by Halo.173 is diagnostic-only
and does not automatically restart the AP during ordinary startup. Raw SDK
WIFI_EVENT_AP_WRONG_PASSWORD is not forwarded by Arduino;174 diagnostics are
being prepared to observe that event and manually test public SHA1/HMAC/PBKDF2
vectors without exposing keys or changing global allocators.

Closed evidence: device173/INSTALLATION173-REVIEW.json,
health173-observed001/RESULT.json, and ap-restart173-ab001/RESULT.json under
`/Users/MattTaylor/halo-provisioning-review-2026-09-16`. Both original USB capture
owners and both Android log children were closed/reaped; only the two temporary
Galaxy test network entries were forgotten. It returned to Garage Member.
