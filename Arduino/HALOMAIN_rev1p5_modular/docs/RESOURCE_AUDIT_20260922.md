# Both-board resource audit — September 22–23, 2026

This is an application resource audit and optimization proposal, not new
firmware or physical qualification. Production remains **211**; the exact **212**
candidate remains [on user hold](RELEASE_CANDIDATE_212.md). Do not change its
source snapshot, artifacts or staged objects. A runtime experiment requires a
fresh unused version (213 or later after inventory) and private bench evidence.

## Evidence and scope

Reviewed production application source, both wrappers, shared transports and
drivers, retained allocation diagnostics, and the exact canonical 212 artifacts:

- Source `ee3fd65bd15cb7206e5e4493faa8fbcc3a1693a1`, firmware tree
  `2eab1b218209d96e722d330a313f7f3e4024c348`.
- Build `6.4.212-20260922T204419Z-ee3fd65bd15c` in
  `/Users/MattTaylor/halo-firmware-candidates/6.4.212`.
- Source at audit start: documentation descendant
  `107bae48fcd332acce74e65dab312bb7b5bd19d1`.
- Arduino ESP32 3.3.8 / IDF 5.5.4, production flags, pinned libraries and actual
  linked ELF symbols. 212 differs from 211 in metadata and the OTA power reminder,
  with no resource-ownership or scheduling change.

The audit covers camera, microphone/audio, provisioning, Wi-Fi/TLS, image/voice
uploads and retries, shopping lists, OTA, LVGL/display/animations, UART, tasks,
queues, diagnostics and persistent storage. It is not a whole-SDK correctness
proof, custom-PCB qualification, new peak-memory measurement or full device test.
Dynamic peaks and worst-case stack margins remain unknown where unmeasured.
Detailed investigation evidence is under
`/Users/MattTaylor/halo-resource-audit-20260922`.

## Resource boundaries

Sense and LCD are separate ESP32-S3 processors with independent internal RAM
and octal PSRAM. Free LCD memory cannot satisfy a Sense allocation. UART runs at
115200 baud; even its ideal 8-N-1 payload ceiling is only 11,520 bytes/s before
framing and acknowledgment overhead. Moving bulk processing between chips can
increase latency and resource ownership complexity. Keep network/camera/audio
on Sense and display/typed SD media custody on LCD for these proposals.

PSRAM is volatile; queued captures still need existing flash/SD persistence
across deep sleep or reset. Extra flash capacity cannot substitute for internal
DMA RAM. The [hardware review](HARDWARE_RESOURCE_REVIEW_188_20260917.md) records
devkit/custom-PCB distinctions; its older runtime observations are historical.

### Exact compiled baseline

| Resource | Sense 212 | LCD 212 | Interpretation |
| --- | ---: | ---: | --- |
| Static internal data/BSS/noinit | 159,524 B | 204,356 B | Excludes dynamic task stacks, queues and driver allocations |
| IRAM sections, including alignment | 81,152 B | 85,248 B | Separate from data/BSS; do not add dummy aliases |
| RTC fast / slow sections | 136 / 2,528 B | 136 / 268 B | Retained state, not ordinary heap |
| Actual OTA application BIN | 1,873,408 B | 2,033,088 B | Binary stored in a slot, not ELF/debug-file size |
| Actual application slot | 1,966,080 B | 2,621,440 B | From verified production partition proof |
| Remaining slot space | **92,672 B (4.7%)** | 588,352 B (22.4%) | Sense firmware growth needs particular attention |
| Static external allocations | No application external BSS pool | No application external BSS pool | Dynamic PSRAM allocations are separate; linker dummy ranges are not RAM allocations |

The Arduino Sense compilation message reports a larger sketch maximum than
the deployed application partition. Use the verified slot and actual BIN,
not that headline limit, for release decisions. Report IRAM/RTC separately;
do not add linker aliases or overlapping heap capability totals.

Actual compiled SDK configuration uses octal PSRAM at 80 MHz, a 4,096-byte
ordinary-malloc internal preference threshold, zero configured internal-malloc
reserve, Wi-Fi RX limits of 8 static/32 dynamic buffers and 32 dynamic TX
buffers. These are configuration limits, not a sum of permanently live memory.
Normal task creation still allocates internal stacks. The external-stack
permission does not move existing tasks. The scoped TLS allocator overrides
selected client allocations at runtime; the SDK's default TLS flag alone does
not describe those clients. Precompiled-library configuration cannot be safely
changed by adding unrelated application defines.

## Inventory and current ownership

These are retained symbols and allocation requests in the exact build. Rows
overlap the static totals above; they must not be summed into a new peak estimate.
Requested heap bytes omit allocator overhead unless explicitly stated. Detailed
source, disassembly and proof references are in the
[Sense investigation](/Users/MattTaylor/halo-resource-audit-20260922/sense-audit.md)
and [LCD investigation](/Users/MattTaylor/halo-resource-audit-20260922/lcd-audit.md).

### Sense

| Owner | Storage and lifetime | Implication |
| --- | --- | --- |
| OTA applier | 65,536 B marker workspace + 4,096 B write staging, permanent internal BSS | Largest isolated internal-data opportunity; preserve flash-safe staging |
| Shopping model | 12,000 B internal BSS for 50 complete item records | CPU-only cache candidate; keep mutex, IDs and complete-copy ownership |
| Main queues | Operation 1,292 B; upload 5,372 B; unused UI 2,492 B requested internal, each including a 92 B queue control block | Used queues carry operation/media metadata; never reduce capacity without backpressure/custody tests |
| Unused microphone mutex | 92 B requested internal | No consumers found; combined with unused UI queue, 2,584 B cleanup candidate |
| Application stacks | Loop 16,384 + operation 16,384 + upload 12,288 + audio 4,000 = 49,056 B internal requests, plus TCBs | SDK tasks and temporary camera worker are additional; compiler frames are not high-water measurements |
| Camera | Two PSRAM framebuffers; retained JPEG copy is PSRAM-first; temporary 4,096 B grab-worker stack; conditional 16,384 B internal DMA reserve | Buffers/copy can overlap. Reserve is released for setup/camera initialization. Keep qualified alignment/cache fallback and real driver completion |
| Voice | 524,288 B PSRAM-first staging, retained while awake; normal 10-second limit produces up to 320,000 B PCM copied into a queued buffer | A full recording can request 844,288 B payload/staging, normally external, before other jobs; lazy allocation or zero-copy needs new ownership |
| Audio | RX/TX channels, 4,000 B worker (already counted above), 2,048 B read buffer; per-frame 2,048 B PSRAM-preferred scaling scratch | Active paths, not dormant demo code. Reuse scratch before considering channel/task removal; DMA controls stay internal |
| Claim/image TLS | Existing task-owned PSRAM preference for selected mbedTLS allocations | Preserve operation scope through client destruction; voice/list do not yet use that scope |
| Queued media | Fresh payloads PSRAM-first; saved replay explicitly PSRAM; LCD SD and retained fallback paths provide durable custody | Ten maximum 512 KiB queue entries imply a theoretical 5 MiB payload capacity, not a measured peak or guaranteed fit |

Image/framebuffer capacities and actual driver/TLS peaks remain unmeasured.
The upload worker serializes expensive media networking and claim; foreground
capture can park a job until cancellation cleanup completes. MQTT creates no
client, queue or worker in current production, so those dormant allocations
cannot be counted as future savings. Important code owners include
[`Sense_Minimal.ino`](../Sense_Minimal/Sense_Minimal.ino),
[`sense_camera.h`](../Sense_Minimal/sense_camera.h),
[`audio_bsp.c`](../Sense_Minimal/audio_bsp.c),
[`sense_upload_persist.h`](../Sense_Minimal/sense_upload_persist.h) and
[`ScopedTlsMemory.h`](../halo_ota_demo/firmware/shared/ScopedTlsMemory.h).

### LCD

| Owner | Storage and lifetime | Implication |
| --- | --- | --- |
| Fixed LVGL arena | 98,304 B internal BSS, 48.1% of static data | Widgets, decoding and drawing scratch share this bounded pool. Existing PSRAM allocator helpers are inactive in this configuration |
| Shopping models | Active and pending 8,808 B each, 17,616 B internal BSS total | Both copies support UART/UI handoff; do not collapse them into one shared mutable model |
| Voice UI | Six pages totaling 7,212 B plus 4,096 B incoming JSON storage | 11,308 B internal BSS candidate; preserve generation, parse and UI ownership |
| Queues | UI events 10,844 B; UART TX 1,532 B requested internal, including control blocks | Depth/union size affect overload behavior and provisioning/input reliability |
| Application stacks | UI 12,288 + UART 12,288 + loop 8,192 = 32,768 B internal requests, plus TCBs | No stack reduction justified; a smaller UART stack previously overflowed |
| Display draw buffers | Normally two 25,920 B PSRAM-first allocations; internal DMA fallback. Recovery uses one 2,880 B DMA-first buffer | Already mostly external. SPI staging/descriptors are separate; teardown ownership needs repair before broad pool migration |
| Typed SD spool | 7,552 B retained scratch across image/voice; small store metadata; chunked SD I/O | LCD does not load the full media file to replay it. Preserve leases and backend acknowledgment before deletion |
| UART protocol | Three 580 B malloc buffers = 1,740 B per protocol instance, plus object; image/voice instances can remain after use | Potential later lifetime cleanup; partial RX, duplicate checks and OTA cannot borrow the same scratch without proof |

The linked LCD Wi-Fi SDK state does not establish live radio/TLS use. Legacy
LCD audio routines appearing in compiler stack files are not retained live
tasks in the production ELF. Flash icon/font assets are not internal heap.
Important owners include [`lv_conf.h`](../LCD_Minimal/lv_conf.h),
[`LCD_Minimal.ino`](../LCD_Minimal/LCD_Minimal.ino),
[`lcd_bsp.c`](../LCD_Minimal/lcd_bsp.c),
[`lcd_activity.h`](../LCD_Minimal/lcd_activity.h),
[`lcd_image_spool.h`](../LCD_Minimal/lcd_image_spool.h) and
[`lcd_voice_spool.h`](../LCD_Minimal/lcd_voice_spool.h).

### JSON and response bounds need actual allocator evidence

Both boards link ArduinoJson **7.2.0**. Its compatibility
`DynamicJsonDocument(N)` and `StaticJsonDocument<N>` use heap-backed documents;
the old capacity argument is not a reserved fixed pool or hard memory bound.
Do not claim 2,048 B saved from a request document, a 32 KiB PSRAM list parser,
or stack storage just from those source spellings. Ending a serialized request
document's lifetime before TLS remains useful, but savings depend on its actual
contents and allocator requests. Keep the serialized body independently owned.

Several HTTP paths accumulate `getString()` before checking response length.
Later length/JSON checks do not bound that earlier allocation. A bounded response
reader is a separate robustness proposal: test framing, chunking, truncation,
oversize refusal, cancellation and retention of the old list on error. Preserve
current contracts and do not combine this with a broad allocator migration.

## Ranked opportunities

All savings below are proposals. Static sizes are measured symbols; dynamic
figures are identified requested allocations, not measured peak savings.

| Priority | Proposal | Potential effect | Risk and first proof needed |
| --- | --- | --- | --- |
| 1: small cleanup | Remove unused Sense UI event queue and microphone mutex, after final consumer review | About 2,584 B of requested internal heap plus allocator overhead | Low relative scope; boot, audio capture, camera and user-interruption smoke tests |
| 1: shorter lifetime | Destroy the serialized presign request's JSON document before opening TLS | Content-dependent heap relief during presign; unmeasured | Low relative scope; body remains owned; request identity, cancellation and response handling tests |
| Before broader LCD migration | Repair draw-buffer/mutex ownership across display teardown and reinitialization | Avoid retaining up to 51,840 B of normal draw buffers per teardown cycle, plus mutex; usually PSRAM | Medium; source-level missing cleanup, physical accumulation unmeasured; join DMA/callback/UI owners before freeing |
| 2: targeted allocation | Extend the existing scoped TLS preference to one voice or list transport at a time | Variable internal relief during connections; no promised byte saving | Medium; prove task ownership, cancellation/client destruction and foreground list/voice behavior |
| 3: CPU-only cache | Move Sense's shopping cache to explicitly owned PSRAM | 12,000 B internal BSS candidate | Medium; initialization/fallback, locking, stale pointers, refresh/delete/capture overlap |
| 4: largest Sense opportunity | Allocate the 64 KiB OTA marker workspace in PSRAM only during the OTA operation | Remove 65,536 B permanent internal BSS, less owner metadata | Significant OTA risk; preserve marker/hash/write/restart/abort behavior and qualify actual transfers |
| 5: largest LCD opportunity | Put the fixed LVGL pool in PSRAM while retaining its bounded allocator | 98,304 B internal BSS candidate | Broad UI impact; exact LVGL configuration, cache/flash lifetime, allocation failure and frame/touch tests |
| Later, measured | LCD data caches; audio scratch reuse/lifecycle; queue envelope packing | Smaller or workload-dependent reductions | Review ownership and actual overlap before pooling or reducing capacity |

The two large opportunities are on different boards; they are not a shared
160 KiB pool. Their potential gains are not additive to small candidates that
already live inside an allocator pool. Prefer isolated changes with an exact
before/after measurement over changing multiple memory policies together.

### Sense OTA marker workspace

[`SenseOtaApplier.cpp`](../halo_ota_demo/firmware/shared/SenseOtaApplier.cpp)
retains `marker_check_buffer[64 * 1024]` and a separate 4,096-byte write buffer
as internal BSS. The marker buffer alone is 41.1% of Sense's static internal
data, despite being used only during firmware installation.

Proposed design: acquire an exclusively owned, checked external workspace
before destructive OTA work; preserve the same initial marker window and
validation; copy verified bytes through the existing small internal write
buffer before flash writes; release on every return, abort and restart path.
Keep ownership/control state and the calling stack internal. Unavailable PSRAM
should produce a bounded recoverable refusal, not silently reserve 64 KiB
internally again or mark a failed update complete. This design is not yet coded.

IDF's [flash implementation](https://github.com/espressif/esp-idf/blob/v5.5.4/components/spi_flash/esp_flash_api.c)
has a small staging path for non-direct input. A direct substitution of an
external pointer can therefore change throughput even when accepted by the API.
Audit the actual linked flash path and use measured internal staging; do not
assume moving the array is the whole fix. Required cases include PSRAM failure,
split/missing/wrong markers, short reads, resume/full restart, cancellation,
hash/size mismatch, interrupted writes, boot selection, both-board update and
normal subsequent capture/sleep. Keep all existing time and retry bounds.

### Existing successful memory work stays intact

207's scoped allocator sends eligible account-claim and image mbedTLS
allocations to PSRAM, including small allocations that ordinary malloc tends
to place internally. Camera JPEG and large voice payloads already use PSRAM;
203 added a qualified camera PSRAM-DMA fallback with specific sensor/driver,
alignment, cache and ownership requirements. These are distinct fixes.

In one actual immediate-provisioning/capture comparison, internal free RAM
after PUT connection rose from 16,764 B to 27,672 B; largest internal DMA block
rose from 5,108 B to 10,228 B. The newer first claim and PUT succeeded, with
complete zero-failure allocation windows and a decoded cloud JPEG. This is
historical case evidence, not a new benchmark or guaranteed peak saving.
See [207's detailed audit](POST_PROVISION_RESOURCE_AUDIT_207.md).

## Sharing, concurrency and responsiveness

### Display teardown needs explicit ownership

`lcd_lvgl_Init()` in [`lcd_bsp.c`](../LCD_Minimal/lcd_bsp.c) allocates raw draw
buffers and a mutex. [`lcd_anim.h`](../LCD_Minimal/lcd_anim.h) can deinitialize
LVGL/panel state and later rebuild it during OTA/headless recovery. The raw
buffers are outside LVGL's pool; neither that teardown nor `lcd_panel_deinit()`
frees them, and reinitialization overwrites the buffer pointers and mutex handle.
This is a source-level retention finding, not a measured leak rate or proof of
the cause of a past device failure. A reboot discards those volatile allocations.

Give the BSP one explicit display-resource owner and an idempotent teardown,
including partial-initialization failure. First stop producers, join real DMA
completion and callbacks, release UI ownership, then free each buffer and delete
the mutex exactly once. Repeated teardown/reinit, failed second-buffer allocation,
user takeover during headless retry, and interrupted OTA are the acceptance
cases. Do this separately from moving the whole LVGL pool to PSRAM.

Prefer reusing a buffer within its existing owner over creating a global pool.
Camera, recording, saved-media upload, provisioning and shopping traffic have
different lifetimes. Cancellation is not complete until a worker releases its
client/buffer and acknowledges handoff. A late callback must not write into a
buffer already lent to the next job. Preserve copied identifiers, generation
checks, durable receipt identity and bounded queue backpressure.

User capture and list interaction must retain priority over background retries.
Save/release a background operation's resources before reuse, keep retries dark,
and measure time to foreground acceptance. Moving memory does not fix blocking
network calls, UART bandwidth or CPU/cache contention. Measure all three.

Do not move interrupt-visible DMA descriptors, SDK hardware buffers, locks,
atomic owner flags or task stacks just because external memory is available.
Do not eliminate an active audio worker or reduce a queue/stack based only on
its name or a single successful run. Preserve display quiescence during OTA,
actual asynchronous DMA completion and the narrow camera cache-coherency path.

## Instrumentation gaps and next work

Current traces cover claim/image/voice allocation-failure phases and internal
and DMA free/largest checkpoints. Durable diagnostics retain some internal
free/min/largest state; selected OTA paths also report PSRAM. Stack telemetry
is limited rather than a measured profile of every application/SDK task.
The fixed LVGL pool needs its own usage/fragmentation measurements in addition
to the system heap. A post-call snapshot can miss a short-lived peak.
LCD's existing UI heartbeat variable named `heap_internal` actually calls
`esp_get_free_heap_size()`, so it is not an internal-capability-only measure.
Use explicit capability queries when making placement claims. The Sense trace
recorder's current owner is not a general cross-task API; adding list tracing
needs proven serialization or a separate concurrency-safe owner design.

For the first chosen private optimization, collect before/after data for the
same sequence: provisioning → immediate capture; voice → repeated list refresh;
background retry → user capture; screen churn; and the affected OTA paths.
Capture internal/DMA/PSRAM free and largest blocks, minimum free memory, stack
high-water, allocation failures/dropped records, queue depth, frame/input/upload
timing and awake duration. Record missing data as unknown; cumulative allocation
requests are not live bytes saved. No new always-on telemetry, SDK tracing or
device command was introduced by this audit.

Use [RESOURCE_REVIEW.md](RESOURCE_REVIEW.md) for the required change record,
offline report and finite test matrix. Source cleanup and a scoped non-OTA
client/cache experiment are the first low-blast-radius candidates; the 64 KiB
OTA and 96 KiB UI changes deserve separate private qualification. Production
211 and held 212 remain unchanged while these proposals are reviewed.

## Repeatable audit evidence

The new offline [`resource_report.py`](../tools/resource_report.py) reads pinned
canonical proofs, actual ELF sections/symbols, SDK configuration and partition
tables. Twelve focused tests passed, including dummy-section exclusion,
flash-symbol classification, actual slot sizing, changed input hashes and output
path protection. The final test also covers the exported snapshot layout, so
the normal sibling regression directory remains usable without permitting
output inside source. See the
[focused result](/Users/MattTaylor/halo-resource-audit-20260922/tool-tests04/RESULT.json).
The copied exported-layout run also passed; its
[validation receipt](/Users/MattTaylor/halo-resource-audit-20260922/TEST-HARNESS-VALIDATION.json)
preserves the earlier path-boundary failure and final test hashes.
Actual paired reports passed for
[211](/Users/MattTaylor/halo-resource-audit-20260922/report211-01/REPORT.md) and
[212](/Users/MattTaylor/halo-resource-audit-20260922/report212-02/REPORT.md).
The comparison shows unchanged RAM and Sense BIN size; LCD flash/BIN grew 48 B.
All referenced report input hashes were rechecked. A prior test-fixture failure
and its correction remain recorded in
[tool validation](/Users/MattTaylor/halo-resource-audit-20260922/TOOL-VALIDATION.json).

The tool test is registered in the full offline regression gate, bringing this
working source to 116 suites; held 212 retains its original 115-suite snapshot.
The final working-source gate receipt belongs at
[`regression-final02/RESULT.json`](/Users/MattTaylor/halo-resource-audit-20260922/regression-final02/RESULT.json).
The first gate was deliberately interrupted while correcting the test helper's
exported-layout boundary; its partial result is retained and is not a pass.
Its actual status and final commit are recorded in the external
[completion receipt](/Users/MattTaylor/halo-resource-audit-20260922/COMPLETED.json).
These are host checks, not firmware builds or new physical qualification.
