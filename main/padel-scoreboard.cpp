// HUB12 test animation using DMD2.
//
// Deliberately low power: at most 4 LEDs are lit at once, and OE runs at
// CONFIG_LED_MATRIX_BRIGHTNESS (default 50/255).

#include <Arduino.h>
#include <DMD2.h>

#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

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

// Position of step `i` on a clockwise walk around the panel border.
static void perimeter(int i, int w, int h, int &x, int &y)
{
    const int len = 2 * (w + h) - 4;
    i %= len;
    if (i < w)              { x = i;                 y = 0; return; }
    i -= w;
    if (i < h - 1)          { x = w - 1;             y = i + 1; return; }
    i -= h - 1;
    if (i < w - 1)          { x = w - 2 - i;         y = h - 1; return; }
    i -= w - 1;
    x = 0; y = h - 2 - i;
}

extern "C" void app_main(void)
{
    initArduino();

    dmd.setBrightness(CONFIG_LED_MATRIX_BRIGHTNESS);
    dmd.beginNoTimer();

    esp_timer_create_args_t args = {};
    args.callback = scan_cb;
    args.name = "dmd_scan";
    esp_timer_handle_t scan_timer;
    ESP_ERROR_CHECK(esp_timer_create(&args, &scan_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(scan_timer, CONFIG_LED_MATRIX_ROW_PERIOD_US));

    const int w = dmd.width, h = dmd.height;
    ESP_LOGI(TAG, "%dx%d display, brightness %d/255, row period %d us",
             w, h, CONFIG_LED_MATRIX_BRIGHTNESS, CONFIG_LED_MATRIX_ROW_PERIOD_US);
    ESP_LOGI(TAG, "Animation: 1 px chasing the border, 3 px ball bouncing inside");

    // Ball: head + 2-pixel tail, bouncing inside the border.
    int bx = 3, by = 2, dx = 1, dy = 1;
    int tail_x[2] = {bx, bx}, tail_y[2] = {by, by};
    int cx = 0, cy = 0;
    uint32_t step = 0;

    while (true) {
        // Erase last frame's pixels (only the ones we lit).
        dmd.setPixel(cx, cy, GRAPHICS_OFF);
        dmd.setPixel(tail_x[1], tail_y[1], GRAPHICS_OFF);

        perimeter(step, w, h, cx, cy);

        tail_x[1] = tail_x[0]; tail_y[1] = tail_y[0];
        tail_x[0] = bx;        tail_y[0] = by;
        if (bx + dx < 1 || bx + dx > w - 2) dx = -dx;
        if (by + dy < 1 || by + dy > h - 2) dy = -dy;
        bx += dx; by += dy;

        dmd.setPixel(cx, cy, GRAPHICS_ON);
        dmd.setPixel(bx, by, GRAPHICS_ON);
        dmd.setPixel(tail_x[0], tail_y[0], GRAPHICS_ON);
        dmd.setPixel(tail_x[1], tail_y[1], GRAPHICS_ON);

        if (step % 20 == 0) {
            ESP_LOGI(TAG, "step %lu: border px (%d,%d), ball (%d,%d)",
                     (unsigned long)step, cx, cy, bx, by);
        }
        step++;
        delay(50);
    }
}
