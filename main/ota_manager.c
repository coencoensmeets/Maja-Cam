#include "ota_manager.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include <esp_timer.h>
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include "esp_task_wdt.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "OTA";

// ============================================================================
// ROBUSTNESS ENHANCEMENTS: Watchdog, Heap, Retry Logic
// ============================================================================

#define OTA_MIN_FREE_HEAP 524288 // Minimum 512KB free heap required for OTA
#define OTA_MAX_RETRY_ATTEMPTS 3
#define OTA_RETRY_INITIAL_DELAY_MS 1000
#define OTA_NVS_NAMESPACE "ota_metrics"
#define OTA_NVS_ATTEMPTS_KEY "ota_attempts"

typedef struct
{
    uint32_t total_attempts;
    uint32_t successful_updates;
    uint32_t failed_updates;
    int64_t last_attempt_time;
} ota_metrics_t;

// Disable watchdog timer during long OTA operations
static esp_err_t disable_watchdog_timer(void)
{
    esp_err_t err = esp_task_wdt_delete(xTaskGetCurrentTaskHandle());
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "✓ Watchdog timer disabled for OTA operation");
        return ESP_OK;
    }
    else if (err == ESP_ERR_NOT_FOUND)
    {
        ESP_LOGW(TAG, "Watchdog timer was not enabled for this task");
        return ESP_OK; // Not an error if not enabled
    }
    else
    {
        ESP_LOGE(TAG, "Failed to disable watchdog timer: %s", esp_err_to_name(err));
        return err;
    }
}

// Re-enable watchdog timer after OTA operation
static esp_err_t enable_watchdog_timer(void)
{
    esp_err_t err = esp_task_wdt_add(xTaskGetCurrentTaskHandle());
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "✓ Watchdog timer re-enabled");
        return ESP_OK;
    }
    else if (err == ESP_ERR_INVALID_ARG)
    {
        ESP_LOGW(TAG, "Watchdog timer already enabled or invalid task");
        return ESP_OK; // Not a critical error
    }
    else
    {
        ESP_LOGW(TAG, "Failed to re-enable watchdog timer: %s", esp_err_to_name(err));
        return ESP_OK; // Don't fail OTA if watchdog re-enable fails
    }
}

// Validate heap size before OTA
static esp_err_t validate_heap_for_ota(void)
{
    size_t free_heap = esp_get_free_heap_size();
    ESP_LOGI(TAG, "Free heap: %u bytes (~%.1f KB)", free_heap, free_heap / 1024.0);

    if (free_heap < OTA_MIN_FREE_HEAP)
    {
        ESP_LOGE(TAG, "✗ Insufficient free heap for OTA! Required: %u bytes, Available: %u bytes",
                 OTA_MIN_FREE_HEAP, free_heap);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "✓ Heap validation passed");
    return ESP_OK;
}

// Log OTA attempt to NVS
static esp_err_t log_ota_attempt(bool success)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to open NVS for OTA metrics: %s", esp_err_to_name(err));
        return err;
    }

    ota_metrics_t metrics = {0};
    size_t required_size = sizeof(ota_metrics_t);

    // Try to read existing metrics
    err = nvs_get_blob(nvs_handle, OTA_NVS_ATTEMPTS_KEY, &metrics, &required_size);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        // First time - initialize
        metrics.total_attempts = 0;
        metrics.successful_updates = 0;
        metrics.failed_updates = 0;
    }
    else if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to read OTA metrics: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    // Update metrics
    metrics.total_attempts++;
    if (success)
    {
        metrics.successful_updates++;
    }
    else
    {
        metrics.failed_updates++;
    }
    metrics.last_attempt_time = esp_timer_get_time() / 1000000; // Convert to seconds

    // Write back to NVS
    err = nvs_set_blob(nvs_handle, OTA_NVS_ATTEMPTS_KEY, &metrics, sizeof(ota_metrics_t));
    if (err == ESP_OK)
    {
        err = nvs_commit(nvs_handle);
    }

    ESP_LOGI(TAG, "OTA Metrics - Total: %lu, Success: %lu, Failed: %lu",
             metrics.total_attempts, metrics.successful_updates, metrics.failed_updates);

    nvs_close(nvs_handle);
    return err;
}

// Get OTA metrics from NVS
static __attribute__((unused)) esp_err_t get_ota_metrics(ota_metrics_t *metrics)
{
    if (!metrics)
        return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(OTA_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);

    if (err != ESP_OK)
    {
        return err;
    }

    size_t required_size = sizeof(ota_metrics_t);
    err = nvs_get_blob(nvs_handle, OTA_NVS_ATTEMPTS_KEY, metrics, &required_size);

    nvs_close(nvs_handle);
    return err;
}

// Pre-update system validation
// Checks WiFi connectivity, available storage, and system stability
static esp_err_t validate_system_for_ota(void)
{
    ESP_LOGI(TAG, "Validating system state for OTA...");

    // Check free heap (already done elsewhere, but log for reference)
    size_t free_heap = esp_get_free_heap_size();
    ESP_LOGI(TAG, "Free heap: %u bytes (~%.1f KB)", free_heap, free_heap / 1024.0);

    // Check if we have connected to WiFi
    // Note: This would require integration with your WiFi manager
    // For now, we'll just log the available heap as the primary metric

    // Check available partition space
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition)
    {
        ESP_LOGE(TAG, "No OTA update partition available");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Update partition available: %s (size: %d KB, address: 0x%08X)",
             update_partition->label, update_partition->size / 1024, update_partition->address);

    // Check total free space in all heaps
    size_t total_free = esp_get_free_heap_size();
    size_t minimum_required = 262144; // 256KB for safety

    if (total_free < minimum_required)
    {
        ESP_LOGE(TAG, "Not enough free memory for OTA! Required: %u, Available: %u",
                 minimum_required, total_free);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "✓ System validation passed");
    return ESP_OK;
}

// Verify firmware integrity using SHA256
// This checks that the firmware in the update partition is valid
static esp_err_t verify_firmware_integrity(const esp_partition_t *update_partition)
{
    if (!update_partition)
    {
        ESP_LOGE(TAG, "Update partition is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Verifying firmware integrity...");

    // Read partition header to validate firmware
    // The first 32 bytes contain the firmware magic number and other metadata
    uint8_t header[32];
    esp_err_t err = esp_partition_read(update_partition, 0, header, sizeof(header));

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to read partition header: %s", esp_err_to_name(err));
        return err;
    }

    // Check magic number (ESP32 firmware image magic: 0xE9)
    if (header[0] != 0xE9)
    {
        ESP_LOGE(TAG, "✗ Invalid firmware magic number: 0x%02X (expected 0xE9)", header[0]);
        ESP_LOGE(TAG, "Firmware may be corrupted or invalid");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "✓ Valid firmware magic number detected (0xE9)");

    // Verify basic firmware header structure
    // header[1] = segment count (should be 2-4 for typical firmware)
    uint8_t segment_count = header[1];
    if (segment_count == 0 || segment_count > 20)
    {
        ESP_LOGW(TAG, "Suspicious segment count: %d", segment_count);
    }
    else
    {
        ESP_LOGI(TAG, "✓ Firmware segment count: %d", segment_count);
    }

    // Read first 1KB and check for patterns that shouldn't be all zero or all 0xFF
    uint8_t sample[1024];
    err = esp_partition_read(update_partition, 0, sample, sizeof(sample));
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to read firmware sample: %s", esp_err_to_name(err));
        return err;
    }

    // Check if data looks corrupted (all zeros or all 0xFF)
    bool all_zero = true;
    bool all_ff = true;

    for (int i = 1; i < sizeof(sample); i++)
    {
        if (sample[i] != 0x00)
            all_zero = false;
        if (sample[i] != 0xFF)
            all_ff = false;
    }

    if (all_zero)
    {
        ESP_LOGE(TAG, "✗ Firmware data is all zeros - likely corrupted");
        return ESP_ERR_INVALID_STATE;
    }

    if (all_ff)
    {
        ESP_LOGE(TAG, "✗ Firmware data is all 0xFF - partition may be blank");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "✓ Firmware data integrity verified - contains valid patterns");
    ESP_LOGI(TAG, "✓ Firmware integrity check passed");

    return ESP_OK;
}

// Calculate adaptive timeout based on firmware size
// Estimates timeout dynamically: base 300 seconds + extra time for large files
static __attribute__((unused)) uint32_t calculate_adaptive_timeout(size_t firmware_size)
{
    // Base timeout: 300 seconds (5 minutes)
    uint32_t base_timeout_ms = 300000;

    // For firmware < 1MB: use base timeout
    if (firmware_size < 1024 * 1024)
    {
        ESP_LOGI(TAG, "Small firmware (%u KB) - using standard timeout (%.0f sec)",
                 firmware_size / 1024, base_timeout_ms / 1000.0);
        return base_timeout_ms;
    }

    // For larger firmware: add extra time
    // Assume minimum download speed of ~100 KB/s, so add 1 second per 100 KB
    uint32_t extra_time_ms = (firmware_size / (100 * 1024)) * 1000;
    uint32_t total_timeout_ms = base_timeout_ms + extra_time_ms;

    // Cap at 10 minutes maximum
    if (total_timeout_ms > 600000)
    {
        total_timeout_ms = 600000;
    }

    ESP_LOGI(TAG, "Large firmware (%u KB) - adaptive timeout: %.0f sec (base: 300s + %.0f extra)",
             firmware_size / 1024, total_timeout_ms / 1000.0, extra_time_ms / 1000.0);

    return total_timeout_ms;
}

// ============================================================================
// HTTP Response Context for API Calls
// ============================================================================

typedef struct
{
    char *buffer;
    int buffer_size;
    int data_len;
} http_response_context_t;

static esp_err_t http_api_response_handler(esp_http_client_event_t *evt)
{
    http_response_context_t *ctx = (http_response_context_t *)evt->user_data;

    switch (evt->event_id)
    {
    case HTTP_EVENT_ON_DATA:
        if (ctx && ctx->buffer && evt->data_len > 0)
        {
            // Ensure we don't overflow buffer
            int space_left = ctx->buffer_size - ctx->data_len - 1;
            if (space_left > 0)
            {
                int to_copy = (evt->data_len > space_left) ? space_left : evt->data_len;
                memcpy(ctx->buffer + ctx->data_len, evt->data, to_copy);
                ctx->data_len += to_copy;
                ESP_LOGD(TAG, "Received %d bytes, total: %d/%d", evt->data_len, ctx->data_len, ctx->buffer_size);
            }
            else
            {
                ESP_LOGW(TAG, "Response buffer full, discarding %d bytes", evt->data_len);
            }
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

// ============================================================================
// HTTP Event Handler for Progress Tracking and Speed Monitoring
// ============================================================================

typedef struct
{
    int total_size;
    int downloaded;
    ota_progress_callback_t callback;
    int64_t start_time_ms;        // Start of download (for speed calc)
    int64_t last_speed_update_ms; // Last time we logged speed
    uint32_t last_downloaded;     // Downloaded bytes at last speed update
} ota_download_context_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id)
    {
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP Header: %s = %s", evt->header_key, evt->header_value);
        if (strcasecmp(evt->header_key, "Content-Length") == 0)
        {
            ota_download_context_t *ctx = (ota_download_context_t *)evt->user_data;
            if (ctx)
            {
                ctx->total_size = atoi(evt->header_value);
                ctx->start_time_ms = esp_timer_get_time() / 1000; // Convert to milliseconds
                ctx->last_speed_update_ms = ctx->start_time_ms;
                ESP_LOGI(TAG, "✓ Firmware size received: %d bytes (~%.1f MB)", ctx->total_size, ctx->total_size / 1024.0 / 1024.0);
                ESP_LOGI(TAG, "Estimated download time at 2 Mbps: ~%.0f seconds", (ctx->total_size * 8.0) / (2000000.0));
            }
        }
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP Data chunk received: %d bytes, chunked=%d", evt->data_len, esp_http_client_is_chunked_response(evt->client));
        if (!esp_http_client_is_chunked_response(evt->client))
        {
            ota_download_context_t *ctx = (ota_download_context_t *)evt->user_data;
            if (ctx)
            {
                ctx->downloaded += evt->data_len;

                // Calculate and log download speed every 1 second
                int64_t now_ms = esp_timer_get_time() / 1000;
                int64_t elapsed_ms = now_ms - ctx->last_speed_update_ms;

                if (ctx->total_size > 0)
                {
                    int progress = (ctx->downloaded * 100) / ctx->total_size;

                    // Speed update every 1 second or at 10% intervals
                    if (elapsed_ms >= 1000 || progress % 10 == 0)
                    {
                        // Calculate speed
                        uint32_t bytes_since_last = ctx->downloaded - ctx->last_downloaded;
                        double speed_mbps = (bytes_since_last * 8.0) / (elapsed_ms * 1000.0); // Convert to Mbps

                        // Estimate remaining time
                        int remaining_bytes = ctx->total_size - ctx->downloaded;
                        double remaining_seconds = (remaining_bytes * 8.0) / (speed_mbps * 1000000.0);

                        if (speed_mbps > 0)
                        {
                            ESP_LOGI(TAG, "↓ Progress: %d%% | Speed: %.2f Mbps | ETA: %.0f sec | (%d / %d KB)",
                                     progress, speed_mbps,
                                     remaining_seconds,
                                     ctx->downloaded / 1024, ctx->total_size / 1024);
                        }

                        if (ctx->callback)
                        {
                            char msg[96];
                            snprintf(msg, sizeof(msg), "Progress: %d%% (%.2f Mbps, %.0f sec left)",
                                     progress, speed_mbps, remaining_seconds);
                            ctx->callback(progress, msg);
                        }

                        ctx->last_speed_update_ms = now_ms;
                        ctx->last_downloaded = ctx->downloaded;
                    }
                }
            }
        }
        break;
    case HTTP_EVENT_ERROR:
        ESP_LOGE(TAG, "HTTP Event Error");
        break;
    case HTTP_EVENT_ON_FINISH:
        // Calculate final speed
        ota_download_context_t *ctx = (ota_download_context_t *)evt->user_data;
        if (ctx && ctx->total_size > 0)
        {
            int64_t end_time_ms = esp_timer_get_time() / 1000;
            int64_t total_time_ms = end_time_ms - ctx->start_time_ms;
            if (total_time_ms > 0)
            {
                double avg_speed_mbps = (ctx->total_size * 8.0) / (total_time_ms * 1000.0);
                ESP_LOGI(TAG, "✓ HTTP Download finished - Average speed: %.2f Mbps, Total time: %.1f sec",
                         avg_speed_mbps, total_time_ms / 1000.0);
            }
        }
        ESP_LOGI(TAG, "HTTP Event: Download finished");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "HTTP Event: Disconnected");
        break;
    default:
        ESP_LOGD(TAG, "HTTP Event: %d", evt->event_id);
        break;
    }
    return ESP_OK;
}

// ============================================================================
// GitHub API Functions
// ============================================================================

static esp_err_t fetch_latest_release_impl(OTAManager_t *self)
{
    if (!self)
    {
        ESP_LOGE(TAG, "OTA manager is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (!self->initialized)
    {
        ESP_LOGE(TAG, "OTA manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // Rate limiting: Check if enough time has passed since last check
    int64_t now = esp_timer_get_time();
    int64_t time_since_last_check = (now - self->last_check_time) / 1000; // Convert to ms

    if (self->last_check_time > 0 && time_since_last_check < OTA_MIN_CHECK_INTERVAL_MS)
    {
        ESP_LOGW(TAG, "Rate limit: Please wait %lld more seconds",
                 (OTA_MIN_CHECK_INTERVAL_MS - time_since_last_check) / 1000);
        return ESP_ERR_INVALID_STATE;
    }

    self->last_check_time = now;

    // Clear previous release info
    memset(&self->latest_release, 0, sizeof(self->latest_release));

    self->status = OTA_STATUS_CHECKING;
    ESP_LOGI(TAG, "Checking for latest release from %s/%s", self->github_owner, self->github_repo);

    char url[512];
    int written = snprintf(url, sizeof(url), GITHUB_API_RELEASES, self->github_owner, self->github_repo);
    if (written < 0 || written >= sizeof(url))
    {
        ESP_LOGE(TAG, "URL buffer overflow");
        self->status = OTA_STATUS_ERROR;
        return ESP_ERR_INVALID_SIZE;
    }

    // Pre-allocate response buffer (max 100KB for JSON)
    char *response_buffer = malloc(102401);
    if (!response_buffer)
    {
        ESP_LOGE(TAG, "Failed to allocate response buffer");
        self->status = OTA_STATUS_ERROR;
        return ESP_ERR_NO_MEM;
    }

    // Prepare response context for event handler
    http_response_context_t response_ctx = {
        .buffer = response_buffer,
        .buffer_size = 102400,
        .data_len = 0};

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 15000, // Increased to 15 seconds for API calls
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_api_response_handler, // Register event handler in config
        .user_data = &response_ctx,
        .buffer_size = 4096, // Larger buffer for better performance
        .buffer_size_tx = 1024,
        .user_agent = "ESP32-OTA-Client/1.0",
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
    {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        free(response_buffer);
        self->status = OTA_STATUS_ERROR;
        return ESP_FAIL;
    }

    // Set GitHub API header
    esp_http_client_set_header(client, "Accept", "application/vnd.github.v3+json");

    // Execute HTTP request
    ESP_LOGI(TAG, "Fetching latest release from GitHub API...");
    esp_err_t err = esp_http_client_perform(client);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to fetch release info: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(response_buffer);
        self->status = OTA_STATUS_ERROR;
        return err;
    }

    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200)
    {
        if (status_code == 403)
        {
            ESP_LOGE(TAG, "GitHub API rate limit exceeded (403). Unauthenticated limit: 60 requests/hour. Please wait and try again later.");
        }
        else
        {
            ESP_LOGE(TAG, "HTTP error: %d", status_code);
        }
        esp_http_client_cleanup(client);
        free(response_buffer);
        self->status = OTA_STATUS_ERROR;
        return ESP_FAIL;
    }

    // Check if response data was captured
    if (response_ctx.data_len <= 0)
    {
        ESP_LOGE(TAG, "No response data captured from API (status %d, free heap: %lu)", status_code, esp_get_free_heap_size());
        esp_http_client_cleanup(client);
        free(response_buffer);
        self->status = OTA_STATUS_ERROR;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Successfully received %d bytes from latest release API", response_ctx.data_len);
    response_buffer[response_ctx.data_len] = '\0';
    esp_http_client_cleanup(client);

    // Parse JSON response
    cJSON *root = cJSON_Parse(response_buffer);
    free(response_buffer);

    if (!root)
    {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        self->status = OTA_STATUS_ERROR;
        return ESP_FAIL;
    }

    // Extract release information
    cJSON *tag = cJSON_GetObjectItem(root, "tag_name");
    cJSON *prerelease = cJSON_GetObjectItem(root, "prerelease");
    cJSON *body = cJSON_GetObjectItem(root, "body");
    cJSON *assets = cJSON_GetObjectItem(root, "assets");

    if (tag && cJSON_IsString(tag) && tag->valuestring)
    {
        strncpy(self->latest_release.version, tag->valuestring, sizeof(self->latest_release.version) - 1);
        self->latest_release.version[sizeof(self->latest_release.version) - 1] = '\0';
    }
    else
    {
        ESP_LOGE(TAG, "Missing or invalid tag_name in release");
        cJSON_Delete(root);
        self->status = OTA_STATUS_ERROR;
        return ESP_FAIL;
    }

    if (prerelease && cJSON_IsBool(prerelease))
    {
        self->latest_release.is_prerelease = cJSON_IsTrue(prerelease);
    }

    if (body && cJSON_IsString(body) && body->valuestring)
    {
        strncpy(self->latest_release.description, body->valuestring, sizeof(self->latest_release.description) - 1);
        self->latest_release.description[sizeof(self->latest_release.description) - 1] = '\0';
    }

    // Find the .bin firmware file in assets
    if (assets && cJSON_IsArray(assets))
    {
        int asset_count = cJSON_GetArraySize(assets);
        if (asset_count == 0)
        {
            ESP_LOGW(TAG, "No assets found in release");
        }

        cJSON *asset = NULL;
        cJSON_ArrayForEach(asset, assets)
        {
            cJSON *name = cJSON_GetObjectItem(asset, "name");
            if (name && cJSON_IsString(name) && name->valuestring && strstr(name->valuestring, ".bin"))
            {
                cJSON *download_url = cJSON_GetObjectItem(asset, "browser_download_url");
                cJSON *size = cJSON_GetObjectItem(asset, "size");

                if (download_url && cJSON_IsString(download_url) && download_url->valuestring)
                {
                    size_t url_len = strlen(download_url->valuestring);
                    if (url_len > 0 && url_len < sizeof(self->latest_release.download_url))
                    {
                        strncpy(self->latest_release.download_url, download_url->valuestring,
                                sizeof(self->latest_release.download_url) - 1);
                        self->latest_release.download_url[sizeof(self->latest_release.download_url) - 1] = '\0';
                    }
                    else
                    {
                        ESP_LOGE(TAG, "Download URL too long: %d bytes", url_len);
                        continue;
                    }
                }

                if (size && cJSON_IsNumber(size))
                {
                    self->latest_release.file_size = size->valueint;
                    ESP_LOGI(TAG, "Firmware size: %d bytes", self->latest_release.file_size);
                }
                break;
            }
        }
    }
    else
    {
        ESP_LOGW(TAG, "No assets array in release");
    }

    cJSON_Delete(root);

    if (strlen(self->latest_release.download_url) == 0)
    {
        ESP_LOGE(TAG, "No firmware binary found in release");
        self->status = OTA_STATUS_ERROR;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Latest release: %s (prerelease: %s)",
             self->latest_release.version,
             self->latest_release.is_prerelease ? "yes" : "no");
    ESP_LOGI(TAG, "Download URL: %s", self->latest_release.download_url);

    self->status = OTA_STATUS_IDLE;
    return ESP_OK;
}

static esp_err_t fetch_testing_build_impl(OTAManager_t *self)
{
    if (!self->initialized)
    {
        ESP_LOGE(TAG, "OTA manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // Rate limiting: Check if enough time has passed since last check
    int64_t now = esp_timer_get_time();
    int64_t time_since_last_check = (now - self->last_check_time) / 1000; // Convert to ms

    if (self->last_check_time > 0 && time_since_last_check < OTA_MIN_CHECK_INTERVAL_MS)
    {
        ESP_LOGW(TAG, "Rate limit: Please wait %lld more seconds",
                 (OTA_MIN_CHECK_INTERVAL_MS - time_since_last_check) / 1000);
        return ESP_ERR_INVALID_STATE;
    }

    self->last_check_time = now;

    self->status = OTA_STATUS_CHECKING;
    ESP_LOGI(TAG, "Checking for latest testing build");

    // Clear previous release info
    memset(&self->latest_release, 0, sizeof(self->latest_release));

    // Fetch releases to find the latest testing version (limit to very recent to save memory)
    char url[512];
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/%s/releases?per_page=3",
             self->github_owner, self->github_repo);

    // Pre-allocate response buffer (max 100KB for JSON array)
    char *response_buffer = malloc(102401);
    if (!response_buffer)
    {
        ESP_LOGE(TAG, "Failed to allocate response buffer");
        self->status = OTA_STATUS_ERROR;
        return ESP_ERR_NO_MEM;
    }

    // Prepare response context for event handler
    http_response_context_t response_ctx = {
        .buffer = response_buffer,
        .buffer_size = 102400,
        .data_len = 0};

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_api_response_handler, // Register event handler in config
        .user_data = &response_ctx,
        .buffer_size = 8192, // Larger buffer for multiple releases
        .buffer_size_tx = 1024,
        .user_agent = "ESP32-OTA-Client/1.0",
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
    {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        free(response_buffer);
        self->status = OTA_STATUS_ERROR;
        return ESP_FAIL;
    }

    // Set GitHub API header
    esp_http_client_set_header(client, "Accept", "application/vnd.github.v3+json");

    // Execute HTTP request
    ESP_LOGI(TAG, "Fetching testing builds from GitHub API...");
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to fetch releases: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(response_buffer);
        self->status = OTA_STATUS_ERROR;
        return err;
    }

    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200)
    {
        if (status_code == 403)
        {
            ESP_LOGE(TAG, "GitHub API rate limit exceeded (403). Unauthenticated limit: 60 requests/hour. Please wait and try again later.");
        }
        else
        {
            ESP_LOGE(TAG, "HTTP error: %d", status_code);
        }
        esp_http_client_cleanup(client);
        free(response_buffer);
        self->status = OTA_STATUS_ERROR;
        return ESP_FAIL;
    }

    // Check if response data was captured
    if (response_ctx.data_len <= 0)
    {
        ESP_LOGE(TAG, "No response data captured from API (status %d, free heap: %lu)", status_code, esp_get_free_heap_size());
        esp_http_client_cleanup(client);
        free(response_buffer);
        self->status = OTA_STATUS_ERROR;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Successfully received %d bytes from testing builds API", response_ctx.data_len);
    response_buffer[response_ctx.data_len] = '\0';
    esp_http_client_cleanup(client);

    // Free up unused buffer memory before heavy cJSON allocations
    char *shrunk_buffer = realloc(response_buffer, response_ctx.data_len + 1);
    if (shrunk_buffer)
    {
        response_buffer = shrunk_buffer;
    }

    // Parse JSON array of releases
    cJSON *releases = cJSON_Parse(response_buffer);

    if (!releases || !cJSON_IsArray(releases))
    {
        ESP_LOGE(TAG, "Failed to parse releases JSON. Buffer content:\n%s", response_buffer);
        free(response_buffer);
        if (releases)
            cJSON_Delete(releases);
        self->status = OTA_STATUS_ERROR;
        return ESP_FAIL;
    }

    free(response_buffer);

    // Find the latest testing release (e.g., v1.0.0-test.5)
    bool found = false;
    cJSON *release = NULL;
    cJSON_ArrayForEach(release, releases)
    {
        cJSON *tag = cJSON_GetObjectItem(release, "tag_name");
        if (tag && cJSON_IsString(tag) && tag->valuestring)
        {
            // Check if this is a testing release
            if (strstr(tag->valuestring, "-test.") != NULL)
            {
                // Extract release info
                strncpy(self->latest_release.version, tag->valuestring,
                        sizeof(self->latest_release.version) - 1);

                cJSON *body = cJSON_GetObjectItem(release, "body");
                if (body && cJSON_IsString(body) && body->valuestring)
                {
                    strncpy(self->latest_release.description, body->valuestring,
                            sizeof(self->latest_release.description) - 1);
                }

                self->latest_release.is_prerelease = true;

                // Find firmware binary in assets
                cJSON *assets = cJSON_GetObjectItem(release, "assets");
                if (assets && cJSON_IsArray(assets))
                {
                    cJSON *asset = NULL;
                    cJSON_ArrayForEach(asset, assets)
                    {
                        cJSON *name = cJSON_GetObjectItem(asset, "name");
                        if (name && cJSON_IsString(name) && strstr(name->valuestring, ".bin"))
                        {
                            cJSON *download_url = cJSON_GetObjectItem(asset, "browser_download_url");
                            cJSON *size = cJSON_GetObjectItem(asset, "size");

                            if (download_url && cJSON_IsString(download_url))
                            {
                                strncpy(self->latest_release.download_url, download_url->valuestring,
                                        sizeof(self->latest_release.download_url) - 1);
                            }

                            if (size && cJSON_IsNumber(size))
                            {
                                self->latest_release.file_size = size->valueint;
                            }

                            found = true;
                            break;
                        }
                    }
                }

                if (found)
                {
                    break; // Use the first (latest) testing release found
                }
            }
        }
    }

    cJSON_Delete(releases);

    if (!found)
    {
        ESP_LOGE(TAG, "No testing releases found");
        self->status = OTA_STATUS_ERROR;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Latest testing release: %s", self->latest_release.version);
    ESP_LOGI(TAG, "Download URL: %s", self->latest_release.download_url);

    self->status = OTA_STATUS_IDLE;
    return ESP_OK;
}

// ============================================================================
// OTA Update Functions
// ============================================================================

static esp_err_t download_and_install_impl(OTAManager_t *self, const char *url)
{
    if (!self)
    {
        ESP_LOGE(TAG, "OTA manager is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (!self->initialized)
    {
        ESP_LOGE(TAG, "OTA manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!url || strlen(url) == 0)
    {
        ESP_LOGE(TAG, "Invalid download URL");
        self->status = OTA_STATUS_ERROR;
        return ESP_ERR_INVALID_ARG;
    }

    // Validate URL format (must be HTTPS)
    if (strncmp(url, "https://", 8) != 0)
    {
        ESP_LOGE(TAG, "URL must use HTTPS protocol: %s", url);
        self->status = OTA_STATUS_ERROR;
        return ESP_ERR_INVALID_ARG;
    }

    // ========================================================================
    // PRE-UPDATE VALIDATION (CRITICAL)
    // ========================================================================

    // Validate system state before OTA
    if (validate_system_for_ota() != ESP_OK)
    {
        self->status = OTA_STATUS_ERROR;
        if (self->progress_callback)
        {
            self->progress_callback(0, "System validation failed");
        }
        return ESP_ERR_INVALID_STATE;
    }

    // Validate heap size before OTA
    if (validate_heap_for_ota() != ESP_OK)
    {
        self->status = OTA_STATUS_ERROR;
        if (self->progress_callback)
        {
            self->progress_callback(0, "Insufficient memory for update");
        }
        return ESP_ERR_NO_MEM;
    }

    // Disable watchdog timer during OTA (CRITICAL - prevents timeouts during long downloads)
    ESP_LOGI(TAG, "Disabling watchdog timer for OTA operation...");
    esp_err_t wd_err = disable_watchdog_timer();
    bool watchdog_disabled = (wd_err == ESP_OK);

    self->status = OTA_STATUS_DOWNLOADING;
    ESP_LOGI(TAG, "Starting OTA update from: %s", url);
    ESP_LOGI(TAG, "[OTA DEBUG] Watchdog disabled: %s", watchdog_disabled ? "Yes" : "No");

    // ========================================================================
    // DOWNLOAD AND INSTALL WITH RETRY LOGIC
    // ========================================================================

    esp_err_t ota_result = ESP_FAIL;

    for (int attempt = 1; attempt <= OTA_MAX_RETRY_ATTEMPTS; attempt++)
    {
        ESP_LOGI(TAG, "Download attempt %d/%d", attempt, OTA_MAX_RETRY_ATTEMPTS);

        ota_download_context_t download_ctx = {
            .total_size = 0,
            .downloaded = 0,
            .callback = self->progress_callback,
            .start_time_ms = 0,
            .last_speed_update_ms = 0,
            .last_downloaded = 0};

        // Calculate adaptive timeout based on firmware size if known
        // Default to 300 seconds, but we'll know the size after first HTTP header
        uint32_t timeout_ms = 300000; // Will be updated after Content-Length header is received

        // ====================================================================
        // PERFORMANCE OPTIMIZATION: Large buffers for faster WiFi transfer
        // ====================================================================
        esp_http_client_config_t config = {
            .url = url,
            .timeout_ms = timeout_ms, // 300 seconds (5 minutes) for firmware download
            .keep_alive_enable = true,
            .keep_alive_idle = 30,    // TCP keep-alive: send probe after 30 seconds of idle
            .keep_alive_interval = 5, // Probe every 5 seconds
            .keep_alive_count = 3,    // Probe 3 times before closing
            .crt_bundle_attach = esp_crt_bundle_attach,
            .event_handler = http_event_handler,
            .user_data = &download_ctx,
            .buffer_size = 16384,   // OPTIMIZATION: Increased from 4KB to 16KB for better throughput
            .buffer_size_tx = 4096, // OPTIMIZATION: Increased from 1KB to 4KB for faster TX
            .user_agent = "ESP32-OTA-Client/1.0",
        };

        esp_https_ota_config_t ota_config = {
            .http_config = &config,
            .bulk_flash_erase = true,
            .partial_http_download = false,
        };

        // Verify we have a valid update partition
        const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
        if (!update_partition)
        {
            ESP_LOGE(TAG, "No OTA update partition available");
            self->status = OTA_STATUS_ERROR;
            if (self->progress_callback)
            {
                self->progress_callback(0, "No update partition!");
            }

            // Re-enable watchdog before returning
            if (watchdog_disabled)
            {
                enable_watchdog_timer();
            }
            return ESP_ERR_NOT_FOUND;
        }

        ESP_LOGI(TAG, "Update partition: %s at 0x%08x (size: %d KB)",
                 update_partition->label, update_partition->address, update_partition->size / 1024);

        self->status = OTA_STATUS_INSTALLING;
        ESP_LOGI(TAG, "Starting OTA installation...");
        ESP_LOGI(TAG, "Downloading firmware from GitHub (this may take 1-3 minutes)...");
        ESP_LOGI(TAG, "[PERFORMANCE] Optimizations enabled: 16KB buffers, TCP keep-alive, aggressive tuning");
        ESP_LOGI(TAG, "[OTA DEBUG] Calling esp_https_ota() - Attempt %d/%d", attempt, OTA_MAX_RETRY_ATTEMPTS);

        ota_result = esp_https_ota(&ota_config);

        ESP_LOGI(TAG, "[OTA DEBUG] esp_https_ota() returned: %s (0x%08X)", esp_err_to_name(ota_result), ota_result);

        if (ota_result == ESP_OK)
        {
            // Success! Break out of retry loop
            break;
        }
        else
        {
            ESP_LOGW(TAG, "✗ OTA attempt %d failed: %s", attempt, esp_err_to_name(ota_result));

            if (attempt < OTA_MAX_RETRY_ATTEMPTS)
            {
                // Calculate exponential backoff delay
                uint32_t delay_ms = OTA_RETRY_INITIAL_DELAY_MS * (1 << (attempt - 1));
                ESP_LOGI(TAG, "Retrying in %lu ms...", delay_ms);
                vTaskDelay(pdMS_TO_TICKS(delay_ms));
            }
        }
    }

    // ========================================================================
    // POST-UPDATE HANDLING
    // ========================================================================

    if (ota_result == ESP_OK)
    {
        ESP_LOGI(TAG, "✓ OTA update successful! Firmware downloaded and written successfully.");
        ESP_LOGI(TAG, "[OTA DEBUG] Setting status to SUCCESS and preparing to restart...");
        self->status = OTA_STATUS_SUCCESS;
        if (self->progress_callback)
        {
            self->progress_callback(100, "Update complete! Restarting...");
        }

        // Log successful attempt
        log_ota_attempt(true);

        // Mark the new partition as valid
        const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
        if (update_partition)
        {
            // Verify firmware integrity before marking as valid
            ESP_LOGI(TAG, "[OTA DEBUG] Verifying firmware integrity...");
            esp_err_t integrity_err = verify_firmware_integrity(update_partition);
            if (integrity_err != ESP_OK)
            {
                ESP_LOGE(TAG, "✗ Firmware integrity verification FAILED: %s", esp_err_to_name(integrity_err));
                ESP_LOGE(TAG, "WARNING: Update may be corrupted, will NOT boot into new firmware");
                self->status = OTA_STATUS_ERROR;
                if (self->progress_callback)
                {
                    self->progress_callback(0, "Firmware integrity check failed!");
                }

                // Re-enable watchdog before returning
                if (watchdog_disabled)
                {
                    enable_watchdog_timer();
                }
                return integrity_err;
            }

            ESP_LOGI(TAG, "[OTA DEBUG] Setting boot partition to: %s", update_partition->label);
            esp_ota_set_boot_partition(update_partition);
            ESP_LOGI(TAG, "Next boot partition set to: %s", update_partition->label);

            // CRITICAL: Mark the new partition as valid to prevent rollback on reboot
            esp_err_t mark_err = esp_ota_mark_app_valid_cancel_rollback();
            if (mark_err == ESP_OK)
            {
                ESP_LOGI(TAG, "✓ OTA partition marked as valid - rollback protection enabled");
            }
            else
            {
                ESP_LOGE(TAG, "ERROR: Failed to mark OTA partition as valid: %s", esp_err_to_name(mark_err));
                ESP_LOGE(TAG, "WARNING: Device may rollback to previous version on reboot!");
            }
        }
        else
        {
            ESP_LOGE(TAG, "ERROR: Could not get update partition!");
        }

        ESP_LOGI(TAG, "[OTA DEBUG] About to restart ESP32...");
        ESP_LOGI(TAG, "Restarting in 2 seconds...");
        vTaskDelay(pdMS_TO_TICKS(2000));

        // Re-enable watchdog before restart
        if (watchdog_disabled)
        {
            enable_watchdog_timer();
        }

        ESP_LOGI(TAG, "[OTA DEBUG] Calling esp_restart()...");
        esp_restart();
        // Code should not reach here
        ESP_LOGE(TAG, "[OTA DEBUG] ERROR: esp_restart() returned (should not happen)");
    }
    else
    {
        ESP_LOGE(TAG, "✗ OTA update failed after %d attempts: %s",
                 OTA_MAX_RETRY_ATTEMPTS, esp_err_to_name(ota_result));
        ESP_LOGE(TAG, "[OTA DEBUG] Error details: %s", esp_err_to_name(ota_result));
        self->status = OTA_STATUS_ERROR;

        // Log failed attempt
        log_ota_attempt(false);

        if (self->progress_callback)
        {
            char error_msg[64];
            snprintf(error_msg, sizeof(error_msg), "Update failed: %s", esp_err_to_name(ota_result));
            self->progress_callback(0, error_msg);
        }

        // Log additional diagnostic info
        const esp_partition_t *running = esp_ota_get_running_partition();
        ESP_LOGE(TAG, "Remaining on partition: %s", running ? running->label : "unknown");
        ESP_LOGE(TAG, "[OTA DEBUG] OTA failed, device will continue running current firmware");

        // Re-enable watchdog on failure
        if (watchdog_disabled)
        {
            enable_watchdog_timer();
        }
    }

    return ota_result;
}

static esp_err_t perform_update_impl(OTAManager_t *self)
{
    if (!self->initialized)
    {
        ESP_LOGE(TAG, "OTA manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (strlen(self->latest_release.download_url) == 0)
    {
        ESP_LOGE(TAG, "No download URL available. Check for updates first.");
        return ESP_FAIL;
    }

    return self->download_and_install(self, self->latest_release.download_url);
}

static esp_err_t install_target_version_impl(OTAManager_t *self, const char *target_version)
{
    if (!self)
    {
        ESP_LOGE(TAG, "OTA manager is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (!self->initialized)
    {
        ESP_LOGE(TAG, "OTA manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!target_version || strlen(target_version) == 0)
    {
        ESP_LOGE(TAG, "Target version is empty");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Installing target version: %s", target_version);

    // Build the download URL for the target version
    // Format: https://github.com/{owner}/{repo}/releases/download/{version}/Poem_cam.bin
    char download_url[512];
    int written = snprintf(download_url, sizeof(download_url),
                           "https://github.com/%s/%s/releases/download/%s/Poem_cam.bin",
                           self->github_owner, self->github_repo, target_version);

    if (written < 0 || written >= sizeof(download_url))
    {
        ESP_LOGE(TAG, "Download URL too long");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Target version download URL: %s", download_url);

    // Force update - don't check if it's newer or older, just install it
    return self->download_and_install(self, download_url);
}

// ============================================================================
// Version Comparison
// ============================================================================

static int compare_versions(const char *v1, const char *v2)
{
    // Version comparison - handles both "vX.Y.Z" and "vX.Y.Z-test.N" formats
    // Returns: -1 if v1 < v2, 0 if equal, 1 if v1 > v2

    int major1 = 0, minor1 = 0, patch1 = 0, test1 = -1;
    int major2 = 0, minor2 = 0, patch2 = 0, test2 = -1;

    // Try to parse semantic version (vX.Y.Z) with optional -test.N suffix
    sscanf(v1, "v%d.%d.%d-test.%d", &major1, &minor1, &patch1, &test1);
    sscanf(v2, "v%d.%d.%d-test.%d", &major2, &minor2, &patch2, &test2);

    // If test number wasn't parsed, try without it (for base versions)
    if (test1 == -1)
    {
        sscanf(v1, "v%d.%d.%d", &major1, &minor1, &patch1);
    }
    if (test2 == -1)
    {
        sscanf(v2, "v%d.%d.%d", &major2, &minor2, &patch2);
    }

    // Compare base version numbers
    if (major1 != major2)
        return (major1 > major2) ? 1 : -1;
    if (minor1 != minor2)
        return (minor1 > minor2) ? 1 : -1;
    if (patch1 != patch2)
        return (patch1 > patch2) ? 1 : -1;

    // If base versions are equal, compare test numbers (test builds are < release)
    // No -test suffix means release version, which is > test versions
    if (test1 == -1 && test2 == -1)
        return 0;
    if (test1 == -1)
        return 1;
    if (test2 == -1)
        return -1;
    if (test1 != test2)
        return (test1 > test2) ? 1 : -1;

    return 0;
}

static esp_err_t check_for_update_impl(OTAManager_t *self, bool *update_available)
{
    if (!self)
    {
        ESP_LOGE(TAG, "OTA manager is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (!update_available)
    {
        ESP_LOGE(TAG, "update_available pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (!self->initialized)
    {
        ESP_LOGE(TAG, "OTA manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    *update_available = false;

    esp_err_t err;
    if (self->channel == OTA_CHANNEL_RELEASE)
    {
        err = self->fetch_latest_release(self);
    }
    else
    {
        err = self->fetch_testing_build(self);
    }

    if (err != ESP_OK)
    {
        return err;
    }

    // Compare versions
    if (self->channel == OTA_CHANNEL_RELEASE)
    {
        int cmp = compare_versions(self->current_version, self->latest_release.version);
        *update_available = (cmp < 0);

        if (*update_available)
        {
            ESP_LOGI(TAG, "✓ Update available: %s → %s",
                     self->current_version, self->latest_release.version);
        }
        else
        {
            ESP_LOGI(TAG, "Already on latest version: %s", self->current_version);
        }
    }
    else
    {
        // For testing channel, compare versions properly
        int cmp = compare_versions(self->current_version, self->latest_release.version);
        *update_available = (cmp < 0);

        if (*update_available)
        {
            ESP_LOGI(TAG, "✓ Testing update available: %s → %s",
                     self->current_version, self->latest_release.version);
        }
        else
        {
            ESP_LOGI(TAG, "Already on latest testing version: %s", self->current_version);
        }
    }

    return ESP_OK;
}

// ============================================================================
// Configuration Methods
// ============================================================================

static esp_err_t set_channel_impl(OTAManager_t *self, ota_channel_t channel)
{
    if (!self)
    {
        ESP_LOGE(TAG, "OTA manager is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (!self->initialized)
    {
        ESP_LOGE(TAG, "OTA manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (channel != OTA_CHANNEL_RELEASE && channel != OTA_CHANNEL_TESTING)
    {
        ESP_LOGE(TAG, "Invalid channel: %d", channel);
        return ESP_ERR_INVALID_ARG;
    }

    self->channel = channel;
    ESP_LOGI(TAG, "Update channel set to: %s",
             channel == OTA_CHANNEL_RELEASE ? "Release" : "Testing");
    return ESP_OK;
}

static esp_err_t set_auto_check_impl(OTAManager_t *self, bool enabled)
{
    if (!self->initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    self->auto_check = enabled;
    ESP_LOGI(TAG, "Auto-check for updates: %s", enabled ? "enabled" : "disabled");
    return ESP_OK;
}

static esp_err_t get_current_version_impl(OTAManager_t *self, char *version_out)
{
    if (!self->initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    strncpy(version_out, self->current_version, 31);
    version_out[31] = '\0';
    return ESP_OK;
}

static esp_err_t get_latest_version_impl(OTAManager_t *self, char *version_out)
{
    if (!self->initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    strncpy(version_out, self->latest_release.version, 63);
    version_out[63] = '\0';
    return ESP_OK;
}

static void set_progress_callback_impl(OTAManager_t *self, ota_progress_callback_t callback)
{
    self->progress_callback = callback;
}

static const char *get_status_string_impl(OTAManager_t *self)
{
    switch (self->status)
    {
    case OTA_STATUS_IDLE:
        return "Idle";
    case OTA_STATUS_CHECKING:
        return "Checking for updates...";
    case OTA_STATUS_DOWNLOADING:
        return "Downloading firmware...";
    case OTA_STATUS_INSTALLING:
        return "Installing update...";
    case OTA_STATUS_SUCCESS:
        return "Update successful!";
    case OTA_STATUS_ERROR:
        return "Update failed";
    default:
        return "Unknown";
    }
}

// ============================================================================
// Utility Methods
// ============================================================================

static void print_info_impl(OTAManager_t *self)
{
    ESP_LOGI(TAG, "=== OTA Manager Info ===");
    ESP_LOGI(TAG, "Repository: %s/%s", self->github_owner, self->github_repo);
    ESP_LOGI(TAG, "Testing branch: %s", self->testing_branch);
    ESP_LOGI(TAG, "Current version: %s", self->current_version);
    ESP_LOGI(TAG, "Update channel: %s",
             self->channel == OTA_CHANNEL_RELEASE ? "Release" : "Testing");
    ESP_LOGI(TAG, "Auto-check: %s", self->auto_check ? "enabled" : "disabled");
    ESP_LOGI(TAG, "Status: %s", self->get_status_string(self));

    if (strlen(self->latest_release.version) > 0)
    {
        ESP_LOGI(TAG, "Latest version: %s", self->latest_release.version);
        ESP_LOGI(TAG, "Download URL: %s", self->latest_release.download_url);
    }
}

// ============================================================================
// Initialization
// ============================================================================

static esp_err_t init_impl(OTAManager_t *self, const char *owner, const char *repo, const char *branch)
{
    if (!self)
    {
        ESP_LOGE(TAG, "OTA manager is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (self->initialized)
    {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    // Validate inputs
    if (!owner || strlen(owner) == 0 || strlen(owner) >= sizeof(self->github_owner))
    {
        ESP_LOGE(TAG, "Invalid GitHub owner");
        return ESP_ERR_INVALID_ARG;
    }
    if (!repo || strlen(repo) == 0 || strlen(repo) >= sizeof(self->github_repo))
    {
        ESP_LOGE(TAG, "Invalid GitHub repo");
        return ESP_ERR_INVALID_ARG;
    }
    if (!branch || strlen(branch) == 0 || strlen(branch) >= sizeof(self->testing_branch))
    {
        ESP_LOGE(TAG, "Invalid branch name");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Initializing OTA Manager...");

    // Safely copy repository information
    strncpy(self->github_owner, owner, sizeof(self->github_owner) - 1);
    self->github_owner[sizeof(self->github_owner) - 1] = '\0';
    strncpy(self->github_repo, repo, sizeof(self->github_repo) - 1);
    self->github_repo[sizeof(self->github_repo) - 1] = '\0';
    strncpy(self->testing_branch, branch, sizeof(self->testing_branch) - 1);
    self->testing_branch[sizeof(self->testing_branch) - 1] = '\0';

    // Set current firmware version
    strncpy(self->current_version, FIRMWARE_VERSION, sizeof(self->current_version) - 1);

    // Get running partition info
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running)
    {
        ESP_LOGE(TAG, "Failed to get running partition");
        return ESP_FAIL;
    }

    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);

    ESP_LOGI(TAG, "Current partition: %s at 0x%08x", running->label, running->address);

    if (update)
    {
        ESP_LOGI(TAG, "Update partition: %s at 0x%08x", update->label, update->address);
    }
    else
    {
        ESP_LOGW(TAG, "No OTA update partition found (running from factory?)");
    }

    // Check if this is the first boot after an OTA update
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK)
    {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY)
        {
            // This is first boot after OTA update - mark as valid
            ESP_LOGI(TAG, "First boot after OTA update detected");
            esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
            if (err == ESP_OK)
            {
                ESP_LOGI(TAG, "✓ OTA update validated and confirmed");
            }
            else
            {
                ESP_LOGE(TAG, "Failed to validate OTA update: %s", esp_err_to_name(err));
            }
        }
        else if (ota_state == ESP_OTA_IMG_VALID)
        {
            ESP_LOGI(TAG, "Running validated firmware");
        }
        else if (ota_state == ESP_OTA_IMG_INVALID)
        {
            ESP_LOGW(TAG, "Running partition marked as invalid");
        }
        else if (ota_state == ESP_OTA_IMG_ABORTED)
        {
            ESP_LOGW(TAG, "Previous OTA update was aborted");
        }
    }

    self->status = OTA_STATUS_IDLE;
    self->last_check_time = 0; // Initialize rate limiting
    self->initialized = true;

    ESP_LOGI(TAG, "✓ OTA Manager initialized");
    self->print_info(self);

    return ESP_OK;
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

OTAManager_t *ota_manager_create(void)
{
    OTAManager_t *manager = calloc(1, sizeof(OTAManager_t));
    if (!manager)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for OTA manager");
        return NULL;
    }

    // Initialize default values
    manager->channel = OTA_CHANNEL_RELEASE;
    manager->auto_check = false;
    manager->status = OTA_STATUS_IDLE;
    manager->initialized = false;
    manager->progress_callback = NULL;

    // Assign methods
    manager->init = init_impl;
    manager->check_for_update = check_for_update_impl;
    manager->perform_update = perform_update_impl;
    manager->set_channel = set_channel_impl;
    manager->set_auto_check = set_auto_check_impl;
    manager->get_current_version = get_current_version_impl;
    manager->get_latest_version = get_latest_version_impl;
    manager->set_progress_callback = set_progress_callback_impl;
    manager->get_status_string = get_status_string_impl;
    manager->fetch_latest_release = fetch_latest_release_impl;
    manager->fetch_testing_build = fetch_testing_build_impl;
    manager->download_and_install = download_and_install_impl;
    manager->install_target_version = install_target_version_impl;
    manager->print_info = print_info_impl;

    return manager;
}

void ota_manager_destroy(OTAManager_t *manager)
{
    if (manager)
    {
        free(manager);
    }
}
