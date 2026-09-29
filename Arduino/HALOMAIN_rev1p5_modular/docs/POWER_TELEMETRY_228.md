# Private supply telemetry candidate

September 28, 2026. Implementation checkpoint, pending complete host gates,
canonical paired builds and finite device acceptance. Intended unused version
6.4.228 was inventoried; it is not yet built, installed or published. Public
firmware remains224; the bench baseline is private227. Continue from the227
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
