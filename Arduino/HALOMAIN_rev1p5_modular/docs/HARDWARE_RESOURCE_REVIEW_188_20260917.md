# Hardware and resource review — September 17, 2026

Read-only review against published 188 and the KiCad task **Design ESP32 PCBA in KiCad** (`01a06eb0-de45-75f1-b7fd-98b7a5e63569`). The user prioritizes shipping with reliable everyday functionality and accepts replacement of units affected by rare OTA failures. This review changes no firmware, wiring, partitions or release acceptance. Its optimization candidates are not shipping blockers.

## Verified division of work

| Platform | Sense | LCD/UI |
| --- | --- | --- |
| Current devkit unit | ESP32-S3; prior service readbacks identify 8 MB physical flash. Camera, PDM microphone, Wi-Fi/TLS, provisioning and OTA orchestration. | ESP32-S3; prior service readbacks identify 16 MB physical flash. Display, touch/knob and durable typed image/voice SD storage. |
| New dual-S3 PCB, revision 0.12-SW | U300, WROOM-1U-N16R8: 16 MB flash / 8 MB octal PSRAM. Camera and PDM microphone connected here. | U200, same module/memory. Display, touch/knob and a 1-bit SDMMC socket connected here. |

The current production build uses octal PSRAM on both processors. Each S3 has 512 KB total internal SRAM before code, system and driver use; the chips have separate address spaces. Changing CPU affinity does not create another memory pool. Current-unit OTA validation is not a firmware-port or bring-up qualification for the custom PCB.

Hardware evidence: [current integration contract](/Users/MattTaylor/Dev/knob-sense-pcba/docs/engineering/firmware-contract.md), [native Toppop netlist](/Users/MattTaylor/Dev/knob-sense-pcba/checks/power-switch-display-20260914/toppop.xml), and [engineering BOM](/Users/MattTaylor/Dev/knob-sense-pcba/hardware/power-switch-display-20260914/toppop-engineering-bom.csv). The custom PCB's UI SD uses GPIO 43/44/6 in 1-bit mode; the existing devkit's 4-bit preset cannot be reused. The chosen Toppop/ST77916 display and startup/power pins also require the documented board-specific port.

The separate [P4+C5 proposal](/Users/MattTaylor/Dev/knob-sense-pcba/hardware/variants/p4/README.md) remains an architecture study: P4 owns the product application and TLS; C5 is its radio companion. It is not the hardware running this firmware and is not a quick shipping fix.

## What is actually constrained

Camera framebuffers already use PSRAM (`Sense_Minimal/sense_camera.h`). The camera reserve is a 16 KiB contiguous **internal DMA** allocation (`Sense_Minimal.ino`). Spare megabytes of PSRAM do not by themselves meet that requirement. During this manual 188 attempt, before flash writes, Sense reported 38,720 internal free bytes with a 17,396-byte largest block; LCD's handoff reported 30,860 free with a 16,372-byte largest block. These are phase-specific observations, not a complete memory profile or worst-case margin.

LCD HTTPS was deliberately removed from the production wrapper after its own memory failures; Sense downloads and proxies its update. Preserve that split. UART is 115200 baud: 8-N-1 has an ideal 11,520 bytes/second ceiling before protocol overhead. Moving a 100,000-byte photo across it adds at least 8.68 seconds. Moving raw 16 kHz / 16-bit mono audio at 32,000 bytes/second is not viable through that link without another representation or buffering strategy.

Espressif documents external-memory DMA restrictions, internal descriptor requirements and cache restrictions during flash writes. Driver-supported camera PSRAM-DMA deserves a separate measured experiment; it is not established by adding a macro to an application using precompiled libraries. References: [S3 datasheet](https://documentation.espressif.com/esp32_s3_datasheet_en.pdf), [IDF 5.5.4 external RAM](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/api-guides/external-ram.html), [camera driver](https://github.com/espressif/esp32-camera).

## Corrections to the older hardware review

The [September 8 memory review](/Users/MattTaylor/Dev/knob-sense-pcba/docs/engineering/firmware-memory-pcba-review-20260908.md) remains useful, but its SD-disabled/omitted conclusion is historical. Current `lcd_voice_spool.h` and `lcd_image_spool.h` enable separate typed media stores; the older generic photo spool remains disabled. The new PCB also includes UI-side SD.

Its Sense partition warning remains current: the production table ends at 0x400000, using 4 MB of the 8 MB physical flash, with 192 KiB SPIFFS. LCD's production layout uses 8 MB of its 16 MB flash. Unallocated flash can potentially expand durable storage after a deliberate migration; it cannot fix internal RAM pressure. Do not change deployed partition tables as an ordinary cosmetic or OTA update.

## Recommendation for shipping

Keep the existing board roles and 188 runtime. Focus finite acceptance on wake, check-in/dish/discard, voice reaching the intended list, shopping-list interactions, saved-media recovery, and return to sleep. Preserve already collected evidence and distinguish regressions from untested extensions. Do not gate this release on a hardware redesign or another exhaustive rare-failure campaign.

For subsequent optimization, first measure internal largest-free-block and stack high-water across real activity transitions. A concrete candidate is `UploadJob` in `sense_ops.h`: it stores both voice and image identity envelopes, while the ten-entry queue copies the whole structure. A tagged union could save roughly 2 KiB of queue payload. That estimate is not a compiled saving or an implemented fix; all envelope readers, fresh/parked/replayed jobs and persisted receipt identities need checking before changing it. Retain all IDs and durable custody semantics.

Keep UI formatting/cache and SD storage on LCD, and camera/audio/network ownership on Sense. Consider moving proven CPU-only buffers into PSRAM only after auditing their flash/cache lifetime. Faster UART, camera PSRAM-DMA and partition expansion are separate experiments, not prerequisites for this shipment.
