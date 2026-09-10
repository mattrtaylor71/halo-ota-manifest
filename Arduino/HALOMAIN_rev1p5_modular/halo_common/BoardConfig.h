#ifndef HALO_BOARD_CONFIG_H
#define HALO_BOARD_CONFIG_H

#include <Arduino.h>

#if defined(HALO_BOARD_SENSE) && defined(HALO_BOARD_LCD)
#error "Both HALO_BOARD_SENSE and HALO_BOARD_LCD are defined"
#endif

#if !defined(HALO_BOARD_SENSE) && !defined(HALO_BOARD_LCD)
#error "Define HALO_BOARD_SENSE or HALO_BOARD_LCD before including BoardConfig.h"
#endif

#if defined(HALO_BOARD_SENSE)
#define HALO_BOARD_NAME "sense"
#define HALO_WAKE_GPIO GPIO_NUM_2
#define HALO_WAKE_LEVEL 0
#endif

#if defined(HALO_BOARD_LCD)
#define HALO_BOARD_NAME "lcd"
#define HALO_WAKE_GPIO GPIO_NUM_9
#define HALO_WAKE_LEVEL 0
#endif

#define HALO_WAKE_ACTIVE_LEVEL HALO_WAKE_LEVEL
#define HALO_WAKE_INACTIVE_LEVEL ((HALO_WAKE_LEVEL) == 0 ? 1 : 0)

#if defined(HALO_BOARD_SENSE) && (HALO_WAKE_GPIO != GPIO_NUM_2)
#error "Sense wake pin mismatch: expected GPIO_NUM_2"
#endif

#if defined(HALO_BOARD_LCD) && (HALO_WAKE_GPIO != GPIO_NUM_9)
#error "LCD wake pin mismatch: expected GPIO_NUM_9"
#endif

#if (HALO_WAKE_LEVEL != 0) && (HALO_WAKE_LEVEL != 1)
#error "HALO_WAKE_LEVEL must be 0 or 1"
#endif

#endif  // HALO_BOARD_CONFIG_H
