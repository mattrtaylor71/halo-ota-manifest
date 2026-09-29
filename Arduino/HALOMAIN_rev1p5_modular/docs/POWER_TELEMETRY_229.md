# Private229 supply telemetry correction

September28,2026. Draft build/test record; no public release. Public firmware
remains6.4.224. Continue from the private228 descendant on `codex/ram-qualification`,
retaining the RAM, shopping refresh, haptics-off and10-second display fixes.

The228 health test found two isolated telemetry problems. An ordinary PONG-proven
awake peer could have `link_synced=false`, so the additional link-sync condition
prevented the optional power frame from being sent. The complete USB diagnostic
JSON also exceeded the existing bounded256-byte CDC output buffer and was cut
short. Cloud readback confirmed the missing peer snapshot rather than merely a
missing USB receipt. See [preserved228 results](POWER_TELEMETRY_228.md).

229 changes only the optional power-send admission and compact cached USB readout.
A current awake flag plus an accepted PONG/SYNC_ACK within two seconds suffices;
all existing transfer, sleep, OTA and queued-user-work exclusions remain. It does
not issue another ping, wake a peer, renew activity or change any user-action
handshake. The compact diagnostic has a distinct schema/prefix and fits below
the current USB buffer size. ADC acquisition, system-rail interpretation, report
payloads, cloud routing, schedules, upload storage and retry policies are unchanged.

Build and finite hardware results are pending. Required acceptance is two ordinary
wake/Home/sleep cycles with complete local voltage and native Sense receipts,
ordinary report correlation, then one camera and one voice/list transport case.
No voltage percentage, electrical calibration accuracy, charger/source inference,
OTA transfer or full provisioning claim follows from these tests.
