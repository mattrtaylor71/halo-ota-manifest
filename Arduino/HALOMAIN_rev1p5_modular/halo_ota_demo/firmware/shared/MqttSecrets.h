#ifndef HALO_MQTT_SECRETS_H
#define HALO_MQTT_SECRETS_H

// Development overrides remain local. Canonical production builds reject them
// and use the tracked configuration of the qualified MQTT-disabled build.
#if __has_include("MqttSecrets.local.h")
#include "MqttSecrets.local.h"
#else
#include "MqttDisabledConfig.h"
#endif

#endif  // HALO_MQTT_SECRETS_H
