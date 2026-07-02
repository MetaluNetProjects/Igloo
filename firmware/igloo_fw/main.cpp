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
#include "trajectory_table.hpp"
#include "partition_info.h"
#include "wifi_partition.hpp"
//#include "romtable.hpp"
#include "pico/bootrom.h"
#include "boot/picobin.h"

// PID settings:
const float PID_KP = 90.0;
const float PID_KI = 100.0;
const float PID_KD = 1.0;

// RAMP settings:
const float RAMP_ACCEL = 2000;    // steps/s²
const float RAMP_MAXSPEED = 5400; // steps/s

// TABLERAMP settings:
const float TABLERAMP_MAXSPEED = 100; // steps/s
const float TABLERAMP_ACCEL = (TABLERAMP_MAXSPEED / 3);    // steps/s², 3s 0->fullspeed

// STALLED settings:
const int STALLED_TIME_MS = 2000;

// CURRENT SECURITY
int OVERCURRENT_MA = 3000;
int OVERCURRENT_MS = 4000;

// pins definition
#define PIN_MOT_A       15
#define PIN_MOT_B        9
#define PIN_MOT_PWM     11
#define PIN_MOT_SEL     13
#define PIN_MOT_CURRENT 26
#define PIN_ENC_A        0
#define PIN_ENC_B        2

#define PIN_VBATT       27

Motor motor{PIN_MOT_A, PIN_MOT_B, PIN_MOT_PWM};

// encoder on J2: 4=A 5=B 6= 7=
Encoder encoder(PIN_ENC_A, PIN_ENC_B);

float position;
float pwm;
float setPoint;

PID pid(&position, &pwm, &setPoint, PID_KP, PID_KI, PID_KD, P_ON_E, DIRECT);
Ramp ramp(RAMP_ACCEL, RAMP_MAXSPEED);
Ramp tableramp(TABLERAMP_ACCEL, TABLERAMP_MAXSPEED);

TrajTable table;
int play_time_ms = 0;
float play_ms_step = 10.0;

float speed_before_error = 0.0;

enum class State{manual, ramp, table, tableramp, error, recover, none} state = State::manual, next_state = State::none;

const char *state_name(State s) {
    const char* name;
    switch(s) {
        case(State::manual): name = "manual"; break;
        case(State::ramp): name = "ramp"; break;
        case(State::table): name = "table"; break;
        case(State::tableramp): name = "tableramp"; break;
        case(State::error): name = "error"; break;
        case(State::recover): name = "recover"; break;
        case(State::none): name = "none"; break;
    }
    return name;
}

absolute_time_t nextLed_time;
void blink(int ledPeriod) {
    static bool led = false;

    if(time_reached(nextLed_time)) {
        set_led(led = !led);
        nextLed_time = make_timeout_time_ms(ledPeriod);
    }
}

void gpio_callback(uint gpio, uint32_t events) {
    if(encoder.gpio_handler(gpio, events)) return;
}

void setup_motor_current() {
    adc_init();
    adc_gpio_init(PIN_MOT_CURRENT);

    gpio_init(PIN_MOT_SEL);
    gpio_set_dir(PIN_MOT_SEL, GPIO_OUT);
    gpio_put(PIN_MOT_SEL, 0);
}

void setup() {
    //ip_addr_t ip;
    //IP4_ADDR(&ip, 192, 168, 5, WIFI_IPADDR_D);
    //netif_set_ipaddr(netif_default, &ip);
    gpio_set_irq_enabled_with_callback(PIN_ENC_A, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true, &gpio_callback);
    setup_motor_current();
    adc_gpio_init(PIN_VBATT);
    encoder.init();
    motor.init();
    pid.SetOutputLimits(-32767.0, 32767.0);
    pid.SetSampleTime(5);
    table.setup();
}

float get_vbatt() {
    static float adc_filtered = 0;
    static absolute_time_t next_time = 0;
    if(time_reached(next_time)) {
        next_time = make_timeout_time_ms(1);
        adc_select_input(PIN_VBATT - 26);
        float adc = adc_read();
        adc_filtered += (adc - adc_filtered) * 0.05;
    }
    // Vadc = 1k/(1k + 4.7k)*Vbatt : Vbatt = Vadc*5.7
    // adc = Vadc * 4096 / 3.3 : Vadc = adc*3.3/4096
    // Vbatt =  adc * 3.3 * 5.7 / 4096
    return adc_filtered * (3.3f * 5.7f / 4096.0f);
}

void enableMotorControl(bool enable) {
    if(enable) {
        position = setPoint = encoder.get_count();
        ramp.set(setPoint);
    } else pid.SetMode(0);
}

int get_motor_current_mA() {
    static float current_filtered = 0;
    static absolute_time_t next_time = 0;
    if(time_reached(next_time)) {
        next_time = make_timeout_time_ms(1);
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

void motor_ramp_check_stalled(bool ramp_is_stopped) {
    static absolute_time_t stop_time = at_the_end_of_time;
    if(! ramp_is_stopped) {
        pid.SetMode(1);
        stop_time = make_timeout_time_ms(STALLED_TIME_MS);
        return;
    }
    if(! time_reached(stop_time)) return;
    stop_time = at_the_end_of_time;
    fraise_printf("l stalled... stopping motor\n");
    pid.SetMode(0);
    pwm = 0;
}

void motor_check_current() {
    static absolute_time_t alert_time = at_the_end_of_time;
    if(get_motor_current_mA() > OVERCURRENT_MA) {
        if(alert_time == at_the_end_of_time) {
            alert_time = make_timeout_time_ms(OVERCURRENT_MS);
        } else if(time_reached(alert_time)) {
            alert_time = at_the_end_of_time;
            enableMotorControl(false);
            motor.goto_pwm_ms(pwm = 0, 10);
            //state = 
            next_state = State::error;
            //tableramp.stop();
            fraise_printf("e overcurrent error!\n");
        }
    } else alert_time = at_the_end_of_time;
}

void motor_updatepwm() {
    motor.goto_pwm_ms(pwm, 10);
    gpio_put(PIN_MOT_SEL, pwm > 0); // update current monitoring
}

void motorcontrol_update() {
    position = encoder.get_count();

    switch(state) {
    case State::ramp:
        motor_ramp_check_stalled(ramp.is_stopped());
        if(pid.Compute()) {
            setPoint = ramp.get_position();
            ramp.compute();
            motor_updatepwm();
        }
        break;
    case State::table:
        pid.SetMode(1);
        if(pid.Compute()) {
            setPoint = table.current_value();
            motor_updatepwm();
        }
        break;
    case State::tableramp:
        motor_ramp_check_stalled(tableramp.is_stopped());
        if(pid.Compute()) {
            static int last_step = -1;
            int step = tableramp.get_position();
            if(last_step != step) {
                last_step = step;
                setPoint = table.read(step);
            }
            static int last_sent_step = -1;
            static absolute_time_t next_rstep_send_time = make_timeout_time_ms(0);
            if((last_sent_step != step) && time_reached(next_rstep_send_time)) {
                last_sent_step = step;
                fraise_printf("rstep %d\n", step);
                next_rstep_send_time = make_timeout_time_ms(50);
            }
            tableramp.compute();
            motor_updatepwm();
        }
        break;
    }

    motor.update();
    motor.reset_watchdog();
    motor_check_current();
}

bool table_search(int searched, int startpos, int direction) {
    fraise_printf("l table_search %d %d %d\n", searched, startpos, direction);
    for(int i = 0; (i < 300) && (startpos + i * direction >= 0) && (startpos + i * direction < table.get_length()); i++) {
        //fraise_printf("l %d %d\n", startpos + i * direction, table.read(startpos + i * direction));
        if(abs(table.read(startpos + i * direction) - searched) < 100) {
            fraise_printf("l table_search OK %d\n", startpos + i * direction);
            tableramp.set(startpos + i * direction);
            return true;
        }
    }
    return false;
}

bool recover_position() {
    int direction = speed_before_error >= 0.0 ? -1 : 1; // look backward
    if(table_search(encoder.get_count(), tableramp.get_position(), direction)) return true;
    // if not found, look forward:
    if(table_search(encoder.get_count(), tableramp.get_position(), -direction)) return true;
    return false;
}

void state_update() {
    static absolute_time_t change_state_time = at_the_end_of_time;
    static State last_next_state = next_state;

    if(last_next_state != next_state) {
        change_state_time = at_the_end_of_time;
        last_next_state = next_state;
    }

    /*if((next_state == State::tableramp) && (table.get_length() == 0)) {
        next_state = State::manual;
    }*/

    if(next_state != State::none) {
        switch(state) {
        case State::ramp:
        case State::manual:
        case State::table:
        case State::tableramp:
            state = State::manual;
            ramp.stop();
            table.stop();
            tableramp.stop();
            if(change_state_time == at_the_end_of_time) {
                int stop_time_ms = 3000.0 * abs(pwm) / 32768.0; // stop in maximum 3sec
                motor.goto_pwm_ms(pwm = 0, stop_time_ms);
                change_state_time = make_timeout_time_ms(stop_time_ms);
            }
            break;
            /*if(change_state_time == at_the_end_of_time) {
                ramp.stop();
                change_state_time = make_timeout_time_ms(10000);
            }
            if(ramp.is_stopped()) change_state_time = make_timeout_time_ms(0);
            break;
            table.stop();
            tableramp.stop();
            if(change_state_time == at_the_end_of_time) {
                int stop_time_ms = 3000.0 * abs(pwm) / 32768.0; // stop in maximum 3sec
                motor.goto_pwm_ms(pwm = 0, stop_time_ms);
                change_state_time = make_timeout_time_ms(stop_time_ms);
            }
            break;*/
        case State::error:
            //tableramp.stop();
            speed_before_error = tableramp.get_speed();
            tableramp.set(tableramp.get_position());
            change_state_time = make_timeout_time_ms(0);
            break;
        case State::recover:
            change_state_time = make_timeout_time_ms(0);
            break;
        }

        if(time_reached(change_state_time)) {
            change_state_time = at_the_end_of_time;
            state = next_state;
            next_state = State::none;
            switch(state) {
            case State::manual:
                enableMotorControl(false);
                break;
            case State::ramp:
                enableMotorControl(true);
                break;
            case State::table:
                table.stop();
                encoder.set_count(0);
                setPoint = 0;
                enableMotorControl(true);
                //pid.SetMode(1);
                table.play_at(play_time_ms, play_ms_step);
                break;
            case State::tableramp:
                tableramp.stop();
                tableramp.set(0);
                encoder.set_count(0);
                setPoint = 0;
                enableMotorControl(true);
                fraise_printf("rstep 0\n");
                //pid.SetMode(1);
                break;
            case State::error:
                tableramp.stop();
                enableMotorControl(false);
                pwm = 0;
                break;
            case State::recover:
                tableramp.stop();
                if(recover_position()) {
                    enableMotorControl(true);
                    fraise_printf("rstep %d\n", (int)tableramp.get_position());
                    state = State::tableramp;
                } else {
                    state = State::error;
                }
                break;
            }
        }
    }

    switch(state) {
    case State::manual:
        break;
    case State::ramp:
        break;
    case State::table:
        if(! table.is_playing()) {
            next_state = State::manual;
        }
        break;
    case State::tableramp:
        break;
   case State::error:
        enableMotorControl(false);
        //tableramp.stop();
        pwm = 0;
        break;
    }

    static absolute_time_t nextSendTime = 0;
    if(time_reached(nextSendTime)) {
        nextSendTime = make_timeout_time_ms(500);
        fraise_printf("state %s %d\n", state_name(state), table.get_hash());
    }
}

void state_prepare_change(int stateid) {
    switch(stateid) {
    case 0: next_state = State::manual; break;
    case 1: next_state = State::ramp; break;
    case 2: next_state = State::table; break;
    case 3: next_state = State::tableramp; break;
    case 4: next_state = State::error; break;
    case 5: next_state = State::recover; break;
    }
}

/*void ramp_to(float dest) {
    ramp.set_destination(dest);
    if(state == State::ramp) pid.SetMode(1);
}*/

void loop() {
    blink(250);
    state_update();
    table.update();
    motorcontrol_update();
    get_motor_current_mA();
    get_vbatt();
}

void fraise_receivebytes(const char* data, uint8_t len) {
    char command = fraise_get_uint8();
    switch(command) {
    case 0: // get sensors
        fraise_printf("sense %d %f\n", get_motor_current_mA(), get_vbatt());
        break;
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
    case 10:
        table.fraise_receive();
        break;
    case 11: // GOTO
        ramp.set_destination(fraise_get_int32());
        break;
    case 12: // SPEED
        ramp.set_maxspeed(fraise_get_int32());
        break;
    case 13: // RAMP STOP
        ramp.stop();
        break;
    case 14: // RAMP ACCEL
        ramp.set_accel(fraise_get_int32());
        break;
    case 20: // CHANGE STATE
        state_prepare_change(fraise_get_uint8());
        break;
    case 25: // START TABLE READ
        play_time_ms = fraise_get_int32() * 1000 + fraise_get_int16();
        play_ms_step = fraise_get_int32() / 1000.0;
        next_state = State::table;
        break;
    case 30: // CLEAR ERROR
        if(state == State::error) {
            pwm = 0;
            enableMotorControl(false);
            next_state = state = State::manual;
        }
        break;
    case 41: // TABLERAMP GOTO
        tableramp.set_destination(fraise_get_int32());
        break;
    case 42: // TABLERAMPSPEED
        tableramp.set_maxspeed(fraise_get_int32());
        break;
    case 43: // TABLERAMP RAMP STOP
        tableramp.stop();
        break;
    case 44: // TABLERAMP RAMP ACCEL
        tableramp.set_accel(fraise_get_int32());
        break;
    case 45: // TABLERAMP SET
        {
            int ramp_pos = fraise_get_int32();
            tableramp.set(ramp_pos);
            if(state == State::tableramp) {
                encoder.set_count(table.read(ramp_pos));
                setPoint = encoder.get_count();
            }
        }
        break;
    case 100: // MANUAL PWM
        if(state == State::manual) {
            enableMotorControl(false);
            pwm = fraise_get_int16();
            motor_updatepwm();
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
    case 120: // CURRENT SECURITY PARAMS
        {
            OVERCURRENT_MA = fraise_get_uint16();
            OVERCURRENT_MS = fraise_get_uint16();
        }
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
    case 241:
        wifi_print_status();
        {
            const ip4_addr_t *a = netif_ip4_addr(netif_default);
            fraise_printf("l IP %s\n", ip4addr_ntoa(a));
        }
        break;
    case 242:
        print_partitions();
        break;
    case 243:
        {
            intptr_t start;
            int length;
            if (get_partition_address("Tables", &start, &length)) {
                fraise_printf("l Tables partition found 0x%08x %d\n", start, length);
            } else fraise_printf("l Tables partition not found!\n");
        }
        break;
    case 244:
        {
            const char *ssid = WifiPartition::get_ssid();
            const char *pw = WifiPartition::get_password();
            if(ssid && pw) fraise_printf("l wifi current config %s %s\n", ssid, pw);
            else fraise_printf("l wifi config not initialized\n");
        }
        break;
    }
}

extern void print_version(); // from version.cpp

void fraise_receivechars(const char *data, uint8_t len){
    if(data[0] == 'E') { // Echo
        fraise_printf("E%s\n", data + 1);
    } else if(data[0] == 'V') { // Version
        print_version();
    } else if(data[0] == 'W') { // wifi config
        char ssid[64]{0};
        char pw[64]{0};
        sscanf(data, "WIFI_CONFIG %64s %64s", ssid, pw);
        if(strlen(ssid) && strlen(pw)) {
            fraise_printf("l wifi saving config %s %s: ", ssid, pw);
            if(WifiPartition::set(ssid, pw)) fraise_printf("SUCCESS\n");
            else fraise_printf("FAILURE\n");
        }
    } else fraise_printf("unknown %d\n", data[0]);
}

