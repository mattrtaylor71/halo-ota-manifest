# HALO workspace baseline

- The authoritative production project is `Arduino/HALOMAIN_rev1p5_modular`; read its `AGENTS.md` and `docs/FROZEN_RELEASE_158.md` before changing firmware.
- Use `RELEASE_BASELINE.json.current_baseline`, currently frozen 6.4.158, to select source and artifacts. Work from this checkout or reviewed descendants of `halo-v6.4.158`. Historical records and `Arduino/HALOMAIN_rev1` are not alternative current baselines.
- Run `python3 -B tools/verify_frozen_baseline.py` from the production firmware directory before preparing new firmware. This read-only check verifies ancestry and retained artifact identity, not the correctness of uncommitted changes.
- Preserve the immutable tag and published 158 bytes. Future changes use an unused version 6.4.159 or later, clean committed source, the canonical snapshot/builder, and the documented paired publication workflow.
- Keep the recorded acceptance limits. Do not call an assisted installation a scheduled pass, clear unresolved OTA debt to force a passing test, or treat a factory image as an ordinary update.
- Keep current release metadata synchronized with actual receipts. The private release archive contains source history and is not a public download asset. Do not push, upload, or delete archives merely as documentation cleanup.
