# OTA installation screen handoff — published 187

The user's physical manual 185-to-186 update completed both images and both SDK-valid boots. The LCD nevertheless displayed a frozen “Checking for updates” spinner through its own transfer. The recorded pass for transport must not be described as a UI pass; see [the 186 test record](MANUAL_OTA_186_20260917.md).

The receive flag intentionally stops both LVGL render owners before flash erase/write. Previously it became active before the UI could replace its checking screen. Removing that protection would risk flash/cache activity overlapping display DMA.

The requested correction is limited to the LCD presentation handoff. The existing UI owner draws a static “Something new is coming” / “Installing update” screen before receive inhibition. There is no frozen spinner or misleading frozen percentage. A session-specific acknowledgment and the existing real-DMA drain share the original bounded handoff and OTA attempt deadline; refusal must happen before diagnostics, progress persistence or image erase. The UI owner retains its mutex between acknowledgment and UART inhibition, so another render cannot replace the accepted frame.

Sense, transfer transport, native daily allowance, retries, the 02:00 Pacific schedule, provisioning, shopping and media behavior remain outside this change. No extra preview command or synthetic BEGIN is used to manufacture hardware acceptance. A host rendering of the actual LVGL UI proves appearance; a USB installation/boot smoke test is distinct from a subsequent real OTA running the new receiver. Actual artifacts and acceptance are recorded only after completion.

## Completed release and validation

Source: `6523648c44938a339d0342e4fa14c185ac3bc3ba`; firmware tree `5faa214da16b4a98c2bf6042c23a7be9759a4859`; build `6.4.187-20260917T233400Z-6523648c4493`. The three LCD runtime headers are the only runtime changes; generated versions differ on both boards. Both canonical builds and exact artifact checks pass. All 11 snapshot suites pass; the focused actual-function test passed 76 checks under ASan/UBSan and reproduced 186's stale-frame bug as a negative control. Actual LVGL 8.3.6/font rendering confirms unclipped static text with no spinner/bar/percentage.

Controlled USB installation wrote only LCD's inactive app1 and alternate NEW selector, preserving 186 app0, NVS, partition table and Sense. Original-descriptor boot confirmation and subsequent fresh nonce/CRC identity show LCD 187 app1 and Sense 186 app0 SDKVALID. A real actuator wake and console-triggered live eight-item shopping list passed; the list returned Home automatically and both boards slept, with 29 seconds quiet and zero USB reopens. The first post-install actuator helper stalled; its failed record and closure follow-up are preserved. The relay reset proved USB disappearance/return and the subsequent stroke completed and was reaped.

The production pair is published with exact paired manifest/binary readback. Local tag `halo-v6.4.187` identifies the artifact source; no remote Git push. Sense 187 is published but is not installed on this unit yet. No daily allowance, unresolved debt or schedule configuration was changed. Final smoke Sense sleep used its existing 21,600-second clock fallback; LCD retained a 33,533-second nightly arm. This is not a new 02:00 scheduled qualification.

**Limit:** the actual new incoming-OTA presentation is not physically qualified yet. This release has host handoff/pixel coverage plus USB installation and normal-device smoke; it does not claim another full paired OTA, power-cut, USB-free or full-product test. Preserve the 186 observed UI failure as historical evidence.

Receipts:

- [Sealed canonical pair](/Users/MattTaylor/halo-ota-ui187-20260917/candidate187-001/RELEASE-PAIR.json) — SHA256 `544153dee8e5fd8fcfb5edeceb4b89d845e2a423b20ef31742b903e9ce7c77be`.
- [Actual-LVGL pixels](/Users/MattTaylor/halo-ota-ui187-20260917/visual-review001/RESULT.json) — SHA256 `6141342fedf9312ad28563cd39b42443d24249f1ec796ec767ae65074a8ffff0`.
- [Runtime independent review](/Users/MattTaylor/halo-ota-ui187-20260917/INDEPENDENT-HANDOFF-REVIEW.json) — SHA256 `1b6e79bde00dfcc3e7479ed2e7267f4cb1a78da53dec63e07d30fb18b61d098f`.
- [Source/install/smoke independent review](/Users/MattTaylor/halo-ota-ui187-20260917/SEALED-INSTALL187-INDEPENDENT-REVIEW.json) — SHA256 `ef55e715ac9b33af3f452a907ef8ad764f305376d59bdb0ab99d65ed72ddb2b8`.
- [Device result](/Users/MattTaylor/halo-ota-ui187-20260917/UI187-RESULT.json) — SHA256 `c7f176ca1c70894e9b2927e7ae7f46cc765ec179705bec7d82d90ea0490efac1`.
- [Publication](/Users/MattTaylor/halo-ota-ui187-20260917/PUBLISHED187.json) — SHA256 `03e6b368e9c2af667e8a6db71dbe637062abe029a968f4608785aa7e9974d437`.
