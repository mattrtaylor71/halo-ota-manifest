# Published provisioning UI release 6.4.200

September 21, 2026. Both production OTA manifests and complete binaries were published and read back. User manual installation is pending; neither board was flashed during this release.

## Included changes

- Increase “Scroll for next step” from 12 to 16 px.
- Use a 4 px curved arrow around the full text length, with one arrowhead at the bottom.
- Match the app’s white Profile badge and custom dark/teal/cream kitchen assistant illustration.

Runtime differences from199 are limited to LCD provisioning artwork and its generated alpha mask. Sense behavior, OTA policy, retries and 02:00 Pacific scheduling retain199 source. Main menu artwork and provisioning state transitions are unchanged.

## Identity and verification

Source `a145313b6ada576f0a3413abf919fe68447d9ed1`; firmware tree `9adb706f3528c89c6c97c5381b3723398f8c4da2`. Build `6.4.200-20260921T230415Z-a145313b6ada`. Local immutable source tag `halo-v6.4.200` (`2c7c81504e95b6772f324a5ed66c91af677bd416`); no Git remote push.

| Board | Application bytes | SHA-256 |
| --- | ---: | --- |
| Sense | 1,869,008 | `9de8d20c20b488ccdd7946a42ff3a74837f29c8104845852e73dfe3a259ced00` |
| Lcd | 2,032,720 | `6c586f4c41b0948592b2a0ca1913415c7a70fbb9d99e2b4beac9d87c96b2e110` |

The complete working-tree and exact-snapshot 104-suite gates passed, as did paired canonical builds and artifact checks. Actual LVGL page rendering showed no overlap/clipping; 50 redraws retained no additional heap. LCD application grows 2,592 bytes versus199; Sense size is unchanged. Static RAM/RTC figures are retained in both build proofs.

## Installation limits

The same bench unit has a documented orphan saved `halo/lcd_ota_due=1` despite a resolved198 policy record. That can return “Update postponed” before fetching the new manifest. This UI release does not fix or clear it. Last observed paired 199 installation and its partial voice/schedule acceptance remain separate; publishing 200 does not establish manual or scheduled OTA success. The earlier first-wake AES allocation failure also remains unresolved.

After the user’s manual attempt, verify both actual running 200 versions, valid application state and normal UI before recording installation. Preserve real failure evidence if the request is deferred; do not clear state to manufacture a passing test.

## Retained evidence

`/Users/MattTaylor/halo-release200-20260921/` contains the inventory, immutable snapshot/materialization, exact-snapshot regression, paired build/artifact proofs, prepared publication, stage/promote receipts, `PUBLIC-READBACK.json` and `RELEASE-RECORD.json`. Public full binary readbacks are `promote001/get-003.body` (Sense) and `get-005.body` (LCD); final latest manifests are `get-010.body` and `get-008.body` respectively.

Continue future firmware from 200 or reviewed descendants, using an unused 201+ after fresh inventory. EOL factory remains 197 and historical 158 recovery packages remain immutable. [Presentation details and previews](PROVISIONING_READABILITY_20260921.md).

## Private archive

The exact release package is `/Users/MattTaylor/halo-releases/6.4.200`; archive `/Users/MattTaylor/halo-releases/halo-6.4.200-release.tar.gz`. All 2,817 payload files were rehashed from the archive. Archive SHA-256: `a0a1035529aab3df53d48a62afb086e652bd992eb9ddb4b445999ea9ac83c65e`. The package includes exact application/ELF/build-source and test/publication evidence; compiler caches are excluded. This archive is local and was not uploaded.
