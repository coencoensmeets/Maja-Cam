#ifndef SETTINGS_MANAGER_H
#define SETTINGS_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// Secrets structure - sensitive data stored separately
typedef struct
{
    char wifi_ssid[32];
    char wifi_password[64];
    bool configured;
} secrets_t;

// Settings structure - general configuration
typedef struct
{
    // Camera settings
    uint8_t camera_quality;    // JPEG quality (0-63, lower = higher quality)
    uint16_t camera_framesize; // Frame size (e.g., FRAMESIZE_VGA)
    bool camera_flip_h;        // Horizontal flip
    bool camera_flip_v;        // Vertical flip
    uint16_t camera_rotation;  // Rotation in degrees (0, 90, 180, 270)
    int8_t camera_brightness;  // -2 to 2
    int8_t camera_contrast;    // -2 to 2
    int8_t camera_saturation;  // -2 to 2

    // System settings
    char device_name[32];        // Device hostname
    bool led_enabled;            // Enable/disable status LED
    uint8_t led_ring_brightness; // LED ring brightness (0-100%)
    uint8_t led_ring_count;      // Number of LEDs in ring
    uint8_t led_ring_red;        // LED ring color - Red (0-255)
    uint8_t led_ring_green;      // LED ring color - Green (0-255)
    uint8_t led_ring_blue;       // LED ring color - Blue (0-255)
    uint16_t http_port;          // Web server port

    // LED ring hardware settings
    bool led_ring_enabled;       // Enable/disable LED ring
    uint8_t led_ring_data_pin;   // Data pin for LED ring (GPIO number)

    // Rotary encoder hardware settings
    bool encoder_enabled;        // Enable/disable rotary encoder
    uint8_t encoder_clk_pin;     // CLK pin (GPIO number)
    uint8_t encoder_dt_pin;      // DT pin (GPIO number)
    uint8_t encoder_sw_pin;      // Switch/button pin (GPIO number)

    // Server settings
    char server_upload_url[256];     // Flask server upload URL
    bool server_upload_enabled;      // Enable/disable auto upload
    uint32_t server_upload_interval; // Upload interval in seconds
    uint32_t server_poll_interval;   // Polling interval in milliseconds

    // Thermal printer settings
    bool printer_enabled;         // Enable/disable thermal printer
    uint8_t printer_uart_port;    // UART port number (1 or 2)
    uint8_t printer_tx_pin;       // TX pin (GPIO number)
    uint8_t printer_rx_pin;       // RX pin (GPIO number)
    int8_t printer_rts_pin;       // RTS pin (GPIO number, -1 if not used)
    uint32_t printer_baud_rate;   // Baud rate (typically 9600)
    uint8_t printer_max_width;    // Maximum characters per line
    // ESC 7 heating parameters — primary lever for print darkness.
    uint8_t printer_max_heating_dots; // 0-255; heated dots = 8 × (n+1)
    uint8_t printer_heating_time;     // 3-255; units of 10µs
    uint8_t printer_heating_interval; // 0-255; units of 10µs
    // DC2 # density — separate control from ESC 7, often the stronger one.
    uint8_t printer_density;          // 0-31; darkness = 50% + 5% × n
    uint8_t printer_break_time;       // 0-7; units of 250µs
    // Extra-pass darkening — costs time, not peak current.
    bool printer_bold;                // ESC E emphasized
    bool printer_double_strike;       // ESC G second pass per line
    // Whether the printer understands ESC 7 / DC2 #. Off for EM205-class
    // controllers, which print the argument bytes instead of acting on them.
    bool printer_heating_commands;
    // Pause after each printed line, ms (100-3000). Firmware-side, so it works
    // no matter what the controller supports.
    uint16_t printer_line_delay_ms;

    // Camera feature settings
    bool self_timer_enabled;      // Enable/disable 5-second self-timer countdown
    bool flash_enabled;           // Enable/disable camera flash (LED ring white flash)
    bool auto_print_enabled;      // Enable/disable automatic poem printing after capture

    // Poem settings
    char poem_style[32];          // Poem generation style (general, shakespeare, dickinson, etc.)

    // Log settings
    bool log_upload_enabled;      // Enable/disable log uploading to server
    uint32_t log_upload_interval; // Log upload interval in seconds
    uint16_t log_queue_size;      // Maximum number of logs to queue (10-1000)

    // OTA update settings
    char ota_github_owner[64];    // GitHub repository owner
    char ota_github_repo[64];     // GitHub repository name
    char ota_testing_branch[32];  // Branch name for testing builds
    uint8_t ota_update_channel;   // 0=Release, 1=Testing
    bool ota_auto_check;          // Auto-check for updates on startup

    // Version
    uint32_t version; // Settings version
} app_settings_t;

// Settings Manager "Class"
typedef struct SettingsManager_t
{
    app_settings_t settings;
    secrets_t secrets;
    bool initialized;
    bool spiffs_mounted;

    // Methods
    esp_err_t (*init)(struct SettingsManager_t *self);
    esp_err_t (*load_settings)(struct SettingsManager_t *self);
    esp_err_t (*save_settings)(struct SettingsManager_t *self);
    esp_err_t (*load_secrets)(struct SettingsManager_t *self);
    esp_err_t (*save_secrets)(struct SettingsManager_t *self);
    esp_err_t (*reset_to_defaults)(struct SettingsManager_t *self);
    esp_err_t (*clear_all)(struct SettingsManager_t *self);

    // WiFi settings (secrets)
    esp_err_t (*set_wifi_credentials)(struct SettingsManager_t *self,
                                      const char *ssid, const char *password);
    esp_err_t (*get_wifi_credentials)(struct SettingsManager_t *self,
                                      char *ssid, char *password);
    bool (*has_wifi_credentials)(struct SettingsManager_t *self);

    // Camera settings
    esp_err_t (*set_camera_quality)(struct SettingsManager_t *self, uint8_t quality);
    esp_err_t (*set_camera_framesize)(struct SettingsManager_t *self, uint16_t framesize);
    esp_err_t (*set_camera_flip)(struct SettingsManager_t *self, bool h_flip, bool v_flip);
    esp_err_t (*set_camera_rotation)(struct SettingsManager_t *self, uint16_t rotation);
    esp_err_t (*set_camera_brightness)(struct SettingsManager_t *self, int8_t brightness);

    // System settings
    esp_err_t (*set_device_name)(struct SettingsManager_t *self, const char *name);
    esp_err_t (*set_led_enabled)(struct SettingsManager_t *self, bool enabled);
    esp_err_t (*set_led_ring_brightness)(struct SettingsManager_t *self, uint8_t brightness);
    esp_err_t (*set_led_ring_count)(struct SettingsManager_t *self, uint8_t count);
    esp_err_t (*set_led_ring_color)(struct SettingsManager_t *self, uint8_t red, uint8_t green, uint8_t blue);

    // Server settings
    esp_err_t (*set_server_upload_url)(struct SettingsManager_t *self, const char *url);
    esp_err_t (*set_server_upload_enabled)(struct SettingsManager_t *self, bool enabled);
    esp_err_t (*set_server_upload_interval)(struct SettingsManager_t *self, uint32_t interval);
    esp_err_t (*set_server_poll_interval)(struct SettingsManager_t *self, uint32_t interval_ms);

    // File operations
    esp_err_t (*export_settings_json)(struct SettingsManager_t *self, char **json_str);
    esp_err_t (*import_settings_json)(struct SettingsManager_t *self, const char *json_str);

    // Utility
    void (*print)(struct SettingsManager_t *self);
    bool (*file_exists)(struct SettingsManager_t *self, const char *path);
} SettingsManager_t;

// Constructor
SettingsManager_t *settings_manager_create(void);

// Destructor
void settings_manager_destroy(SettingsManager_t *manager);

// File paths
#define SETTINGS_FILE_PATH "/spiffs/settings.json"
#define SECRETS_FILE_PATH "/spiffs/secrets.json"

// Default values
#define SETTINGS_VERSION 1
#define DEFAULT_DEVICE_NAME "ESP32-Camera"
#define DEFAULT_HTTP_PORT 80
#define DEFAULT_CAMERA_QUALITY 12
#define DEFAULT_CAMERA_FRAMESIZE 10 // FRAMESIZE_SVGA
#define DEFAULT_CAMERA_ROTATION 0    // No rotation
#define DEFAULT_CAMERA_BRIGHTNESS 0
#define DEFAULT_CAMERA_CONTRAST 0
#define DEFAULT_CAMERA_SATURATION 0
#define DEFAULT_LED_RING_BRIGHTNESS 10
#define DEFAULT_LED_RING_COUNT 8
#define DEFAULT_LED_RING_RED 255
#define DEFAULT_LED_RING_GREEN 255
#define DEFAULT_LED_RING_BLUE 255
#define DEFAULT_LED_RING_ENABLED true
#define DEFAULT_LED_RING_DATA_PIN 1       // GPIO 1 (matches settings.json)
#define DEFAULT_ENCODER_ENABLED true
#define DEFAULT_ENCODER_CLK_PIN 48        // GPIO 48 (matches settings.json)
#define DEFAULT_ENCODER_DT_PIN 21         // GPIO 21 (matches settings.json)
#define DEFAULT_ENCODER_SW_PIN 20         // GPIO 20 (matches settings.json)
#define DEFAULT_SERVER_UPLOAD_URL "http://192.168.178.119:5000/api/capture"  // Corrected: your current IP
#define DEFAULT_SERVER_UPLOAD_ENABLED true
#define DEFAULT_SERVER_UPLOAD_INTERVAL 30
#define DEFAULT_SERVER_POLL_INTERVAL 500
#define DEFAULT_PRINTER_ENABLED true     // Corrected: disabled by default (matches settings.json)
#define DEFAULT_PRINTER_UART_PORT 2       // Corrected: UART2 (matches settings.json)
#define DEFAULT_PRINTER_TX_PIN 41         // Corrected: TX on GPIO 41 (matches settings.json)
#define DEFAULT_PRINTER_RX_PIN 42         // Corrected: RX on GPIO 42 (matches settings.json)
#define DEFAULT_PRINTER_RTS_PIN 2
#define DEFAULT_PRINTER_BAUD_RATE 9600
// Below the head's 32-character capacity at font A, for a margin either side
// of the poem rather than edge-to-edge text.
#define DEFAULT_PRINTER_MAX_WIDTH 24
// ESC 7 heating parameters. Values confirmed good on the actual hardware
// rather than derived from a reference design: 9 -> 80 dots fired at once,
// 1200µs heating, 1000µs recovery between dot groups.
#define DEFAULT_PRINTER_MAX_HEATING_DOTS 9
#define DEFAULT_PRINTER_HEATING_TIME 120
#define DEFAULT_PRINTER_HEATING_INTERVAL 100
// DC2 # density. 10 = 100% darkness, 3 = 750µs break between lines.
#define DEFAULT_PRINTER_DENSITY 10
#define DEFAULT_PRINTER_BREAK_TIME 3
// On by default: these darken without raising peak current, which is the only
// headroom available when the printer shares a supply.
#define DEFAULT_PRINTER_BOLD true
#define DEFAULT_PRINTER_DOUBLE_STRIKE true
// Off: at least one of ESC 7 / DC2 # is echoed as text by this printer rather
// than executed, which surfaces as junk characters ahead of the first line.
// Whether either is genuinely honoured is still unconfirmed. The five values
// above are inert while this is off.
#define DEFAULT_PRINTER_HEATING_COMMANDS false
// 1500ms between lines: long enough for the rail to recover on a shared supply,
// which is what alternating dark/light rows point at.
#define DEFAULT_PRINTER_LINE_DELAY_MS 1500
#define DEFAULT_SELF_TIMER_ENABLED true
#define DEFAULT_FLASH_ENABLED true
#define DEFAULT_AUTO_PRINT_ENABLED false
#define DEFAULT_POEM_STYLE "general"
#define DEFAULT_LOG_UPLOAD_ENABLED true
#define DEFAULT_LOG_UPLOAD_INTERVAL 30    // 30 seconds
#define DEFAULT_LOG_QUEUE_SIZE 50         // Queue up to 50 log entries (reduced from 100 to prevent overflow)
#define DEFAULT_OTA_GITHUB_OWNER "coencoensmeets"
#define DEFAULT_OTA_GITHUB_REPO "Maja-Cam"
#define DEFAULT_OTA_TESTING_BRANCH "main"
#define DEFAULT_OTA_UPDATE_CHANNEL 0      // 0=Release, 1=Testing
#define DEFAULT_OTA_AUTO_CHECK false

#endif // SETTINGS_MANAGER_H
