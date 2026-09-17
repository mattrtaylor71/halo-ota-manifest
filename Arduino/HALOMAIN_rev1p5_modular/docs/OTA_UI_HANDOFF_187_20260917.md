# OTA installation screen handoff (187 candidate)

The user's physical manual185-to186 update completed both images and both SDK-valid boots. The LCD nevertheless displayed a frozen “Checking for updates” spinner through its own transfer. The recorded pass for transport must not be described as a UI pass; see [the186 test record](MANUAL_OTA_186_20260917.md).

The receive flag intentionally stops both LVGL render owners before flash erase/write. Previously it became active before the UI could replace its checking screen. Removing that protection would risk flash/cache activity overlapping display DMA.

The requested correction is limited to the LCD presentation handoff. The existing UI owner draws a static “Something new is coming” / “Installing update” screen before receive inhibition. There is no frozen spinner or misleading frozen percentage. A session-specific acknowledgment and the existing real-DMA drain share the original bounded handoff and OTA attempt deadline; refusal must happen before diagnostics, progress persistence or image erase. The UI owner retains its mutex between acknowledgment and UART inhibition, so another render cannot replace the accepted frame.

Sense, transfer transport, native daily allowance, retries, the02:00 Pacific schedule, provisioning, shopping and media behavior remain outside this change. No extra preview command or synthetic BEGIN is used to manufacture hardware acceptance. A host rendering of the actual LVGL UI proves appearance; a USB installation/boot smoke test is distinct from a subsequent real OTA running the new receiver. Actual artifacts and acceptance are recorded only after completion.
