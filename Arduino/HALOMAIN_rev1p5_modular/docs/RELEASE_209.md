# HALO 6.4.209 version-only release

**Published and installed on both boards through the user’s manual OTA.** The user requested a newer version for ordinary manual OTA. Source `cd2b84bc072a8f287c4f93bed99df091e04610ba`, firmware tree `607b49e924d036923c4f90a911416918efc04cca`, build `6.4.209-20260922T175500Z-cd2b84bc072a`; local annotated tag `halo-v6.4.209` (object `91133914f364a3d70c4485b8567729f3b3e30a12`).

Compared with the exact 208 snapshot, only three generated version/build headers and eight documentation/selector files differ. All 1,429 other files and runtime code within those headers are identical. The exact 114-suite snapshot gate, both canonical builds and camera-aware artifact checker passed. Both 208 latest manifests were conditionally advanced to 209; complete public binary readbacks match:

| Image | Bytes | SHA256 |
| --- | ---: | --- |
| Sense | 1,874,928 | `6dfc8840e25e1c3466eb1ec999a813f2b7707c03df5b458585dd282e6e65f9ca` |
| LCD | 2,032,896 | `3bf8881e2db47c3f9a5c53296ba6bb4ff707744608675d87b0b0c87150c2ef72` |

The user’s second manual attempt updated both boards from 208: Sense 209/app1 and LCD 209/app0, full image hashes matching the table above, SDK VALID, native `policy_target_valid resolution=observed_pair`, then a fresh paired identity, Home and deep sleep. The first attempt failed during network requests at weaker observed signal (about −85 dBm versus −63 dBm on the successful retry); it also exposed the LCD preflight lease expiring before blocking manifest retries returned. That UI feedback defect remains open.

The next nightly schedule was stored and verified for **September 23, 2026 at 02:00 PDT** (`1790154000`). LCD preparation wake is 01:59:45 PDT (`1790153985`); final sleep timers were 51,837 seconds for LCD and 51,852 seconds for Sense. This proves persistence and actual timer arming, not that the next scheduled execution has occurred. Production scheduling was not changed.

Actual acceptance is `/Users/MattTaylor/halo-release209-20260922/DEVICE-ACCEPTANCE-209.json`, SHA256 `b2b754396bd965a77fa584119f62108f8c523436400dacacd0efe170d822267f`; it binds both the successful capture and the earlier failure. This is one successful paired manual retry, not repeated reliability, a separate 209 no-update test, physical scheduled execution or full-product qualification. Prior 208 acceptance remains separately retained in [the 208 handoff](RELEASE_208.md). Factory 197 and frozen 158 are unchanged.

Exact source/proofs/tests, publication, public readback and scope review are retained under `/Users/MattTaylor/halo-release209-20260922`. No remote Git push was performed. Future releases must use source cd2b84b or reviewed descendants and a freshly inventoried unused 210+ version.

The private recovery/debug package is `/Users/MattTaylor/halo-releases/6.4.209.tar.gz`; its archive record verifies all 1,754 members and the exact source history. `RELEASE-RECORD.json` and the immutable archive retain their original publication checkpoint with device testing then pending. The separate `DEVICE-ACCEPTANCE-209.json` now records the later successful installation; those historical artifacts were not rewritten. Raw device logs and media are excluded.
