/*
 * oboskrnl/arch/x86_64/cmos.h
 *
 * Copyright (c) 2025 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <memmanip.h>

#include <arch/x86_64/asm_helpers.h>
#include <arch/x86_64/cmos.h>

#include <uacpi/acpi.h>
#include <uacpi/tables.h>

static bool has_cmos = false;
static bool initialized_cmos = false;
static unsigned CMOS_REGISTER_CENTURY = 0;
static uint8_t cmos_flags = 0;

static uint8_t read_cmos8_no_bcd(uint8_t offset)
{
    cli();
    outb(CMOS_SELECT, offset);
    uint8_t val = inb(CMOS_DATA);
    sti();
    return val;
}
static uint8_t read_cmos8(uint8_t offset)
{
    uint8_t val = read_cmos8_no_bcd(offset);
    if (cmos_flags & 0x4)
        return val;
    uint8_t ret = val & 0xf;
    ret |= ((val >> 4) * 10);
    return ret;
}

obos_status Arch_CMOSInitialize()
{
    if (initialized_cmos)
        return OBOS_STATUS_ALREADY_INITIALIZED;
    struct uacpi_table tbl = {};
    uacpi_table_find_by_signature(ACPI_FADT_SIGNATURE, &tbl);
    struct acpi_fadt* fadt = (void*)tbl.hdr;
    if (fadt->iapc_boot_arch & ACPI_IA_PC_NO_CMOS_RTC)
        return OBOS_STATUS_NOT_FOUND;
    has_cmos = true;
    initialized_cmos = true;
    cmos_flags = read_cmos8_no_bcd(CMOS_REGISTER_STATUS_B);
    CMOS_REGISTER_CENTURY = fadt->century;
    return OBOS_STATUS_SUCCESS;
}

obos_status Arch_CMOSGetTimeOfDay(cmos_timeofday* time)
{
    if (!has_cmos)
        return OBOS_STATUS_NOT_FOUND;
    if (!time)
        return OBOS_STATUS_INVALID_ARGUMENT;
    uint8_t val = 0;
    do {
        val = read_cmos8_no_bcd(CMOS_REGISTER_STATUS_A);
    } while(val & BIT(7));
    time->seconds = read_cmos8(CMOS_REGISTER_SECONDS);
    time->minutes = read_cmos8(CMOS_REGISTER_MINUTES);
    time->hours = read_cmos8(CMOS_REGISTER_HOURS);
    time->day_of_month = read_cmos8(CMOS_REGISTER_DAY_OF_MONTH);
    time->month = read_cmos8(CMOS_REGISTER_MONTH);
    time->year = read_cmos8(CMOS_REGISTER_YEAR) + 4;
    uint16_t century = CMOS_REGISTER_CENTURY ? read_cmos8(CMOS_REGISTER_CENTURY) : 20;
    century *= 100;
    time->year += century;
    return OBOS_STATUS_SUCCESS;
}


static int days_from_civil(int y, unsigned m, unsigned d) 
{
	y -= m <= 2;
	const int era = (y >= 0 ? y : y - 399) / 400;
	const unsigned yoe = (unsigned)(y - era * 400); // [0, 399]
	const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1; // [0, 365]
	const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy; // [0, 146096]
	return era * 146097 + (int)(doe) - 719468;
}

obos_status SysS_ClockGet(int clock, long *secs, long *nsecs)
{
    OBOS_UNUSED(clock);
    if (!secs || !nsecs)
        return OBOS_STATUS_INVALID_ARGUMENT;

    cmos_timeofday tm = {};
    Arch_CMOSGetTimeOfDay(&tm);

    long days = days_from_civil(tm.year, tm.month, tm.day_of_month);
    long res = (days * 86400) + (tm.hours*60*60) + (tm.minutes * 60) + tm.seconds;
    obos_status status = memcpy_k_to_usr(secs, &res, sizeof(long));
    if (obos_is_error(status))
        return status;
    res *= 1000000000;
    status = memcpy_k_to_usr(nsecs, &res, sizeof(long));
    if (obos_is_error(status))
        return status;

    return OBOS_STATUS_SUCCESS;
}
