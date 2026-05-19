// Trajectory table reader

#pragma once
#include "fraise.h"
#include "pico/stdlib.h"
#include "math.h"

class TrajTable {
private:
    static const int MAX_STEPS = 60000; // 10mn @ 1step/10ms
    int zero_ms_since_boot;
    int table[MAX_STEPS]{0};
    int table_id = 5;
    int length = 0;
    int start_time_ms;
    int step = 0;
    bool playing = false;
    float step_ms = 10.0;
    int hash = 0;

public:
    int get_length() {
        return length;
    }

    int modulo(int x, int m) {
        int res = x % m;
        if(res < 0) res += m;
        return res;
    }

    void update_hash() { // 16bit checksum
        int64_t sum = 0;
        for(int i = 0; i < length; i++) {
            sum = (sum + modulo(read(i), 65536)) % 65536;
        }
        hash = sum;// && ((1 << 22) -1 );
    }

    int get_hash() { // 16bit checksum
        return hash;
    }

    int now_ms() {
        return to_ms_since_boot(get_absolute_time()) - zero_ms_since_boot;
    }

    void update_time(int current_sec) {
        int zero_ms_new = to_ms_since_boot(get_absolute_time()) - (current_sec * 1000);
        int diff_ms = zero_ms_new - zero_ms_since_boot;
        bool updated = false;
        if(diff_ms < 0 || diff_ms > 500) {
            zero_ms_since_boot = zero_ms_new;
            updated = true;
        } else {
            zero_ms_since_boot += diff_ms / 10;
        }
        fraise_printf("time %d %d\n", diff_ms, updated ? 1 : 0);
    }

    void setup() {
        udp.set_table(table_id, (uint8_t *)table, sizeof(table));
        udp.set_table_callbacks(table_id,
            [this](int i, int n){
                fraise_printf("l udp table start\n");
                length = 0;
            },
            [this](int i, int n){
                fraise_printf("l udp table end %d\n", n / 4);
                length = n / 4;
                update_hash();
            }
        );
    }

    int read(int step) {
        return table[MIN(step, MAX_STEPS - 1)];
    }

    int current_value() {
        return read(step);
    }

    void update() {
        if(! playing) return;
        if(start_time_ms <= now_ms()) {
            int new_step = (now_ms() - start_time_ms) / step_ms;
            if(new_step >= length) {
                playing = 0;
                return;
            }
            if(step != new_step) {
                step = new_step;
                fraise_printf("step %d %d\n", step, read(step));
            }
        }
    }

    void play_at(int time_ms, float _step_ms) {
        step = 0;
        start_time_ms = time_ms;
        step_ms = _step_ms;
        if(start_time_ms > now_ms()) {
            playing = true;
            fraise_printf("play %d %f\n", start_time_ms, step_ms);
        }
    }

    void stop() {
        playing = false;
    }

    bool is_playing() {
        return playing;
    }

    void fraise_receive() {
        char command = fraise_get_uint8();
        switch(command) {
        case 0: // update time
            update_time(fraise_get_int32());
            break;
        case 1: // table read
            {
                int step = fraise_get_int32();
                fraise_printf("tabread %d %d\n", step, read(step));
            }
            break;
        case 2: // play at
            if(playing) break;
            start_time_ms = fraise_get_int32() * 1000 + fraise_get_int16();
            step_ms = fraise_get_int32() / 1000.0;
            play_at(start_time_ms, step_ms);
            break;
        case 3: // stop
            stop();
            break;
        case 10: // get hash
            fraise_printf("tablehash %d\n", get_hash());
            break;
        }
    }
};
