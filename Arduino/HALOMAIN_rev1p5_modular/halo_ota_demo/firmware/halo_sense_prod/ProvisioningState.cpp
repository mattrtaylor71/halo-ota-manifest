// Stub-include: the single source of truth for this file is ../shared/ProvisioningState.cpp
//
// Arduino only compiles .cpp files that live in the sketch directory, so this
// directory previously held a full COPY of the shared implementation. The two
// drifted — shared/MqttClient.cpp still had MQTT enabled where the shipping
// copy had it disabled, and shared/Truth.h was missing a declaration the
// shipping Truth.cpp relied on. Editing the wrong copy was a silent no-op.
// Same pattern already used by lcd_bsp.c / esp_lcd_sh8601.c in halo_lcd_prod.
#include "../shared/ProvisioningState.cpp"
