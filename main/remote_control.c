#include "remote_control.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include <esp_timer.h>
#include "cJSON.h"
#include "main_menu.h"
#include "main.h"
#include "ota_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "REMOTE_CONTROL";

// HTTP response buffer
#define MAX_HTTP_OUTPUT_BUFFER 2048

// Task handle for polling
static TaskHandle_t polling_task_handle = NULL;

// Track last processed OTA commands to avoid spam
static int64_t last_ota_check_time = 0;
static int64_t last_ota_update_time = 0;
#define OTA_COMMAND_COOLDOWN_MS 65000  // 65 seconds cooldown between OTA commands

// Helper function to send acknowledgment to Flask server
static void send_ota_acknowledgment(RemoteControl_t *self)
{
    char ack_url[300];
    snprintf(ack_url, sizeof(ack_url), "%s/api/ota/ack", self->server_url);
    
    esp_http_client_config_t config = {
        .url = ack_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 3000,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client)
    {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_err_t err = esp_http_client_perform(client);
        
        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "Sent acknowledgment to server");
        }
        else
        {
            ESP_LOGW(TAG, "Failed to send acknowledgment: %s", esp_err_to_name(err));
        }
        
        esp_http_client_cleanup(client);
    }
}

// Polling task
static void polling_task(void *arg)
{
    RemoteControl_t *self = (RemoteControl_t *)arg;
    char response_buffer[MAX_HTTP_OUTPUT_BUFFER];

    ESP_LOGI(TAG, "Polling task started (interval: %d ms)", self->poll_interval_ms);

    bool was_waiting_for_wifi = false;
    while (self->running)
    {
        // Skip polling until WiFi is connected to avoid flooding logs with
        // "Host is unreachable" errors during boot / reconnect.
        if (self->wifi && !self->wifi->connected)
        {
            if (!was_waiting_for_wifi)
            {
                ESP_LOGI(TAG, "Waiting for WiFi before polling server...");
                was_waiting_for_wifi = true;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (was_waiting_for_wifi)
        {
            ESP_LOGI(TAG, "WiFi connected, resuming polling");
            was_waiting_for_wifi = false;
        }

        // Check for capture command
        char command_url[300];
        snprintf(command_url, sizeof(command_url), "%s/api/command", self->server_url);

        esp_http_client_config_t config = {
            .url = command_url,
            .method = HTTP_METHOD_GET,
            .timeout_ms = 5000,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client)
        {
            memset(response_buffer, 0, sizeof(response_buffer));
            esp_http_client_set_header(client, "Content-Type", "application/json");

            esp_err_t err = esp_http_client_open(client, 0);
            if (err == ESP_OK)
            {
                esp_http_client_fetch_headers(client);
                int data_read = esp_http_client_read_response(client, response_buffer, MAX_HTTP_OUTPUT_BUFFER - 1);

                if (data_read >= 0)
                {
                    response_buffer[data_read] = '\0';
                    int status_code = esp_http_client_get_status_code(client);

                    if (status_code == 200)
                    {
                        // Parse JSON response
                        cJSON *root = cJSON_Parse(response_buffer);
                        if (root)
                        {
                            cJSON *command = cJSON_GetObjectItem(root, "command");
                            if (command && cJSON_IsString(command))
                            {
                                // Only log non-"none" commands
                                if (strcmp(command->valuestring, "none") != 0)
                                {
                                    ESP_LOGI(TAG, "Command received: '%s'", command->valuestring);
                                }
                                
                                if (strcmp(command->valuestring, "capture") == 0)
                                {
                                    ESP_LOGI(TAG, "Capture command received from server");
                                    self->http_client->capture_and_upload(self->http_client);
                                }
                                else if (strcmp(command->valuestring, "print") == 0)
                                {
                                    ESP_LOGI(TAG, "Print command received from server");
                                    
                                    // Stop poem loading animation first (whether printer is available or not)
                                    stop_poem_loading_animation();
                                    
                                    // Check if printer is available
                                    if (self->printer && self->printer->initialized)
                                    {
                                        ESP_LOGI(TAG, "Printer is available and initialized");
                                        
                                        // Extract print data
                                        cJSON *print_data = cJSON_GetObjectItem(root, "print_data");
                                        if (print_data && cJSON_IsObject(print_data))
                                        {
                                            cJSON *title = cJSON_GetObjectItem(print_data, "title");
                                            cJSON *poet_style = cJSON_GetObjectItem(print_data, "poet_style");
                                            cJSON *poem_text = cJSON_GetObjectItem(print_data, "poem_text");
                                            
                                            const char *title_str = (title && cJSON_IsString(title)) ? title->valuestring : "Untitled";
                                            const char *poet_str = (poet_style && cJSON_IsString(poet_style)) ? poet_style->valuestring : "General";
                                            const char *poem_str = (poem_text && cJSON_IsString(poem_text)) ? poem_text->valuestring : "";
                                            
                                            ESP_LOGI(TAG, "=== RECEIVED POEM ===");
                                            ESP_LOGI(TAG, "Title: %s", title_str);
                                            ESP_LOGI(TAG, "Style: %s", poet_str);
                                            ESP_LOGI(TAG, "Poem Text:");
                                            ESP_LOGI(TAG, "%s", poem_str);
                                            ESP_LOGI(TAG, "====================");
                                            
                                            thermal_printer_print_poem(self->printer, title_str, poet_str, poem_str);
                                        }
                                        else
                                        {
                                            ESP_LOGW(TAG, "Print command received but no print_data found");
                                            
                                            // Flash red to indicate error (no print data)
                                            if (self->led_ring)
                                            {
                                                int led_count = self->led_ring->num_leds;
                                                
                                                // Quick red flash 3 times
                                                for (int flash = 0; flash < 3; flash++)
                                                {
                                                    // Flash on
                                                    for (int i = 0; i < led_count; i++)
                                                    {
                                                        self->led_ring->set_pixel(self->led_ring, i, 255, 0, 0);
                                                    }
                                                    self->led_ring->refresh(self->led_ring);
                                                    vTaskDelay(pdMS_TO_TICKS(200));
                                                    
                                                    // Flash off
                                                    self->led_ring->clear(self->led_ring);
                                                    self->led_ring->refresh(self->led_ring);
                                                    vTaskDelay(pdMS_TO_TICKS(200));
                                                }
                                            }
                                        }
                                    }
                                    else
                                    {
                                        ESP_LOGW(TAG, "Print command received but printer not available (printer=%p, initialized=%d)", 
                                                 self->printer, self->printer ? self->printer->initialized : 0);
                                        
                                        // Flash red to indicate printer error
                                        if (self->led_ring)
                                        {
                                            int led_count = self->led_ring->num_leds;
                                            
                                            // Quick red flash 3 times
                                            for (int flash = 0; flash < 3; flash++)
                                            {
                                                // Flash on - bright red
                                                for (int i = 0; i < led_count; i++)
                                                {
                                                    self->led_ring->set_pixel(self->led_ring, i, 255, 0, 0);
                                                }
                                                self->led_ring->refresh(self->led_ring);
                                                vTaskDelay(pdMS_TO_TICKS(200));
                                                
                                                // Flash off
                                                self->led_ring->clear(self->led_ring);
                                                self->led_ring->refresh(self->led_ring);
                                                vTaskDelay(pdMS_TO_TICKS(200));
                                            }
                                        }
                                    }
                                }
                                else if (strcmp(command->valuestring, "ota_check") == 0)
                                {
                                    // Check cooldown to avoid spam
                                    int64_t now = esp_timer_get_time() / 1000; // Convert to ms
                                    if (last_ota_check_time > 0 && (now - last_ota_check_time) < OTA_COMMAND_COOLDOWN_MS)
                                    {
                                        ESP_LOGW(TAG, "OTA check command ignored (cooldown: %lld seconds remaining)", 
                                                (OTA_COMMAND_COOLDOWN_MS - (now - last_ota_check_time)) / 1000);
                                    }
                                    else
                                    {
                                        last_ota_check_time = now;
                                        ESP_LOGI(TAG, "OTA check command received from server");
                                        
                                        // Send acknowledgment
                                        send_ota_acknowledgment(self);
                                        
                                        if (g_ota_manager && g_ota_manager->initialized)
                                        {
                                            // Just check for updates, don't install
                                            bool update_available = false;
                                            if (g_ota_manager->check_for_update(g_ota_manager, &update_available) == ESP_OK)
                                            {
                                                char current[64], latest[64];
                                                g_ota_manager->get_current_version(g_ota_manager, current);
                                                g_ota_manager->get_latest_version(g_ota_manager, latest);
                                                
                                                if (update_available)
                                                {
                                                    ESP_LOGI(TAG, "Update available: %s -> %s", current, latest);
                                                }
                                                else
                                                {
                                                    ESP_LOGI(TAG, "Firmware is up to date: %s", current);
                                                }
                                            }
                                            else
                                            {
                                                ESP_LOGW(TAG, "Check skipped (rate limited or failed)");
                                            }
                                        }
                                        else
                                        {
                                            ESP_LOGE(TAG, "OTA manager not available");
                                        }
                                    }
                                }
                                else if (strcmp(command->valuestring, "ota_update") == 0)
                                {
                                    // Check cooldown to avoid spam
                                    int64_t now = esp_timer_get_time() / 1000; // Convert to ms
                                    if (last_ota_update_time > 0 && (now - last_ota_update_time) < OTA_COMMAND_COOLDOWN_MS)
                                    {
                                        ESP_LOGW(TAG, "OTA update command ignored (cooldown: %lld seconds remaining)", 
                                                (OTA_COMMAND_COOLDOWN_MS - (now - last_ota_update_time)) / 1000);
                                    }
                                    else
                                    {
                                        last_ota_update_time = now;
                                        ESP_LOGI(TAG, "OTA update command received from server");
                                        
                                        // Send acknowledgment
                                        send_ota_acknowledgment(self);
                                        
                                        if (g_ota_manager && g_ota_manager->initialized)
                                        {
                                            // Check for updates
                                            bool update_available = false;
                                            if (g_ota_manager->check_for_update(g_ota_manager, &update_available) == ESP_OK)
                                            {
                                                if (update_available)
                                                {
                                                    char latest[64];
                                                    g_ota_manager->get_latest_version(g_ota_manager, latest);
                                                    ESP_LOGI(TAG, "Update available: %s", latest);
                                                    
                                                    // Perform update (checks, downloads, and installs)
                                                    if (g_ota_manager->perform_update(g_ota_manager) == ESP_OK)
                                                    {
                                                        ESP_LOGI(TAG, "Update successful, rebooting...");
                                                        vTaskDelay(pdMS_TO_TICKS(1000));
                                                        esp_restart();
                                                    }
                                                    else
                                                    {
                                                        ESP_LOGE(TAG, "Update failed");
                                                    }
                                                }
                                                else
                                                {
                                                    ESP_LOGI(TAG, "Firmware is already up to date");
                                                }
                                            }
                                            else
                                            {
                                                ESP_LOGW(TAG, "Update check skipped (rate limited or failed)");
                                            }
                                        }
                                        else
                                        {
                                            ESP_LOGE(TAG, "OTA manager not available");
                                        }
                                    }
                                }
                                else if (strcmp(command->valuestring, "ota_update_custom") == 0)
                                {
                                    // Check cooldown to avoid spam
                                    int64_t now = esp_timer_get_time() / 1000; // Convert to ms
                                    if (last_ota_update_time > 0 && (now - last_ota_update_time) < OTA_COMMAND_COOLDOWN_MS)
                                    {
                                        ESP_LOGW(TAG, "OTA custom update command ignored (cooldown: %lld seconds remaining)", 
                                                (OTA_COMMAND_COOLDOWN_MS - (now - last_ota_update_time)) / 1000);
                                    }
                                    else
                                    {
                                        last_ota_update_time = now;
                                        ESP_LOGI(TAG, "OTA custom update command received from server");
                                        
                                        // Send acknowledgment
                                        send_ota_acknowledgment(self);
                                        
                                        // Build custom firmware URL
                                        char firmware_url[300];
                                        snprintf(firmware_url, sizeof(firmware_url), "%s/firmware/custom_firmware.bin", self->server_url);
                                        
                                        ESP_LOGI(TAG, "Starting custom firmware update from: %s", firmware_url);
                                        
                                        // Perform OTA update directly from custom URL
                                        esp_http_client_config_t ota_config = {
                                            .url = firmware_url,
                                            .timeout_ms = 30000,
                                            .crt_bundle_attach = esp_crt_bundle_attach,
                                        };
                                        
                                        esp_https_ota_config_t https_ota_config = {
                                            .http_config = &ota_config,
                                        };
                                        
                                        esp_err_t ret = esp_https_ota(&https_ota_config);
                                        if (ret == ESP_OK)
                                        {
                                            ESP_LOGI(TAG, "Custom firmware update successful, rebooting...");
                                            vTaskDelay(pdMS_TO_TICKS(1000));
                                            esp_restart();
                                        }
                                        else
                                        {
                                            ESP_LOGE(TAG, "Custom firmware update failed: %s", esp_err_to_name(ret));
                                        }
                                    }
                                }
                                else if (strcmp(command->valuestring, "ota_update_target") == 0)
                                {
                                    // No cooldown for forced version selection - user explicitly chose this version
                                    last_ota_update_time = esp_timer_get_time() / 1000; // Update timestamp
                                    
                                    // Get target version from settings
                                    cJSON *settings = cJSON_GetObjectItem(root, "settings");
                                    if (settings && cJSON_HasObjectItem(settings, "ota"))
                                        {
                                            cJSON *ota_obj = cJSON_GetObjectItem(settings, "ota");
                                            if (ota_obj && cJSON_HasObjectItem(ota_obj, "target_version"))
                                            {
                                                cJSON *target_version = cJSON_GetObjectItem(ota_obj, "target_version");
                                                if (target_version && cJSON_IsString(target_version) && target_version->valuestring)
                                                {
                                                    ESP_LOGI(TAG, "OTA target version update command received: %s", target_version->valuestring);
                                                    
                                                    // Send acknowledgment
                                                    send_ota_acknowledgment(self);
                                                    
                                                    if (g_ota_manager && g_ota_manager->initialized)
                                                    {
                                                        // Install target version directly (force update, no version checking)
                                                        if (g_ota_manager->install_target_version(g_ota_manager, target_version->valuestring) == ESP_OK)
                                                        {
                                                            ESP_LOGI(TAG, "Target version update successful, rebooting...");
                                                            vTaskDelay(pdMS_TO_TICKS(1000));
                                                            esp_restart();
                                                        }
                                                        else
                                                        {
                                                            ESP_LOGE(TAG, "Target version update failed");
                                                        }
                                                    }
                                                    else
                                                    {
                                                        ESP_LOGE(TAG, "OTA manager not available");
                                                    }
                                                }
                                            }
                                            else
                                            {
                                                ESP_LOGW(TAG, "No target_version in OTA settings");
                                            }
                                        }
                                        else
                                        {
                                            ESP_LOGW(TAG, "No settings or OTA settings in command");
                                        }
                                }
                                else if (strcmp(command->valuestring, "none") == 0)
                                {
                                    // No command, continue polling (don't log to reduce spam)
                                }
                                else
                                {
                                    ESP_LOGW(TAG, "Unknown command received: '%s'", command->valuestring);
                                }
                            }
                            else
                            {
                                ESP_LOGW(TAG, "No command field in response or not a string");
                            }

                            // Check for settings updates
                            cJSON *settings = cJSON_GetObjectItem(root, "settings");
                            if (settings && cJSON_IsObject(settings))
                            {
                                ESP_LOGI(TAG, "Settings update received from server");
                                bool settings_changed = false;

                                // Update camera settings (modify in memory, don't save yet)
                                cJSON *camera = cJSON_GetObjectItem(settings, "camera");
                                if (camera)
                                {
                                    cJSON *framesize = cJSON_GetObjectItem(camera, "framesize");
                                    if (framesize && cJSON_IsNumber(framesize))
                                    {
                                        self->settings->settings.camera_framesize = framesize->valueint;
                                        settings_changed = true;
                                    }

                                    cJSON *quality = cJSON_GetObjectItem(camera, "quality");
                                    if (quality && cJSON_IsNumber(quality))
                                    {
                                        if (quality->valueint <= 63)
                                        {
                                            self->settings->settings.camera_quality = quality->valueint;
                                            settings_changed = true;
                                        }
                                    }

                                    cJSON *brightness = cJSON_GetObjectItem(camera, "brightness");
                                    if (brightness && cJSON_IsNumber(brightness))
                                    {
                                        if (brightness->valueint >= -2 && brightness->valueint <= 2)
                                        {
                                            self->settings->settings.camera_brightness = brightness->valueint;
                                            settings_changed = true;
                                        }
                                    }

                                    // Handle flip settings. `vflip`/`hmirror` is
                                    // the wire name the web settings page uses;
                                    // `flip_vertical`/`flip_horizontal` is the
                                    // canonical name in settings.json and the
                                    // Flutter UI. Accept both.
                                    cJSON *vflip = cJSON_GetObjectItem(camera, "vflip");
                                    if (!vflip)
                                        vflip = cJSON_GetObjectItem(camera, "flip_vertical");
                                    cJSON *hmirror = cJSON_GetObjectItem(camera, "hmirror");
                                    if (!hmirror)
                                        hmirror = cJSON_GetObjectItem(camera, "flip_horizontal");
                                    if ((vflip && cJSON_IsBool(vflip)) || (hmirror && cJSON_IsBool(hmirror)))
                                    {
                                        if (hmirror && cJSON_IsBool(hmirror))
                                        {
                                            self->settings->settings.camera_flip_h = cJSON_IsTrue(hmirror);
                                            settings_changed = true;
                                        }
                                        if (vflip && cJSON_IsBool(vflip))
                                        {
                                            self->settings->settings.camera_flip_v = cJSON_IsTrue(vflip);
                                            settings_changed = true;
                                        }
                                    }

                                    // Handle rotation setting
                                    cJSON *rotation = cJSON_GetObjectItem(camera, "rotation");
                                    if (rotation && cJSON_IsNumber(rotation))
                                    {
                                        int rot = rotation->valueint;
                                        if (rot == 0 || rot == 90 || rot == 180 || rot == 270)
                                        {
                                            self->settings->settings.camera_rotation = rot;
                                            settings_changed = true;
                                            ESP_LOGI(TAG, "Camera rotation updated to %d°", rot);
                                        }
                                    }

                                }

                                // Capture-feature booleans. settings.json keeps
                                // these under "camera_features" while the web
                                // settings page pushes them inside "camera" —
                                // read either, so a client that sends only the
                                // canonical section (the Flutter app) is not
                                // silently ignored.
                                {
                                    cJSON *features = cJSON_GetObjectItem(settings, "camera_features");

                                    cJSON *flash_enabled = features ? cJSON_GetObjectItem(features, "flash_enabled") : NULL;
                                    if (!flash_enabled && camera)
                                        flash_enabled = cJSON_GetObjectItem(camera, "flash_enabled");
                                    if (flash_enabled && cJSON_IsBool(flash_enabled))
                                    {
                                        self->settings->settings.flash_enabled = cJSON_IsTrue(flash_enabled);
                                        settings_changed = true;
                                        ESP_LOGI(TAG, "Flash enabled updated to %s", self->settings->settings.flash_enabled ? "true" : "false");
                                    }

                                    cJSON *self_timer_enabled = features ? cJSON_GetObjectItem(features, "self_timer_enabled") : NULL;
                                    if (!self_timer_enabled && camera)
                                        self_timer_enabled = cJSON_GetObjectItem(camera, "self_timer_enabled");
                                    if (self_timer_enabled && cJSON_IsBool(self_timer_enabled))
                                    {
                                        self->settings->settings.self_timer_enabled = cJSON_IsTrue(self_timer_enabled);
                                        settings_changed = true;
                                        ESP_LOGI(TAG, "Self-timer enabled updated to %s", self->settings->settings.self_timer_enabled ? "true" : "false");
                                    }

                                    cJSON *auto_print_enabled = features ? cJSON_GetObjectItem(features, "auto_print_enabled") : NULL;
                                    if (!auto_print_enabled && camera)
                                        auto_print_enabled = cJSON_GetObjectItem(camera, "auto_print_enabled");
                                    if (auto_print_enabled && cJSON_IsBool(auto_print_enabled))
                                    {
                                        self->settings->settings.auto_print_enabled = cJSON_IsTrue(auto_print_enabled);
                                        settings_changed = true;
                                        ESP_LOGI(TAG, "Auto-print enabled updated to %s", self->settings->settings.auto_print_enabled ? "true" : "false");
                                    }
                                }

                                // Update poem settings (modify in memory, don't save yet)
                                cJSON *poem = cJSON_GetObjectItem(settings, "poem");
                                if (poem)
                                {
                                    cJSON *poem_style = cJSON_GetObjectItem(poem, "style");
                                    if (poem_style && cJSON_IsString(poem_style))
                                    {
                                        strncpy(self->settings->settings.poem_style, poem_style->valuestring, sizeof(self->settings->settings.poem_style) - 1);
                                        self->settings->settings.poem_style[sizeof(self->settings->settings.poem_style) - 1] = '\0';
                                        settings_changed = true;
                                        ESP_LOGI(TAG, "Poem style updated to: %s", self->settings->settings.poem_style);
                                    }
                                }

                                // Update server settings (modify in memory, don't save yet)
                                cJSON *server = cJSON_GetObjectItem(settings, "server");
                                if (server)
                                {
                                    cJSON *upload_interval = cJSON_GetObjectItem(server, "upload_interval_seconds");
                                    if (upload_interval && cJSON_IsNumber(upload_interval))
                                    {
                                        self->settings->settings.server_upload_interval = upload_interval->valueint;
                                        settings_changed = true;
                                    }

                                    cJSON *poll_interval = cJSON_GetObjectItem(server, "poll_interval_ms");
                                    if (poll_interval && cJSON_IsNumber(poll_interval))
                                    {
                                        if (poll_interval->valueint >= 50 && poll_interval->valueint <= 10000)
                                        {
                                            self->settings->settings.server_poll_interval = poll_interval->valueint;
                                            self->poll_interval_ms = poll_interval->valueint;
                                            settings_changed = true;
                                            ESP_LOGI(TAG, "Poll interval updated to %d ms", self->poll_interval_ms);
                                        }
                                    }
                                }

                                // Update log settings
                                cJSON *log = cJSON_GetObjectItem(settings, "log");
                                if (log)
                                {
                                    cJSON *upload_enabled = cJSON_GetObjectItem(log, "upload_enabled");
                                    if (upload_enabled)
                                    {
                                        self->settings->settings.log_upload_enabled = cJSON_IsTrue(upload_enabled);
                                        settings_changed = true;
                                        ESP_LOGI(TAG, "Log upload %s", self->settings->settings.log_upload_enabled ? "enabled" : "disabled");
                                    }

                                    cJSON *upload_interval = cJSON_GetObjectItem(log, "upload_interval_seconds");
                                    if (upload_interval && cJSON_IsNumber(upload_interval))
                                    {
                                        if (upload_interval->valueint >= 10 && upload_interval->valueint <= 300)
                                        {
                                            self->settings->settings.log_upload_interval = upload_interval->valueint;
                                            settings_changed = true;
                                            ESP_LOGI(TAG, "Log upload interval updated to %d seconds", self->settings->settings.log_upload_interval);
                                        }
                                    }

                                    cJSON *queue_size = cJSON_GetObjectItem(log, "queue_size");
                                    if (queue_size && cJSON_IsNumber(queue_size))
                                    {
                                        if (queue_size->valueint >= 10 && queue_size->valueint <= 1000)
                                        {
                                            self->settings->settings.log_queue_size = queue_size->valueint;
                                            settings_changed = true;
                                            ESP_LOGI(TAG, "Log queue size updated to %d (restart required)", self->settings->settings.log_queue_size);
                                        }
                                    }
                                }

                                // Update thermal printer heating parameters live.
                                // Pin/UART/baud changes are not applied at runtime
                                // because the UART driver is already bound — those
                                // require a reboot.
                                cJSON *printer_settings = cJSON_GetObjectItem(settings, "thermal_printer");
                                if (printer_settings)
                                {
                                    cJSON *max_dots = cJSON_GetObjectItem(printer_settings, "max_heating_dots");
                                    if (max_dots && cJSON_IsNumber(max_dots) &&
                                        max_dots->valueint >= 0 && max_dots->valueint <= 255)
                                    {
                                        self->settings->settings.printer_max_heating_dots = (uint8_t)max_dots->valueint;
                                        settings_changed = true;
                                        ESP_LOGI(TAG, "Printer max_heating_dots updated to %d", max_dots->valueint);
                                    }

                                    cJSON *heat_time = cJSON_GetObjectItem(printer_settings, "heating_time");
                                    if (heat_time && cJSON_IsNumber(heat_time) &&
                                        heat_time->valueint >= 3 && heat_time->valueint <= 255)
                                    {
                                        self->settings->settings.printer_heating_time = (uint8_t)heat_time->valueint;
                                        settings_changed = true;
                                        ESP_LOGI(TAG, "Printer heating_time updated to %d (%dµs)", heat_time->valueint, heat_time->valueint * 10);
                                    }

                                    cJSON *heat_interval = cJSON_GetObjectItem(printer_settings, "heating_interval");
                                    if (heat_interval && cJSON_IsNumber(heat_interval) &&
                                        heat_interval->valueint >= 0 && heat_interval->valueint <= 255)
                                    {
                                        self->settings->settings.printer_heating_interval = (uint8_t)heat_interval->valueint;
                                        settings_changed = true;
                                        ESP_LOGI(TAG, "Printer heating_interval updated to %d (%dµs)", heat_interval->valueint, heat_interval->valueint * 10);
                                    }

                                    cJSON *density = cJSON_GetObjectItem(printer_settings, "density");
                                    if (density && cJSON_IsNumber(density) &&
                                        density->valueint >= 0 && density->valueint <= 31)
                                    {
                                        self->settings->settings.printer_density = (uint8_t)density->valueint;
                                        settings_changed = true;
                                        ESP_LOGI(TAG, "Printer density updated to %d (%d%%)", density->valueint, 50 + 5 * density->valueint);
                                    }

                                    cJSON *break_time = cJSON_GetObjectItem(printer_settings, "break_time");
                                    if (break_time && cJSON_IsNumber(break_time) &&
                                        break_time->valueint >= 0 && break_time->valueint <= 7)
                                    {
                                        self->settings->settings.printer_break_time = (uint8_t)break_time->valueint;
                                        settings_changed = true;
                                        ESP_LOGI(TAG, "Printer break_time updated to %d (%dµs)", break_time->valueint, break_time->valueint * 250);
                                    }

                                    cJSON *bold = cJSON_GetObjectItem(printer_settings, "bold");
                                    if (bold && cJSON_IsBool(bold))
                                    {
                                        self->settings->settings.printer_bold = cJSON_IsTrue(bold);
                                        settings_changed = true;
                                        ESP_LOGI(TAG, "Printer bold updated to %s", cJSON_IsTrue(bold) ? "ON" : "OFF");
                                    }

                                    cJSON *double_strike = cJSON_GetObjectItem(printer_settings, "double_strike");
                                    if (double_strike && cJSON_IsBool(double_strike))
                                    {
                                        self->settings->settings.printer_double_strike = cJSON_IsTrue(double_strike);
                                        settings_changed = true;
                                        ESP_LOGI(TAG, "Printer double_strike updated to %s", cJSON_IsTrue(double_strike) ? "ON" : "OFF");
                                    }

                                    cJSON *heating_cmds = cJSON_GetObjectItem(printer_settings, "heating_commands");
                                    if (heating_cmds && cJSON_IsBool(heating_cmds))
                                    {
                                        self->settings->settings.printer_heating_commands = cJSON_IsTrue(heating_cmds);
                                        if (self->printer)
                                        {
                                            self->printer->config.heating_commands =
                                                self->settings->settings.printer_heating_commands;
                                        }
                                        settings_changed = true;
                                        ESP_LOGI(TAG, "Printer heating_commands updated to %s", cJSON_IsTrue(heating_cmds) ? "ON" : "OFF");
                                    }

                                    cJSON *line_delay = cJSON_GetObjectItem(printer_settings, "line_delay_ms");
                                    if (line_delay && cJSON_IsNumber(line_delay) &&
                                        line_delay->valueint >= 100 && line_delay->valueint <= 3000)
                                    {
                                        self->settings->settings.printer_line_delay_ms = (uint16_t)line_delay->valueint;
                                        if (self->printer)
                                        {
                                            // print_line reads this from the cached config on
                                            // every line, so it applies to the next print.
                                            self->printer->config.line_delay_ms =
                                                self->settings->settings.printer_line_delay_ms;
                                        }
                                        settings_changed = true;
                                        ESP_LOGI(TAG, "Printer line_delay_ms updated to %d", line_delay->valueint);
                                    }

                                    // Apply immediately so the next print uses the new values.
                                    // Gated: on a printer that doesn't implement ESC 7 / DC2 #,
                                    // every settings push would otherwise spray the argument
                                    // bytes into the line buffer, to surface as junk ahead of
                                    // the next print.
                                    if (self->printer && self->printer->initialized &&
                                        self->printer->config.heating_commands)
                                    {
                                        self->printer->set_print_density(
                                            self->printer,
                                            self->settings->settings.printer_max_heating_dots,
                                            self->settings->settings.printer_heating_time,
                                            self->settings->settings.printer_heating_interval);
                                        self->printer->set_density(
                                            self->printer,
                                            self->settings->settings.printer_density,
                                            self->settings->settings.printer_break_time);
                                    }
                                    if (self->printer && self->printer->initialized)
                                    {
                                        // Update the cached config too — print_poem restores
                                        // bold from it after the title, so a stale value there
                                        // would undo this on the very next print.
                                        self->printer->config.bold = self->settings->settings.printer_bold;
                                        self->printer->config.double_strike = self->settings->settings.printer_double_strike;
                                        self->printer->set_bold(self->printer, self->printer->config.bold);
                                        self->printer->set_double_strike(self->printer, self->printer->config.double_strike);
                                    }
                                }

                                // Update LED ring settings
                                cJSON *led_ring = cJSON_GetObjectItem(settings, "led_ring");
                                if (led_ring)
                                {
                                    ESP_LOGI(TAG, "LED ring settings detected in update");
                                    
                                    cJSON *brightness = cJSON_GetObjectItem(led_ring, "brightness");
                                    if (brightness && cJSON_IsNumber(brightness))
                                    {
                                        uint8_t new_brightness = brightness->valueint;
                                        if (new_brightness <= 100)
                                        {
                                            self->settings->settings.led_ring_brightness = new_brightness;
                                            settings_changed = true;
                                            
                                            // Update LED ring brightness immediately
                                            if (self->led_ring)
                                            {
                                                self->led_ring->set_brightness(self->led_ring, new_brightness);
                                                // Refresh the menu display to show new brightness
                                                refresh_led_ring_menu();
                                                ESP_LOGI(TAG, "✓ LED ring brightness updated to %d%% and menu refreshed", new_brightness);
                                            }
                                        }
                                    }

                                    cJSON *count = cJSON_GetObjectItem(led_ring, "count");
                                    if (count && cJSON_IsNumber(count))
                                    {
                                        uint8_t new_count = count->valueint;
                                        if (new_count >= 1)
                                        {
                                            self->settings->settings.led_ring_count = new_count;
                                            settings_changed = true;
                                            ESP_LOGI(TAG, "✓ LED ring count updated to %d (restart required)", new_count);
                                        }
                                    }
                                }

                                // Save all settings once if anything changed
                                if (settings_changed)
                                {
                                    self->settings->save_settings(self->settings);
                                    
                                    // Send updated settings back to server
                                    remote_control_send_current_settings(self);
                                }
                            }

                            cJSON_Delete(root);
                        }
                    }
                }
            }

            esp_http_client_close(client);
            esp_http_client_cleanup(client);
        }

        // Wait before next poll
        vTaskDelay(pdMS_TO_TICKS(self->poll_interval_ms));
    }

    ESP_LOGI(TAG, "Polling task stopped");
    polling_task_handle = NULL;
    vTaskDelete(NULL);
}

static esp_err_t remote_control_init_impl(RemoteControl_t *self)
{
    ESP_LOGI(TAG, "Initializing Remote Control");
    ESP_LOGI(TAG, "Server URL: %s", self->server_url);
    ESP_LOGI(TAG, "Poll interval: %d ms", self->poll_interval_ms);
    return ESP_OK;
}

static void remote_control_start_polling_impl(RemoteControl_t *self)
{
    if (self->running)
    {
        ESP_LOGW(TAG, "Polling already running");
        return;
    }

    self->running = true;

    BaseType_t result = xTaskCreate(
        polling_task,
        "remote_poll",
        8192,
        self,
        10, // Higher priority for responsive polling
        &polling_task_handle);

    if (result == pdPASS)
    {
        ESP_LOGI(TAG, "Remote polling task started");
    }
    else
    {
        ESP_LOGE(TAG, "Failed to create polling task");
        self->running = false;
    }
}

static void remote_control_stop_polling_impl(RemoteControl_t *self)
{
    if (!self->running)
    {
        return;
    }

    self->running = false;

    if (polling_task_handle)
    {
        vTaskDelete(polling_task_handle);
        polling_task_handle = NULL;
    }

    ESP_LOGI(TAG, "Remote polling stopped");
}

// Send current settings to server
esp_err_t remote_control_send_current_settings(RemoteControl_t *self)
{
    if (!self || !self->settings)
    {
        ESP_LOGE(TAG, "Invalid parameters for sending settings");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Sending current settings to server");

    // Build JSON with current settings
    cJSON *root = cJSON_CreateObject();
    if (!root)
    {
        ESP_LOGE(TAG, "Failed to create JSON object");
        return ESP_ERR_NO_MEM;
    }

    // Firmware version
    cJSON_AddStringToObject(root, "firmware_version", FIRMWARE_VERSION);

    // Camera settings
    cJSON *camera = cJSON_CreateObject();
    cJSON_AddNumberToObject(camera, "framesize", self->settings->settings.camera_framesize);
    cJSON_AddNumberToObject(camera, "quality", self->settings->settings.camera_quality);
    cJSON_AddNumberToObject(camera, "brightness", self->settings->settings.camera_brightness);
    cJSON_AddNumberToObject(camera, "contrast", self->settings->settings.camera_contrast);
    cJSON_AddNumberToObject(camera, "saturation", self->settings->settings.camera_saturation);
    cJSON_AddBoolToObject(camera, "vflip", self->settings->settings.camera_flip_v);
    cJSON_AddBoolToObject(camera, "hmirror", self->settings->settings.camera_flip_h);
    cJSON_AddNumberToObject(camera, "rotation", self->settings->settings.camera_rotation);
    cJSON_AddBoolToObject(camera, "flash_enabled", self->settings->settings.flash_enabled);
    cJSON_AddBoolToObject(camera, "self_timer_enabled", self->settings->settings.self_timer_enabled);
    cJSON_AddBoolToObject(camera, "auto_print_enabled", self->settings->settings.auto_print_enabled);
    cJSON_AddItemToObject(root, "camera", camera);

    // Poem settings
    cJSON *poem = cJSON_CreateObject();
    cJSON_AddStringToObject(poem, "style", self->settings->settings.poem_style);
    cJSON_AddItemToObject(root, "poem", poem);

    // Server settings
    cJSON *server = cJSON_CreateObject();
    cJSON_AddNumberToObject(server, "poll_interval_ms", self->settings->settings.server_poll_interval);
    cJSON_AddItemToObject(root, "server", server);

    // Log settings
    cJSON *log = cJSON_CreateObject();
    cJSON_AddBoolToObject(log, "upload_enabled", self->settings->settings.log_upload_enabled);
    cJSON_AddNumberToObject(log, "upload_interval_seconds", self->settings->settings.log_upload_interval);
    cJSON_AddNumberToObject(log, "queue_size", self->settings->settings.log_queue_size);
    cJSON_AddItemToObject(root, "log", log);

    // LED ring settings
    cJSON *led_ring = cJSON_CreateObject();
    cJSON_AddNumberToObject(led_ring, "brightness", self->settings->settings.led_ring_brightness);
    cJSON_AddNumberToObject(led_ring, "count", self->settings->settings.led_ring_count);
    cJSON_AddItemToObject(root, "led_ring", led_ring);

    // Thermal printer heating params — round-tripped so the Flutter UI shows
    // the live values rather than stale defaults.
    cJSON *thermal_printer = cJSON_CreateObject();
    cJSON_AddBoolToObject(thermal_printer, "enabled", self->settings->settings.printer_enabled);
    cJSON_AddNumberToObject(thermal_printer, "max_heating_dots", self->settings->settings.printer_max_heating_dots);
    cJSON_AddNumberToObject(thermal_printer, "heating_time", self->settings->settings.printer_heating_time);
    cJSON_AddNumberToObject(thermal_printer, "heating_interval", self->settings->settings.printer_heating_interval);
    cJSON_AddNumberToObject(thermal_printer, "density", self->settings->settings.printer_density);
    cJSON_AddNumberToObject(thermal_printer, "break_time", self->settings->settings.printer_break_time);
    cJSON_AddBoolToObject(thermal_printer, "bold", self->settings->settings.printer_bold);
    cJSON_AddBoolToObject(thermal_printer, "double_strike", self->settings->settings.printer_double_strike);
    cJSON_AddBoolToObject(thermal_printer, "heating_commands", self->settings->settings.printer_heating_commands);
    cJSON_AddNumberToObject(thermal_printer, "line_delay_ms", self->settings->settings.printer_line_delay_ms);
    cJSON_AddItemToObject(root, "thermal_printer", thermal_printer);

    // OTA settings
    cJSON *ota = cJSON_CreateObject();
    cJSON_AddNumberToObject(ota, "update_channel", self->settings->settings.ota_update_channel);
    cJSON_AddBoolToObject(ota, "auto_check", self->settings->settings.ota_auto_check);
    cJSON_AddStringToObject(ota, "github_owner", self->settings->settings.ota_github_owner);
    cJSON_AddStringToObject(ota, "github_repo", self->settings->settings.ota_github_repo);
    cJSON_AddStringToObject(ota, "testing_branch", self->settings->settings.ota_testing_branch);
    cJSON_AddItemToObject(root, "ota", ota);

    // Convert to string
    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);

    if (!json_str)
    {
        ESP_LOGE(TAG, "Failed to serialize JSON");
        return ESP_ERR_NO_MEM;
    }

    // Send to server
    char url[300];
    snprintf(url, sizeof(url), "%s/api/current-settings", self->server_url);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = ESP_FAIL;

    if (client)
    {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, json_str, strlen(json_str));

        err = esp_http_client_perform(client);

        if (err == ESP_OK)
        {
            int status_code = esp_http_client_get_status_code(client);
            if (status_code == 200)
            {
                ESP_LOGI(TAG, "Current settings sent successfully");
            }
            else
            {
                ESP_LOGW(TAG, "Server returned status %d", status_code);
                err = ESP_FAIL;
            }
        }
        else
        {
            ESP_LOGE(TAG, "Failed to send settings: %s", esp_err_to_name(err));
        }

        esp_http_client_cleanup(client);
    }

    free(json_str);
    return err;
}

// Constructor
RemoteControl_t *remote_control_create(SettingsManager_t *settings, HttpClient_t *http_client, LEDRing_t *led_ring, ThermalPrinter_t *printer, WiFi_t *wifi)
{
    RemoteControl_t *remote_control = malloc(sizeof(RemoteControl_t));
    if (!remote_control)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for RemoteControl");
        return NULL;
    }

    // Get server URL from settings and construct command endpoint base
    strncpy(remote_control->server_url, settings->settings.server_upload_url, sizeof(remote_control->server_url) - 1);

    // Remove /api/capture suffix if present to get base URL
    char *api_capture = strstr(remote_control->server_url, "/api/capture");
    if (api_capture)
    {
        *api_capture = '\0';
    }

    remote_control->poll_interval_ms = settings->settings.server_poll_interval;
    remote_control->settings = settings;
    remote_control->http_client = http_client;
    remote_control->led_ring = led_ring;
    remote_control->printer = printer;
    remote_control->wifi = wifi;
    remote_control->running = false;
    remote_control->init = remote_control_init_impl;
    remote_control->start_polling = remote_control_start_polling_impl;
    remote_control->stop_polling = remote_control_stop_polling_impl;

    return remote_control;
}

// Destructor
void remote_control_destroy(RemoteControl_t *remote_control)
{
    if (remote_control)
    {
        if (remote_control->running)
        {
            remote_control->stop_polling(remote_control);
        }
        free(remote_control);
    }
}
