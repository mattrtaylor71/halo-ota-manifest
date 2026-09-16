# Kitchen assistant UI design reference

Source of truth: the existing device UI in `/Users/MattTaylor/halo-camera-recovery-2026-09-15/Arduino/HALOMAIN_rev1p5_modular/LCD_Minimal`, especially `lcd_theme.h`, `lcd_ui_design.h`, and `lcd_ship_screens.h`. This is a prototype design reference; no firmware changes were made.

## Visual language

- Warm cream canvas: `#F5E9D8`. White raised surfaces, ink `#1A1A1A` for borders and primary text.
- Nunito Black for headings, badges, and short action labels. Montserrat Medium for secondary explanatory copy. Use fewer words and a clear hierarchy.
- Rounded surfaces with 2px ink outlines. Use crisp, small shadows offset down and right, without blur: generally 3px; compact pills 2px; existing main-menu cards 4px with 85% ink opacity.
- Yellow `#FFD54F` identifies contextual guidance, such as the “Trepo App” pill. Keep the pill compact, centered, with generous horizontal padding and the small offset shadow. Device badge baseline is 25px high, radius12, Nunito12; larger prototype guidance uses the same proportions with readable 16px type.
- Forest green `#1F4D2B` for confirmation and check-in; red `#E53935` for discard; teal `#296065` for dish and guidance; amber `#A66A00` for mic and More.
- Center each screen’s content on x180 of the 360px display. The right-edge scroll cue is independent: it must not shift the content left. Preserve breathing room between the cards and curved cue.
- One clear task per screen. Context pill first, action or QR next, no redundant sentences. Step labels remain quiet. QR codes retain a clean white quiet zone.
- Motion communicates progress or completion: a simple rotating connection arc, a brief success check, short transitions. Honor reduced-motion preferences. Avoid orbiting decoration or competing animations.

## Main-menu artwork is fixed

Keep the current five-card cross layout, all white cards, 92×92px, radius26. Coordinates: dish(134,24), check-in(24,134), mic(134,134), discard(244,134), More(134,244).

- Dish: reuse the existing 54×54 bitmap alpha, recolored teal. The baseline `assets/current/dish_icon.png` alpha matches the current firmware byte for byte.
- Mic: retain the current clean 54px `HALO_ICON_MIC` drawing in `lcd_ui_design.h`, with a 4px stroke. Do not use the legacy mic bitmap; it caused the faint grey background defect.
- Plus and minus: solid rounded 46×10 bars; plus adds a vertical 10×46 bar.
- More: three 12px filled dots, spaced 21px between centers.
- Do not replace these with generic thin-stroke menu icons. The prototype now uses the existing dish alpha and mirrors the current menu drawing geometry, including in the companion app tutorial.

## Current prototype revision

Steps1 and2 are centered independently of the edge arrow. Step1 has no camera instruction. Steps2 and4 say “Trepo App.” New setup screens retain the device palette, typography, outlined rounded surfaces and crisp shadows. Production menu artwork stays unchanged when this flow is implemented.
