# Camera Production Recommendations

This note captures recommended production firmware changes for camera capture tuning based on **93 captures across 25+ configurations**. The goal is to improve capture reliability, color consistency, and artifact suppression in `Sense_Minimal.ino`.

## Primary Recommendation

Apply these three changes first. They provide the highest impact with the smallest code diff:

```cpp
// Both camera profiles:
- s->set_lenc(s, 1);
+ s->set_lenc(s, 0);
+ s->set_wb_mode(s, 2);  // Cloudy

// Shared camera config:
- static int JPEG_QUALITY = 5;
+ static int JPEG_QUALITY = 10;
```

These three updates address:

- Green/yellow banding caused by lens correction
- Inconsistent color caused by automatic white balance drift
- Low-light JPEG capture failures caused by `Q5`

## Production Firmware Recommendations

### 1. Critical: Disable Lens Correction

Lens correction on the OV2640 introduced visible **green/yellow color banding** during testing. Configurations with `lenc=1` consistently showed the artifact, while captures with `lenc=0` did not.

Recommended change:

```cpp
s->set_lenc(s, 0);
```

Apply this in both production capture profiles:

- `apply_sensor_profile_normal()`
- `apply_sensor_profile_low_light()`

Note: `Sense_Minimal.ino` also currently enables lens correction in the default tuning path, so that path should be reviewed as well if it participates in production capture setup.

### 2. Critical: Force White Balance to Cloudy

Production currently leaves `wb_mode` on Auto. In testing, Auto white balance shifted between captures and reduced consistency across the Normal and Low-Light pair. Cloudy mode (`2`) produced the most consistent warm and accurate color across tested lighting conditions.

Recommended change:

```cpp
s->set_wb_mode(s, 2);  // Cloudy
```

Add this to both production capture profiles:

- `apply_sensor_profile_normal()`
- `apply_sensor_profile_low_light()`

### 3. Critical: Raise JPEG Quality Setting from 5 to 10

The current production setting is:

```cpp
static int JPEG_QUALITY = 5;
```

Testing showed `Q5` is not reliable for the Low-Light profile. The combination of `aec2=1`, `GAINCEILING_64X`, and `Q5` can overwhelm the OV2640 JPEG encoder.

| Quality | 20 MHz | 10 MHz | Reliability |
| --- | --- | --- | --- |
| Q5 | Fail 0/3 | Fail 0/3 (Low-Light), about 2/3 (Hybrid) | Broken for Low-Light |
| Q7 | Pass 3/3 | Pass 3/3 | Best reliable setting at 20 MHz |
| Q10 | Pass 3/3 | Pass 3/3 | Rock solid in all tested cases |

Recommended change:

```cpp
static int JPEG_QUALITY = 10;  // was 5
```

`Q7` appears viable, but `Q10` leaves more reliability margin with only a modest file size increase.

## Recommended Timing Changes

Production currently uses short settle times, which likely reduce AEC and AWB convergence reliability after profile switches.

| Parameter | Current | Recommended | Reason |
| --- | --- | --- | --- |
| `CAMERA_INIT_WARMUP_FRAMES` | 1 | 5 | More AEC iterations during startup |
| `CAMERA_CAPTURE_SETTLE_MS` | 40 ms | 200 ms | Gives AWB time to stabilize after profile switch |
| `CAMERA_INIT_SETTLE_DELAY_MS` | 150 ms | 150 ms | Keep unchanged |

### Timing Tradeoff

With `10 MHz` XCLK, `Q10`, `5` warmup frames, and `200 ms` settle delay, dual capture measured about **5.8 s** in testing.

Two viable production directions:

- **Option A:** Accept about `6 s` for dual capture in exchange for safer convergence
- **Option B:** Validate `20 MHz` XCLK with `Q7`, which measured about **3.9 s** for dual capture

If the existing `5 s` dual-capture budget is flexible, Option A is the lower-risk path.

## Optional Tuning Changes

### 4. Optional: Reduce Low-Light Gain Ceiling

Production Low-Light currently uses:

```cpp
s->set_gainceiling(s, GAINCEILING_64X);
```

This increases noise and may amplify residual green or IR contamination. Testing suggested `GAINCEILING_32X` produced cleaner images in hybrid settings.

Recommended experiment:

```cpp
s->set_gainceiling(s, GAINCEILING_32X);  // was GAINCEILING_64X
```

If truly dark scenes need more amplification, the retry path can escalate back to `64X`.

### 5. Optional: Soften Low-Light Contrast

Production Low-Light currently uses:

```cpp
s->set_contrast(s, -2);
```

This was aggressive in testing and tended to crush shadow detail. If shadow preservation remains an issue, try:

```cpp
s->set_contrast(s, -1);  // was -2
```

## Current Production Touchpoints in `Sense_Minimal.ino`

These are the main places where the recommended changes map into the current firmware:

- `JPEG_QUALITY` shared camera config constant
- `CAMERA_INIT_WARMUP_FRAMES`
- `CAMERA_INIT_SETTLE_DELAY_MS`
- `CAMERA_CAPTURE_SETTLE_MS`
- `apply_sensor_profile_normal()`
- `apply_sensor_profile_low_light()`

Also review:

- `apply_default_sensor_tuning()` because it currently enables `lenc`

## Minimum Viable Change Set

If only the smallest safe production patch is desired, implement the following:

```cpp
// apply_sensor_profile_normal()
s->set_lenc(s, 0);
s->set_wb_mode(s, 2);  // Cloudy

// apply_sensor_profile_low_light()
s->set_lenc(s, 0);
s->set_wb_mode(s, 2);  // Cloudy

// shared config
static int JPEG_QUALITY = 10;
```

This set should fix:

- Lens-correction banding
- White-balance inconsistency between captures
- Low-Light capture failures tied to `Q5`

Everything else in this note is optimization and tuning beyond the minimum reliable baseline.
