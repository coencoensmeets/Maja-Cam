# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

---

## Project Overview

**Maja-Cam-C-Code** is an ESP32-S3 embedded firmware project featuring an OV2640 camera, WiFi connectivity, JSON configuration management, and integration with the Flask backend for image upload and remote control.

---

## Development Commands

### Build and Flash

```bash
# Full clean rebuild and flash
idf.py fullclean build flash monitor

# Quick rebuild (incremental)
idf.py build flash monitor

# Monitor serial output only
idf.py monitor

# Build without flashing
idf.py build

# Erase entire flash and rebuild
idf.py erase-flash && idf.py build flash monitor

# Flash bootloader only
idf.py bootloader-flash

# Clean build artifacts
idf.py clean
```

### Configuration

```bash
# Interactive menuconfig
idf.py menuconfig

# Set specific component options
idf.py build -DCONFIG_OPTION=VALUE
```

### Debugging

```bash
# View serial logs with timestamps
idf.py monitor -m goanna

# Monitor with line ending conversion
idf.py monitor --print-filter="I" --line-ending=CR

# Flash and monitor with debug output
idf.py build flash monitor -v
```

---

## Setup & Configuration

### Initial Setup

1. **Create secrets file:**
   ```bash
   cp data/secrets.json.example data/secrets.json
   ```

2. **Edit WiFi credentials:**
   ```json
   {
     "wifi_ssid": "YourNetwork",
     "wifi_password": "YourPassword"
   }
   ```

3. **Verify GPIO pins** (if using non-standard hardware):
   - Edit `main/camera.h` for camera pins
   - Edit `main/main.h` for other hardware assignments

4. **Build and flash:**
   ```bash
   idf.py fullclean build flash monitor
   ```

### Configuration Files

**`data/secrets.json`** (WiFi credentials)

```json
{
  "wifi_ssid": "YourNetwork",
  "wifi_password": "YourPassword"
}
```

**`data/settings.json`** (Camera & system settings)

```json
{
  "camera_resolution": 13,
  "camera_quality": 12,
  "camera_brightness": 0,
  "camera_contrast": 0,
  "camera_saturation": 0,
  "system_led_enabled": true,
  "system_hostname": "esp32-camera"
}
```

Modify these files before flashing to customize behavior.

---

## Architecture

### Project Structure

```
Maja-Cam-C-Code/
├── main/
│   ├── main.c                      # FreeRTOS task management
│   ├── camera.c/h                  # OV2640 camera driver
│   ├── web_server.c/h              # HTTP server for streaming
│   ├── wifi_manager.c/h            # WiFi connection logic
│   ├── wifi_provisioning.c/h       # Captive portal setup
│   ├── http_client.c/h             # Upload to Flask backend
│   ├── settings_manager.c/h        # JSON config management
│   ├── thermal_printer.c/h         # Thermal printer integration
│   ├── led.c/h & led_ring.c/h      # LED indicators
│   ├── remote_control.c/h          # Flask polling for commands
│   ├── rotary_encoder.c/h          # Input device support
│   ├── log_manager.c/h             # Application logging
│   ├── main_menu.c/h               # UI menu system
│   └── CMakeLists.txt              # Component build config
│
├── data/
│   ├── secrets.json                # WiFi credentials (gitignored)
│   ├── secrets.json.example        # Template
│   ├── settings.json               # Camera settings
│   └── settings.json.example       # Template
│
├── docs/
│   ├── API_REFERENCE.md
│   ├── ARCHITECTURE.md
│   ├── ORGANIZATION.md
│   └── SPIFFS.md
│
├── partitions.csv                  # Partition table (960KB SPIFFS)
├── CMakeLists.txt                  # Top-level build config
└── sdkconfig                       # Menuconfig output
```

### Task Architecture (FreeRTOS)

The firmware runs multiple concurrent tasks:

1. **Main Task** - Initialization and state machine
2. **Camera Task** - Capture images
3. **WiFi Task** - Connection management
4. **HTTP Server Task** - Web streaming
5. **Upload Task** - Send images to Flask backend
6. **Remote Control Task** - Poll Flask for commands
7. **LED Task** - Status indicators
8. **UI Task** - Menu and user input

Tasks communicate via queues and shared state.

---

## Key Components

### Camera Module (`main/camera.c`)

```c
// Initialize OV2640
esp_err_t camera_init(camera_config_t *config);

// Capture frame
camera_fb_t *esp_camera_fb_get();
void esp_camera_fb_return(camera_fb_t *fb);

// Adjust settings
void camera_set_quality(uint8_t quality);
void camera_set_brightness(int8_t brightness);
```

### WiFi Manager (`main/wifi_manager.c`)

Handles WiFi connection with fallback to provisioning portal:

```c
void wifi_init_sta();      // Connect to saved credentials
void wifi_start_ap();      // Start provisioning portal
bool is_wifi_connected();
```

### HTTP Client (`main/http_client.c`)

Uploads captured images to Flask backend:

```c
void upload_image_to_flask(uint8_t *jpg_buf, size_t jpg_len);
// POSTs to /api/capture endpoint
```

### Settings Manager (`main/settings_manager.c`)

Reads/writes JSON configuration from SPIFFS:

```c
cJSON *settings_load();
void settings_save(cJSON *settings);
int settings_get_int(cJSON *root, const char *key, int default_val);
```

### LED Ring (`main/led_ring.c`)

Controls addressable LED indicators:

```c
void led_ring_init(gpio_num_t pin, int num_leds);
void led_ring_set_color(int index, uint8_t r, uint8_t g, uint8_t b);
void led_ring_update();
```

---

## File System (SPIFFS)

Configuration stored in SPIFFS partition (960KB):

```
/data/
  ├── secrets.json
  ├── settings.json
  └── logs/
      └── app.log
```

Access via SPIFFS API:

```c
FILE *f = fopen("/data/settings.json", "r");
if (f) {
    char buf[512];
    fread(buf, 1, sizeof(buf), f);
    fclose(f);
}
```

---

## WiFi Setup

### Option 1: Pre-configured (Recommended)

Edit `data/secrets.json` before flashing:

```json
{
  "wifi_ssid": "YourNetwork",
  "wifi_password": "YourPassword"
}
```

Then flash normally. Credentials load automatically.

### Option 2: Provisioning Portal

If no credentials found:

1. ESP32 creates WiFi AP: **ESP32-Camera-Setup**
2. Connect to this network (password: `setupesp32`)
3. Browser auto-opens to `http://192.168.4.1`
4. Enter credentials in web form
5. ESP32 saves and reboots

---

## Integration with Flask Backend

### Image Upload

```c
// POST multipart image to Flask
void upload_image_to_flask(uint8_t *jpg_buf, size_t jpg_len) {
    // Sends to: http://flask-server:5000/api/capture
    // As multipart/form-data with image file
}
```

### Remote Control

```c
// Poll Flask for commands
void remote_control_task() {
    while (1) {
        // Fetch commands from /api/remote-status
        // Execute local actions
        vTaskDelay(pdMS_TO_TICKS(5000));  // 5-second polling
    }
}
```

---

## GPIO Pin Configuration

Edit in header files if using different pins:

**`main/camera.h`** - OV2640 pins
**`main/main.h`** - Other peripherals (LED, rotary encoder, printer, etc.)

Default configuration:
- D0-D7: Camera data pins
- VSYNC, HSYNC, PCLK: Camera control
- SDA/SCL: I2C for camera
- Rotary: GPIO for encoder input
- LED: GPIO for LED ring output

---

## Building and Flashing

### Prerequisites

- ESP-IDF v5.0+ installed
- USB-UART cable connected
- Proper drivers installed

### Build Process

```bash
# 1. Configure
idf.py menuconfig

# 2. Build
idf.py build

# 3. Flash
idf.py flash

# 4. Monitor
idf.py monitor
```

**All in one:**
```bash
idf.py fullclean build flash monitor
```

---

## Serial Monitor Output

Expected startup sequence:

```
I (xxx) main: Starting camera initialization...
I (xxx) camera: OV2640 detected
I (xxx) wifi_manager: Starting WiFi...
I (xxx) wifi_manager: WiFi connected to YourNetwork
I (xxx) web_server: Starting HTTP server on port 80
I (xxx) http_client: Ready to upload images
```

### Debug Output

Enable verbose logging in `main/main.c`:

```c
#define DEBUG_LEVEL 3  // 0=none, 3=verbose
```

---

## Common Issues

**Camera initialization failed:**
- Check ribbon cable connections are secure
- Verify OV2640 model (not OV7670 or other)
- Ensure sufficient power supply
- Check GPIO pins in camera.h match hardware

**WiFi won't connect:**
- Verify 2.4GHz WiFi (5GHz not supported)
- Check signal strength (move closer to router)
- Verify credentials in secrets.json are correct
- Look for provisioning portal if credentials missing

**SPIFFS not found:**
- Ensure partitions.csv present in project root
- Run `idf.py fullclean` to regenerate
- Verify SPIFFS partition size (960KB)

**Upload to Flask fails:**
- Check Flask backend is running
- Verify network connectivity
- Check HTTP client is using correct URL
- Monitor serial output for POST errors

**Compilation errors:**
- Run `idf.py clean && idf.py build`
- Verify ESP-IDF version (v5.0+)
- Update component.lock: `idf.py update-dependencies`
- Check for circular includes in .h files

**Memory issues:**
- Monitor heap usage: `idf.py monitor | grep heap`
- Reduce image resolution in settings.json
- Increase task stack sizes if needed
- Profile with `idf.py monitor -m esp32` (gdb)

---

## Troubleshooting Workflow

1. **Check serial output** - Look for error messages and stack traces
2. **Verify configuration** - Check secrets.json and settings.json
3. **Test connectivity** - Ping ESP32 from computer
4. **Monitor resources** - Check heap and task status
5. **Rebuild from clean** - `idf.py fullclean build flash monitor`

---

## Git Repository

This is a standalone git repository with independent version control from the parent project.

---

## Key Files to Know

| File | Purpose |
|------|---------|
| `main/main.c` | Task management and state machine |
| `main/camera.c` | Camera driver initialization |
| `main/http_client.c` | Flask backend uploads |
| `main/wifi_manager.c` | WiFi and provisioning |
| `main/settings_manager.c` | JSON config I/O |
| `partitions.csv` | Flash partition layout |
| `data/secrets.json` | WiFi credentials |
| `sdkconfig` | Build configuration |

---

## Hardware Requirements

- **ESP32-S3** development board
- **OV2640** camera module
- **USB-UART cable** for programming and debugging
- Optional: **LED ring** (WS2812B or similar)
- Optional: **Thermal printer** (via UART)
- Optional: **Rotary encoder** for UI input

---

## Next Steps

1. Copy `data/secrets.json.example` → `data/secrets.json`
2. Add your WiFi credentials
3. Connect ESP32 via USB
4. Run `idf.py fullclean build flash monitor`
5. Monitor serial output for startup messages
6. Access at `http://<ESP32-IP>` (or `http://192.168.4.1` for provisioning portal)

