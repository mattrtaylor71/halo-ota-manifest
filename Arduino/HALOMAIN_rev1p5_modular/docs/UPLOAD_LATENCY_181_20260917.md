# Upload latency investigation, 17 September 2026

This follow-up starts from the reviewed180 source. At investigation start the
private unit runs Sense179 app1 and LCD180 app0, both SDK VALID. Public OTA
remains162. Candidate181 is not a publication or a new full-product qualification.

## Measured before changes

Evidence: `/Users/MattTaylor/halo-upload181-20260917/`.

- An ordinary wake obtained Wi-Fi in3.77s with RSSI−77dBm. Fresh SNTP expired
  after15s; automatic OTA readiness retained the wake until its120s deadline.
- Another wake obtained Wi-Fi in2.71s and fresh time592ms later. The difference
  is intermittent clock synchronization, not uniformly slow Wi-Fi association.
- `baseline-voice004` captured100352 bytes in3089ms. The upload queue waited
  71.51s before sleep flush. With no fresh time proof, the exact recording was
  committed to LCD SD in11.89s and a300s retry was armed. No HTTP upload occurred
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

## Private181 measurement and182 boundary correction

Private181 source `53b8e56cb72789a43093032a2eee71bacb8b8533` passed23
canonical-snapshot suites and both artifact checks. Controlled Sense-only service
selected181 app0 and preserved179 app1; LCD180 remained unchanged. Both observed
boards were SDK VALID. This is a controlled service, not an OTA transfer.

`voice181-001` uploaded100352 bytes. Recording completion to observed HTTP202
was13.286s: queue to sleep flush7.26s, HTTP POST to2025.91s. Exact cloud session
correlation confirmed durable custody and backend completion2.749s after server
acceptance. Fresh time arrived on the primary attempt; this is not a measurement
of the secondary-clock path or a guarantee of that latency under weak Wi-Fi.

`image181-001` caught a real missing boundary: sleep-path work crossed the
primary15s deadline after owner service, so the pending predicate became false
before the requested secondary generation could start. The image was saved to
SD with a300s retry; the strict immediate-delivery test failed. Retain that
failure.182 gives the owner at most one second after the original primary
deadline to admit the already-requested opportunity. This does not extend
DNS/SNTP acceptance, create another generation, or hold sleep indefinitely if
Wi-Fi is unavailable. The actual sleep guard with time advancing between owner
service and sleep admission reproduces the failure on frozen181 and passes the
bounded correction.
