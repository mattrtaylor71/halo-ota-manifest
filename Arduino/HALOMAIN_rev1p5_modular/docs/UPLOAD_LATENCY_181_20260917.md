# Upload latency investigation, 17 September 2026

This follow-up starts from the reviewed180 source. At investigation start the
private unit runs Sense179 app1 and LCD 180 app0, both SDK VALID. Public OTA
remains162. Candidate181 is not a publication or a new full-product qualification.

## Measured before changes

Evidence: `/Users/MattTaylor/halo-upload181-20260917/`.

- An ordinary wake obtained Wi-Fi in3.77s with RSSI−77dBm. Fresh SNTP expired
  after15s; automatic OTA readiness retained the wake until its120s deadline.
- Another wake obtained Wi-Fi in2.71s and fresh time592ms later. The difference
  is intermittent clock synchronization, not uniformly slow Wi-Fi association.
- `baseline-voice004` captured100352 bytes in3089ms. The upload queue waited
  71.51 s before sleep flush. With no fresh time proof, the exact recording was
  committed to LCD SD in11.89s and a300 s retry was armed. No HTTP upload occurred
  on that initial wake. The capture began in an already-running wake; this is
  not a cold-boot-to-cloud measurement. Commands injected the existing local
  encoder event and voice inputs; this is not physical finger-edge testing.
- Three earlier controller attempts stopped before recording. Their receipts
  are retained: an overstrict readiness gate and a busy queue diagnostic were
  test-harness failures, not lost audio.

## Scope of the follow-up

Preserve the active-session upload hold: TLS allocation during a camera session
has previously fragmented camera DMA memory. Wi-Fi can associate asynchronously
while the user records. Actual uploads drain when the user leaves.

End only unentered automatic OTA readiness when fresh media is queued; retain
manual intent, entered OTA, verification, transfer, allowance/debt and stored
targets. Give queued fresh media one bounded alternate clock opportunity, shared
with the existing per-boot secondary generation, without accepting stale time.

Make fresh upload sockets yield at safe owner-task boundaries on new user input,
preserving separate saved-retry policy. Park unaccepted fresh work instead of
blocking a new action with SD persistence; a verified accepted receipt remains
authoritative. Do not close another task's socket or reset its Wi-Fi.

Remove the unconditional two-second image PUT response drain when bounded HTTP
framing already proves completion. Incomplete replies preserve original custody
for a same-identity retry.

## Acceptance limits

Synchronous SDK DNS/TCP/TLS calls cannot be interrupted cross-task safely. The
upload client bounds connect/handshake and checks cancellation when those calls
return; this is not a guarantee of instantaneous network cancellation. Host
boundary tests do not replace actual device timing and interruption checks.

Record final source, canonical artifact identity, host results, installation and
measured after-results here after they exist. Preserve all failed test receipts.

## Private 181 measurement and182 boundary correction

Private 181 source `53b8e56cb72789a43093032a2eee71bacb8b8533` passed23
canonical-snapshot suites and both artifact checks. Controlled Sense-only service
selected181 app0 and preserved179 app1; LCD 180 remained unchanged. Both observed
boards were SDK VALID. This is a controlled service, not an OTA transfer.

`voice181-001` uploaded100352 bytes. Recording completion to observed HTTP202
was13.286 s: queue to sleep flush7.26s, HTTP POST to2025.91s. Exact cloud session
correlation confirmed durable custody and backend completion2.749 s after server
acceptance. Fresh time arrived on the primary attempt; this is not a measurement
of the secondary-clock path or a guarantee of that latency under weak Wi-Fi.

`image181-001` caught a real missing boundary: sleep-path work crossed the
primary15s deadline after owner service, so the pending predicate became false
before the requested secondary generation could start. The image was saved to
SD with a300 s retry; the strict immediate-delivery test failed. Retain that
failure.182 gives the owner at most one second after the original primary
deadline to admit the already-requested opportunity. This does not extend
DNS/SNTP acceptance, create another generation, or hold sleep indefinitely if
Wi-Fi is unavailable. The actual sleep guard with time advancing between owner
service and sleep admission reproduces the failure on frozen181 and passes the
bounded correction.

## Current 182 implementation and measured outcome

Source: `ab87b6341a9732ea088639672da6ff7bd85965a1`; firmware tree
`2d3df8998536284bea091d468754060ae5dfa8a1`. Both canonical 182 artifacts and
23 snapshot host suites pass. The pair receipt is
`/Users/MattTaylor/halo-upload181-20260917/candidate182-001/RELEASE-PAIR.json`
(SHA256 `715c83b431800e41d353ed9c1c224851ac27091be942f1233ee5209f2842b2e0`).
The three focused transport/custody suites exercise 561 fresh-priority assertions,
406 PUT-framing assertions, and88 clock-retry assertions with sanitizers.
Frozen 181 fails the reproduced clock/sleep boundary test; 182 passes.

Controlled Sense-only service installed 182 in app1, preserved 181 app0, and left
LCD 180 untouched. NVS and the partition table were unchanged; there were no LCD,
bootloader, current-bank, policy-fixture or NVS writes. The boot log binds the
exact 182 build to app1, followed at uptime 10088 ms by the SDK mark-valid success
with current readback. This is local Sense health, not a fresh paired 182/180
functional acceptance or an OTA-transfer result. Public manifests remain 162.

| Observation | Measured time | Qualification |
| --- | ---: | --- |
| Wi-Fi connection in sampled wakes | 2.71–3.77 s | Different RF/clock conditions, not a statistical benchmark |
| Original voice, recording end to durable HTTP202 | 396.505 s | Failed fresh clock;71.51 s queue wait, SD save,300 s retry |
| Private 181 voice, recording end to HTTP202 | 13.286 s | Primary clock succeeded; exact 100352-byte cloud object |
| Private 181 voice, queue to sleep flush | 7.261 s | Upload intentionally waits until foreground use finishes |
| Private 181 voice, POST start to HTTP202 | 5.911 s | Includes network/server acceptance |
| Same voice, cloud acceptance to processing complete | 2.749 s | Cloud timestamps; no useful shopping-list command was asserted |
| Saved image replay, PUT duration | 2.613 s | Exact125448 bytes/SHA256 independently confirmed in cloud |

The396.505 s and13.286 s recordings experienced different clock outcomes. They show
that the large delay can be avoided; they do not establish a repeatable 30x
firmware speedup. The image 181 first-wake attempt failed and remains a failure;
its later successful SD replay does not retrospectively pass that timing case.

The response-framing change removes an unconditional 2 s wait for a complete PUT
response. Host positive/negative tests establish that removal. Current physical
logs lack a body-finished timestamp, so they cannot separately measure the 2 s
saving on real hardware. The image replay's subsequent dark 60 s wake reconciled
an empty SD inventory, acknowledged a zero retry arm, and returned to the normal
nightly schedule. It was not another media upload or a foreground screen wake.

Independent cloud receipts:
- `cloud-baseline001/RESULT.json`: exact voice session,100352 bytes, backend complete.
- `cloud-voice181-001/RESULT.json`: exact voice session,100352 bytes, backend complete.
- `cloud-image181-001/RESULT.json`: exact image125448 bytes and SHA256, backend DONE.
All paths are under the campaign directory; verification used metadata only and
did not download media or mutate/replay backend requests.

## Remaining physical checks and harness failure

After 182 installation, `image182-001`, `image182-002` and `image182-003` all stopped
before issuing capture commands. The actuator USB open hung; the last attempt
records the precise `opening` stage before the bounded helper timeout. No stroke
was sent by that attempt. Collectors closed cleanly and processes were reaped.
An actuator USB reconnect was requested. These are unavailable bench tests,
not evidence of an 182 image failure or an 182 upload success.

At that blocked checkpoint, the pending182 tests were fresh first-wake image delivery, actual secondary-clock
success following a primary miss, and interruption of an in-flight voice upload
by a live shopping-list refresh and by a camera capture. Require the exact job
to park, the foreground operation to complete, and the same job to resume and
obtain backend acceptance before claiming these pass. The host tests cover
these ownership paths, but they cannot replace hardware latency evidence.

Synchronous DNS/TCP/TLS and an already-entered Wi-Fi reset cannot be interrupted
instantaneously. Cancellation is checked at safe owner-task boundaries; the 8 s
handshake cap is not an operation-wide maximum or a guarantee of immediate
foreground response. The one-shot 20 s media clock retry remains bounded. If fresh
time is still unavailable, media retains its existing durable retry path.

## Relay recovery and resumed182 device tests

The user added an actuator-only normally closed USB relay. A ten-second power
cut restored its serial port and responsive sketch; the actuator then woke Halo.
See `ACTUATOR_RELAY_RECOVERY_20260917.md`. This resolved the bench blocker above.

`image182-004` passed first-wake image acceptance and paired sleep plus15seconds
quiet. It exercised the actual primary SNTP timeout and successful secondary
clock attempt (fresh after1722ms).126412 bytes uploaded: capture start to PUT200
21.300s, queue to flush14.605s, flush to PUT2006.393s. Exact cloud admission/key,
length and backend DONE were verified independently. Device SHA was not logged;
a stored object SHA alone is not an independent device-to-cloud hash comparison.
The image was unrecognized by grocery processing; this verifies delivery rather
than recognition quality.

`interrupt182-001` passed the live shopping-list overlap: voice job20 paused,
parked, resumed the same session and reached HTTP202. List refresh completed in
1118ms (948ms HTTP plus97ms parse); command to observed list receipt was1.258s.
Both boards slept afterward. Independent cloud evidence found one ingress,
one queued worker and completed processing for100352 bytes. Inputs were console
commands through the device's real handlers, not physical menu navigation.

`interrupt-camera182-001` failed and must remain a failed capture test. Voice22
was cancelled, but the camera attempted allocation after `http_inflight` cleared
and before the voice function's complete `SenseBackupWifiCall` lease and DMA
reserve destructor finished. The camera's Wi-Fi teardown correctly refused an
active lease; camera init nevertheless proceeded and failed a16384-byte DMA
allocation (largest available10740 bytes). Retry remained short at12276 bytes.
The98304-byte voice payload was preserved to SD. Its automatic dark replay later
obtained HTTP202 and deleted the SD record, as captured in
`camera182-failure-followup001`; successful voice recovery does not pass the
failed camera operation.

The follow-up must gate camera allocation on complete transport cleanup, not
HTTP unlock alone. Retain the accepted capture while waiting at safe task
boundaries, exclude new network entrants during camera ownership, and never
allocate camera DMA after a drain timeout. No OTA ownership/policy change is
needed for this correction. Final build and device results follow separately.


## Private183 camera ownership correction

Source `ad9eae59596131f501520c06cad7f3b4d98e9bfb`, firmware tree
`4a3d346feb41cd80d75ab9ea27b0f03b63b79617`, closes the reproduced182 handoff gap.
Camera claims network/DMA ownership before waiting for both HTTP release and the
complete upload Wi-Fi-call lease to end. That lease outlives transport-local DMA
cleanup. New network entrants remain excluded; a20-second drain timeout releases
the claim and fails before allocating camera DMA. Cleanup does not reacquire the
camera reserve while a transport lease is still alive. Existing media custody,
foreground cancellation, reconnect ownership and OTA policy remain intact.

All25 exact-snapshot host suites pass. The focused handoff test exercises20
scenarios/6794 checks using the actual camera and upload guard destructors; the
frozen182 negative control reproduces the failure. These are host ownership and
cleanup proofs; the separate device case is recorded below.

Sense183 compiled from the unchanged canonical snapshot. The explicitly approved
external adapter called canonical `run()` with a2.5GiB free-space reserve; its
next-board check correctly stopped before LCD at2.486GiB. A separate approved
LCD-only2.0GiB build passed, based on the measured186MiB LCD footprint.
The original failure receipt is retained. Source, SDK, shipping flags, timeout
and process cleanup are unchanged; this exception is not the ordinary builder
CLI. Both artifact checks passed and the shipping pair is sealed at
`candidate183-001/RELEASE-PAIR.json`, SHA256
`ca4060d1627cbe38456538bac0aaa2e3f72bfef0fa5d0f4f7d5277a7f01c84ab`.
Sense183 is208 binary bytes larger than182, with unchanged static RAM/RTC; LCD
size/RAM/RTC are unchanged. Controlled Sense-only service installed183 app0,
preserving182 app1 and unchanged LCD180 app0/LCD179 app1 fallback. Both full-bank
backups and named protected ranges were verified; no NVS, bootloader, current-bank
or LCD writes occurred. `install183-001/result.json` closed/reaped successfully;
its historical status precedes the released boot log's SDK VALID/current-readback
success at10,077ms (`sense-released.raw`, line126). The later183 camera case
confirmed fresh paired183/LCD180 SDK health. Prior182 passed image/list
results and failed camera result above remain separate. Public manifests remain162. Any next candidate184 requires a fresh
version inventory check.


##183 camera overlap and visible list observed

`interrupt-camera183-001` stopped before media capture. Boot readiness relocked
and then unlocked with `policy_not_due`, but the controller retained a single
older UI reply. Its timeout and clean collector closure remain recorded. The
next controller added read-only `id1`/`ui` queries within the unchanged3-second
admission bound; firmware and admission conditions were unchanged.

`interrupt-camera183-002` passed the scoped device test. Fresh exact identities
show Sense183 app0 and LCD180 app0 SDK VALID. The camera request interrupted an
actual voice POST: voice job22/102400bytes parked with the same identity; complete
transport cleanup drained before camera DMA allocation. Check-in job48 captured
127193bytes in1825ms according to `CAMERA_TIMING`. Both fresh uploads obtained
their device-validated acceptance before the first paired sleep, followed by
15seconds of quiet; no camera error was observed. The controller and collector
closed/reaped cleanly. Camera input and quantity were console commands through
real handlers after `WAITING_INPUT`, not physical menu gestures.

Device acceptance is not backend processing completion. Independent cloud
metadata checks and the separate183 list cases are recorded below. Evidence
under the campaign directory: `interrupt-camera183-002/controller-result.json`
SHA256 `d7981f3b76cec76cf351b491c9f0cf4854d7e42b96812b2537d4780367dac9db`,
collector `RESULT.json` SHA256
`1119a12540cb8837f70054b604d5d43ca559455e1d55b3d1f6b42f39ccba61ba`.


The first183 list-overlap case, `interrupt183-001`, proved voice park/resume and
a live HTTP200 list fetch, but the direct `list` console command bypassed the
LCD wake path: `UI_STATE` showed screen16 with `idle_dark=1` and backlight0.
This is transport-priority evidence only, not a visible-list responsiveness pass.
The corrected `interrupt-ui183-001` uses the real local encoder `scroll1` path
before list and requires a fresh screen16, running LVGL, `idle_dark=0` and positive
backlight before returning Home. Its completed result follows; the earlier
dark-list limitation remains recorded.


`interrupt-ui183-001` passed the corrected visible-list test. It injected the
real local encoder event before the list command, then received live HTTP200
(780ms) and `UI_LIST`0.879s after the actual encoder event on serial timestamps. At0.960s, fresh
`UI_STATE` confirmed screen16, unlocked OTA, running LVGL, `idle_dark=0`, backlight255
and a live UI task. Controller-observed intervals were0.952s/1.061s; these use
different observation points and include polling/buffering. Voice21/98304bytes parked and resumed the same identity,
obtaining HTTP202 before the first paired sleep plus15seconds quiet. Collector
and controller closed/reaped. This proves the injected wake-handler/list flow,
not physical finger geometry or every possible concurrent input.

Independent metadata-only checks now confirm camera-case voice102400bytes
completed and image127193bytes reached backend DONE, and the first list-case
voice100352bytes completed. Exact identity/key/options and length match; there
is no independent device content-hash comparison, transcript/recognition/list
semantic acceptance or global exactly-once claim. No cloud writes or media-body
downloads occurred. Final visible-list voice98304bytes also reached confirmed cloud custody and completed processing (one recorded enqueue and worker attempt).

- `cloud-interrupt-camera183-002/RESULT.json`: SHA256 `4a7db2c3a245f8325c14ccfb1e91d5485874839ab5913f5802788505d018e646`.
- `cloud-interrupt183-001/RESULT.json`: SHA256 `31211d4348edd3ba29bc60258e244b67d33efa5964f24bb31ec952b1f30fcb4a`.
- `interrupt-ui183-001/controller-result.json`: SHA256 `9c47e9dae8c61697a28a07de7239a419cc58971895dabad3274e27d76c063ad4`.
- `interrupt-ui183-001/RESULT.json`: SHA256 `961d58404546f6ede2aa927d00d2e88877841ba1c658adccbfa8ec3ef6dd0222`.

- `cloud-interrupt-ui183-001/RESULT.json`: SHA256 `05b1935385c6cc58652b60e3ccb73a1ee5690234bea35947372b04b391f79245`.
- `FINAL-DEVICE-REVIEW.json`: SHA256 `bed06a3826ca9fb34ab5e4ebd194c4c1b2447ee51b1207bf013f20b87f04a7e1`.

All three finite183 overlap cases and independent cloud reads are closed; the
corrected visible-list case supplies the latest paired Sense183 app0/LCD180 app0
SDK VALID evidence. The earlier two-case review and failed/dark cases are retained.
There is no new full OTA-transfer, scheduled-OTA, phone-provisioning, physical
navigation, recognition/list-action semantics or broad product qualification.
The presign reserve-reacquisition warning still appeared (largest10228 versus
16384bytes required), so the successful camera handoff is not a broad memory
resolution. Parked jobs can cycle through the existing queue roughly every40ms
while foreground work remains active; the reviewed cases did not start a second
voice POST until the later flush. Public162 and frozen158 remain unchanged.
