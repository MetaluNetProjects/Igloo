// Ramp generator

#pragma once
#include "fraise.h"
#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "pico/mutex.h"
#include "math.h"
#include <algorithm>

class Ramp {
private:
    float accel = 1.0f;
    float position = 0.0f;
    float speed = 0.0f;
    float maxspeed = 100.0;
    float destination = 0.0f;
    absolute_time_t last_time = get_absolute_time();

public:
    Ramp(float _accel, float _maxspeed): accel(_accel), maxspeed(_maxspeed) {}
    
    void set_destination(float d){
        destination = d;
    }

    float get_position(){
        return position;
    }

    void set(float d){
        destination = position = d;
        speed = 0.0f;
    }

    void set_maxspeed(float _maxspeed){
        maxspeed = _maxspeed;
    }

    void set_accel(float _accel){
        accel = _accel;
    }

    bool is_stopped(){
        return (abs(position - destination) < 1.0) && (speed == 0.0);
    }

    void stop() {
        float stop_distance = abs(speed) * speed / (2.0 * accel);
        set_destination(position + stop_distance);
    }

    void compute() {
        float dt = absolute_time_diff_us(last_time, get_absolute_time()) / (float)1e6;
        last_time = get_absolute_time();
        if(dt == 0) return;

/*
    stop_position = position + abs(speed) * speed / (2.0 * accel)
    stop_position_ideal = destination
    destination - position = abs(speed_ideal) * speed_ideal / (2.0 * accel)
    sgn(speed_ideal)*speed_ideal² = (destination - position) * 2.0 * accel
    speed_ideal² = abs(destination - position) * 2.0 * accel
    speed_ideal = sgn(destination - position) * sqrt(abs(destination - position) * 2.0 * accel)
*/
        float error = destination - position;
        float speed_ideal = copysign(sqrt(abs(error) * 2.0 * accel), error);
        float dv = std::clamp(speed_ideal - speed, -accel * dt, accel * dt);

        speed += dv;
        if(speed > maxspeed) speed = MAX(speed - accel * dt, maxspeed);
        else if(speed < -maxspeed) speed = MIN(speed + accel * dt, -maxspeed);
        if(abs(speed) < (accel * /*dt **/ 0.001)) {
            speed = 0.0;
        }
        position += speed * dt;
    }
};
