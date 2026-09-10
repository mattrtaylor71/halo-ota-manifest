// Stub-include: the single source of truth is ../shared/UartOtaProtocol.cpp
//
// This directory previously held a full COPY. It went stale the moment shared/
// was fixed: the UB in the frame-header parser was corrected in shared/ and the
// LCD kept compiling the broken copy, so a valid 512-byte chunk still parsed as
// data_len=59649. Worse, the local .h lacked the MSG_IMG_* types that
// lcd_sdspool.h gets from shared/ — two different headers for one protocol.
// Same pattern as lcd_bsp.c / esp_lcd_sh8601.c in this directory.
#include "../shared/UartOtaProtocol.cpp"
