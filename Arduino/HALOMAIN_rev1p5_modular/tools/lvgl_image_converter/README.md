# LVGL Image Converter

Turn any image (PNG/JPG/…) into an LVGL `lv_img_dsc_t` C asset you can drop into
the firmware. Tuned for this project's LVGL config: **RGB565, `LV_COLOR_16_SWAP=1`**
(so the byte-swap is on by default — the thing that's easy to get wrong by hand).

Two ways to use it: a web UI (drag-drop + live preview) or a CLI.

## Web UI

```bash
python3 tools/lvgl_image_converter/server.py        # http://localhost:9097
python3 tools/lvgl_image_converter/server.py 9099   # custom port
```

Drop an image, tweak the options, and the preview shows **exactly what the panel
will render** (RGB565 quantisation + alpha + recolour) on a transparency
checkerboard and on teal / white / black button backgrounds. Then **Download .c**
or **Copy source** / **Copy LV_IMG_DECLARE**.

## CLI

```bash
# colour icon, fit to 90 px tall, trim transparent edges
python3 convert.py "bird.png" -n ai_bird --crop --height 90 -o ai_bird.c

# dark monochrome icon for a white button (force colour, keep the shape's alpha)
python3 convert.py "utensils.png" -n dish_icon_img --crop --width 40 --height 40 \
    --recolor 1a1a1a -o dish_icon.c

# recolourable mask (smallest — 1 byte/px, tint at runtime)
python3 convert.py "utensils.png" -n dish_mask -f alpha_8bit --crop --height 40 -o dish_mask.c
```

Useful flags: `--width/--height` (one = scale by aspect, both = exact, neither =
original), `--crop` (trim to content), `--alpha source|white_key|opaque`
(`white_key` makes a white background transparent), `--recolor RRGGBB`,
`--no-swap` (for an `LV_COLOR_16_SWAP=0` build), `--preview out.png`.

## Formats

| Format | Bytes/px | Use for |
|---|---|---|
| `true_color_alpha` *(default)* | 3 | colour icon with transparency (the bird) |
| `alpha_8bit` | 1 | single-colour icon, recoloured at runtime — smallest |
| `true_color` | 2 | opaque image, no transparency |

For `alpha_8bit`, set the colour in code: `lv_obj_set_style_img_recolor(img, c, 0);`
and `lv_obj_set_style_img_recolor_opa(img, LV_OPA_COVER, 0);`

## Wiring it into the firmware

1. Put the generated `.c` next to the other assets:
   `halo_ota_demo/firmware/halo_lcd_prod/` (it's compiled by the `halo_lcd_prod` wrapper).
2. Declare it where you use it: `LV_IMG_DECLARE(my_img);`
3. Render it:
   ```c
   lv_obj_t *img = lv_img_create(parent);
   lv_img_set_src(img, &my_img);
   lv_obj_center(img);
   ```
4. Rebuild + flash the LCD per `CLAUDE.md`.

## Memory

Assets are `const` and live in **flash** (`.rodata`), `width × height × bytes/px`.
LVGL streams `true_color*` pixels straight from flash during rendering — **no extra
RAM/PSRAM** beyond the existing draw buffer. Unreferenced assets are dropped by the
linker (`--gc-sections`).

Needs Pillow: `pip3 install Pillow`.
