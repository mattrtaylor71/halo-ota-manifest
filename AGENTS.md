# HALO workspace baseline

- Latest working code: use `RELEASE_BASELINE.json.current_working_source` (159 LCD wake fix) and read `docs/LCD_USER_WAKE_159.md`. Preserve frozen158 as the recovery/previous scheduled qualification reference; do not revert the159 wake fix for subsequent releases. Public latest162 contains the camera input-recovery candidate; read `docs/CAMERA_INPUT_RECOVERY_162.md` in the production firmware directory. The assembled unit remains LCD159/Sense158;162 device acceptance is pending installation.

- The authoritative production project is `Arduino/HALOMAIN_rev1p5_modular`; read its `AGENTS.md` and `docs/FROZEN_RELEASE_158.md` before changing firmware.
- Use `RELEASE_BASELINE.json.current_working_source` for new changes and `current_baseline` for the retained frozen158 recovery artifacts. Work from this checkout or reviewed descendants preserving both158 OTA fixes and159 LCD wake handling. Historical records and `Arduino/HALOMAIN_rev1` are not alternative current baselines.
- Run `python3 -B tools/verify_frozen_baseline.py` from the production firmware directory before preparing new firmware. This read-only check verifies ancestry and retained artifact identity, not the correctness of uncommitted changes.
- Preserve the immutable tag and published 158 bytes. Future changes use an unused version 6.4.159 or later, clean committed source, the canonical snapshot/builder, and the documented paired publication workflow.
- Keep the recorded acceptance limits. Do not call an assisted installation a scheduled pass, clear unresolved OTA debt to force a passing test, or treat a factory image as an ordinary update.
- Keep current release metadata synchronized with actual receipts. The private release archive contains source history and is not a public download asset. Do not push, upload, or delete archives merely as documentation cleanup.
