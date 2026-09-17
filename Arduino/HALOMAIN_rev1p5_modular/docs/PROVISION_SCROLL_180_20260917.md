# LCD180 scroll cue and Chocolate voice investigation

September 17, 2026. Source `f42dad1462122325d14ec40724be049f42d51cb2`.

## Installed change

Only the upper arrow on “Scroll for next step” was removed. The lower arrow and text remain at identical coordinates with identical pixels. The two changed files are the cue generator and generated alpha asset. Canonical180 builds and artifact checks passed; the LCD image is 2,464 bytes smaller than179 with unchanged static RAM/RTC use. Unrelated runtime suites were not rerun for this cosmetic change; previous179 results remain separate evidence.

The actual unit is **Sense179 app1 / LCD180 app0, both SDK VALID**. Sense180 was prepared but not installed. LCD179 app1 remains a valid fallback; Sense179 and its178 fallback were untouched. The application-only service verified the current bank, NVS and partition table were preserved. Public OTA remains162; no publication or firmware transfer qualification is claimed.

One initial calibrated actuator stroke was used when LCD USB was absent; no tap was sent to an already awake foreground screen. Service identity collection explicitly used the diagnostic awake lease and was not entirely passive. The first installer invocation lacked the Python serial dependency and stopped before device acquisition or writes. The corrected environment completed the second invocation successfully.

Post-install observation verified a nonce/length/CRC-protected LCD180 identity, exact Sense179 build/SDK state, unlocked Home, active LVGL, a real HTTP200 list refresh with eight items transferred to the LCD, and coordinated sleep with 29.9 seconds of passive quiet. The later liststate/home diagnostic batch arrived after automatic inactivity return/dimming; no liststate reply or item-specific display proof was obtained. Physical provisioning artwork was not photographed and Wi-Fi provisioning was not restarted for this cosmetic check.

## What happened to Chocolate

The exact recording succeeded. The cloud transcribed **“Add chocolate to the shopping list.”** and executed the real shopping-list addition. Independent reads confirmed both the canonical list row and shared row were ADDED at **10:33:43 a.m. Pacific**, September17. The cloud job completed once at10:33:44; the later device voice queue was empty. No duplicate addition, backend mutation or job replay was performed during investigation.

Current firmware holds fresh voice uploads until the session sleep flush. Startup OTA readiness can delay sleep; after an asynchronous voice upload is accepted, the device does not poll its completion or automatically refresh the displayed shopping list. These mechanisms can make a successful request appear delayed or absent until another refresh. The original utterance's release timestamp and displayed list were not captured, so an exact delay or specific original blocker is not proved. A retained voice-session timestamp can be reused and must not be treated as the recording timestamp.

This release does not change voice scheduling or list refresh behavior. No backend defect was established. Relevant current source: `Sense_Minimal.ino` session-hold/upload worker and sleep guard, `sense_voice.h` accepted-upload handling, `sense_sleep.h` flush, and the LCD list-entry refresh path. A future latency change needs a separate bounded test that records microphone release, upload admission, cloud completion and subsequent list contents in the same run.

## Retained evidence

- Build and source: `/Users/MattTaylor/halo-provision-cue180-20260917/candidate180-001/RELEASE-PAIR.json` (SHA256 `a46bf82a068373ba2debd08e9a425a43b8127a4cbf68cd97a5ec6b4d0e9d2453`). Its prepared-only fields are historical; actual installation is layered in the service/health receipts.
- Installation: `/Users/MattTaylor/halo-provision-cue180-20260917/INSTALLATION-REVIEW.json` (SHA256 `8b6b9831de90b10cc1eae4dfa16c8131bd83fa356f69fa846d839499a286458d`). Independent review passed78 checks and rehashed11 retained read files.
- Health/list/sleep: `/Users/MattTaylor/halo-provision-cue180-20260917/postinstall001/REVIEW.json` (SHA256 `c37290ed7c9cea95d92d75978a545ab2c17bb62e71af0ded500ac57cf7ec9137`).
- Exact voice/cloud investigation: `/Users/MattTaylor/halo-chocolate-20260917/VOICE-CLOUD-REVIEW.json` (SHA256 `44ea0b7d504aa69bcbcd18db9ccc535ab5602b659a8736c81e2def64d6a1d530`). Raw account/device evidence remains private outside Git.

Prior179 runtime and qualification limits remain in [RECOVERY_179_20260917.md](RECOVERY_179_20260917.md). No new full-product, phone provisioning, scheduled OTA, power-cut or USB-free qualification was performed. Continue from the180 source or reviewed descendants; do not select an older build because Sense remains179 on this unit.
