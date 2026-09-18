# HALO regression gate

Run the complete offline gate after each firmware fix. Focused tests are useful while editing, but are not a substitute for this gate before a new release. It covers everyday actions and the transitions between user input, uploads, retries, sleep, provisioning and OTA. It does not change the installed firmware, open serial ports, operate the actuator, contact the backend or publish anything.

## One command during development

From the production firmware root, with a fresh output path outside the source tree:

```sh
PY=/Users/MattTaylor/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/bin/python3.12
env -u __PYVENV_LAUNCHER__ "$PY" -B tools/run_regression_suite.py \
  --out /absolute/path/to/new-regression-results
```

The reviewed catalog is `tools/regression_suite.json`. The initial catalog contains 95 suites, including the seeded media storage/transport stress suites and tests of the runner and publisher themselves. New `tools/test_*.py`, `tools/stress_media_*.py` or `halo_ota_demo/tools/ota/test_*.py` files must be reviewed and added to the catalog; an omitted, missing or duplicate entry fails the gate. Do not register a script that interacts with real hardware or cloud services. `--list` prints the inventory without running it. `--jobs 1` reduces load; the default is two concurrent suites.

Every suite gets a bounded process group, a separate log and a result entry. The runner continues after individual failures so one run exposes all failing suites. A timeout, nonzero exit, reported skip, missing dependency, or changed source fails the aggregate result. It records the complete source fingerprint before and after the tests, including untracked files, with only Git/Python/OS cache files excluded. Do not edit source while a final gate is running. On interruption the runner stops admitting work and terminates its owned process groups; tests that explicitly start separate sessions remain responsible for their own cleanup.

`RESULT.json` records the exact inputs, suite results, log hashes, durations and limitations. Keep failing receipts too. Never replace a failing test with a source-text check or relax its assertion merely to obtain a pass. A regression test should exercise the failure and its corrected behavior; use a targeted negative control where appropriate.

## What is covered

| Area | Host coverage |
| --- | --- |
| Everyday interaction | Shopping fetch/delete, list idle behavior, scan confirmation, quantity/action summaries, camera handoff, voice status, touch priority |
| Uploads and offline storage | Image/voice receipts, spool persistence, corruption/partial writes, transport retries, Wi-Fi and clock loss, HTTP uncertainty, cancellation, duplicate acceptance and delete acknowledgments |
| Multiple actions | New capture interrupting an upload, saved work yielding to input, custody across cancellation → offline persistence → replay, sleep while work is pending |
| Sleep and wake | User wake, dark maintenance/retry wake, peer readiness, flush timing, UART tail budgets and reboot cleanup |
| Provisioning | AP authentication/restart, Wi-Fi ownership, async claim, memory reservation, progress/success screens, icon rendering and sleep inhibition |
| OTA | Manual readiness/touch handoff, clocks and daily schedule logic, discovery recovery, LVGL ownership/static installation frame, UART recovery, policy settlement and build/publication contracts |

The new composed voice test follows the same capture identity through uncertain upload, user interruption, a parked worker, Wi-Fi/time unavailability, persistence, later replay, an unrelated receipt, accepted duplicate response, lost storage-delete acknowledgment and a subsequent acknowledged delete. It compiles actual production decision functions with sanitizers; RTOS, radio and storage boundaries are doubles. The seeded transport stress suite also requires a retained pre-fix source that must fail its relevant regression.

## Host prerequisites

Use the qualified macOS host with Python 3.10+ (without `-O`), Apple C/C++ compilers/CommonCrypto, CMake, Git, Arduino CLI, ArduinoJson, LVGL, Homebrew mbedTLS and the installed ESP32 3.3.8 SDK. The preflight lists missing paths explicitly. Some suites compile actual LVGL screens and Preferences code; passing logic-only tests does not replace those suites. No dependency installation or SDK patching occurs automatically.

Two suites read retained Git history. `--history-repo` can select the original checkout when testing an exported snapshot. The default historical negative fixture is:

```text
/Users/MattTaylor/halo-device-analytics-2026-09-10/offline-backup-20260915/candidate163-002/snapshot/source
```

Use `--negative-source-root` if the same retained source is relocated. Preserve that small source fixture and Git history when cleaning build caches. Do not substitute current source or suppress the negative control. This first gate is deliberately qualified for the existing Mac; portability to another OS needs a separate reviewed adaptation.

## Bind the gate to a release

A working-tree PASS helps development but cannot authorize publication. Commit the reviewed changes, prepare the canonical release snapshot, then run **the runner inside that snapshot**:

```sh
env -u __PYVENV_LAUNCHER__ "$PY" -B \
  "$OUT/snapshot/source/tools/run_regression_suite.py" --out "$OUT/regression"
```

The adjacent materialization receipt is detected automatically. The publisher's `prepare` command requires `--host-result "$OUT/regression/RESULT.json"`. It binds both board build proofs to that exact materialization, source fingerprint, runner/catalog, complete case list and immutable log hashes. Stage and promote revalidate the evidence. Missing/failed/stale/partial receipts refuse publication before remote writes. Prior sealed releases remain historical evidence; do not edit their tools or receipts to retrofit this gate.

See [BUILD_AND_RELEASE.md](BUILD_AND_RELEASE.md) for the full sequence. Production schedule, OTA allowance, installed firmware and release bytes are unaffected by running host tests.

## Finite device acceptance

Host tests cannot prove real touch, camera/audio quality, RF conditions, backend processing or power behavior. Record the installed version, time, action and observed result for a short manual pass; preserve serial logs and cloud request IDs where available:

1. Wake from sleep; open/leave shopping list and settings; scroll a longer list and confirm a delete appears in the app.
2. Check in, dish and discard: capture, complete the choices, and verify the corresponding backend result. Try another action promptly after the first.
3. Hold/release voice, then use the menu again while upload is pending. Verify the requested list change or transcription in the backend, not just the local completion animation.
4. With a controlled Wi-Fi outage, save one image and one voice item. Restore Wi-Fi, confirm later delivery, and verify retry wakes stay dark. Tap during a retry and confirm user input wins. Keep this separate from ordinary online acceptance.
5. Let the device sleep and wake it again. Watch for an unwanted lit wake or frozen screen.
6. For changes touching onboarding, complete provisioning once. For OTA changes, test the relevant manual/scheduled path on exact published bytes; a host schedule test or manual update is not a physical scheduled-OTA pass.

Keep shipping decisions proportional to the change. Do not broaden a cosmetic or test-only update into an unbounded OTA campaign. A host PASS and a human device pass are separate evidence; neither proves every possible edge case.
