#pragma once

// One 32x16 HUB12 panel driven through DMD2's SoftDMD, scanned from an
// esp_timer. Pins, brightness and row period come from menuconfig
// ("LED Matrix").

#include <initializer_list>
#include <Arduino.h>
#include <DMD2.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "sdkconfig.h"

// SoftDMD that drives OE with LEDC instead of analogWrite(), which on
// arduino-esp32 3.x detaches the pin from GPIO and breaks DMD2's
// digitalWrite() blanking. OE is active high.
class Hub12 : public SoftDMD {
public:
    Hub12()
        : SoftDMD(1, 1, CONFIG_LED_MATRIX_GPIO_OE, CONFIG_LED_MATRIX_GPIO_A,
                  CONFIG_LED_MATRIX_GPIO_B, CONFIG_LED_MATRIX_GPIO_LAT,
                  CONFIG_LED_MATRIX_GPIO_CLK, CONFIG_LED_MATRIX_GPIO_R)
    {
    }

    void start()
    {
        for (int pin : {CONFIG_LED_MATRIX_GPIO_A, CONFIG_LED_MATRIX_GPIO_B, CONFIG_LED_MATRIX_GPIO_LAT,
                        CONFIG_LED_MATRIX_GPIO_CLK, CONFIG_LED_MATRIX_GPIO_R}) {
            pinMode(pin, OUTPUT);
            digitalWrite(pin, LOW);
        }

        ledc_timer_config_t timer_config = {};
        timer_config.speed_mode = LEDC_LOW_SPEED_MODE;
        timer_config.duty_resolution = LEDC_TIMER_8_BIT;
        timer_config.timer_num = OE_TIMER;
        timer_config.freq_hz = 20000; // many PWM cycles per row period
        timer_config.clk_cfg = LEDC_AUTO_CLK;
        ESP_ERROR_CHECK(ledc_timer_config(&timer_config));

        ledc_channel_config_t channel_config = {};
        channel_config.gpio_num = CONFIG_LED_MATRIX_GPIO_OE;
        channel_config.speed_mode = LEDC_LOW_SPEED_MODE;
        channel_config.channel = OE_CHANNEL;
        channel_config.timer_sel = OE_TIMER;
        channel_config.duty = 0;
        ESP_ERROR_CHECK(ledc_channel_config(&channel_config));

        clearScreen();

        esp_timer_create_args_t args = {};
        args.callback = [](void *self) { static_cast<Hub12 *>(self)->scanDisplay(); };
        args.arg = this;
        args.name = "hub12_scan";
        esp_timer_handle_t timer;
        ESP_ERROR_CHECK(esp_timer_create(&args, &timer));
        ESP_ERROR_CHECK(esp_timer_start_periodic(timer, CONFIG_LED_MATRIX_ROW_PERIOD_US));
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
        writeSPIData(rows, rowsize); // shifts while the previous row is still lit

        ledc_stop(LEDC_LOW_SPEED_MODE, OE_CHANNEL, 0); // blank immediately
        gpio_set_level((gpio_num_t)CONFIG_LED_MATRIX_GPIO_LAT, 1);
        gpio_set_level((gpio_num_t)CONFIG_LED_MATRIX_GPIO_LAT, 0);
        gpio_set_level((gpio_num_t)CONFIG_LED_MATRIX_GPIO_A, scan_row & 1);
        gpio_set_level((gpio_num_t)CONFIG_LED_MATRIX_GPIO_B, (scan_row >> 1) & 1);
        scan_row = (scan_row + 1) % 4;

        ledc_set_duty(LEDC_LOW_SPEED_MODE, OE_CHANNEL, CONFIG_LED_MATRIX_BRIGHTNESS);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, OE_CHANNEL);
    }

private:
    static constexpr ledc_timer_t OE_TIMER = LEDC_TIMER_3;
    static constexpr ledc_channel_t OE_CHANNEL = LEDC_CHANNEL_7;
};
