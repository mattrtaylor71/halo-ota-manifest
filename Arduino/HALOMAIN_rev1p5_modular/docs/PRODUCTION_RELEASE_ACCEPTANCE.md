# Production OTA acceptance — 6.4.112

**Release disposition: failed acceptance; not released.** The successor source is commit `b6ff853e4e3aa4f014a45a48230351bbed802d7c`, firmware tree `8529653e0c187f0b2c1c47689f03de2efe11d5ea`. The final release record must bind the actual paired artifacts, hardware outcomes, production publication and development-checkout adoption in `RELEASE_BASELINE.json`. A later documentation commit does not change compiled source identity.

## Corrections and evidence

The shipping 102→103 run failed during the LCD download and did not confirm a retry arm. Source review found two gates that rejected a healthy old LCD after an aborted inactive-image transfer: an increased begin count, and a requirement for a live coordinator lease that normal transfer handling clears or outlives. The first correction requires confirmed cleanup before a fresh peer query and accepts either the original live owner or an exactly empty owner with zero lease. Exact invocation-local firmware, boot, partition and SDK VALID checks remain required.

The subsequent 105→106 run confirmed that this arm step now works, but exposed the next failure. The LCD accepted 1,383,690 of 1,877,264 bytes before a 30-second HTTP stall; an immediate reconnect then failed before data. Historical CRC-valid Sense generation 6 records a confirmed retry arm for 13:20:17 Pacific, with peer boot 300637462. Historical LCD arm bytes independently match its ID, due time and saved time. Both boards woke, but generation 7 deferred at due+7 seconds without spending a second network reservation.

Retained debt can queue a boot check as `coord_recovery` before an early LCD timer notice arrives. The existing queue preserves its first reason and deadline. The readiness check incorrectly required that reason to equal `lcd_timer`, even when the accepted timer origin matched the current peer and retry arm. A native regression reproduces that rejection on the previous source. The successor removes only that queue-label requirement; the accepted timer origin, exact arm ID, current queried peer boot, TIMER wake, readiness and nonlegacy checks remain.

The exact rejected wire payload was not captured in the hardware run. The persisted sequence proves successful arming followed by missed admission; the identified source path is independently reproduced. The original network stall remains unexplained. Saved socket and memory snapshots do not prove router, TLS or heap failure, and source review found no specific transport defect to justify a speculative change.

## September 10 final scheduled run

The one-hour deadline was missed. Both boards woke automatically for the15:04:13 PDT calendar window. Sense later reported `lcd_proxy_failed_defer`, two LCD BEGINs, one network/application attempt and no Sense BEGIN. It armed a retry for15:18:42 while retaining1,813,863 ms of work allowance. Both boards woke at15:18:30.769. The saved readiness snapshot was `not_due` with `origin:false`; they returned to sleep by15:19:01 still reporting109 and unchanged attempt counters. The queue-label correction did not establish successful retry admission on this hardware run.

A later diagnostic TAP and serial open are separate from the autonomous case. The reader saw live OTA ownership and stopped before sending identity commands. Saved unsolicited SDK reports identify109/app0 VALID, but do not establish an idle UI or112 completion. The later saved canonical is DEFERRED generation8 with unchanged attempt counters. The wireless D3 export lacked the transfer failure record; the retained on-device ring subsequently recovered two concrete failures: HTTP no-data at835,080 bytes after400,060 ms (Wi-Fi connected, RSSI−74) and HTTP disconnect at304,405 bytes after178,154 ms (Wi-Fi connection lost). LCD records corroborate the byte counts. The cause of the first stream stall remains unknown; neither record establishes USB or heap as its cause. The closed case and exact raw evidence are bound in the baseline.

## Acceptance matrix

| Check | Actual result |
|---|---|
| Native transfer-cleanup and retry-arm regression | PASS: partial/late failure, live/cleared/expired lease, 25 identity/ownership negatives, cleanup before query, missing ACK and unchanged debit/caps. |
| Native early-wake regression | Old source FAIL; successor PASS. Five queue reasons, ten invalid-origin conditions, clock/storage/expiry/deadline boundaries, original-window waiting and actual canonical reservation/codec. |
| Exact production 112 artifacts | PASS: exact committed b6 source, canonical shipping profile, production endpoints/channel, image hashes/slots and unchanged frame/static/RTC sets. Actual112 pair and independent peer are bound in the baseline. |
| 109 controlled setup | PASS:109/app0 on both boards, SDK VALID and safe UI idle, followed by natural paired sleep. Eight exact writes and full physical readbacks; valid105 fallback and provisioning preserved. First setup attempt stopped before writes because the harness lacked105 metadata; its corrected rerun passed. |
| 109→112 scheduled production install | FAIL: first LCD proxy transfer failed. Automatic retry was armed for15:18:42 PDT, woke early and returned to sleep still on109 without a second network/apply reservation. No112 installation or paired SDK VALID proof. |
| Pacific schedule | Configuration/readback PASS: only timezone changed, canonical/provisioning/VALID images preserved. Saved Sense target1789117200 is September11 02:00Pacific; LCD target1789117185 is15s earlier. Both normally released. Initial identity qualifier stopped on firmware OTA_LOCK; later saved raw shows HOME and timer selection. No full qualifier PASS claimed. |
| Recovery package | PASS:36 files,115,802,823 bytes,37 checksum entries, both merged-image component layouts and erased factory NVS/app1 independently verified. ELF, map and provenance included. |
| Production promotion, local release tag and development checkout adoption | Not performed: failed hardware acceptance. Shared production latest remains6.4.14. |

The user explicitly limited release work to one hour on September 10. The release gate is the single clean scheduled install of the exact production pair plus packaging and service restoration. Additional deliberate integrity-fault testing, a second scheduled repeat, a separate final manual reinstall, physical power cuts, controlled network interruption, USB-free shipping operation and later-day recovery are deferred. They are not prerequisites introduced by this document and are not counted as passed.

Earlier 98→99, 99→100 and 100→101 scheduled passes were bench-profile evidence. Controlled shipping 105 setup passed. Shipping 102→103 and 105→106 failed; 104 and 108 were never production releases. Do not carry earlier passes onto new artifacts.

## Conditions and limits

The pair has an 8 MiB Sense and a physically 16 MiB LCD, both configured for 8 MiB images. Tests use Garage Member Wi-Fi and connected USB. Opening USB diagnostics can reset a board, so post-case query/capture records are separated from autonomous records. A normal Home screen that has timed out to consistent idle-dark state is recorded as idle, not as an actively rendered Home observation or an OTA failure.

Nearby scheduled tests move normal local 02:00 using an explicitly declared temporary POSIX timezone while preserving genuine UTC. Final service must restore `PST8PDT,M3.2.0,M11.1.0` and verify the next 02:00 Pacific timer. This is calendar-path testing, not an observed Pacific overnight run.

New-case NVS fixtures are applied only between closed archived cases. They preserve ownership, Wi-Fi, authentication, unrelated NVS and valid firmware. They are test setup, not an in-case refund or customer recovery procedure. Shipping keeps its 40-minute daily work allowance, 120-second preflight reservation, network/apply/begin caps and strictly later-UTC-day refill rule.

Private fault objects have bounded conditional cleanup; immutable releases and shared production downloads stay healthy. The 105→106 transfer did not reach its deliberately corrupted late byte, so integrity-fault coverage remains unmeasured. A real failed-transfer recovery can establish retry behavior without claiming checksum-fault coverage.

Physical power cuts, deliberate network loss, USB-free shipping operation and observed later-day recovery remain unmeasured unless added with actual evidence. Native checks do not replace those cases. Source-frame/static/RTC comparisons do not prove worst-case TLS/library/RTOS runtime stack margin. A USB reset is not an electrical cold boot, and offline factory packaging does not qualify a production flashing station.

## Next focused correction

[The next-fix note](OTA_RETRY_NEXT_FIX.md) proposes preserving the existing readiness window during the valid15-second early retry interval, even when the origin latch is absent. No reservation may occur before the exact due time. This is a proposed narrow correction with an explicit real-source regression recipe, not an implemented or tested fix.

## Closure

Promote production latest only after exact 112 acceptance, preserving previous pointers and complete served-byte readbacks. Finish the portable recovery package, accurate release records, local commit/tag and safe original-checkout adoption. Preserve unrelated user changes. Close temporary owners and awake assertions, restore Pacific scheduling and pause the old test automation. Artifact-source and final release-record commits remain distinct; no remote Git push of unrelated ancestry is included.

Actual deadline closure: production112 was not promoted, tagged or adopted. The portable candidate package is retained. Pacific configuration and saved next-timer selection were restored; the recurring soak automation is paused. Source and actual failure/service records are committed, with no further runtime changes during closure.

The failed campaign retains its original not-before/next-normal time, September11 15:04:13 Pacific. Restoring the daily2am timer does not reset that debt or promise another OTA during the first2am wake. All hardware/cloud owners and the finite host-awake assertion are closed.
