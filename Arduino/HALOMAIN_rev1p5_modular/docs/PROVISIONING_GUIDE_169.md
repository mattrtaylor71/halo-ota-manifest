# Guided kitchen assistant setup — candidate 169

This change implements the approved six-screen provisioning design on the LCD. The source starts from private168 and preserves the existing menu artwork, camera/audio recovery, OTA policy, schedule, partitions and stored media. The known168 clock-timeout sleep limitation remains separate.

## Flow

1. Get the **Trepo app**, with a real QR for `https://hellotrepo.com/app-signup`.
2. Yellow **On Trepo App** pill, then **Profile → Setup kitchen assistant**.
3. **Scan to connect**, with the actual device provisioning QR.
4. After the app contacts the setup API: **On Trepo App**, **Select home Wi-Fi**.
5. Wi-Fi connection animation, then account-link progress.
6. **Setup complete / Welcome to your assistant**, followed by the normal menu after4.5seconds.

Steps1–3 accept scrolling. The curved right-edge cue is independent of the centered content. Forward scrolling stops at3; a detected app session takes precedence even if the user has scrolled backward. After app detection, navigation is driven by real provisioning state. Repeated QR/status packets must not regress the guide or restart its success timer.

## State contract

Sense retains its existing provisioning state machine and network/claim retry budgets. A presentation bridge converts state to `PROVISION_STATUS` messages:

| Message | LCD meaning |
| --- | --- |
| `ap_setup` | Setup guide is available; retain current scroll page. |
| `app_connected` | App API request detected; advance to4 and lock scrolling. |
| `connecting` | Home-network connection is underway; show5. |
| `claiming` | Wi-Fi connected, account not linked yet; retain5. |
| `connected` | During guided setup, Wi-Fi and owner presence are confirmed; show6. Normal provisioned wakes do not show a success screen. |
| `failed` / `error` | Recoverable setup failure; do not show success. |

App detection stays latched through an HTTP pause. Status is repeated every3seconds during setup to recover missed UART frames, with at most one presentation poll per500ms. Account completion is still observed during the existing post-AP retry window. Failure after that bounded window must remain visible rather than being misreported as successful setup.

## Design and scope

The approved prototype is retained in `provisioning-ui/approved-prototype.html`; firmware-derived visual rules are in `provisioning-ui/DESIGN.md`. The main-menu icon shapes are fixed. The companion iOS tutorial in the prototype is a separate app proposal; this firmware change does not modify the independently developed iOS checkout.

Build, installation and physical validation receipts will be recorded after they exist. A diagnostic screen preview is rendering/state evidence, not proof of a real phone provisioning and cloud-owner claim.
