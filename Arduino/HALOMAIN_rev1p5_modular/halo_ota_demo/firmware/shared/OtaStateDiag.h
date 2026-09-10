#ifndef OTA_STATE_DIAG_H
#define OTA_STATE_DIAG_H

/**
 * One-time boot diagnostic: print [OTA_STATE_DIAG] lines with:
 * - Running / boot / next-update partition (label, address, size where applicable)
 * - OTA image state for running partition (raw + string: NEW, PENDING_VERIFY, VALID, INVALID, ABORTED)
 * - CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE (if sdkconfig.h available) or "missing (Arduino)"
 *
 * Call once early in setup(), before WiFi begins. Prints even if not provisioned.
 * After OTA apply + reboot, logs clearly show whether we are in PENDING_VERIFY or not.
 */
void otaStateDiagPrint(void);

#endif /* OTA_STATE_DIAG_H */
