#include "ch32v003fun.h"
#include <stdbool.h>

// --- Pin Definitions ---
#define PIN_MOSFET   1 // PC1 (Output, Low = ON)
#define PIN_PG       2 // PC2 (Input, Low = Power applied)
#define PIN_STATUS   3 // PC3 (Input)
#define PIN_BTN      4 // PC4 (Input, Low = Pressed)
#define PIN_RECORD   5 // PC5 (Output)
#define PIN_LED      4 // PD4 (Output, Low = ON)

// --- Battery Thresholds (mV) ---
#define BATT_95 4100
#define BATT_80 3830
#define BATT_60 3700
#define BATT_40 3610
#define BATT_20 3500
#define BATT_5  3200

// --- System States ---
typedef enum {
    STATE_STANDBY,
    STATE_OFF_WAKE,
    STATE_ON
} SystemState;

SystemState current_state = STATE_STANDBY;
bool is_recording = false;
bool is_charging = false;
uint32_t last_batt_blink = 0; // Moved to global scope for the Arduino loop()

// --- Hardware Control Functions ---
void led_on()  { GPIOD->BCR = (1 << PIN_LED); }  // Drive Low
void led_off() { GPIOD->BSHR = (1 << PIN_LED); } // Drive High
void mosfet_on()  { GPIOC->BCR = (1 << PIN_MOSFET); }  // Drive Low
void mosfet_off() { GPIOC->BSHR = (1 << PIN_MOSFET); } // Drive High
void rec_high() { GPIOC->BSHR = (1 << PIN_RECORD); }
void rec_low()  { GPIOC->BCR = (1 << PIN_RECORD); }

bool btn_pressed() { return !(GPIOC->INDR & (1 << PIN_BTN)); }
bool pg_active()   { return !(GPIOC->INDR & (1 << PIN_PG)); }
bool status_high() { return (GPIOC->INDR & (1 << PIN_STATUS)); }

// --- Hardware Initialization ---
void init_hardware() {
    // Enable Clocks for Port C, Port D, ADC1, and AFIO
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOC | RCC_APB2Periph_GPIOD | RCC_APB2Periph_ADC1 | RCC_APB2Periph_AFIO;

    // Configure PC1 (MOSFET), PC5 (RECORD) as Push-Pull Outputs
    GPIOC->CFGLR &= ~(0xF << (4 * PIN_MOSFET) | 0xF << (4 * PIN_RECORD));
    GPIOC->CFGLR |= (0x3 << (4 * PIN_MOSFET) | 0x3 << (4 * PIN_RECORD)); // 50MHz Push-Pull
    mosfet_off();
    rec_low();

    // Configure PD4 (LED) as Push-Pull Output
    GPIOD->CFGLR &= ~(0xF << (4 * PIN_LED));
    GPIOD->CFGLR |= (0x3 << (4 * PIN_LED));
    led_off();

    // Configure PC2 (PG), PC3 (STATUS), PC4 (BTN) as Inputs with Pull-Up
    GPIOC->CFGLR &= ~(0xF << (4 * PIN_PG) | 0xF << (4 * PIN_STATUS) | 0xF << (4 * PIN_BTN));
    GPIOC->CFGLR |= (0x8 << (4 * PIN_PG) | 0x8 << (4 * PIN_STATUS) | 0x8 << (4 * PIN_BTN)); // Input with pull-up/down
    GPIOC->OUTDR |= (1 << PIN_PG) | (1 << PIN_STATUS) | (1 << PIN_BTN); // Set to Pull-Up

    // Configure EXTI for PC2 (PG) and PC4 (BTN) to wake from Standby
    AFIO->EXTICR = (2 << (PIN_PG * 2)) | (2 << (PIN_BTN * 2)); // Assign Port C to EXTI2 and EXTI4
    EXTI->INTENR |= (1 << PIN_PG) | (1 << PIN_BTN);            // Enable EXTI lines
    EXTI->FTENR |= (1 << PIN_PG) | (1 << PIN_BTN);             // Falling edge trigger
}

// --- ADC & Battery Functions ---
void init_adc() {
    // ADC Setup for Channel 8 (Internal 1.2V Vref)
    ADC1->CTLR2 |= ADC_ADON; // Turn on ADC
    ADC1->RSQR3 = 8;         // Select Channel 8
    ADC1->SAMPTR2 = (7 << (3 * 8)); // Max sample time
    
    // Reset Calibration
    ADC1->CTLR2 |= ADC_RSTCAL;
    while(ADC1->CTLR2 & ADC_RSTCAL);
    // Start Calibration
    ADC1->CTLR2 |= ADC_CAL;
    while(ADC1->CTLR2 & ADC_CAL);
}

uint32_t get_battery_mv() {
    ADC1->CTLR2 |= ADC_SWSTART; // Start Conversion
    while(!(ADC1->STATR & ADC_EOC)); // Wait for End Of Conversion
    uint32_t adc_val = ADC1->RDATAR;
    
    // VDD (Battery) = (1.2V * 1023) / ADC_Reading
    if(adc_val == 0) return 0;
    return (1200 * 1023) / adc_val;
}

// --- LED Sequences ---
void blink_led(int count, uint32_t ms_on, uint32_t ms_off) {
    for(int i = 0; i < count; i++) {
        led_on();
        Delay_Ms(ms_on);
        led_off();
        if(i < count - 1) Delay_Ms(ms_off);
    }
}

void show_battery_level(uint32_t batt_mv) {
    if(batt_mv < BATT_5) {
        blink_led(3, 100, 100);
    } else if(batt_mv < BATT_20) {
        blink_led(1, 300, 300);
    } else if(batt_mv < BATT_40) {
        blink_led(2, 300, 300);
    } else if(batt_mv < BATT_60) {
        blink_led(3, 300, 300);
    } else if(batt_mv < BATT_80) {
        blink_led(4, 300, 300);
    } else {
        blink_led(4, 300, 300);
        Delay_Ms(300);
        blink_led(3, 100, 100);
    }
}

// --- Shutdown Sequence ---
void shutdown_sequence() {
    is_recording = false;
    rec_low();
    
    // 10Hz 33% duty cycle for 5 seconds to SSC338Q
    // 10Hz = 100ms period. 33% = 33ms HIGH, 67ms LOW.
    for(int i = 0; i < 50; i++) {
        rec_high();
        Delay_Ms(33);
        rec_low();
        Delay_Ms(67);
    }
    
    mosfet_off();
    current_state = STATE_STANDBY;
}

// ==========================================
// ARDUINO ENTRY POINTS (setup & loop)
// ==========================================

void setup() {
    init_hardware();
    init_adc();

    // Check Cold Boot State (External power applied, no battery)
    if(pg_active() && status_high()) {
        mosfet_on();
        rec_high();
        is_recording = true;
        current_state = STATE_ON;
    }
}

void loop() {
    uint32_t now = millis(); // using the Arduino Core's built-in millis()
    uint32_t batt_mv = get_battery_mv();

    switch(current_state) {
        
        case STATE_STANDBY: { 
            // Deep Sleep (retains RAM, continues code below on wake-up)
            PWR->CTLR &= ~PWR_CTLR_PDDS; // Clear PDDS for normal Deep Sleep
            PFIC->SCTLR |= (1<<2);       // Set Sleepdeep bit
            __WFI();                     // Wait For Interrupt (Wakes on PC2 or PC4)
            
            // --- Wake Up Handling ---
            // (Clocks are automatically restored from deep sleep)
            
            // Wake logic evaluation
            if(btn_pressed()) {
                current_state = STATE_OFF_WAKE;
            } else if(pg_active() && !status_high()) {
                is_charging = true;
            }
            break;
        }

        case STATE_OFF_WAKE: { 
            // Wait for button release to show battery
            while(btn_pressed());
            show_battery_level(batt_mv);

            // Wait for sequence: Press within 1000ms, then hold for 2s
            uint32_t seq_start = millis();
            bool sequence_success = false;

            while(millis() - seq_start < 1000) {
                if(btn_pressed()) {
                    uint32_t hold_start = millis();
                    bool hold_failed = false;

                    // Check for 2-second hold
                    while(millis() - hold_start < 2000) {
                        if(!btn_pressed()) {
                            hold_failed = true;
                            break;
                        }
                        // Rapid blink during hold
                        if((millis() / 100) % 2 == 0) led_on();
                        else led_off();
                    }
                    
                    led_off(); // Ensure LED is off before transition

                    if(!hold_failed) {
                        if(batt_mv >= BATT_5) {
                            sequence_success = true;
                            led_on();
                            mosfet_on();
                            while(btn_pressed()); // Wait for release
                            led_off();
                        } else {
                            // Battery too low
                            blink_led(3, 100, 100);
                        }
                    }
                    break; 
                }
            }

            if(sequence_success) {
                current_state = STATE_ON;
                last_batt_blink = millis();
            } else {
                current_state = STATE_STANDBY;
            }
            break;
        }

        case STATE_ON: { 
            // Non-blocking battery percentage flash every 3500ms
            if(now - last_batt_blink >= 3500 && !btn_pressed()) {
                show_battery_level(batt_mv);
                last_batt_blink = millis();
            }

            // Handle Button Holds while ON
            if(btn_pressed()) {
                uint32_t hold_start = millis();
                bool recording_toggled = false;

                while(btn_pressed()) {
                    uint32_t held_time = millis() - hold_start;

                    // 4 Seconds: Turn Off Sequence
                    if(held_time >= 4000) {
                        blink_led(2, 300, 300);
                        shutdown_sequence();
                        break; 
                    }
                    // > 2 Seconds: Rapid warning blink
                    else if(held_time >= 2000) {
                        if((millis() / 100) % 2 == 0) led_on();
                        else led_off();
                    }
                    // 1-2 Seconds: Ready to toggle recording (triggers on release)
                    else if(held_time >= 1000 && !recording_toggled) {
                        recording_toggled = true; // Mark as ready to toggle
                    }
                }

                led_off(); // Ensure off upon release

                // If released between 1 and 2 seconds, toggle recording
                if(recording_toggled && current_state == STATE_ON) {
                    is_recording = !is_recording;
                    if(is_recording) rec_high();
                    else rec_low();
                }
            }
            break;
        }
    }

    // Charging logic update while in Standby loop (if not handled by EXTI waking)
    if(current_state == STATE_STANDBY) {
        if(!pg_active() || status_high()) {
            if(is_charging) {
                is_charging = false;
                led_off();
            }
        } else {
            // If charging, blink slowly
            is_charging = true;
            if((millis() / 1000) % 2 == 0) led_on();
            else led_off();
        }
    }
}