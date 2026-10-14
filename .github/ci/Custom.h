#ifndef ESPEASY_CUSTOM_H
#define ESPEASY_CUSTOM_H

// Minimal ESPEasy build used by CI to check that P251 compiles (and to provide a test firmware).

#define CONTROLLER_SET_NONE
#define NOTIFIER_SET_NONE
#define PLUGIN_SET_NONE

#define USES_P001   // Switch input
#define USES_P026   // SysInfo
#define USES_P033   // Dummy device
#define USES_P251   // Security - Fingerprint (Grow R503/R307)

#define USES_C005   // Home Assistant (openHAB) MQTT
#define USES_C008   // Generic HTTP

#endif // ESPEASY_CUSTOM_H
