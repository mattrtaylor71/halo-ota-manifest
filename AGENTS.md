# HALO workspace baseline

- Current installed development baseline is private167 (Sense app1, LCD app0, SDK VALID). A minimal168 candidate removes the optional final Wi-Fi diagnostic UART frame; read `docs/SLEEP_TAIL_168.md` and `docs/OFFLINE_MEDIA_STRESS_167.md`. Disk space is available; new build/device validation is pending. Preserve the tested167 pair, known incomplete image residue, and historical165/166 evidence. Public OTA is last-verified162; no168 installation or publication yet. Durable image backend remains test-owner only.

- The authoritative production project is `Arduino/HALOMAIN_rev1p5_modular`; read its `AGENTS.md` and `docs/FROZEN_RELEASE_158.md` before changing firmware.
- Use `RELEASE_BASELINE.json.current_working_source` for new changes and `current_baseline` for the retained frozen158 recovery artifacts. Work from this checkout or reviewed descendants preserving158 OTA fixes,159 LCD wake handling and162 camera input recovery. Historical records and `Arduino/HALOMAIN_rev1` are not alternative current baselines.
- Run `python3 -B tools/verify_frozen_baseline.py` from the production firmware directory before preparing new firmware. This read-only check verifies ancestry and retained artifact identity, not the correctness of uncommitted changes.
- Preserve the immutable tag and published 158 bytes. Future changes use an unused version 6.4.168 or later, clean committed source, the canonical snapshot/builder, and the documented paired publication workflow.
- Keep the recorded acceptance limits. Do not call an assisted installation a scheduled pass, clear unresolved OTA debt to force a passing test, or treat a factory image as an ordinary update.
- Keep current release metadata synchronized with actual receipts. The private release archive contains source history and is not a public download asset. Do not push, upload, or delete archives merely as documentation cleanup.
