#ifndef THERMAL_PRINTER_H
#define THERMAL_PRINTER_H

#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include <stdbool.h>

/**
 * @file thermal_printer.h
 * @brief Thermal printer driver for EM205 and compatible printers
 */

// Forward declaration
typedef struct ThermalPrinter ThermalPrinter_t;

// Printer configuration
typedef struct {
    uart_port_t uart_port;      // UART port (UART_NUM_1 or UART_NUM_2)
    gpio_num_t tx_pin;           // TX pin (ESP32 -> Printer RX)
    gpio_num_t rx_pin;           // RX pin (ESP32 <- Printer TX)
    gpio_num_t rts_pin;          // RTS pin (optional, set to -1 if not used)
    int baud_rate;               // Baud rate (default 9600)
    int max_print_width;         // Maximum characters per line (default 32)
    // ESC 7 heating parameters applied during init (see set_print_density).
    uint8_t max_heating_dots;    // 0-255; heated dots = 8 × (n+1)
    uint8_t heating_time;        // 3-255; units of 10µs
    uint8_t heating_interval;    // 0-255; units of 10µs
    // DC2 # parameters applied during init (see set_density). Independent of
    // ESC 7 above: this scales how dark each fired dot comes out, where ESC 7
    // controls the heating pulse itself.
    uint8_t density;             // 0-31; darkness = 50% + 5% × n
    uint8_t break_time;          // 0-7; units of 250µs
    // Darkness via extra passes rather than extra power. Both re-fire the same
    // dots instead of driving them harder, so they cost print time but not
    // peak current — the right lever when the supply is the limit.
    bool bold;                   // ESC E — emphasized
    bool double_strike;          // ESC G — each line struck twice
    // ESC 7 / DC2 # are CSN-A2 dialect and not universal — a controller that
    // lacks them swallows the escape prefix and prints the argument bytes as
    // text instead, surfacing as junk ahead of the first line. At least one of
    // the two behaves that way on this hardware, so the gate defaults off.
    // While off, the five heating/density fields above have no effect at all.
    bool heating_commands;       // send ESC 7 / DC2 # at init
    // Pause after each printed line. Purely a firmware-side wait, so it works
    // regardless of what the controller supports — the lever of last resort
    // when the supply can't sustain consecutive lines (symptom: alternating
    // dark/light rows). 100-3000ms.
    uint16_t line_delay_ms;
} printer_config_t;

// Printer object structure
struct ThermalPrinter {
    // Configuration
    printer_config_t config;
    
    // State
    bool initialized;
    
    // Methods
    esp_err_t (*init)(ThermalPrinter_t *self);
    esp_err_t (*print_text)(ThermalPrinter_t *self, const char *text);
    esp_err_t (*print_line)(ThermalPrinter_t *self, const char *text);
    esp_err_t (*feed_lines)(ThermalPrinter_t *self, int lines);
    esp_err_t (*set_align)(ThermalPrinter_t *self, int align); // 0=left, 1=center, 2=right
    esp_err_t (*set_bold)(ThermalPrinter_t *self, bool enabled);
    esp_err_t (*set_size)(ThermalPrinter_t *self, int width, int height); // 1-8
    esp_err_t (*cut_paper)(ThermalPrinter_t *self);
    /**
     * Tune the printer's heating parameters (ESC 7). Higher heating_time and
     * max_heating_dots produce darker output at the cost of slower printing
     * and higher peak current draw.
     *
     *   max_heating_dots: 0-255. Heated dots per line = 8 × (n+1). Higher =
     *     darker but draws more current at once (printer can brown out if PSU
     *     is weak).
     *   heating_time:     3-255 (units of 10µs). Higher = darker but slower.
     *     Common dark-but-safe value: ~150-200.
     *   heating_interval: 0-255 (units of 10µs). Recovery time between heating
     *     pulses. Increase if you push max_heating_dots high.
     */
    esp_err_t (*set_print_density)(ThermalPrinter_t *self,
                                   uint8_t max_heating_dots,
                                   uint8_t heating_time,
                                   uint8_t heating_interval);
    /**
     * Set printing density (DC2 #). This is a separate control from the ESC 7
     * heating parameters above and on many of these controllers has the larger
     * effect on darkness — ESC 7 sets how hard each dot is driven, this sets
     * the density the controller targets.
     *
     *   density:    0-31. Darkness = 50% + 5% × n, so 10 = 100%. Above ~14 the
     *     text darkens further but edges start to bleed.
     *   break_time: 0-7 (units of 250µs). Pause between print lines; raise it
     *     if the supply sags on long passages.
     */
    esp_err_t (*set_density)(ThermalPrinter_t *self,
                             uint8_t density,
                             uint8_t break_time);
    /**
     * Enable double-strike (ESC G): the head makes a second pass over each
     * line, re-firing the same dots. Roughly doubles darkness for double the
     * print time, with no change to peak current — unlike density/heating,
     * which darken by drawing more power at once.
     *
     * Not universally implemented on cheap controllers; one that lacks it
     * ignores the command silently rather than erroring, so pair it with bold
     * (ESC E) which has much broader support.
     */
    esp_err_t (*set_double_strike)(ThermalPrinter_t *self, bool enabled);
    void (*destroy)(ThermalPrinter_t *self);
};

/**
 * @brief Create thermal printer object
 * 
 * @param config Printer configuration
 * @return ThermalPrinter_t* Pointer to printer object or NULL on failure
 */
ThermalPrinter_t *thermal_printer_create(printer_config_t config);

/**
 * @brief Destroy thermal printer object
 * 
 * @param printer Pointer to printer object
 */
void thermal_printer_destroy(ThermalPrinter_t *printer);

/**
 * @brief Print a poem with nice formatting
 * 
 * @param printer Pointer to printer object
 * @param title Title of the poem (e.g., "Sunset at the Beach")
 * @param poet_style Poet style (e.g., "Shakespeare", "Haiku")
 * @param poem_text The poem text
 * @return esp_err_t ESP_OK on success
 */
esp_err_t thermal_printer_print_poem(ThermalPrinter_t *printer, 
                                      const char *title,
                                      const char *poet_style,
                                      const char *poem_text);

#endif // THERMAL_PRINTER_H
