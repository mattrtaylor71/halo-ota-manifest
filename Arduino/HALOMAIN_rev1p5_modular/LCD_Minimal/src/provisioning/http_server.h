#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <Arduino.h>

// Start HTTP server (call when entering AP_SETUP state)
void startHttpServer();

// Stop HTTP server
void stopHttpServer();

// Check if server is running
bool isHttpServerRunning();

#endif // HTTP_SERVER_H

