# Private supply telemetry candidate

September 28, 2026. Candidate6.4.228 is built from clean source
`08836719356fa92dca5aa9cc6954074ef23ed42c`, build
`6.4.228-20260929T031433Z-08836719356f`. The canonical Mini full128-suite
snapshot gate and paired production builds passed before device work. Finite
device acceptance stopped with the two telemetry defects documented below; no publication is authorized. Public firmware
remains224; the pre-install bench baseline is private227. Continue from the227
descendant branch, retaining RAM, shopping refresh, haptics-off and10-second
visible-inactivity fixes. Do not change the separate discharge probe.

## Measurement and ownership

The original handoff is
`/Users/MattTaylor/halo-voltage-probe-20260928/handoff-20260928/FIRMWARE_HANDOFF.md`.
Its GPIO1/ADC1_CH0, nominal10k/10k divider and calibrated×2 rule measure the LCD
system supply. USB can mask cell voltage. No percentage curve, charger inference,
low-voltage policy or change to OTA allowance/schedule is implemented.

LCD main-loop owns one ADC oneshot unit and curve-fitting calibration handle.
Each awake burst attempts at most32 conversions, calibrates each before averaging,
and is capped at100ms. Driver errors, calibration errors, raw endpoints and partial
bursts invalidate the value. Initialization failure cleans partial handles and is
not retried in a loop. Final sleep deletes handles; an aborted sleep retains them.
No new task, interrupt, NVS writes or persistent media schema exists.

Sampling runs at most1Hz outside the LVGL lock. A short critical section copies
the fixed POD mailbox; no ADC, JSON, logging or UART operation occurs inside it.
The UART owner coalesces optional reports at most once per3seconds, with first and
forced pre-sleep samples exempt from cadence. It also requires a recent accepted
PONG/SYNC_ACK, ordinary transport mode, and no queued user work. No new probe,
peer wake or ACK/retransmission is introduced. A failed write is attempted only
once for that sequence. A single driver write carries both newline delimiters.

Normal sleep gets one absolute150ms opportunity, returning to the main loop to
sample without holding the UI lock. User activity cancels it. Missing peer,
ADC error, transport busy or missing write confirmation cannot renew the budget.
Both direct headless sleep paths use a bounded local burst from the same unlocked
loop owner and never wait for or wake the peer. An absent Sense cannot receive
that final observation; no extra cloud connection is created to deliver it.

Sense consumes power frames before activity/liveness accounting. Its volatile
cache resets to missing at reboot/deep sleep, and duplicate/late sequence numbers
cannot renew freshness. Monotonic receive time alone would falsely rejuvenate a
queued UART frame. Sense therefore uses a conservative epoch age including one
second of quantization, then advances it monotonically; missing/inconsistent
paired clock evidence yields unknown age and `fresh:false`. A measured stale
voltage retains its provenance; a failed acquisition emits null voltage.

See [payload coverage and backend limitations](SYSTEM_POWER_TELEMETRY.md).
Transmission metadata stays outside immutable media bodies and signed diagnostic
envelopes. Ordinary report bodies retain it today; other API handlers currently
ignore the added observational header. No backend deployment is included.

## Resource review before build

| Resource | Ownership and bound | Qualification |
| --- | --- | --- |
| LCD driver/calibration | Two boot-lifetime SDK handles; main-loop only; cleanup on partial failure/final sleep | SDK allocation amount pending hardware/ELF evidence; initialization logs internal free/largest before/after |
| LCD/Sense cached values | Fixed32-byte Snapshot; short portMUX copy; no queue/history | Actual ELF static deltas pending |
| LCD UART scratch | 256-byte automatic buffer; max-width host frame220 bytes | Actual stack/frame evidence pending |
| Cloud header scratch | 512-byte automatic buffer; max model fixture421 bytes | Request-owned HTTPClient header String additionally persists through request, with allocator overhead |
| Bounded report POST | Existing768-byte header plus512-byte power fragment, three added pointer/size entries | Same existing request/cleanup deadlines; full call-chain stack pending |
| Ordinary report object | Filled directly in existing JsonDocument from one cache view | No second parse document/string roundtrip; allocation failure refuses incomplete report |
| Durable storage | None added | Saved media IDs, bytes, hashes, custody, replay/HMAC unchanged |

These are allocation/ownership bounds, not measured global free RAM. Internal/DMA
views overlap and cannot be summed. The retained227 report and runtime findings
are the comparison baseline; historical resource-model source drift remains
review-required and receives no speculative PSRAM credit. Build and device
results must be appended as dated evidence, not inferred from host tests.

The100ms/150ms budgets are cooperative checks before SDK calls, not preemption
of a stuck SDK call. Epoch-based age assumes usable paired clocks; clock skew
has not been electrically measured. Qualified HTTPClient3.3.8 creates temporary
name/value Strings and a local headerLine before appending to its retained
headers. Those can overlap header reallocation: one retained String is not the
peak. At the511-byte header ceiling, the value and line alone can coexist at
512+535 bytes before retained headers/allocator overhead. Device measurements
must cover that transient, not just the512-byte formatter stack buffer.

Focused tests execute the real shared cache/parser/formatters, LCD owner/pre-sleep
adapter and bounded HTTP writer with hardware/clock/I/O doubles. They cover
read/calibration/initialization failure, saturation, partial samples, delayed and
missing clocks, duplicate frames, stale cache, missing hook, short writes,
user takeover, coalescing, header injection/truncation, immutable body/signature,
cancellation and deadline preservation. They cannot qualify electrical accuracy,
radio behavior, RTOS scheduling or physical user experience.

## Finite intended device acceptance

Preserve227 fallback and all device data while installing only the identified
paired bench devices. Observe valid USB-fed supply measurements and matching
LCD/Sense sample identities; retain ADC initialization resource observations.
Check ordinary/pre-sleep cloud snapshots, two wake/Home/sleep cycles, one camera
upload and one voice/list interaction with resource samples. Stop on failures
and preserve evidence. No induced discharge, production publication or changes
to the protected probe's services are part of this campaign.

## MacBook working-gate attempt

`/Users/MattTaylor/halo-power-audit-20260928/full-gate-01/RESULT.json` is a failed,
incomplete128-suite attempt, not a pass. It recorded96 completed suite entries
before interruption. The unchanged LVGL OOM fixture reached its intended
`lv_mem_buf_get -> abort -> pthread_kill` path, then remained in macOS UE state;
the outer runner recorded its300-second timeout. Two historical negative-control
children subsequently stalled in the same exiting state. The operator interrupted
the owned runner and its parent processes rather than repeatedly waiting on that
host condition. Source and assertions were not relaxed. Child/core-dump handling
is an unresolved host limitation, not evidence of an allocator-loop regression.

The complete unchanged firmware must pass the full128-suite gate on the qualified
Mini before compilation/device work. The canonical build wrapper runs that exact
snapshot gate before either compiler; a failure must stop the build. Retain the
MacBook receipt even if the Mini gate passes. Moving temporary directories is not
an established fix for the MacBook problem.

## Qualified Mini build and resource comparison

Canonical job `20260928T201627-fc001bf3` completed with exit0. The exact
128-suite snapshot passed before either compiler, including the unchanged
LVGL intentional-abort test that stalled on the MacBook. The environment lock,
camera-driver checks, paired artifact verifier and candidate verifier passed.
The separate MacBook failure above remains recorded.

| Board | Application bytes | OTA slot remaining | Static DRAM change from227 | Application change |
| --- | ---: | ---: | ---: | ---: |
| Sense | 1,880,960 | 85,120 (4.33%) | +64 | +4,624 |
| LCD | 2,046,528 | 574,912 (21.93%) | +336 | +12,896 |

IRAM and RTC section totals are unchanged on both boards. The LCD's measured
336-byte DRAM delta includes newly linked SDK state; it is not merely the113-byte
source mailbox estimate. The static report is authoritative for linked totals.

Individual compiler frames: Sense bounded `write_request`896→1440bytes;
`parse_input_message`768→896; header helper656/592 in its two translation units.
LCD UART task1648→1664, new power-send helper432, sampling owner96; loop320→288.
These frames cannot be summed into a complete task bound or credited as runtime
headroom. Device samples must assess overlapping calls, HTTPClient temporary
Strings and ADC/calibration initialization. The historical resource guard remains
`REVIEW_REQUIRED` for source drift; its calibration/lock was not rewritten.

Sense BIN SHA256 `99c563cc28e660dadf71ac89ca934c1388c56ba9f703e005686d029c0255f11a`.
LCD BIN SHA256 `0e49f9db147420fddfb682ee07f9bdffdbf9d33f9ff993cf3b62bc3c7a6c475e`.
Mini candidate: `/Volumes/Trepo-Work/Workspaces/halo-firmware-bench/candidates/6.4.228`.
Local evidence: `/Users/MattTaylor/halo-power-audit-20260928/build228/SUMMARY.json`;
root artifact review `ROOT-ARTIFACT-REVIEW.json` in that directory. The mirror
`receipt-bundle05` retains128-suite logs/proofs and resource/frame reports.

The inactive-bank installer passed22 offline guard checks and was separately
reviewed against the prior227 installer. Its authorization is private bench only:
protect227/app0 and all device data while writing228/app1 after fresh exact-board
identity and empty mounted-media inventories. Installation/SDK validation and
physical behavior are separate evidence, not implied by this build result.


## Hardware result: installed, telemetry acceptance failed

Both boards were installed on app1 and observed SDK VALID, with227/app0 and
protected device data preserved. Service receipts live at
`/Users/MattTaylor/halo-power-audit-20260928/bench-private/INSTALL-PAIR.json`.
The first separate readiness-to-installer handoff expired during natural sleep,
with no commands or flash writes; the integrated readiness installer then passed.

The first health cycle (Sense boot284, LCD boot1846845330) measured Home panel-off
at10.0147seconds despite six maintenance keepalives. Both boards slept normally;
the recorded normal nightly target was September29,02:00Pacific, with media retry
arm0. The case stopped before its75-second quiet qualification and second wake.
It remains FAIL_OR_INCOMPLETE, not a passing power test.

LCD ADC initialization succeeded and partial USB prefixes showed3182–3200mV.
Those truncated records cannot qualify complete sample provenance. The requested
JSON readout exceeded the bounded256-byte USB output buffer. Separately, the
optional UART send path required `link_synced`, although this ordinary boot had
an accepted PONG and awake proof without a SYNC exchange. No power frame reached
Sense. One exact read-only cloud query confirmed that boot284's ordinary
pre-sleep report retained `system_power.status=peer_missing`, null voltage and
false valid/fresh flags. Cloud retention works; complete measurement forwarding
did not pass. No further228 media test was run after this failure.

Raw evidence: `bench-private/power228-health-20260928-05` under the audit directory;
cloud correlation: `acceptance228-cloud-health05/CORRELATION.json`. Candidate228
bytes remain immutable. The narrow correction will use a separately inventoried
version, preserve the bounded USB timeout and all transfer/sleep/user-work guards,
and must pass independent tests before another device acceptance attempt.
