# Maja-Cam

**Maja-Cam** is an ESP32-S3 embedded firmware project designed to operate as a smart, connected camera. It captures images, manages local network configurations, interfaces with local peripherals, and integrates seamlessly with a Flask-based backend for remote control and image storage.

---

## ?? Key Features

*   **Smart Image Capture:** High-fidelity JPEG capture using the OV2640 camera module.
*   **Flask Backend Integration:** Automatically uploads captured images to a central API (`/api/capture`).
*   **Over-The-Air (OTA) Updates:** Seamless remote firmware upgrades through the custom Flask backend using dual fallback application partitions.
*   **Remote Remote Control:** Periodically polls a remote dashboard (`/api/remote-status`) to trigger local actions.
*   **Easy WiFi Setup:** Supports pre-configured credentials or an intuitive Captive Portal fallback (`ESP32-Camera-Setup`).
*   **Local Hardware UI:** Navigate onboard menus via a Rotary Encoder and view system status via an Addressable LED Ring.
*   **Physical Interactivity:** UART Thermal Printer support to print physical tickets and receipts.
*   **Persistent Configuration:** JSON-based configuration management stored securely in the ESP32's SPIFFS filesystem.

---

## ??? Hardware Requirements

*   **Microcontroller:** ESP32-S3 (with FreeRTOS)
*   **Camera Module:** OV2640
*   **Input:** Rotary Encoder (for local menu navigation)
*   **Output:** 
    *   Addressable LED Ring (WS2812B or similar)
    *   Thermal Printer (via UART)
*   **Cable:** USB-UART cable for programming and power

---

## ?? Quick Start

### 1. Initial Setup

Clone the repository and set up your local configuration files.

```bash
# Create the WiFi credentials file from the template
cp data/secrets.json.example data/secrets.json
```

Edit `data/secrets.json` to include your default Wi-Fi network:
```json
{
  "wifi_ssid": "YourNetwork",
  "wifi_password": "YourPassword"
}
```

Optional: Adjust your default camera parameters in `data/settings.json`.

### 2. Build and Flash

Ensure you have **ESP-IDF v5.0+** installed and sourced in your terminal.

```bash
# Build the application, flash to the board, and open the serial monitor
idf.py fullclean build flash monitor
```

### 3. Usage & Access

*   **Pre-configured:** If `secrets.json` matches an available network, the ESP32 will connect automatically. Check the serial monitor for the assigned IP address, and visit `http://<ESP32-IP>` in your browser.
*   **Captive Portal:** If no network is found, connect to the `ESP32-Camera-Setup` WiFi access point (Password: `setupesp32`), and a captive portal will prompt you to enter credentials.

---

## ??? Architecture overview

Maja-Cam relies on FreeRTOS to handle multiple concurrent tasks without blocking:

1.  **Main Task:** Master state machine and peripheral initialization.
2.  **Camera Task:** Captures frames from the OV2640.
3.  **WiFi/HTTP Tasks:** Manages network connections and local HTTP streaming.
4.  **Upload/Remote Tasks:** Handles Flask backend integration (`POST` images, `GET` commands).
5.  **UI Tasks:** Manages Rotary Encoder inputs, Thermal Printer processes, and LED Ring animations.
6.  **OTA Manager:** Background orchestration of seamless Over-The-Air firmware updates.

SPIFFS partitions are utilized to store system settings (`settings.json`) and run logs (`app.log`) persistently across reboots. Dual application partitions (`ota_0` and `ota_1`) are utilized to ensure safe remote firmware version rollbacks on boot failures.

---

## ?? Documentation

For more detailed technical information, please refer to the documents in the `docs/` folder:

*   [API Reference](docs/API_REFERENCE.md)
*   [Architecture](docs/ARCHITECTURE.md)
*   [SPIFFS Usage](docs/SPIFFS.md)
*   [Flask Integration](docs/FLASK_INTEGRATION.md)
*   [OTA Update System](docs/OTA_UPDATE_SYSTEM.md)
*   [OTA Safety Features](docs/OTA_SAFETY_FEATURES.md)

---

## ?? Troubleshooting

*   **Camera Initialization Failed:** Check physical pins and ribbon cables. Ensure OV2640 is used.
*   **WiFi Disconnects:** Verify it is a 2.4GHz network. The ESP32 does not support 5GHz natively.
*   **Flash Errors:** Ensure the device is securely plugged in and the proper serial driver is active. If partition errors occur, run `idf.py erase-flash`.

