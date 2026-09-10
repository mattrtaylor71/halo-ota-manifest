#ifndef HALO_MQTT_DISABLED_CONFIG_H
#define HALO_MQTT_DISABLED_CONFIG_H

#ifndef MQTT_DEV_INSECURE
#define MQTT_DEV_INSECURE 0
#endif

// Tracked configuration matching the qualified build. Client certificate/key are empty.
// MQTT remains disabled; this contains no device credential.
#define MQTT_SECRETS_PRESENT 1

#ifndef MQTT_BROKER_URI
#define MQTT_BROKER_URI "mqtts://arq86ma48kw9j-ats.iot.us-east-1.amazonaws.com"
#endif
#ifndef MQTT_BROKER_PORT
#define MQTT_BROKER_PORT 8883
#endif

extern const char kMqttRootCa[];
extern const char kMqttClientCert[];
extern const char kMqttClientKey[];

#ifndef MQTT_ROOT_CA
#define MQTT_ROOT_CA kMqttRootCa
#endif
#ifndef MQTT_CLIENT_CERT
#define MQTT_CLIENT_CERT kMqttClientCert
#endif
#ifndef MQTT_CLIENT_KEY
#define MQTT_CLIENT_KEY kMqttClientKey
#endif

#endif  // HALO_MQTT_DISABLED_CONFIG_H
