#ifndef MC146818RTC_H
#define MC146818RTC_H

#include "isa.h"
#include "mc146818rtc_regs.h"
#include "qemu/pin.h"

#define RTC_IO_BASE 0x70

#define TYPE_RTC "mc146818rtc"
#define RTC(obj) OBJECT_CHECK(RTCState, (obj), TYPE_RTC)

typedef struct RTCState RTCState;

RTCState *rtc_init(int base_year);

Pin *rtc_get_irq(RTCState *s);
MemoryRegion *rtc_get_io(RTCState *s);

void rtc_set_memory(RTCState *dev, int addr, int val);
void rtc_set_date(RTCState *dev, const struct tm *tm);

static inline RTCState *rtc_isa_init(ISABus *isa_bus, int base_year)
{
    RTCState *rtc;

    rtc = rtc_init(2000);
    pin_connect_pin(rtc_get_irq(rtc), isa_get_pin(isa_bus, RTC_ISA_IRQ));
    memory_region_add_subregion(isa_bus->address_space_io, RTC_IO_BASE,
                                rtc_get_io(rtc));

    return rtc;
}

#endif /* !MC146818RTC_H */
