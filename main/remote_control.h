#ifndef REMOTE_CONTROL_H
#define REMOTE_CONTROL_H

#include "esp_err.h"
#include "settings_manager.h"
#include "http_client.h"
#include "led_ring.h"
#include "thermal_printer.h"
#include "wifi_manager.h"
#include <stdbool.h>

// RemoteControl "Class" - Polls server for commands
typedef struct RemoteControl_t {
    char server_url[256];
    int poll_interval_ms;
    SettingsManager_t* settings;
    HttpClient_t* http_client;
    LEDRing_t* led_ring;
    ThermalPrinter_t* printer;
    WiFi_t* wifi;
    bool running;

    // Methods
    esp_err_t (*init)(struct RemoteControl_t* self);
    void (*start_polling)(struct RemoteControl_t* self);
    void (*stop_polling)(struct RemoteControl_t* self);
} RemoteControl_t;

// Constructor
RemoteControl_t* remote_control_create(SettingsManager_t* settings, HttpClient_t* http_client, LEDRing_t* led_ring, ThermalPrinter_t* printer, WiFi_t* wifi);

// Destructor
void remote_control_destroy(RemoteControl_t* remote_control);

// Send current settings to server
esp_err_t remote_control_send_current_settings(RemoteControl_t* self);

#endif // REMOTE_CONTROL_H
