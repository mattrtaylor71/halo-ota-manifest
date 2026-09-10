#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <WiFi.h>
#include <Arduino.h>

/**
 * WifiManager: Wi-Fi connection management with bounded retries and explicit timeouts.
 * 
 * Phase 1: Non-reentrant state machine for reliable Wi-Fi connection.
 * - No blocking forever: all operations have explicit timeouts
 * - Bounded retries with configurable delays
 * - Status reporting for connection state
 * - Prevents calling WiFi.begin() while connection in progress
 */
class WifiManager {
public:
  enum Status {
    STATUS_IDLE,           // Not attempting connection
    STATUS_CONNECTING,     // Connection attempt in progress
    STATUS_CONNECTED,      // Successfully connected
    STATUS_FAILED          // Failed after all retries
  };
  
  enum ConnectResult {
    CONNECT_RESULT_IN_PROGRESS,  // Connection attempt already in progress
    CONNECT_RESULT_STARTED       // Connection attempt started
  };
  
  WifiManager();
  ~WifiManager();
  
  // Connect to Wi-Fi network (non-reentrant)
  // Returns CONNECT_RESULT_STARTED if attempt started, CONNECT_RESULT_IN_PROGRESS if already connecting
  // timeout_ms: maximum time to wait for connection (per attempt)
  // max_retries: maximum number of connection attempts
  // retry_delay_ms: delay between retries
  ConnectResult connect(const char* ssid, const char* password, 
                        unsigned long timeout_ms = 10000, 
                        uint8_t max_retries = 3,
                        unsigned long retry_delay_ms = 2000);
  
  // Disconnect from Wi-Fi
  void disconnect();
  
  // Check if connected
  bool isConnected() const;
  
  // Get current status
  Status getStatus() const { return status; }
  
  // Get SSID of connected network (or NULL if not connected)
  const char* getSSID() const;
  
  // Get IP address (or 0.0.0.0 if not connected)
  IPAddress getIP() const;
  
  // Update (call from loop() to check connection state)
  // This implements the state machine: IDLE -> START_ATTEMPT -> WAIT -> SUCCESS | FAIL
  void update();

  // Total budget for all retries + backoff + safety margin.
  unsigned long getBudgetMs(unsigned long safety_margin_ms = 0) const;

private:
  enum InternalState {
    STATE_IDLE,            // Not attempting connection
    STATE_START_ATTEMPT,    // About to call WiFi.begin()
    STATE_WAIT,            // Waiting for connection (checking WiFi.status())
    STATE_SUCCESS,         // Connected successfully
    STATE_FAIL,            // Current attempt failed (will retry or give up)
    STATE_BACKOFF          // Waiting before next retry attempt
  };
  
  Status status;           // Public status
  InternalState state;     // Internal state machine state
  String ssid;
  String password;
  unsigned long attempt_start_ms;  // When current attempt started
  unsigned long timeout_ms;
  uint8_t max_retries;
  uint8_t current_retry;
  unsigned long retry_delay_ms;
  unsigned long backoff_until_ms;   // When backoff period ends
  
  void setStatus(Status new_status);
  void transitionTo(InternalState new_state);
};

#endif // WIFI_MANAGER_H
