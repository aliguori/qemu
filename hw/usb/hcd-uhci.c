/*
 * USB UHCI controller emulation
 *
 * Copyright (c) 2005 Fabrice Bellard
 *
 * Copyright (c) 2008 Max Krasnyansky
 *     Magor rewrite of the UHCI data structures parser and frame processor
 *     Support for fully async operation and multiple outstanding transactions
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
 */
#include "hw/hw.h"
#include "hw/usb.h"
#include "hw/pci.h"
#include "qemu-timer.h"
#include "iov.h"
#include "dma.h"
#include "trace.h"

//#define DEBUG
//#define DEBUG_DUMP_DATA

#define UHCI_CMD_FGR      (1 << 4)
#define UHCI_CMD_EGSM     (1 << 3)
#define UHCI_CMD_GRESET   (1 << 2)
#define UHCI_CMD_HCRESET  (1 << 1)
#define UHCI_CMD_RS       (1 << 0)

#define UHCI_STS_HCHALTED (1 << 5)
#define UHCI_STS_HCPERR   (1 << 4)
#define UHCI_STS_HSERR    (1 << 3)
#define UHCI_STS_RD       (1 << 2)
#define UHCI_STS_USBERR   (1 << 1)
#define UHCI_STS_USBINT   (1 << 0)

#define TD_CTRL_SPD     (1 << 29)
#define TD_CTRL_ERROR_SHIFT  27
#define TD_CTRL_IOS     (1 << 25)
#define TD_CTRL_IOC     (1 << 24)
#define TD_CTRL_ACTIVE  (1 << 23)
#define TD_CTRL_STALL   (1 << 22)
#define TD_CTRL_BABBLE  (1 << 20)
#define TD_CTRL_NAK     (1 << 19)
#define TD_CTRL_TIMEOUT (1 << 18)

#define UHCI_PORT_SUSPEND (1 << 12)
#define UHCI_PORT_RESET (1 << 9)
#define UHCI_PORT_LSDA  (1 << 8)
#define UHCI_PORT_RD    (1 << 6)
#define UHCI_PORT_ENC   (1 << 3)
#define UHCI_PORT_EN    (1 << 2)
#define UHCI_PORT_CSC   (1 << 1)
#define UHCI_PORT_CCS   (1 << 0)

#define UHCI_PORT_READ_ONLY    (0x1bb)
#define UHCI_PORT_WRITE_CLEAR  (UHCI_PORT_CSC | UHCI_PORT_ENC)

#define FRAME_TIMER_FREQ 1000

#define FRAME_MAX_LOOPS  256

#define NB_PORTS 2

enum {
    TD_RESULT_STOP_FRAME = 10,
    TD_RESULT_COMPLETE,
    TD_RESULT_NEXT_QH,
    TD_RESULT_ASYNC_START,
    TD_RESULT_ASYNC_CONT,
};

typedef struct UHCIState UHCIState;
typedef struct UHCIAsync UHCIAsync;
typedef struct UHCIQueue UHCIQueue;

/* 
 * Pending async transaction.
 * 'packet' must be the first field because completion
 * handler does "(UHCIAsync *) pkt" cast.
 */

struct UHCIAsync {
    USBPacket packet;
    QEMUSGList sgl;
    UHCIQueue *queue;
    QTAILQ_ENTRY(UHCIAsync) next;
    uint32_t  td_addr;
    uint8_t   done;
};

struct UHCIQueue {
    uint32_t  qh_addr;
    uint32_t  token;
    UHCIState *uhci;
    USBEndpoint *ep;
    QTAILQ_ENTRY(UHCIQueue) next;
    QTAILQ_HEAD(asyncs_head, UHCIAsync) asyncs;
    int8_t    valid;
};

typedef struct UHCIPort {
    USBPort port;
    uint16_t ctrl;
} UHCIPort;

struct UHCIState {
    PCIDevice dev;
    MemoryRegion io_bar;
    USBBus bus; /* Note unused when we're a companion controller */
    uint16_t cmd; /* cmd register */
    uint16_t status;
    uint16_t intr; /* interrupt enable register */
    uint16_t frnum; /* frame number */
    uint32_t fl_base_addr; /* frame list base address */
    uint8_t sof_timing;
    uint8_t status2; /* bit 0 and 1 are used to generate UHCI_STS_USBINT */
    int64_t expire_time;
    QEMUTimer *frame_timer;
    QEMUBH *bh;
    uint32_t frame_bytes;
    uint32_t frame_bandwidth;
    UHCIPort ports[NB_PORTS];

    /* Interrupts that should be raised at the end of the current frame.  */
    uint32_t pending_int_mask;
    int irq_pin;

    /* Active packets */
    QTAILQ_HEAD(, UHCIQueue) queues;
    uint8_t num_ports_vmstate;

    /* Properties */
    char *masterbus;
    uint32_t firstport;
};

typedef struct UHCI_TD {
    uint32_t link;
    uint32_t ctrl; /* see TD_CTRL_xxx */
    uint32_t token;
    uint32_t buffer;
} UHCI_TD;

typedef struct UHCI_QH {
    uint32_t link;
    uint32_t el_link;
} UHCI_QH;

static void uhci_async_cancel(UHCIAsync *async);
static void uhci_queue_fill(UHCIQueue *q, UHCI_TD *td);

static inline int32_t uhci_queue_token(UHCI_TD *td)
{
    if ((td->token & (0xf << 15)) == 0) {
        /* ctrl ep, cover ep and dev, not pid! */
        return td->token & 0x7ff00;
    } else {
        /* covers ep, dev, pid -> identifies the endpoint */
        return td->token & 0x7ffff;
    }
}

static UHCIQueue *uhci_queue_new(UHCIState *s, uint32_t qh_addr, UHCI_TD *td,
                                 USBEndpoint *ep)
{
    UHCIQueue *queue;

    queue = g_new0(UHCIQueue, 1);
    queue->uhci = s;
    queue->qh_addr = qh_addr;
    queue->token = uhci_queue_token(td);
    queue->ep = ep;
    QTAILQ_INIT(&queue->asyncs);
    QTAILQ_INSERT_HEAD(&s->queues, queue, next);
    /* valid needs to be large enough to handle 10 frame delay
     * for initial isochronous requests */
    queue->valid = 32;
    trace_usb_uhci_queue_add(queue->token);
    return queue;
}

static void uhci_queue_free(UHCIQueue *queue, const char *reason)
{
    UHCIState *s = queue->uhci;
    UHCIAsync *async;

    while (!QTAILQ_EMPTY(&queue->asyncs)) {
        async = QTAILQ_FIRST(&queue->asyncs);
        uhci_async_cancel(async);
    }

    trace_usb_uhci_queue_del(queue->token, reason);
    QTAILQ_REMOVE(&s->queues, queue, next);
    g_free(queue);
}

static UHCIQueue *uhci_queue_find(UHCIState *s, UHCI_TD *td)
{
    uint32_t token = uhci_queue_token(td);
    UHCIQueue *queue;

    QTAILQ_FOREACH(queue, &s->queues, next) {
        if (queue->token == token) {
            return queue;
        }
    }
    return NULL;
}

static bool uhci_queue_verify(UHCIQueue *queue, uint32_t qh_addr, UHCI_TD *td,
                              uint32_t td_addr, bool queuing)
{
    UHCIAsync *first = QTAILQ_FIRST(&queue->asyncs);

    return queue->qh_addr == qh_addr &&
           queue->token == uhci_queue_token(td) &&
           (queuing || !(td->ctrl & TD_CTRL_ACTIVE) || first == NULL ||
            first->td_addr == td_addr);
}

static UHCIAsync *uhci_async_alloc(UHCIQueue *queue, uint32_t td_addr)
{
    UHCIAsync *async = g_new0(UHCIAsync, 1);

    async->queue = queue;
    async->td_addr = td_addr;
    usb_packet_init(&async->packet);
    pci_dma_sglist_init(&async->sgl, &queue->uhci->dev, 1);
    trace_usb_uhci_packet_add(async->queue->token, async->td_addr);

    return async;
}

static void uhci_async_free(UHCIAsync *async)
{
    trace_usb_uhci_packet_del(async->queue->token, async->td_addr);
    usb_packet_cleanup(&async->packet);
    qemu_sglist_destroy(&async->sgl);
    g_free(async);
}

static void uhci_async_link(UHCIAsync *async)
{
    UHCIQueue *queue = async->queue;
    QTAILQ_INSERT_TAIL(&queue->asyncs, async, next);
    trace_usb_uhci_packet_link_async(async->queue->token, async->td_addr);
}

static void uhci_async_unlink(UHCIAsync *async)
{
    UHCIQueue *queue = async->queue;
    QTAILQ_REMOVE(&queue->asyncs, async, next);
    trace_usb_uhci_packet_unlink_async(async->queue->token, async->td_addr);
}

static void uhci_async_cancel(UHCIAsync *async)
{
    uhci_async_unlink(async);
    trace_usb_uhci_packet_cancel(async->queue->token, async->td_addr,
                                 async->done);
    if (!async->done)
        usb_cancel_packet(&async->packet);
    usb_packet_unmap(&async->packet, &async->sgl);
    uhci_async_free(async);
}

/*
 * Mark all outstanding async packets as invalid.
 * This is used for canceling them when TDs are removed by the HCD.
 */
static void uhci_async_validate_begin(UHCIState *s)
{
    UHCIQueue *queue;

    QTAILQ_FOREACH(queue, &s->queues, next) {
        queue->valid--;
    }
}

/*
 * Cancel async packets that are no longer valid
 */
static void uhci_async_validate_end(UHCIState *s)
{
    UHCIQueue *queue, *n;

    QTAILQ_FOREACH_SAFE(queue, &s->queues, next, n) {
        if (!queue->valid) {
            uhci_queue_free(queue, "validate-end");
        }
    }
}

static void uhci_async_cancel_device(UHCIState *s, USBDevice *dev)
{
    UHCIQueue *queue, *n;

    QTAILQ_FOREACH_SAFE(queue, &s->queues, next, n) {
        if (queue->ep->dev == dev) {
            uhci_queue_free(queue, "cancel-device");
        }
    }
}

static void uhci_async_cancel_all(UHCIState *s)
{
    UHCIQueue *queue, *nq;

    QTAILQ_FOREACH_SAFE(queue, &s->queues, next, nq) {
        uhci_queue_free(queue, "cancel-all");
    }
}

static UHCIAsync *uhci_async_find_td(UHCIState *s, uint32_t td_addr)
{
    UHCIQueue *queue;
    UHCIAsync *async;

    QTAILQ_FOREACH(queue, &s->queues, next) {
        QTAILQ_FOREACH(async, &queue->asyncs, next) {
            if (async->td_addr == td_addr) {
                return async;
            }
        }
    }
    return NULL;
}

static void uhci_update_irq(UHCIState *s)
{
    int level;
    if (((s->status2 & 1) && (s->intr & (1 << 2))) ||
        ((s->status2 & 2) && (s->intr & (1 << 3))) ||
        ((s->status & UHCI_STS_USBERR) && (s->intr & (1 << 0))) ||
        ((s->status & UHCI_STS_RD) && (s->intr & (1 << 1))) ||
        (s->status & UHCI_STS_HSERR) ||
        (s->status & UHCI_STS_HCPERR)) {
        level = 1;
    } else {
        level = 0;
    }
    qemu_set_irq(s->dev.irq[s->irq_pin], level);
}

static void uhci_reset(void *opaque)
{
    UHCIState *s = opaque;
    uint8_t *pci_conf;
    int i;
    UHCIPort *port;

    trace_usb_uhci_reset();

    pci_conf = s->dev.config;

    pci_conf[0x6a] = 0x01; /* usb clock */
    pci_conf[0x6b] = 0x00;
    s->cmd = 0;
    s->status = 0;
    s->status2 = 0;
    s->intr = 0;
    s->fl_base_addr = 0;
    s->sof_timing = 64;

    for(i = 0; i < NB_PORTS; i++) {
        port = &s->ports[i];
        port->ctrl = 0x0080;
        if (port->port.dev && port->port.dev->attached) {
            usb_port_reset(&port->port);
        }
    }

    uhci_async_cancel_all(s);
    qemu_bh_cancel(s->bh);
    uhci_update_irq(s);
}

static const VMStateDescription vmstate_uhci_port = {
    .name = "uhci port",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields      = (VMStateField []) {
        VMSTATE_UINT16(ctrl, UHCIPort),
        VMSTATE_END_OF_LIST()
    }
};

static int uhci_post_load(void *opaque, int version_id)
{
    UHCIState *s = opaque;

    if (version_id < 2) {
        s->expire_time = qemu_get_clock_ns(vm_clock) +
            (get_ticks_per_sec() / FRAME_TIMER_FREQ);
    }
    return 0;
}

static const VMStateDescription vmstate_uhci = {
    .name = "uhci",
    .version_id = 2,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .post_load = uhci_post_load,
    .fields      = (VMStateField []) {
        VMSTATE_PCI_DEVICE(dev, UHCIState),
        VMSTATE_UINT8_EQUAL(num_ports_vmstate, UHCIState),
        VMSTATE_STRUCT_ARRAY(ports, UHCIState, NB_PORTS, 1,
                             vmstate_uhci_port, UHCIPort),
        VMSTATE_UINT16(cmd, UHCIState),
        VMSTATE_UINT16(status, UHCIState),
        VMSTATE_UINT16(intr, UHCIState),
        VMSTATE_UINT16(frnum, UHCIState),
        VMSTATE_UINT32(fl_base_addr, UHCIState),
        VMSTATE_UINT8(sof_timing, UHCIState),
        VMSTATE_UINT8(status2, UHCIState),
        VMSTATE_TIMER(frame_timer, UHCIState),
        VMSTATE_INT64_V(expire_time, UHCIState, 2),
        VMSTATE_END_OF_LIST()
    }
};

static void uhci_ioport_writeb(void *opaque, uint32_t addr, uint32_t val)
{
    UHCIState *s = opaque;

    addr &= 0x1f;
    switch(addr) {
    case 0x0c:
        s->sof_timing = val;
        break;
    }
}

static uint32_t uhci_ioport_readb(void *opaque, uint32_t addr)
{
    UHCIState *s = opaque;
    uint32_t val;

    addr &= 0x1f;
    switch(addr) {
    case 0x0c:
        val = s->sof_timing;
        break;
    default:
        val = 0xff;
        break;
    }
    return val;
}

static void uhci_ioport_writew(void *opaque, uint32_t addr, uint32_t val)
{
    UHCIState *s = opaque;

    addr &= 0x1f;
    trace_usb_uhci_mmio_writew(addr, val);

    switch(addr) {
    case 0x00:
        if ((val & UHCI_CMD_RS) && !(s->cmd & UHCI_CMD_RS)) {
            /* start frame processing */
            trace_usb_uhci_schedule_start();
            s->expire_time = qemu_get_clock_ns(vm_clock) +
                (get_ticks_per_sec() / FRAME_TIMER_FREQ);
            qemu_mod_timer(s->frame_timer, qemu_get_clock_ns(vm_clock));
            s->status &= ~UHCI_STS_HCHALTED;
        } else if (!(val & UHCI_CMD_RS)) {
            s->status |= UHCI_STS_HCHALTED;
        }
        if (val & UHCI_CMD_GRESET) {
            UHCIPort *port;
            int i;

            /* send reset on the USB bus */
            for(i = 0; i < NB_PORTS; i++) {
                port = &s->ports[i];
                usb_device_reset(port->port.dev);
            }
            uhci_reset(s);
            return;
        }
        if (val & UHCI_CMD_HCRESET) {
            uhci_reset(s);
            return;
        }
        s->cmd = val;
        break;
    case 0x02:
        s->status &= ~val;
        /* XXX: the chip spec is not coherent, so we add a hidden
           register to distinguish between IOC and SPD */
        if (val & UHCI_STS_USBINT)
            s->status2 = 0;
        uhci_update_irq(s);
        break;
    case 0x04:
        s->intr = val;
        uhci_update_irq(s);
        break;
    case 0x06:
        if (s->status & UHCI_STS_HCHALTED)
            s->frnum = val & 0x7ff;
        break;
    case 0x10 ... 0x1f:
        {
            UHCIPort *port;
            USBDevice *dev;
            int n;

            n = (addr >> 1) & 7;
            if (n >= NB_PORTS)
                return;
            port = &s->ports[n];
            dev = port->port.dev;
            if (dev && dev->attached) {
                /* port reset */
                if ( (val & UHCI_PORT_RESET) &&
                     !(port->ctrl & UHCI_PORT_RESET) ) {
                    usb_device_reset(dev);
                }
            }
            port->ctrl &= UHCI_PORT_READ_ONLY;
            port->ctrl |= (val & ~UHCI_PORT_READ_ONLY);
            /* some bits are reset when a '1' is written to them */
            port->ctrl &= ~(val & UHCI_PORT_WRITE_CLEAR);
        }
        break;
    }
}

static uint32_t uhci_ioport_readw(void *opaque, uint32_t addr)
{
    UHCIState *s = opaque;
    uint32_t val;

    addr &= 0x1f;
    switch(addr) {
    case 0x00:
        val = s->cmd;
        break;
    case 0x02:
        val = s->status;
        break;
    case 0x04:
        val = s->intr;
        break;
    case 0x06:
        val = s->frnum;
        break;
    case 0x10 ... 0x1f:
        {
            UHCIPort *port;
            int n;
            n = (addr >> 1) & 7;
            if (n >= NB_PORTS)
                goto read_default;
            port = &s->ports[n];
            val = port->ctrl;
        }
        break;
    default:
    read_default:
        val = 0xff7f; /* disabled port */
        break;
    }

    trace_usb_uhci_mmio_readw(addr, val);

    return val;
}

static void uhci_ioport_writel(void *opaque, uint32_t addr, uint32_t val)
{
    UHCIState *s = opaque;

    addr &= 0x1f;
    trace_usb_uhci_mmio_writel(addr, val);

    switch(addr) {
    case 0x08:
        s->fl_base_addr = val & ~0xfff;
        break;
    }
}

static uint32_t uhci_ioport_readl(void *opaque, uint32_t addr)
{
    UHCIState *s = opaque;
    uint32_t val;

    addr &= 0x1f;
    switch(addr) {
    case 0x08:
        val = s->fl_base_addr;
        break;
    default:
        val = 0xffffffff;
        break;
    }
    trace_usb_uhci_mmio_readl(addr, val);
    return val;
}

/* signal resume if controller suspended */
static void uhci_resume (void *opaque)
{
    UHCIState *s = (UHCIState *)opaque;

    if (!s)
        return;

    if (s->cmd & UHCI_CMD_EGSM) {
        s->cmd |= UHCI_CMD_FGR;
        s->status |= UHCI_STS_RD;
        uhci_update_irq(s);
    }
}

static void uhci_attach(USBPort *port1)
{
    UHCIState *s = port1->opaque;
    UHCIPort *port = &s->ports[port1->index];

    /* set connect status */
    port->ctrl |= UHCI_PORT_CCS | UHCI_PORT_CSC;

    /* update speed */
    if (port->port.dev->speed == USB_SPEED_LOW) {
        port->ctrl |= UHCI_PORT_LSDA;
    } else {
        port->ctrl &= ~UHCI_PORT_LSDA;
    }

    uhci_resume(s);
}

static void uhci_detach(USBPort *port1)
{
    UHCIState *s = port1->opaque;
    UHCIPort *port = &s->ports[port1->index];

    uhci_async_cancel_device(s, port1->dev);

    /* set connect status */
    if (port->ctrl & UHCI_PORT_CCS) {
        port->ctrl &= ~UHCI_PORT_CCS;
        port->ctrl |= UHCI_PORT_CSC;
    }
    /* disable port */
    if (port->ctrl & UHCI_PORT_EN) {
        port->ctrl &= ~UHCI_PORT_EN;
        port->ctrl |= UHCI_PORT_ENC;
    }

    uhci_resume(s);
}

static void uhci_child_detach(USBPort *port1, USBDevice *child)
{
    UHCIState *s = port1->opaque;

    uhci_async_cancel_device(s, child);
}

static void uhci_wakeup(USBPort *port1)
{
    UHCIState *s = port1->opaque;
    UHCIPort *port = &s->ports[port1->index];

    if (port->ctrl & UHCI_PORT_SUSPEND && !(port->ctrl & UHCI_PORT_RD)) {
        port->ctrl |= UHCI_PORT_RD;
        uhci_resume(s);
    }
}

static USBDevice *uhci_find_device(UHCIState *s, uint8_t addr)
{
    USBDevice *dev;
    int i;

    for (i = 0; i < NB_PORTS; i++) {
        UHCIPort *port = &s->ports[i];
        if (!(port->ctrl & UHCI_PORT_EN)) {
            continue;
        }
        dev = usb_find_device(&port->port, addr);
        if (dev != NULL) {
            return dev;
        }
    }
    return NULL;
}

static void uhci_read_td(UHCIState *s, UHCI_TD *td, uint32_t link)
{
    pci_dma_read(&s->dev, link & ~0xf, td, sizeof(*td));
    le32_to_cpus(&td->link);
    le32_to_cpus(&td->ctrl);
    le32_to_cpus(&td->token);
    le32_to_cpus(&td->buffer);
}

static int uhci_complete_td(UHCIState *s, UHCI_TD *td, UHCIAsync *async, uint32_t *int_mask)
{
    int len = 0, max_len, err, ret;
    uint8_t pid;

    max_len = ((td->token >> 21) + 1) & 0x7ff;
    pid = td->token & 0xff;

    ret = async->packet.result;

    if (td->ctrl & TD_CTRL_IOS)
        td->ctrl &= ~TD_CTRL_ACTIVE;

    if (ret < 0)
        goto out;

    len = async->packet.result;
    td->ctrl = (td->ctrl & ~0x7ff) | ((len - 1) & 0x7ff);

    /* The NAK bit may have been set by a previous frame, so clear it
       here.  The docs are somewhat unclear, but win2k relies on this
       behavior.  */
    td->ctrl &= ~(TD_CTRL_ACTIVE | TD_CTRL_NAK);
    if (td->ctrl & TD_CTRL_IOC)
        *int_mask |= 0x01;

    if (pid == USB_TOKEN_IN) {
        if ((td->ctrl & TD_CTRL_SPD) && len < max_len) {
            *int_mask |= 0x02;
            /* short packet: do not update QH */
            trace_usb_uhci_packet_complete_shortxfer(async->queue->token,
                                                     async->td_addr);
            return TD_RESULT_NEXT_QH;
        }
    }

    /* success */
    trace_usb_uhci_packet_complete_success(async->queue->token,
                                           async->td_addr);
    return TD_RESULT_COMPLETE;

out:
    switch(ret) {
    case USB_RET_NAK:
        td->ctrl |= TD_CTRL_NAK;
        return TD_RESULT_NEXT_QH;

    case USB_RET_STALL:
        td->ctrl |= TD_CTRL_STALL;
        trace_usb_uhci_packet_complete_stall(async->queue->token,
                                             async->td_addr);
        err = TD_RESULT_NEXT_QH;
        break;

    case USB_RET_BABBLE:
        td->ctrl |= TD_CTRL_BABBLE | TD_CTRL_STALL;
        /* frame interrupted */
        trace_usb_uhci_packet_complete_babble(async->queue->token,
                                              async->td_addr);
        err = TD_RESULT_STOP_FRAME;
        break;

    case USB_RET_IOERROR:
    case USB_RET_NODEV:
    default:
        td->ctrl |= TD_CTRL_TIMEOUT;
        td->ctrl &= ~(3 << TD_CTRL_ERROR_SHIFT);
        trace_usb_uhci_packet_complete_error(async->queue->token,
                                             async->td_addr);
        err = TD_RESULT_NEXT_QH;
        break;
    }

    td->ctrl &= ~TD_CTRL_ACTIVE;
    s->status |= UHCI_STS_USBERR;
    if (td->ctrl & TD_CTRL_IOC) {
        *int_mask |= 0x01;
    }
    uhci_update_irq(s);
    return err;
}

static int uhci_handle_td(UHCIState *s, UHCIQueue *q, uint32_t qh_addr,
                          UHCI_TD *td, uint32_t td_addr, uint32_t *int_mask)
{
    int len = 0, max_len;
    bool spd;
    bool queuing = (q != NULL);
    uint8_t pid = td->token & 0xff;
    UHCIAsync *async = uhci_async_find_td(s, td_addr);

    if (async) {
        if (uhci_queue_verify(async->queue, qh_addr, td, td_addr, queuing)) {
            assert(q == NULL || q == async->queue);
            q = async->queue;
        } else {
            uhci_queue_free(async->queue, "guest re-used pending td");
            async = NULL;
        }
    }

    if (q == NULL) {
        q = uhci_queue_find(s, td);
        if (q && !uhci_queue_verify(q, qh_addr, td, td_addr, queuing)) {
            uhci_queue_free(q, "guest re-used qh");
            q = NULL;
        }
    }

    if (q) {
        q->valid = 32;
    }

    /* Is active ? */
    if (!(td->ctrl & TD_CTRL_ACTIVE)) {
        if (async) {
            /* Guest marked a pending td non-active, cancel the queue */
            uhci_queue_free(async->queue, "pending td non-active");
        }
        /*
         * ehci11d spec page 22: "Even if the Active bit in the TD is already
         * cleared when the TD is fetched ... an IOC interrupt is generated"
         */
        if (td->ctrl & TD_CTRL_IOC) {
                *int_mask |= 0x01;
        }
        return TD_RESULT_NEXT_QH;
    }

    if (async) {
        if (queuing) {
            /* we are busy filling the queue, we are not prepared
               to consume completed packages then, just leave them
               in async state */
            return TD_RESULT_ASYNC_CONT;
        }
        if (!async->done) {
            UHCI_TD last_td;
            UHCIAsync *last = QTAILQ_LAST(&async->queue->asyncs, asyncs_head);
            /*
             * While we are waiting for the current td to complete, the guest
             * may have added more tds to the queue. Note we re-read the td
             * rather then caching it, as we want to see guest made changes!
             */
            uhci_read_td(s, &last_td, last->td_addr);
            uhci_queue_fill(async->queue, &last_td);

            return TD_RESULT_ASYNC_CONT;
        }
        uhci_async_unlink(async);
        goto done;
    }

    /* Allocate new packet */
    if (q == NULL) {
        USBDevice *dev = uhci_find_device(s, (td->token >> 8) & 0x7f);
        USBEndpoint *ep = usb_ep_get(dev, pid, (td->token >> 15) & 0xf);
        q = uhci_queue_new(s, qh_addr, td, ep);
    }
    async = uhci_async_alloc(q, td_addr);

    max_len = ((td->token >> 21) + 1) & 0x7ff;
    spd = (pid == USB_TOKEN_IN && (td->ctrl & TD_CTRL_SPD) != 0);
    usb_packet_setup(&async->packet, pid, q->ep, td_addr, spd,
                     (td->ctrl & TD_CTRL_IOC) != 0);
    qemu_sglist_add(&async->sgl, td->buffer, max_len);
    usb_packet_map(&async->packet, &async->sgl);

    switch(pid) {
    case USB_TOKEN_OUT:
    case USB_TOKEN_SETUP:
        len = usb_handle_packet(q->ep->dev, &async->packet);
        if (len >= 0)
            len = max_len;
        break;

    case USB_TOKEN_IN:
        len = usb_handle_packet(q->ep->dev, &async->packet);
        break;

    default:
        /* invalid pid : frame interrupted */
        usb_packet_unmap(&async->packet, &async->sgl);
        uhci_async_free(async);
        s->status |= UHCI_STS_HCPERR;
        uhci_update_irq(s);
        return TD_RESULT_STOP_FRAME;
    }
 
    if (len == USB_RET_ASYNC) {
        uhci_async_link(async);
        if (!queuing) {
            uhci_queue_fill(q, td);
        }
        return TD_RESULT_ASYNC_START;
    }

    async->packet.result = len;

done:
    len = uhci_complete_td(s, td, async, int_mask);
    usb_packet_unmap(&async->packet, &async->sgl);
    uhci_async_free(async);
    return len;
}

static void uhci_async_complete(USBPort *port, USBPacket *packet)
{
    UHCIAsync *async = container_of(packet, UHCIAsync, packet);
    UHCIState *s = async->queue->uhci;

    if (packet->result == USB_RET_REMOVE_FROM_QUEUE) {
        uhci_async_unlink(async);
        uhci_async_cancel(async);
        return;
    }

    async->done = 1;
    if (s->frame_bytes < s->frame_bandwidth) {
        qemu_bh_schedule(s->bh);
    }
}

static int is_valid(uint32_t link)
{
    return (link & 1) == 0;
}

static int is_qh(uint32_t link)
{
    return (link & 2) != 0;
}

static int depth_first(uint32_t link)
{
    return (link & 4) != 0;
}

/* QH DB used for detecting QH loops */
#define UHCI_MAX_QUEUES 128
typedef struct {
    uint32_t addr[UHCI_MAX_QUEUES];
    int      count;
} QhDb;

static void qhdb_reset(QhDb *db)
{
    db->count = 0;
}

/* Add QH to DB. Returns 1 if already present or DB is full. */
static int qhdb_insert(QhDb *db, uint32_t addr)
{
    int i;
    for (i = 0; i < db->count; i++)
        if (db->addr[i] == addr)
            return 1;

    if (db->count >= UHCI_MAX_QUEUES)
        return 1;

    db->addr[db->count++] = addr;
    return 0;
}

static void uhci_queue_fill(UHCIQueue *q, UHCI_TD *td)
{
    uint32_t int_mask = 0;
    uint32_t plink = td->link;
    UHCI_TD ptd;
    int ret;

    while (is_valid(plink)) {
        uhci_read_td(q->uhci, &ptd, plink);
        if (!(ptd.ctrl & TD_CTRL_ACTIVE)) {
            break;
        }
        if (uhci_queue_token(&ptd) != q->token) {
            break;
        }
        trace_usb_uhci_td_queue(plink & ~0xf, ptd.ctrl, ptd.token);
        ret = uhci_handle_td(q->uhci, q, q->qh_addr, &ptd, plink, &int_mask);
        if (ret == TD_RESULT_ASYNC_CONT) {
            break;
        }
        assert(ret == TD_RESULT_ASYNC_START);
        assert(int_mask == 0);
        plink = ptd.link;
    }
    usb_device_flush_ep_queue(q->ep->dev, q->ep);
}

static void uhci_process_frame(UHCIState *s)
{
    uint32_t frame_addr, link, old_td_ctrl, val, int_mask;
    uint32_t curr_qh, td_count = 0;
    int cnt, ret;
    UHCI_TD td;
    UHCI_QH qh;
    QhDb qhdb;

    frame_addr = s->fl_base_addr + ((s->frnum & 0x3ff) << 2);

    pci_dma_read(&s->dev, frame_addr, &link, 4);
    le32_to_cpus(&link);

    int_mask = 0;
    curr_qh  = 0;

    qhdb_reset(&qhdb);

    for (cnt = FRAME_MAX_LOOPS; is_valid(link) && cnt; cnt--) {
        if (s->frame_bytes >= s->frame_bandwidth) {
            /* We've reached the usb 1.1 bandwidth, which is
               1280 bytes/frame, stop processing */
            trace_usb_uhci_frame_stop_bandwidth();
            break;
        }
        if (is_qh(link)) {
            /* QH */
            trace_usb_uhci_qh_load(link & ~0xf);

            if (qhdb_insert(&qhdb, link)) {
                /*
                 * We're going in circles. Which is not a bug because
                 * HCD is allowed to do that as part of the BW management.
                 *
                 * Stop processing here if no transaction has been done
                 * since we've been here last time.
                 */
                if (td_count == 0) {
                    trace_usb_uhci_frame_loop_stop_idle();
                    break;
                } else {
                    trace_usb_uhci_frame_loop_continue();
                    td_count = 0;
                    qhdb_reset(&qhdb);
                    qhdb_insert(&qhdb, link);
                }
            }

            pci_dma_read(&s->dev, link & ~0xf, &qh, sizeof(qh));
            le32_to_cpus(&qh.link);
            le32_to_cpus(&qh.el_link);

            if (!is_valid(qh.el_link)) {
                /* QH w/o elements */
                curr_qh = 0;
                link = qh.link;
            } else {
                /* QH with elements */
            	curr_qh = link;
            	link = qh.el_link;
            }
            continue;
        }

        /* TD */
        uhci_read_td(s, &td, link);
        trace_usb_uhci_td_load(curr_qh & ~0xf, link & ~0xf, td.ctrl, td.token);

        old_td_ctrl = td.ctrl;
        ret = uhci_handle_td(s, NULL, curr_qh, &td, link, &int_mask);
        if (old_td_ctrl != td.ctrl) {
            /* update the status bits of the TD */
            val = cpu_to_le32(td.ctrl);
            pci_dma_write(&s->dev, (link & ~0xf) + 4, &val, sizeof(val));
        }

        switch (ret) {
        case TD_RESULT_STOP_FRAME: /* interrupted frame */
            goto out;

        case TD_RESULT_NEXT_QH:
        case TD_RESULT_ASYNC_CONT:
            trace_usb_uhci_td_nextqh(curr_qh & ~0xf, link & ~0xf);
            link = curr_qh ? qh.link : td.link;
            continue;

        case TD_RESULT_ASYNC_START:
            trace_usb_uhci_td_async(curr_qh & ~0xf, link & ~0xf);
            link = curr_qh ? qh.link : td.link;
            continue;

        case TD_RESULT_COMPLETE:
            trace_usb_uhci_td_complete(curr_qh & ~0xf, link & ~0xf);
            link = td.link;
            td_count++;
            s->frame_bytes += (td.ctrl & 0x7ff) + 1;

            if (curr_qh) {
                /* update QH element link */
                qh.el_link = link;
                val = cpu_to_le32(qh.el_link);
                pci_dma_write(&s->dev, (curr_qh & ~0xf) + 4, &val, sizeof(val));

                if (!depth_first(link)) {
                    /* done with this QH */
                    curr_qh = 0;
                    link    = qh.link;
                }
            }
            break;

        default:
            assert(!"unknown return code");
        }

        /* go to the next entry */
    }

out:
    s->pending_int_mask |= int_mask;
}

static void uhci_bh(void *opaque)
{
    UHCIState *s = opaque;
    uhci_process_frame(s);
}

static void uhci_frame_timer(void *opaque)
{
    UHCIState *s = opaque;

    /* prepare the timer for the next frame */
    s->expire_time += (get_ticks_per_sec() / FRAME_TIMER_FREQ);
    s->frame_bytes = 0;
    qemu_bh_cancel(s->bh);

    if (!(s->cmd & UHCI_CMD_RS)) {
        /* Full stop */
        trace_usb_uhci_schedule_stop();
        qemu_del_timer(s->frame_timer);
        uhci_async_cancel_all(s);
        /* set hchalted bit in status - UHCI11D 2.1.2 */
        s->status |= UHCI_STS_HCHALTED;
        return;
    }

    /* Complete the previous frame */
    if (s->pending_int_mask) {
        s->status2 |= s->pending_int_mask;
        s->status  |= UHCI_STS_USBINT;
        uhci_update_irq(s);
    }
    s->pending_int_mask = 0;

    /* Start new frame */
    s->frnum = (s->frnum + 1) & 0x7ff;

    trace_usb_uhci_frame_start(s->frnum);

    uhci_async_validate_begin(s);

    uhci_process_frame(s);

    uhci_async_validate_end(s);

    qemu_mod_timer(s->frame_timer, s->expire_time);
}

static const MemoryRegionPortio uhci_portio[] = {
    { 0, 32, 2, .write = uhci_ioport_writew, },
    { 0, 32, 2, .read = uhci_ioport_readw, },
    { 0, 32, 4, .write = uhci_ioport_writel, },
    { 0, 32, 4, .read = uhci_ioport_readl, },
    { 0, 32, 1, .write = uhci_ioport_writeb, },
    { 0, 32, 1, .read = uhci_ioport_readb, },
    PORTIO_END_OF_LIST()
};

static const MemoryRegionOps uhci_ioport_ops = {
    .old_portio = uhci_portio,
};

static USBPortOps uhci_port_ops = {
    .attach = uhci_attach,
    .detach = uhci_detach,
    .child_detach = uhci_child_detach,
    .wakeup = uhci_wakeup,
    .complete = uhci_async_complete,
};

static USBBusOps uhci_bus_ops = {
};

static int usb_uhci_common_initfn(PCIDevice *dev)
{
    PCIDeviceClass *pc = PCI_DEVICE_GET_CLASS(dev);
    UHCIState *s = DO_UPCAST(UHCIState, dev, dev);
    uint8_t *pci_conf = s->dev.config;
    int i;

    pci_conf[PCI_CLASS_PROG] = 0x00;
    /* TODO: reset value should be 0. */
    pci_conf[USB_SBRN] = USB_RELEASE_1; // release number

    switch (pc->device_id) {
    case PCI_DEVICE_ID_INTEL_82801I_UHCI1:
        s->irq_pin = 0;  /* A */
        break;
    case PCI_DEVICE_ID_INTEL_82801I_UHCI2:
        s->irq_pin = 1;  /* B */
        break;
    case PCI_DEVICE_ID_INTEL_82801I_UHCI3:
        s->irq_pin = 2;  /* C */
        break;
    default:
        s->irq_pin = 3;  /* D */
        break;
    }
    pci_config_set_interrupt_pin(pci_conf, s->irq_pin + 1);

    if (s->masterbus) {
        USBPort *ports[NB_PORTS];
        for(i = 0; i < NB_PORTS; i++) {
            ports[i] = &s->ports[i].port;
        }
        if (usb_register_companion(s->masterbus, ports, NB_PORTS,
                s->firstport, s, &uhci_port_ops,
                USB_SPEED_MASK_LOW | USB_SPEED_MASK_FULL) != 0) {
            return -1;
        }
    } else {
        usb_bus_new(&s->bus, &uhci_bus_ops, &s->dev.qdev);
        for (i = 0; i < NB_PORTS; i++) {
            usb_register_port(&s->bus, &s->ports[i].port, s, i, &uhci_port_ops,
                              USB_SPEED_MASK_LOW | USB_SPEED_MASK_FULL);
        }
    }
    s->bh = qemu_bh_new(uhci_bh, s);
    s->frame_timer = qemu_new_timer_ns(vm_clock, uhci_frame_timer, s);
    s->num_ports_vmstate = NB_PORTS;
    QTAILQ_INIT(&s->queues);

    qemu_register_reset(uhci_reset, s);

    memory_region_init_io(&s->io_bar, &uhci_ioport_ops, s, "uhci", 0x20);
    /* Use region 4 for consistency with real hardware.  BSD guests seem
       to rely on this.  */
    pci_register_bar(&s->dev, 4, PCI_BASE_ADDRESS_SPACE_IO, &s->io_bar);

    return 0;
}

static int usb_uhci_vt82c686b_initfn(PCIDevice *dev)
{
    UHCIState *s = DO_UPCAST(UHCIState, dev, dev);
    uint8_t *pci_conf = s->dev.config;

    /* USB misc control 1/2 */
    pci_set_long(pci_conf + 0x40,0x00001000);
    /* PM capability */
    pci_set_long(pci_conf + 0x80,0x00020001);
    /* USB legacy support  */
    pci_set_long(pci_conf + 0xc0,0x00002000);

    return usb_uhci_common_initfn(dev);
}

static void usb_uhci_exit(PCIDevice *dev)
{
    UHCIState *s = DO_UPCAST(UHCIState, dev, dev);

    memory_region_destroy(&s->io_bar);
}

static Property uhci_properties[] = {
    DEFINE_PROP_STRING("masterbus", UHCIState, masterbus),
    DEFINE_PROP_UINT32("firstport", UHCIState, firstport, 0),
    DEFINE_PROP_UINT32("bandwidth", UHCIState, frame_bandwidth, 1280),
    DEFINE_PROP_END_OF_LIST(),
};

static void piix3_uhci_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->init = usb_uhci_common_initfn;
    k->exit = usb_uhci_exit;
    k->vendor_id = PCI_VENDOR_ID_INTEL;
    k->device_id = PCI_DEVICE_ID_INTEL_82371SB_2;
    k->revision = 0x01;
    k->class_id = PCI_CLASS_SERIAL_USB;
    dc->vmsd = &vmstate_uhci;
    dc->props = uhci_properties;
}

static TypeInfo piix3_uhci_info = {
    .name          = "piix3-usb-uhci",
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(UHCIState),
    .class_init    = piix3_uhci_class_init,
};

static void piix4_uhci_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->init = usb_uhci_common_initfn;
    k->exit = usb_uhci_exit;
    k->vendor_id = PCI_VENDOR_ID_INTEL;
    k->device_id = PCI_DEVICE_ID_INTEL_82371AB_2;
    k->revision = 0x01;
    k->class_id = PCI_CLASS_SERIAL_USB;
    dc->vmsd = &vmstate_uhci;
    dc->props = uhci_properties;
}

static TypeInfo piix4_uhci_info = {
    .name          = "piix4-usb-uhci",
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(UHCIState),
    .class_init    = piix4_uhci_class_init,
};

static void vt82c686b_uhci_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->init = usb_uhci_vt82c686b_initfn;
    k->exit = usb_uhci_exit;
    k->vendor_id = PCI_VENDOR_ID_VIA;
    k->device_id = PCI_DEVICE_ID_VIA_UHCI;
    k->revision = 0x01;
    k->class_id = PCI_CLASS_SERIAL_USB;
    dc->vmsd = &vmstate_uhci;
    dc->props = uhci_properties;
}

static TypeInfo vt82c686b_uhci_info = {
    .name          = "vt82c686b-usb-uhci",
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(UHCIState),
    .class_init    = vt82c686b_uhci_class_init,
};

static void ich9_uhci1_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->init = usb_uhci_common_initfn;
    k->vendor_id = PCI_VENDOR_ID_INTEL;
    k->device_id = PCI_DEVICE_ID_INTEL_82801I_UHCI1;
    k->revision = 0x03;
    k->class_id = PCI_CLASS_SERIAL_USB;
    dc->vmsd = &vmstate_uhci;
    dc->props = uhci_properties;
}

static TypeInfo ich9_uhci1_info = {
    .name          = "ich9-usb-uhci1",
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(UHCIState),
    .class_init    = ich9_uhci1_class_init,
};

static void ich9_uhci2_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->init = usb_uhci_common_initfn;
    k->vendor_id = PCI_VENDOR_ID_INTEL;
    k->device_id = PCI_DEVICE_ID_INTEL_82801I_UHCI2;
    k->revision = 0x03;
    k->class_id = PCI_CLASS_SERIAL_USB;
    dc->vmsd = &vmstate_uhci;
    dc->props = uhci_properties;
}

static TypeInfo ich9_uhci2_info = {
    .name          = "ich9-usb-uhci2",
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(UHCIState),
    .class_init    = ich9_uhci2_class_init,
};

static void ich9_uhci3_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->init = usb_uhci_common_initfn;
    k->vendor_id = PCI_VENDOR_ID_INTEL;
    k->device_id = PCI_DEVICE_ID_INTEL_82801I_UHCI3;
    k->revision = 0x03;
    k->class_id = PCI_CLASS_SERIAL_USB;
    dc->vmsd = &vmstate_uhci;
    dc->props = uhci_properties;
}

static TypeInfo ich9_uhci3_info = {
    .name          = "ich9-usb-uhci3",
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(UHCIState),
    .class_init    = ich9_uhci3_class_init,
};

static void uhci_register_types(void)
{
    type_register_static(&piix3_uhci_info);
    type_register_static(&piix4_uhci_info);
    type_register_static(&vt82c686b_uhci_info);
    type_register_static(&ich9_uhci1_info);
    type_register_static(&ich9_uhci2_info);
    type_register_static(&ich9_uhci3_info);
}

type_init(uhci_register_types)
