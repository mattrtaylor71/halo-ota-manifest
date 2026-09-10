// Stub-include: the single source of truth is ../shared/UartOtaProtocol.h
//
// This was a stale COPY carrying the SAME include guard (UART_OTA_PROTOCOL_H)
// as the shared header. Two headers, one guard: whichever is included first
// wins and silently suppresses the other. The copy lacked the MSG_IMG_* types,
// so a translation unit that reached this file first would see the image-spool
// constants vanish with no error at the point of the mistake. The .cpp beside
// it was already a stub; only the header was left behind.
#include "../shared/UartOtaProtocol.h"
