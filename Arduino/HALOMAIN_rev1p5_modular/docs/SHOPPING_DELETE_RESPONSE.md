# Immediate shopping-list delete feedback

The September 18 manual capture measured 1.144 seconds from the Delete tap to the matching backend result being applied. The prior UI then used a 150 ms slide/fade followed by a 120 ms collapse. Evidence is retained in `/Users/MattTaylor/halo-voice-list189-20260917/manual-user190-prefix001` and the original `manual-user190-001` capture.

The LCD now removes the row from its presentation as soon as the existing UART queue accepts the delete. The active data and persistent cache still retain the item until a matching positive backend result. A rejected enqueue does not hide anything. Failure, link failure or the existing 60-second result timeout restores the row and shows the existing error message. Successful confirmation persists removal once, with no second exit animation.

Pending deletion is projected by item ID on every rebuild, so a stale refresh cannot briefly restore it or hide the wrong row after reordering. Rendered slots retain source indices; touch and encoder input still address the correct remaining items. Store headings and the displayed count reflect the visible rows. The existing single-pending-delete limit remains unchanged. No backend, Sense, upload, Wi-Fi, OTA or sleep code changes.

This is optimistic visual feedback, not offline delete persistence: a reset can restore an unconfirmed cached item until the next successful refresh. Exact firmware build and device acceptance must be recorded separately. The installed Sense190/LCD188 pair has not been changed by this source edit.
