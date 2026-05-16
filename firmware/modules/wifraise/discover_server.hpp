// UDP server handling Fraise and table messages

#pragma once

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/util/queue.h"
#include "pico/unique_id.h"

#include "lwip/pbuf.h"
#include "lwip/udp.h"

#include "fraise.h"
#include <string>

class DiscoverServer {
private:
    struct udp_pcb* client = NULL;
    int port = 61616;
    absolute_time_t next_time;
    char unique_name[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 2] = "f";
public:
    void setup() {
        client = udp_new();
        pico_get_unique_board_id_string(unique_name + 1, sizeof(unique_name) - 1);
        next_time = make_timeout_time_ms(100);
    }

    void send_message(const char* data, uint8_t len) {
        if(!client) return;
        cyw43_arch_lwip_begin();
        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
        memcpy((char*)p->payload, data, len);
        udp_sendto(client, p, IP_ADDR_BROADCAST, port);
        pbuf_free(p);
        cyw43_arch_lwip_end();
    }

    void service(int period_ms) {
        if(! time_reached(next_time)) return;
        next_time = make_timeout_time_ms(period_ms);
        send_message(unique_name, strlen(unique_name));
    }
};
