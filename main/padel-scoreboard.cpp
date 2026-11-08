// Padel scoreboard: shows "1" or "2" for the two 433 MHz remote buttons.
//
// Button identities are not stored: the first distinct code received after
// boot is button 1, the next different code is button 2, anything else is
// ignored.
//
// Deliberately low power: a single 5x7 digit (~10-15 LEDs), and OE runs at
// CONFIG_LED_MATRIX_BRIGHTNESS (default 50/255).

#include <Arduino.h>
#include <DMD2.h>
#include "fonts/SystemFont5x7.h"

#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "rx433.h"
#include "sdkconfig.h"

// A held button repeats its frame; frames of the same code closer together
// than this count as one press.
static constexpr uint32_t PRESS_GAP_MS = 250;

static const char *TAG = "matrix";

#ifdef CONFIG_LED_MATRIX_OE_ACTIVE_HIGH
static constexpr bool OE_ACTIVE_HIGH = true;
#else
static constexpr bool OE_ACTIVE_HIGH = false;
#endif

// SoftDMD (bit-banged CLK/R) with an ESP32 scanDisplay():
//  - OE brightness comes from a LEDC PWM channel instead of analogWrite(),
//    which clashes with digitalWrite() on arduino-esp32 3.x.
//  - OE polarity follows menuconfig.
// Everything else (framebuffer, drawing, shift-out) is stock DMD2.
class Esp32SoftDMD : public SoftDMD {
public:
    Esp32SoftDMD(byte wide, byte high, byte oe, byte a, byte b, byte lat, byte clk, byte r)
        : SoftDMD(wide, high, oe, a, b, lat, clk, r) {}

    void beginNoTimer() override
    {
        // Sets pins as GPIO outputs and does one scan (with oe_ready_ false,
        // so OE is left alone and the panel stays dark).
        SoftDMD::beginNoTimer();

        ledc_timer_config_t timer = {};
        timer.speed_mode = LEDC_LOW_SPEED_MODE;
        timer.duty_resolution = LEDC_TIMER_10_BIT;
        timer.timer_num = LEDC_TIMER_0;
        timer.freq_hz = 20000; // ~20 PWM cycles per 1 ms row, inaudible
        timer.clk_cfg = LEDC_AUTO_CLK;
        ESP_ERROR_CHECK(ledc_timer_config(&timer));

        ledc_channel_config_t ch = {};
        ch.gpio_num = pin_noe;
        ch.speed_mode = LEDC_LOW_SPEED_MODE;
        ch.channel = LEDC_CHANNEL_0;
        ch.timer_sel = LEDC_TIMER_0;
        ch.duty = 0; // panel off until the first scan
        ch.flags.output_invert = OE_ACTIVE_HIGH ? 0 : 1;
        ESP_ERROR_CHECK(ledc_channel_config(&ch));

        oe_ready_ = true;
    }

    void scanDisplay() override
    {
        const int rowsize = unified_width_bytes();
        volatile uint8_t *rows[4] = {
            bitmap + (scan_row + 0) * rowsize,
            bitmap + (scan_row + 4) * rowsize,
            bitmap + (scan_row + 8) * rowsize,
            bitmap + (scan_row + 12) * rowsize,
        };
        writeSPIData(rows, rowsize);

        digitalWrite(pin_sck, HIGH); // latch (DMD2 calls LAT "sck")
        digitalWrite(pin_sck, LOW);
        digitalWrite(pin_a, scan_row & 0x01);
        digitalWrite(pin_b, scan_row & 0x02);
        scan_row = (scan_row + 1) % 4;

        if (oe_ready_ && brightness != applied_brightness_) {
            uint32_t duty = brightness >= 255 ? 1024 : (uint32_t)brightness * 1024 / 255;
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
            applied_brightness_ = brightness;
        }
    }

private:
    bool oe_ready_ = false;
    int applied_brightness_ = -1;
};

static Esp32SoftDMD dmd(CONFIG_LED_MATRIX_PANELS_WIDE, CONFIG_LED_MATRIX_PANELS_HIGH,
                        CONFIG_LED_MATRIX_GPIO_OE, CONFIG_LED_MATRIX_GPIO_A,
                        CONFIG_LED_MATRIX_GPIO_B, CONFIG_LED_MATRIX_GPIO_LAT,
                        CONFIG_LED_MATRIX_GPIO_CLK, CONFIG_LED_MATRIX_GPIO_R);

static void scan_cb(void *) { dmd.scanDisplay(); }

static void show_digit(int n)
{
    char c = '0' + n;
    int x = (dmd.width - SYSTEM5x7_WIDTH) / 2;
    int y = (dmd.height - SYSTEM5x7_HEIGHT) / 2;
    dmd.clearScreen();
    dmd.drawChar(x, y, c);
}

extern "C" void app_main(void)
{
    initArduino();

    dmd.setBrightness(CONFIG_LED_MATRIX_BRIGHTNESS);
    dmd.selectFont(System5x7);
    dmd.beginNoTimer();

    esp_timer_create_args_t args = {};
    args.callback = scan_cb;
    args.name = "dmd_scan";
    esp_timer_handle_t scan_timer;
    ESP_ERROR_CHECK(esp_timer_create(&args, &scan_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(scan_timer, CONFIG_LED_MATRIX_ROW_PERIOD_US));

    QueueHandle_t rx_queue;
    ESP_ERROR_CHECK(rx433_start(CONFIG_RX433_GPIO, &rx_queue));

    ESP_LOGI(TAG, "%dx%d display, brightness %d/255; receiver on GPIO%d",
             dmd.width, dmd.height, CONFIG_LED_MATRIX_BRIGHTNESS, CONFIG_RX433_GPIO);
    ESP_LOGI(TAG, "Press a remote button: first code seen = 1, next different code = 2");

    uint32_t buttons[2] = {0, 0}; // 0 = not seen yet
    uint32_t last_code = 0;
    TickType_t last_rx = 0;
    rx433_frame_t f;

    while (true) {
        if (!xQueueReceive(rx_queue, &f, portMAX_DELAY)) {
            continue;
        }

        TickType_t now = xTaskGetTickCount();
        bool new_press = f.code != last_code || now - last_rx > pdMS_TO_TICKS(PRESS_GAP_MS);
        last_code = f.code;
        last_rx = now;
        if (!new_press) {
            continue;
        }

        if (!buttons[0]) {
            buttons[0] = f.code;
            ESP_LOGI(TAG, "code 0x%06lx assigned to button 1", (unsigned long)f.code);
        } else if (!buttons[1] && f.code != buttons[0]) {
            buttons[1] = f.code;
            ESP_LOGI(TAG, "code 0x%06lx assigned to button 2", (unsigned long)f.code);
        }

        int n = f.code == buttons[0] ? 1 : f.code == buttons[1] ? 2 : 0;
        ESP_LOGI(TAG, "code=0x%06lx bits=%u proto=%u pulse=%uus -> %s",
                 (unsigned long)f.code, f.bits, f.protocol, f.pulse_us,
                 n == 1 ? "button 1" : n == 2 ? "button 2" : "unknown, ignored");
        if (n) {
            show_digit(n);
        }
    }
}
