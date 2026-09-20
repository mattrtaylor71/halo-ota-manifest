# Halo account-independent backend acceptance — September 20, 2026

**Status: image admission restriction corrected; full account-independent acceptance is incomplete.** Firmware source remains the frozen 197 baseline and public OTA remains 196. This work does not publish firmware or qualify a new hardware build.

## Confirmed production correction

The serving image presigner had `UPLOAD_ADMISSION_V1=true` with `UPLOAD_ADMISSION_OWNERS` restricted to one earlier test owner. A normally provisioned account outside that list received HTTP503 before obtaining an image upload grant. This affected check-in, discard and dish, including stored retries.

On September19 at17:53Pacific, the owner key was removed entirely using a revision-guarded Lambda configuration update. Every other environment setting and the deployed code digest remained unchanged. Empty string and `*` are **not** all-account values: the key must be absent. A read-only nonexistent-operation reconciliation under the user's current account changed from account-rejected503 to operation-absent410. That verifies admission eligibility, not a successful upload.

September20 readback confirms the same configuration/code remains live. Both ordinary and selected-device image/discard buckets have enabled EventBridge processing rules. Voice ingest and worker have no discovered owner/cohort allowlist. Device-based bucket routing remains unchanged.

The real user's Late July discard subsequently reached the backend at18:10:08Pacific on September19 and completed processing at18:10:29. It is consistent with saved retry recovery, but no physical spool log establishes that it was the exact earlier pending capture. Its `add_to_shopping_list=true` intent survived. The worker nevertheless ended with `shopping_list_status=SKIPPED_UNKNOWN_ITEM`, and no matching shopping-list row was found. Thus this case is **not** an end-to-end discard/add success.

## Required finite acceptance

Use normally app-created QA accounts, each with a separate household and synthetic Halo ID. Do not rebind a real device or seed results directly in product tables. Record exact owner/device/request/job identities privately.

| Case | Required observation |
| --- | --- |
| Provisioning | App issues code, normal claim returns the user UUID, device persists/readbacks identity; distinguish UUID from household code |
| Check-in | Exact firmware presign JSON and checksummed immutable JPEG PUT; completed recognition plus kitchen record |
| Discard/add | Recognized discard, intended kitchen decrement/removal, exactly one visible shopping-list addition |
| Discard/no-add | Discard recorded without an unintended list addition |
| Dish | Completed result visible in the corresponding account |
| Voice | Raw16kHz mono PCM with frozen firmware request/session identity; transcript/action plus expected list item |
| Shopping list | Device-format read returns canonical item UUID; delete returns affectedRows>0 and stays deleted on refresh |
| Recovery | Persist envelope+bytes, restart client, partial image PUT, lost acknowledgements, same-operation replay; original cloud identity retained |
| Duplicate prevention | Repeated accepted image/voice submissions do not repeat side effects; changed immutable details are rejected |
| Account switch | A's captures never replay into B; cached A list cannot display as B; concurrent old responses cannot repopulate cleared cache |

A200 PUT or202 voice receipt proves cloud acceptance only. Verify worker completion and actual application state separately. Computer probes do not establish physical SD durability, radio recovery, backlight behavior, gestures, OTA or provider sign-in.

## Evidence and unresolved work

Private evidence directory: `/Users/MattTaylor/halo-account-readiness-20260919`.

- `admission-rollout001/RESULT.json`: exact correction and pre/post eligibility probes; private configuration/ZIP backups remain beside it.
- `cloud-inventory004.private.json`: September20 read-only configuration and processing-trigger inventory.
- `backend-admission-tests001/RESULT.json`:49 local admission/signing/triage tests passed. Local source is older than serving reconciliation code; these are not live backend acceptance.
- `probe-tests-20260920/RESULT.json`:8 image and3 voice transport/mock tests passed, including process restart, interrupted transfer, duplicate/conflict handling and request journaling.
- `FIRMWARE_ACCOUNT_CONTRACT.md`: source references for account identity, retained retries, and unresolved account-switch cases.
- `recovered-discard-20260920.private.json`: exact real discard job/SQL outcome; add-to-list did not pass.
- Two administrative test accounts were created with serving account-initializer helpers before the safety interruption. They bypassed normal provider sign-in and are **not valid proof of normal app signup**. Their private generated credentials remain unused. Eight image and two voice cases were prepared locally; zero upload/provisioning attempts were executed for those cases. Retain the account/fixture inventory for narrowly scoped cleanup; do not delete broadly.
- The run stopped with safety-system reason “Potentially unintended activity”; no specific denied tool operation was identified. Live account tests remain pending selection of a normally app-created account. Do not retry generated-credential provisioning to work around that boundary.

The source review identifies an unbound LCD shopping-list cache across reprovisioning, missing owner-write readback at provisioning success, and a RAM-photo retention edge case during owner reset. These have not been physically reproduced or fixed in this checkpoint. Persisted SD image/voice and SPIFFS voice retries compare the saved owner before replay; legacy flash photos intentionally do not replay without identity.

Do not claim general exactly-once database effects solely from immutable uploads. Existing discard SQL insertion and job-completion recording are separate operations; a worker crash between them requires explicit recovery coverage.

## Prioritized implementation plan

1. **Discard recognition and list delivery:** replace substring-based unknown classification with explicit structured identity/confidence handling. The deployed normalizer can erase a nonempty recognized product solely because the explanation contains a phrase such as “exact product,” including positive wording. Characterize legitimate unknown cases and disagreements between fast/deep results before choosing a fallback. Persist the final recognized discard consistently and apply the saved add-to-list intent exactly once using a stable job-derived identity and durable completion record. Reconcile member/shared list writes with the actual serving list contract. Do not patch product rows manually and call the flow fixed.
2. **Account transition:** bind list caches/results to the current owner and a request generation; hide unbound legacy cache. Validate and read back owner persistence before reporting setup success. Prove that RAM captures have durable custody before clearing their owner during reprovisioning.
3. **Normal-account live matrix:** select app-created QA identities through the ordinary sign-in flow, then execute the matrix above via the real firmware endpoints. Test separate households first; add shared-household membership and owner transition cases with explicit fixture scope. Keep persisted operation bytes/identities unchanged between process restarts.
4. **Release integration:** reconcile clean backend source against serving packages, run focused regression tests and the live matrix, then preserve deployment receipts and all-account configuration checks. Any firmware correction must retain197 ancestry and use an unused198+ build with the full exact-snapshot host and finite device acceptance gates.

These are planned corrections, not deployed fixes. The exact original deep-recognition explanation was not retained, so its triggering phrase cannot be reconstructed. The observed unknown conversion and skipped list stage are independently evidenced.

## Prevent a deployment regression

The shared backend checkout contains unrelated changes and differs materially from the serving packages. Its presign template omits live admission/routing variables, and its local reconciliation/voice retry code is older. **Do not deploy that checkout wholesale.** Reconcile a clean, reviewed backend source with the serving artifacts first, preserve environment variables and reconciliation permissions, then repeat this matrix.

Run the read-only collector at `.../halo-account-readiness-20260919/tools/cloud_inventory.cjs` into a fresh private file. Then, from this firmware directory:

```
python3 -B tools/verify_backend_scope.py /absolute/path/to/fresh-inventory.json
python3 -B tools/test_verify_backend_scope.py -v
```

The guard checks the reviewed serving image/voice code digests, required settings, function readiness and all required inventory checks. It rejects reinstated owner gates, empty/wildcard gates, stale snapshots, older code and incomplete evidence. Default maximum snapshot age is24hours. It reads a local sanitized inventory only; it does not refresh AWS, authorize a deployment, run in the firmware, or prove end-to-end processing. Reviewed backend updates need a deliberate contract refresh plus new evidence. Keep credential/token files and private cloud receipts out of Git.
