#include "io/adc_handler.h"
#include "hardware/adc.h"
#include "hardware/pin_config.h"

void AdcHandler::init() {
    adc_init();
    adc_gpio_init(PIN_POT_ADC);
    adc_select_input(PIN_POT_ADC_INPUT);
    raw_ = adc_read();
    smoothed_ = static_cast<float>(raw_) / 4095.0f;
}

void AdcHandler::poll() {
    raw_ = adc_read();
    const float target = static_cast<float>(raw_) / 4095.0f;
    smoothed_ += SMOOTH * (target - smoothed_);
}
