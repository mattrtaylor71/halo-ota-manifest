# Kitchen assistant provisioning: iOS handoff

Prepared September 17, 2026. This documents the installed **6.4.177** device behavior and the companion app work needed to make setup clear and responsive. “Halo” remains the internal device/protocol name; use **kitchen assistant** in customer-facing app copy.

Firmware source: `ebb006726a81b947f13fc2dc500c3de9ffd66444`; build: `6.4.177-20260917T063649Z-ebb006726a81`. Checkout: `/Users/MattTaylor/halo-camera-recovery-2026-09-15`, production root: `Arduino/HALOMAIN_rev1p5_modular`.

The user reported that the latest setup “worked really well.” This is user feedback, not a new instrumented run. The earlier measured 176 setup connected to home Wi-Fi on its **first attempt in 1.135 seconds**, then spent **17.367 seconds** completing cloud registration after two transport failures. Do not interpret all time on the connecting screen as Wi-Fi association time. Those two transport failures do not have a proven underlying cause.

## What the user sees on the device

For an already configured device: **main menu → bottom three-dot button → Settings → Change Wi-Fi → START**. The confirmation reads “Forget this network and reconnect with your phone?” and offers **CANCEL / START**. Cancel returns to Settings. Start requests a fresh setup session. A device entering setup automatically can open the guide without that change-network confirmation.

| Stage | Exact device content and behavior | What the app should do |
| --- | --- | --- |
| 1 of 6 | “Get the **Trepo app**”; QR opens `https://hellotrepo.com/app-signup`. Right-edge “Scroll for next step” cue. | This QR is the app download/signup link, not the pairing QR. Have the user sign in before pairing. |
| 2 of 6 | Yellow **On Trepo App** pill; **Profile → Setup kitchen assistant**. Content stays centered independently of the side arrow. | Make this route easy to find and match these labels. The inspected app still uses **Profile → Halo Device → Pair Halo Device**. |
| 3 of 6 | **Scan to connect**, with the current device's Wi-Fi QR. | Scan in the app, join the temporary device network, then contact its local API. |
| 4 of 6 | **On Trepo App** pill; phone illustration; **Select home Wi-Fi**. | Show nearby networks, manual SSID entry, refresh, and password entry. |
| 5 of 6, Wi-Fi phase | Animated arc; **Connecting to home Wi-Fi...** | Show a live connection stage, retain entered details, and poll device status. |
| 5 of 6, account phase | Same animated arc; **Finishing setup...** | Say that Wi-Fi is connected and account linking is underway. Keep the UI responsive during cloud retries. |
| Completion | Green background, yellow **SETUP COMPLETE** badge and check seal; **Welcome to your assistant**. A 360 ms eased entrance fades and settles the content. | Show app success after verified completion; offer the brief usage tutorial below. |
| Return | Device returns to its existing main menu approximately **4.5 seconds after entering completion**; this includes the entrance animation. | The app can stay on success/tutorial. Do not wait for another device tap to finish. |

Steps 1–3 are navigated by turning/scrolling, with bounds at 1 and 3. There is no Next button. Backward scrolling is allowed until the app is detected. The setup service is already available during the guide: an app API request advances the LCD to step 4 even if the user is viewing step 1 or 2. After that, real device state drives progress and scrolling cannot move it backward. Repeated QR/status messages must not restart the guide or completion timer.

App detection is **not** just scanning the QR or accepting the system Wi-Fi join prompt. It comes from app-identified requests to the device's HTTP API. The app must make those requests promptly after joining.

Device failure screens show **Couldn't connect**, explanatory copy, **BACK / TRY AGAIN**. BACK returns to the beginning of the guide; TRY AGAIN starts a fresh setup session. The device also has **No setup code yet** if its provisioning QR cannot be obtained/rendered. A requested QR has a 12-second wait limit. These are distinct from an expired cloud account-linking code.

## End-to-end responsibilities

Sense owns the temporary Wi-Fi access point, local HTTP API, home Wi-Fi connection and cloud account claim. LCD renders device state received from Sense. The iOS app talks to Sense over local HTTP; it does not control LCD page numbers or send the internal UART status messages.

```mermaid
sequenceDiagram
    actor User
    participant App as Trepo iOS app
    participant Cloud as Trepo cloud
    participant Sense as Sense / setup API
    participant LCD as Device display
    App->>Cloud: Obtain signed-in user's setup code while internet is available
    Cloud-->>App: code + expires_at
    User->>App: Scan current device Wi-Fi QR
    App->>Sense: Join temporary AP; GET /info with app headers
    Sense-->>LCD: App detected → Select home Wi-Fi
    App->>Sense: GET /scan; poll while 202
    Sense-->>App: Available networks
    User->>App: Choose network and enter password
    App->>Sense: POST /wifi {ssid, password, owner_code}
    Sense-->>App: 200 accepted / connecting
    Sense-->>LCD: Connecting to home Wi-Fi
    loop Status polling, one request at a time
        App->>Sense: GET /status
        Sense-->>App: Wi-Fi state, attempts, owner presence, last error
    end
    Sense->>Cloud: Claim setup code after joining home Wi-Fi
    Sense-->>LCD: Finishing setup
    Cloud-->>Sense: Owner information on successful claim
    Sense-->>App: /status: connected + owner_id_set
    Sense-->>LCD: Setup complete → main menu after 4.5 s
    App->>App: Success, release temporary AP configuration, optional tutorial
```

The temporary AP can lose reachability during Wi-Fi changes, and it is deliberately removed after setup. A disconnected HTTP socket by itself proves neither failure nor success. Preserve the current session and reconcile its status before restarting it.

## Current firmware API contract

### Join and identify

The two QR codes are different. Step 1 is the signup URL. Step 3 encodes:

```text
WIFI:T:WPA;S:<current-device-AP-SSID>;P:<current-device-AP-password>;;
```

The pairing QR contains no device ID, HTTP address, owner code or app-session token. Each newly created setup session generates a `Trepo-Halo-XXXX-YYYY` SSID and a 12-character, case-sensitive alphanumeric password. Do not reuse an earlier QR's credentials after a setup restart. Read stable identity from `/info`; match that `device_id` on later responses.

Use **`http://192.168.4.1`**, with these headers on local requests:

```http
X-Halo-Provisioning-Client: halo_app
X-Halo-App-Version: <actual app version/build>
Content-Type: application/json
```

`Content-Type` applies to JSON POST bodies. Any nonempty client header identifies an app session; `halo_app` is the current iOS value. Requests renew a 45-second app-session hint that drives device UI/captive-portal behavior. It is not authentication. Use the numeric address because native-app mode suppresses wildcard DNS servicing. The AP shares a radio with the home connection; do not assume its channel or reachability stays constant during the transition.

The existing iOS local client disables cellular and uses 8-second request/15-second resource timeouts. Preserve local routing and distinguish it from the authenticated internet request used to obtain a setup code.

### Requests and responses

| Endpoint | Contract |
| --- | --- |
| Cloud `POST /v1/provisioning/code` | Use the app's existing authenticated service while internet is available. Retain both `code` and `expires_at`. This is not an endpoint on `192.168.4.1`. |
| Local `GET /info` | HTTP 200: `status`, `api_version:1`, `transport:"softap_http"`, `owner_flow:"owner_code_claim"`, `device_id`, `ap_ssid`, `ap_password`, `ap_ip`, `state`, `app_session_active`, `last_client`. Verify the expected protocol/device before progressing. |
| Local `GET /scan` | HTTP 200: `{api_version:1,status:"ok",networks:[{ssid,rssi,secured}]}`. Results are strongest-first and are not deduplicated. HTTP 202 with `status:"scanning"` means poll again. During a claim, a cached 200 or 202 `status:"busy"` may be returned with `Retry-After: 2`. A start failure can return 500 `error:"scan_failed"`. |
| Local `GET /scan?force=1` | Explicit refresh. Requires the app-session hint, has a 5-second cooldown, and should be followed by ordinary scan polling. Cached lists can remain visible while AP clients are connected; do not promise freshness without a completed refresh. |
| Local `POST /wifi` | Send `{ssid,password,owner_code}`. HTTP 200 means **accepted/connecting**, not setup complete. Response includes `api_version:1`, `status:"connecting"`, `wifi_state:"connecting"`, `owner_flow:"owner_code_claim"`, `connect_timeout_ms:45000`, `connect_max:2`. |
| Local `GET /status` | Authoritative local observation of network/owner state; schema below. Poll sequentially, approximately once per second after the preceding request completes, with bounded backoff for transport failures. This polling cadence is an app recommendation. |
| Local `POST /user-id` | Legacy compatibility route: `{owner_id}` writes local owner state directly and can return `{api_version:1,status:"success",owner_id}`. It is not proof of a cloud claim. It should not silently replace `owner_code_claim` merely because linking takes longer. |

`POST /wifi` requires a JSON body of at most 512 bytes, SSID length 1–32 bytes, a present password field of 0–63 bytes, and a nonempty owner code of at most 31 raw bytes. Empty password supports an open network. Firmware removes non-alphanumeric characters from the code and uppercases it; do not invent a six-character firmware requirement. `ownerCode` is accepted as an alias. An `owner_id` hint in this POST is ignored while the setup code is primary. Invalid bodies/fields return HTTP 400 with `error:"invalid_payload"` and a `reason`.

While a claim is busy, **both POST routes** return HTTP 409 plus `Retry-After: 2`:

```json
{"error":"claim_busy","retryable":true,"retry_after_ms":2000}
```

The existing operation continues. This is not a bad password or an instruction to restart. Decode the body, respect the retry hint and continue observing status.

**Accepted `/wifi` requests are not idempotent.** Repeating the same payload clears owner state, resets connection/claim counters and starts again. Disable duplicate Connect taps. If a POST response is lost, query status before deciding whether to submit again. There is no submission ID in the protocol; do not claim exact request correlation that the device cannot provide.

### Status and completion

`GET /status` returns these fields:

```text
status, api_version, transport, owner_flow, device_id,
state, provisioned, connect_attempt, connect_max, connect_timeout_ms,
wifi_state, home_ssid, last_error,
app_session_active, last_client, last_app_version,
owner_id, owner_id_set
```

| Observation | Interpretation and app action |
| --- | --- |
| `state` | One of `unprovisioned`, `ap_setup`, `connecting`, `connected`, `error`. |
| `wifi_state:"ap_setup"` | Setup network active; credentials have not yet produced a connection. |
| `wifi_state:"connecting"` | Connecting/retrying home Wi-Fi. `connect_attempt` is **zero-based**: 0 means first attempt, 1 means second. Display `+1` only in this phase. The counter resets to 0 on stable success and terminal Wi-Fi failure. |
| `wifi_state:"connected"` | Raw Wi-Fi link is up. This can precede the 2-second stability verification; it does not mean account linking is complete. |
| `state:"connected"`, owner not set | Verified home connection; account linking is still unresolved. Show **Finishing setup**. |
| `provisioned:true` alone | Wi-Fi has been verified. This is set before account claim completion and is not sufficient for success. |
| `wifi_state:"failed"` | Device reports a connection failure. Use `last_error` to guide recovery. |
| Nonempty `last_error` | Can describe a transient claim failure while retries continue. It is not, by itself, proof that the session is terminal. |

For the normal code-claim flow, latch local success only for the expected `device_id` and selected `home_ssid`, with `state == "connected"`, `wifi_state == "connected"`, `owner_id_set == true` and a nonempty `owner_id`. Compare owner identity with the signed-in account where the account identifier format is established; a mismatch must not be presented as success. If stronger cloud-association verification is required, use an appropriate authenticated backend check after internet returns. Do not treat local owner presence as cloud attestation: the compatibility endpoint can write it directly.

The HTTP API exposes **no** `claim_state`, claim attempt count, structured claim error code, exhausted flag or session/submission ID. Keep account-linking progress indeterminate. The internal Sense→LCD presentation values `app_connected`, `claiming` and `failed` are not additional HTTP fields to wait for.

### Timing and recovery boundaries

| Firmware behavior | Implication for the app |
| --- | --- |
| Home Wi-Fi: 45 seconds per attempt, at most 2 attempts; 2-second stable verification; 5-second tolerance for brief drops after a link was first observed. | A fixed 90-second total setup deadline leaves no allowance for verification or cloud linking. Some failures finish earlier. Use the response's budgets rather than a fake countdown. |
| No matching SSID can fail early; a connect-failed status is given at least 8 seconds. Retry can briefly block the device owner loop for up to roughly 5 seconds while restarting STA. | One delayed HTTP response is not proof that the device froze. Keep local request/recovery bounds finite. |
| Cloud claim: at most 4 attempts, nominal 2/5/10-second spacing measured from the previous attempt's **start**. It waits for pending time synchronization to finish or expire. | A long failed attempt may be followed immediately by another; do not model each retry as a mandatory additional pause. |
| Claim transport: 5-second TCP, 8-second TLS handshake, 3-second read limits and a 20-second logical attempt-age limit. Synchronous SDK calls mean this is not a guaranteed 20-second wall-clock cap. | Avoid a countdown promising an exact completion time. The async worker keeps local HTTP/UART service available during claim work. |
| AP normally closes 15 seconds after owner presence is observed, or 150 seconds after the initial home-link observation without an owner, subject to active claim draining/minimum connected delay. | Poll promptly to catch success. After losing the AP, retain the outcome already observed; otherwise present completion as uncertain and reconcile. Do not promise indefinite local status access or guaranteed extra claim attempts after AP shutdown. |
| LCD connection/account progress has a 240-second limit. Failure clears that progress hold. Success lasts 4.5 seconds. | A proposed app post-submit session budget is 240 seconds, with shorter per-request/stage bounds; validate this on the real flow. This is a finite UX/recovery deadline, not a promise that firmware always completes within it. |
| Scan has a 15-second device deadline, a 60-second cache and a 5-second forced-refresh cooldown. | Show pending versus empty distinctly, avoid repeated forced scans, and allow manual entry. |

Claim error text can include **Device already linked**, **Setup code not recognized**, **Setup code expired**, **Setup code already used**, **Rate limited, try again**, or an HTTP failure. An expired/used code needs an account-code recovery flow; a transient cloud transport error should not send the user back to retype a known-working Wi-Fi password. The second failed Wi-Fi attempt keeps the setup AP/API available for corrected credentials.

## App experience and implementation priorities

These are **recommendations for the iOS agent**, not claims that the app already implements them.

1. **Separate the stages.** Use “Connecting to your kitchen assistant,” “Choose your home Wi-Fi,” “Connecting to home Wi-Fi,” and “Finishing setup.” Once Wi-Fi is connected but the owner is not set, stop labeling the entire operation as Wi-Fi connection. A network attempt count is not a percentage of overall setup.
2. **Keep one cancellable session.** One owner should manage QR scanning, AP join, reachability, scans, credential submission and status polling. Fence asynchronous callbacks with a session/generation identifier. Cancel obsolete work on retry, start-over, dismissal and explicit Cancel; a delayed callback must not overwrite the new session's screen. Handle returning from Settings/backgrounding by resuming/reconciling the session. UI animations and controls must not depend on a networking task making progress.
3. **Use bounded recovery with useful copy.** Allow temporary local HTTP failures during AP/STA transition; probe the same device and resume status polling. Do not immediately discard the chosen network/password, mint another device setup session or show a generic terminal failure. At the overall deadline, distinguish “could not confirm completion” from a device-reported failure and offer an explicit recovery action.
4. **Match the actual route and visual language.** Put **Setup kitchen assistant** where the device says it is, under **Profile**. Use the device's cream background (`#F5E9D8`), dark ink (`#1A1A1A`), yellow contextual pills (`#FFD54F`), green success (`#1F4D2B`) and teal guidance (`#296065`). Rounded white cards, clear hierarchy and small crisp shadows should carry across. Keep progress motion simple; honor iOS reduced-motion settings. Avoid fake numeric progress.
5. **Make entry and recovery comfortable.** Handle camera/local-network permission failures specifically. Keep a manual AP join path. Use a secure password field with an explicit reveal control. Support networks that report `secured=false` using an empty password. Preserve SSIDs/passwords exactly; validate byte limits without silently trimming a password. Refresh the cloud setup code before it expires while internet is available.
6. **Finish clearly.** After a confirmed result, keep success stable even if the AP subsequently disappears. Offer a skippable/replayable tutorial for check-in, dish, discard and **More → Shopping list**. Reuse the existing device menu artwork: white five-card cross, green plus, red minus, teal dish, amber mic and dots. Do not replace it with thin generic icons. Tutorial taps should demonstrate actions, not send real captures or inventory mutations.

## Inspected iOS implementation

Active checkout: `/Users/MattTaylor/Desktop/Apps/trepo-ios-codex`, inspected HEAD `7232f46acf60edbda71334babff940ea3bed1949`. Other app work is in progress; read that checkout's `AGENTS.md` and preserve it. The provisioning files inspected here were unchanged against that HEAD. This is a source audit, not proof of which app binary is on a phone.

The existing app already obtains a cloud code before joining, joins the AP natively, uses local HTTP, scans networks, submits credentials, polls status, offers manual AP join and shows success/error screens. Focus changes on the following gaps:

| Current behavior | Work for the iOS agent |
| --- | --- |
| A fixed 90-second outer status loop and “up to 90 seconds” copy. | Derive a finite deadline from firmware Wi-Fi budgets plus account-link time; see timing below. Bound individual requests too. Do not promise an elapsed duration shorter than the firmware's allowed work. |
| Attempt-based progress bar continues into account linking. | Use distinct stages with an indeterminate animation where total work is unknown. Show actual attempt counts only in the Wi-Fi phase. |
| After 20 seconds connected without an owner, the app attempts the compatibility `POST /user-id`; it can repeat on later polls. | Keep the primary setup-code claim flow authoritative. Do not treat a slower claim as permission to bypass it. Handle `409 claim_busy` by continuing to observe; coordinate any fallback policy with the backend/firmware owners. |
| The local client handles `/wifi` HTTP 200 and 400 but turns 409 into “Invalid response.” | Decode the structured error and retry hints; retain the active operation instead of resetting it. |
| Setup-code `expires_at` is received but not used; Retry can reuse an old code. | Track validity and renew when needed before leaving internet access. An expired code is an account-linking recovery issue, not proof of a bad Wi-Fi password. |
| Multiple async entry points replace task handles; cancellation is swallowed in some paths; no generation fence or general sheet-disappearance cleanup. | Implement one session lifecycle and prevent stale success/error callbacks. Test Cancel, swipe-dismiss, returning from Settings and rapid Retry. |
| Open-network rows are shown, but empty passwords are rejected; password is visible by default. | Match firmware open-network support and use secure entry with reveal. |
| App route/title still says Halo Device / Pair Halo Device / Halo Setup. | Align customer-facing naming and entry labels with the installed device guide. |

## Verification for the app agent

Use a fake provisioning transport for deterministic UI/state tests, then one real-phone/device run. Keep mock results distinct from physical results.

| Scenario | Expected app behavior |
| --- | --- |
| Normal setup | Device identity established; one credential submission; Wi-Fi and account phases distinct; both screens complete; tutorial available. |
| Wi-Fi connects immediately, cloud claim fails twice then succeeds | App remains animated/responsive in account-linking stage; no premature success, password error or automatic direct-owner mutation. |
| First Wi-Fi attempt times out; second succeeds | Polling continues through the reported budget and then account linking; attempt display is correct. |
| Brief AP loss or a timed-out POST response | Preserve the session; reconcile `/status` and device identity before resubmitting credentials. No overlapping POST loop. |
| Scan pending, empty list, refresh and open/hidden network | Pending is not rendered as “no networks”; refresh is bounded; manual SSID/open-network entry works. |
| Wrong password, expired code, busy claim and owner mismatch | Each has its own recovery path; preserve useful input; never report unverified success. |
| Cancel/dismiss/retry/background during every stage | Networking is canceled or explicitly suspended; obsolete callbacks cannot change the next screen/session; returning resumes safely. |
| AP disappears immediately after observed success | Success stays visible and the app returns to ordinary networking; disappearance does not become a failure screen. |
| Provisioning followed immediately by check-in | Include this physical case in joint firmware/app testing. Earlier 176 failed here; 177 includes a fix, but the ordinary capture smoke was not a same-boot post-provisioning qualification. |

Record monotonic timestamps for code preparation, AP join request/result, first `/info`, each scan result, credential POST, Wi-Fi attempt/state changes, first connected status, owner confirmation, transport loss/recovery and completion. Include app build, stable device ID, session ID, HTTP status/error and duration. Redact Wi-Fi/AP passwords, setup codes, auth tokens and raw credential-bearing responses. Do not invent a firmware version from `/info`: the current endpoint does not expose one.

## Source map

Firmware paths are relative to the production root above:

- `LCD_Minimal/lcd_provision.h`: screen copy, QR rendering, 360 ms completion motion, reset confirmation and retry actions.
- `LCD_Minimal/lcd_provision_flow.h`: scroll bounds, app latch, 240-second progress limit and 4.5-second completion.
- `LCD_Minimal/lcd_ship_screens.h`: existing main menu, More/Settings and icon artwork.
- `LCD_Minimal/src/provisioning/qr_display.cpp`: pairing QR encoding.
- `halo_ota_demo/firmware/shared/ProvisioningManager.cpp` / `.h`: authoritative HTTP handlers and timing constants.
- `halo_ota_demo/firmware/shared/ProvisioningDisplayStatus.h`: device UI mapping; these status names are not the HTTP schema.
- `halo_ota_demo/firmware/shared/ProvisioningClaimJob.h` and `ProvisioningClaimTransport.h`: async claim ownership, cancellation and transport bounds.
- `docs/PROVISIONING_JOIN_INVESTIGATION_20260916.md`: observed device outcomes and remaining qualification limits.

iOS files are under `trepo_v0/trepo_v0/` in the app checkout:

- `ProfileView.swift`, `MainTabView.swift`: entry point and naming.
- `ProvisionFlowView.swift`: screens, progress, recovery, success and dismissal.
- `ProvisionViewModel.swift`: task lifecycle, QR parsing, join, submission, polling and fallback.
- `HaloProvisioningClient.swift`: local HTTP schema and request configuration.
- `ProvisioningAPIService.swift`, `TrepoAuth.swift`: cloud code and authenticated requests.
- `QRScannerView.swift`, `Info.plist`, `trepo_v0.entitlements`: scanner, permission messages and network configuration.

## Suggested instruction to the iOS agent

> Read this handoff and the current app source. Improve the kitchen assistant provisioning experience to match firmware 177: align labels, separate device join/home Wi-Fi/account linking, keep animations and cancellation responsive, fix bounded recovery and stale asynchronous callbacks, and add a short optional tutorial using the real device icons. Preserve the setup-code ownership flow and other agents' app changes. Test with deterministic transport scenarios and report what was also verified on a real phone. Do not change firmware, reset the device, or replace the account-linking protocol as part of this app UI work.
