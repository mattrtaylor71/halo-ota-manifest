# HALO 6.4.209 version-only release

**Published for both boards; device 209 testing is pending.** The user requested a newer version for ordinary manual OTA. Source `cd2b84bc072a8f287c4f93bed99df091e04610ba`, firmware tree `607b49e924d036923c4f90a911416918efc04cca`, build `6.4.209-20260922T175500Z-cd2b84bc072a`; local annotated tag `halo-v6.4.209` (object `91133914f364a3d70c4485b8567729f3b3e30a12`).

Compared with the exact 208 snapshot, only three generated version/build headers and eight documentation/selector files differ. All 1,429 other files and runtime code within those headers are identical. The exact 114-suite snapshot gate, both canonical builds and camera-aware artifact checker passed. Both 208 latest manifests were conditionally advanced to 209; complete public binary readbacks match:

| Image | Bytes | SHA256 |
| --- | ---: | --- |
| Sense | 1,874,928 | `6dfc8840e25e1c3466eb1ec999a813f2b7707c03df5b458585dd282e6e65f9ca` |
| LCD | 2,032,896 | `3bf8881e2db47c3f9a5c53296ba6bb4ff707744608675d87b0b0c87150c2ef72` |

Last verified installed pair remains Sense 208/app0 and LCD 208/app1, both SDK VALID. Its one LCD manual transfer and one same-version check after Sense USB bootstrap are recorded in [the 208 handoff](RELEASE_208.md); they are not 209 device acceptance. No new physical scheduled, Sense self-OTA or broader reliability qualification is claimed. Production 02:00 Pacific, factory 197 and frozen 158 remain unchanged.

Exact source/proofs/tests, publication, public readback and scope review are retained under `/Users/MattTaylor/halo-release209-20260922`. No remote Git push was performed. Future releases must use source cd2b84b or reviewed descendants and a freshly inventoried unused 210+ version.

The private recovery/debug package is `/Users/MattTaylor/halo-releases/6.4.209.tar.gz`; its archive record verifies all 1,754 members and the exact source history. `RELEASE-RECORD.json` binds that archive to the actual publication and pending device test. Raw device logs and media are excluded.
