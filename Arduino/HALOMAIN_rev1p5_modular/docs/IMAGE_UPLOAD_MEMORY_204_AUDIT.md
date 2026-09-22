# Image PUT memory audit — private Sense204

The offline HTTP, ownership/resource and exact-ELF review found no new firmware
blocker in the image URL-buffer removal. The additional complete-PUT harness
passes, and the expanded full host gate passes all 112 suites. This supports
the scoped mitigation; it does **not** prove that the first-wake AES allocation
failure is cured.

At the offline-audit checkpoint the bench retained private Sense203 with LCD201.
The subsequent204 installation and boot/sleep result are recorded below;
post-provision upload acceptance remains pending. Public paired201 and EOL197
are unchanged. No LCD image was built or published for this candidate.
See [the camera/upload investigation](POST_PROVISION_MEMORY_202.md) for the
physical203 failure and successful durable recovery that motivated this work.

## Candidate identity and evidence

- Build: `6.4.204-20260922T041510Z-ea8c32fe1325`.
- Source: `ea8c32fe132543c0abfb8aceedb766147d475e8c`; firmware tree:
  `da64454b2c0103ef7a5967c2a12d82b1a915428e`.
- Reviewed runtime diff: `b493a51df7e3ab1d25f1f4fa0353f93a317dd09e..ea8c32fe132543c0abfb8aceedb766147d475e8c`.
- Private evidence root: `/Users/MattTaylor/halo-provision-memory202-20260921`.

The canonical files under `release204/build/sense/artifacts` were independently
rehashed after the audit-tool correction below:

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `halo_sense_prod.ino.bin` | 1,872,912 | `fb5d7b59e5768d0b1f27effc726ad9fb3342662e662cfe6eeba027eaab4124fc` |
| `halo_sense_prod.ino.elf` | 22,308,452 | `03fb4990666d7aa454ed4ab99c89c181c8c207f46811c924f697854824b96fb9` |
| `halo_sense_prod.ino.partitions.bin` | 3,072 | `1ae446228d79cf83a4b83de41c33d1e01dc21a3557149d7da90fbcbfc303311d` |

[QUALIFICATION.json](/Users/MattTaylor/halo-provision-memory202-20260921/release204/QUALIFICATION.json)
binds the canonical build, artifact checker and original exact-snapshot
111-suite PASS. The later
[112-suite result](/Users/MattTaylor/halo-provision-memory202-20260921/audit204/regression112/RESULT.json)
is a full working-tree gate at documentation descendant `fb44f953f547d6e8dd9763c95cf927d12d4f1079`,
with the integration test/catalog additions and no runtime-source changes.
It reports unchanged source during testing. It does not replace the sealed
snapshot receipt or create a different204 binary.

## HTTP and resource review

The new parser/header writer is used only by image PUT. The worker's local
presign object owns the immutable URL, content type and checksum throughout
the synchronous call and all its retries. The borrowed path is neither queued
nor retained by another task. Supported S3 request bytes, query escapes/order,
headers, TLS verification, body chunks and retry budgets remain unchanged.
The stricter parser rejects malformed inputs outside the existing production
S3 contract. Signed credential fragments are no longer printed.

Every header transport write is at most 512 bytes and checks the existing
deadline, cancellation and foreground state. A failed header cannot enter the
image body. Existing response completion and exact-object412 reconciliation
remain the delivery boundary; uncertain delivery retains the original capture.
No OTA policy, quota/history, provisioning, sleep or durable-custody rule changed.
Voice retains its request/receipt and retry behavior while sharing the
refactored memory trace.

The trace is declared before TLS and finishes after stop/destruction. Linked
call-site review confirms that all clients using its raw owner pointer run on
the one upload worker; other list/claim/OTA clients do not use those hooks.
The allocation callback never dereferences that pointer. Attempt teardown is
idempotent, and the worker retains custody while diagnostics print. This
single-owner finding is specific to the current call graph, not a general
thread-safe guarantee for future instrumented clients.

Detailed evidence: [HTTP review](/Users/MattTaylor/halo-provision-memory202-20260921/audit204/http-review.md),
[resource/ELF review](/Users/MattTaylor/halo-provision-memory202-20260921/audit204/memory-review.md),
and [initial coverage review](/Users/MattTaylor/halo-provision-memory202-20260921/audit204/coverage-review.md).
The initial coverage report predates the integration test below.

## Tests and compiled-path checks

The original helper test passes 8,891 ASan/UBSan checks covering exact signed
wire bytes, parser bounds, partial writes and every-byte failure/cancellation.
The new [whole-PUT comparison](/Users/MattTaylor/halo-provision-memory202-20260921/audit204/put-comparison03/RESULT.json)
passes **154 scenarios / 1,769 assertions**, compiling the production PUT,
actual cancellation client and diagnostic code, with the previous PUT function
run through the same harness. It covers final-header failure without body
transmission, original request bytes across retries, cancellation/deadlines,
complete412 versus uncertain replies, DMA/HTTP cleanup and report lifetime.
The new test is included in the passing112-suite gate. Existing priority,
sleep/worker-custody, immutable-image and OTA-policy suites also pass.

The [corrected exact-ELF collector receipt](/Users/MattTaylor/halo-provision-memory202-20260921/audit204/memory-verified-elf/RECEIPT.json)
passes **44 automated checks**. Its explicit manual-review requirements are
addressed in the resource/ELF review; the script alone does not qualify firmware.
The actual build consumes the pinned SDK flags, including
`-mdisable-hardware-atomics`. Review binds the internal diagnostic gate address
to the linked dispatcher's single-CAS path, with no retry/critical-section
branch on that path. The camera wrapper, internal flag access, actual camera
allocator linkage, flag clear and existing OV2640 guard preserve203r2 behavior.
Callback/ring logic is unchanged and performs no allocation, heap query or
printing. No broader cache-off/ISR safety claim is made.

Static global RAM remains 159,508 bytes; the diagnostic ring remains 96 bytes.
The compiled upload-worker frame rises from 1,776 to 1,936 bytes, and the voice
frame from 1,136 to 1,152 bytes. The task allocation remains 12,288 bytes. These
are individual frames, not a complete nested stack bound or measured high-water
margin. Diagnostic printing can allocate a small temporary buffer after TLS
stop; it is bounded output, not allocation-free or a hard wall-clock guarantee.

## Corrected audit-tool incident

The initial collector incorrectly ran `objcopy --dump-section` without a
separate output ELF. That command rewrote nine non-section header bytes in
each compile/artifact ELF copy; code/data section bytes were unchanged. Both
copies were restored byte-for-byte to the canonical ELF hash above. The
corrected private collector supplies a separate output and verifies that its
input hash remains unchanged. The final44-check review uses the restored
authoritative artifact.

[AUDIT_TOOL_CORRECTION.json](/Users/MattTaylor/halo-provision-memory202-20260921/audit204/AUDIT_TOOL_CORRECTION.json)
records the changed/restored hashes and supersedes the preliminary
`memory-elf/RECEIPT.json` and `memory-artifact-elf/RECEIPT.json` as original-ELF
qualification evidence. Those receipts remain retained. This was an audit-tool
mutation, not a firmware rebuild; no device or cloud action occurred in that
audit. The application binary remains the canonical hash above.

## Remaining physical limits

SDK sockets, heap behavior, time and storage boundaries in host tests are
doubles. The complete PUT is exercised, but its surrounding worker, backend
reconciliation and concurrent sleep scheduler are separately tested. A
synchronous SDK write cannot be interrupted midway by the new helper.

The diagnostic ring observes process allocations during the owner's interval,
not only TLS allocations. Client construction precedes the image trace window;
heap snapshots are not low-water measurements. `complete=1` describes valid,
non-overflowed ring evidence, not successful upload or delivery of every log
line. TLS byte counts include headers; inner attempt numbers restart on outer
retries.

Pending204 device acceptance must establish the immediate post-provision
capture and first-attempt PUT200, exact cloud JPEG size/hash/decode, user
interruption with preserved custody, ordinary voice and paired sleep. Saved
recovery/dark behavior and another camera capture retain their own physical
limits. Installation, host PASS and compiled-path review alone cannot close
the AES investigation or establish full-product/OTA qualification.

## Private bench installation and boot result

`service204-03` subsequently installed the exact audited204 Sense image into
app1. Both complete banks were backed up; Sense203/app0, NVS, partition table,
filesystem and the selected old selector sector were preserved. The two writes
were the padded inactive image and alternate NEW selector. Independent receipt
review verifies the protected-range checks, candidate checksums and clean
descriptor/lock/child closure. No LCD image, public OTA or EOL change occurred.

`postservice204-health01/HEALTH-RESULT.json` records the separate runtime result:
the release boot log identifies204/app1, registers the allocation hook, and
reports successful SDK mark-valid with current readback. Fresh peer queries
identify LCD201/app1/SDK VALID and boot-ready. Wi-Fi connected, both boards
entered deep sleep, and captured LCD heartbeats show backlight/panel off.
The next maintenance target remains September22 at02:00Pacific; the LCD logs
its timer arm. The ACK line is interleaved, so it is not a complete standalone
JSON acknowledgement. This is USB service/boot/sleep evidence, not a paired OTA
or physical power-cycle test.

Earlier attempts remain retained: `service204-01` refused a truncated final
UI-state reply before any flash; `service204-02` refused an initial macOS EBUSY
before opening either board or sending commands. The bounded host controller
now re-requests a complete UI reply, and the capture retries only initial EBUSY
for two seconds with fresh USB identity and cu/tty ownership checks. Twenty
focused controller tests passed; no firmware guard was relaxed. The actuator
probe also failed before opening USB or sending a stroke; its worker was later
observed absent, without claiming normal reaping.

The passive `reprovision-camera204-01` capture was then armed for the user's
normal app provisioning followed immediately by Check-in/Confirm. That upload
acceptance is pending; do not infer an AES fix from successful installation.
Private raw logs/backups remain in the case root and can contain credentials.
