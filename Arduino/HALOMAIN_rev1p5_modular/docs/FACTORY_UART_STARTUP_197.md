# Factory UART startup correction — September 18, 2026

The EOL station reproduced missing Sense replies after a complete paired 6.4.196 factory flash. Diagnostic traffic and complete flash readback had passed. This differs from the configured bench's successful 196 OTA and is a factory acceptance failure.

## Cause and correction

Missing or unreadable `lcd_xfer/unsafe` correctly starts Sense in quarantine. However, the automatic control probe refused to run during provisioning, precisely the mode selected by blank NVS. Ordinary FW_INFO messages were suppressed, while their caller incorrectly logged them as sent. Probe expiry also incorrectly created LCD OTA debt from transport uncertainty.

Keep the fail-closed default. Allow the existing bounded query-only service during provisioning and require a fresh matching nonce, nonzero LCD boot identity, boot-ready/SDK VALID, matching running and selected partitions, explicit idle state and explicit empty owner/zero lease/no waiting state. Recheck local UART ownership under its lock and bind polling/cancellation to the original nonce. Image/voice/spool ownership contributes to LCD idle proof. Success sends fresh FW_INFO automatically; existing periodic setup guidance retries recover previous dropped messages. Both success and timeout leave existing OTA debt untouched: transport readiness does not prove transfer completion. The service retains its 120-second total budget with queries of at most three seconds; expiry leaves quarantine enabled.

The sender returns whether the complete local frame was written; FW_INFO/query logs now distinguish skipped sends. This return value does not prove peer receipt. Restore-origin diagnostics occur after Serial initialization. A failed safe-marker write causes a fresh probe next boot.

## Validation and remaining device work

Nine focused suites passed, including a composed regression with 472 checks and 22 unsafe responses. Its seeded old setup gate reproduces the original deadlock. Coverage includes missing/unreadable/saved-unsafe NVS, stale/invalid/missing ownership proof, interrupted binary owners, timeout, replacement-query ownership and storage-write failure. Preferences and serial I/O are host doubles; these results do not qualify physical factory startup.

The composed test simulates interrupted-transfer ownership flags and saved-unsafe restoration. The separately passing `test_lcd_ota_uart_recovery`, `test_lcd_reboot_cleanup` and `test_retry_peer_ready` suites exercise actual-source session cleanup, forgotten-session reboot proof, stale-session isolation and retained deadlines. These remain host checks, not new physical power-interruption tests.

Both the working-tree and immutable snapshot gates passed all 99 suites. Canonical Sense/LCD builds and paired artifact checks passed. Source is `7f87340d7c271bfdfc9b888aa6a0fcbf43822a8e`, build `6.4.197-20260918T231600Z-7f87340d7c27`. Version 197 remains unpublished; public latest is unchanged at 196. The EOL pair now runs 197/app0/SDK VALID. This build also includes the previously committed installation subtitle “Keep Halo powered on.”

The factory test must use both complete images with erased NVS, full flash readback, automatic startup FW_INFO before any special console request, then a fresh production request/response and Settings identity. Check sleep/wake and actual power removal separately. Preserve interrupted-update regression evidence; do not default the guard trusted or use a console recovery command to pass initial boot.

Target EOL pair: Sense `10:20:ba:03:97:b8`, LCD `d0:cf:13:1e:19:bc`. Original failure evidence remains in the Mac mini station's `factory-runs/49c855eb5b3b109143718433/manual-investigation/check-1789771182/`. Working evidence: `/Users/MattTaylor/halo-factory-startup-20260918/`.

## Closed factory installation and first-start evidence

The `factory001` run identified the exact EOL ROM MACs/security/flash sizes, backed up all 8 MiB of Sense and 16 MiB of LCD, wrote both complete canonical factory images and compared every flash byte successfully. Factory images contain erased NVS. Startup acceptance then failed with zero USB bytes on the retained loader handles and no console commands sent. Preserve this failure; do not label the original standalone run a pass.

The inherited reset sequence asserted DTR before esptool's RTS reset, and capture reused the loader handles. Reviewed adapter R2 deasserts DTR before reset, drains/closes those handles and opens fresh exact-device passive handles without PySerial's implicit input flush. Its 19 inert tests passed. This corrects test tooling, not firmware.

The separate `release002` recovery made **zero flash writes**. Before normal application release, it verified both actual NVS and both boot-selector ranges still matched the pristine factory image bytes, binding the original complete-readback receipt. It then captured this real startup sequence on Sense:

```
mode_restore origin=namespace_missing_or_unreadable quarantine=1 marker_pending=1
FW_INFO skipped (lcd_query=0)
control_probe_started quarantine=1
control_mode_confirmed quarantine=0
```

LCD received the actual automatic `FW_INFO` for 6.4.197 and the exact build before any console command. Passive acceptance completed about 2.94 seconds after capture began. Subsequent ordinary `fw`/`fwinfo` requests were received by Sense and answered at LCD; both running and selected partitions are app0, both SDK states VALID, LCD boot_ready=true. Initial Sense UNDEFINED was transient during the existing factory validation interval; final acceptance required VALID. Both hardware processes exited and their owned process groups were absent.

The original failing 196 Sense backup also passed a scoped NVS parse: `lcd_xfer/unsafe` was absent, while `halo/lcd_ota_due` was uint32 1. This corroborates absent guard state and stored debt; the backup alone cannot identify which historical code path wrote that debt. No provisioning values were exposed.

## Remaining qualification and distribution limits

Physical all-power-off recovery, visible Settings verification and configured sleep/wake remain pending. The passive post-install observer's USB attachment caused another USB_UART_CHIP_RESET; logs showed `origin=saved_safe quarantine=0` and a received 197/app0/VALID identity. This is USB-reset recovery evidence, not a physical power-cycle pass. The five-minute observer closed both ports cleanly with zero console commands; despite its directory name `cold-power003`, no physical power removal occurred in that capture.

No new OTA transfer, scheduled OTA, Wi-Fi or full-product pass is claimed. Existing production endpoints, partition tables, profiles and 02:00 local/Pacific-default scheduling are unchanged. Sense application grows 1,024 bytes and static RAM 40 bytes; LCD application grows 128 bytes with unchanged static RAM. Runtime heap was not newly characterized.

Exact candidate package on Mac mini: `/Users/mikehunt/halo-factory-startup-20260918/package/`. Reviewed programming adapter: `tooling-r2/` beside it. The live EOL station and original failed acceptance record were not rewritten; the EOL owner must adopt this candidate explicitly. Portable local evidence is in `/Users/MattTaylor/halo-factory-startup-20260918/remote-evidence/` and `CHECKPOINT.json`.

The independently reviewed evidence chain and scoped acceptance flags are recorded in `/Users/MattTaylor/halo-factory-startup-20260918/FACTORY-ACCEPTANCE-REVIEW.json`. `RELEASE_BASELINE.json.current_working_source` now selects the fixed197 source; the prior196 source, publication and configured-bench qualification remain separately retained. New firmware changes must start from197 or a reviewed descendant and allocate an unused198+ after inventory.
