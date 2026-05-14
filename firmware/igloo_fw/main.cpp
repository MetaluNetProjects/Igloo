// Palantir main

#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "udp_server.hpp"
#include "fraise.h"
#include "cpuload.h"
#include "free_heap.h"
#include "system.h"
#include "math.h"
#include "encoder.hpp"
#include "motor.hpp"
#include "ramp.hpp"
#include "PID_v1.h"

// PID settings:
const int PID_KP = 90;
const int PID_KI = 100;
const int PID_KD = 1;

// RAMP settings:
const int RAMP_ACCEL = 2000;    // steps/s²
const int RAMP_MAXSPEED = 5400; // steps/s
const int HOMING_PWM = 6000;    // max 32767 (but don't do that ;-)

// CURRENT SECURITY
const int OVERCURRENT_MA = 800;
const int OVERCURRENT_MS = 2000;

// LOWSWITCH MARGIN
const int LOWSWITCH_MARGIN = 2000;

#define PIN_MOT_A       15
#define PIN_MOT_B        9
#define PIN_MOT_PWM     11
#define PIN_MOT_SEL     13
#define PIN_MOT_CURRENT 26
#define PIN_ENC_A        0
#define PIN_ENC_B        2

Motor motor{PIN_MOT_A, PIN_MOT_B, PIN_MOT_PWM};

// encoder on J2: 4=A 5=B 6= 7=
Encoder encoder(PIN_ENC_A, PIN_ENC_B);

float position;
float pwm;
float setPoint;

PID pid(&position, &pwm, &setPoint, PID_KP, PID_KI, PID_KD, P_ON_E, DIRECT);
Ramp ramp(RAMP_ACCEL, RAMP_MAXSPEED);
bool ramp_to_pid = false;


bool do_print_led = false;

void blink(int ledPeriod) {
    static absolute_time_t nextLed;
    static bool led = false;

    if(time_reached(nextLed)) {
        set_led(led = !led);
        nextLed = make_timeout_time_ms(ledPeriod);
        if(do_print_led) fraise_printf("led %d\n", led ? 1 : 0);
    }
}

void gpio_callback(uint gpio, uint32_t events) {
    if(encoder.gpio_handler(gpio, events)) return;
}

void setup_motor_current() {
    adc_init();
    adc_gpio_init(PIN_MOT_CURRENT);
}

void setup() {
    ip_addr_t ip;
    IP4_ADDR(&ip, 192, 168, 5, WIFI_IPADDR_D);
    //netif_set_ipaddr(netif_default, &ip);
    gpio_set_irq_enabled_with_callback(PIN_ENC_A, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true, &gpio_callback);
    setup_motor_current();
    encoder.init();
    motor.init();
    pid.SetOutputLimits(-32767.0, 32767.0);
    pid.SetSampleTime(5);
}

int get_motor_current_mA(bool update = false) {
    static float current_filtered = 0;
    if(update) {
        float current_raw = 0;
        adc_select_input(PIN_MOT_CURRENT - 26);
        current_raw = adc_read();
        current_filtered += (current_raw - current_filtered) * 0.05;
    }
    // VNH7070 Rsense = 1k
    // adc = vsense * 4096 / 3.3
    // vsense = isense * 1k = imotor/1540 * 1000
    // imotor = vsense * 1540 / 1000 = (adc * 3.3 / 4096) * 1540 / 1000 = adc * (3.3 *  1540) / (4096 * 1000)
    // imotor_mA = adc * (3.3 *  1540) / 4096 = adc * 1.24
    return (current_filtered * ((3.3 * 1540) / 4096));
}

void motor_check_current() {
    static absolute_time_t alert_time = at_the_end_of_time;
    if(get_motor_current_mA() > OVERCURRENT_MA) {
        if(alert_time == at_the_end_of_time) {
            alert_time = make_timeout_time_ms(OVERCURRENT_MS);
        } else if(time_reached(alert_time)) {
            motor.goto_pwm_ms(0, 10);
            fraise_printf("e overcurrent error!\n");
            alert_time = at_the_end_of_time;
        }
    } else alert_time = at_the_end_of_time;
}

void motor_updatepwm() {
    motor.goto_pwm_ms(pwm, 10);
}

void enableMotorControl(bool enable) {
	if(enable) {
		position = setPoint = encoder.get_count();
		ramp.set(setPoint);
	}
	pid.SetMode(enable ? 1 : 0);
	ramp_to_pid = enable;
}

void motorcontrol_update() {
	position = encoder.get_count();
	if(pid.Compute()) {
		ramp.compute();
		if(ramp_to_pid) setPoint = ramp.get_position();
		motor_updatepwm();
	}
	motor.update();
	motor.reset_watchdog();
	get_motor_current_mA(true);
	motor_check_current();
}

void loop() {
    blink(250);
    motorcontrol_update();
}

void fraise_receivebytes(const char* data, uint8_t len) {
    char command = fraise_get_uint8();
    switch(command) {
        case 1:
            fraise_printf("l pwm %d\n", motor.get_pwm());
            break;
        case 2:
            fraise_printf("l current %d\n", get_motor_current_mA());
            break;
        case 3:
            fraise_printf("l m %d %f\n", encoder.get_count(), encoder.speed_process());
            break;
        case 4:
            fraise_printf("l ramp %f\n", ramp.get_position());
            break;
        case 10: // HOMING
            enableMotorControl(false);
            pwm = -1 * (HOMING_PWM);
            break;
        case 11: // GOTO
            ramp.set_destination(fraise_get_int32());
            break;
        case 12: // SPEED
            ramp.set_maxspeed(fraise_get_int32());
            break;
        case 100: { // MANUAL PWM
	            pwm = fraise_get_int16();
	            motor_updatepwm();
	            enableMotorControl(false);
            }
            break;
        case 110: // PID params
            {
                float P, I, D;
                P = fraise_get_int32() / 1000.0;
                I = fraise_get_int32() / 1000.0;
                D = fraise_get_int32() / 1000.0;
                pid.SetTunings(P, I, D);
            }
            break;
        case 120: // ramp accel
            ramp.set_accel(fraise_get_int32());
            break;
        case 210: // set encoder
            encoder.set_count(fraise_get_int32());
            break;
        case 220:
            enableMotorControl(fraise_get_uint8() > 0);
            break;
        case 230:
            setPoint = fraise_get_int32();
            break;
        case 240: fraise_printf("l free ram: %ld\n", getFreeHeap()); break;
        case 241: wifi_print_status(); break;
    }
}

extern void print_version(); // from version.cpp

void fraise_receivechars(const char *data, uint8_t len){
    if(data[0] == 'E') { // Echo
        fraise_printf("E%s\n", data + 1);
    } else if(data[0] == 'V') { // Version
        print_version();
    } else fraise_printf("unknown %d\n", data[0]);
}

