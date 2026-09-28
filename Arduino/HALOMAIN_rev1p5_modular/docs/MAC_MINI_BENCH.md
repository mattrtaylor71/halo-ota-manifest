# Current production source on the Mac mini

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
