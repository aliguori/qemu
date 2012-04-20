#ifndef MC146818RTC_H
#define MC146818RTC_H

#include "isa.h"
#include "mc146818rtc_regs.h"

typedef struct RTCState RTCState;

RTCState *rtc_init(ISABus *bus, int base_year, qemu_irq intercept_irq);
void rtc_set_memory(RTCState *dev, int addr, int val);
void rtc_set_date(RTCState *dev, const struct tm *tm);

#endif /* !MC146818RTC_H */
