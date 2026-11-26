#include "status_led.h"
#include <driver/rmt_tx.h>

// The WS2812 RGB LED of the ESP32-S3-DevKitC-1 (N16R8 boards and v1.0). v1.1 boards use GPIO38.
static constexpr gpio_num_t LED_GPIO = GPIO_NUM_48;

static rmt_channel_handle_t s_channel;
static rmt_encoder_handle_t s_encoder;

bool led_driver_init() {
    rmt_tx_channel_config_t channel = {};
    channel.gpio_num = LED_GPIO;
    channel.clk_src = RMT_CLK_SRC_DEFAULT;
    channel.resolution_hz = 10 * 1000 * 1000; // 0.1 us per tick
    channel.mem_block_symbols = 64;           // the 24 bits fit, so WiFi interrupts cannot disturb the timing
    channel.trans_queue_depth = 1;

    // WS2812 bits: 0 = 0.3 us high, 0.9 us low; 1 = 0.9 us high, 0.3 us low
    rmt_bytes_encoder_config_t bytes = {};
    bytes.bit0.duration0 = 3;
    bytes.bit0.level0 = 1;
    bytes.bit0.duration1 = 9;
    bytes.bit0.level1 = 0;
    bytes.bit1.duration0 = 9;
    bytes.bit1.level0 = 1;
    bytes.bit1.duration1 = 3;
    bytes.bit1.level1 = 0;
    bytes.flags.msb_first = 1;

    return rmt_new_tx_channel(&channel, &s_channel) == ESP_OK &&
           rmt_new_bytes_encoder(&bytes, &s_encoder) == ESP_OK &&
           rmt_enable(s_channel) == ESP_OK;
}

void led_driver_show(Rgb colour) {
    const uint8_t grb[3] = {colour.g, colour.r, colour.b};
    rmt_transmit_config_t transmit = {};
    if (rmt_transmit(s_channel, s_encoder, grb, sizeof(grb), &transmit) == ESP_OK) {
        rmt_tx_wait_all_done(s_channel, 50); // `grb` must stay alive until it is sent
    }
}
