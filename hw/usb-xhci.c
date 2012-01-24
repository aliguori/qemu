/*
 * USB xHCI controller emulation
 *
 * Copyright (c) 2011 Securiforest
 * Date: 2011-05-11 ;  Author: Hector Martin <hector@marcansoft.com>
 * Based on usb-ohci.c, emulates Renesas NEC USB 3.0
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
#include "hw.h"
#include "qemu-timer.h"
#include "usb.h"
#include "pci.h"
#include "qdev-addr.h"
#include "msi.h"

//#define DEBUG_XHCI
//#define DEBUG_DATA

#ifdef DEBUG_XHCI
#define DPRINTF(...) fprintf(stderr, __VA_ARGS__)
#else
#define DPRINTF(...) do {} while (0)
#endif
#define FIXME() do { fprintf(stderr, "FIXME %s:%d\n", \
                             __func__, __LINE__); abort(); } while (0)

#define MAXSLOTS 8
#define MAXINTRS 1

#define USB2_PORTS 4
#define USB3_PORTS 4

#define MAXPORTS (USB2_PORTS+USB3_PORTS)

#define TD_QUEUE 24
#define BG_XFERS 8
#define BG_PKTS 8

/* Very pessimistic, let's hope it's enough for all cases */
#define EV_QUEUE (((3*TD_QUEUE)+16)*MAXSLOTS)
/* Do not deliver ER Full events. NEC's driver does some things not bound
 * to the specs when it gets them */
#define ER_FULL_HACK

#define LEN_CAP         0x40
#define OFF_OPER        LEN_CAP
#define LEN_OPER        (0x400 + 0x10 * MAXPORTS)
#define OFF_RUNTIME     ((OFF_OPER + LEN_OPER + 0x20) & ~0x1f)
#define LEN_RUNTIME     (0x20 + MAXINTRS * 0x20)
#define OFF_DOORBELL    (OFF_RUNTIME + LEN_RUNTIME)
#define LEN_DOORBELL    ((MAXSLOTS + 1) * 0x20)

/* must be power of 2 */
#define LEN_REGS        0x2000

#if (OFF_DOORBELL + LEN_DOORBELL) > LEN_REGS
# error Increase LEN_REGS
#endif

#if MAXINTRS > 1
# error TODO: only one interrupter supported
#endif

/* bit definitions */
#define USBCMD_RS       (1<<0)
#define USBCMD_HCRST    (1<<1)
#define USBCMD_INTE     (1<<2)
#define USBCMD_HSEE     (1<<3)
#define USBCMD_LHCRST   (1<<7)
#define USBCMD_CSS      (1<<8)
#define USBCMD_CRS      (1<<9)
#define USBCMD_EWE      (1<<10)
#define USBCMD_EU3S     (1<<11)

#define USBSTS_HCH      (1<<0)
#define USBSTS_HSE      (1<<2)
#define USBSTS_EINT     (1<<3)
#define USBSTS_PCD      (1<<4)
#define USBSTS_SSS      (1<<8)
#define USBSTS_RSS      (1<<9)
#define USBSTS_SRE      (1<<10)
#define USBSTS_CNR      (1<<11)
#define USBSTS_HCE      (1<<12)


#define PORTSC_CCS          (1<<0)
#define PORTSC_PED          (1<<1)
#define PORTSC_OCA          (1<<3)
#define PORTSC_PR           (1<<4)
#define PORTSC_PLS_SHIFT        5
#define PORTSC_PLS_MASK     0xf
#define PORTSC_PP           (1<<9)
#define PORTSC_SPEED_SHIFT      10
#define PORTSC_SPEED_MASK   0xf
#define PORTSC_SPEED_FULL   (1<<10)
#define PORTSC_SPEED_LOW    (2<<10)
#define PORTSC_SPEED_HIGH   (3<<10)
#define PORTSC_SPEED_SUPER  (4<<10)
#define PORTSC_PIC_SHIFT        14
#define PORTSC_PIC_MASK     0x3
#define PORTSC_LWS          (1<<16)
#define PORTSC_CSC          (1<<17)
#define PORTSC_PEC          (1<<18)
#define PORTSC_WRC          (1<<19)
#define PORTSC_OCC          (1<<20)
#define PORTSC_PRC          (1<<21)
#define PORTSC_PLC          (1<<22)
#define PORTSC_CEC          (1<<23)
#define PORTSC_CAS          (1<<24)
#define PORTSC_WCE          (1<<25)
#define PORTSC_WDE          (1<<26)
#define PORTSC_WOE          (1<<27)
#define PORTSC_DR           (1<<30)
#define PORTSC_WPR          (1<<31)

#define CRCR_RCS        (1<<0)
#define CRCR_CS         (1<<1)
#define CRCR_CA         (1<<2)
#define CRCR_CRR        (1<<3)

#define IMAN_IP         (1<<0)
#define IMAN_IE         (1<<1)

#define ERDP_EHB        (1<<3)

#define TRB_SIZE 16
typedef struct XHCITRB {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
    target_phys_addr_t addr;
    bool ccs;
} XHCITRB;


typedef enum TRBType {
    TRB_RESERVED = 0,
    TR_NORMAL,
    TR_SETUP,
    TR_DATA,
    TR_STATUS,
    TR_ISOCH,
    TR_LINK,
    TR_EVDATA,
    TR_NOOP,
    CR_ENABLE_SLOT,
    CR_DISABLE_SLOT,
    CR_ADDRESS_DEVICE,
    CR_CONFIGURE_ENDPOINT,
    CR_EVALUATE_CONTEXT,
    CR_RESET_ENDPOINT,
    CR_STOP_ENDPOINT,
    CR_SET_TR_DEQUEUE,
    CR_RESET_DEVICE,
    CR_FORCE_EVENT,
    CR_NEGOTIATE_BW,
    CR_SET_LATENCY_TOLERANCE,
    CR_GET_PORT_BANDWIDTH,
    CR_FORCE_HEADER,
    CR_NOOP,
    ER_TRANSFER = 32,
    ER_COMMAND_COMPLETE,
    ER_PORT_STATUS_CHANGE,
    ER_BANDWIDTH_REQUEST,
    ER_DOORBELL,
    ER_HOST_CONTROLLER,
    ER_DEVICE_NOTIFICATION,
    ER_MFINDEX_WRAP,
    /* vendor specific bits */
    CR_VENDOR_VIA_CHALLENGE_RESPONSE = 48,
    CR_VENDOR_NEC_FIRMWARE_REVISION  = 49,
    CR_VENDOR_NEC_CHALLENGE_RESPONSE = 50,
} TRBType;

#define CR_LINK TR_LINK

typedef enum TRBCCode {
    CC_INVALID = 0,
    CC_SUCCESS,
    CC_DATA_BUFFER_ERROR,
    CC_BABBLE_DETECTED,
    CC_USB_TRANSACTION_ERROR,
    CC_TRB_ERROR,
    CC_STALL_ERROR,
    CC_RESOURCE_ERROR,
    CC_BANDWIDTH_ERROR,
    CC_NO_SLOTS_ERROR,
    CC_INVALID_STREAM_TYPE_ERROR,
    CC_SLOT_NOT_ENABLED_ERROR,
    CC_EP_NOT_ENABLED_ERROR,
    CC_SHORT_PACKET,
    CC_RING_UNDERRUN,
    CC_RING_OVERRUN,
    CC_VF_ER_FULL,
    CC_PARAMETER_ERROR,
    CC_BANDWIDTH_OVERRUN,
    CC_CONTEXT_STATE_ERROR,
    CC_NO_PING_RESPONSE_ERROR,
    CC_EVENT_RING_FULL_ERROR,
    CC_INCOMPATIBLE_DEVICE_ERROR,
    CC_MISSED_SERVICE_ERROR,
    CC_COMMAND_RING_STOPPED,
    CC_COMMAND_ABORTED,
    CC_STOPPED,
    CC_STOPPED_LENGTH_INVALID,
    CC_MAX_EXIT_LATENCY_TOO_LARGE_ERROR = 29,
    CC_ISOCH_BUFFER_OVERRUN = 31,
    CC_EVENT_LOST_ERROR,
    CC_UNDEFINED_ERROR,
    CC_INVALID_STREAM_ID_ERROR,
    CC_SECONDARY_BANDWIDTH_ERROR,
    CC_SPLIT_TRANSACTION_ERROR
} TRBCCode;

#define TRB_C               (1<<0)
#define TRB_TYPE_SHIFT          10
#define TRB_TYPE_MASK       0x3f
#define TRB_TYPE(t)         (((t).control >> TRB_TYPE_SHIFT) & TRB_TYPE_MASK)

#define TRB_EV_ED           (1<<2)

#define TRB_TR_ENT          (1<<1)
#define TRB_TR_ISP          (1<<2)
#define TRB_TR_NS           (1<<3)
#define TRB_TR_CH           (1<<4)
#define TRB_TR_IOC          (1<<5)
#define TRB_TR_IDT          (1<<6)
#define TRB_TR_TBC_SHIFT        7
#define TRB_TR_TBC_MASK     0x3
#define TRB_TR_BEI          (1<<9)
#define TRB_TR_TLBPC_SHIFT      16
#define TRB_TR_TLBPC_MASK   0xf
#define TRB_TR_FRAMEID_SHIFT    20
#define TRB_TR_FRAMEID_MASK 0x7ff
#define TRB_TR_SIA          (1<<31)

#define TRB_TR_DIR          (1<<16)

#define TRB_CR_SLOTID_SHIFT     24
#define TRB_CR_SLOTID_MASK  0xff
#define TRB_CR_EPID_SHIFT       16
#define TRB_CR_EPID_MASK    0x1f

#define TRB_CR_BSR          (1<<9)
#define TRB_CR_DC           (1<<9)

#define TRB_LK_TC           (1<<1)

#define EP_TYPE_MASK        0x7
#define EP_TYPE_SHIFT           3

#define EP_STATE_MASK       0x7
#define EP_DISABLED         (0<<0)
#define EP_RUNNING          (1<<0)
#define EP_HALTED           (2<<0)
#define EP_STOPPED          (3<<0)
#define EP_ERROR            (4<<0)

#define SLOT_STATE_MASK     0x1f
#define SLOT_STATE_SHIFT        27
#define SLOT_STATE(s)       (((s)>>SLOT_STATE_SHIFT)&SLOT_STATE_MASK)
#define SLOT_ENABLED        0
#define SLOT_DEFAULT        1
#define SLOT_ADDRESSED      2
#define SLOT_CONFIGURED     3

#define SLOT_CONTEXT_ENTRIES_MASK 0x1f
#define SLOT_CONTEXT_ENTRIES_SHIFT 27

typedef enum EPType {
    ET_INVALID = 0,
    ET_ISO_OUT,
    ET_BULK_OUT,
    ET_INTR_OUT,
    ET_CONTROL,
    ET_ISO_IN,
    ET_BULK_IN,
    ET_INTR_IN,
} EPType;

typedef struct XHCIRing {
    target_phys_addr_t base;
    target_phys_addr_t dequeue;
    bool ccs;
} XHCIRing;

typedef struct XHCIPort {
    USBPort port;
    uint32_t portsc;
} XHCIPort;

struct XHCIState;
typedef struct XHCIState XHCIState;

typedef struct XHCITransfer {
    XHCIState *xhci;
    USBPacket packet;
    bool running;
    bool cancelled;
    bool complete;
    bool backgrounded;
    unsigned int iso_pkts;
    unsigned int slotid;
    unsigned int epid;
    bool in_xfer;
    bool iso_xfer;
    bool bg_xfer;

    unsigned int trb_count;
    unsigned int trb_alloced;
    XHCITRB *trbs;

    unsigned int data_length;
    unsigned int data_alloced;
    uint8_t *data;

    TRBCCode status;

    unsigned int pkts;
    unsigned int pktsize;
    unsigned int cur_pkt;
} XHCITransfer;

typedef struct XHCIEPContext {
    XHCIRing ring;
    unsigned int next_xfer;
    unsigned int comp_xfer;
    XHCITransfer transfers[TD_QUEUE];
    bool bg_running;
    bool bg_updating;
    unsigned int next_bg;
    XHCITransfer bg_transfers[BG_XFERS];
    EPType type;
    target_phys_addr_t pctx;
    unsigned int max_psize;
    bool has_bg;
    uint32_t state;
} XHCIEPContext;

typedef struct XHCISlot {
    bool enabled;
    target_phys_addr_t ctx;
    unsigned int port;
    unsigned int devaddr;
    XHCIEPContext * eps[31];
} XHCISlot;

typedef struct XHCIEvent {
    TRBType type;
    TRBCCode ccode;
    uint64_t ptr;
    uint32_t length;
    uint32_t flags;
    uint8_t slotid;
    uint8_t epid;
} XHCIEvent;

struct XHCIState {
    PCIDevice pci_dev;
    USBBus bus;
    qemu_irq irq;
    MemoryRegion mem;
    const char *name;
    uint32_t msi;
    unsigned int devaddr;

    /* Operational Registers */
    uint32_t usbcmd;
    uint32_t usbsts;
    uint32_t dnctrl;
    uint32_t crcr_low;
    uint32_t crcr_high;
    uint32_t dcbaap_low;
    uint32_t dcbaap_high;
    uint32_t config;

    XHCIPort ports[MAXPORTS];
    XHCISlot slots[MAXSLOTS];

    /* Runtime Registers */
    uint32_t mfindex;
    /* note: we only support one interrupter */
    uint32_t iman;
    uint32_t imod;
    uint32_t erstsz;
    uint32_t erstba_low;
    uint32_t erstba_high;
    uint32_t erdp_low;
    uint32_t erdp_high;

    target_phys_addr_t er_start;
    uint32_t er_size;
    bool er_pcs;
    unsigned int er_ep_idx;
    bool er_full;

    XHCIEvent ev_buffer[EV_QUEUE];
    unsigned int ev_buffer_put;
    unsigned int ev_buffer_get;

    XHCIRing cmd_ring;
};

typedef struct XHCIEvRingSeg {
    uint32_t addr_low;
    uint32_t addr_high;
    uint32_t size;
    uint32_t rsvd;
} XHCIEvRingSeg;

static void xhci_kick_ep(XHCIState *xhci, unsigned int slotid,
                         unsigned int epid);

static inline target_phys_addr_t xhci_addr64(uint32_t low, uint32_t high)
{
#if TARGET_PHYS_ADDR_BITS > 32
    return low | ((target_phys_addr_t)high << 32);
#else
    return low;
#endif
}

static inline target_phys_addr_t xhci_mask64(uint64_t addr)
{
#if TARGET_PHYS_ADDR_BITS > 32
    return addr;
#else
    return addr & 0xffffffff;
#endif
}

static void xhci_irq_update(XHCIState *xhci)
{
    int level = 0;

    if (xhci->iman & IMAN_IP && xhci->iman & IMAN_IE &&
        xhci->usbcmd && USBCMD_INTE) {
        level = 1;
    }

    DPRINTF("xhci_irq_update(): %d\n", level);

    if (xhci->msi && msi_enabled(&xhci->pci_dev)) {
        if (level) {
            DPRINTF("xhci_irq_update(): MSI signal\n");
            msi_notify(&xhci->pci_dev, 0);
        }
    } else {
        qemu_set_irq(xhci->irq, level);
    }
}

static inline int xhci_running(XHCIState *xhci)
{
    return !(xhci->usbsts & USBSTS_HCH) && !xhci->er_full;
}

static void xhci_die(XHCIState *xhci)
{
    xhci->usbsts |= USBSTS_HCE;
    fprintf(stderr, "xhci: asserted controller error\n");
}

static void xhci_write_event(XHCIState *xhci, XHCIEvent *event)
{
    XHCITRB ev_trb;
    target_phys_addr_t addr;

    ev_trb.parameter = cpu_to_le64(event->ptr);
    ev_trb.status = cpu_to_le32(event->length | (event->ccode << 24));
    ev_trb.control = (event->slotid << 24) | (event->epid << 16) |
                     event->flags | (event->type << TRB_TYPE_SHIFT);
    if (xhci->er_pcs) {
        ev_trb.control |= TRB_C;
    }
    ev_trb.control = cpu_to_le32(ev_trb.control);

    DPRINTF("xhci_write_event(): [%d] %016"PRIx64" %08x %08x\n",
            xhci->er_ep_idx, ev_trb.parameter, ev_trb.status, ev_trb.control);

    addr = xhci->er_start + TRB_SIZE*xhci->er_ep_idx;
    cpu_physical_memory_write(addr, (uint8_t *) &ev_trb, TRB_SIZE);

    xhci->er_ep_idx++;
    if (xhci->er_ep_idx >= xhci->er_size) {
        xhci->er_ep_idx = 0;
        xhci->er_pcs = !xhci->er_pcs;
    }
}

static void xhci_events_update(XHCIState *xhci)
{
    target_phys_addr_t erdp;
    unsigned int dp_idx;
    bool do_irq = 0;

    if (xhci->usbsts & USBSTS_HCH) {
        return;
    }

    erdp = xhci_addr64(xhci->erdp_low, xhci->erdp_high);
    if (erdp < xhci->er_start ||
        erdp >= (xhci->er_start + TRB_SIZE*xhci->er_size)) {
        fprintf(stderr, "xhci: ERDP out of bounds: "TARGET_FMT_plx"\n", erdp);
        fprintf(stderr, "xhci: ER at "TARGET_FMT_plx" len %d\n",
                xhci->er_start, xhci->er_size);
        xhci_die(xhci);
        return;
    }
    dp_idx = (erdp - xhci->er_start) / TRB_SIZE;
    assert(dp_idx < xhci->er_size);

    /* NEC didn't read section 4.9.4 of the spec (v1.0 p139 top Note) and thus
     * deadlocks when the ER is full. Hack it by holding off events until
     * the driver decides to free at least half of the ring */
    if (xhci->er_full) {
        int er_free = dp_idx - xhci->er_ep_idx;
        if (er_free <= 0) {
            er_free += xhci->er_size;
        }
        if (er_free < (xhci->er_size/2)) {
            DPRINTF("xhci_events_update(): event ring still "
                    "more than half full (hack)\n");
            return;
        }
    }

    while (xhci->ev_buffer_put != xhci->ev_buffer_get) {
        assert(xhci->er_full);
        if (((xhci->er_ep_idx+1) % xhci->er_size) == dp_idx) {
            DPRINTF("xhci_events_update(): event ring full again\n");
#ifndef ER_FULL_HACK
            XHCIEvent full = {ER_HOST_CONTROLLER, CC_EVENT_RING_FULL_ERROR};
            xhci_write_event(xhci, &full);
#endif
            do_irq = 1;
            break;
        }
        XHCIEvent *event = &xhci->ev_buffer[xhci->ev_buffer_get];
        xhci_write_event(xhci, event);
        xhci->ev_buffer_get++;
        do_irq = 1;
        if (xhci->ev_buffer_get == EV_QUEUE) {
            xhci->ev_buffer_get = 0;
        }
    }

    if (do_irq) {
        xhci->erdp_low |= ERDP_EHB;
        xhci->iman |= IMAN_IP;
        xhci->usbsts |= USBSTS_EINT;
        xhci_irq_update(xhci);
    }

    if (xhci->er_full && xhci->ev_buffer_put == xhci->ev_buffer_get) {
        DPRINTF("xhci_events_update(): event ring no longer full\n");
        xhci->er_full = 0;
    }
    return;
}

static void xhci_event(XHCIState *xhci, XHCIEvent *event)
{
    target_phys_addr_t erdp;
    unsigned int dp_idx;

    if (xhci->er_full) {
        DPRINTF("xhci_event(): ER full, queueing\n");
        if (((xhci->ev_buffer_put+1) % EV_QUEUE) == xhci->ev_buffer_get) {
            fprintf(stderr, "xhci: event queue full, dropping event!\n");
            return;
        }
        xhci->ev_buffer[xhci->ev_buffer_put++] = *event;
        if (xhci->ev_buffer_put == EV_QUEUE) {
            xhci->ev_buffer_put = 0;
        }
        return;
    }

    erdp = xhci_addr64(xhci->erdp_low, xhci->erdp_high);
    if (erdp < xhci->er_start ||
        erdp >= (xhci->er_start + TRB_SIZE*xhci->er_size)) {
        fprintf(stderr, "xhci: ERDP out of bounds: "TARGET_FMT_plx"\n", erdp);
        fprintf(stderr, "xhci: ER at "TARGET_FMT_plx" len %d\n",
                xhci->er_start, xhci->er_size);
        xhci_die(xhci);
        return;
    }

    dp_idx = (erdp - xhci->er_start) / TRB_SIZE;
    assert(dp_idx < xhci->er_size);

    if ((xhci->er_ep_idx+1) % xhci->er_size == dp_idx) {
        DPRINTF("xhci_event(): ER full, queueing\n");
#ifndef ER_FULL_HACK
        XHCIEvent full = {ER_HOST_CONTROLLER, CC_EVENT_RING_FULL_ERROR};
        xhci_write_event(xhci, &full);
#endif
        xhci->er_full = 1;
        if (((xhci->ev_buffer_put+1) % EV_QUEUE) == xhci->ev_buffer_get) {
            fprintf(stderr, "xhci: event queue full, dropping event!\n");
            return;
        }
        xhci->ev_buffer[xhci->ev_buffer_put++] = *event;
        if (xhci->ev_buffer_put == EV_QUEUE) {
            xhci->ev_buffer_put = 0;
        }
    } else {
        xhci_write_event(xhci, event);
    }

    xhci->erdp_low |= ERDP_EHB;
    xhci->iman |= IMAN_IP;
    xhci->usbsts |= USBSTS_EINT;

    xhci_irq_update(xhci);
}

static void xhci_ring_init(XHCIState *xhci, XHCIRing *ring,
                           target_phys_addr_t base)
{
    ring->base = base;
    ring->dequeue = base;
    ring->ccs = 1;
}

static TRBType xhci_ring_fetch(XHCIState *xhci, XHCIRing *ring, XHCITRB *trb,
                               target_phys_addr_t *addr)
{
    while (1) {
        TRBType type;
        cpu_physical_memory_read(ring->dequeue, (uint8_t *) trb, TRB_SIZE);
        trb->addr = ring->dequeue;
        trb->ccs = ring->ccs;
        le64_to_cpus(&trb->parameter);
        le32_to_cpus(&trb->status);
        le32_to_cpus(&trb->control);

        DPRINTF("xhci: TRB fetched [" TARGET_FMT_plx "]: "
                "%016" PRIx64 " %08x %08x\n",
                ring->dequeue, trb->parameter, trb->status, trb->control);

        if ((trb->control & TRB_C) != ring->ccs) {
            return 0;
        }

        type = TRB_TYPE(*trb);

        if (type != TR_LINK) {
            if (addr) {
                *addr = ring->dequeue;
            }
            ring->dequeue += TRB_SIZE;
            return type;
        } else {
            ring->dequeue = xhci_mask64(trb->parameter);
            if (trb->control & TRB_LK_TC) {
                ring->ccs = !ring->ccs;
            }
        }
    }
}

static int xhci_ring_chain_length(XHCIState *xhci, const XHCIRing *ring)
{
    XHCITRB trb;
    int length = 0;
    target_phys_addr_t dequeue = ring->dequeue;
    bool ccs = ring->ccs;
    /* hack to bundle together the two/three TDs that make a setup transfer */
    bool control_td_set = 0;

    while (1) {
        TRBType type;
        cpu_physical_memory_read(dequeue, (uint8_t *) &trb, TRB_SIZE);
        le64_to_cpus(&trb.parameter);
        le32_to_cpus(&trb.status);
        le32_to_cpus(&trb.control);

        DPRINTF("xhci: TRB peeked [" TARGET_FMT_plx "]: "
                "%016" PRIx64 " %08x %08x\n",
                dequeue, trb.parameter, trb.status, trb.control);

        if ((trb.control & TRB_C) != ccs) {
            return -length;
        }

        type = TRB_TYPE(trb);

        if (type == TR_LINK) {
            dequeue = xhci_mask64(trb.parameter);
            if (trb.control & TRB_LK_TC) {
                ccs = !ccs;
            }
            continue;
        }

        length += 1;
        dequeue += TRB_SIZE;

        if (type == TR_SETUP) {
            control_td_set = 1;
        } else if (type == TR_STATUS) {
            control_td_set = 0;
        }

        if (!control_td_set && !(trb.control & TRB_TR_CH)) {
            return length;
        }
    }
}

static void xhci_er_reset(XHCIState *xhci)
{
    XHCIEvRingSeg seg;

    /* cache the (sole) event ring segment location */
    if (xhci->erstsz != 1) {
        fprintf(stderr, "xhci: invalid value for ERSTSZ: %d\n", xhci->erstsz);
        xhci_die(xhci);
        return;
    }
    target_phys_addr_t erstba = xhci_addr64(xhci->erstba_low, xhci->erstba_high);
    cpu_physical_memory_read(erstba, (uint8_t *) &seg, sizeof(seg));
    le32_to_cpus(&seg.addr_low);
    le32_to_cpus(&seg.addr_high);
    le32_to_cpus(&seg.size);
    if (seg.size < 16 || seg.size > 4096) {
        fprintf(stderr, "xhci: invalid value for segment size: %d\n", seg.size);
        xhci_die(xhci);
        return;
    }
    xhci->er_start = xhci_addr64(seg.addr_low, seg.addr_high);
    xhci->er_size = seg.size;

    xhci->er_ep_idx = 0;
    xhci->er_pcs = 1;
    xhci->er_full = 0;

    DPRINTF("xhci: event ring:" TARGET_FMT_plx " [%d]\n",
            xhci->er_start, xhci->er_size);
}

static void xhci_run(XHCIState *xhci)
{
    DPRINTF("xhci_run()\n");

    xhci->usbsts &= ~USBSTS_HCH;
}

static void xhci_stop(XHCIState *xhci)
{
    DPRINTF("xhci_stop()\n");
    xhci->usbsts |= USBSTS_HCH;
    xhci->crcr_low &= ~CRCR_CRR;
}

static void xhci_set_ep_state(XHCIState *xhci, XHCIEPContext *epctx,
                              uint32_t state)
{
    uint32_t ctx[5];
    if (epctx->state == state) {
        return;
    }

    cpu_physical_memory_read(epctx->pctx, (uint8_t *) ctx, sizeof(ctx));
    ctx[0] &= ~EP_STATE_MASK;
    ctx[0] |= state;
    ctx[2] = epctx->ring.dequeue | epctx->ring.ccs;
    ctx[3] = (epctx->ring.dequeue >> 16) >> 16;
    DPRINTF("xhci: set epctx: " TARGET_FMT_plx " state=%d dequeue=%08x%08x\n",
            epctx->pctx, state, ctx[3], ctx[2]);
    cpu_physical_memory_write(epctx->pctx, (uint8_t *) ctx, sizeof(ctx));
    epctx->state = state;
}

static TRBCCode xhci_enable_ep(XHCIState *xhci, unsigned int slotid,
                               unsigned int epid, target_phys_addr_t pctx,
                               uint32_t *ctx)
{
    XHCISlot *slot;
    XHCIEPContext *epctx;
    target_phys_addr_t dequeue;
    int i;

    assert(slotid >= 1 && slotid <= MAXSLOTS);
    assert(epid >= 1 && epid <= 31);

    DPRINTF("xhci_enable_ep(%d, %d)\n", slotid, epid);

    slot = &xhci->slots[slotid-1];
    if (slot->eps[epid-1]) {
        fprintf(stderr, "xhci: slot %d ep %d already enabled!\n", slotid, epid);
        return CC_TRB_ERROR;
    }

    epctx = g_malloc(sizeof(XHCIEPContext));
    memset(epctx, 0, sizeof(XHCIEPContext));

    slot->eps[epid-1] = epctx;

    dequeue = xhci_addr64(ctx[2] & ~0xf, ctx[3]);
    xhci_ring_init(xhci, &epctx->ring, dequeue);
    epctx->ring.ccs = ctx[2] & 1;

    epctx->type = (ctx[1] >> EP_TYPE_SHIFT) & EP_TYPE_MASK;
    DPRINTF("xhci: endpoint %d.%d type is %d\n", epid/2, epid%2, epctx->type);
    epctx->pctx = pctx;
    epctx->max_psize = ctx[1]>>16;
    epctx->max_psize *= 1+((ctx[1]>>8)&0xff);
    epctx->has_bg = false;
    if (epctx->type == ET_ISO_IN) {
        epctx->has_bg = true;
    }
    DPRINTF("xhci: endpoint %d.%d max transaction (burst) size is %d\n",
            epid/2, epid%2, epctx->max_psize);
    for (i = 0; i < ARRAY_SIZE(epctx->transfers); i++) {
        usb_packet_init(&epctx->transfers[i].packet);
    }

    epctx->state = EP_RUNNING;
    ctx[0] &= ~EP_STATE_MASK;
    ctx[0] |= EP_RUNNING;

    return CC_SUCCESS;
}

static int xhci_ep_nuke_xfers(XHCIState *xhci, unsigned int slotid,
                               unsigned int epid)
{
    XHCISlot *slot;
    XHCIEPContext *epctx;
    int i, xferi, killed = 0;
    assert(slotid >= 1 && slotid <= MAXSLOTS);
    assert(epid >= 1 && epid <= 31);

    DPRINTF("xhci_ep_nuke_xfers(%d, %d)\n", slotid, epid);

    slot = &xhci->slots[slotid-1];

    if (!slot->eps[epid-1]) {
        return 0;
    }

    epctx = slot->eps[epid-1];

    xferi = epctx->next_xfer;
    for (i = 0; i < TD_QUEUE; i++) {
        XHCITransfer *t = &epctx->transfers[xferi];
        if (t->running) {
            t->cancelled = 1;
            /* libusb_cancel_transfer(t->usbxfer) */
            DPRINTF("xhci: cancelling transfer %d, waiting for it to complete...\n", i);
            killed++;
        }
        if (t->backgrounded) {
            t->backgrounded = 0;
        }
        if (t->trbs) {
            g_free(t->trbs);
        }
        if (t->data) {
            g_free(t->data);
        }

        t->trbs = NULL;
        t->data = NULL;
        t->trb_count = t->trb_alloced = 0;
        t->data_length = t->data_alloced = 0;
        xferi = (xferi + 1) % TD_QUEUE;
    }
    if (epctx->has_bg) {
        xferi = epctx->next_bg;
        for (i = 0; i < BG_XFERS; i++) {
            XHCITransfer *t = &epctx->bg_transfers[xferi];
            if (t->running) {
                t->cancelled = 1;
                /* libusb_cancel_transfer(t->usbxfer); */
                DPRINTF("xhci: cancelling bg transfer %d, waiting for it to complete...\n", i);
                killed++;
            }
            if (t->data) {
                g_free(t->data);
            }

            t->data = NULL;
            xferi = (xferi + 1) % BG_XFERS;
        }
    }
    return killed;
}

static TRBCCode xhci_disable_ep(XHCIState *xhci, unsigned int slotid,
                               unsigned int epid)
{
    XHCISlot *slot;
    XHCIEPContext *epctx;

    assert(slotid >= 1 && slotid <= MAXSLOTS);
    assert(epid >= 1 && epid <= 31);

    DPRINTF("xhci_disable_ep(%d, %d)\n", slotid, epid);

    slot = &xhci->slots[slotid-1];

    if (!slot->eps[epid-1]) {
        DPRINTF("xhci: slot %d ep %d already disabled\n", slotid, epid);
        return CC_SUCCESS;
    }

    xhci_ep_nuke_xfers(xhci, slotid, epid);

    epctx = slot->eps[epid-1];

    xhci_set_ep_state(xhci, epctx, EP_DISABLED);

    g_free(epctx);
    slot->eps[epid-1] = NULL;

    return CC_SUCCESS;
}

static TRBCCode xhci_stop_ep(XHCIState *xhci, unsigned int slotid,
                             unsigned int epid)
{
    XHCISlot *slot;
    XHCIEPContext *epctx;

    DPRINTF("xhci_stop_ep(%d, %d)\n", slotid, epid);

    assert(slotid >= 1 && slotid <= MAXSLOTS);

    if (epid < 1 || epid > 31) {
        fprintf(stderr, "xhci: bad ep %d\n", epid);
        return CC_TRB_ERROR;
    }

    slot = &xhci->slots[slotid-1];

    if (!slot->eps[epid-1]) {
        DPRINTF("xhci: slot %d ep %d not enabled\n", slotid, epid);
        return CC_EP_NOT_ENABLED_ERROR;
    }

    if (xhci_ep_nuke_xfers(xhci, slotid, epid) > 0) {
        fprintf(stderr, "xhci: FIXME: endpoint stopped w/ xfers running, "
                "data might be lost\n");
    }

    epctx = slot->eps[epid-1];

    xhci_set_ep_state(xhci, epctx, EP_STOPPED);

    return CC_SUCCESS;
}

static TRBCCode xhci_reset_ep(XHCIState *xhci, unsigned int slotid,
                              unsigned int epid)
{
    XHCISlot *slot;
    XHCIEPContext *epctx;
    USBDevice *dev;

    assert(slotid >= 1 && slotid <= MAXSLOTS);

    DPRINTF("xhci_reset_ep(%d, %d)\n", slotid, epid);

    if (epid < 1 || epid > 31) {
        fprintf(stderr, "xhci: bad ep %d\n", epid);
        return CC_TRB_ERROR;
    }

    slot = &xhci->slots[slotid-1];

    if (!slot->eps[epid-1]) {
        DPRINTF("xhci: slot %d ep %d not enabled\n", slotid, epid);
        return CC_EP_NOT_ENABLED_ERROR;
    }

    epctx = slot->eps[epid-1];

    if (epctx->state != EP_HALTED) {
        fprintf(stderr, "xhci: reset EP while EP %d not halted (%d)\n",
                epid, epctx->state);
        return CC_CONTEXT_STATE_ERROR;
    }

    if (xhci_ep_nuke_xfers(xhci, slotid, epid) > 0) {
        fprintf(stderr, "xhci: FIXME: endpoint reset w/ xfers running, "
                "data might be lost\n");
    }

    uint8_t ep = epid>>1;

    if (epid & 1) {
        ep |= 0x80;
    }

    dev = xhci->ports[xhci->slots[slotid-1].port-1].port.dev;
    if (!dev) {
        return CC_USB_TRANSACTION_ERROR;
    }

    xhci_set_ep_state(xhci, epctx, EP_STOPPED);

    return CC_SUCCESS;
}

static TRBCCode xhci_set_ep_dequeue(XHCIState *xhci, unsigned int slotid,
                                    unsigned int epid, uint64_t pdequeue)
{
    XHCISlot *slot;
    XHCIEPContext *epctx;
    target_phys_addr_t dequeue;

    assert(slotid >= 1 && slotid <= MAXSLOTS);

    if (epid < 1 || epid > 31) {
        fprintf(stderr, "xhci: bad ep %d\n", epid);
        return CC_TRB_ERROR;
    }

    DPRINTF("xhci_set_ep_dequeue(%d, %d, %016"PRIx64")\n", slotid, epid, pdequeue);
    dequeue = xhci_mask64(pdequeue);

    slot = &xhci->slots[slotid-1];

    if (!slot->eps[epid-1]) {
        DPRINTF("xhci: slot %d ep %d not enabled\n", slotid, epid);
        return CC_EP_NOT_ENABLED_ERROR;
    }

    epctx = slot->eps[epid-1];


    if (epctx->state != EP_STOPPED) {
        fprintf(stderr, "xhci: set EP dequeue pointer while EP %d not stopped\n", epid);
        return CC_CONTEXT_STATE_ERROR;
    }

    xhci_ring_init(xhci, &epctx->ring, dequeue & ~0xF);
    epctx->ring.ccs = dequeue & 1;

    xhci_set_ep_state(xhci, epctx, EP_STOPPED);

    return CC_SUCCESS;
}

static int xhci_xfer_data(XHCITransfer *xfer, uint8_t *data,
                          unsigned int length, bool in_xfer, bool out_xfer,
                          bool report)
{
    int i;
    uint32_t edtla = 0;
    unsigned int transferred = 0;
    unsigned int left = length;
    bool reported = 0;
    bool shortpkt = 0;
    XHCIEvent event = {ER_TRANSFER, CC_SUCCESS};
    XHCIState *xhci = xfer->xhci;

    DPRINTF("xhci_xfer_data(len=%d, in_xfer=%d, out_xfer=%d, report=%d)\n",
            length, in_xfer, out_xfer, report);

    assert(!(in_xfer && out_xfer));

    for (i = 0; i < xfer->trb_count; i++) {
        XHCITRB *trb = &xfer->trbs[i];
        target_phys_addr_t addr;
        unsigned int chunk = 0;

        switch (TRB_TYPE(*trb)) {
        case TR_DATA:
            if ((!(trb->control & TRB_TR_DIR)) != (!in_xfer)) {
                fprintf(stderr, "xhci: data direction mismatch for TR_DATA\n");
                xhci_die(xhci);
                return transferred;
            }
            /* fallthrough */
        case TR_NORMAL:
        case TR_ISOCH:
            addr = xhci_mask64(trb->parameter);
            chunk = trb->status & 0x1ffff;
            if (chunk > left) {
                chunk = left;
                shortpkt = 1;
            }
            if (in_xfer || out_xfer) {
                if (trb->control & TRB_TR_IDT) {
                    uint64_t idata;
                    if (chunk > 8 || in_xfer) {
                        fprintf(stderr, "xhci: invalid immediate data TRB\n");
                        xhci_die(xhci);
                        return transferred;
                    }
                    idata = le64_to_cpu(trb->parameter);
                    memcpy(data, &idata, chunk);
                } else {
                    DPRINTF("xhci_xfer_data: r/w(%d) %d bytes at "
                            TARGET_FMT_plx "\n", in_xfer, chunk, addr);
                    if (in_xfer) {
                        cpu_physical_memory_write(addr, data, chunk);
                    } else {
                        cpu_physical_memory_read(addr, data, chunk);
                    }
#ifdef DEBUG_DATA
                    unsigned int count = chunk;
                    int i;
                    if (count > 16) {
                        count = 16;
                    }
                    DPRINTF(" ::");
                    for (i = 0; i < count; i++) {
                        DPRINTF(" %02x", data[i]);
                    }
                    DPRINTF("\n");
#endif
                }
            }
            left -= chunk;
            data += chunk;
            edtla += chunk;
            transferred += chunk;
            break;
        case TR_STATUS:
            reported = 0;
            shortpkt = 0;
            break;
        }

        if (report && !reported && (trb->control & TRB_TR_IOC ||
            (shortpkt && (trb->control & TRB_TR_ISP)))) {
            event.slotid = xfer->slotid;
            event.epid = xfer->epid;
            event.length = (trb->status & 0x1ffff) - chunk;
            event.flags = 0;
            event.ptr = trb->addr;
            if (xfer->status == CC_SUCCESS) {
                event.ccode = shortpkt ? CC_SHORT_PACKET : CC_SUCCESS;
            } else {
                event.ccode = xfer->status;
            }
            if (TRB_TYPE(*trb) == TR_EVDATA) {
                event.ptr = trb->parameter;
                event.flags |= TRB_EV_ED;
                event.length = edtla & 0xffffff;
                DPRINTF("xhci_xfer_data: EDTLA=%d\n", event.length);
                edtla = 0;
            }
            xhci_event(xhci, &event);
            reported = 1;
        }
    }
    return transferred;
}

static void xhci_stall_ep(XHCITransfer *xfer)
{
    XHCIState *xhci = xfer->xhci;
    XHCISlot *slot = &xhci->slots[xfer->slotid-1];
    XHCIEPContext *epctx = slot->eps[xfer->epid-1];

    epctx->ring.dequeue = xfer->trbs[0].addr;
    epctx->ring.ccs = xfer->trbs[0].ccs;
    xhci_set_ep_state(xhci, epctx, EP_HALTED);
    DPRINTF("xhci: stalled slot %d ep %d\n", xfer->slotid, xfer->epid);
    DPRINTF("xhci: will continue at "TARGET_FMT_plx"\n", epctx->ring.dequeue);
}

static int xhci_submit(XHCIState *xhci, XHCITransfer *xfer,
                       XHCIEPContext *epctx);

static void xhci_bg_update(XHCIState *xhci, XHCIEPContext *epctx)
{
    if (epctx->bg_updating) {
        return;
    }
    DPRINTF("xhci_bg_update(%p, %p)\n", xhci, epctx);
    assert(epctx->has_bg);
    DPRINTF("xhci: fg=%d bg=%d\n", epctx->comp_xfer, epctx->next_bg);
    epctx->bg_updating = 1;
    while (epctx->transfers[epctx->comp_xfer].backgrounded &&
           epctx->bg_transfers[epctx->next_bg].complete) {
        XHCITransfer *fg = &epctx->transfers[epctx->comp_xfer];
        XHCITransfer *bg = &epctx->bg_transfers[epctx->next_bg];
#if 0
        DPRINTF("xhci: completing fg %d from bg %d.%d (stat: %d)\n",
                epctx->comp_xfer, epctx->next_bg, bg->cur_pkt,
                bg->usbxfer->iso_packet_desc[bg->cur_pkt].status
               );
#endif
        assert(epctx->type == ET_ISO_IN);
        assert(bg->iso_xfer);
        assert(bg->in_xfer);
        uint8_t *p = bg->data + bg->cur_pkt * bg->pktsize;
#if 0
        int len = bg->usbxfer->iso_packet_desc[bg->cur_pkt].actual_length;
        fg->status = libusb_to_ccode(bg->usbxfer->iso_packet_desc[bg->cur_pkt].status);
#else
        int len = 0;
        FIXME();
#endif
        fg->complete = 1;
        fg->backgrounded = 0;

        if (fg->status == CC_STALL_ERROR) {
            xhci_stall_ep(fg);
        }

        xhci_xfer_data(fg, p, len, 1, 0, 1);

        epctx->comp_xfer++;
        if (epctx->comp_xfer == TD_QUEUE) {
            epctx->comp_xfer = 0;
        }
        DPRINTF("next fg xfer: %d\n", epctx->comp_xfer);
        bg->cur_pkt++;
        if (bg->cur_pkt == bg->pkts) {
            bg->complete = 0;
            if (xhci_submit(xhci, bg, epctx) < 0) {
                fprintf(stderr, "xhci: bg resubmit failed\n");
            }
            epctx->next_bg++;
            if (epctx->next_bg == BG_XFERS) {
                epctx->next_bg = 0;
            }
            DPRINTF("next bg xfer: %d\n", epctx->next_bg);

        xhci_kick_ep(xhci, fg->slotid, fg->epid);
        }
    }
    epctx->bg_updating = 0;
}

#if 0
static void xhci_xfer_cb(struct libusb_transfer *transfer)
{
    XHCIState *xhci;
    XHCITransfer *xfer;

    xfer = (XHCITransfer *)transfer->user_data;
    xhci = xfer->xhci;

    DPRINTF("xhci_xfer_cb(slot=%d, ep=%d, status=%d)\n", xfer->slotid,
            xfer->epid, transfer->status);

    assert(xfer->slotid >= 1 && xfer->slotid <= MAXSLOTS);
    assert(xfer->epid >= 1 && xfer->epid <= 31);

    if (xfer->cancelled) {
        DPRINTF("xhci: transfer cancelled, not reporting anything\n");
        xfer->running = 0;
        return;
    }

    XHCIEPContext *epctx;
    XHCISlot *slot;
    slot = &xhci->slots[xfer->slotid-1];
    assert(slot->eps[xfer->epid-1]);
    epctx = slot->eps[xfer->epid-1];

    if (xfer->bg_xfer) {
        DPRINTF("xhci: background transfer, updating\n");
        xfer->complete = 1;
        xfer->running = 0;
        xhci_bg_update(xhci, epctx);
        return;
    }

    if (xfer->iso_xfer) {
        transfer->status = transfer->iso_packet_desc[0].status;
        transfer->actual_length = transfer->iso_packet_desc[0].actual_length;
    }

    xfer->status = libusb_to_ccode(transfer->status);

    xfer->complete = 1;
    xfer->running = 0;

    if (transfer->status == LIBUSB_TRANSFER_STALL)
        xhci_stall_ep(xhci, epctx, xfer);

    DPRINTF("xhci: transfer actual length = %d\n", transfer->actual_length);

    if (xfer->in_xfer) {
        if (xfer->epid == 1) {
            xhci_xfer_data(xhci, xfer, xfer->data + 8,
                           transfer->actual_length, 1, 0, 1);
        } else {
            xhci_xfer_data(xhci, xfer, xfer->data,
                           transfer->actual_length, 1, 0, 1);
        }
    } else {
        xhci_xfer_data(xhci, xfer, NULL, transfer->actual_length, 0, 0, 1);
    }

    xhci_kick_ep(xhci, xfer->slotid, xfer->epid);
}

static int xhci_hle_control(XHCIState *xhci, XHCITransfer *xfer,
                            uint8_t bmRequestType, uint8_t bRequest,
                            uint16_t wValue, uint16_t wIndex, uint16_t wLength)
{
    uint16_t type_req = (bmRequestType << 8) | bRequest;

    switch (type_req) {
        case 0x0000 | USB_REQ_SET_CONFIGURATION:
            DPRINTF("xhci: HLE switch configuration\n");
            return xhci_switch_config(xhci, xfer->slotid, wValue) == 0;
        case 0x0100 | USB_REQ_SET_INTERFACE:
            DPRINTF("xhci: HLE set interface altsetting\n");
            return xhci_set_iface_alt(xhci, xfer->slotid, wIndex, wValue) == 0;
        case 0x0200 | USB_REQ_CLEAR_FEATURE:
            if (wValue == 0) { // endpoint halt
                DPRINTF("xhci: HLE clear halt\n");
                return xhci_clear_halt(xhci, xfer->slotid, wIndex);
            }
        case 0x0000 | USB_REQ_SET_ADDRESS:
            fprintf(stderr, "xhci: warn: illegal SET_ADDRESS request\n");
            return 0;
        default:
            return 0;
    }
}
#endif

static int xhci_setup_packet(XHCITransfer *xfer, XHCIPort *port, int ep)
{
    usb_packet_setup(&xfer->packet,
                     xfer->in_xfer ? USB_TOKEN_IN : USB_TOKEN_OUT,
                     xfer->xhci->slots[xfer->slotid-1].devaddr,
                     ep & 0x7f);
    usb_packet_addbuf(&xfer->packet, xfer->data, xfer->data_length);
    DPRINTF("xhci: setup packet pid 0x%x addr %d ep %d\n",
            xfer->packet.pid, xfer->packet.devaddr, xfer->packet.devep);
    return 0;
}

static int xhci_complete_packet(XHCITransfer *xfer, int ret)
{
    if (ret == USB_RET_ASYNC) {
        xfer->running = 1;
        xfer->complete = 0;
        xfer->cancelled = 0;
        return 0;
    } else {
        xfer->running = 0;
        xfer->complete = 1;
    }

    if (ret >= 0) {
        xfer->status = CC_SUCCESS;
        xhci_xfer_data(xfer, xfer->data, ret, xfer->in_xfer, 0, 1);
        return 0;
    }

    /* error */
    switch (ret) {
    case USB_RET_NODEV:
        xfer->status = CC_USB_TRANSACTION_ERROR;
        xhci_xfer_data(xfer, xfer->data, 0, xfer->in_xfer, 0, 1);
        xhci_stall_ep(xfer);
        break;
    case USB_RET_STALL:
        xfer->status = CC_STALL_ERROR;
        xhci_xfer_data(xfer, xfer->data, 0, xfer->in_xfer, 0, 1);
        xhci_stall_ep(xfer);
        break;
    default:
        fprintf(stderr, "%s: FIXME: ret = %d\n", __FUNCTION__, ret);
        FIXME();
    }
    return 0;
}

static int xhci_fire_ctl_transfer(XHCIState *xhci, XHCITransfer *xfer)
{
    XHCITRB *trb_setup, *trb_status;
    uint8_t bmRequestType, bRequest;
    uint16_t wValue, wLength, wIndex;
    XHCIPort *port;
    USBDevice *dev;
    int ret;

    DPRINTF("xhci_fire_ctl_transfer(slot=%d)\n", xfer->slotid);

    trb_setup = &xfer->trbs[0];
    trb_status = &xfer->trbs[xfer->trb_count-1];

    /* at most one Event Data TRB allowed after STATUS */
    if (TRB_TYPE(*trb_status) == TR_EVDATA && xfer->trb_count > 2) {
        trb_status--;
    }

    /* do some sanity checks */
    if (TRB_TYPE(*trb_setup) != TR_SETUP) {
        fprintf(stderr, "xhci: ep0 first TD not SETUP: %d\n",
                TRB_TYPE(*trb_setup));
        return -1;
    }
    if (TRB_TYPE(*trb_status) != TR_STATUS) {
        fprintf(stderr, "xhci: ep0 last TD not STATUS: %d\n",
                TRB_TYPE(*trb_status));
        return -1;
    }
    if (!(trb_setup->control & TRB_TR_IDT)) {
        fprintf(stderr, "xhci: Setup TRB doesn't have IDT set\n");
        return -1;
    }
    if ((trb_setup->status & 0x1ffff) != 8) {
        fprintf(stderr, "xhci: Setup TRB has bad length (%d)\n",
                (trb_setup->status & 0x1ffff));
        return -1;
    }

    bmRequestType = trb_setup->parameter;
    bRequest = trb_setup->parameter >> 8;
    wValue = trb_setup->parameter >> 16;
    wIndex = trb_setup->parameter >> 32;
    wLength = trb_setup->parameter >> 48;

    if (xfer->data && xfer->data_alloced < wLength) {
        xfer->data_alloced = 0;
        g_free(xfer->data);
        xfer->data = NULL;
    }
    if (!xfer->data) {
        DPRINTF("xhci: alloc %d bytes data\n", wLength);
        xfer->data = g_malloc(wLength+1);
        xfer->data_alloced = wLength;
    }
    xfer->data_length = wLength;

    port = &xhci->ports[xhci->slots[xfer->slotid-1].port-1];
    dev = port->port.dev;
    if (!dev) {
        fprintf(stderr, "xhci: slot %d port %d has no device\n", xfer->slotid,
                xhci->slots[xfer->slotid-1].port);
        return -1;
    }

    xfer->in_xfer = bmRequestType & USB_DIR_IN;
    xfer->iso_xfer = false;

    xhci_setup_packet(xfer, port, 0);
    if (!xfer->in_xfer) {
        xhci_xfer_data(xfer, xfer->data, wLength, 0, 1, 0);
    }
    ret = usb_device_handle_control(dev, &xfer->packet,
                                    (bmRequestType << 8) | bRequest,
                                    wValue, wIndex, wLength, xfer->data);

    xhci_complete_packet(xfer, ret);
    if (!xfer->running) {
        xhci_kick_ep(xhci, xfer->slotid, xfer->epid);
    }
    return 0;
}

static int xhci_submit(XHCIState *xhci, XHCITransfer *xfer, XHCIEPContext *epctx)
{
    XHCIPort *port;
    USBDevice *dev;
    int ret;

    DPRINTF("xhci_submit(slotid=%d,epid=%d)\n", xfer->slotid, xfer->epid);
    uint8_t ep = xfer->epid>>1;

    xfer->in_xfer = epctx->type>>2;
    if (xfer->in_xfer) {
        ep |= 0x80;
    }

    if (xfer->data && xfer->data_alloced < xfer->data_length) {
        xfer->data_alloced = 0;
        g_free(xfer->data);
        xfer->data = NULL;
    }
    if (!xfer->data && xfer->data_length) {
        DPRINTF("xhci: alloc %d bytes data\n", xfer->data_length);
        xfer->data = g_malloc(xfer->data_length);
        xfer->data_alloced = xfer->data_length;
    }
    if (epctx->type == ET_ISO_IN || epctx->type == ET_ISO_OUT) {
        if (!xfer->bg_xfer) {
            xfer->pkts = 1;
        }
    } else {
        xfer->pkts = 0;
    }

    port = &xhci->ports[xhci->slots[xfer->slotid-1].port-1];
    dev = port->port.dev;
    if (!dev) {
        fprintf(stderr, "xhci: slot %d port %d has no device\n", xfer->slotid,
                xhci->slots[xfer->slotid-1].port);
        return -1;
    }

    xhci_setup_packet(xfer, port, ep);

    switch(epctx->type) {
    case ET_INTR_OUT:
    case ET_INTR_IN:
    case ET_BULK_OUT:
    case ET_BULK_IN:
        break;
    case ET_ISO_OUT:
    case ET_ISO_IN:
        FIXME();
        break;
    default:
        fprintf(stderr, "xhci: unknown or unhandled EP type %d (ep %02x)\n",
                epctx->type, ep);
        return -1;
    }

    if (!xfer->in_xfer) {
        xhci_xfer_data(xfer, xfer->data, xfer->data_length, 0, 1, 0);
    }
    ret = usb_handle_packet(dev, &xfer->packet);

    xhci_complete_packet(xfer, ret);
    if (!xfer->running) {
        xhci_kick_ep(xhci, xfer->slotid, xfer->epid);
    }
    return 0;
}

static int xhci_fire_transfer(XHCIState *xhci, XHCITransfer *xfer, XHCIEPContext *epctx)
{
    int i;
    unsigned int length = 0;
    XHCITRB *trb;

    DPRINTF("xhci_fire_transfer(slotid=%d,epid=%d)\n", xfer->slotid, xfer->epid);

    for (i = 0; i < xfer->trb_count; i++) {
        trb = &xfer->trbs[i];
        if (TRB_TYPE(*trb) == TR_NORMAL || TRB_TYPE(*trb) == TR_ISOCH) {
            length += trb->status & 0x1ffff;
        }
    }
    DPRINTF("xhci: total TD length=%d\n", length);

    if (!epctx->has_bg) {
        xfer->data_length = length;
        xfer->backgrounded = 0;
        return xhci_submit(xhci, xfer, epctx);
    } else {
        if (!epctx->bg_running) {
            for (i = 0; i < BG_XFERS; i++) {
                XHCITransfer *t = &epctx->bg_transfers[i];
                t->xhci = xhci;
                t->epid = xfer->epid;
                t->slotid = xfer->slotid;
                t->pkts = BG_PKTS;
                t->pktsize = epctx->max_psize;
                t->data_length = t->pkts * t->pktsize;
                t->bg_xfer = 1;
                if (xhci_submit(xhci, t, epctx) < 0) {
                    fprintf(stderr, "xhci: bg submit failed\n");
                    return -1;
                }
            }
            epctx->bg_running = 1;
        }
        xfer->backgrounded = 1;
        xhci_bg_update(xhci, epctx);
        return 0;
    }
}

static void xhci_kick_ep(XHCIState *xhci, unsigned int slotid, unsigned int epid)
{
    XHCIEPContext *epctx;
    int length;
    int i;

    assert(slotid >= 1 && slotid <= MAXSLOTS);
    assert(epid >= 1 && epid <= 31);
    DPRINTF("xhci_kick_ep(%d, %d)\n", slotid, epid);

    if (!xhci->slots[slotid-1].enabled) {
        fprintf(stderr, "xhci: xhci_kick_ep for disabled slot %d\n", slotid);
        return;
    }
    epctx = xhci->slots[slotid-1].eps[epid-1];
    if (!epctx) {
        fprintf(stderr, "xhci: xhci_kick_ep for disabled endpoint %d,%d\n",
                epid, slotid);
        return;
    }

    if (epctx->state == EP_HALTED) {
        DPRINTF("xhci: ep halted, not running schedule\n");
        return;
    }

    xhci_set_ep_state(xhci, epctx, EP_RUNNING);

    while (1) {
        XHCITransfer *xfer = &epctx->transfers[epctx->next_xfer];
        if (xfer->running || xfer->backgrounded) {
            DPRINTF("xhci: ep is busy\n");
            break;
        }
        length = xhci_ring_chain_length(xhci, &epctx->ring);
        if (length < 0) {
            DPRINTF("xhci: incomplete TD (%d TRBs)\n", -length);
            break;
        } else if (length == 0) {
            break;
        }
        DPRINTF("xhci: fetching %d-TRB TD\n", length);
        if (xfer->trbs && xfer->trb_alloced < length) {
            xfer->trb_count = 0;
            xfer->trb_alloced = 0;
            g_free(xfer->trbs);
            xfer->trbs = NULL;
        }
        if (!xfer->trbs) {
            xfer->trbs = g_malloc(sizeof(XHCITRB) * length);
            xfer->trb_alloced = length;
        }
        xfer->trb_count = length;

        for (i = 0; i < length; i++) {
            assert(xhci_ring_fetch(xhci, &epctx->ring, &xfer->trbs[i], NULL));
        }
        xfer->xhci = xhci;
        xfer->epid = epid;
        xfer->slotid = slotid;

        if (epid == 1) {
            if (xhci_fire_ctl_transfer(xhci, xfer) >= 0) {
                epctx->next_xfer = (epctx->next_xfer + 1) % TD_QUEUE;
            } else {
                fprintf(stderr, "xhci: error firing CTL transfer\n");
            }
        } else {
            if (xhci_fire_transfer(xhci, xfer, epctx) >= 0) {
                epctx->next_xfer = (epctx->next_xfer + 1) % TD_QUEUE;
            } else {
                fprintf(stderr, "xhci: error firing data transfer\n");
            }
        }

        /*
         * Qemu usb can't handle multiple in-flight xfers.
         * Also xfers might be finished here already,
         * possibly with an error.  Stop here for now.
         */
        break;
    }
}

static TRBCCode xhci_enable_slot(XHCIState *xhci, unsigned int slotid)
{
    assert(slotid >= 1 && slotid <= MAXSLOTS);
    DPRINTF("xhci_enable_slot(%d)\n", slotid);
    xhci->slots[slotid-1].enabled = 1;
    xhci->slots[slotid-1].port = 0;
    memset(xhci->slots[slotid-1].eps, 0, sizeof(XHCIEPContext*)*31);

    return CC_SUCCESS;
}

static TRBCCode xhci_disable_slot(XHCIState *xhci, unsigned int slotid)
{
    int i;

    assert(slotid >= 1 && slotid <= MAXSLOTS);
    DPRINTF("xhci_disable_slot(%d)\n", slotid);

    for (i = 1; i <= 31; i++) {
        if (xhci->slots[slotid-1].eps[i-1]) {
            xhci_disable_ep(xhci, slotid, i);
        }
    }

    xhci->slots[slotid-1].enabled = 0;
    return CC_SUCCESS;
}

static TRBCCode xhci_address_slot(XHCIState *xhci, unsigned int slotid,
                                  uint64_t pictx, bool bsr)
{
    XHCISlot *slot;
    USBDevice *dev;
    target_phys_addr_t ictx, octx, dcbaap;
    uint64_t poctx;
    uint32_t ictl_ctx[2];
    uint32_t slot_ctx[4];
    uint32_t ep0_ctx[5];
    unsigned int port;
    int i;
    TRBCCode res;

    assert(slotid >= 1 && slotid <= MAXSLOTS);
    DPRINTF("xhci_address_slot(%d)\n", slotid);

    dcbaap = xhci_addr64(xhci->dcbaap_low, xhci->dcbaap_high);
    cpu_physical_memory_read(dcbaap + 8*slotid,
                             (uint8_t *) &poctx, sizeof(poctx));
    ictx = xhci_mask64(pictx);
    octx = xhci_mask64(le64_to_cpu(poctx));

    DPRINTF("xhci: input context at "TARGET_FMT_plx"\n", ictx);
    DPRINTF("xhci: output context at "TARGET_FMT_plx"\n", octx);

    cpu_physical_memory_read(ictx, (uint8_t *) ictl_ctx, sizeof(ictl_ctx));

    if (ictl_ctx[0] != 0x0 || ictl_ctx[1] != 0x3) {
        fprintf(stderr, "xhci: invalid input context control %08x %08x\n",
                ictl_ctx[0], ictl_ctx[1]);
        return CC_TRB_ERROR;
    }

    cpu_physical_memory_read(ictx+32, (uint8_t *) slot_ctx, sizeof(slot_ctx));
    cpu_physical_memory_read(ictx+64, (uint8_t *) ep0_ctx, sizeof(ep0_ctx));

    DPRINTF("xhci: input slot context: %08x %08x %08x %08x\n",
            slot_ctx[0], slot_ctx[1], slot_ctx[2], slot_ctx[3]);

    DPRINTF("xhci: input ep0 context: %08x %08x %08x %08x %08x\n",
            ep0_ctx[0], ep0_ctx[1], ep0_ctx[2], ep0_ctx[3], ep0_ctx[4]);

    port = (slot_ctx[1]>>16) & 0xFF;
    dev = xhci->ports[port-1].port.dev;

    if (port < 1 || port > MAXPORTS) {
        fprintf(stderr, "xhci: bad port %d\n", port);
        return CC_TRB_ERROR;
    } else if (!dev) {
        fprintf(stderr, "xhci: port %d not connected\n", port);
        return CC_USB_TRANSACTION_ERROR;
    }

    for (i = 0; i < MAXSLOTS; i++) {
        if (xhci->slots[i].port == port) {
            fprintf(stderr, "xhci: port %d already assigned to slot %d\n",
                    port, i+1);
            return CC_TRB_ERROR;
        }
    }

    slot = &xhci->slots[slotid-1];
    slot->port = port;
    slot->ctx = octx;

    if (bsr) {
        slot_ctx[3] = SLOT_DEFAULT << SLOT_STATE_SHIFT;
    } else {
        slot->devaddr = xhci->devaddr++;
        slot_ctx[3] = (SLOT_ADDRESSED << SLOT_STATE_SHIFT) | slot->devaddr;
        DPRINTF("xhci: device address is %d\n", slot->devaddr);
        usb_device_handle_control(dev, NULL,
                                  DeviceOutRequest | USB_REQ_SET_ADDRESS,
                                  slot->devaddr, 0, 0, NULL);
    }

    res = xhci_enable_ep(xhci, slotid, 1, octx+32, ep0_ctx);

    DPRINTF("xhci: output slot context: %08x %08x %08x %08x\n",
            slot_ctx[0], slot_ctx[1], slot_ctx[2], slot_ctx[3]);
    DPRINTF("xhci: output ep0 context: %08x %08x %08x %08x %08x\n",
            ep0_ctx[0], ep0_ctx[1], ep0_ctx[2], ep0_ctx[3], ep0_ctx[4]);

    cpu_physical_memory_write(octx, (uint8_t *) slot_ctx, sizeof(slot_ctx));
    cpu_physical_memory_write(octx+32, (uint8_t *) ep0_ctx, sizeof(ep0_ctx));

    return res;
}


static TRBCCode xhci_configure_slot(XHCIState *xhci, unsigned int slotid,
                                  uint64_t pictx, bool dc)
{
    target_phys_addr_t ictx, octx;
    uint32_t ictl_ctx[2];
    uint32_t slot_ctx[4];
    uint32_t islot_ctx[4];
    uint32_t ep_ctx[5];
    int i;
    TRBCCode res;

    assert(slotid >= 1 && slotid <= MAXSLOTS);
    DPRINTF("xhci_configure_slot(%d)\n", slotid);

    ictx = xhci_mask64(pictx);
    octx = xhci->slots[slotid-1].ctx;

    DPRINTF("xhci: input context at "TARGET_FMT_plx"\n", ictx);
    DPRINTF("xhci: output context at "TARGET_FMT_plx"\n", octx);

    if (dc) {
        for (i = 2; i <= 31; i++) {
            if (xhci->slots[slotid-1].eps[i-1]) {
                xhci_disable_ep(xhci, slotid, i);
            }
        }

        cpu_physical_memory_read(octx, (uint8_t *) slot_ctx, sizeof(slot_ctx));
        slot_ctx[3] &= ~(SLOT_STATE_MASK << SLOT_STATE_SHIFT);
        slot_ctx[3] |= SLOT_ADDRESSED << SLOT_STATE_SHIFT;
        DPRINTF("xhci: output slot context: %08x %08x %08x %08x\n",
                slot_ctx[0], slot_ctx[1], slot_ctx[2], slot_ctx[3]);
        cpu_physical_memory_write(octx, (uint8_t *) slot_ctx, sizeof(slot_ctx));

        return CC_SUCCESS;
    }

    cpu_physical_memory_read(ictx, (uint8_t *) ictl_ctx, sizeof(ictl_ctx));

    if ((ictl_ctx[0] & 0x3) != 0x0 || (ictl_ctx[1] & 0x3) != 0x1) {
        fprintf(stderr, "xhci: invalid input context control %08x %08x\n",
                ictl_ctx[0], ictl_ctx[1]);
        return CC_TRB_ERROR;
    }

    cpu_physical_memory_read(ictx+32, (uint8_t *) islot_ctx, sizeof(islot_ctx));
    cpu_physical_memory_read(octx, (uint8_t *) slot_ctx, sizeof(slot_ctx));

    if (SLOT_STATE(slot_ctx[3]) < SLOT_ADDRESSED) {
        fprintf(stderr, "xhci: invalid slot state %08x\n", slot_ctx[3]);
        return CC_CONTEXT_STATE_ERROR;
    }

    for (i = 2; i <= 31; i++) {
        if (ictl_ctx[0] & (1<<i)) {
            xhci_disable_ep(xhci, slotid, i);
        }
        if (ictl_ctx[1] & (1<<i)) {
            cpu_physical_memory_read(ictx+32+(32*i),
                                     (uint8_t *) ep_ctx, sizeof(ep_ctx));
            DPRINTF("xhci: input ep%d.%d context: %08x %08x %08x %08x %08x\n",
                    i/2, i%2, ep_ctx[0], ep_ctx[1], ep_ctx[2],
                    ep_ctx[3], ep_ctx[4]);
            xhci_disable_ep(xhci, slotid, i);
            res = xhci_enable_ep(xhci, slotid, i, octx+(32*i), ep_ctx);
            if (res != CC_SUCCESS) {
                return res;
            }
            DPRINTF("xhci: output ep%d.%d context: %08x %08x %08x %08x %08x\n",
                    i/2, i%2, ep_ctx[0], ep_ctx[1], ep_ctx[2],
                    ep_ctx[3], ep_ctx[4]);
            cpu_physical_memory_write(octx+(32*i),
                                      (uint8_t *) ep_ctx, sizeof(ep_ctx));
        }
    }

    slot_ctx[3] &= ~(SLOT_STATE_MASK << SLOT_STATE_SHIFT);
    slot_ctx[3] |= SLOT_CONFIGURED << SLOT_STATE_SHIFT;
    slot_ctx[0] &= ~(SLOT_CONTEXT_ENTRIES_MASK << SLOT_CONTEXT_ENTRIES_SHIFT);
    slot_ctx[0] |= islot_ctx[0] & (SLOT_CONTEXT_ENTRIES_MASK <<
                                   SLOT_CONTEXT_ENTRIES_SHIFT);
    DPRINTF("xhci: output slot context: %08x %08x %08x %08x\n",
            slot_ctx[0], slot_ctx[1], slot_ctx[2], slot_ctx[3]);

    cpu_physical_memory_write(octx, (uint8_t *) slot_ctx, sizeof(slot_ctx));

    return CC_SUCCESS;
}


static TRBCCode xhci_evaluate_slot(XHCIState *xhci, unsigned int slotid,
                                   uint64_t pictx)
{
    target_phys_addr_t ictx, octx;
    uint32_t ictl_ctx[2];
    uint32_t iep0_ctx[5];
    uint32_t ep0_ctx[5];
    uint32_t islot_ctx[4];
    uint32_t slot_ctx[4];

    assert(slotid >= 1 && slotid <= MAXSLOTS);
    DPRINTF("xhci_evaluate_slot(%d)\n", slotid);

    ictx = xhci_mask64(pictx);
    octx = xhci->slots[slotid-1].ctx;

    DPRINTF("xhci: input context at "TARGET_FMT_plx"\n", ictx);
    DPRINTF("xhci: output context at "TARGET_FMT_plx"\n", octx);

    cpu_physical_memory_read(ictx, (uint8_t *) ictl_ctx, sizeof(ictl_ctx));

    if (ictl_ctx[0] != 0x0 || ictl_ctx[1] & ~0x3) {
        fprintf(stderr, "xhci: invalid input context control %08x %08x\n",
                ictl_ctx[0], ictl_ctx[1]);
        return CC_TRB_ERROR;
    }

    if (ictl_ctx[1] & 0x1) {
        cpu_physical_memory_read(ictx+32,
                                 (uint8_t *) islot_ctx, sizeof(islot_ctx));

        DPRINTF("xhci: input slot context: %08x %08x %08x %08x\n",
                islot_ctx[0], islot_ctx[1], islot_ctx[2], islot_ctx[3]);

        cpu_physical_memory_read(octx, (uint8_t *) slot_ctx, sizeof(slot_ctx));

        slot_ctx[1] &= ~0xFFFF; /* max exit latency */
        slot_ctx[1] |= islot_ctx[1] & 0xFFFF;
        slot_ctx[2] &= ~0xFF00000; /* interrupter target */
        slot_ctx[2] |= islot_ctx[2] & 0xFF000000;

        DPRINTF("xhci: output slot context: %08x %08x %08x %08x\n",
                slot_ctx[0], slot_ctx[1], slot_ctx[2], slot_ctx[3]);

        cpu_physical_memory_write(octx, (uint8_t *) slot_ctx, sizeof(slot_ctx));
    }

    if (ictl_ctx[1] & 0x2) {
        cpu_physical_memory_read(ictx+64,
                                 (uint8_t *) iep0_ctx, sizeof(iep0_ctx));

        DPRINTF("xhci: input ep0 context: %08x %08x %08x %08x %08x\n",
                iep0_ctx[0], iep0_ctx[1], iep0_ctx[2],
                iep0_ctx[3], iep0_ctx[4]);

        cpu_physical_memory_read(octx+32, (uint8_t *) ep0_ctx, sizeof(ep0_ctx));

        ep0_ctx[1] &= ~0xFFFF0000; /* max packet size*/
        ep0_ctx[1] |= iep0_ctx[1] & 0xFFFF0000;

        DPRINTF("xhci: output ep0 context: %08x %08x %08x %08x %08x\n",
                ep0_ctx[0], ep0_ctx[1], ep0_ctx[2], ep0_ctx[3], ep0_ctx[4]);

        cpu_physical_memory_write(octx+32,
                                  (uint8_t *) ep0_ctx, sizeof(ep0_ctx));
    }

    return CC_SUCCESS;
}

static TRBCCode xhci_reset_slot(XHCIState *xhci, unsigned int slotid)
{
    uint32_t slot_ctx[4];
    target_phys_addr_t octx;
    int i;

    assert(slotid >= 1 && slotid <= MAXSLOTS);
    DPRINTF("xhci_reset_slot(%d)\n", slotid);

    octx = xhci->slots[slotid-1].ctx;

    DPRINTF("xhci: output context at "TARGET_FMT_plx"\n", octx);

    for (i = 2; i <= 31; i++) {
        if (xhci->slots[slotid-1].eps[i-1]) {
            xhci_disable_ep(xhci, slotid, i);
        }
    }

    cpu_physical_memory_read(octx, (uint8_t *) slot_ctx, sizeof(slot_ctx));
    slot_ctx[3] &= ~(SLOT_STATE_MASK << SLOT_STATE_SHIFT);
    slot_ctx[3] |= SLOT_DEFAULT << SLOT_STATE_SHIFT;
    DPRINTF("xhci: output slot context: %08x %08x %08x %08x\n",
            slot_ctx[0], slot_ctx[1], slot_ctx[2], slot_ctx[3]);
    cpu_physical_memory_write(octx, (uint8_t *) slot_ctx, sizeof(slot_ctx));

    return CC_SUCCESS;
}

static unsigned int xhci_get_slot(XHCIState *xhci, XHCIEvent *event, XHCITRB *trb)
{
    unsigned int slotid;
    slotid = (trb->control >> TRB_CR_SLOTID_SHIFT) & TRB_CR_SLOTID_MASK;
    if (slotid < 1 || slotid > MAXSLOTS) {
        fprintf(stderr, "xhci: bad slot id %d\n", slotid);
        event->ccode = CC_TRB_ERROR;
        return 0;
    } else if (!xhci->slots[slotid-1].enabled) {
        fprintf(stderr, "xhci: slot id %d not enabled\n", slotid);
        event->ccode = CC_SLOT_NOT_ENABLED_ERROR;
        return 0;
    }
    return slotid;
}

static TRBCCode xhci_get_port_bandwidth(XHCIState *xhci, uint64_t pctx)
{
    target_phys_addr_t ctx;
    uint8_t bw_ctx[MAXPORTS+1];

    DPRINTF("xhci_get_port_bandwidth()\n");

    ctx = xhci_mask64(pctx);

    DPRINTF("xhci: bandwidth context at "TARGET_FMT_plx"\n", ctx);

    /* TODO: actually implement real values here */
    bw_ctx[0] = 0;
    memset(&bw_ctx[1], 80, MAXPORTS); /* 80% */
    cpu_physical_memory_write(ctx, bw_ctx, sizeof(bw_ctx));

    return CC_SUCCESS;
}

static uint32_t rotl(uint32_t v, unsigned count)
{
    count &= 31;
    return (v << count) | (v >> (32 - count));
}


static uint32_t xhci_nec_challenge(uint32_t hi, uint32_t lo)
{
    uint32_t val;
    val = rotl(lo - 0x49434878, 32 - ((hi>>8) & 0x1F));
    val += rotl(lo + 0x49434878, hi & 0x1F);
    val -= rotl(hi ^ 0x49434878, (lo >> 16) & 0x1F);
    return ~val;
}

static void xhci_via_challenge(uint64_t addr)
{
    uint32_t buf[8];
    uint32_t obuf[8];
    target_phys_addr_t paddr = xhci_mask64(addr);

    cpu_physical_memory_read(paddr, (uint8_t *) &buf, 32);

    memcpy(obuf, buf, sizeof(obuf));

    if ((buf[0] & 0xff) == 2) {
        obuf[0] = 0x49932000 + 0x54dc200 * buf[2] + 0x7429b578 * buf[3];
        obuf[0] |=  (buf[2] * buf[3]) & 0xff;
        obuf[1] = 0x0132bb37 + 0xe89 * buf[2] + 0xf09 * buf[3];
        obuf[2] = 0x0066c2e9 + 0x2091 * buf[2] + 0x19bd * buf[3];
        obuf[3] = 0xd5281342 + 0x2cc9691 * buf[2] + 0x2367662 * buf[3];
        obuf[4] = 0x0123c75c + 0x1595 * buf[2] + 0x19ec * buf[3];
        obuf[5] = 0x00f695de + 0x26fd * buf[2] + 0x3e9 * buf[3];
        obuf[6] = obuf[2] ^ obuf[3] ^ 0x29472956;
        obuf[7] = obuf[2] ^ obuf[3] ^ 0x65866593;
    }

    cpu_physical_memory_write(paddr, (uint8_t *) &obuf, 32);
}

static void xhci_process_commands(XHCIState *xhci)
{
    XHCITRB trb;
    TRBType type;
    XHCIEvent event = {ER_COMMAND_COMPLETE, CC_SUCCESS};
    target_phys_addr_t addr;
    unsigned int i, slotid = 0;

    DPRINTF("xhci_process_commands()\n");
    if (!xhci_running(xhci)) {
        DPRINTF("xhci_process_commands() called while xHC stopped or paused\n");
        return;
    }

    xhci->crcr_low |= CRCR_CRR;

    while ((type = xhci_ring_fetch(xhci, &xhci->cmd_ring, &trb, &addr))) {
        event.ptr = addr;
        switch (type) {
        case CR_ENABLE_SLOT:
            for (i = 0; i < MAXSLOTS; i++) {
                if (!xhci->slots[i].enabled) {
                    break;
                }
            }
            if (i >= MAXSLOTS) {
                fprintf(stderr, "xhci: no device slots available\n");
                event.ccode = CC_NO_SLOTS_ERROR;
            } else {
                slotid = i+1;
                event.ccode = xhci_enable_slot(xhci, slotid);
            }
            break;
        case CR_DISABLE_SLOT:
            slotid = xhci_get_slot(xhci, &event, &trb);
            if (slotid) {
                event.ccode = xhci_disable_slot(xhci, slotid);
            }
            break;
        case CR_ADDRESS_DEVICE:
            slotid = xhci_get_slot(xhci, &event, &trb);
            if (slotid) {
                event.ccode = xhci_address_slot(xhci, slotid, trb.parameter,
                                                trb.control & TRB_CR_BSR);
            }
            break;
        case CR_CONFIGURE_ENDPOINT:
            slotid = xhci_get_slot(xhci, &event, &trb);
            if (slotid) {
                event.ccode = xhci_configure_slot(xhci, slotid, trb.parameter,
                                                  trb.control & TRB_CR_DC);
            }
            break;
        case CR_EVALUATE_CONTEXT:
            slotid = xhci_get_slot(xhci, &event, &trb);
            if (slotid) {
                event.ccode = xhci_evaluate_slot(xhci, slotid, trb.parameter);
            }
            break;
        case CR_STOP_ENDPOINT:
            slotid = xhci_get_slot(xhci, &event, &trb);
            if (slotid) {
                unsigned int epid = (trb.control >> TRB_CR_EPID_SHIFT)
                    & TRB_CR_EPID_MASK;
                event.ccode = xhci_stop_ep(xhci, slotid, epid);
            }
            break;
        case CR_RESET_ENDPOINT:
            slotid = xhci_get_slot(xhci, &event, &trb);
            if (slotid) {
                unsigned int epid = (trb.control >> TRB_CR_EPID_SHIFT)
                    & TRB_CR_EPID_MASK;
                event.ccode = xhci_reset_ep(xhci, slotid, epid);
            }
            break;
        case CR_SET_TR_DEQUEUE:
            slotid = xhci_get_slot(xhci, &event, &trb);
            if (slotid) {
                unsigned int epid = (trb.control >> TRB_CR_EPID_SHIFT)
                    & TRB_CR_EPID_MASK;
                event.ccode = xhci_set_ep_dequeue(xhci, slotid, epid,
                                                  trb.parameter);
            }
            break;
        case CR_RESET_DEVICE:
            slotid = xhci_get_slot(xhci, &event, &trb);
            if (slotid) {
                event.ccode = xhci_reset_slot(xhci, slotid);
            }
            break;
        case CR_GET_PORT_BANDWIDTH:
            event.ccode = xhci_get_port_bandwidth(xhci, trb.parameter);
            break;
        case CR_VENDOR_VIA_CHALLENGE_RESPONSE:
            xhci_via_challenge(trb.parameter);
            break;
        case CR_VENDOR_NEC_FIRMWARE_REVISION:
            event.type = 48; /* NEC reply */
            event.length = 0x3025;
            break;
        case CR_VENDOR_NEC_CHALLENGE_RESPONSE:
        {
            uint32_t chi = trb.parameter >> 32;
            uint32_t clo = trb.parameter;
            uint32_t val = xhci_nec_challenge(chi, clo);
            event.length = val & 0xFFFF;
            event.epid = val >> 16;
            slotid = val >> 24;
            event.type = 48; /* NEC reply */
        }
        break;
        default:
            fprintf(stderr, "xhci: unimplemented command %d\n", type);
            event.ccode = CC_TRB_ERROR;
            break;
        }
        event.slotid = slotid;
        xhci_event(xhci, &event);
    }
}

static void xhci_update_port(XHCIState *xhci, XHCIPort *port, int is_detach)
{
    int nr = port->port.index + 1;

    port->portsc = PORTSC_PP;
    if (port->port.dev && !is_detach) {
        port->portsc |= PORTSC_CCS;
        switch (port->port.dev->speed) {
        case USB_SPEED_LOW:
            port->portsc |= PORTSC_SPEED_LOW;
            break;
        case USB_SPEED_FULL:
            port->portsc |= PORTSC_SPEED_FULL;
            break;
        case USB_SPEED_HIGH:
            port->portsc |= PORTSC_SPEED_HIGH;
            break;
        }
    }

    if (xhci_running(xhci)) {
        port->portsc |= PORTSC_CSC;
        XHCIEvent ev = { ER_PORT_STATUS_CHANGE, CC_SUCCESS, nr << 24};
        xhci_event(xhci, &ev);
        DPRINTF("xhci: port change event for port %d\n", nr);
    }
}

static void xhci_reset(void *opaque)
{
    XHCIState *xhci = opaque;
    int i;

    DPRINTF("xhci: full reset\n");
    if (!(xhci->usbsts & USBSTS_HCH)) {
        fprintf(stderr, "xhci: reset while running!\n");
    }

    xhci->usbcmd = 0;
    xhci->usbsts = USBSTS_HCH;
    xhci->dnctrl = 0;
    xhci->crcr_low = 0;
    xhci->crcr_high = 0;
    xhci->dcbaap_low = 0;
    xhci->dcbaap_high = 0;
    xhci->config = 0;
    xhci->devaddr = 2;

    for (i = 0; i < MAXSLOTS; i++) {
        xhci_disable_slot(xhci, i+1);
    }

    for (i = 0; i < MAXPORTS; i++) {
        xhci_update_port(xhci, xhci->ports + i, 0);
    }

    xhci->mfindex = 0;
    xhci->iman = 0;
    xhci->imod = 0;
    xhci->erstsz = 0;
    xhci->erstba_low = 0;
    xhci->erstba_high = 0;
    xhci->erdp_low = 0;
    xhci->erdp_high = 0;

    xhci->er_ep_idx = 0;
    xhci->er_pcs = 1;
    xhci->er_full = 0;
    xhci->ev_buffer_put = 0;
    xhci->ev_buffer_get = 0;
}

static uint32_t xhci_cap_read(XHCIState *xhci, uint32_t reg)
{
    DPRINTF("xhci_cap_read(0x%x)\n", reg);

    switch (reg) {
    case 0x00: /* HCIVERSION, CAPLENGTH */
        return 0x01000000 | LEN_CAP;
    case 0x04: /* HCSPARAMS 1 */
        return (MAXPORTS<<24) | (MAXINTRS<<8) | MAXSLOTS;
    case 0x08: /* HCSPARAMS 2 */
        return 0x0000000f;
    case 0x0c: /* HCSPARAMS 3 */
        return 0x00000000;
    case 0x10: /* HCCPARAMS */
#if TARGET_PHYS_ADDR_BITS > 32
        return 0x00081001;
#else
        return 0x00081000;
#endif
    case 0x14: /* DBOFF */
        return OFF_DOORBELL;
    case 0x18: /* RTSOFF */
        return OFF_RUNTIME;

    /* extended capabilities */
    case 0x20: /* Supported Protocol:00 */
#if USB3_PORTS > 0
        return 0x02000402; /* USB 2.0 */
#else
        return 0x02000002; /* USB 2.0 */
#endif
    case 0x24: /* Supported Protocol:04 */
        return 0x20425455; /* "USB " */
    case 0x28: /* Supported Protocol:08 */
        return 0x00000001 | (USB2_PORTS<<8);
    case 0x2c: /* Supported Protocol:0c */
        return 0x00000000; /* reserved */
#if USB3_PORTS > 0
    case 0x30: /* Supported Protocol:00 */
        return 0x03000002; /* USB 3.0 */
    case 0x34: /* Supported Protocol:04 */
        return 0x20425455; /* "USB " */
    case 0x38: /* Supported Protocol:08 */
        return 0x00000000 | (USB2_PORTS+1) | (USB3_PORTS<<8);
    case 0x3c: /* Supported Protocol:0c */
        return 0x00000000; /* reserved */
#endif
    default:
        fprintf(stderr, "xhci_cap_read: reg %d unimplemented\n", reg);
    }
    return 0;
}

static uint32_t xhci_port_read(XHCIState *xhci, uint32_t reg)
{
    uint32_t port = reg >> 4;
    if (port >= MAXPORTS) {
        fprintf(stderr, "xhci_port_read: port %d out of bounds\n", port);
        return 0;
    }

    switch (reg & 0xf) {
    case 0x00: /* PORTSC */
        return xhci->ports[port].portsc;
    case 0x04: /* PORTPMSC */
    case 0x08: /* PORTLI */
        return 0;
    case 0x0c: /* reserved */
    default:
        fprintf(stderr, "xhci_port_read (port %d): reg 0x%x unimplemented\n",
                port, reg);
        return 0;
    }
}

static void xhci_port_write(XHCIState *xhci, uint32_t reg, uint32_t val)
{
    uint32_t port = reg >> 4;
    uint32_t portsc;

    if (port >= MAXPORTS) {
        fprintf(stderr, "xhci_port_read: port %d out of bounds\n", port);
        return;
    }

    switch (reg & 0xf) {
    case 0x00: /* PORTSC */
        portsc = xhci->ports[port].portsc;
        /* write-1-to-clear bits*/
        portsc &= ~(val & (PORTSC_CSC|PORTSC_PEC|PORTSC_WRC|PORTSC_OCC|
                           PORTSC_PRC|PORTSC_PLC|PORTSC_CEC));
        if (val & PORTSC_LWS) {
            /* overwrite PLS only when LWS=1 */
            portsc &= ~(PORTSC_PLS_MASK << PORTSC_PLS_SHIFT);
            portsc |= val & (PORTSC_PLS_MASK << PORTSC_PLS_SHIFT);
        }
        /* read/write bits */
        portsc &= ~(PORTSC_PP|PORTSC_WCE|PORTSC_WDE|PORTSC_WOE);
        portsc |= (val & (PORTSC_PP|PORTSC_WCE|PORTSC_WDE|PORTSC_WOE));
        /* write-1-to-start bits */
        if (val & PORTSC_PR) {
            DPRINTF("xhci: port %d reset\n", port);
            if (xhci->ports[port].port.dev) {
                usb_send_msg(xhci->ports[port].port.dev, USB_MSG_RESET);
            }
            portsc |= PORTSC_PRC | PORTSC_PED;
        }
        xhci->ports[port].portsc = portsc;
        break;
    case 0x04: /* PORTPMSC */
    case 0x08: /* PORTLI */
    default:
        fprintf(stderr, "xhci_port_write (port %d): reg 0x%x unimplemented\n",
                port, reg);
    }
}

static uint32_t xhci_oper_read(XHCIState *xhci, uint32_t reg)
{
    DPRINTF("xhci_oper_read(0x%x)\n", reg);

    if (reg >= 0x400) {
        return xhci_port_read(xhci, reg - 0x400);
    }

    switch (reg) {
    case 0x00: /* USBCMD */
        return xhci->usbcmd;
    case 0x04: /* USBSTS */
        return xhci->usbsts;
    case 0x08: /* PAGESIZE */
        return 1; /* 4KiB */
    case 0x14: /* DNCTRL */
        return xhci->dnctrl;
    case 0x18: /* CRCR low */
        return xhci->crcr_low & ~0xe;
    case 0x1c: /* CRCR high */
        return xhci->crcr_high;
    case 0x30: /* DCBAAP low */
        return xhci->dcbaap_low;
    case 0x34: /* DCBAAP high */
        return xhci->dcbaap_high;
    case 0x38: /* CONFIG */
        return xhci->config;
    default:
        fprintf(stderr, "xhci_oper_read: reg 0x%x unimplemented\n", reg);
    }
    return 0;
}

static void xhci_oper_write(XHCIState *xhci, uint32_t reg, uint32_t val)
{
    DPRINTF("xhci_oper_write(0x%x, 0x%08x)\n", reg, val);

    if (reg >= 0x400) {
        xhci_port_write(xhci, reg - 0x400, val);
        return;
    }

    switch (reg) {
    case 0x00: /* USBCMD */
        if ((val & USBCMD_RS) && !(xhci->usbcmd & USBCMD_RS)) {
            xhci_run(xhci);
        } else if (!(val & USBCMD_RS) && (xhci->usbcmd & USBCMD_RS)) {
            xhci_stop(xhci);
        }
        xhci->usbcmd = val & 0xc0f;
        if (val & USBCMD_HCRST) {
            xhci_reset(xhci);
        }
        xhci_irq_update(xhci);
        break;

    case 0x04: /* USBSTS */
        /* these bits are write-1-to-clear */
        xhci->usbsts &= ~(val & (USBSTS_HSE|USBSTS_EINT|USBSTS_PCD|USBSTS_SRE));
        xhci_irq_update(xhci);
        break;

    case 0x14: /* DNCTRL */
        xhci->dnctrl = val & 0xffff;
        break;
    case 0x18: /* CRCR low */
        xhci->crcr_low = (val & 0xffffffcf) | (xhci->crcr_low & CRCR_CRR);
        break;
    case 0x1c: /* CRCR high */
        xhci->crcr_high = val;
        if (xhci->crcr_low & (CRCR_CA|CRCR_CS) && (xhci->crcr_low & CRCR_CRR)) {
            XHCIEvent event = {ER_COMMAND_COMPLETE, CC_COMMAND_RING_STOPPED};
            xhci->crcr_low &= ~CRCR_CRR;
            xhci_event(xhci, &event);
            DPRINTF("xhci: command ring stopped (CRCR=%08x)\n", xhci->crcr_low);
        } else {
            target_phys_addr_t base = xhci_addr64(xhci->crcr_low & ~0x3f, val);
            xhci_ring_init(xhci, &xhci->cmd_ring, base);
        }
        xhci->crcr_low &= ~(CRCR_CA | CRCR_CS);
        break;
    case 0x30: /* DCBAAP low */
        xhci->dcbaap_low = val & 0xffffffc0;
        break;
    case 0x34: /* DCBAAP high */
        xhci->dcbaap_high = val;
        break;
    case 0x38: /* CONFIG */
        xhci->config = val & 0xff;
        break;
    default:
        fprintf(stderr, "xhci_oper_write: reg 0x%x unimplemented\n", reg);
    }
}

static uint32_t xhci_runtime_read(XHCIState *xhci, uint32_t reg)
{
    DPRINTF("xhci_runtime_read(0x%x)\n", reg);

    switch (reg) {
    case 0x00: /* MFINDEX */
        fprintf(stderr, "xhci_runtime_read: MFINDEX not yet implemented\n");
        return xhci->mfindex;
    case 0x20: /* IMAN */
        return xhci->iman;
    case 0x24: /* IMOD */
        return xhci->imod;
    case 0x28: /* ERSTSZ */
        return xhci->erstsz;
    case 0x30: /* ERSTBA low */
        return xhci->erstba_low;
    case 0x34: /* ERSTBA high */
        return xhci->erstba_high;
    case 0x38: /* ERDP low */
        return xhci->erdp_low;
    case 0x3c: /* ERDP high */
        return xhci->erdp_high;
    default:
        fprintf(stderr, "xhci_runtime_read: reg 0x%x unimplemented\n", reg);
    }
    return 0;
}

static void xhci_runtime_write(XHCIState *xhci, uint32_t reg, uint32_t val)
{
    DPRINTF("xhci_runtime_write(0x%x, 0x%08x)\n", reg, val);

    switch (reg) {
    case 0x20: /* IMAN */
        if (val & IMAN_IP) {
            xhci->iman &= ~IMAN_IP;
        }
        xhci->iman &= ~IMAN_IE;
        xhci->iman |= val & IMAN_IE;
        xhci_irq_update(xhci);
        break;
    case 0x24: /* IMOD */
        xhci->imod = val;
        break;
    case 0x28: /* ERSTSZ */
        xhci->erstsz = val & 0xffff;
        break;
    case 0x30: /* ERSTBA low */
        /* XXX NEC driver bug: it doesn't align this to 64 bytes
        xhci->erstba_low = val & 0xffffffc0; */
        xhci->erstba_low = val & 0xfffffff0;
        break;
    case 0x34: /* ERSTBA high */
        xhci->erstba_high = val;
        xhci_er_reset(xhci);
        break;
    case 0x38: /* ERDP low */
        if (val & ERDP_EHB) {
            xhci->erdp_low &= ~ERDP_EHB;
        }
        xhci->erdp_low = (val & ~ERDP_EHB) | (xhci->erdp_low & ERDP_EHB);
        break;
    case 0x3c: /* ERDP high */
        xhci->erdp_high = val;
        xhci_events_update(xhci);
        break;
    default:
        fprintf(stderr, "xhci_oper_write: reg 0x%x unimplemented\n", reg);
    }
}

static uint32_t xhci_doorbell_read(XHCIState *xhci, uint32_t reg)
{
    DPRINTF("xhci_doorbell_read(0x%x)\n", reg);
    /* doorbells always read as 0 */
    return 0;
}

static void xhci_doorbell_write(XHCIState *xhci, uint32_t reg, uint32_t val)
{
    DPRINTF("xhci_doorbell_write(0x%x, 0x%08x)\n", reg, val);

    if (!xhci_running(xhci)) {
        fprintf(stderr, "xhci: wrote doorbell while xHC stopped or paused\n");
        return;
    }

    reg >>= 2;

    if (reg == 0) {
        if (val == 0) {
            xhci_process_commands(xhci);
        } else {
            fprintf(stderr, "xhci: bad doorbell 0 write: 0x%x\n", val);
        }
    } else {
        if (reg > MAXSLOTS) {
            fprintf(stderr, "xhci: bad doorbell %d\n", reg);
        } else if (val > 31) {
            fprintf(stderr, "xhci: bad doorbell %d write: 0x%x\n", reg, val);
        } else {
            xhci_kick_ep(xhci, reg, val);
        }
    }
}

static uint64_t xhci_mem_read(void *ptr, target_phys_addr_t addr,
                              unsigned size)
{
    XHCIState *xhci = ptr;

    /* Only aligned reads are allowed on xHCI */
    if (addr & 3) {
        fprintf(stderr, "xhci_mem_read: Mis-aligned read\n");
        return 0;
    }

    if (addr < LEN_CAP) {
        return xhci_cap_read(xhci, addr);
    } else if (addr >= OFF_OPER && addr < (OFF_OPER + LEN_OPER)) {
        return xhci_oper_read(xhci, addr - OFF_OPER);
    } else if (addr >= OFF_RUNTIME && addr < (OFF_RUNTIME + LEN_RUNTIME)) {
        return xhci_runtime_read(xhci, addr - OFF_RUNTIME);
    } else if (addr >= OFF_DOORBELL && addr < (OFF_DOORBELL + LEN_DOORBELL)) {
        return xhci_doorbell_read(xhci, addr - OFF_DOORBELL);
    } else {
        fprintf(stderr, "xhci_mem_read: Bad offset %x\n", (int)addr);
        return 0;
    }
}

static void xhci_mem_write(void *ptr, target_phys_addr_t addr,
                           uint64_t val, unsigned size)
{
    XHCIState *xhci = ptr;

    /* Only aligned writes are allowed on xHCI */
    if (addr & 3) {
        fprintf(stderr, "xhci_mem_write: Mis-aligned write\n");
        return;
    }

    if (addr >= OFF_OPER && addr < (OFF_OPER + LEN_OPER)) {
        xhci_oper_write(xhci, addr - OFF_OPER, val);
    } else if (addr >= OFF_RUNTIME && addr < (OFF_RUNTIME + LEN_RUNTIME)) {
        xhci_runtime_write(xhci, addr - OFF_RUNTIME, val);
    } else if (addr >= OFF_DOORBELL && addr < (OFF_DOORBELL + LEN_DOORBELL)) {
        xhci_doorbell_write(xhci, addr - OFF_DOORBELL, val);
    } else {
        fprintf(stderr, "xhci_mem_write: Bad offset %x\n", (int)addr);
    }
}

static const MemoryRegionOps xhci_mem_ops = {
    .read = xhci_mem_read,
    .write = xhci_mem_write,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void xhci_attach(USBPort *usbport)
{
    XHCIState *xhci = usbport->opaque;
    XHCIPort *port = &xhci->ports[usbport->index];

    xhci_update_port(xhci, port, 0);
}

static void xhci_detach(USBPort *usbport)
{
    XHCIState *xhci = usbport->opaque;
    XHCIPort *port = &xhci->ports[usbport->index];

    xhci_update_port(xhci, port, 1);
}

static void xhci_complete(USBPort *port, USBPacket *packet)
{
    XHCITransfer *xfer = container_of(packet, XHCITransfer, packet);

    xhci_complete_packet(xfer, packet->result);
    xhci_kick_ep(xfer->xhci, xfer->slotid, xfer->epid);
}

static void xhci_child_detach(USBPort *port, USBDevice *child)
{
    FIXME();
}

static USBPortOps xhci_port_ops = {
    .attach   = xhci_attach,
    .detach   = xhci_detach,
    .complete = xhci_complete,
    .child_detach = xhci_child_detach,
};

static USBBusOps xhci_bus_ops = {
};

static void usb_xhci_init(XHCIState *xhci, DeviceState *dev)
{
    int i;

    xhci->usbsts = USBSTS_HCH;

    usb_bus_new(&xhci->bus, &xhci_bus_ops, &xhci->pci_dev.qdev);

    for (i = 0; i < MAXPORTS; i++) {
        memset(&xhci->ports[i], 0, sizeof(xhci->ports[i]));
        usb_register_port(&xhci->bus, &xhci->ports[i].port, xhci, i,
                          &xhci_port_ops, USB_SPEED_MASK_HIGH);
    }
    for (i = 0; i < MAXSLOTS; i++) {
        xhci->slots[i].enabled = 0;
    }

    qemu_register_reset(xhci_reset, xhci);
}

static int usb_xhci_initfn(struct PCIDevice *dev)
{
    int ret;

    XHCIState *xhci = DO_UPCAST(XHCIState, pci_dev, dev);

    xhci->pci_dev.config[PCI_CLASS_PROG] = 0x30;    /* xHCI */
    xhci->pci_dev.config[PCI_INTERRUPT_PIN] = 0x01; /* interrupt pin 1 */
    xhci->pci_dev.config[PCI_CACHE_LINE_SIZE] = 0x10;
    xhci->pci_dev.config[0x60] = 0x30; /* release number */

    usb_xhci_init(xhci, &dev->qdev);

    xhci->irq = xhci->pci_dev.irq[0];

    memory_region_init_io(&xhci->mem, &xhci_mem_ops, xhci,
                          "xhci", LEN_REGS);
    pci_register_bar(&xhci->pci_dev, 0,
                     PCI_BASE_ADDRESS_SPACE_MEMORY|PCI_BASE_ADDRESS_MEM_TYPE_64,
                     &xhci->mem);

    ret = pcie_cap_init(&xhci->pci_dev, 0xa0, PCI_EXP_TYPE_ENDPOINT, 0);
    assert(ret >= 0);

    if (xhci->msi) {
        ret = msi_init(&xhci->pci_dev, 0x70, 1, true, false);
        assert(ret >= 0);
    }

    return 0;
}

static void xhci_write_config(PCIDevice *dev, uint32_t addr, uint32_t val,
                              int len)
{
    XHCIState *xhci = DO_UPCAST(XHCIState, pci_dev, dev);

    pci_default_write_config(dev, addr, val, len);
    if (xhci->msi) {
        msi_write_config(dev, addr, val, len);
    }
}

static const VMStateDescription vmstate_xhci = {
    .name = "xhci",
    .unmigratable = 1,
};

static PCIDeviceInfo xhci_info = {
    .qdev.name    = "nec-usb-xhci",
    .qdev.alias   = "xhci",
    .qdev.size    = sizeof(XHCIState),
    .qdev.vmsd    = &vmstate_xhci,
    .init         = usb_xhci_initfn,
    .vendor_id    = PCI_VENDOR_ID_NEC,
    .device_id    = PCI_DEVICE_ID_NEC_UPD720200,
    .class_id     = PCI_CLASS_SERIAL_USB,
    .revision     = 0x03,
    .is_express   = 1,
    .config_write = xhci_write_config,
    .qdev.props   = (Property[]) {
        DEFINE_PROP_UINT32("msi", XHCIState, msi, 0),
        DEFINE_PROP_END_OF_LIST(),
    }
};

static void xhci_register(void)
{
    pci_qdev_register(&xhci_info);
}
device_init(xhci_register);
