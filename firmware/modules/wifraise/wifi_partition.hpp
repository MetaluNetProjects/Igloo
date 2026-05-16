/**
 * Copyright (c) 2026 metalu.net
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once
#include "partition_info.h"
#include <string>
#include "pico/bootrom.h"
#include "boot/picobin.h"
#include "hardware/flash.h"

class WifiPartition {
private:
    inline static const int field_size = 64;
    inline static intptr_t start = 0;
    inline static int length = 0;
    inline static char ssid[field_size]{0};
    inline static char password[field_size]{0};

    static bool read_bytes(char* buf, int offset) {
        cflash_flags_t flags;
        flags.flags = (CFLASH_OP_VALUE_READ << CFLASH_OP_LSB) | (CFLASH_SECLEVEL_VALUE_SECURE << CFLASH_SECLEVEL_LSB);
        int ret = rom_flash_op(flags, start + XIP_BASE + offset, field_size, (uint8_t*)buf);
        return (ret == 0);
    }

    static bool write() {
        uint8_t flash_buffer[FLASH_SECTOR_SIZE] = {};
        memcpy(flash_buffer, ssid, strlen(ssid) + 1);
        memcpy(flash_buffer + field_size, password, strlen(password) + 1);
        uint32_t status = save_and_disable_interrupts();
        flash_range_erase(start, FLASH_SECTOR_SIZE);
        flash_range_program(start, (const uint8_t *)flash_buffer, FLASH_SECTOR_SIZE);
        restore_interrupts(status);
        return true;
    }

public:
    static void setup() {
        if (get_partition_address("WIFI", &start, &length)) {
            fraise_printf("l WIFI partition found 0x%08x %d\n", start, length);
        } else fraise_printf("l WIFI partition not found!\n");
    }
    static bool is_initialized() {
        return (start != 0) && (length != 0);
    }
    static const char *get_ssid() {
        if(! is_initialized()) return nullptr;
        if(!read_bytes(ssid, 0)) return nullptr;
        if(ssid[0] <= 0) return nullptr;
        int len = strnlen(ssid, field_size);
        if(len > 0 && len < field_size - 1) return ssid;
        else return nullptr;
    }
    static const char *get_password() {
        if(! is_initialized()) return nullptr;
        if(!read_bytes(password, field_size)) return nullptr;
        if(password[0] < 0) return nullptr;
        int len = strnlen(password, field_size - 1);
        if(len > 0 && len < field_size) return password;
        else return nullptr;
    }
    static bool set(const char *ssid_new, const char *passwd_new) {
        if(! is_initialized()) return false;
        strncpy(ssid, ssid_new, field_size);
        strncpy(password, passwd_new, field_size);
        write();
        return true;
    }
    static char get_char(int n) {
        if(! is_initialized()) return 0;
        return 0;
    }
};
