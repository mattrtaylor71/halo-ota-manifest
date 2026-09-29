# Kitchen Assistant production firmware

Public paired OTA is **6.4.233**. Start with [the release handoff](docs/RELEASE_233.md) and [PRODUCTION_BASELINE.json](PRODUCTION_BASELINE.json). Continue from exact compiled source `730bdd4cedbfe27e1708dc9e986900c8349024f2` or reviewed descendants on `codex/ram-qualification`, retaining the accumulated RAM, refresh, haptics-off, display, telemetry, voice/list and battery-presentation fixes.

Use this canonical checkout, then follow [build and release](docs/BUILD_AND_RELEASE.md). Different firmware bytes need a freshly inventoried unused **234+** identity, committed source, the complete snapshot gate, paired canonical builds and relevant finite device checks. Building or flashing a candidate does not publish it; future public releases need explicit approval.

All130 exact-snapshot suites, paired artifact checks and full public binary readbacks passed. Both bench boards are233/app1/boot app1/SDK VALID with232/app0 and data preserved. Finite Home/List/Home, voltage/UI-resource samples,10.004-second panel-off, next02:00 Pacific timer arming and paired sleep/75s quiet passed. No new233 OTA transfer or scheduled execution was tested. Battery percentage remains a provisional system-rail estimate; see the release handoff for physical and resource qualification limits.

EOL factory selection stays197; frozen158 recovery and earlier private packages remain unchanged. [RELEASE_BASELINE.json](RELEASE_BASELINE.json) preserves their evidence. Historical entries named “current” do not override the compact production selector.
