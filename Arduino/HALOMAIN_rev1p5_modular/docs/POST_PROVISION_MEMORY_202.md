# Post-provision memory investigation

Candidate work from production201 source `df018153ccd99900836732ca9eebe70bcb840485`; publication, installation and physical acceptance are not implied by this note. Preserve the201 OTA hint repair and all earlier fixes. Evidence workspace: `/Users/MattTaylor/halo-provision-memory202-20260921`.

## What is proved

The199 immediate post-provision voice case failed with ESP AES allocation errors twice, saved the exact recording, then delivered it on a clean automatic retry. Largest DMA/internal free block after setup was12,788bytes. The subsequent201 cucumber case delivered on its first POST despite the same reserve warning and a13,300-byte largest block. Both had about24KB aggregate DMA free but could not reserve one16,384-byte camera block. This establishes lost contiguous headroom; it does not identify the exact failed AES allocation or prove the camera would fail after its existing network recovery.

The scan response cache has a definite lifetime bug: it remains allocated after setup is finished. Arduino3.3.8 `String` retains capacity for assignment from an empty string, `clear()`, and even an empty temporary String. Assignment from a null C string invokes its invalidate/free path. Stop the setup HTTP server, explicitly release this unused scan cache, then retain the existing DNS/AP teardown and DMA admission order. Existing claim ownership, grace period, running-scan and user-priority guards remain intact. Before/after free/largest values quantify the actual contribution instead of assuming it cures all memory pressure.

## Bounded diagnosis

The supported ESP failed-allocation callback records four fixed observations: sequence, requested bytes, capability mask, active voice phase and a bounded hash of the allocator function name. One 32-bit compare/exchange drops a contended observation and sets a sticky loss marker with one store. Once loss occurs, reporting stays conservatively incomplete until reboot; the marker is not a dropped-event count. The hook performs no allocation, heap query, logging, wait or network operation. Overflow and loss are explicit; it never represents incomplete capture as complete.

Voice-owned snapshots record internal free, DMA free and largest DMA block before client construction, before/after TLS connect, before the first write/read and after client destruction. The owner reports them only after its TLS objects are destroyed. The byte counter measures bytes handed to TLS (including HTTP headers), not confirmed cloud delivery. Allocation failures from other tasks during the same phase are possible; the record is temporal evidence rather than a task-stack attribution.

The first-read point can be a zero-byte SDK connection probe, not reception of the HTTP response. SDK-internal reads can also occur outside the wrapper. Use the actual POST result and cloud correlation to establish acceptance; these phase samples only locate memory pressure in time.

No TLS allocator, cipher, SDK, payload size, timeout, retry count, Wi-Fi reset rule, user interruption, OTA guard, schedule, NVS or persistence behavior is changed. Existing512-byte background TLS writes remain intact. The observed AES log has several SDK allocation sites; do not call it a16KB AES request without the new evidence.

## Validation plan

Host tests exercise actual pinned String allocation/release semantics, setup lifecycle guards and negative controls, plus bounded diagnostic overflow/contention/owner lifetime and concurrent callbacks. Run the full source and exact-build snapshot gates before installing a candidate. Then capture a real re-provision and immediate voice note, correlate request/transcript/list completion, and inspect memory milestones. A same-wake camera capture is the separate next check. Do not clear saved uploads or force a radio reset to manufacture a healthy measurement.

The working-source gate passed all107 suites at `regression001/RESULT.json` in the evidence workspace. Focused diagnostics passed38 checks, including60,000 concurrent failure-hook calls. The scan-release test passed71 checks against pinned Arduino String allocation semantics and rejected both old empty-assignment and omitted-cleanup negative controls. These host checks do not establish that the physical AES failure is fixed. Candidate build, installation and physical results will be recorded separately.

The first candidate build stopped on the lock-free static assertion before any installation. Actual S3 assembly showed that byte atomic_flag and fetch-add can retry, while a strong32-bit compare/exchange emits one S32C1I. The revised gate uses the latter, with simple scalar loads/stores. The diagnostic test now compiles the actual gate with the pinned S3 compiler and rejects calls or branches in gate/load/store/release assembly. It also checks that a previous loss keeps later reports incomplete. Preserve the failed build under `release202`; the revised candidate uses `release202r2`.

Reference: [Espressif5.5.4 AES DMA allocation implementation](https://github.com/espressif/esp-idf/blob/v5.5.4/components/mbedtls/port/aes/dma/esp_aes_dma_core.c). The installed Arduino3.3.8 source and build proofs remain authoritative for compiled configuration.
