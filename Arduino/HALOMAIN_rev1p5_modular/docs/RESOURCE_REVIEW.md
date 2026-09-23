# Resource review for firmware changes

Use this alongside [development and release](DEVELOPMENT_AND_RELEASE.md).
The initial [both-board audit](RESOURCE_AUDIT_20260922.md) records actual 212
artifacts and proposed optimizations. Production remains 211; candidate 212 is
[on hold](RELEASE_CANDIDATE_212.md). A resource proposal is not an implemented
change or authorization to publish.

## Review every change, measure changes that can affect resources

Every firmware change needs a short resource-impact entry. A wording-only
change may state that buffers, ownership and scheduling are unchanged and
report its build-size delta. Changes to buffers, queues, UI objects, tasks,
network clients, drivers or operation lifetimes need the fuller review below.
Use exact before/after artifacts, not a historical source folder with similar
names. Keep the report with the candidate; never overwrite its predecessor.

```text
Change / boards / exact source and build IDs:
Allocation: maximum bytes, number of instances, alignment and memory capabilities:
Lifetime: acquire, owner/task, transfer, reuse, all release paths:
Overlap: other allocations and peripherals alive at the worst transition:
Failure: unavailable PSRAM, fragmentation, cancellation, retry and shutdown:
Static delta: internal data/BSS, IRAM, RTC, PSRAM, flash and OTA-slot margin:
Measured runtime: internal free/min/largest; DMA free/largest; PSRAM free/largest:
Stacks: task allocation and measured minimum remaining bytes under stress:
Responsiveness: user takeover latency, UI timing, upload timing and awake duration:
Custody: what survives reset/sleep; no lost media, duplicate jobs or stale pointers:
Evidence / remaining unknowns / decision:
```

## Placement and ownership rules

- Keep hardware descriptors, cache-disabled/ISR data, critical locks, ownership
  flags and ordinary task stacks internal unless the exact compiled driver and
  every caller prove an alternative safe. Internal free bytes and largest
  suitable block are separate constraints.
- Prefer explicit PSRAM allocation for substantial CPU-only payloads with
  clear ownership. Verify successful initialization and the maximum allocation.
  Choose and document bounded fallback or graceful failure; silent unlimited
  fallback into internal RAM can recreate the original failure.
- Existing scoped TLS allocation belongs to a particular operation and task.
  Preserve client destruction before scope release, nonblocking ownership,
  cancellation, certificate verification and failure cleanup. Do not make a
  blanket global allocator change to fix one client.
- Do not share a scratch buffer merely because two normal paths appear
  sequential. Prove retry, cancellation, callbacks and user takeover cannot
  overlap. Use an explicit lease and generation/ownership checks. A foreground
  action must not wait on a long background network call to borrow memory.
- PSRAM belongs to its board and is volatile. It is not shared across the UART,
  and cannot replace SD/flash custody for queued image or voice data. Preserve
  the durable acknowledgment before retiring a capture.
- Drawing payloads can be external while SPI DMA staging and descriptors remain
  internal. Audit both layers. Large UI allocations and camera PSRAM DMA also
  need latency, alignment and cache-coherency tests.
- Keep OTA marker verification, byte/hash accounting, boot selection, abort
  and retry semantics intact. A buffer-location change is still an OTA change
  when it participates in firmware writes.
- Do not shrink stacks from compiler frame sizes alone or enable global SDK
  memory options through application defines. Qualified precompiled libraries,
  actual SDK configuration and runtime overrides determine behavior.
- Check the linked allocator implementation rather than inferring placement or
  bounds from API names. Both current boards use ArduinoJson 7 compatibility
  documents; a legacy capacity argument is not a fixed pool, hard bound or
  stack allocation. Likewise, a response-length check after `getString()` does
  not bound the earlier response allocation.

These constraints are consistent with the pinned
[ESP-IDF 5.5.4 external-RAM rules](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/api-guides/external-ram.html).
Exact board-specific behavior still needs the source/build evidence and device
tests described here.

## Repeatable build report

After the canonical build and artifact checker produce both `verified.json`
files, run the offline report tool using the qualified Python:

```sh
python3 -B tools/resource_report.py \
  --sense-proof "$CANDIDATE/build/sense/artifacts/verified.json" \
  --lcd-proof "$CANDIDATE/build/lcd/artifacts/verified.json" \
  --out "$RESOURCE_REPORT_DIR"
```

`RESOURCE_REPORT_DIR` must be a new directory outside the checkout and input
release workspaces, with an existing parent (for example, a fresh directory
under `/Users/MattTaylor/halo-resource-audit-20260922`). Outputs are `REPORT.json`
and `REPORT.md`. Add `--compare /absolute/path/to/previous/REPORT.json` to show
static deltas for matching board/FQBN/slot layouts. Retain these reports and input
hashes with a reference from the candidate's evidence. Compare with the matching
previous report when changing allocations.
The tool measures compiled sections and symbols; it does not predict runtime
heap, stack peaks, UI performance or safe placement. Linker dummy/alias ranges
and debug sections must not be added to live RAM. Flash-resident assets are not
internal RAM simply because a symbol listing calls them data.

Do not add internal data, IRAM, capability heaps and RTC figures into one
invented available-memory number. The allocator's overlapping capability views
must not be summed. Current symbol sizes are opportunities, not guaranteed
future heap gains. A passing report is evidence for review, not release approval.

## Runtime measurements and finite acceptance

Use the existing memory traces and diagnostics first. For a future candidate
that changes allocation, fill gaps with bounded samples outside allocation
failure hooks/ISRs. Never log, allocate, query heaps or wait in the failure hook.
Tag measurements with board/build/boot, operation and phase. Record allocation
size/capabilities, all dropped samples and whether a counter is cumulative.
Use the SDK's [heap inspection guidance](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/api-reference/system/heap_debug.html)
to distinguish free memory, fragmentation and low-water observations.

Measure before admission, after peripheral/client setup, at peak overlap,
after cancellation and after cleanup. Collect internal free/min/largest,
internal DMA free/largest, PSRAM free/largest, task stack high-water, queue depth
and transport/UI durations. Include the LVGL pool's own free/fragmentation
figures while it remains separate from the system heap. A return-boundary heap
sample is not the peak. Missing task/phase measurements remain unknown.

Choose cases for the changed owner and its neighbors:

| Transition | Required evidence |
| --- | --- |
| Provisioning/AP + station + account claim, then immediate image or voice | First attempt and retry; setup success/failure/timeout; AP cleanup; no stranded allocation |
| Voice upload while browsing, refreshing or deleting list items | UI responsiveness; correct list IDs; cancellation/resume and exactly one durable job |
| Background saved-media retry interrupted by capture | Prompt foreground takeover; no use-after-free, dropped capture or lost queued media; retries stay dark |
| Repeated camera capture after setup and after a failed upload | Correct camera mode/alignment; JPEG checksum/decode; cleanup; retained retry and eventual upload |
| UI screen churn, long lists, animation and media display | UI-pool/heap stability, frame/touch timing; decoder/display/SD ownership |
| Manual and timer-origin OTA; no-update and interrupted download/transfer | Marker/hash/size checks, correct board/slot, flash-safe buffers, recovery and return to sleep |
| Allocation failure, reset, deep sleep and wake | Every acquired resource released or deliberately retained; durable data survives; no unexplained visible wake |

Run relevant host failure-injection tests, the full host gate for firmware
changes, canonical builds and finite physical tests on a private candidate.
Choose a finite repetition count before each device campaign and preserve every
failure. Do not equate repeated identical host runs with hardware coverage.
Compare the same transitions and board/profile under similar network conditions;
record actual minimum margins and latency rather than inventing a universal
safe free-heap threshold. Reject newly starving hardware allocations,
unbounded growth, corrupted media, lost custody or worse foreground behavior.

Change one allocation family at a time so that measured improvements and
regressions can be attributed. Keep production unchanged until an explicitly
approved release, even when an optimization looks promising.
