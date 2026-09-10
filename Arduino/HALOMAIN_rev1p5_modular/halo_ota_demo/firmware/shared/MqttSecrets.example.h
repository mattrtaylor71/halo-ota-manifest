#ifndef HALO_MQTT_SECRETS_EXAMPLE_H
#define HALO_MQTT_SECRETS_EXAMPLE_H

#ifndef MQTT_DEV_INSECURE
#define MQTT_DEV_INSECURE 0
#endif

// Example template (no real secrets)
#define MQTT_SECRETS_PRESENT 0

#ifndef MQTT_BROKER_URI
#define MQTT_BROKER_URI "mqtts://YOUR_ENDPOINT_HERE"
#endif
#ifndef MQTT_BROKER_PORT
#define MQTT_BROKER_PORT 8883
#endif
#ifndef MQTT_ALT_BROKER_URI
#define MQTT_ALT_BROKER_URI MQTT_BROKER_URI
#endif
#ifndef MQTT_ALT_BROKER_PORT
#define MQTT_ALT_BROKER_PORT 443
#endif
#ifndef MQTT_ALT_ALPN
#define MQTT_ALT_ALPN "x-amzn-mqtt-ca"
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

#endif  // HALO_MQTT_SECRETS_EXAMPLE_H
