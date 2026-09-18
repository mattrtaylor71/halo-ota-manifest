# Factory UART startup correction — September 18, 2026

The EOL station reproduced missing Sense replies after a complete paired 6.4.196 factory flash. Diagnostic traffic and complete flash readback had passed. This differs from the configured bench's successful 196 OTA and is a factory acceptance failure.

## Cause and correction

Missing or unreadable `lcd_xfer/unsafe` correctly starts Sense in quarantine. However, the automatic control probe refused to run during provisioning, precisely the mode selected by blank NVS. Ordinary FW_INFO messages were suppressed, while their caller incorrectly logged them as sent. Probe expiry also incorrectly created LCD OTA debt from transport uncertainty.

Keep the fail-closed default. Allow the existing bounded query-only service during provisioning and require a fresh matching nonce, nonzero LCD boot identity, boot-ready/SDK VALID, matching running and selected partitions, explicit idle state and explicit empty owner/zero lease/no waiting state. Recheck local UART ownership under its lock and bind polling/cancellation to the original nonce. Image/voice/spool ownership contributes to LCD idle proof. Success sends fresh FW_INFO automatically; existing periodic setup guidance retries recover previous dropped messages. Timeouts preserve existing obligations without creating new debt.

The sender returns whether the complete local frame was written; FW_INFO/query logs now distinguish skipped sends. This return value does not prove peer receipt. Restore-origin diagnostics occur after Serial initialization. A failed safe-marker write causes a fresh probe next boot.

## Validation and remaining device work

Nine focused suites passed, including a composed regression with 472 checks and 22 unsafe responses. Its seeded old setup gate reproduces the original deadlock. Coverage includes missing/unreadable/saved-unsafe NVS, stale/invalid/missing ownership proof, interrupted binary owners, timeout, replacement-query ownership and storage-write failure. Preferences and serial I/O are host doubles; these results do not qualify physical factory startup.

Full working-tree and immutable snapshot gates, canonical paired build and factory acceptance are pending at this source checkpoint. Candidate version 197 was absent in fresh authenticated S3 listings; public latest remains 196. No factory flash or publication has occurred for this fix yet.

The factory test must use both complete images with erased NVS, full flash readback, automatic startup FW_INFO before any special console request, then a fresh production request/response and Settings identity. Check sleep/wake and actual power removal separately. Preserve interrupted-update regression evidence; do not default the guard trusted or use a console recovery command to pass initial boot.

Target EOL pair: Sense `10:20:ba:03:97:b8`, LCD `d0:cf:13:1e:19:bc`. Original failure evidence remains in the Mac mini station's `factory-runs/49c855eb5b3b109143718433/manual-investigation/check-1789771182/`. Working evidence: `/Users/MattTaylor/halo-factory-startup-20260918/`.
