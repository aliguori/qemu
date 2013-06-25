/*
 * QEMU PowerPC pSeries Logical Partition (aka sPAPR) hardware System Emulator
 *
 * PAPR Virtualized Interrupt System, aka ICS/ICP aka xics
 *
 * Copyright (c) 2010,2011 David Gibson, IBM Corporation.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */
#if !defined(__XICS_H__)
#define __XICS_H__

#include "hw/sysbus.h"

#define TYPE_XICS "xics"
#define XICS(obj) OBJECT_CHECK(struct icp_state, (obj), TYPE_XICS)

#define XICS_IPI        0x2
#define XICS_BUID       0x1
#define XICS_IRQ_BASE   (XICS_BUID << 12)

/*
 * We currently only support one BUID which is our interrupt base
 * (the kernel implementation supports more but we don't exploit
 *  that yet)
 */

typedef struct icp_state {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    uint32_t nr_servers;
    uint32_t nr_irqs;
    struct icp_server_state *ss;
    struct ics_state *ics;
} icp_state;

typedef struct icp_server_state {
    uint32_t xirr;
    uint8_t pending_priority;
    uint8_t mfrr;
    qemu_irq output;
} icp_server_state;

typedef struct ics_state {
    uint32_t nr_irqs;
    uint32_t offset;
    qemu_irq *qirqs;
    bool *islsi;
    struct ics_irq_state *irqs;
    struct icp_state *icp;
} ics_state;

typedef struct ics_irq_state {
    uint32_t server;
    uint8_t priority;
    uint8_t saved_priority;
#define XICS_STATUS_ASSERTED           0x1
#define XICS_STATUS_SENT               0x2
#define XICS_STATUS_REJECTED           0x4
#define XICS_STATUS_MASKED_PENDING     0x8
    uint8_t status;
} ics_irq_state;

qemu_irq xics_get_qirq(struct icp_state *icp, int irq);
void xics_set_irq_type(struct icp_state *icp, int irq, bool lsi);

void xics_common_init(struct icp_state *icp, qemu_irq_handler handler);
void xics_common_cpu_setup(struct icp_state *icp, PowerPCCPU *cpu);
void xics_common_reset(struct icp_state *icp);

void xics_cpu_setup(struct icp_state *icp, PowerPCCPU *cpu);

extern const VMStateDescription vmstate_icp_server;
extern const VMStateDescription vmstate_ics;

#endif /* __XICS_H__ */
