# Private229 supply telemetry correction

September 28, 2026. Private build and hardware test record; no public release. Public firmware
remains6.4.224. Continue from the private228 descendant on `codex/ram-qualification`,
retaining the RAM, shopping refresh, haptics-off and10-second display fixes.

The228 health test found two isolated telemetry problems. An ordinary PONG-proven
awake peer could have `link_synced=false`, so the additional link-sync condition
prevented the optional power frame from being sent. The complete USB diagnostic
JSON also exceeded the existing bounded256-byte CDC output buffer and was cut
short. Cloud readback confirmed the missing peer snapshot rather than merely a
missing USB receipt. See [preserved228 results](POWER_TELEMETRY_228.md).

229 changes only the optional power-send admission and compact cached USB readout.
A current awake flag plus an accepted PONG/SYNC_ACK within two seconds suffices;
all existing transfer, sleep, OTA and queued-user-work exclusions remain. It does
not issue another ping, wake a peer, renew activity or change any user-action
handshake. The compact diagnostic has a distinct schema/prefix and fits below
the current USB buffer size. ADC acquisition, system-rail interpretation, report
payloads, cloud routing, schedules, upload storage and retry policies are unchanged.

The canonical build passed and both boards are installed as 229/app0/SDK VALID,
with 228/app1 and device data preserved. Two ordinary wakes produced complete
local voltage and native Sense receipts. The second completed the timer and
75-second quiet observation. The first original controller failure is preserved
below. Ordinary cloud report correlation passed with an honestly stale sample.
The camera check passed. The voice/list first attempt timed out, then a separate
passive observation proved automatic acknowledgment, saved-file retirement,
cleared retry state and dark sleep. Acceptance is complete with this latency
finding and the stated coverage limits; see [functional summary](FUNCTIONAL_229_20260928.md).
No voltage percentage, electrical calibration accuracy, charger/source inference,
OTA transfer or full provisioning claim follows from these tests.


## Canonical build

Compiled source `fc28f8679901db41c982d3b229620a504dc75255`, build
`6.4.229-20260929T035051Z-fc28f8679901`. Job`20260928T205410-6c1300f2`
passed all128 exact-snapshot regression suites before both compilers, then passed
locked-environment, artifact and candidate verification. The focused LCD tests
cover35 cases. A compiled negative control with the actual old link predicate
fails the intended PONG-without-SYNC case; the corrected expression passes.

Sense application1,880,944bytes leaves85,136bytes(4.33%) in its OTA slot. LCD
application2,045,632bytes leaves575,808bytes(21.97%). Static DRAM/IRAM/RTC totals
are unchanged from228. Relative to227, linked static DRAM increases64bytes on
Sense and336bytes on LCD for the overall telemetry feature. Individual LCD
UART compiler frame decreases1664→1600bytes; loop288, sampling owner96 and
telemetry-send432 remain unchanged. These are individual frames, not complete
call-chain/runtime bounds. Historical resource-model calibration remains
REVIEW_REQUIRED; no calibration or lock was rewritten to manufacture a pass.

Local build receipts are in
`/Users/MattTaylor/halo-power-audit-20260928/build229/`; root artifact review
SHA256`db103f9b6ec7cd15bb90107929b503913cde2751a9fc0580c15bad5c7a33ffaa`.
Sense BIN SHA256`42683ca65da21051c9647949b0e8a93f2d8ff4d8380f2a37f110312b567a852d`;
LCD BIN SHA256`57dc9e4890ec5ccf9f9ebfc58b253177e98f8cdd0c3119248c3b754c775b75aa`.

The compactUSB fixture is135bytes including its newline, against the existing
256-byte buffer. The adapter prechecks available capacity and makes one write
without an application retry. “Never wait” in its source comment means no added
application wait/retry, not an absolute SDK guarantee: the qualified HWCDC
implementation uses the configured1ms transmit timeout for its mutex and
ring-buffer paths, subject to RTOS tick conversion. The SDK default100ms is
overridden by `Serial.setTxTimeoutMs(1)` during LCD setup. This is not a hard
whole-write wall-clock bound. The existing timeout is retained. No global serial buffer, timeout or
logging behavior was changed. Concurrent writers may still cause a dropped or
partial observation, which the test must reject rather than repair.


## Hardware installation and ordinary wakes

Sense06 and LCD07 wrote only the inactive application bank and OTA selection.
All protected regions and the 228 fallback were verified. NVS, bootloader,
filesystem and policy-fixture writes were zero. Sense06 release logs ended
before immediate SDK health was observable; LCD07 admission and both later
health checks independently proved the exact installed pair SDK VALID. The
original release-observation gap remains recorded.

| Case | Evidence | Qualification |
| --- | --- | --- |
| Health08 | Eight complete fresh local readings, one exact Sense sample; panel off 10.014764 s, paired sleep and intact 2 a.m. timer fields | Original FAIL retained: earlier peer-arm diagnostic interleaved; 75 s quiet not observed in this case |
| Health09 | Eight complete fresh local readings, one exact Sense sample; panel off 10.014030 s, zero retry arms, next 2 a.m. Pacific maintenance, native paired sleep, 75 s quiet | One new complete cycle; no scheduled execution or physical pixel proof |

Health09 still dimmed after 10 seconds while six natural maintenance keepalives
were received. Its next maintenance start is 2026-09-29 02:00 PDT; the LCD arms
15 seconds earlier by the existing design. LCD boot identities differ between
08 (232575957) and 09 (2026246906). Native Sense sample age was 2000 ms in08 and
7000 ms in09; the latter is not fresh-forwarded evidence. Local readings were
fresh at observation.

For the separate229 observer correction, source review established that each
media-retry arm resets its ACK latch, accepts only the matching fresh token,
interval and positive ACK, and invalidates that token on return. Final anchored
`seconds=0 selected=0 peer_ack=1` therefore proves a successful zero-interval arm.
The revised observer requires that row and the LCD's zero-arm acknowledgement,
retains every prior calendar check, and rejects any intact contradictory earlier
arm row. It does not reconstruct logs or alter firmware. Its38 host tests include
23 negative calendar cases and single-cycle bounds. Original08 remains unchanged.

Cloud08 retained all18 fields for LCD boot232575957, sequence1,3056mV, exactly
matching LCD raw1663–1943/32samples and the native Sense receipt. At report
construction the sample was13,726ms old and marked `fresh:false`. Forwarding and
retention are proved; fresh-at-report and voltage accuracy are not. One exact
read-only cloud query was used; no backend write or deployment occurred.

Receipts under `/Users/MattTaylor/halo-power-audit-20260928/`:

- `bench-private/INSTALL229-PAIR.json`: `a62f737e77a43b7cca52a92560f59ca440d4088a9531f803f07244585b8ff882`
- `bench-private/power229-health-20260928-08/RESULT.json`: `9b2d98417d8829c4b1dbdbc8cd1e842ba923bcadbf1713560bc4a13d0c7e8ad2`
- `bench-private/power229-health-20260928-09/RESULT.json`: `8cdcd8bdcd0b313a07b62a552e8e34fd2efab1cfd012e674459beb796567e462`
- `acceptance229-cloud-health08/CORRELATION.json`: `2361d081e899e1c6be88c25371537fb8f966103451b9ae56e8d37067b7ac536d`
- `acceptance229-prep-r2/HOST-CHECKS.json`: `7e05504a6259aeab1dadfcd37ee76c55e36164f2c82207b54e8e4261e7cf28f5`

These are private qualification records. They do not promote229 to production,
relabel the prior227 full campaign as a229 campaign, or remove the existing
voice/list first-attempt timeout finding. The separate discharge LCD and its
capture services were not accessed or changed.

Health09's separate pre-sleep report reached its response-header deadline
(`code=0 stage=5 result=9 sent=4296 cleanup=1`). No HTTP200 or cloud retention is
claimed for09;08 supplies the independently matched successful retention. The
normal sleep test did not depend on report delivery. ADC initialization in09
showed internal free59292→59112B with largest block42996B unchanged; concurrent
allocations mean the180B difference is not isolated ADC cost. There is no
cumulative UART loss counter, so no emitted drop diagnostic cannot prove zero
drops. [Local review receipt](/Users/MattTaylor/halo-power-audit-20260928/acceptance229-health09-LOCAL-RESOURCE.json).


## Incremental media checks

Image10 stopped before any capture command: Wi-Fi associated at RSSI−82dBm,
but the current boot's15-second SNTP attempt timed out. Retained TLS time was
valid but explicitly unconfirmed. Its original failure is preserved. A narrow
test-only revision records clock readiness as an immediate optional observation
instead of requiring a fresh SNTP response before permitting a normal capture.
Actual identity, empty-store, enqueue, cloud acknowledgement and sleep checks
remain unchanged. Two actual-controller tests cover both action branches and
four failures in the retained enqueue/delivery checks. No firmware change was
made for this adjustment.

Image11 completed one check-in, job8/143472bytes, with intact queue-success and
PUT200/done evidence, native paired sleep, acknowledged zero retry arms, exact
next2amPacific calendar and75seconds without reopening. Initial mounted voice
and image stores were empty. The camera-dimension diagnostic was damaged, so
native dimensions remain unknown. The initial clock observation was unqualified;
final calendar evidence was independently complete.

All six image-upload phase samples were complete, hook enabled, allocation
failures0/loss0. Minima: internal39044B, DMA31380B, largestDMA18420B, PSRAM7634336B,
largestPSRAM7077876B. Initial LCD allocator free79560B/largest78900B. These meet
the documented sampled floors, with14468B internal,14996B DMA and10228B largest
DMA margin. The overlapping internal/DMA margins must not be added. Upload
worker unused stack5568B is a since-creation high-water reading, not an isolated
whole-call-chain bound. No post-image LCD allocator sample was recorded.

Image11 result SHA256`0628b4332cd2744a80396596146056fb96dd7e44c15b0e2a6e89ccbdb0ef8843`
at `bench-private/power229-image-20260928-11/RESULT.json`. Original10 result
SHA256`83d76b275072356519ada726a514a571c7d01af98e874f533e8aa7cd2845721c`.

Cloud verification for image11 found one completed owner/device/grocery job
`b76ac5a864354cfdbe809bd824758f8a`, actionIN, and an S3 HEAD length143472B.
There is no duplicate row in the exact case window. This is identity/mode/time/
length correlation, not native byte-hash equality; no media download was used.
Cloud receipt `acceptance229-media-cloud/image11/RESULT.json`
SHA256`65edb2bb142f3f02db3c6d4bf58dd33af9f3252fd34142fb547f1c4757dbaa13`;
resource receipt `image11-RESOURCE.json`
SHA256`1d330d5e1db2d6d2de60c0be59b618fe349f49c8c905fd1d77c25d1ea4ad57f8`.

Voice12 captured102400B for job8/session`halo-d45c-d3f1-1-5054`; native list and
refresh handlers were observed. HTTP−11/read timeout reproduced atRSSI−49dBm,
so weak radio signal alone is not an explanation. The unchanged list-active
voice path uses1500ms response-read timeout; the normal HTTP path uses60000ms.
This attempt was saved durably, pending1/backoff0, and armed a300-second retry.
Its original controller remains FAIL at the150-second exact202 observation
bound; host closure was1790656383.388. No additional voice capture was sent.

All six first-attempt voice phases were complete, hook enabled, failures0/loss0.
Minima: internal40644B, DMA32980B, largestDMA18420B. Unused upload-worker stack
5564B is a task lifetime high-water reading. Scoped external allocator failures
and default fallbacks were0. Both recorded LCD pool samples were79560B free/
78900B largest; these are before the list state, not list-screen measurements.
Exact request`4d4ccff9ba958d7085a7b47c5862e920`, native SHA256
`d3ca176dd8610827664bc2e75d889278263948014000c701d06f93849787b56b`,
Sense boot293. Original12 receipt
SHA256`15f77790e088dae98e8355c3dcf1ee6aab1261fd9f33faa6e1b80cebada863dc`.
The final native retry is observed in a separate bounded passive case13.


## Final passive recovery and closure

Case13 used the unchanged exact-device capture-only runner, with no command
inbox, actuator tap, reset, network change or replay request. Sense boot294
automatically retried the same session/job8/102400B at1790656591.357 and received
202 at1790656597.402. LCD delivery code8 and post-delete saved inventory0/pending0
were observed. The native delete-control row prints job0; the same-capture exact
job8/session delivery plus the single-item inventory transition supplies the
association, not a fabricated job8 delete line. Both media arms were acknowledged
at0, and native Sense/LCD sleep followed with LCD backlight/panel0. Final LCD
maintenance starts2026-09-29 02:00PDT, target15seconds early. The recorder retained
127.5656seconds of quiet after both descriptors disappeared. Original12 remains
failed;13 is separate recovery evidence.

The retry had six complete voice phases, HTTP202, hook1/failures0/loss0/complete1.
Point minima: internal40836B, DMA33172B, largestDMA18420B. These remain above
documented floors. The three229 upload windows contain18 complete phase samples.
They do not qualify continuous TLS/header String peaks or untested provisioning/
OTA concurrency.

Passive13 capture result
SHA256`9fdacf30d1dd694238c4bd27bb87a201bda8536996896e9af81cf673b9749e3e`;
stop-reason SHA256`36832ae7a79929b815d827f45c3dc3ec30ce7e42085af7f3d17291551f5b4fa6`
in `bench-private/power229-voice-recovery-20260928-13/`. Root rehashed all nine
closed files. `RECOVERY22913-CLOSURE.json` confirms worker and child exited, all
descriptors closed, no lease owner and no awake target USB ports. No additional
hardware action is scheduled. Both production manifests were freshly read back
as unchanged224; `FINAL-PUBLIC-MANIFESTS.json`
SHA256`ba78120e210bb0640c0628ea62090c4570b245f970d0b48b63f1955b3171249b`.

See `ACCEPTANCE-INDEX.json` for exact independent media cloud/resource receipts.
The test audio was ambient; transport/custody is qualified, not a new spoken
shopping-list semantic test. The separately matched227 Bananas test supplies
that earlier semantic evidence.

Final exact-request cloud readback found two ingest receipts (original and
automatic replay) but one unchanged completed job, one worker attempt, enqueue
count1 and one unchanged dashboard record. Accepted/completed times, payload
SHA/length, transcript and response were unchanged. The ambient transcript
produced actionnull; no new list mutation was observed in the original scoped
readback. The original native−11 remains a failure; deduplication and later
native retirement are separately proved. Retry cloud receipt
`acceptance229-media-cloud/retry13/RESULT.json`
SHA256`8fec369d6d3b196c3965665ff781ebe44daf121e8b084787e77a0bd7b8fdb0a2`;
resource receipt `retry13-RESOURCE.json`
SHA256`1c52dfffe73feab37f35b7c45d6440ea233fa77c9949d478b63ee670a0b4a657`.
Both resource observers preserve task-lifetime stack and point-sample limits.
