# Private168: bounded final sleep UART traffic

The167 offline voice/Dish/Discard burst retained and uploaded all three captures, but its LCD received a truncated SLEEP_READY after persisting MAINT_WINDOW. It eventually used the existing sleep fallback. Five other observed episodes slept normally. See [the167 record](OFFLINE_MEDIA_STRESS_167.md) for the exact custody evidence and remaining limits.

The candidate removes the optional WIFI_DIAG_SUMMARY transmission immediately before SLEEP_READY, preserving local Wi-Fi duration finalization. The LCD may pause UART service during maintenance-window NVS persistence; the old roughly250-byte summary plus the ready frame exceeds its128-byte hardware FIFO. The exact electrical loss mechanism is not proven. Reducing the final tail to one small ready frame addresses that exposure without adding receive dispatch or a wait after the upload worker has been suspended.

The LCD will no longer receive this per-sleep legacy Wi-Fi summary. Existing connection/error/action telemetry remains. Do not move the summary immediately before MAINT_WINDOW: the summary handler also writes NVS. Final timer selection, OTA policy, worker teardown, retry deadlines and user-input boundaries are unchanged. The bound applies to ordinary drained sleep, not guardian-forced teardown with pending work.

This supersedes the earlier proposed acknowledgment-barrier implementation. Validate the actual sender serialization and final source block, a modeled FIFO stall, existing sleep/user-input/media regressions, and canonical production builds. Then repeat two finite offline burst/replay cycles on the device and inspect both normal sleep handshakes and exact media custody. A modeled pass is not electrical proof or full-product acceptance.

At source preparation: disk space recovered to36GiB; runtime change is implemented, build/device validation is pending. Both installed boards remain167 SDK VALID. Preserve the exact167 package and all failed165/166 evidence. Use unused6.4.168. No publication has occurred.
