# Check-in directly acknowledges capture

The requested flow is **Check-in → capture → Got it! → Home**. Sense submits
quantity one with no device-entered expiration; the quantity/confirmation page
and its 30-second input wait are no longer part of production check-in.

The existing LCD `SCAN/check-in/DONE` route already shows the success animation.
Sense emits that status only after `queue_upload_job` accepts the captured JPEG.
Camera, image-copy allocation and queue failures still take their error paths.
The acknowledgment means captured and queued, not that cloud delivery has
already completed. Existing background upload, durable retry and sleep paths
are unchanged. Discard retains its separate Add to list / Not now choice.

Check-in initializes literal quantity one and an empty expiration, without
exposing its active job to old quantity messages. Obsolete `INPUT_EXPIRY_DATE`
messages are ignored, so late input cannot change the current or next capture.
LCD routing for older Sense firmware is retained; an LCD update by itself does
not remove the old Sense's wait. This change requires the new Sense image.

`tools/test_scan_choice_ui.py` exercises the actual Sense setup, legacy input
handler, capture/copy/enqueue branch and LCD route. It covers consecutive
captures, stale input before/during/after capture, queue-before-DONE ordering,
camera init/capture/allocation/queue failure, late UI statuses, and retained
discard/old-Sense routing. The full regression gate and canonical production
build are required before a release. Host doubles do not establish physical
camera, radio or UI timing; retain actual device acceptance separately.

The disabled `HALO_DEMO_MODE=1` scenario still scripts the historical expiry
screen and would time out on its obsolete input. That demo is outside this
production change; the canonical shipping build has demo mode disabled.
