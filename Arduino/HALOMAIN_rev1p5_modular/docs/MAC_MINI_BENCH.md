# Current production source on the Mac mini

## September 28 hub-move recovery

The USB-C hub also carries the build SSD. Moving it during the first226 snapshot
gate force-unmounted executable backing files: macOS recorded matching SIGBUS
crashes for the job worker, pipeline and regression runner, with the kernel
message `Object has no pager because the backing vnode was force unmounted`.
This was a storage interruption before compilation, not a firmware assertion.

Before resuming after such a move, verify the actual external APFS UUID
`84213E3E-886F-4110-850F-27A262DED004`, storage guard, locked toolchain, exact Git
source/snapshot and absence of the old processes. Preserve interrupted evidence.
Never create a replacement directory on the internal disk at a missing mount.
Keep the hub attached while a build or flash is active.

For this observed pre-compilation interruption only, the original226 directory
and job were retained; a fresh `candidates/6.4.226-recovery01` was authorized with
the same source `1ce0d04e95b1b2a039b27364a616a535642cd10b`, version and epoch.
No226 binary had been built or published. The unchanged canonical candidate
tools and bounded nonhardware runner are used; wrapper allocation rules and
artifact/proof records are not weakened or edited. This is not permission to
rebuild changed firmware under an occupied version.

USB serial identities survive a port move; physical locations change. The
verified Uno moved to `2-1.2.2.4` and its USB5V relay to `2-1.2.2.3`.
Only those two location fields were updated after passive identity proof.
Sense/LCD locations were not inferred while their interfaces were absent.
See `/Users/MattTaylor/halo-ram-campaign-20260928/followup226-prep/HUB-MOVE-HANDOFF.json`
and `/Users/MattTaylor/halo-refresh-haptics-20260928/build226/`.

## Original qualification and source migration

September 28, 2026. This is a private build migration, not installation or
production approval. Production remains paired6.4.224. New work preserves the
published224 source floor and uses freshly inventoried unused225+.

The bench is `/Volumes/Trepo-Work/Workspaces/halo-firmware-bench`. Current source
uses its separate `source-production/` Git checkout. Preserve historical
`source/` at the private222 qualification checkpoint: old immutable snapshots
reference that original repository and policy. Do not merge the private222
runtime or unrelated battery work into the current production branch.

The host-only changes were reviewed from commits
`ed134fafabf15edb4edd1c2f07994f440a0ff3d4` and
`e9ad3605b9aa345798ec471929d841ded6e706d4`. Existing production tests now honor
the isolated Arduino data/library paths and mbedTLS prefix. The negative163
fixture and qualified artifact checker have explicit host paths. The absent
private account-transfer test is excluded; newer224 test behavior is preserved.
These changes do not alter runtime firmware or production policy.

Run the SSD guard and use the bench's `./halo` entry point with the exact
reviewed commit, unused version, explicit UTC epoch and new candidate output.
The wrapper retains clean-source, ancestry, storage, dependency, frozen158,
immutable snapshot and artifact checks. It runs the snapshot's complete host
catalog and both canonical boards. All Mini outputs and temporary files stay
on the real Trepo-Work SSD. Never recreate a missing mount.

The qualified Python, Arduino CLI, SDK, libraries and compiler remain locked by
`control/build-environment.lock.json`. Do not refresh that lock to bypass a
mismatch. The reviewed Mini checker SHA256 is
`669263d7185debfdcc976500037ee001884f507a5af4642144f53e7504b75ffa`;
it independently verifies its retained original checker/input closure. Only
the original MacBook checker and this exact Mini adaptation are admitted.

The historical222 cross-host qualification does not qualify current224/RAM
source. Require new host gates, canonical paired artifacts, actual resource
reports and finite device evidence. Neither host tests nor a production-route
private build flashes a device, publishes OTA, or approves a release. Keep
resource/model gaps and historical failed receipts visible.
