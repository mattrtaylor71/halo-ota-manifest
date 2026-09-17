# Guided kitchen assistant setup — installed private169

> Historical implementation record. For the current 177 flow and iOS integration contract, use [Kitchen assistant provisioning: iOS handoff](PROVISIONING_IOS_HANDOFF_177.md).

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

A diagnostic screen preview is rendering/state evidence, not proof of a real phone provisioning and cloud-owner claim.

## Installed result — September16, 2026

The exact artifact source is `ba5012421ce48397ddf53f5203f7925c4f860513`, build `6.4.169-20260916T215419Z-ba5012421ce4`. Implementation is committed in `8cef1ca`, lifecycle cleanup in `00a7b4d`, and the scoped regression adjustment in `ba50124`. Both canonical production builds, exact artifact checks, and nine host suites passed. The retained failed001 build exposed obsolete cleanup names;002 exposed an overly broad historical source-equality fixture.003 is the final checked pair; all earlier evidence is preserved.

Both boards were installed by inactive-bank USB service and observed running/selected SDK VALID: Sense169 app1, LCD169 app0. Prior168 complete fallback banks remain. Before/after NVS and partition tables are byte-identical; stored credentials, SD media, OTA schedule, allowance and debt were not reset. This installation is not an OTA qualification or public publication. Public manifests remain last-verified162.

On the actual LCD, all approved text in steps1–3 rendered, both scroll clamps and backward navigation worked, and three repeated page cycles returned to stable same-page LVGL free memory without increased panel flush failures. The unit was left at the real step3 provisioning QR. Original menu geometry/artwork remains unchanged. The test reads actual object layouts and driver counters, not a photograph of the panel.

The separate six-screen simulation refused to run because real provisioning was already active. The pre-install168 capture contains repeated PROVISION_QR before either board was flashed; this state was preserved. Hardware tests therefore used the real first three pages without changing credentials or faking app completion. Phone-driven steps4–6, the connection/success animation and4.5-second return need a real app setup to finish validation. Shopping refresh and ordinary sleep were not attempted while setup remained active. Their existing scoped host regressions pass, and the inherited168 clock-timeout sleep issue remains unresolved by this UI change.

## Evidence

All private receipts live under `/Users/MattTaylor/halo-provisioning-review-2026-09-16/device169`:

- `candidate169-003/RELEASE-PAIR.json`: exact paired bytes/source; SHA256 `51c4e5b5e3a550ea36e8f3a84b1bada90480850d1a1365da93692cb20a4894e5`.
- `candidate169-003/host-final001/RESULT.json`: nine passing suites; includes actual Sense display bridge, LCD flow, provisioning ownership, LVGL allocation, OTA render/presentation and wake/media regressions.
- `INSTALLATION169-PAIR-SERVICE-REVIEW.json`: independently rehashed bank backups/readbacks, preserved NVS/table/VALID fallback selectors and closed installer owners.
- `HEALTH169-OBSERVED001.json`: fresh paired169 running/selected SDK VALID identity with LCD nonce/CRC verification.
- `hardware169-guide001/RESULT.json`: preserved refused preview attempt during real setup.
- `hardware169-realguide001/RESULT.json`: passing real steps1–3, scroll/memory/flush results, raw capture references and closed descriptor.

Static resource change versus168: Sense +624bytes application/+40bytes RAM; LCD +36,448bytes application/−8bytes RAM; RTC allocations unchanged. No whole-task stack high-water or full product acceptance is inferred. Continue future work from this source or reviewed descendants, using170 or later; preserve the frozen158 reference and the168 media/sleep evidence.
