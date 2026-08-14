#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_wifi.h"

// Include our custom "classes"
#include "led.h"
#include "led_ring.h"
#include "camera.h"
#include "wifi_manager.h"
#include "wifi_provisioning.h"
#include "settings_manager.h"
#include "http_client.h"
#include "remote_control.h"
#include "rotary_encoder.h"
#include "main_menu.h"
#include "thermal_printer.h"
#include "ota_manager.h"

#include "log_manager.h"

static const char *TAG = "MAIN";

// Global pointers for callbacks
static Camera_t *g_camera = NULL;
static HttpClient_t *g_http_client = NULL;
static ThermalPrinter_t *g_thermal_printer = NULL;
static LEDRing_t *g_led_ring = NULL;
static SettingsManager_t *g_settings = NULL;
static LogManager_t *g_log_manager = NULL;
static WiFi_t *g_wifi = NULL;
OTAManager_t *g_ota_manager = NULL; // Non-static so it can be accessed from remote_control.c

// Sub-menu state
static bool g_sub_menu_selection = false;       // false = OFF (red), true = ON (green)
static int g_last_encoder_position = 0;         // Track last encoder position for relative changes
static int g_sub_menu_position_accumulator = 0; // Accumulate encoder changes in sub-menu
static int g_sub_menu_option = -1;              // Track which menu option opened the sub-menu (0=flash, 1=self-timer)

// WiFi provisioning pulse state
static bool g_stop_provisioning_pulse = false;
static TaskHandle_t g_provisioning_pulse_task = NULL;

// Startup animation state
static bool g_stop_startup_animation = false;
static TaskHandle_t g_startup_animation_task = NULL;

// Poem loading animation state
static bool g_stop_poem_loading_animation = false;
static TaskHandle_t g_poem_loading_animation_task = NULL;

// Menu configuration
#define CLICKS_PER_OPTION 2         // Changed from 4 to 2 for better response
#define SUB_MENU_CLICKS_TO_TOGGLE 3 // Require 3 ticks in sub-menu to toggle

// Forward declarations
void stop_poem_loading_animation(void);
static void led_ring_wifi_connected_flash(LEDRing_t *led_ring);

// Thermal printer init is ~1.5s of pure waiting on the printer's own reset
// timing. Nothing else depends on it, so run it off the boot path: the printer
// object exists immediately (remote_control captures the pointer at create
// time) and flips ->initialized once the hardware handshake finishes.
static void printer_init_task(void *arg)
{
    ThermalPrinter_t *printer = (ThermalPrinter_t *)arg;
    if (printer->init(printer) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize Thermal Printer!");
    }
    vTaskDelete(NULL);
}

// Task wrapper for wifi->wait_for_connection_retry. FreeRTOS tasks must never
// return, so wrap the call and self-delete on completion. Once WiFi is up,
// stop the startup spinner and play the green "connected" flash so the LED
// ring isn't fighting the menu task over the RMT channel.
static void wifi_wait_task(void *arg)
{
    WiFi_t *wifi = (WiFi_t *)arg;
    if (wifi && wifi->wait_for_connection_retry)
    {
        wifi->wait_for_connection_retry(wifi);
    }

    if (g_led_ring && g_startup_animation_task != NULL)
    {
        g_stop_startup_animation = true;
        vTaskDelay(pdMS_TO_TICKS(100)); // let startup task clear LEDs and exit
        g_startup_animation_task = NULL;

        // Only play the green "connected" flash if we actually connected —
        // wait_for_connection_retry also returns on timeout, and signalling
        // success there would tell the user the opposite of what happened.
        if (wifi && wifi->connected)
        {
            led_ring_wifi_connected_flash(g_led_ring);
        }
        else
        {
            g_led_ring->clear(g_led_ring);
            g_led_ring->refresh(g_led_ring);
        }
    }

    vTaskDelete(NULL);
}

// LED Animation functions
static void led_ring_startup_animation(LEDRing_t *led_ring, bool *stop_flag)
{
    if (!led_ring)
        return;

    int led_count = led_ring->num_leds;

    // Calculate delay per LED for 2 revolutions per second
    int delay_per_led = 500 / led_count; // 500ms total per rotation / number of LEDs

    // Keep running until stop flag is set
    while (!(*stop_flag))
    {
        for (int i = 0; i < led_count; i++)
        {
            if (*stop_flag)
                break;

            // Clear all LEDs
            for (int j = 0; j < led_count; j++)
            {
                led_ring->set_pixel(led_ring, j, 0, 0, 0);
            }

            // Light current LED white
            led_ring->set_pixel(led_ring, i, 255, 255, 255);
            led_ring->refresh(led_ring);
            vTaskDelay(pdMS_TO_TICKS(delay_per_led));
        }
    }

    // Clear all LEDs at end
    led_ring->clear(led_ring);
    led_ring->refresh(led_ring);
}

// Task wrapper for startup animation (runs in separate task)
static void startup_animation_task(void *pvParameters)
{
    LEDRing_t *led_ring = (LEDRing_t *)pvParameters;
    led_ring_startup_animation(led_ring, &g_stop_startup_animation);
    vTaskDelete(NULL);
}

// Magenta pixel loading animation (for poem generation)
static void led_ring_poem_loading_animation(LEDRing_t *led_ring, bool *stop_flag)
{
    if (!led_ring)
        return;

    int led_count = led_ring->num_leds;

    // Calculate delay per LED for 0.5 second per rotation
    int delay_per_led = 500 / led_count; // 500ms total / number of LEDs

    // Keep running until stop flag is set
    while (!(*stop_flag))
    {
        for (int i = 0; i < led_count; i++)
        {
            if (*stop_flag)
                break;

            // Clear all LEDs
            for (int j = 0; j < led_count; j++)
            {
                led_ring->set_pixel(led_ring, j, 0, 0, 0);
            }

            // Light current LED magenta (255, 0, 255)
            led_ring->set_pixel(led_ring, i, 255, 0, 255);
            led_ring->refresh(led_ring);
            vTaskDelay(pdMS_TO_TICKS(delay_per_led));
        }
    }

    // Clear all LEDs at end
    led_ring->clear(led_ring);
    led_ring->refresh(led_ring);
}

// Task wrapper for poem loading animation (runs in separate task)
static void poem_loading_animation_task(void *pvParameters)
{
    LEDRing_t *led_ring = (LEDRing_t *)pvParameters;
    led_ring_poem_loading_animation(led_ring, &g_stop_poem_loading_animation);
    vTaskDelete(NULL);
}

static void led_ring_wifi_connected_flash(LEDRing_t *led_ring)
{
    if (!led_ring)
        return;

    int led_count = led_ring->num_leds;

    // Smooth fade in green (1.5 seconds - increased from 510ms)
    for (int brightness = 0; brightness <= 255; brightness += 3)
    {
        for (int i = 0; i < led_count; i++)
        {
            led_ring->set_pixel(led_ring, i, 0, brightness, 0);
        }
        led_ring->refresh(led_ring);
        vTaskDelay(pdMS_TO_TICKS(18)); // 18ms * 85 steps = 1530ms fade in
    }

    // Hold at full brightness longer (1 second - increased from 200ms)
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Smooth fade out (1.5 seconds - increased from 306ms)
    for (int brightness = 255; brightness >= 0; brightness -= 3)
    {
        for (int i = 0; i < led_count; i++)
        {
            led_ring->set_pixel(led_ring, i, 0, brightness, 0);
        }
        led_ring->refresh(led_ring);
        vTaskDelay(pdMS_TO_TICKS(18)); // 18ms * 85 steps = 1530ms fade out
    }

    // Clear all LEDs
    led_ring->clear(led_ring);
    led_ring->refresh(led_ring);
}

static void led_ring_wifi_provisioning_pulse(LEDRing_t *led_ring, bool *stop_flag)
{
    if (!led_ring)
        return;

    int led_count = led_ring->num_leds;

    while (!(*stop_flag))
    {
        // Breathe in (2 seconds - increased from 1 second)
        for (int brightness = 0; brightness <= 255; brightness += 2)
        {
            if (*stop_flag)
                break;

            for (int i = 0; i < led_count; i++)
            {
                led_ring->set_pixel(led_ring, i, 0, 0, brightness);
            }
            led_ring->refresh(led_ring);
            vTaskDelay(pdMS_TO_TICKS(16)); // 16ms * 128 steps = 2048ms (2 seconds)
        }

        // Breathe out (2 seconds - increased from 1 second)
        for (int brightness = 255; brightness >= 0; brightness -= 2)
        {
            if (*stop_flag)
                break;

            for (int i = 0; i < led_count; i++)
            {
                led_ring->set_pixel(led_ring, i, 0, 0, brightness);
            }
            led_ring->refresh(led_ring);
            vTaskDelay(pdMS_TO_TICKS(16)); // 16ms * 128 steps = 2048ms (2 seconds)
        }
    }

    // Clear all LEDs when stopped
    led_ring->clear(led_ring);
    led_ring->refresh(led_ring);
}

// Task wrapper for provisioning pulse (runs in separate task)
static void provisioning_pulse_task(void *pvParameters)
{
    LEDRing_t *led_ring = (LEDRing_t *)pvParameters;
    led_ring_wifi_provisioning_pulse(led_ring, &g_stop_provisioning_pulse);
    vTaskDelete(NULL);
}

// Rotary encoder rotation callback
void on_rotary_rotation(RotaryEncoder_t *encoder, int position)
{
    // Check if we're in sub-menu mode
    if (is_sub_menu_active())
    {
        // Accumulate encoder changes - require SUB_MENU_CLICKS_TO_TOGGLE ticks to toggle
        int position_delta = position - g_last_encoder_position;

        if (position_delta != 0)
        {
            g_sub_menu_position_accumulator += position_delta;
            g_last_encoder_position = position;

            // Check if we've accumulated enough ticks to toggle
            if (abs(g_sub_menu_position_accumulator) >= SUB_MENU_CLICKS_TO_TOGGLE)
            {
                g_sub_menu_selection = !g_sub_menu_selection;
                g_sub_menu_position_accumulator = 0; // Reset accumulator after toggle
                ESP_LOGI(TAG, "Sub-menu selection toggled: Self-Timer %s", g_sub_menu_selection ? "ON" : "OFF");
                main_menu_update_sub_menu(g_sub_menu_selection);
            }
        }
        return;
    }

    // Main menu rotation handling
    // Calculate menu option based on encoder position
    int new_option = ((position / CLICKS_PER_OPTION) % get_menu_options_count() + get_menu_options_count()) % get_menu_options_count();

    // If this is the first rotation, fade in the menu
    if (!is_menu_visible())
    {
        ESP_LOGI(TAG, "First rotation - fading in menu: %s", get_menu_option_name(new_option));
        main_menu_fade_in(new_option);
        return;
    }

    // Only update if menu option changed
    if (new_option != get_current_menu_option())
    {
        ESP_LOGI(TAG, "Menu: %s (Option %d)", get_menu_option_name(new_option), new_option);
        update_led_ring_menu(new_option);
    }
    else
    {
        // Even if option didn't change, reset the fade-out timer
        main_menu_reset_timer();
    }
}

// Long press progress callback - warning LED
void on_button_long_press_progress(RotaryEncoder_t *encoder, int progress)
{
    if (g_led_ring)
    {
        // Set LED ring to red with increasing brightness as warning
        g_led_ring->set_all(g_led_ring, 255, 0, 0);       // Full red
        g_led_ring->set_brightness(g_led_ring, progress); // 0-100 percent
        g_led_ring->refresh(g_led_ring);
    }
}

// Long press callback - factory reset
void on_button_long_press(RotaryEncoder_t *encoder)
{
    ESP_LOGI(TAG, "Factory reset initiated!");

    // Reset all settings to defaults
    if (g_settings)
    {
        g_settings->reset_to_defaults(g_settings);
        g_settings->save_settings(g_settings);
        ESP_LOGI(TAG, "Settings reset to defaults");
    }

    // Reset WiFi settings
    esp_wifi_restore();
    ESP_LOGI(TAG, "WiFi settings reset");

    // Restart the device
    ESP_LOGI(TAG, "Restarting device...");
    esp_restart();
}

// Button press callback - take a picture or handle menu selection
void on_button_press(RotaryEncoder_t *encoder)
{
    ESP_LOGI(TAG, "Button pressed!");

    // Check if we're in sub-menu mode
    if (is_sub_menu_active())
    {
        // Save the appropriate setting based on which option opened the sub-menu
        if (g_settings)
        {
            if (g_sub_menu_option == 0)
            {
                // Flash setting
                g_settings->settings.flash_enabled = g_sub_menu_selection;
                g_settings->save_settings(g_settings);
                ESP_LOGI(TAG, "Flash setting saved: %s", g_sub_menu_selection ? "ENABLED" : "DISABLED");
            }
            else if (g_sub_menu_option == 1)
            {
                // Self-timer setting
                g_settings->settings.self_timer_enabled = g_sub_menu_selection;
                g_settings->save_settings(g_settings);
                ESP_LOGI(TAG, "Self-timer setting saved: %s", g_sub_menu_selection ? "ENABLED" : "DISABLED");
            }
            else if (g_sub_menu_option == 2)
            {
                // Auto-print setting
                g_settings->settings.auto_print_enabled = g_sub_menu_selection;
                g_settings->save_settings(g_settings);
                ESP_LOGI(TAG, "Auto-print setting saved: %s", g_sub_menu_selection ? "ENABLED" : "DISABLED");
            }
        }

        // Exit sub-menu (fade out and return to main mode)
        main_menu_exit_sub_menu();
        g_sub_menu_option = -1; // Reset
        return;
    }

    // Check if main menu is visible
    if (is_menu_visible())
    {
        int selected_option = get_current_menu_option();
        ESP_LOGI(TAG, "Menu option %d selected: %s", selected_option, get_menu_option_name(selected_option));

        // Option 0 (Red - Flash Settings)
        if (selected_option == 0)
        {
            ESP_LOGI(TAG, "Opening Flash settings sub-menu...");

            // Get current setting
            bool current_setting = true; // Default to enabled
            if (g_settings)
            {
                current_setting = g_settings->settings.flash_enabled;
            }

            // Enter sub-menu with current setting as initial selection
            g_sub_menu_selection = current_setting;
            g_sub_menu_option = 0; // Track that we're editing flash
            g_last_encoder_position = encoder->get_position(encoder);
            g_sub_menu_position_accumulator = 0;
            main_menu_enter_sub_menu(current_setting);
            return;
        }

        // Option 1 (Blue - Self Timer Settings)
        if (selected_option == 1)
        {
            ESP_LOGI(TAG, "Opening Self-Timer settings sub-menu...");

            // Get current setting
            bool current_setting = false;
            if (g_settings)
            {
                current_setting = g_settings->settings.self_timer_enabled;
            }

            // Enter sub-menu with current setting as initial selection
            g_sub_menu_selection = current_setting;
            g_sub_menu_option = 1;                                    // Track that we're editing self-timer
            g_last_encoder_position = encoder->get_position(encoder); // Save current position
            g_sub_menu_position_accumulator = 0;                      // Reset accumulator for sub-menu
            main_menu_enter_sub_menu(current_setting);
            return;
        }

        // Option 2 (Green - Auto Print Settings)
        if (selected_option == 2)
        {
            ESP_LOGI(TAG, "Opening Auto Print settings sub-menu...");

            // Get current setting
            bool current_setting = false;
            if (g_settings)
            {
                current_setting = g_settings->settings.auto_print_enabled;
            }

            // Enter sub-menu with current setting as initial selection
            g_sub_menu_selection = current_setting;
            g_sub_menu_option = 2; // Track that we're editing auto-print
            g_last_encoder_position = encoder->get_position(encoder);
            g_sub_menu_position_accumulator = 0;
            main_menu_enter_sub_menu(current_setting);
            return;
        }

        // Option 3 (Yellow - OTA Update)
        if (selected_option == 3)
        {
            ESP_LOGI(TAG, "Opening OTA Update menu...");

            if (!g_ota_manager)
            {
                ESP_LOGE(TAG, "OTA Manager not initialized!");
                // Flash red to indicate error
                if (g_led_ring)
                {
                    int led_count = g_led_ring->num_leds;
                    for (int i = 0; i < 3; i++)
                    {
                        for (int j = 0; j < led_count; j++)
                        {
                            g_led_ring->set_pixel(g_led_ring, j, 255, 0, 0);
                        }
                        g_led_ring->refresh(g_led_ring);
                        vTaskDelay(pdMS_TO_TICKS(200));

                        for (int j = 0; j < led_count; j++)
                        {
                            g_led_ring->set_pixel(g_led_ring, j, 0, 0, 0);
                        }
                        g_led_ring->refresh(g_led_ring);
                        vTaskDelay(pdMS_TO_TICKS(200));
                    }
                }
                return;
            }

            // Disable menu fade-out during OTA process
            main_menu_stop_timer();

            // Show checking animation
            if (g_led_ring)
            {
                int led_count = g_led_ring->num_leds;
                for (int i = 0; i < led_count; i++)
                {
                    g_led_ring->set_pixel(g_led_ring, i, 255, 255, 0); // Yellow
                }
                g_led_ring->refresh(g_led_ring);
            }

            // Check for updates
            bool update_available = false;
            esp_err_t err = g_ota_manager->check_for_update(g_ota_manager, &update_available);

            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to check for updates: %s", esp_err_to_name(err));
                // Flash red to indicate error
                if (g_led_ring)
                {
                    int led_count = g_led_ring->num_leds;
                    for (int i = 0; i < 3; i++)
                    {
                        for (int j = 0; j < led_count; j++)
                        {
                            g_led_ring->set_pixel(g_led_ring, j, 255, 0, 0);
                        }
                        g_led_ring->refresh(g_led_ring);
                        vTaskDelay(pdMS_TO_TICKS(200));

                        for (int j = 0; j < led_count; j++)
                        {
                            g_led_ring->set_pixel(g_led_ring, j, 0, 0, 0);
                        }
                        g_led_ring->refresh(g_led_ring);
                        vTaskDelay(pdMS_TO_TICKS(200));
                    }
                }
                return;
            }

            if (update_available)
            {
                char current_ver[32], latest_ver[32];
                g_ota_manager->get_current_version(g_ota_manager, current_ver);
                g_ota_manager->get_latest_version(g_ota_manager, latest_ver);
                ESP_LOGI(TAG, "Update available: %s -> %s", current_ver, latest_ver);
                ESP_LOGI(TAG, "Installing update...");

                // Pulse green while updating
                if (g_led_ring)
                {
                    int led_count = g_led_ring->num_leds;
                    for (int j = 0; j < led_count; j++)
                    {
                        g_led_ring->set_pixel(g_led_ring, j, 0, 255, 0);
                    }
                    g_led_ring->refresh(g_led_ring);
                }

                // Perform the update (will restart on success)
                esp_err_t update_err = g_ota_manager->perform_update(g_ota_manager);

                // If we reach here, update failed
                ESP_LOGE(TAG, "OTA update failed: %s", esp_err_to_name(update_err));
                if (g_led_ring)
                {
                    int led_count = g_led_ring->num_leds;
                    for (int i = 0; i < 5; i++)
                    {
                        for (int j = 0; j < led_count; j++)
                        {
                            g_led_ring->set_pixel(g_led_ring, j, 255, 0, 0);
                        }
                        g_led_ring->refresh(g_led_ring);
                        vTaskDelay(pdMS_TO_TICKS(200));

                        for (int j = 0; j < led_count; j++)
                        {
                            g_led_ring->set_pixel(g_led_ring, j, 0, 0, 0);
                        }
                        g_led_ring->refresh(g_led_ring);
                        vTaskDelay(pdMS_TO_TICKS(200));
                    }
                }
            }
            else
            {
                ESP_LOGI(TAG, "No update available - already on latest version");
                // Flash blue to indicate no update
                if (g_led_ring)
                {
                    int led_count = g_led_ring->num_leds;
                    for (int i = 0; i < 3; i++)
                    {
                        for (int j = 0; j < led_count; j++)
                        {
                            g_led_ring->set_pixel(g_led_ring, j, 0, 0, 255);
                        }
                        g_led_ring->refresh(g_led_ring);
                        vTaskDelay(pdMS_TO_TICKS(200));

                        for (int j = 0; j < led_count; j++)
                        {
                            g_led_ring->set_pixel(g_led_ring, j, 0, 0, 0);
                        }
                        g_led_ring->refresh(g_led_ring);
                        vTaskDelay(pdMS_TO_TICKS(200));
                    }
                }
            }
            return;
        }

        // For other menu options, just fade out and take picture
        ESP_LOGI(TAG, "Other menu option - taking picture");
        main_menu_stop_timer();
    }
    else
    {
        ESP_LOGI(TAG, "Menu not visible - taking picture");
    }

    // Take picture with or without self-timer based on setting
    bool use_self_timer = false;
    if (g_settings)
    {
        use_self_timer = g_settings->settings.self_timer_enabled;
    }

    if (use_self_timer && g_led_ring)
    {
        ESP_LOGI(TAG, "Starting 5-second self-timer countdown...");

        int led_count = g_led_ring->num_leds;
        int delay_per_led = 1000 / led_count; // 1 second divided by number of LEDs for smooth animation

        for (int round = 0; round < 5; round++)
        {
            // Calculate color for this round (fade red to green across the 5 rounds)
            uint8_t red = 255 - ((255 * round) / 5);
            uint8_t green = (255 * round) / 5;

            // One complete circle around the ring
            for (int i = 0; i < led_count; i++)
            {
                // Only light the current LED, turn off all others
                for (int j = 0; j < led_count; j++)
                {
                    if (j == i)
                    {
                        // Current LED - color for this round
                        g_led_ring->set_pixel(g_led_ring, j, red, green, 0);
                    }
                    else
                    {
                        // All other LEDs - off
                        g_led_ring->set_pixel(g_led_ring, j, 0, 0, 0);
                    }
                }

                g_led_ring->refresh(g_led_ring);
                vTaskDelay(pdMS_TO_TICKS(delay_per_led));
            }
        }

        // Flash bright white if flash is enabled (500ms)
        bool flash_enabled = true; // Default to enabled
        if (g_settings)
        {
            flash_enabled = g_settings->settings.flash_enabled;
        }

        if (flash_enabled)
        {
            // Turn on flash for 100ms to let it stabilize
            for (int i = 0; i < led_count; i++)
            {
                g_led_ring->set_pixel(g_led_ring, i, 255, 255, 255);
            }
            g_led_ring->refresh(g_led_ring);
            vTaskDelay(pdMS_TO_TICKS(100)); // Pre-flash delay to stabilize
        }
    }
    else
    {
        // No self-timer, but check if we should flash
        bool flash_enabled = true; // Default to enabled
        if (g_settings)
        {
            flash_enabled = g_settings->settings.flash_enabled;
        }

        if (flash_enabled && g_led_ring)
        {
            int led_count = g_led_ring->num_leds;
            // Turn on flash for 100ms to let it stabilize before capture
            for (int i = 0; i < led_count; i++)
            {
                g_led_ring->set_pixel(g_led_ring, i, 255, 255, 255);
            }
            g_led_ring->refresh(g_led_ring);
            vTaskDelay(pdMS_TO_TICKS(100)); // Pre-flash delay to stabilize
        }
    }

    // Take the picture DURING the flash peak
    if (g_camera && g_http_client)
    {
        // CHECK: Is WiFi connected? If not, we can't upload, so don't take the picture.
        if (!g_wifi || !g_wifi->connected) {
            ESP_LOGW(TAG, "Cannot take picture: WiFi not connected");
            
            // Turn off flash if it was on
            if (g_led_ring) {
                g_led_ring->clear(g_led_ring);
                g_led_ring->refresh(g_led_ring);
                
                // Flash red to indicate "Not Ready / No WiFi"
                int led_count = g_led_ring->num_leds;
                for (int flash = 0; flash < 2; flash++) {
                    for (int i = 0; i < led_count; i++) {
                        g_led_ring->set_pixel(g_led_ring, i, 255, 0, 0);
                    }
                    g_led_ring->refresh(g_led_ring);
                    vTaskDelay(pdMS_TO_TICKS(200));
                    
                    g_led_ring->clear(g_led_ring);
                    g_led_ring->refresh(g_led_ring);
                    vTaskDelay(pdMS_TO_TICKS(200));
                }
            }
            return; 
        }

        // Flush old frames from buffer (we have 2 buffers configured)
        for (int i = 0; i < 2; i++)
        {
            camera_fb_t *flush_fb = g_camera->capture(g_camera);
            if (flush_fb)
            {
                g_camera->return_frame(g_camera, flush_fb);
            }
        }

        // Now capture the fresh frame taken during flash
        camera_fb_t *fb = g_camera->capture(g_camera);

        // Turn off the flash immediately after capture
        if (g_led_ring)
        {
            g_led_ring->clear(g_led_ring);
            g_led_ring->refresh(g_led_ring);
        }

        if (fb)
        {
            ESP_LOGI(TAG, "Picture captured: %zu bytes", fb->len);

            // Check if auto-print is enabled
            bool auto_print = false;
            if (g_settings)
            {
                auto_print = g_settings->settings.auto_print_enabled;
            }

            // Start magenta loading animation if auto-print is enabled
            if (auto_print && g_led_ring)
            {
                ESP_LOGI(TAG, "Auto-print enabled - starting poem loading animation");

                // Ensure LEDs are completely off before starting new animation
                g_led_ring->clear(g_led_ring);
                g_led_ring->refresh(g_led_ring);
                vTaskDelay(pdMS_TO_TICKS(50)); // Small delay to ensure RMT channel is released

                g_stop_poem_loading_animation = false;
                xTaskCreate(poem_loading_animation_task, "PoemLoadAnim", 4096, (void *)g_led_ring, 5, &g_poem_loading_animation_task);
            }

            // Upload to server
            esp_err_t result = g_http_client->upload_image(g_http_client, fb);
            if (result == ESP_OK)
            {
                ESP_LOGI(TAG, "Picture uploaded successfully!");
            }
            else
            {
                ESP_LOGE(TAG, "Failed to upload picture");

                // Stop animation if auto-print is on
                if (auto_print)
                {
                    stop_poem_loading_animation();
                }

                // Flash red to indicate upload failure
                if (g_led_ring)
                {
                    int led_count = g_led_ring->num_leds;

                    // Quick red flash 3 times
                    for (int flash = 0; flash < 3; flash++)
                    {
                        for (int i = 0; i < led_count; i++)
                        {
                            g_led_ring->set_pixel(g_led_ring, i, 255, 0, 0);
                        }
                        g_led_ring->refresh(g_led_ring);
                        vTaskDelay(pdMS_TO_TICKS(200));

                        g_led_ring->clear(g_led_ring);
                        g_led_ring->refresh(g_led_ring);
                        vTaskDelay(pdMS_TO_TICKS(200));
                    }
                }
            }

            g_camera->return_frame(g_camera, fb);
        }
        else
        {
            ESP_LOGE(TAG, "Failed to capture picture");
        }
    }

    // Turn off LEDs after picture
    if (g_led_ring)
    {
        int led_count = g_led_ring->num_leds;
        for (int i = 0; i < led_count; i++)
        {
            g_led_ring->set_pixel(g_led_ring, i, 0, 0, 0);
        }
        g_led_ring->refresh(g_led_ring);
    }
}

// Function to stop poem loading animation (can be called from other modules)
void stop_poem_loading_animation(void)
{
    if (g_poem_loading_animation_task != NULL)
    {
        ESP_LOGI(TAG, "Stopping poem loading animation (called externally)");
        g_stop_poem_loading_animation = true;
        vTaskDelay(pdMS_TO_TICKS(100)); // Give task time to finish
        g_poem_loading_animation_task = NULL;
    }
}

void app_main(void)
{
    // Set log levels to reduce console clutter
    esp_log_level_set("*", ESP_LOG_INFO);                   // Default: INFO for all
    esp_log_level_set("wifi", ESP_LOG_DEBUG);               // WiFi to DEBUG
    esp_log_level_set("wifi_init", ESP_LOG_DEBUG);          // WiFi init to DEBUG
    esp_log_level_set("phy_init", ESP_LOG_DEBUG);           // PHY init to DEBUG
    esp_log_level_set("pp", ESP_LOG_DEBUG);                 // PP to DEBUG
    esp_log_level_set("net80211", ESP_LOG_DEBUG);           // net80211 to DEBUG
    esp_log_level_set("esp_netif_handlers", ESP_LOG_DEBUG); // netif to DEBUG
    esp_log_level_set("WIFI", ESP_LOG_INFO);                // Keep WIFI manager at INFO
    esp_log_level_set("CAMERA", ESP_LOG_DEBUG);             // Camera to DEBUG
    esp_log_level_set("camera", ESP_LOG_DEBUG);             // Camera sensor to DEBUG
    esp_log_level_set("cam_hal", ESP_LOG_DEBUG);            // Camera HAL to DEBUG
    esp_log_level_set("sccb-ng", ESP_LOG_DEBUG);            // I2C to DEBUG
    esp_log_level_set("s3 ll_cam", ESP_LOG_DEBUG);          // Low-level cam to DEBUG
    esp_log_level_set("ov2640", ESP_LOG_DEBUG);             // OV2640 to DEBUG
    esp_log_level_set("ROTARY_ENCODER", ESP_LOG_DEBUG);     // Encoder to DEBUG
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_DEBUG);        // HTTP client to DEBUG
    esp_log_level_set("REMOTE_CONTROL", ESP_LOG_DEBUG);     // Remote control to DEBUG

    // Uncomment the line below to clear stored WiFi credentials (force re-provisioning)
    // wifi_credentials_clear();

    LED_t *led = led_create(GPIO_NUM_2);
    if (!led)
    {
        ESP_LOGE(TAG, "Failed to create LED object!");
        return;
    }

    // Init here (not later): the WiFi event handler blinks this LED on GOT_IP,
    // and WiFi is now started well before the rest of the hardware is up.
    led->init(led);

    Camera_t *camera = camera_create(led);
    if (!camera)
    {
        ESP_LOGE(TAG, "Failed to create Camera object!");
        led_destroy(led);
        return;
    }

    SettingsManager_t *settings = settings_manager_create();
    if (!settings)
    {
        ESP_LOGE(TAG, "Failed to create Settings Manager!");
        camera_destroy(camera);
        led_destroy(led);
        return;
    }

    if (settings->init(settings) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize Settings Manager!");
        settings_manager_destroy(settings);
        camera_destroy(camera);
        led_destroy(led);
        return;
    }

    // Uncomment the line below ONCE to reset all settings to defaults (fixes wrong GPIO pins)
    // After flashing once with this enabled, comment it out again and reflash
    // settings->reset_to_defaults(settings);
    // ESP_LOGW(TAG, "Settings reset to defaults - please comment out reset line and reflash!");

    // Store settings globally for callback access
    g_settings = settings;

    // Print current settings
    settings->print(settings);

    g_log_manager = log_manager_create();
    if (g_log_manager)
    {
        // Build log upload URL (make buffer larger to avoid truncation warning)
        char log_url[300];

        // Replace /api/capture with /api/logs
        const char *base_url = settings->settings.server_upload_url;
        char *api_pos = strstr(base_url, "/api/capture");

        if (api_pos)
        {
            // Copy everything before /api/capture, then add /api/logs
            size_t prefix_len = api_pos - base_url;
            snprintf(log_url, sizeof(log_url), "%.*s/api/logs", (int)prefix_len, base_url);
        }
        else
        {
            // If /api/capture not found, just append /api/logs to base URL
            snprintf(log_url, sizeof(log_url), "%s/api/logs", base_url);
        }

        if (g_log_manager->init(g_log_manager, log_url) == ESP_OK)
        {
            // Log manager initialized successfully
        }
        else
        {
            ESP_LOGE(TAG, "Failed to initialize Log Manager!");
            log_manager_destroy(g_log_manager);
            g_log_manager = NULL;
        }
    }
    else
    {
        ESP_LOGW(TAG, "Log Manager creation failed - logs will not be sent to server");
    }

    LEDRing_t *led_ring = NULL;
    if (settings->settings.led_ring_enabled)
    {

        led_ring = led_ring_create((gpio_num_t)settings->settings.led_ring_data_pin,
                                   settings->settings.led_ring_count);
        if (!led_ring)
        {
            ESP_LOGE(TAG, "Failed to create LED Ring object!");
            settings_manager_destroy(settings);
            camera_destroy(camera);
            led_destroy(led);
            return;
        }

        // Initialize LED ring
        led_ring->init(led_ring);
        led_ring->set_brightness(led_ring, settings->settings.led_ring_brightness);

        // Start startup animation in background task (non-blocking)
        g_stop_startup_animation = false;
        xTaskCreate(startup_animation_task, "StartupAnim", 4096, (void *)led_ring, 5, &g_startup_animation_task);

        // Assign to global for button callback access
        g_led_ring = led_ring;
    }
    // Initialize main menu system
    main_menu_init(led_ring, settings);

    if (!settings->has_wifi_credentials(settings))
    {
        ESP_LOGI(TAG, "No WiFi credentials found. Starting provisioning mode...");

        // Create provisioning manager
        WiFiProvisioning_t *provisioning = wifi_provisioning_create(led);
        if (!provisioning)
        {
            ESP_LOGE(TAG, "Failed to create provisioning object!");
            camera_destroy(camera);
            led_ring_destroy(led_ring);
            led_destroy(led);
            return;
        }

        // Initialize provisioning
        provisioning->init(provisioning);

        // Start Access Point
        if (provisioning->start_ap(provisioning) != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to start AP!");
            wifi_provisioning_destroy(provisioning);
            camera_destroy(camera);
            led_ring_destroy(led_ring);
            led_destroy(led);
            return;
        }

        // Start web portal
        if (provisioning->start_portal(provisioning) != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to start portal!");
            wifi_provisioning_destroy(provisioning);
            camera_destroy(camera);
            led_ring_destroy(led_ring);
            led_destroy(led);
            return;
        }

        ESP_LOGI(TAG, "\n===========================================");
        ESP_LOGI(TAG, "  PROVISIONING MODE");
        ESP_LOGI(TAG, "===========================================");
        ESP_LOGI(TAG, "  1. Connect to WiFi: %s", "ESP32-Camera-Setup");
        ESP_LOGI(TAG, "  2. Open browser: http://192.168.4.1");
        ESP_LOGI(TAG, "  3. Enter your WiFi credentials");
        ESP_LOGI(TAG, "===========================================\n");

        // Stop startup animation and start blue pulsing animation on LED ring during provisioning
        if (led_ring)
        {
            // Stop startup animation if still running
            g_stop_startup_animation = true;
            vTaskDelay(pdMS_TO_TICKS(100)); // Give startup animation time to clean up

            // Start provisioning pulse
            g_stop_provisioning_pulse = false;
            xTaskCreate(provisioning_pulse_task, "ProvisionPulse", 4096, (void *)led_ring, 5, &g_provisioning_pulse_task);
        }

        // Wait for credentials (5 minutes timeout)
        if (!provisioning->wait_for_credentials(provisioning, 300000))
        {
            ESP_LOGE(TAG, "Provisioning timeout! Restarting...");

            // Stop pulsing animation
            if (led_ring)
            {
                g_stop_provisioning_pulse = true;
                vTaskDelay(pdMS_TO_TICKS(100)); // Give task time to exit
            }

            wifi_provisioning_destroy(provisioning);
            settings_manager_destroy(settings);
            camera_destroy(camera);
            led_ring_destroy(led_ring);
            led_destroy(led);
            vTaskDelay(3000 / portTICK_PERIOD_MS);
            esp_restart();
            return;
        }

        // Stop pulsing animation when credentials received
        if (led_ring)
        {
            g_stop_provisioning_pulse = true;
            vTaskDelay(pdMS_TO_TICKS(100)); // Give task time to clean up
        }

        // Get credentials and save to settings
        wifi_credentials_t *creds = provisioning->get_credentials(provisioning);
        settings->set_wifi_credentials(settings, creds->ssid, creds->password);

        // Stop provisioning
        provisioning->stop(provisioning);
        wifi_provisioning_destroy(provisioning);

        // Wait a bit for AP to shut down
        vTaskDelay(2000 / portTICK_PERIOD_MS);

        ESP_LOGI(TAG, "Provisioning complete! Connecting to WiFi...");
    }

    // Create WiFi manager with credentials from settings
    char ssid[32], password[64];
    if (settings->get_wifi_credentials(settings, ssid, password) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to get WiFi credentials!");
        settings_manager_destroy(settings);
        camera_destroy(camera);
        led_ring_destroy(led_ring);
        led_destroy(led);
        return;
    }

    g_wifi = wifi_create(ssid, password, led);

    if (!g_wifi)
    {
        ESP_LOGE(TAG, "Failed to create WiFi object!");
        settings_manager_destroy(settings);
        camera_destroy(camera);
        led_ring_destroy(led_ring);
        led_destroy(led);
        return;
    }

    // Start WiFi before the rest of the hardware. Association + DHCP take
    // several seconds and are entirely asynchronous, so kicking them off here
    // lets them overlap with the printer and camera bring-up below instead of
    // running after it.
    g_wifi->init(g_wifi);

    // Create HTTP client for uploading to Flask server
    HttpClient_t *http_client = http_client_create(camera, settings);
    if (!http_client)
    {
        ESP_LOGE(TAG, "Failed to create HTTP Client object!");
        settings_manager_destroy(settings);
        wifi_destroy(g_wifi);
        camera_destroy(camera);
        led_ring_destroy(led_ring);
        led_destroy(led);
        return;
    }

    if (settings->settings.printer_enabled)
    {
        printer_config_t printer_config = {
            .uart_port = (uart_port_t)settings->settings.printer_uart_port,
            .tx_pin = (gpio_num_t)settings->settings.printer_tx_pin,
            .rx_pin = (gpio_num_t)settings->settings.printer_rx_pin,
            .rts_pin = (gpio_num_t)settings->settings.printer_rts_pin,
            .baud_rate = settings->settings.printer_baud_rate,
            .max_print_width = settings->settings.printer_max_width,
            .max_heating_dots = settings->settings.printer_max_heating_dots,
            .heating_time = settings->settings.printer_heating_time,
            .heating_interval = settings->settings.printer_heating_interval,
            .density = settings->settings.printer_density,
            .break_time = settings->settings.printer_break_time,
            .bold = settings->settings.printer_bold,
            .double_strike = settings->settings.printer_double_strike,
            .heating_commands = settings->settings.printer_heating_commands,
            .line_delay_ms = settings->settings.printer_line_delay_ms};

        g_thermal_printer = thermal_printer_create(printer_config);
        if (!g_thermal_printer)
        {
            ESP_LOGE(TAG, "Failed to create Thermal Printer object!");
            // Continue without printer (non-critical)
        }
        else
        {
            // Hardware handshake runs in the background; see printer_init_task.
            xTaskCreate(printer_init_task, "PrinterInit", 4096, (void *)g_thermal_printer, 4, NULL);
        }
    }
    else
    {
        g_thermal_printer = NULL;
    }

    // Create Remote Control for polling server commands
    RemoteControl_t *remote_control = remote_control_create(settings, http_client, led_ring, g_thermal_printer, g_wifi);
    if (!remote_control)
    {
        ESP_LOGE(TAG, "Failed to create Remote Control object!");
        if (g_thermal_printer)
            thermal_printer_destroy(g_thermal_printer);
        if (g_log_manager)
            log_manager_destroy(g_log_manager);
        http_client_destroy(http_client);
        settings_manager_destroy(settings);
        wifi_destroy(g_wifi);
        camera_destroy(camera);
        led_ring_destroy(led_ring);
        led_destroy(led);
        return;
    }

    RotaryEncoder_t *rotary = NULL;
    if (settings->settings.encoder_enabled)
    {
        rotary = rotary_encoder_create((gpio_num_t)settings->settings.encoder_clk_pin,
                                       (gpio_num_t)settings->settings.encoder_dt_pin,
                                       (gpio_num_t)settings->settings.encoder_sw_pin);
        if (!rotary)
        {
            ESP_LOGE(TAG, "Failed to create Rotary Encoder object!");
            remote_control_destroy(remote_control);
            http_client_destroy(http_client);
            settings_manager_destroy(settings);
            wifi_destroy(g_wifi);
            camera_destroy(camera);
            if (led_ring)
                led_ring_destroy(led_ring);
            led_destroy(led);
            return;
        }
    }

    if (camera->init(camera) != ESP_OK)
    {
        ESP_LOGE(TAG, "Camera initialization failed!");
        while (1)
        {
            led->blink(led, 10);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
    }

    // Apply rotation from settings
    camera->set_rotation(camera, settings->settings.camera_rotation);

    // Set global pointers for callbacks
    g_camera = camera;
    g_http_client = http_client;

    // Set callbacks and initialize rotary encoder (if enabled)
    if (rotary)
    {
        rotary_encoder_set_rotation_callback(rotary, on_rotary_rotation);
        rotary_encoder_set_button_callback(rotary, on_button_press);
        rotary_encoder_set_long_press_callback(rotary, on_button_long_press);
        rotary_encoder_set_long_press_progress_callback(rotary, on_button_long_press_progress);

        // Initialize rotary encoder
        if (rotary->init(rotary) != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to initialize rotary encoder!");
        }
    }

    // Initialize components that don't depend on WiFi yet
    if (http_client->init(http_client) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize HTTP Client!");
    }

    if (remote_control->init(remote_control) == ESP_OK)
    {
        // Remote control polling will wait for WiFi internally
        remote_control->start_polling(remote_control);
    }

    // Get firmware version if OTA manager is initialized
    const char *firmware_version = "unknown";
    if (g_ota_manager && g_ota_manager->initialized)
    {
        firmware_version = g_ota_manager->current_version;
    }

    ESP_LOGI(TAG, "Hardware Ready - Firmware: %s | Camera: %s | Printer: %s",
             firmware_version,
             camera->initialized ? "✓" : "✗",
             g_thermal_printer ? "✓" : "✗");

    // Launch WiFi connection handler in a separate task
    xTaskCreate(wifi_wait_task, "WiFiWaitTask", 4096, (void *)g_wifi, 5, NULL);

    // Main loop handles status and log uploads
    uint32_t loop_count = 0;
    uint32_t log_send_counter = 0;
    uint32_t log_upload_interval_s = settings->settings.log_upload_interval;
    uint32_t log_send_ticks = (log_upload_interval_s * 1000) / 10000;

    while (1)
    {
        // Only try to send logs if WiFi is connected
        if (g_wifi && g_wifi->connected) {
            log_send_counter++;

            // Send logs based on settings interval (if enabled)
            if (g_log_manager && settings->settings.log_upload_enabled)
            {
                int queued = g_log_manager->get_queued_count(g_log_manager);

                // If there are queued logs, send them immediately without waiting for interval
                if (queued > 0 && log_send_counter >= log_send_ticks)
                {
                    log_send_counter = 0; // Reset counter

                    // Keep sending batches until queue is empty
                    while (queued > 0)
                    {
                        esp_err_t log_result = g_log_manager->send_logs(g_log_manager);

                        // Flash red if log upload fails
                        if (log_result != ESP_OK && g_led_ring)
                        {
                            // ... (log failure flash)
                            break; 
                        }
                        queued = g_log_manager->get_queued_count(g_log_manager);
                    }
                }
            }
        }

        // Log status every minute (6 x 10s)
        if (loop_count % 6 == 0) {
             ESP_LOGI(TAG, "System running... (uptime: %lu minutes, WiFi: %s)", 
                     loop_count, (g_wifi && g_wifi->connected) ? "Connected" : "Connecting...");
        }
        loop_count++;

        vTaskDelay(10000 / portTICK_PERIOD_MS); // Wait 10 seconds
    }


    // Cleanup (never reached, but good practice)
    if (g_thermal_printer)
        thermal_printer_destroy(g_thermal_printer);
    if (g_log_manager)
        log_manager_destroy(g_log_manager);
    rotary_encoder_destroy(rotary);
    remote_control_destroy(remote_control);
    http_client_destroy(http_client);
    wifi_destroy(g_wifi);
    camera_destroy(camera);
    led_ring_destroy(led_ring);
    led_destroy(led);
    settings_manager_destroy(settings);
}
