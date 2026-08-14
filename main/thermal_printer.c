#include "thermal_printer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "THERMAL_PRINTER";

// ESC/POS Commands
#define ESC 0x1B
#define GS  0x1D

// Initialize printer
static esp_err_t thermal_printer_init_impl(ThermalPrinter_t *self)
{
    ESP_LOGI(TAG, "Initializing thermal printer on UART%d", self->config.uart_port);
    ESP_LOGI(TAG, "TX: GPIO%d, RX: GPIO%d, RTS: GPIO%d, Baud: %d", 
             self->config.tx_pin, self->config.rx_pin, self->config.rts_pin, self->config.baud_rate);
    
    // Configure UART
    uart_config_t uart_config = {
        .baud_rate = self->config.baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    esp_err_t err;
    
    // Install UART driver with larger buffers
    err = uart_driver_install(self->config.uart_port, 2048, 2048, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install UART driver: %s", esp_err_to_name(err));
        return err;
    }
    
    // Configure UART parameters
    err = uart_param_config(self->config.uart_port, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure UART: %s", esp_err_to_name(err));
        uart_driver_delete(self->config.uart_port);
        return err;
    }
    
    // Set UART pins (including RTS if specified)
    gpio_num_t rts = (self->config.rts_pin >= 0) ? self->config.rts_pin : UART_PIN_NO_CHANGE;
    err = uart_set_pin(self->config.uart_port, 
                      self->config.tx_pin, 
                      self->config.rx_pin,
                      rts, 
                      UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set UART pins: %s", esp_err_to_name(err));
        uart_driver_delete(self->config.uart_port);
        return err;
    }
    
    // Flush any pending data
    uart_flush(self->config.uart_port);
    uart_flush_input(self->config.uart_port);
    
    // Wait for printer to be ready
    vTaskDelay(pdMS_TO_TICKS(250));

    // Send printer initialization command
    uint8_t init_cmd[] = {ESC, '@'}; // Initialize printer
    ESP_LOGI(TAG, "Sending ESC @ (init command): 0x%02X 0x%02X", init_cmd[0], init_cmd[1]);
    uart_write_bytes(self->config.uart_port, (const char *)init_cmd, sizeof(init_cmd));
    uart_wait_tx_done(self->config.uart_port, pdMS_TO_TICKS(200));
    
    vTaskDelay(pdMS_TO_TICKS(250)); // Wait for reset to complete

    // Try setting international character set (might help)
    uint8_t intl_cmd[] = {ESC, 'R', 0x00}; // USA
    ESP_LOGI(TAG, "Setting international charset: ESC R 0");
    uart_write_bytes(self->config.uart_port, (const char *)intl_cmd, sizeof(intl_cmd));
    uart_wait_tx_done(self->config.uart_port, pdMS_TO_TICKS(200));
    
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // Disable Chinese character mode explicitly
    uint8_t chinese_off[] = {0x1C, 0x26}; // Cancel Chinese character mode (FS &)
    ESP_LOGI(TAG, "Disabling Chinese mode: 0x1C 0x26");
    uart_write_bytes(self->config.uart_port, (const char *)chinese_off, sizeof(chinese_off));
    uart_wait_tx_done(self->config.uart_port, pdMS_TO_TICKS(200));
    
    vTaskDelay(pdMS_TO_TICKS(200));
    
    self->initialized = true;

    // ESC 7 / DC2 # only when the printer is known to understand them. On a
    // controller that doesn't, these emit nothing useful and leave their
    // argument bytes in the line buffer to be printed as junk ahead of the
    // first real line. See printer_config_t.heating_commands.
    if (self->config.heating_commands) {
        self->set_print_density(self,
                                self->config.max_heating_dots,
                                self->config.heating_time,
                                self->config.heating_interval);
        vTaskDelay(pdMS_TO_TICKS(100));

        self->set_density(self, self->config.density, self->config.break_time);
        vTaskDelay(pdMS_TO_TICKS(100));
    } else {
        ESP_LOGI(TAG, "Heating/density commands disabled (printer does not support ESC 7 / DC2 #)");
    }

    // Applied as a global baseline so all text inherits it, rather than being
    // toggled per string. Both darken by re-firing dots instead of driving
    // them harder, so they add print time but no peak current.
    self->set_bold(self, self->config.bold);
    self->set_double_strike(self, self->config.double_strike);
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "Thermal printer initialized successfully (ready for printing)");

    return ESP_OK;
}

// Print text without line feed
static esp_err_t thermal_printer_print_text_impl(ThermalPrinter_t *self, const char *text)
{
    if (!self->initialized) {
        ESP_LOGE(TAG, "Printer not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!text) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Log what we're sending to the printer
    ESP_LOGI(TAG, "PRINT: \"%s\"", text);
    
    int bytes_written = uart_write_bytes(self->config.uart_port, text, strlen(text));
    ESP_LOGI(TAG, "Wrote %d bytes to UART", bytes_written);
    
    if (bytes_written < 0) {
        ESP_LOGE(TAG, "UART write error!");
        return ESP_FAIL;
    }
    
    // Wait for UART to finish transmitting
    esp_err_t err = uart_wait_tx_done(self->config.uart_port, pdMS_TO_TICKS(1000));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART TX timeout: %s", esp_err_to_name(err));
    }
    
    return ESP_OK;
}

// Print text with line feed
static esp_err_t thermal_printer_print_line_impl(ThermalPrinter_t *self, const char *text)
{
    if (!self->initialized) {
        ESP_LOGE(TAG, "Printer not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (text) {
        // Log what we're sending to the printer
        ESP_LOGI(TAG, "PRINT LINE: \"%s\"", text);
        int bytes_written = uart_write_bytes(self->config.uart_port, text, strlen(text));
        ESP_LOGI(TAG, "Wrote %d bytes to UART", bytes_written);
    }
    // Use \r\n for better printer compatibility
    uart_write_bytes(self->config.uart_port, "\r\n", 2);
    
    // Wait for UART to finish transmitting
    esp_err_t err = uart_wait_tx_done(self->config.uart_port, pdMS_TO_TICKS(1000));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART TX timeout: %s", esp_err_to_name(err));
    }
    
    // Let the printer finish this line — and, on a shared supply, let the rail
    // recover — before the next one is sent. Read from config on every call so
    // a live settings push takes effect on the next line, not the next boot.
    uint16_t delay_ms = self->config.line_delay_ms;
    if (delay_ms < 100) delay_ms = 100;
    if (delay_ms > 3000) delay_ms = 3000;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));

    return ESP_OK;
}

// Feed paper lines
static esp_err_t thermal_printer_feed_lines_impl(ThermalPrinter_t *self, int lines)
{
    if (!self->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    for (int i = 0; i < lines; i++) {
        uart_write_bytes(self->config.uart_port, "\r\n", 2);
        vTaskDelay(pdMS_TO_TICKS(50)); // Give printer time to feed
    }
    
    return ESP_OK;
}

// Set text alignment
static esp_err_t thermal_printer_set_align_impl(ThermalPrinter_t *self, int align)
{
    if (!self->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    uint8_t cmd[] = {ESC, 'a', (uint8_t)(align & 0x03)};
    uart_write_bytes(self->config.uart_port, (const char *)cmd, sizeof(cmd));
    
    return ESP_OK;
}

// Set bold text
static esp_err_t thermal_printer_set_bold_impl(ThermalPrinter_t *self, bool enabled)
{
    if (!self->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    uint8_t cmd[] = {ESC, 'E', enabled ? 1 : 0};
    uart_write_bytes(self->config.uart_port, (const char *)cmd, sizeof(cmd));

    // Logged so the boot output shows the darkness baseline, same as the two
    // density commands — otherwise there's no way to tell from the log whether
    // bold actually got applied.
    ESP_LOGI(TAG, "Bold: %s", enabled ? "ON" : "OFF");
    return ESP_OK;
}

// Enable/disable double-strike (ESC G)
static esp_err_t thermal_printer_set_double_strike_impl(ThermalPrinter_t *self, bool enabled)
{
    if (!self->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t cmd[] = {ESC, 'G', enabled ? 1 : 0};
    int written = uart_write_bytes(self->config.uart_port, (const char *)cmd, sizeof(cmd));
    if (written < 0) {
        ESP_LOGE(TAG, "Failed to write double-strike command");
        return ESP_FAIL;
    }
    uart_wait_tx_done(self->config.uart_port, pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "Double-strike: %s", enabled ? "ON" : "OFF");
    return ESP_OK;
}

// Set text size
static esp_err_t thermal_printer_set_size_impl(ThermalPrinter_t *self, int width, int height)
{
    if (!self->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Limit to valid range (1-8)
    if (width < 1) width = 1;
    if (width > 8) width = 8;
    if (height < 1) height = 1;
    if (height > 8) height = 8;
    
    uint8_t size = ((width - 1) << 4) | (height - 1);
    uint8_t cmd[] = {GS, '!', size};
    uart_write_bytes(self->config.uart_port, (const char *)cmd, sizeof(cmd));
    
    return ESP_OK;
}

// Tune heating parameters (ESC 7 n1 n2 n3). The factory default on most cheap
// 58mm thermal printers is roughly (7, 80, 2), which prints noticeably faint.
// Bumping heating_time is the primary lever for darkness.
static esp_err_t thermal_printer_set_print_density_impl(ThermalPrinter_t *self,
                                                        uint8_t max_heating_dots,
                                                        uint8_t heating_time,
                                                        uint8_t heating_interval)
{
    if (!self->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t cmd[] = {ESC, '7', max_heating_dots, heating_time, heating_interval};
    int written = uart_write_bytes(self->config.uart_port, (const char *)cmd, sizeof(cmd));
    if (written < 0) {
        ESP_LOGE(TAG, "Failed to write print-density command");
        return ESP_FAIL;
    }
    uart_wait_tx_done(self->config.uart_port, pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "Set print density: max_dots=%u, heating_time=%u (%uµs), interval=%u (%uµs)",
             max_heating_dots, heating_time, heating_time * 10,
             heating_interval, heating_interval * 10);
    return ESP_OK;
}

// Set printing density (DC2 #). Packs density into bits 0-4 and break time
// into bits 5-7 of a single byte.
static esp_err_t thermal_printer_set_density_impl(ThermalPrinter_t *self,
                                                  uint8_t density,
                                                  uint8_t break_time)
{
    if (!self->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // Clamp to the field widths so an out-of-range setting can't corrupt the
    // neighbouring bits and silently set a wildly wrong density.
    if (density > 31) {
        ESP_LOGW(TAG, "density %u out of range (0-31), clamping to 31", density);
        density = 31;
    }
    if (break_time > 7) {
        ESP_LOGW(TAG, "break_time %u out of range (0-7), clamping to 7", break_time);
        break_time = 7;
    }

    uint8_t cmd[] = {0x12, '#', (uint8_t)((break_time << 5) | density)};
    int written = uart_write_bytes(self->config.uart_port, (const char *)cmd, sizeof(cmd));
    if (written < 0) {
        ESP_LOGE(TAG, "Failed to write DC2 # density command");
        return ESP_FAIL;
    }
    uart_wait_tx_done(self->config.uart_port, pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "Set density: density=%u (%u%%), break_time=%u (%uµs), byte=0x%02X",
             density, 50 + 5 * density, break_time, break_time * 250, cmd[2]);
    return ESP_OK;
}

// Cut paper
static esp_err_t thermal_printer_cut_paper_impl(ThermalPrinter_t *self)
{
    if (!self->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Feed some paper before cutting
    thermal_printer_feed_lines_impl(self, 4);
    
    // Full cut command (if supported)
    uint8_t cmd[] = {GS, 'V', 0};
    uart_write_bytes(self->config.uart_port, (const char *)cmd, sizeof(cmd));
    
    vTaskDelay(pdMS_TO_TICKS(500)); // Wait for cut
    
    return ESP_OK;
}

// Destroy printer object
static void thermal_printer_destroy_impl(ThermalPrinter_t *self)
{
    if (self) {
        if (self->initialized) {
            uart_driver_delete(self->config.uart_port);
        }
        free(self);
    }
}

// Constructor
ThermalPrinter_t *thermal_printer_create(printer_config_t config)
{
    ThermalPrinter_t *printer = malloc(sizeof(ThermalPrinter_t));
    if (!printer) {
        ESP_LOGE(TAG, "Failed to allocate memory for printer");
        return NULL;
    }
    
    // Set configuration
    printer->config = config;
    printer->initialized = false;
    
    // Bind methods
    printer->init = thermal_printer_init_impl;
    printer->print_text = thermal_printer_print_text_impl;
    printer->print_line = thermal_printer_print_line_impl;
    printer->feed_lines = thermal_printer_feed_lines_impl;
    printer->set_align = thermal_printer_set_align_impl;
    printer->set_bold = thermal_printer_set_bold_impl;
    printer->set_size = thermal_printer_set_size_impl;
    printer->cut_paper = thermal_printer_cut_paper_impl;
    printer->set_print_density = thermal_printer_set_print_density_impl;
    printer->set_density = thermal_printer_set_density_impl;
    printer->set_double_strike = thermal_printer_set_double_strike_impl;
    printer->destroy = thermal_printer_destroy_impl;
    
    return printer;
}

// Destructor
void thermal_printer_destroy(ThermalPrinter_t *printer)
{
    if (printer && printer->destroy) {
        printer->destroy(printer);
    }
}

// Wrap one line of text to max_width and print it.
//
// Every character of `text` is printed. Words are broken at spaces where a
// space is available, and mid-word when one is not: a run longer than the line
// width (a phone number spelled out in hyphenated words, say) used to be
// truncated to the width with the remainder silently discarded. The only
// characters not carried over to the next line are the spaces at a break
// point, which would otherwise print as a ragged left edge.
static void print_wrapped_line(ThermalPrinter_t *printer, const char *text, int max_width)
{
    if (!text) {
        printer->print_line(printer, "");
        return;
    }

    if (max_width < 1) {
        max_width = 1;
    }

    ESP_LOGI(TAG, "Word-wrapping line (max_width=%d): '%s'", max_width, text);

    // Count leading spaces for indentation
    size_t raw_leading = 0;
    while (text[raw_leading] == ' ') {
        raw_leading++;
    }

    // For thermal printer, reduce indentation by half and limit to max 8 spaces
    int first_indent = (int)(raw_leading / 2);
    if (first_indent > 8) {
        first_indent = 8;
    }

    // Get the actual text content (skip leading spaces)
    const char *content = text + raw_leading;
    size_t len = strlen(content);

    if (len == 0) {
        printer->print_line(printer, "");
        return;
    }

    // Wrapped remainder sits two columns in from the line's own indent, so a
    // run-on reads as a continuation rather than as a new verse line.
    int cont_indent = first_indent + 2;
    if (cont_indent > max_width - 5) {
        cont_indent = (max_width > 5) ? 2 : 0;
    }

    char line[max_width + 1];
    size_t i = 0;
    bool first = true;

    while (i < len) {
        int indent = first ? first_indent : cont_indent;
        if (indent > max_width - 1) {
            indent = 0;
        }
        int avail = max_width - indent;
        if (avail < 1) {
            indent = 0;
            avail = max_width;
        }

        size_t remaining = len - i;
        size_t take;

        if (remaining <= (size_t)avail) {
            take = remaining;
        } else {
            // Last space that lets this line break cleanly. Index `avail` is a
            // legal break point too — that space falls just off the end.
            size_t brk = 0;
            for (size_t j = (size_t)avail; j > 0; j--) {
                if (content[i + j] == ' ') {
                    brk = j;
                    break;
                }
            }
            // No space to break on: fill the line and continue mid-word on the
            // next one rather than dropping the tail.
            take = (brk > 0) ? brk : (size_t)avail;
        }

        memset(line, ' ', (size_t)indent);
        memcpy(line + indent, content + i, take);
        line[indent + take] = '\0';

        // Trailing spaces carry no ink but do shift where a centred line sits.
        for (int k = indent + (int)take - 1; k >= 0 && line[k] == ' '; k--) {
            line[k] = '\0';
        }

        printer->print_line(printer, line);

        i += take;
        // Consume the run of spaces we broke on — and nothing else.
        while (i < len && content[i] == ' ') {
            i++;
        }
        first = false;
    }
}

// Tile a repeating unit out to exactly `width` characters and print it.
//
// Borders are generated rather than written out as fixed-length literals so
// they track max_print_width: narrowing the line narrows the frame with the
// poem body instead of leaving a border wider than the text it frames.
static void print_tiled_line(ThermalPrinter_t *printer, const char *unit, int width)
{
    if (!unit || !*unit || width <= 0) {
        return;
    }

    size_t unit_len = strlen(unit);
    char line[width + 1];
    for (int i = 0; i < width; i++) {
        line[i] = unit[i % unit_len];
    }
    line[width] = '\0';

    // Trailing spaces carry no ink but do shift where the printer centres the
    // line, so a pattern whose unit ends in spaces would sit off to the left.
    for (int i = width - 1; i >= 0 && line[i] == ' '; i--) {
        line[i] = '\0';
    }

    printer->print_line(printer, line);
}

// Get a random decorative line for poem borders
static void print_random_decorative_border(ThermalPrinter_t *printer) {
    const int width = printer->config.max_print_width;

    // 10 different decorative line options (some multi-line)
    int pattern = rand() % 10;

    switch(pattern) {
        case 0: // Original stars
            print_tiled_line(printer, "~*", width);
            break;
        case 1: // Dashes and equals
            print_tiled_line(printer, "-=", width);
            break;
        case 2: // Simple dots
            print_tiled_line(printer, ".", width);
            break;
        case 3: // Hash marks
            print_tiled_line(printer, "#", width);
            break;
        case 4: // Double lines
            print_tiled_line(printer, "=", width);
            break;
        case 5: // Asterisks
            print_tiled_line(printer, "*", width);
            break;
        case 6: // Plus signs
            print_tiled_line(printer, "+", width);
            break;
        case 7: // Chevrons pattern (multi-line)
            print_tiled_line(printer, "  `   ", width);
            print_tiled_line(printer, "' . ' ", width);
            print_tiled_line(printer, "  `   ", width);
            break;
        case 8: // Vertical bars
            print_tiled_line(printer, "|", width);
            break;
        case 9: // Wave pattern (multi-line)
            print_tiled_line(printer, " .  ' ", width);
            print_tiled_line(printer, "'  .  ", width);
            break;
    }
}

// Print a nicely formatted poem
esp_err_t thermal_printer_print_poem(ThermalPrinter_t *printer, 
                                      const char *title,
                                      const char *poet_style,
                                      const char *poem_text)
{
    if (!printer || !printer->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Printing poem: %s", title ? title : "Untitled");
    
    // Seed random number generator with current time
    srand(time(NULL));
    
    // Store random pattern choice to use same pattern throughout
    int border_pattern = rand() % 10;
    
    // Print decorative top border
    printer->set_align(printer, 1); // Center
    printer->set_size(printer, 1, 1); // Normal size
    printer->feed_lines(printer, 1);
    
    // Use the stored pattern
    srand(border_pattern);
    print_random_decorative_border(printer);
    
    printer->feed_lines(printer, 1);
    
    // Print title if provided
    if (title && strlen(title) > 0) {
        printer->set_bold(printer, true);
        printer->set_size(printer, 1, 1);
        // Wrapped like the body: unwrapped, a long title would run on to the
        // head's own 32-character limit and overhang the poem below it.
        print_wrapped_line(printer, title, printer->config.max_print_width);
        // Restore the configured baseline, not hard-off — otherwise the title
        // would silently disable bold for the whole body below it.
        printer->set_bold(printer, printer->config.bold);
        printer->feed_lines(printer, 1);
    }
    
    // Use the same pattern again
    srand(border_pattern);
    print_random_decorative_border(printer);
    
    printer->feed_lines(printer, 2);
    
    // Print poem text (left aligned)
    printer->set_align(printer, 0); // Left align for poem
    printer->set_size(printer, 1, 1);
    
    if (poem_text) {
        // Print each line of the poem with word wrapping
        // We need to avoid nested strtok, so we'll manually parse lines
        char *poem_copy = strdup(poem_text);
        if (poem_copy) {
            char *current = poem_copy;
            char *line_start = current;
            int line_count = 0;
            
            ESP_LOGI(TAG, "Starting to parse poem text (%d chars total)", strlen(poem_text));
            
            while (*current != '\0') {
                if (*current == '\n') {
                    // Found end of line
                    *current = '\0';
                    line_count++;
                    ESP_LOGI(TAG, "Printing poem line %d: '%s'", line_count, line_start);
                    // Use word wrapping for each line (max_print_width chars)
                    print_wrapped_line(printer, line_start, printer->config.max_print_width);
                    current++;
                    line_start = current;
                } else {
                    current++;
                }
            }
            
            // Print the last line if there's any remaining text
            if (line_start < current && *line_start != '\0') {
                line_count++;
                ESP_LOGI(TAG, "Printing final poem line %d: '%s'", line_count, line_start);
                print_wrapped_line(printer, line_start, printer->config.max_print_width);
            }
            
            ESP_LOGI(TAG, "Finished printing %d lines of poem", line_count);
            free(poem_copy);
        } else {
            ESP_LOGE(TAG, "Failed to allocate memory for poem copy!");
        }
    }
    
    // Print footer
    printer->feed_lines(printer, 2);
    printer->set_align(printer, 1); // Center
    
    // Use the same pattern for footer
    srand(border_pattern);
    print_random_decorative_border(printer);
    
    printer->feed_lines(printer, 4);
    
    ESP_LOGI(TAG, "Poem printed successfully");
    return ESP_OK;
}
