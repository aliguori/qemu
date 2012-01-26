#ifndef MC146818RTC_H
#define MC146818RTC_H

#include "isa.h"
#include "notify.h"

#define RTC_ISA_IRQ 8

#define TYPE_RTC "mc146818rtc"

typedef struct RTCState {
    ISADevice dev;
    MemoryRegion io;
    uint8_t cmos_data[128];
    uint8_t cmos_index;
    struct tm current_tm;
    int32_t base_year;
    qemu_irq irq;
    qemu_irq sqw_irq;
    int it_shift;
    /* periodic timer */
    QEMUTimer *periodic_timer;
    int64_t next_periodic_time;
    /* second update */
    int64_t next_second_time;
    uint16_t irq_reinject_on_ack_count;
    uint32_t irq_coalesced;
    uint32_t period;
    QEMUTimer *coalesced_timer;
    QEMUTimer *second_timer;
    QEMUTimer *second_timer2;
    Notifier clock_reset_notifier;
} RTCState;

ISADevice *rtc_init(ISABus *bus, int base_year, qemu_irq intercept_irq);
void rtc_set_memory(ISADevice *dev, int addr, int val);
void rtc_set_date(ISADevice *dev, const struct tm *tm);

#endif /* !MC146818RTC_H */
