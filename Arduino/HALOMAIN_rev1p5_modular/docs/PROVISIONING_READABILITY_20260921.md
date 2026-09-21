# Provisioning readability — September 21, 2026

Unreleased cosmetic changes on the current 199 descendant. Neither board was flashed and no OTA was published; published/installed 199 and its recorded acceptance limits remain unchanged.

- Enlarge “Scroll for next step” from 12 to 16 px (33%). Thicken the single downward arrow and its arrowhead from 2 to 4 px, retaining centered page content and existing scroll behavior.
- Change the step 2 Profile badge from yellow to white, retaining the dark filled person and circular dark outline. The app reference is `Desktop/Apps/trepo-ios-codex/trepo_v0/trepo_v0/MainTabView.swift`, lines 95–103: `person.fill` on a white circular button. Only this setup badge changes; the yellow “On Trepo App” pill retains its existing appearance.
- Regenerate the checked-in alpha mask with the same Nunito Black font. Mask data grows from 6,916 to 12,255 bytes of static flash; no new UI object, font or animation is added.

## Validation

Actual LVGL 8.3 rendering of both production page branches, real fonts and QR widget: no cue/content overlap or circular clipping; minimum cue-to-rim clearance 6.954 px. Existing Profile callback test passed, including its missing-person negative control and repeated redraw allocation check. All 104 offline regression suites passed with no skipped cases. Source stayed unchanged during that gate; this documentation and the preview PNGs were added afterward. No physical-panel or new firmware-build qualification is claimed.

![Step 1](provisioning-ui/readability-step1-20260921.png)
![Step 2](provisioning-ui/readability-step2-20260921.png)

Evidence: `/Users/MattTaylor/halo-provision-readability-20260921/` contains `profile-render001`, `render-tools/page-renders/RESULT.json` and `regression001/RESULT.json`.

Initial readability regression result SHA-256: `48534b75b52a3d3724f633c4fa6cdb51f51a119125a80c839abe432d225c1946`.

Future releases must include these source changes on top of 199 and use the existing canonical build/publication workflow with an unused version 200 or later. Existing release metadata continues to describe the published bytes, not this pending UI change.

## Arrow thickness follow-up

The regenerated asset changes only the lower arrow pixels (x276–315, y279–318); all text pixels are identical. Both actual LVGL page renders again pass overlap/clipping checks, and all 104 offline suites pass on the updated source. The previews above now show the 4 px arrow. No firmware was built, installed or published.

Evidence: `/Users/MattTaylor/halo-provision-arrow-20260921/asset-diff.json`, `regression001/RESULT.json`, and `/Users/MattTaylor/halo-provision-readability-20260921/render-tools/arrow-renders/RESULT.json`.

Arrow follow-up regression SHA-256: `b47cfff860b48c013727f484186695a6e847ce08597c8f805044da5e248c956d`.
