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
#define BATT_5  3200 // Lockout threshold for powering on
#define BATT_SHUTDOWN 3000 // Sag-allowance threshold for auto-shutdown

typedef enum {
    STATE_STANDBY,
    STATE_OFF_WAKE,
    STATE_ON
} SystemState;

SystemState current_state = STATE_STANDBY;
bool is_recording = false;
uint32_t last_batt_blink = 0;
uint32_t low_batt_start = 0; // Tracks consecutive low voltage duration

// --- Apple Breathing LED Curve (Scaled for 50% max brightness) ---
const uint8_t apple_breath[] = {
    0, 0, 1, 2, 4, 6, 8, 10, 13, 16, 18, 21, 23, 24, 25, 25, 
    25, 25, 24, 23, 21, 18, 16, 13, 10, 8, 6, 4, 2, 1, 0, 0
};

// --- Hardware Control Functions ---
void led_on()  { GPIOD->BCR = (1 << PIN_LED); }  
void led_off() { GPIOD->BSHR = (1 << PIN_LED); } 
void mosfet_on()  { GPIOC->BCR = (1 << PIN_MOSFET); }  
void mosfet_off() { GPIOC->BSHR = (1 << PIN_MOSFET); } 
void rec_high() { GPIOC->BSHR = (1 << PIN_RECORD); }
void rec_low()  { GPIOC->BCR = (1 << PIN_RECORD); }

bool btn_pressed() { return !(GPIOC->INDR & (1 << PIN_BTN)); }
bool pg_active()   { return !(GPIOC->INDR & (1 << PIN_PG)); }
bool status_high() { return (GPIOC->INDR & (1 << PIN_STATUS)); }

// --- Hardware Initialization ---
void init_hardware() {
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOC | 
                      RCC_APB2Periph_GPIOD | RCC_APB2Periph_ADC1 | RCC_APB2Periph_AFIO;

    RCC->CFGR0 &= ~(0x1F<<11); 
    RCC->CFGR0 |= (8<<11); 

    // ---- LOW POWER PIN CONFIGURATION ----
    GPIOA->CFGLR &= ~((0xF << (4 * 1)) | (0xF << (4 * 2))); // PA1, PA2
    GPIOC->CFGLR &= ~((0xF << (4 * 0)) | (0xF << (4 * 6)) | (0xF << (4 * 7))); // PC0, PC6, PC7
    GPIOD->CFGLR &= ~((0xF << (4 * 0)) | (0xF << (4 * 2)) | (0xF << (4 * 3)) | 
                      (0xF << (4 * 5)) | (0xF << (4 * 6)) | (0xF << (4 * 7))); 

    // Outputs
    GPIOC->CFGLR &= ~(0xF << (4 * PIN_MOSFET) | 0xF << (4 * PIN_RECORD));
    GPIOC->CFGLR |= (0x3 << (4 * PIN_MOSFET) | 0x3 << (4 * PIN_RECORD)); 
    mosfet_off();
    rec_low();

    GPIOD->CFGLR &= ~(0xF << (4 * PIN_LED));
    GPIOD->CFGLR |= (0x3 << (4 * PIN_LED));
    led_off();

    // Inputs (Pull-Ups Enabled)
    GPIOC->CFGLR &= ~(0xF << (4 * PIN_PG) | 0xF << (4 * PIN_STATUS) | 0xF << (4 * PIN_BTN));
    GPIOC->CFGLR |= (0x8 << (4 * PIN_PG) | 0x8 << (4 * PIN_STATUS) | 0x8 << (4 * PIN_BTN)); 
    GPIOC->OUTDR |= (1 << PIN_PG) | (1 << PIN_STATUS) | (1 << PIN_BTN); 

    // ---- EXTI WAKEUP EVENT CONFIGURATION ----
    AFIO->EXTICR = (2 << (2*2)) | (2 << (3*2)) | (2 << (4*2)); 
    EXTI->EVENR |= (1 << PIN_PG) | (1 << PIN_STATUS) | (1 << PIN_BTN); 
    EXTI->FTENR |= (1 << PIN_PG) | (1 << PIN_STATUS) | (1 << PIN_BTN); 
    EXTI->RTENR |= (1 << PIN_PG) | (1 << PIN_STATUS);                  
}

// --- ADC & Battery Functions ---
void init_adc() {
    RCC->APB2PRSTR |= RCC_APB2Periph_ADC1;
    RCC->APB2PRSTR &= ~RCC_APB2Periph_ADC1;
    ADC1->RSQR3 = 8;         
    ADC1->SAMPTR2 = (7 << (3 * 8)); 
    ADC1->CTLR2 |= ADC_ADON | (1 << 20) | (7 << 17);
    Delay_Ms(1);
    ADC1->CTLR2 |= ADC_RSTCAL;
    while(ADC1->CTLR2 & ADC_RSTCAL);
    ADC1->CTLR2 |= ADC_CAL;
    while(ADC1->CTLR2 & ADC_CAL);
}

uint32_t get_battery_mv() {
    ADC1->STATR &= ~(ADC_EOC | ADC_STRT); 
    ADC1->CTLR2 |= ADC_SWSTART; 
    uint32_t timeout = 100000;
    while(!(ADC1->STATR & ADC_EOC) && timeout > 0) timeout--;
    if(timeout == 0) return 9999; 
    uint32_t adc_val = ADC1->RDATAR;
    if(adc_val == 0) return 0;
    return (1200 * 1023) / adc_val;
}

// ==========================================
// ABORTABLE BATTERY ANIMATION LOGIC
// ==========================================

int delay_abortable(uint32_t ms, uint32_t release_time) {
    uint32_t start = millis();
    while (millis() - start < ms) {
        if (btn_pressed()) {
            if (millis() - release_time <= 750) return 1;
            else return 2;
        }
    }
    return 0;
}

int blink_led_abortable(int count, uint32_t ms_on, uint32_t ms_off, uint32_t release_time) {
    for(int i = 0; i < count; i++) {
        led_on();
        int res = delay_abortable(ms_on, release_time);
        if(res) { led_off(); return res; }
        
        led_off();
        if(i < count - 1) {
            res = delay_abortable(ms_off, release_time);
            if(res) return res;
        }
    }
    return 0;
}

int show_battery_level_abortable(uint32_t batt_mv, uint32_t release_time) {
    int res = 0;
    if(batt_mv < BATT_5) res = blink_led_abortable(3, 100, 100, release_time);
    else if(batt_mv < BATT_20) res = blink_led_abortable(1, 300, 300, release_time);
    else if(batt_mv < BATT_40) res = blink_led_abortable(2, 300, 300, release_time);
    else if(batt_mv < BATT_60) res = blink_led_abortable(3, 300, 300, release_time);
    else if(batt_mv < BATT_80) res = blink_led_abortable(4, 300, 300, release_time);
    else { 
        res = blink_led_abortable(4, 300, 300, release_time);
        if(res) return res;
        res = delay_abortable(300, release_time);
        if(res) return res;
        res = blink_led_abortable(3, 100, 100, release_time);
    }
    return res;
}

void blink_led(int count, uint32_t ms_on, uint32_t ms_off) {
    for(int i = 0; i < count; i++) {
        led_on(); Delay_Ms(ms_on);
        led_off(); if(i < count - 1) Delay_Ms(ms_off);
    }
}
void show_battery_level(uint32_t batt_mv) {
    show_battery_level_abortable(batt_mv, 0); 
}

// ==========================================
// HARDWARE SEQUENCES
// ==========================================

void mosfet_dac_soft_start() {
    for (int duty = 1; duty <= 16; duty++) {
        for (int cycle = 0; cycle < 5000; cycle++) {
            GPIOC->BCR = (1 << PIN_MOSFET);
            for(volatile int i = 0; i < duty; i++) { __asm("nop"); }
            GPIOC->BSHR = (1 << PIN_MOSFET);
            for(volatile int i = 0; i < (16 - duty); i++) { __asm("nop"); }
        }
    }
    GPIOC->BCR = (1 << PIN_MOSFET);
}

void shutdown_sequence() {
    is_recording = false;
    rec_low();
    
    // 10Hz 33% duty cycle for 8 seconds to SSC338Q
    // 80 iterations * 100ms = 8000ms = 8 seconds
    for(int i = 0; i < 80; i++) {
        rec_high(); Delay_Ms(33);
        rec_low(); Delay_Ms(67);
    }
    
    mosfet_off();
    current_state = STATE_STANDBY;
}

// ==========================================
// MAIN LOOP
// ==========================================

void setup() {
    init_hardware();
    init_adc();
}

void loop() {
    uint32_t now = millis(); 
    uint32_t batt_mv = get_battery_mv();

    switch(current_state) {
        
        case STATE_STANDBY: { 
            if (pg_active() && !status_high()) {
                // --- CHARGING: 20kHz Software PWM Breathing ---
                for (int i = 0; i < 32; i++) {
                    uint8_t on_time = apple_breath[i];
                    uint8_t off_time = 50 - on_time; 
                    
                    for (int j = 0; j < 1000; j++) {
                        if (on_time > 0) { led_on(); Delay_Us(on_time); }
                        if (off_time > 0) { led_off(); Delay_Us(off_time); }
                        if (!pg_active() || btn_pressed() || status_high()) break;
                    }
                    if (!pg_active() || btn_pressed() || status_high()) break;
                    
                    if (i == 31) {
                        led_off();
                        for(int j=0; j<8000; j++) {
                            Delay_Us(50);
                            if (!pg_active() || btn_pressed() || status_high()) break;
                        }
                    }
                }
            } 
            else if (pg_active() && status_high() && batt_mv > 4000) {
                // --- FULLY CHARGED (>4.0V): Solid 50% PWM ---
                for (int j = 0; j < 1000; j++) {
                    led_on(); Delay_Us(25);
                    led_off(); Delay_Us(25);
                    if (btn_pressed() || !pg_active() || !status_high()) break;
                }
            } 
            else {
                // --- UNPLUGGED -OR- NO BATTERY (<4.0V): True Deep Sleep ---
                led_off();
                PWR->CTLR &= ~PWR_CTLR_PDDS; 
                PFIC->SCTLR |= (1<<2);       
                __WFE();                     
                SystemInit48HSI();           
            }
            
            if (btn_pressed()) {
                current_state = STATE_OFF_WAKE;
            }
            break;
        }

        case STATE_OFF_WAKE: { 
            led_on();
            while(btn_pressed()); 
            led_off();
            
            uint32_t release_time = millis();
            bool second_press = false;

            int abort_reason = show_battery_level_abortable(batt_mv, release_time);

            if (abort_reason == 1) {
                second_press = true; 
            } 
            else if (abort_reason == 0) {
                while(millis() - release_time <= 750) {
                    if (btn_pressed()) {
                        second_press = true;
                        break;
                    }
                }
            }

            if(second_press) {
                uint32_t hold_start = millis();
                bool hold_failed = false;

                while(btn_pressed()) {
                    if(millis() - hold_start >= 2000) break; 
                    if((millis() / 100) % 2 == 0) led_on(); else led_off();
                }
                led_off(); 

                if(millis() - hold_start >= 2000) {
                    // Lockout boot if battery is <3200mV AND unplugged
                    if(batt_mv >= BATT_5 || pg_active()) {
                        led_on(); Delay_Ms(300); 
                        mosfet_dac_soft_start();
                        while(btn_pressed()); 
                        led_off();
                        current_state = STATE_ON;
                        last_batt_blink = millis();
                        low_batt_start = 0; // Reset sag timer on boot
                    } else {
                        blink_led(3, 100, 100); 
                        current_state = STATE_STANDBY;
                    }
                } else {
                    current_state = STATE_STANDBY; 
                }
            } else {
                current_state = STATE_STANDBY;
            }
            break;
        }

        case STATE_ON: { 
            // 1. Continuous Low Voltage Sag Check
            if(batt_mv < BATT_SHUTDOWN && !pg_active()) {
                if(low_batt_start == 0) {
                    low_batt_start = now; // Mark the exact millisecond voltage dropped
                } else if(now - low_batt_start >= 1000) {
                    // Voltage has been consistently < 3000mV for 1 full second
                    blink_led(2, 300, 300);
                    shutdown_sequence();
                    break; // Exit ON state immediately
                }
            } else {
                low_batt_start = 0; // Voltage recovered, reset the timer
            }

            // 2. Background battery indicator
            if(now - last_batt_blink >= 3500 && !btn_pressed()) {
                show_battery_level(batt_mv);
                last_batt_blink = millis();
            }

            // 3. Button Actions
            if(btn_pressed()) {
                uint32_t press_start = millis();
                bool shutdown_triggered = false;

                while(btn_pressed()) {
                    uint32_t held_time = millis() - press_start;

                    if(held_time >= 4000) {
                        shutdown_triggered = true;
                        break; 
                    }
                    else if(held_time >= 2000) {
                        if((millis() / 100) % 2 == 0) led_on(); else led_off();
                    }
                    else {
                        led_off(); 
                    }
                }

                led_off(); 
                uint32_t held_time = millis() - press_start;

                // Execute actions AFTER release
                if (shutdown_triggered) {
                    blink_led(2, 300, 300);
                    shutdown_sequence();
                } 
                else if (held_time < 500) {
                    rec_high();
                    Delay_Ms(150);
                    rec_low();
                } 
                else if (held_time >= 1000 && held_time < 4000) {
                    is_recording = !is_recording;
                    if(is_recording) rec_high();
                    else rec_low();
                }
                
                last_batt_blink = millis(); 
            }
            break;
        }
    }
}