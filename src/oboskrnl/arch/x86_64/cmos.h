/*
 * oboskrnl/arch/x86_64/cmos.h
 *
 * Copyright (c) 2025 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

enum {
    CMOS_SELECT = 0x70,
    CMOS_DATA = 0x71,
};

// Century register should be fetched from the FADT
enum {
    // 1-byte, range 0-60
    CMOS_REGISTER_SECONDS = 0x00,
    // 1-byte, range 0-60
    CMOS_REGISTER_MINUTES = 0x02,
    // 1-byte, range 0-23 in 24-hour mode, 0-12 in 12-hour mode
    CMOS_REGISTER_HOURS = 0x04,
    // 1-byte, range 1-7
    CMOS_REGISTER_WEEKDAY = 0x06,
    // 1-byte, range 0-31
    CMOS_REGISTER_DAY_OF_MONTH = 0x07,
    // 1-byte, range 1-12
    CMOS_REGISTER_MONTH = 0x08,
    // 1-byte, range 0-99
    CMOS_REGISTER_YEAR = 0x09,
    // 1-byte
    CMOS_REGISTER_STATUS_A = 0x0A,
    // 1-byte
    CMOS_REGISTER_STATUS_B = 0x0B,
};

enum {
    CMOS_SUNDAY = 1,
    CMOS_MONDAY,
    CMOS_TUESDAY,
    CMOS_WEDNESDAY,
    CMOS_THURSDAY,
    CMOS_FRIDAY,
    CMOS_SATURDAY,
};

typedef struct cmos_timeofday
{
    uint8_t seconds;
    uint8_t minutes;
    uint8_t hours;
    uint8_t day_of_month;
    uint8_t month;
    uint16_t year;
} cmos_timeofday;

obos_status Arch_CMOSInitialize();
obos_status Arch_CMOSGetTimeOfDay(cmos_timeofday* time);
OBOS_EXPORT obos_status Arch_CMOSGetEpochTime(long* out);

obos_status SysS_ClockGet(int clock, long *secs, long *nsecs);