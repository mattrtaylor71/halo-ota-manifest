# Provisioning join investigation — September 16, 2026

Current installed private pair is 176, source `770c60aa99e9bb1e498419faebf8d7ca66156fdb`, build `6.4.176-20260917T055849Z-770c60aa99e9`. Both canonical builds/artifact checks and 15 snapshot suites pass. Sense and LCD run app0 SDK VALID, retaining Sense175/LCD174 app1 fallbacks and protected data. Owner claim now runs on the existing upload worker, and LCD has a finite setup-progress sleep lease. Assisted health and one console Check-in capture/upload/sleep smoke passed within scope; touch, fresh intended-owner phone provisioning, and same-boot camera recovery after provisioning remain unqualified. Preserve the observed reserve warnings and the distinction between the logical 20-second claim limit and synchronous SDK timing. No publication. Continue from reviewed 176 descendants and read `docs/PROVISIONING_JOIN_INVESTIGATION_20260916.md` and `RELEASE_BASELINE.json.current_working_source`.

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


## Private174: raw driver control, public crypto checks and AES allocation failure

Exact174 source `75debe873675ed1519c455e23275f3aefd1642db` is installed on
Sense/app0 and LCD/app1, both selected SDK VALID with LCD nonce/CRC verified.
Application-only service preserved173 fallback banks, NVS and partitions.
Both canonical builds and nine focused source-snapshot suites pass; retained
173 seven-suite and17249-suite evidence and unchanged unrelated runtime were
reverified. Sense grows1424bytes and16bytes static RAM versus173; LCD footprints
are unchanged. Runtime stack high-water was not measured.

One closed B117 setup session tested the Galaxy with exact startup credentials,
a deliberately different password, then the exact password again. All three
received target M1, sent M2 and ended with reason15/WRONG_KEY, without target M3
captured. Correct attempts timed out after4.799 and4.041seconds from M1;
the wrong-password control after3.023seconds. The only raw SDK
AP_WRONG_PASSWORD event was logged during the wrong-password control
(count1/total1). No additional event was logged during either correct trial.
Later successful handshakes belong to Garage Member and are excluded.

The manual on-device public HMAC-SHA1 and PBKDF2-HMAC-SHA1 vectors returned0 and
matched expected output. This validates those algorithm executions only; it
does not test the active authenticator's PMK, MIC, peer/nonce state or AES/key-wrap.
The same Sense capture contains197 literal `esp-aes: Failed to allocate memory`
errors in the first correct-password interval. None were additionally logged
in the wrong-password or second correct-password interval. ESP log and Arduino
millis clocks are not equated; ordered raw lines bracket the errors with
periodic diagnostic commands during the first trial.

These are real SDK AES allocation failures and a useful difference from the
wrong-password control. They do not by themselves prove receipt/acceptance of
M2, prove that M3 generation/transmission failed, or establish that freeing a
particular allocation will fix authentication. A175 change to release the
camera DMA reserve during provisioning and reacquire it afterward is being
implemented; no175 build, install or successful phone outcome is claimed here.
Known late/revived setup sleep and blocking STA ownership limits remain separate.

Evidence root: `/Users/MattTaylor/halo-provisioning-review-2026-09-16/device174`.
`RELEASE_BASELINE.json.previous_working_source_174` pins the exact sealed pair,
source/host/artifact receipts, `INSTALLATION174-REVIEW.json`,
`health174-observed001/RESULT.json`, `ap-auth174-experiment001/RESULT.json` and
its immutable `AES-ADDENDUM.json`. All captures are closed and Android log owners
reaped. Root reports final test-network cleanup and restoration to Garage Member,
preserving original networks0/1; neither Mac Wi-Fi changed. No public OTA,
complete provisioning, media recovery or full-product acceptance is claimed.


## Private175: setup AP joins recover; full app flow remains unaccepted

Source `e303bb1cbec1ff4efefdc3232566b387921ad00c`, build
`6.4.175-20260917T052511Z-e303bb1cbec1`, releases the16KiB camera DMA reserve
before provisioning scan/AP startup and suppresses reacquisition while the
actual AP remains active. Both canonical images, artifact checks and ten focused
snapshot suites pass, retaining174/172 host lineage. All unrelated runtime,
including LCD, is unchanged from174. Sense image grows512bytes/static RAM16bytes;
LCD footprints are unchanged. The new reserve hook compiler frame is64bytes;
runtime stack high-water was not measured.

Physical service installed **Sense175/app1 only**, preserving Sense174/app0 and
protected data. LCD remains174/app1. The full175 pair is a build artifact, not an
installed paired175 claim. Fresh mixed-board SDK VALID health includes a bound
LCD nonce/CRC. The first post-service capture failed because the LCD USB port was
absent; an actuator wake preceded the successful health capture. This is assisted
bench evidence, not a scheduled or USB-free wake pass.

The Galaxy joined cold setup sessionC5E7 twice, then warm reset session667C once.
All three reached target BSSID9a:a3:16:f8:1a:6c, COMPLETED, a192.168.4.x address,
and HTTP200 from GET /info. Response SSID/password matched the exact private
startup input. No SDK AES allocation error was captured in retained startup,
phone-join002 or phone-warm001. These are two joins on one fresh setup boot and
one after an existing setup reset, not three device cold boots. They establish
that the previous AP join failure no longer reproduces in this bounded test;
they do not establish a precise PMK/MIC/M3-stage cause or complete provisioning.

Warm setup exit logged reserve reacquisition failure: largest contiguous block
16,372bytes, required16,384. The next AP started with reserve held=0 and still
passed its phone trial. Existing camera recovery after completed setup has not
yet been physically qualified. The later closed app capture verifies eventual provisioned=1/owner=1, but
responsive full setup failed as described below.

Evidence root: `/Users/MattTaylor/halo-provisioning-review-2026-09-16/device175`.
- `candidate175-001/RELEASE-PAIR.json`: sealed full pair, SHA256
  `58764abb34c52feb30b52ac3c1bf174e443143bff8bb0efcd7d87540e4951191`.
- `INSTALLATION175-REVIEW.json`: Sense-only service, SHA256
  `fc05005f0f38c231189010b016df815a102c6e063be075a407a99749912d3fb6`.
- `health175-observed001/RESULT.json`: mixed SDK health, SHA256
  `292a83e99286b8f81b2d58f05ebfc3dac8fe39935375aae6754a78a49c8959be`.
- `ap-join175-cold001/RESULT.json`: two closed cold-session trials, SHA256
  `ea95698268c1576f3578cee36c9eed96aba8f1d3050ce69c5309e503d26b4f9b`.
- `ap-join175-warm001/RESULT.json`: warm trial and reserve warning, SHA256
  `38ed4b278d345f5afcfcb036ccf78d409f6732ac295c614cc32a808e279eee28`.

- `app-flow175-observed001/RESULT.json`: eventual claim with responsiveness failure,
  SHA256 `6c0fb90be24e2aae1a21ac75b68fbc6e730cdb8865db70ec29fae29770ca7101`.

All three Android capture owners closed/reaped. Temporary IDs9/10/11 were
forgotten and the retained final network list contains original IDs0/1 only.
The recorded final status says not connected; later Garage Member restoration
was reported but is not established by that file. Neither Mac Wi-Fi changed.
Known setup sleep/STA ownership limits remain; no publication, full-product,
new media retry, OTA, scheduled-wake or camera recovery acceptance is claimed.


### App flow: eventual ownership, failed responsiveness

The closed app-provision001 capture records halo_app 1.16 POST /wifi. The first
home-station attempt reported reason202; the existing retry connected and marked
provisioned=1. Sense last logged PONG at391206ms, then logged a claim HTTP-1
response at512540ms: a121,334ms response gap. This is the observed log interval,
not a measured TLS function start/end. The next claim returned HTTP200 with
owner present at514691ms. AP shutdown completed at529820ms; firmware then reported
provisioned=1/owner=1 and eventually entered deep sleep at545792ms.

During the gap, LCD entered its sleep sequence and exhausted three25-second
Sense handshakes, then took the15-second fallback timer sleep. The user's
finishing-setup screen appeared frozen while the app reconnected. This was not
a permanent deadlock: owner persistence and shutdown eventually completed.
The long blocking interval nevertheless fails a responsive full setup flow.
A176 claim-liveness/LCD progress-lease fix is being prepared; no176 artifact or
successful retest is claimed here.

Final AP shutdown again failed to reacquire the16KiB reserve (largest14,836),
with later recovery warnings also captured. No same-boot camera capture was
performed before sleep. The immutable closed receipt preserves those warnings,
the actual claim response sequence and both sleep tails. Owner state persistence
is observed; full functional acceptance remains false.


## Private176: async claim and bounded LCD progress; phone retest pending

Exact source `770c60aa99e9bb1e498419faebf8d7ca66156fdb`, build
`6.4.176-20260917T055849Z-770c60aa99e9`, moves claim HTTP/TLS to the existing
12KiB upload worker without another task/stack. Fixed request/result snapshots
and a generation gate keep manager/NVS result application on the owner loop.
TCP, TLS and HTTP read limits are explicit. The 20-second request age is a
logical cancellation/admission limit, not a wall-clock guarantee for synchronous
SDK DNS/TCP/TLS. LCD uses one finite 240-second progress lease; repeated claiming
messages do not extend it.

Both canonical builds/artifact checks and all 15 exact-snapshot host suites pass.
Retained175/172 evidence and byte-identical unrelated runtime were reverified;
only the nine reviewed runtime paths differ. Sense grows6,944 image bytes and
1,152 static RAM bytes; LCD grows192 image bytes and8 static RAM bytes. Largest
new compiler frames include applyClaimResult2,160 and worker_poll1,632bytes.
No runtime stack high-water or whole-call-chain bound was measured.

Application-only service installed LCD176/app0 first, then Sense176/app0,
preserving LCD174/app1 and Sense175/app1 VALID fallbacks, NVS and partitions.
Fresh paired SDK VALID health includes exact LCD nonce/CRC and Sense build.
An actuator wake was required after service; capture began about10seconds after
stroke completion, with the LCD already idle-dark/backlight0. Both boards then
performed a coherent ready handshake and deep sleep. This is not a full awake
UI, coldboot or scheduled-wake pass.

A separate ordinary wake exercised one **console** Check-in and quantity1,
bypassing touch. Camera initialized once and captured115,653bytes at1280×1024
in1,320ms. LCD returned HOME alive; home Wi-Fi reconnected. Client logs show
presign HTTP200 and PUT HTTP200 for job36; the queue drained in8,235ms and both
boards entered coordinated deep sleep. The backend object was not independently
read back in this review. Reserve reacquisition after presign warned that the
largest block was9,716bytes versus16,384 needed. Retain this warning without
claiming a newly diagnosed regression.

This smoke was an ordinary wake, **not the same boot after fresh provisioning**.
The intended owner has been asked to perform a fresh iPhone setup attempt.
Responsive claim completion and camera recovery immediately after that flow
remain pending. Both boards were left asleep with the existing provisioned
state; no public OTA or full-product acceptance is claimed.

Evidence root: `/Users/MattTaylor/halo-provisioning-review-2026-09-16/device176`.

- `candidate176-001/RELEASE-PAIR.json`: SHA256
  `85c60af88cbe8f2bcbc7eb75caac08f996475d5ffbc08184d9a89bbf234bd793`.
- `candidate176-001/host-final001/RESULT.json`:15 suites PASS, SHA256
  `f0943b27d323459c950f31dfbb973b8874f093a4a7ce1b2524bc4c856d416429`.
- `INSTALLATION176-REVIEW.json`: SHA256
  `dadd8ab56ed76e7a682a1dc959583ba41ad22fa8e8fd433cec7d716ebba65002`.
- `health176-observed001/RESULT.json`: SHA256
  `b23e9ee7e94674321f50a204cf1516c80ea21f62167fda833a99b67508826615`.
- `smoke176-observed001/RESULT.json`: SHA256
  `f6da09216bc2ca3a79b01c81f8ace535548c07d39347fd9c2655a7c538e9faec`.

All listed service and capture receipts are closed. Read-only review performed
no hardware action. Historical175 joining successes and its eventual-owner but
unresponsive claim flow remain under `previous_working_source_175`; they are not
176 phone acceptance. Known setup sleep/STA ownership limits remain recorded.
