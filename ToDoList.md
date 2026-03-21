# Maja-Cam Development To-Do List

This document tracks known shortcomings, performance bottlenecks, and architectural issues that need to be addressed to improve the robustness and speed of the Maja-Cam firmware.

## 📷 1. Camera Performance & Lag

- [ ] **Remove "Stale Buffer" workaround:** In `main/camera.c` (`camera_capture_impl`), remove the logic that intentionally grabs a frame, discards it, and sleeps for ~50ms before capturing the real frame.
- [ ] **Optimize `grab_mode`:** Evaluate changing the camera's `grab_mode` from `CAMERA_GRAB_WHEN_EMPTY` to `CAMERA_GRAB_LATEST` so the driver automatically manages serving the freshest frame without manual discard logic.

## 💾 2. Memory & Network Robustness

- [ ] **Fix HTTP POST Memory Fragmentation:** In `main/http_client.c`, stop using `malloc(total_len)` to copy the entire JPEG into a single heap chunk just for multipart headers. This causes severe heap fragmentation and crashing.
- [ ] **Implement Chunked Transfer/Streaming:** Rewrite the HTTP upload logic to stream the image directly from the camera buffer (PSRAM/DMA) using `esp_http_client_write()` to safely upload files without loading them entirely into system RAM.

## ⏱️ 3. Boot & Loading Time

- [ ] **Remove Startup Flushing Loop:** In `main/camera.c` (`camera_init_impl`), remove the blocking `for` loop that captures and drops 10 frames with `vTaskDelay(pdMS_TO_TICKS(50))`. This will shave a guaranteed 500ms off the boot time.
- [x] **Non-blocking Wi-Fi Connections:** Modify `main/wifi_manager.c` (`wifi_wait_for_connection_retry_impl`). Remove the hard reboot (`esp_restart()`) that is triggered when Wi-Fi fails to connect after 20 seconds. Implement a gracious fallback that allows the camera/UI tasks to continue functioning offline while Wi-Fi retries in the background.

## 🏗️ 4. Architecture Enhancements

- [ ] **Replace HTTP Polling with Real-Time Protocols:** The `main/remote_control.c` currently polls `/api/remote-status` every 500ms. This degrades performance, consumes battery/power, and limits response times. Transition to **WebSockets** or **MQTT** for instant, push-based remote control.
- [ ] **Migrate SPIFFS to LittleFS:** The project currently uses SPIFFS for `settings.json` and logs. SPIFFS lacks robust wear-leveling, meaning frequent file writes will degrade the flash partition quickly. Migrate the partition table and `VFS` configuration from SPIFFS to `esp_littlefs`.
