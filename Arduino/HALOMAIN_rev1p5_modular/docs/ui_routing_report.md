# UI Routing Report

## Screen Registry
| ScreenId | Name | Bitmap | Defined At |
| --- | --- | --- | --- |
| SCREEN_SHIP_MAIN_MENU | SHIP_MAIN_MENU | ui_img_Frame_443_png | LCD_Minimal/ui_screen_registry.h |
| SCREEN_SHIP_SECOND_MENU | SHIP_SECOND_MENU | ui_img_Frame_443_1_png | LCD_Minimal/ui_screen_registry.h |
| SCREEN_SHIP_SETTINGS | SHIP_SETTINGS | ui_img_Frame_443_1_png | LCD_Minimal/ui_screen_registry.h |
| SCREEN_SHIP_HOLD_STILL | SHIP_HOLD_STILL | ui_img_SCREEN_HOLDSTILL_png | LCD_Minimal/ui_screen_registry.h |
| SCREEN_SHIP_PROCESSING | SHIP_PROCESSING | ui_img_SCREEN_PROCESSING_png | LCD_Minimal/ui_screen_registry.h |
| SCREEN_SHIP_LOGGED | SHIP_LOGGED | ui_img_SCREEN_LOGGED_png | LCD_Minimal/ui_screen_registry.h |
| SCREEN_SHIP_ERROR | SHIP_ERROR | ui_img_SCREEN_ERROR_png | LCD_Minimal/ui_screen_registry.h |
| SCREEN_SHIP_EXPIRY | SHIP_EXPIRY | (none) | LCD_Minimal/ui_screen_registry.h |
| SCREEN_SHIP_RESULT | SHIP_RESULT | (none) | LCD_Minimal/ui_screen_registry.h |

## Route Table
| op | mode | phase | screen shown | notes |
| --- | --- | --- | --- | --- |
| SCAN | check-in/check-out | CAPTURING | SHIP_HOLD_STILL | |
| SCAN | check-in/check-out | WAITING_INPUT | SHIP_EXPIRY | |
| SCAN | check-in/check-out | DONE | SHIP_LOGGED then HOME | 2s timer |
| SCAN | check-in/check-out | ERROR | SHIP_ERROR then HOME | 3s timer |
| SCAN | discard | CAPTURING | SHIP_HOLD_STILL | |
| SCAN | discard | UPLOAD_STARTING | IGNORED | |
| SCAN | discard | UPLOADING | SHIP_LOGGED then HOME | 2s timer |
| SCAN | discard | ERROR | SHIP_ERROR then HOME | 3s timer |
| SCAN | dish | CAPTURING | SHIP_HOLD_STILL | |
| SCAN | dish | UPLOAD_STARTING/UPLOADING/RESULT_WAITING | SHIP_PROCESSING | |
| SCAN | dish | ERROR | SHIP_ERROR then HOME | 3s timer |

## Expected Flows
### Check-in
1) LCD: HOLD_STILL (CAPTURING)
2) Sense: capture
3) LCD: EXPIRY (WAITING_INPUT)
4) LCD->Sense: INPUT_EXPIRY_DATE
5) LCD: LOGGED for 2s then HOME (on submit)
6) Sense: background presign + upload
7) Sense: error screen after retries if upload fails

### Check-out
1) LCD: HOLD_STILL (CAPTURING)
2) Sense: capture
3) LCD: EXPIRY (WAITING_INPUT)
4) LCD->Sense: INPUT_EXPIRY_DATE
5) LCD: LOGGED for 2s then HOME (on submit)
6) Sense: background presign + upload

### Discard
1) LCD: HOLD_STILL (CAPTURING)
2) Sense: capture
3) LCD: LOGGED for 2s (UPLOADING)
4) Sense: upload continues in background

### Dish log
1) LCD: HOLD_STILL (CAPTURING)
2) Sense: presign + capture
3) LCD: PROCESSING (UPLOAD_STARTING/UPLOADING/RESULT_WAITING)
4) Sense: RESULT_READY
5) LCD: shows "Logged!" (nutrition removed 2026-08-21)
6) LCD: after 10s go HOME then sleep
