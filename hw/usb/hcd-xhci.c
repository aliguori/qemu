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
#include "hw/hw.h"
#include "qemu-timer.h"
#include "hw/usb.h"
#include "hw/pci.h"
#include "hw/msi.h"
#include "hw/msix.h"
#include "trace.h"

//#define DEBUG_XHCI
//#define DEBUG_DATA

#ifdef DEBUG_XHCI
#define DPRINTF(...) fprintf(stderr, __VA_ARGS__)
#else
#define DPRINTF(...) do {} while (0)
#endif
#define FIXME() do { fprintf(stderr, "FIXME %s:%d\n", \
                             __func__, __LINE__); abort(); } while (0)

#define MAXPORTS_2 15
#define MAXPORTS_3 15

#define MAXPORTS (MAXPORTS_2+MAXPORTS_3)
#define MAXSLOTS 64
#define MAXINTRS 16

#define TD_QUEUE 24

/* Very pessimistic, let's hope it's enough for all cases */
#define EV_QUEUE (((3*TD_QUEUE)+16)*MAXSLOTS)
/* Do not deliver ER Full events. NEC's driver does some things not bound
 * to the specs when it gets them */
#define ER_FULL_HACK

#define LEN_CAP         0x40
#define LEN_OPER        (0x400 + 0x10 * MAXPORTS)
#define LEN_RUNTIME     ((MAXINTRS + 1) * 0x20)
#define LEN_DOORBELL    ((MAXSLOTS + 1) * 0x20)

#define OFF_OPER        LEN_CAP
#define OFF_RUNTIME     0x1000
#define OFF_DOORBELL    0x2000
#define OFF_MSIX_TABLE  0x3000
#define OFF_MSIX_PBA    0x3800
/* must be power of 2 */
#define LEN_REGS        0x4000

#if (OFF_OPER + LEN_OPER) > OFF_RUNTIME
#error Increase OFF_RUNTIME
#endif
#if (OFF_RUNTIME + LEN_RUNTIME) > OFF_DOORBELL
#error Increase OFF_DOORBELL
#endif
#if (OFF_DOORBELL + LEN_DOORBELL) > LEN_REGS
# error Increase LEN_REGS
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
    dma_addr_t addr;
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

#define TRB_INTR_SHIFT          22
#define TRB_INTR_MASK       0x3ff
#define TRB_INTR(t)         (((t).status >> TRB_INTR_SHIFT) & TRB_INTR_MASK)

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

typedef struct XHCIState XHCIState;

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
    dma_addr_t base;
    dma_addr_t dequeue;
    bool ccs;
} XHCIRing;

typedef struct XHCIPort {
    XHCIState *xhci;
    uint32_t portsc;
    uint32_t portnr;
    USBPort  *uport;
    uint32_t speedmask;
    char name[16];
    MemoryRegion mem;
} XHCIPort;

typedef struct XHCITransfer {
    XHCIState *xhci;
    USBPacket packet;
    QEMUSGList sgl;
    bool running_async;
    bool running_retry;
    bool cancelled;
    bool complete;
    bool int_req;
    unsigned int iso_pkts;
    unsigned int slotid;
    unsigned int epid;
    bool in_xfer;
    bool iso_xfer;

    unsigned int trb_count;
    unsigned int trb_alloced;
    XHCITRB *trbs;

    TRBCCode status;

    unsigned int pkts;
    unsigned int pktsize;
    unsigned int cur_pkt;

    uint64_t mfindex_kick;
} XHCITransfer;

typedef struct XHCIEPContext {
    XHCIState *xhci;
    unsigned int slotid;
    unsigned int epid;

    XHCIRing ring;
    unsigned int next_xfer;
    unsigned int comp_xfer;
    XHCITransfer transfers[TD_QUEUE];
    XHCITransfer *retry;
    EPType type;
    dma_addr_t pctx;
    unsigned int max_psize;
    uint32_t state;

    /* iso xfer scheduling */
    unsigned int interval;
    int64_t mfindex_last;
    QEMUTimer *kick_timer;
} XHCIEPContext;

typedef struct XHCISlot {
    bool enabled;
    dma_addr_t ctx;
    USBPort *uport;
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

typedef struct XHCIInterrupter {
    uint32_t iman;
    uint32_t imod;
    uint32_t erstsz;
    uint32_t erstba_low;
    uint32_t erstba_high;
    uint32_t erdp_low;
    uint32_t erdp_high;

    bool msix_used, er_pcs, er_full;

    dma_addr_t er_start;
    uint32_t er_size;
    unsigned int er_ep_idx;

    XHCIEvent ev_buffer[EV_QUEUE];
    unsigned int ev_buffer_put;
    unsigned int ev_buffer_get;

} XHCIInterrupter;

struct XHCIState {
    PCIDevice pci_dev;
    USBBus bus;
    qemu_irq irq;
    MemoryRegion mem;
    MemoryRegion mem_cap;
    MemoryRegion mem_oper;
    MemoryRegion mem_runtime;
    MemoryRegion mem_doorbell;
    const char *name;
    unsigned int devaddr;

    /* properties */
    uint32_t numports_2;
    uint32_t numports_3;
    uint32_t numintrs;
    uint32_t numslots;
    uint32_t flags;

    /* Operational Registers */
    uint32_t usbcmd;
    uint32_t usbsts;
    uint32_t dnctrl;
    uint32_t crcr_low;
    uint32_t crcr_high;
    uint32_t dcbaap_low;
    uint32_t dcbaap_high;
    uint32_t config;

    USBPort  uports[MAX(MAXPORTS_2, MAXPORTS_3)];
    XHCIPort ports[MAXPORTS];
    XHCISlot slots[MAXSLOTS];
    uint32_t numports;

    /* Runtime Registers */
    int64_t mfindex_start;
    QEMUTimer *mfwrap_timer;
    XHCIInterrupter intr[MAXINTRS];

    XHCIRing cmd_ring;
};

typedef struct XHCIEvRingSeg {
    uint32_t addr_low;
    uint32_t addr_high;
    uint32_t size;
    uint32_t rsvd;
} XHCIEvRingSeg;

enum xhci_flags {
    XHCI_FLAG_USE_MSI = 1,
    XHCI_FLAG_USE_MSI_X,
};

static void xhci_kick_ep(XHCIState *xhci, unsigned int slotid,
                         unsigned int epid);
static void xhci_event(XHCIState *xhci, XHCIEvent *event, int v);
static void xhci_write_event(XHCIState *xhci, XHCIEvent *event, int v);

static const char *TRBType_names[] = {
    [TRB_RESERVED]                     = "TRB_RESERVED",
    [TR_NORMAL]                        = "TR_NORMAL",
    [TR_SETUP]                         = "TR_SETUP",
    [TR_DATA]                          = "TR_DATA",
    [TR_STATUS]                        = "TR_STATUS",
    [TR_ISOCH]                         = "TR_ISOCH",
    [TR_LINK]                          = "TR_LINK",
    [TR_EVDATA]                        = "TR_EVDATA",
    [TR_NOOP]                          = "TR_NOOP",
    [CR_ENABLE_SLOT]                   = "CR_ENABLE_SLOT",
    [CR_DISABLE_SLOT]                  = "CR_DISABLE_SLOT",
    [CR_ADDRESS_DEVICE]                = "CR_ADDRESS_DEVICE",
    [CR_CONFIGURE_ENDPOINT]            = "CR_CONFIGURE_ENDPOINT",
    [CR_EVALUATE_CONTEXT]              = "CR_EVALUATE_CONTEXT",
    [CR_RESET_ENDPOINT]                = "CR_RESET_ENDPOINT",
    [CR_STOP_ENDPOINT]                 = "CR_STOP_ENDPOINT",
    [CR_SET_TR_DEQUEUE]                = "CR_SET_TR_DEQUEUE",
    [CR_RESET_DEVICE]                  = "CR_RESET_DEVICE",
    [CR_FORCE_EVENT]                   = "CR_FORCE_EVENT",
    [CR_NEGOTIATE_BW]                  = "CR_NEGOTIATE_BW",
    [CR_SET_LATENCY_TOLERANCE]         = "CR_SET_LATENCY_TOLERANCE",
    [CR_GET_PORT_BANDWIDTH]            = "CR_GET_PORT_BANDWIDTH",
    [CR_FORCE_HEADER]                  = "CR_FORCE_HEADER",
    [CR_NOOP]                          = "CR_NOOP",
    [ER_TRANSFER]                      = "ER_TRANSFER",
    [ER_COMMAND_COMPLETE]              = "ER_COMMAND_COMPLETE",
    [ER_PORT_STATUS_CHANGE]            = "ER_PORT_STATUS_CHANGE",
    [ER_BANDWIDTH_REQUEST]             = "ER_BANDWIDTH_REQUEST",
    [ER_DOORBELL]                      = "ER_DOORBELL",
    [ER_HOST_CONTROLLER]               = "ER_HOST_CONTROLLER",
    [ER_DEVICE_NOTIFICATION]           = "ER_DEVICE_NOTIFICATION",
    [ER_MFINDEX_WRAP]                  = "ER_MFINDEX_WRAP",
    [CR_VENDOR_VIA_CHALLENGE_RESPONSE] = "CR_VENDOR_VIA_CHALLENGE_RESPONSE",
    [CR_VENDOR_NEC_FIRMWARE_REVISION]  = "CR_VENDOR_NEC_FIRMWARE_REVISION",
    [CR_VENDOR_NEC_CHALLENGE_RESPONSE] = "CR_VENDOR_NEC_CHALLENGE_RESPONSE",
};

static const char *TRBCCode_names[] = {
    [CC_INVALID]                       = "CC_INVALID",
    [CC_SUCCESS]                       = "CC_SUCCESS",
    [CC_DATA_BUFFER_ERROR]             = "CC_DATA_BUFFER_ERROR",
    [CC_BABBLE_DETECTED]               = "CC_BABBLE_DETECTED",
    [CC_USB_TRANSACTION_ERROR]         = "CC_USB_TRANSACTION_ERROR",
    [CC_TRB_ERROR]                     = "CC_TRB_ERROR",
    [CC_STALL_ERROR]                   = "CC_STALL_ERROR",
    [CC_RESOURCE_ERROR]                = "CC_RESOURCE_ERROR",
    [CC_BANDWIDTH_ERROR]               = "CC_BANDWIDTH_ERROR",
    [CC_NO_SLOTS_ERROR]                = "CC_NO_SLOTS_ERROR",
    [CC_INVALID_STREAM_TYPE_ERROR]     = "CC_INVALID_STREAM_TYPE_ERROR",
    [CC_SLOT_NOT_ENABLED_ERROR]        = "CC_SLOT_NOT_ENABLED_ERROR",
    [CC_EP_NOT_ENABLED_ERROR]          = "CC_EP_NOT_ENABLED_ERROR",
    [CC_SHORT_PACKET]                  = "CC_SHORT_PACKET",
    [CC_RING_UNDERRUN]                 = "CC_RING_UNDERRUN",
    [CC_RING_OVERRUN]                  = "CC_RING_OVERRUN",
    [CC_VF_ER_FULL]                    = "CC_VF_ER_FULL",
    [CC_PARAMETER_ERROR]               = "CC_PARAMETER_ERROR",
    [CC_BANDWIDTH_OVERRUN]             = "CC_BANDWIDTH_OVERRUN",
    [CC_CONTEXT_STATE_ERROR]           = "CC_CONTEXT_STATE_ERROR",
    [CC_NO_PING_RESPONSE_ERROR]        = "CC_NO_PING_RESPONSE_ERROR",
    [CC_EVENT_RING_FULL_ERROR]         = "CC_EVENT_RING_FULL_ERROR",
    [CC_INCOMPATIBLE_DEVICE_ERROR]     = "CC_INCOMPATIBLE_DEVICE_ERROR",
    [CC_MISSED_SERVICE_ERROR]          = "CC_MISSED_SERVICE_ERROR",
    [CC_COMMAND_RING_STOPPED]          = "CC_COMMAND_RING_STOPPED",
    [CC_COMMAND_ABORTED]               = "CC_COMMAND_ABORTED",
    [CC_STOPPED]                       = "CC_STOPPED",
    [CC_STOPPED_LENGTH_INVALID]        = "CC_STOPPED_LENGTH_INVALID",
    [CC_MAX_EXIT_LATENCY_TOO_LARGE_ERROR]
    = "CC_MAX_EXIT_LATENCY_TOO_LARGE_ERROR",
    [CC_ISOCH_BUFFER_OVERRUN]          = "CC_ISOCH_BUFFER_OVERRUN",
    [CC_EVENT_LOST_ERROR]              = "CC_EVENT_LOST_ERROR",
    [CC_UNDEFINED_ERROR]               = "CC_UNDEFINED_ERROR",
    [CC_INVALID_STREAM_ID_ERROR]       = "CC_INVALID_STREAM_ID_ERROR",
    [CC_SECONDARY_BANDWIDTH_ERROR]     = "CC_SECONDARY_BANDWIDTH_ERROR",
    [CC_SPLIT_TRANSACTION_ERROR]       = "CC_SPLIT_TRANSACTION_ERROR",
};

static const char *lookup_name(uint32_t index, const char **list, uint32_t llen)
{
    if (index >= llen || list[index] == NULL) {
        return "???";
    }
    return list[index];
}

static const char *trb_name(XHCITRB *trb)
{
    return lookup_name(TRB_TYPE(*trb), TRBType_names,
                       ARRAY_SIZE(TRBType_names));
}

static const char *event_name(XHCIEvent *event)
{
    return lookup_name(event->ccode, TRBCCode_names,
                       ARRAY_SIZE(TRBCCode_names));
}

static uint64_t xhci_mfindex_get(XHCIState *xhci)
{
    int64_t now = qemu_get_clock_ns(vm_clock);
    return (now - xhci->mfindex_start) / 125000;
}

static void xhci_mfwrap_update(XHCIState *xhci)
{
    const uint32_t bits = USBCMD_RS | USBCMD_EWE;
    uint32_t mfindex, left;
    int64_t now;

    if ((xhci->usbcmd & bits) == bits) {
        now = qemu_get_clock_ns(vm_clock);
        mfindex = ((now - xhci->mfindex_start) / 125000) & 0x3fff;
        left = 0x4000 - mfindex;
        qemu_mod_timer(xhci->mfwrap_timer, now + left * 125000);
    } else {
        qemu_del_timer(xhci->mfwrap_timer);
    }
}

static void xhci_mfwrap_timer(void *opaque)
{
    XHCIState *xhci = opaque;
    XHCIEvent wrap = { ER_MFINDEX_WRAP, CC_SUCCESS };

    xhci_event(xhci, &wrap, 0);
    xhci_mfwrap_update(xhci);
}

static inline dma_addr_t xhci_addr64(uint32_t low, uint32_t high)
{
    if (sizeof(dma_addr_t) == 4) {
        return low;
    } else {
        return low | (((dma_addr_t)high << 16) << 16);
    }
}

static inline dma_addr_t xhci_mask64(uint64_t addr)
{
    if (sizeof(dma_addr_t) == 4) {
        return addr & 0xffffffff;
    } else {
        return addr;
    }
}

static XHCIPort *xhci_lookup_port(XHCIState *xhci, struct USBPort *uport)
{
    int index;

    if (!uport->dev) {
        return NULL;
    }
    switch (uport->dev->speed) {
    case USB_SPEED_LOW:
    case USB_SPEED_FULL:
    case USB_SPEED_HIGH:
        index = uport->index;
        break;
    case USB_SPEED_SUPER:
        index = uport->index + xhci->numports_2;
        break;
    default:
        return NULL;
    }
    return &xhci->ports[index];
}

static void xhci_intx_update(XHCIState *xhci)
{
    int level = 0;

    if (msix_enabled(&xhci->pci_dev) ||
        msi_enabled(&xhci->pci_dev)) {
        return;
    }

    if (xhci->intr[0].iman & IMAN_IP &&
        xhci->intr[0].iman & IMAN_IE &&
        xhci->usbcmd & USBCMD_INTE) {
        level = 1;
    }

    trace_usb_xhci_irq_intx(level);
    qemu_set_irq(xhci->irq, level);
}

static void xhci_msix_update(XHCIState *xhci, int v)
{
    bool enabled;

    if (!msix_enabled(&xhci->pci_dev)) {
        return;
    }

    enabled = xhci->intr[v].iman & IMAN_IE;
    if (enabled == xhci->intr[v].msix_used) {
        return;
    }

    if (enabled) {
        trace_usb_xhci_irq_msix_use(v);
        msix_vector_use(&xhci->pci_dev, v);
        xhci->intr[v].msix_used = true;
    } else {
        trace_usb_xhci_irq_msix_unuse(v);
        msix_vector_unuse(&xhci->pci_dev, v);
        xhci->intr[v].msix_used = false;
    }
}

static void xhci_intr_raise(XHCIState *xhci, int v)
{
    xhci->intr[v].erdp_low |= ERDP_EHB;
    xhci->intr[v].iman |= IMAN_IP;
    xhci->usbsts |= USBSTS_EINT;

    if (!(xhci->intr[v].iman & IMAN_IE)) {
        return;
    }

    if (!(xhci->usbcmd & USBCMD_INTE)) {
        return;
    }

    if (msix_enabled(&xhci->pci_dev)) {
        trace_usb_xhci_irq_msix(v);
        msix_notify(&xhci->pci_dev, v);
        return;
    }

    if (msi_enabled(&xhci->pci_dev)) {
        trace_usb_xhci_irq_msi(v);
        msi_notify(&xhci->pci_dev, v);
        return;
    }

    if (v == 0) {
        trace_usb_xhci_irq_intx(1);
        qemu_set_irq(xhci->irq, 1);
    }
}

static inline int xhci_running(XHCIState *xhci)
{
    return !(xhci->usbsts & USBSTS_HCH) && !xhci->intr[0].er_full;
}

static void xhci_die(XHCIState *xhci)
{
    xhci->usbsts |= USBSTS_HCE;
    fprintf(stderr, "xhci: asserted controller error\n");
}

static void xhci_write_event(XHCIState *xhci, XHCIEvent *event, int v)
{
    XHCIInterrupter *intr = &xhci->intr[v];
    XHCITRB ev_trb;
    dma_addr_t addr;

    ev_trb.parameter = cpu_to_le64(event->ptr);
    ev_trb.status = cpu_to_le32(event->length | (event->ccode << 24));
    ev_trb.control = (event->slotid << 24) | (event->epid << 16) |
                     event->flags | (event->type << TRB_TYPE_SHIFT);
    if (intr->er_pcs) {
        ev_trb.control |= TRB_C;
    }
    ev_trb.control = cpu_to_le32(ev_trb.control);

    trace_usb_xhci_queue_event(v, intr->er_ep_idx, trb_name(&ev_trb),
                               event_name(event), ev_trb.parameter,
                               ev_trb.status, ev_trb.control);

    addr = intr->er_start + TRB_SIZE*intr->er_ep_idx;
    pci_dma_write(&xhci->pci_dev, addr, &ev_trb, TRB_SIZE);

    intr->er_ep_idx++;
    if (intr->er_ep_idx >= intr->er_size) {
        intr->er_ep_idx = 0;
        intr->er_pcs = !intr->er_pcs;
    }
}

static void xhci_events_update(XHCIState *xhci, int v)
{
    XHCIInterrupter *intr = &xhci->intr[v];
    dma_addr_t erdp;
    unsigned int dp_idx;
    bool do_irq = 0;

    if (xhci->usbsts & USBSTS_HCH) {
        return;
    }

    erdp = xhci_addr64(intr->erdp_low, intr->erdp_high);
    if (erdp < intr->er_start ||
        erdp >= (intr->er_start + TRB_SIZE*intr->er_size)) {
        fprintf(stderr, "xhci: ERDP out of bounds: "DMA_ADDR_FMT"\n", erdp);
        fprintf(stderr, "xhci: ER[%d] at "DMA_ADDR_FMT" len %d\n",
                v, intr->er_start, intr->er_size);
        xhci_die(xhci);
        return;
    }
    dp_idx = (erdp - intr->er_start) / TRB_SIZE;
    assert(dp_idx < intr->er_size);

    /* NEC didn't read section 4.9.4 of the spec (v1.0 p139 top Note) and thus
     * deadlocks when the ER is full. Hack it by holding off events until
     * the driver decides to free at least half of the ring */
    if (intr->er_full) {
        int er_free = dp_idx - intr->er_ep_idx;
        if (er_free <= 0) {
            er_free += intr->er_size;
        }
        if (er_free < (intr->er_size/2)) {
            DPRINTF("xhci_events_update(): event ring still "
                    "more than half full (hack)\n");
            return;
        }
    }

    while (intr->ev_buffer_put != intr->ev_buffer_get) {
        assert(intr->er_full);
        if (((intr->er_ep_idx+1) % intr->er_size) == dp_idx) {
            DPRINTF("xhci_events_update(): event ring full again\n");
#ifndef ER_FULL_HACK
            XHCIEvent full = {ER_HOST_CONTROLLER, CC_EVENT_RING_FULL_ERROR};
            xhci_write_event(xhci, &full, v);
#endif
            do_irq = 1;
            break;
        }
        XHCIEvent *event = &intr->ev_buffer[intr->ev_buffer_get];
        xhci_write_event(xhci, event, v);
        intr->ev_buffer_get++;
        do_irq = 1;
        if (intr->ev_buffer_get == EV_QUEUE) {
            intr->ev_buffer_get = 0;
        }
    }

    if (do_irq) {
        xhci_intr_raise(xhci, v);
    }

    if (intr->er_full && intr->ev_buffer_put == intr->ev_buffer_get) {
        DPRINTF("xhci_events_update(): event ring no longer full\n");
        intr->er_full = 0;
    }
}

static void xhci_event(XHCIState *xhci, XHCIEvent *event, int v)
{
    XHCIInterrupter *intr;
    dma_addr_t erdp;
    unsigned int dp_idx;

    if (v >= xhci->numintrs) {
        DPRINTF("intr nr out of range (%d >= %d)\n", v, xhci->numintrs);
        return;
    }
    intr = &xhci->intr[v];

    if (intr->er_full) {
        DPRINTF("xhci_event(): ER full, queueing\n");
        if (((intr->ev_buffer_put+1) % EV_QUEUE) == intr->ev_buffer_get) {
            fprintf(stderr, "xhci: event queue full, dropping event!\n");
            return;
        }
        intr->ev_buffer[intr->ev_buffer_put++] = *event;
        if (intr->ev_buffer_put == EV_QUEUE) {
            intr->ev_buffer_put = 0;
        }
        return;
    }

    erdp = xhci_addr64(intr->erdp_low, intr->erdp_high);
    if (erdp < intr->er_start ||
        erdp >= (intr->er_start + TRB_SIZE*intr->er_size)) {
        fprintf(stderr, "xhci: ERDP out of bounds: "DMA_ADDR_FMT"\n", erdp);
        fprintf(stderr, "xhci: ER[%d] at "DMA_ADDR_FMT" len %d\n",
                v, intr->er_start, intr->er_size);
        xhci_die(xhci);
        return;
    }

    dp_idx = (erdp - intr->er_start) / TRB_SIZE;
    assert(dp_idx < intr->er_size);

    if ((intr->er_ep_idx+1) % intr->er_size == dp_idx) {
        DPRINTF("xhci_event(): ER full, queueing\n");
#ifndef ER_FULL_HACK
        XHCIEvent full = {ER_HOST_CONTROLLER, CC_EVENT_RING_FULL_ERROR};
        xhci_write_event(xhci, &full);
#endif
        intr->er_full = 1;
        if (((intr->ev_buffer_put+1) % EV_QUEUE) == intr->ev_buffer_get) {
            fprintf(stderr, "xhci: event queue full, dropping event!\n");
            return;
        }
        intr->ev_buffer[intr->ev_buffer_put++] = *event;
        if (intr->ev_buffer_put == EV_QUEUE) {
            intr->ev_buffer_put = 0;
        }
    } else {
        xhci_write_event(xhci, event, v);
    }

    xhci_intr_raise(xhci, v);
}

static void xhci_ring_init(XHCIState *xhci, XHCIRing *ring,
                           dma_addr_t base)
{
    ring->base = base;
    ring->dequeue = base;
    ring->ccs = 1;
}

static TRBType xhci_ring_fetch(XHCIState *xhci, XHCIRing *ring, XHCITRB *trb,
                               dma_addr_t *addr)
{
    while (1) {
        TRBType type;
        pci_dma_read(&xhci->pci_dev, ring->dequeue, trb, TRB_SIZE);
        trb->addr = ring->dequeue;
        trb->ccs = ring->ccs;
        le64_to_cpus(&trb->parameter);
        le32_to_cpus(&trb->status);
        le32_to_cpus(&trb->control);

        trace_usb_xhci_fetch_trb(ring->dequeue, trb_name(trb),
                                 trb->parameter, trb->status, trb->control);

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
    dma_addr_t dequeue = ring->dequeue;
    bool ccs = ring->ccs;
    /* hack to bundle together the two/three TDs that make a setup transfer */
    bool control_td_set = 0;

    while (1) {
        TRBType type;
        pci_dma_read(&xhci->pci_dev, dequeue, &trb, TRB_SIZE);
        le64_to_cpus(&trb.parameter);
        le32_to_cpus(&trb.status);
        le32_to_cpus(&trb.control);

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

static void xhci_er_reset(XHCIState *xhci, int v)
{
    XHCIInterrupter *intr = &xhci->intr[v];
    XHCIEvRingSeg seg;

    if (intr->erstsz == 0) {
        /* disabled */
        intr->er_start = 0;
        intr->er_size = 0;
        return;
    }
    /* cache the (sole) event ring segment location */
    if (intr->erstsz != 1) {
        fprintf(stderr, "xhci: invalid value for ERSTSZ: %d\n", intr->erstsz);
        xhci_die(xhci);
        return;
    }
    dma_addr_t erstba = xhci_addr64(intr->erstba_low, intr->erstba_high);
    pci_dma_read(&xhci->pci_dev, erstba, &seg, sizeof(seg));
    le32_to_cpus(&seg.addr_low);
    le32_to_cpus(&seg.addr_high);
    le32_to_cpus(&seg.size);
    if (seg.size < 16 || seg.size > 4096) {
        fprintf(stderr, "xhci: invalid value for segment size: %d\n", seg.size);
        xhci_die(xhci);
        return;
    }
    intr->er_start = xhci_addr64(seg.addr_low, seg.addr_high);
    intr->er_size = seg.size;

    intr->er_ep_idx = 0;
    intr->er_pcs = 1;
    intr->er_full = 0;

    DPRINTF("xhci: event ring[%d]:" DMA_ADDR_FMT " [%d]\n",
            v, intr->er_start, intr->er_size);
}

static void xhci_run(XHCIState *xhci)
{
    trace_usb_xhci_run();
    xhci->usbsts &= ~USBSTS_HCH;
    xhci->mfindex_start = qemu_get_clock_ns(vm_clock);
}

static void xhci_stop(XHCIState *xhci)
{
    trace_usb_xhci_stop();
    xhci->usbsts |= USBSTS_HCH;
    xhci->crcr_low &= ~CRCR_CRR;
}

static void xhci_set_ep_state(XHCIState *xhci, XHCIEPContext *epctx,
                              uint32_t state)
{
    uint32_t ctx[5];

    pci_dma_read(&xhci->pci_dev, epctx->pctx, ctx, sizeof(ctx));
    ctx[0] &= ~EP_STATE_MASK;
    ctx[0] |= state;
    ctx[2] = epctx->ring.dequeue | epctx->ring.ccs;
    ctx[3] = (epctx->ring.dequeue >> 16) >> 16;
    DPRINTF("xhci: set epctx: " DMA_ADDR_FMT " state=%d dequeue=%08x%08x\n",
            epctx->pctx, state, ctx[3], ctx[2]);
    pci_dma_write(&xhci->pci_dev, epctx->pctx, ctx, sizeof(ctx));
    epctx->state = state;
}

static void xhci_ep_kick_timer(void *opaque)
{
    XHCIEPContext *epctx = opaque;
    xhci_kick_ep(epctx->xhci, epctx->slotid, epctx->epid);
}

static TRBCCode xhci_enable_ep(XHCIState *xhci, unsigned int slotid,
                               unsigned int epid, dma_addr_t pctx,
                               uint32_t *ctx)
{
    XHCISlot *slot;
    XHCIEPContext *epctx;
    dma_addr_t dequeue;
    int i;

    trace_usb_xhci_ep_enable(slotid, epid);
    assert(slotid >= 1 && slotid <= xhci->numslots);
    assert(epid >= 1 && epid <= 31);

    slot = &xhci->slots[slotid-1];
    if (slot->eps[epid-1]) {
        fprintf(stderr, "xhci: slot %d ep %d already enabled!\n", slotid, epid);
        return CC_TRB_ERROR;
    }

    epctx = g_malloc(sizeof(XHCIEPContext));
    memset(epctx, 0, sizeof(XHCIEPContext));
    epctx->xhci = xhci;
    epctx->slotid = slotid;
    epctx->epid = epid;

    slot->eps[epid-1] = epctx;

    dequeue = xhci_addr64(ctx[2] & ~0xf, ctx[3]);
    xhci_ring_init(xhci, &epctx->ring, dequeue);
    epctx->ring.ccs = ctx[2] & 1;

    epctx->type = (ctx[1] >> EP_TYPE_SHIFT) & EP_TYPE_MASK;
    DPRINTF("xhci: endpoint %d.%d type is %d\n", epid/2, epid%2, epctx->type);
    epctx->pctx = pctx;
    epctx->max_psize = ctx[1]>>16;
    epctx->max_psize *= 1+((ctx[1]>>8)&0xff);
    DPRINTF("xhci: endpoint %d.%d max transaction (burst) size is %d\n",
            epid/2, epid%2, epctx->max_psize);
    for (i = 0; i < ARRAY_SIZE(epctx->transfers); i++) {
        usb_packet_init(&epctx->transfers[i].packet);
    }

    epctx->interval = 1 << (ctx[0] >> 16) & 0xff;
    epctx->mfindex_last = 0;
    epctx->kick_timer = qemu_new_timer_ns(vm_clock, xhci_ep_kick_timer, epctx);

    epctx->state = EP_RUNNING;
    ctx[0] &= ~EP_STATE_MASK;
    ctx[0] |= EP_RUNNING;

    return CC_SUCCESS;
}

static int xhci_ep_nuke_one_xfer(XHCITransfer *t)
{
    int killed = 0;

    if (t->running_async) {
        usb_cancel_packet(&t->packet);
        t->running_async = 0;
        t->cancelled = 1;
        DPRINTF("xhci: cancelling transfer, waiting for it to complete\n");
        killed = 1;
    }
    if (t->running_retry) {
        XHCIEPContext *epctx = t->xhci->slots[t->slotid-1].eps[t->epid-1];
        if (epctx) {
            epctx->retry = NULL;
            qemu_del_timer(epctx->kick_timer);
        }
        t->running_retry = 0;
    }
    if (t->trbs) {
        g_free(t->trbs);
    }

    t->trbs = NULL;
    t->trb_count = t->trb_alloced = 0;

    return killed;
}

static int xhci_ep_nuke_xfers(XHCIState *xhci, unsigned int slotid,
                               unsigned int epid)
{
    XHCISlot *slot;
    XHCIEPContext *epctx;
    int i, xferi, killed = 0;
    assert(slotid >= 1 && slotid <= xhci->numslots);
    assert(epid >= 1 && epid <= 31);

    DPRINTF("xhci_ep_nuke_xfers(%d, %d)\n", slotid, epid);

    slot = &xhci->slots[slotid-1];

    if (!slot->eps[epid-1]) {
        return 0;
    }

    epctx = slot->eps[epid-1];

    xferi = epctx->next_xfer;
    for (i = 0; i < TD_QUEUE; i++) {
        killed += xhci_ep_nuke_one_xfer(&epctx->transfers[xferi]);
        xferi = (xferi + 1) % TD_QUEUE;
    }
    return killed;
}

static TRBCCode xhci_disable_ep(XHCIState *xhci, unsigned int slotid,
                               unsigned int epid)
{
    XHCISlot *slot;
    XHCIEPContext *epctx;

    trace_usb_xhci_ep_disable(slotid, epid);
    assert(slotid >= 1 && slotid <= xhci->numslots);
    assert(epid >= 1 && epid <= 31);

    slot = &xhci->slots[slotid-1];

    if (!slot->eps[epid-1]) {
        DPRINTF("xhci: slot %d ep %d already disabled\n", slotid, epid);
        return CC_SUCCESS;
    }

    xhci_ep_nuke_xfers(xhci, slotid, epid);

    epctx = slot->eps[epid-1];

    xhci_set_ep_state(xhci, epctx, EP_DISABLED);

    qemu_free_timer(epctx->kick_timer);
    g_free(epctx);
    slot->eps[epid-1] = NULL;

    return CC_SUCCESS;
}

static TRBCCode xhci_stop_ep(XHCIState *xhci, unsigned int slotid,
                             unsigned int epid)
{
    XHCISlot *slot;
    XHCIEPContext *epctx;

    trace_usb_xhci_ep_stop(slotid, epid);
    assert(slotid >= 1 && slotid <= xhci->numslots);

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

    trace_usb_xhci_ep_reset(slotid, epid);
    assert(slotid >= 1 && slotid <= xhci->numslots);

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

    dev = xhci->slots[slotid-1].uport->dev;
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
    dma_addr_t dequeue;

    assert(slotid >= 1 && slotid <= xhci->numslots);

    if (epid < 1 || epid > 31) {
        fprintf(stderr, "xhci: bad ep %d\n", epid);
        return CC_TRB_ERROR;
    }

    trace_usb_xhci_ep_set_dequeue(slotid, epid, pdequeue);
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

static int xhci_xfer_create_sgl(XHCITransfer *xfer, int in_xfer)
{
    XHCIState *xhci = xfer->xhci;
    int i;

    xfer->int_req = false;
    pci_dma_sglist_init(&xfer->sgl, &xhci->pci_dev, xfer->trb_count);
    for (i = 0; i < xfer->trb_count; i++) {
        XHCITRB *trb = &xfer->trbs[i];
        dma_addr_t addr;
        unsigned int chunk = 0;

        if (trb->control & TRB_TR_IOC) {
            xfer->int_req = true;
        }

        switch (TRB_TYPE(*trb)) {
        case TR_DATA:
            if ((!(trb->control & TRB_TR_DIR)) != (!in_xfer)) {
                fprintf(stderr, "xhci: data direction mismatch for TR_DATA\n");
                goto err;
            }
            /* fallthrough */
        case TR_NORMAL:
        case TR_ISOCH:
            addr = xhci_mask64(trb->parameter);
            chunk = trb->status & 0x1ffff;
            if (trb->control & TRB_TR_IDT) {
                if (chunk > 8 || in_xfer) {
                    fprintf(stderr, "xhci: invalid immediate data TRB\n");
                    goto err;
                }
                qemu_sglist_add(&xfer->sgl, trb->addr, chunk);
            } else {
                qemu_sglist_add(&xfer->sgl, addr, chunk);
            }
            break;
        }
    }

    return 0;

err:
    qemu_sglist_destroy(&xfer->sgl);
    xhci_die(xhci);
    return -1;
}

static void xhci_xfer_unmap(XHCITransfer *xfer)
{
    usb_packet_unmap(&xfer->packet, &xfer->sgl);
    qemu_sglist_destroy(&xfer->sgl);
}

static void xhci_xfer_report(XHCITransfer *xfer)
{
    uint32_t edtla = 0;
    unsigned int left;
    bool reported = 0;
    bool shortpkt = 0;
    XHCIEvent event = {ER_TRANSFER, CC_SUCCESS};
    XHCIState *xhci = xfer->xhci;
    int i;

    left = xfer->packet.result < 0 ? 0 : xfer->packet.result;

    for (i = 0; i < xfer->trb_count; i++) {
        XHCITRB *trb = &xfer->trbs[i];
        unsigned int chunk = 0;

        switch (TRB_TYPE(*trb)) {
        case TR_DATA:
        case TR_NORMAL:
        case TR_ISOCH:
            chunk = trb->status & 0x1ffff;
            if (chunk > left) {
                chunk = left;
                if (xfer->status == CC_SUCCESS) {
                    shortpkt = 1;
                }
            }
            left -= chunk;
            edtla += chunk;
            break;
        case TR_STATUS:
            reported = 0;
            shortpkt = 0;
            break;
        }

        if (!reported && ((trb->control & TRB_TR_IOC) ||
                          (shortpkt && (trb->control & TRB_TR_ISP)) ||
                          (xfer->status != CC_SUCCESS))) {
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
            xhci_event(xhci, &event, TRB_INTR(*trb));
            reported = 1;
            if (xfer->status != CC_SUCCESS) {
                return;
            }
        }
    }
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
    DPRINTF("xhci: will continue at "DMA_ADDR_FMT"\n", epctx->ring.dequeue);
}

static int xhci_submit(XHCIState *xhci, XHCITransfer *xfer,
                       XHCIEPContext *epctx);

static int xhci_setup_packet(XHCITransfer *xfer)
{
    XHCIState *xhci = xfer->xhci;
    USBDevice *dev;
    USBEndpoint *ep;
    int dir;

    dir = xfer->in_xfer ? USB_TOKEN_IN : USB_TOKEN_OUT;

    if (xfer->packet.ep) {
        ep = xfer->packet.ep;
        dev = ep->dev;
    } else {
        if (!xhci->slots[xfer->slotid-1].uport) {
            fprintf(stderr, "xhci: slot %d has no device\n",
                    xfer->slotid);
            return -1;
        }
        dev = xhci->slots[xfer->slotid-1].uport->dev;
        ep = usb_ep_get(dev, dir, xfer->epid >> 1);
    }

    xhci_xfer_create_sgl(xfer, dir == USB_TOKEN_IN); /* Also sets int_req */
    usb_packet_setup(&xfer->packet, dir, ep, xfer->trbs[0].addr, false,
                     xfer->int_req);
    usb_packet_map(&xfer->packet, &xfer->sgl);
    DPRINTF("xhci: setup packet pid 0x%x addr %d ep %d\n",
            xfer->packet.pid, dev->addr, ep->nr);
    return 0;
}

static int xhci_complete_packet(XHCITransfer *xfer, int ret)
{
    if (ret == USB_RET_ASYNC) {
        trace_usb_xhci_xfer_async(xfer);
        xfer->running_async = 1;
        xfer->running_retry = 0;
        xfer->complete = 0;
        xfer->cancelled = 0;
        return 0;
    } else if (ret == USB_RET_NAK) {
        trace_usb_xhci_xfer_nak(xfer);
        xfer->running_async = 0;
        xfer->running_retry = 1;
        xfer->complete = 0;
        xfer->cancelled = 0;
        return 0;
    } else {
        xfer->running_async = 0;
        xfer->running_retry = 0;
        xfer->complete = 1;
        xhci_xfer_unmap(xfer);
    }

    if (ret >= 0) {
        trace_usb_xhci_xfer_success(xfer, ret);
        xfer->status = CC_SUCCESS;
        xhci_xfer_report(xfer);
        return 0;
    }

    /* error */
    trace_usb_xhci_xfer_error(xfer, ret);
    switch (ret) {
    case USB_RET_NODEV:
        xfer->status = CC_USB_TRANSACTION_ERROR;
        xhci_xfer_report(xfer);
        xhci_stall_ep(xfer);
        break;
    case USB_RET_STALL:
        xfer->status = CC_STALL_ERROR;
        xhci_xfer_report(xfer);
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
    uint8_t bmRequestType;
    int ret;

    trb_setup = &xfer->trbs[0];
    trb_status = &xfer->trbs[xfer->trb_count-1];

    trace_usb_xhci_xfer_start(xfer, xfer->slotid, xfer->epid);

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

    xfer->in_xfer = bmRequestType & USB_DIR_IN;
    xfer->iso_xfer = false;

    if (xhci_setup_packet(xfer) < 0) {
        return -1;
    }
    xfer->packet.parameter = trb_setup->parameter;

    ret = usb_handle_packet(xfer->packet.ep->dev, &xfer->packet);

    xhci_complete_packet(xfer, ret);
    if (!xfer->running_async && !xfer->running_retry) {
        xhci_kick_ep(xhci, xfer->slotid, xfer->epid);
    }
    return 0;
}

static void xhci_calc_iso_kick(XHCIState *xhci, XHCITransfer *xfer,
                               XHCIEPContext *epctx, uint64_t mfindex)
{
    if (xfer->trbs[0].control & TRB_TR_SIA) {
        uint64_t asap = ((mfindex + epctx->interval - 1) &
                         ~(epctx->interval-1));
        if (asap >= epctx->mfindex_last &&
            asap <= epctx->mfindex_last + epctx->interval * 4) {
            xfer->mfindex_kick = epctx->mfindex_last + epctx->interval;
        } else {
            xfer->mfindex_kick = asap;
        }
    } else {
        xfer->mfindex_kick = (xfer->trbs[0].control >> TRB_TR_FRAMEID_SHIFT)
            & TRB_TR_FRAMEID_MASK;
        xfer->mfindex_kick |= mfindex & ~0x3fff;
        if (xfer->mfindex_kick < mfindex) {
            xfer->mfindex_kick += 0x4000;
        }
    }
}

static void xhci_check_iso_kick(XHCIState *xhci, XHCITransfer *xfer,
                                XHCIEPContext *epctx, uint64_t mfindex)
{
    if (xfer->mfindex_kick > mfindex) {
        qemu_mod_timer(epctx->kick_timer, qemu_get_clock_ns(vm_clock) +
                       (xfer->mfindex_kick - mfindex) * 125000);
        xfer->running_retry = 1;
    } else {
        epctx->mfindex_last = xfer->mfindex_kick;
        qemu_del_timer(epctx->kick_timer);
        xfer->running_retry = 0;
    }
}


static int xhci_submit(XHCIState *xhci, XHCITransfer *xfer, XHCIEPContext *epctx)
{
    uint64_t mfindex;
    int ret;

    DPRINTF("xhci_submit(slotid=%d,epid=%d)\n", xfer->slotid, xfer->epid);

    xfer->in_xfer = epctx->type>>2;

    switch(epctx->type) {
    case ET_INTR_OUT:
    case ET_INTR_IN:
    case ET_BULK_OUT:
    case ET_BULK_IN:
        xfer->pkts = 0;
        xfer->iso_xfer = false;
        break;
    case ET_ISO_OUT:
    case ET_ISO_IN:
        xfer->pkts = 1;
        xfer->iso_xfer = true;
        mfindex = xhci_mfindex_get(xhci);
        xhci_calc_iso_kick(xhci, xfer, epctx, mfindex);
        xhci_check_iso_kick(xhci, xfer, epctx, mfindex);
        if (xfer->running_retry) {
            return -1;
        }
        break;
    default:
        fprintf(stderr, "xhci: unknown or unhandled EP "
                "(type %d, in %d, ep %02x)\n",
                epctx->type, xfer->in_xfer, xfer->epid);
        return -1;
    }

    if (xhci_setup_packet(xfer) < 0) {
        return -1;
    }
    ret = usb_handle_packet(xfer->packet.ep->dev, &xfer->packet);

    xhci_complete_packet(xfer, ret);
    if (!xfer->running_async && !xfer->running_retry) {
        xhci_kick_ep(xhci, xfer->slotid, xfer->epid);
    }
    return 0;
}

static int xhci_fire_transfer(XHCIState *xhci, XHCITransfer *xfer, XHCIEPContext *epctx)
{
    trace_usb_xhci_xfer_start(xfer, xfer->slotid, xfer->epid);
    return xhci_submit(xhci, xfer, epctx);
}

static void xhci_kick_ep(XHCIState *xhci, unsigned int slotid, unsigned int epid)
{
    XHCIEPContext *epctx;
    USBEndpoint *ep = NULL;
    uint64_t mfindex;
    int length;
    int i;

    trace_usb_xhci_ep_kick(slotid, epid);
    assert(slotid >= 1 && slotid <= xhci->numslots);
    assert(epid >= 1 && epid <= 31);

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

    if (epctx->retry) {
        XHCITransfer *xfer = epctx->retry;
        int result;

        trace_usb_xhci_xfer_retry(xfer);
        assert(xfer->running_retry);
        if (xfer->iso_xfer) {
            /* retry delayed iso transfer */
            mfindex = xhci_mfindex_get(xhci);
            xhci_check_iso_kick(xhci, xfer, epctx, mfindex);
            if (xfer->running_retry) {
                return;
            }
            if (xhci_setup_packet(xfer) < 0) {
                return;
            }
            result = usb_handle_packet(xfer->packet.ep->dev, &xfer->packet);
            assert(result != USB_RET_NAK);
            xhci_complete_packet(xfer, result);
        } else {
            /* retry nak'ed transfer */
            if (xhci_setup_packet(xfer) < 0) {
                return;
            }
            result = usb_handle_packet(xfer->packet.ep->dev, &xfer->packet);
            if (result == USB_RET_NAK) {
                return;
            }
            xhci_complete_packet(xfer, result);
        }
        assert(!xfer->running_retry);
        epctx->retry = NULL;
    }

    if (epctx->state == EP_HALTED) {
        DPRINTF("xhci: ep halted, not running schedule\n");
        return;
    }

    xhci_set_ep_state(xhci, epctx, EP_RUNNING);

    while (1) {
        XHCITransfer *xfer = &epctx->transfers[epctx->next_xfer];
        if (xfer->running_async || xfer->running_retry) {
            break;
        }
        length = xhci_ring_chain_length(xhci, &epctx->ring);
        if (length < 0) {
            break;
        } else if (length == 0) {
            break;
        }
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
                ep = xfer->packet.ep;
            } else {
                fprintf(stderr, "xhci: error firing CTL transfer\n");
            }
        } else {
            if (xhci_fire_transfer(xhci, xfer, epctx) >= 0) {
                epctx->next_xfer = (epctx->next_xfer + 1) % TD_QUEUE;
                ep = xfer->packet.ep;
            } else {
                if (!xfer->iso_xfer) {
                    fprintf(stderr, "xhci: error firing data transfer\n");
                }
            }
        }

        if (epctx->state == EP_HALTED) {
            break;
        }
        if (xfer->running_retry) {
            DPRINTF("xhci: xfer nacked, stopping schedule\n");
            epctx->retry = xfer;
            break;
        }
    }
    if (ep) {
        usb_device_flush_ep_queue(ep->dev, ep);
    }
}

static TRBCCode xhci_enable_slot(XHCIState *xhci, unsigned int slotid)
{
    trace_usb_xhci_slot_enable(slotid);
    assert(slotid >= 1 && slotid <= xhci->numslots);
    xhci->slots[slotid-1].enabled = 1;
    xhci->slots[slotid-1].uport = NULL;
    memset(xhci->slots[slotid-1].eps, 0, sizeof(XHCIEPContext*)*31);

    return CC_SUCCESS;
}

static TRBCCode xhci_disable_slot(XHCIState *xhci, unsigned int slotid)
{
    int i;

    trace_usb_xhci_slot_disable(slotid);
    assert(slotid >= 1 && slotid <= xhci->numslots);

    for (i = 1; i <= 31; i++) {
        if (xhci->slots[slotid-1].eps[i-1]) {
            xhci_disable_ep(xhci, slotid, i);
        }
    }

    xhci->slots[slotid-1].enabled = 0;
    return CC_SUCCESS;
}

static USBPort *xhci_lookup_uport(XHCIState *xhci, uint32_t *slot_ctx)
{
    USBPort *uport;
    char path[32];
    int i, pos, port;

    port = (slot_ctx[1]>>16) & 0xFF;
    port = xhci->ports[port-1].uport->index+1;
    pos = snprintf(path, sizeof(path), "%d", port);
    for (i = 0; i < 5; i++) {
        port = (slot_ctx[0] >> 4*i) & 0x0f;
        if (!port) {
            break;
        }
        pos += snprintf(path + pos, sizeof(path) - pos, ".%d", port);
    }

    QTAILQ_FOREACH(uport, &xhci->bus.used, next) {
        if (strcmp(uport->path, path) == 0) {
            return uport;
        }
    }
    return NULL;
}

static TRBCCode xhci_address_slot(XHCIState *xhci, unsigned int slotid,
                                  uint64_t pictx, bool bsr)
{
    XHCISlot *slot;
    USBPort *uport;
    USBDevice *dev;
    dma_addr_t ictx, octx, dcbaap;
    uint64_t poctx;
    uint32_t ictl_ctx[2];
    uint32_t slot_ctx[4];
    uint32_t ep0_ctx[5];
    int i;
    TRBCCode res;

    trace_usb_xhci_slot_address(slotid);
    assert(slotid >= 1 && slotid <= xhci->numslots);

    dcbaap = xhci_addr64(xhci->dcbaap_low, xhci->dcbaap_high);
    pci_dma_read(&xhci->pci_dev, dcbaap + 8*slotid, &poctx, sizeof(poctx));
    ictx = xhci_mask64(pictx);
    octx = xhci_mask64(le64_to_cpu(poctx));

    DPRINTF("xhci: input context at "DMA_ADDR_FMT"\n", ictx);
    DPRINTF("xhci: output context at "DMA_ADDR_FMT"\n", octx);

    pci_dma_read(&xhci->pci_dev, ictx, ictl_ctx, sizeof(ictl_ctx));

    if (ictl_ctx[0] != 0x0 || ictl_ctx[1] != 0x3) {
        fprintf(stderr, "xhci: invalid input context control %08x %08x\n",
                ictl_ctx[0], ictl_ctx[1]);
        return CC_TRB_ERROR;
    }

    pci_dma_read(&xhci->pci_dev, ictx+32, slot_ctx, sizeof(slot_ctx));
    pci_dma_read(&xhci->pci_dev, ictx+64, ep0_ctx, sizeof(ep0_ctx));

    DPRINTF("xhci: input slot context: %08x %08x %08x %08x\n",
            slot_ctx[0], slot_ctx[1], slot_ctx[2], slot_ctx[3]);

    DPRINTF("xhci: input ep0 context: %08x %08x %08x %08x %08x\n",
            ep0_ctx[0], ep0_ctx[1], ep0_ctx[2], ep0_ctx[3], ep0_ctx[4]);

    uport = xhci_lookup_uport(xhci, slot_ctx);
    if (uport == NULL) {
        fprintf(stderr, "xhci: port not found\n");
        return CC_TRB_ERROR;
    }

    dev = uport->dev;
    if (!dev) {
        fprintf(stderr, "xhci: port %s not connected\n", uport->path);
        return CC_USB_TRANSACTION_ERROR;
    }

    for (i = 0; i < xhci->numslots; i++) {
        if (xhci->slots[i].uport == uport) {
            fprintf(stderr, "xhci: port %s already assigned to slot %d\n",
                    uport->path, i+1);
            return CC_TRB_ERROR;
        }
    }

    slot = &xhci->slots[slotid-1];
    slot->uport = uport;
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

    pci_dma_write(&xhci->pci_dev, octx, slot_ctx, sizeof(slot_ctx));
    pci_dma_write(&xhci->pci_dev, octx+32, ep0_ctx, sizeof(ep0_ctx));

    return res;
}


static TRBCCode xhci_configure_slot(XHCIState *xhci, unsigned int slotid,
                                  uint64_t pictx, bool dc)
{
    dma_addr_t ictx, octx;
    uint32_t ictl_ctx[2];
    uint32_t slot_ctx[4];
    uint32_t islot_ctx[4];
    uint32_t ep_ctx[5];
    int i;
    TRBCCode res;

    trace_usb_xhci_slot_configure(slotid);
    assert(slotid >= 1 && slotid <= xhci->numslots);

    ictx = xhci_mask64(pictx);
    octx = xhci->slots[slotid-1].ctx;

    DPRINTF("xhci: input context at "DMA_ADDR_FMT"\n", ictx);
    DPRINTF("xhci: output context at "DMA_ADDR_FMT"\n", octx);

    if (dc) {
        for (i = 2; i <= 31; i++) {
            if (xhci->slots[slotid-1].eps[i-1]) {
                xhci_disable_ep(xhci, slotid, i);
            }
        }

        pci_dma_read(&xhci->pci_dev, octx, slot_ctx, sizeof(slot_ctx));
        slot_ctx[3] &= ~(SLOT_STATE_MASK << SLOT_STATE_SHIFT);
        slot_ctx[3] |= SLOT_ADDRESSED << SLOT_STATE_SHIFT;
        DPRINTF("xhci: output slot context: %08x %08x %08x %08x\n",
                slot_ctx[0], slot_ctx[1], slot_ctx[2], slot_ctx[3]);
        pci_dma_write(&xhci->pci_dev, octx, slot_ctx, sizeof(slot_ctx));

        return CC_SUCCESS;
    }

    pci_dma_read(&xhci->pci_dev, ictx, ictl_ctx, sizeof(ictl_ctx));

    if ((ictl_ctx[0] & 0x3) != 0x0 || (ictl_ctx[1] & 0x3) != 0x1) {
        fprintf(stderr, "xhci: invalid input context control %08x %08x\n",
                ictl_ctx[0], ictl_ctx[1]);
        return CC_TRB_ERROR;
    }

    pci_dma_read(&xhci->pci_dev, ictx+32, islot_ctx, sizeof(islot_ctx));
    pci_dma_read(&xhci->pci_dev, octx, slot_ctx, sizeof(slot_ctx));

    if (SLOT_STATE(slot_ctx[3]) < SLOT_ADDRESSED) {
        fprintf(stderr, "xhci: invalid slot state %08x\n", slot_ctx[3]);
        return CC_CONTEXT_STATE_ERROR;
    }

    for (i = 2; i <= 31; i++) {
        if (ictl_ctx[0] & (1<<i)) {
            xhci_disable_ep(xhci, slotid, i);
        }
        if (ictl_ctx[1] & (1<<i)) {
            pci_dma_read(&xhci->pci_dev, ictx+32+(32*i), ep_ctx,
                         sizeof(ep_ctx));
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
            pci_dma_write(&xhci->pci_dev, octx+(32*i), ep_ctx, sizeof(ep_ctx));
        }
    }

    slot_ctx[3] &= ~(SLOT_STATE_MASK << SLOT_STATE_SHIFT);
    slot_ctx[3] |= SLOT_CONFIGURED << SLOT_STATE_SHIFT;
    slot_ctx[0] &= ~(SLOT_CONTEXT_ENTRIES_MASK << SLOT_CONTEXT_ENTRIES_SHIFT);
    slot_ctx[0] |= islot_ctx[0] & (SLOT_CONTEXT_ENTRIES_MASK <<
                                   SLOT_CONTEXT_ENTRIES_SHIFT);
    DPRINTF("xhci: output slot context: %08x %08x %08x %08x\n",
            slot_ctx[0], slot_ctx[1], slot_ctx[2], slot_ctx[3]);

    pci_dma_write(&xhci->pci_dev, octx, slot_ctx, sizeof(slot_ctx));

    return CC_SUCCESS;
}


static TRBCCode xhci_evaluate_slot(XHCIState *xhci, unsigned int slotid,
                                   uint64_t pictx)
{
    dma_addr_t ictx, octx;
    uint32_t ictl_ctx[2];
    uint32_t iep0_ctx[5];
    uint32_t ep0_ctx[5];
    uint32_t islot_ctx[4];
    uint32_t slot_ctx[4];

    trace_usb_xhci_slot_evaluate(slotid);
    assert(slotid >= 1 && slotid <= xhci->numslots);

    ictx = xhci_mask64(pictx);
    octx = xhci->slots[slotid-1].ctx;

    DPRINTF("xhci: input context at "DMA_ADDR_FMT"\n", ictx);
    DPRINTF("xhci: output context at "DMA_ADDR_FMT"\n", octx);

    pci_dma_read(&xhci->pci_dev, ictx, ictl_ctx, sizeof(ictl_ctx));

    if (ictl_ctx[0] != 0x0 || ictl_ctx[1] & ~0x3) {
        fprintf(stderr, "xhci: invalid input context control %08x %08x\n",
                ictl_ctx[0], ictl_ctx[1]);
        return CC_TRB_ERROR;
    }

    if (ictl_ctx[1] & 0x1) {
        pci_dma_read(&xhci->pci_dev, ictx+32, islot_ctx, sizeof(islot_ctx));

        DPRINTF("xhci: input slot context: %08x %08x %08x %08x\n",
                islot_ctx[0], islot_ctx[1], islot_ctx[2], islot_ctx[3]);

        pci_dma_read(&xhci->pci_dev, octx, slot_ctx, sizeof(slot_ctx));

        slot_ctx[1] &= ~0xFFFF; /* max exit latency */
        slot_ctx[1] |= islot_ctx[1] & 0xFFFF;
        slot_ctx[2] &= ~0xFF00000; /* interrupter target */
        slot_ctx[2] |= islot_ctx[2] & 0xFF000000;

        DPRINTF("xhci: output slot context: %08x %08x %08x %08x\n",
                slot_ctx[0], slot_ctx[1], slot_ctx[2], slot_ctx[3]);

        pci_dma_write(&xhci->pci_dev, octx, slot_ctx, sizeof(slot_ctx));
    }

    if (ictl_ctx[1] & 0x2) {
        pci_dma_read(&xhci->pci_dev, ictx+64, iep0_ctx, sizeof(iep0_ctx));

        DPRINTF("xhci: input ep0 context: %08x %08x %08x %08x %08x\n",
                iep0_ctx[0], iep0_ctx[1], iep0_ctx[2],
                iep0_ctx[3], iep0_ctx[4]);

        pci_dma_read(&xhci->pci_dev, octx+32, ep0_ctx, sizeof(ep0_ctx));

        ep0_ctx[1] &= ~0xFFFF0000; /* max packet size*/
        ep0_ctx[1] |= iep0_ctx[1] & 0xFFFF0000;

        DPRINTF("xhci: output ep0 context: %08x %08x %08x %08x %08x\n",
                ep0_ctx[0], ep0_ctx[1], ep0_ctx[2], ep0_ctx[3], ep0_ctx[4]);

        pci_dma_write(&xhci->pci_dev, octx+32, ep0_ctx, sizeof(ep0_ctx));
    }

    return CC_SUCCESS;
}

static TRBCCode xhci_reset_slot(XHCIState *xhci, unsigned int slotid)
{
    uint32_t slot_ctx[4];
    dma_addr_t octx;
    int i;

    trace_usb_xhci_slot_reset(slotid);
    assert(slotid >= 1 && slotid <= xhci->numslots);

    octx = xhci->slots[slotid-1].ctx;

    DPRINTF("xhci: output context at "DMA_ADDR_FMT"\n", octx);

    for (i = 2; i <= 31; i++) {
        if (xhci->slots[slotid-1].eps[i-1]) {
            xhci_disable_ep(xhci, slotid, i);
        }
    }

    pci_dma_read(&xhci->pci_dev, octx, slot_ctx, sizeof(slot_ctx));
    slot_ctx[3] &= ~(SLOT_STATE_MASK << SLOT_STATE_SHIFT);
    slot_ctx[3] |= SLOT_DEFAULT << SLOT_STATE_SHIFT;
    DPRINTF("xhci: output slot context: %08x %08x %08x %08x\n",
            slot_ctx[0], slot_ctx[1], slot_ctx[2], slot_ctx[3]);
    pci_dma_write(&xhci->pci_dev, octx, slot_ctx, sizeof(slot_ctx));

    return CC_SUCCESS;
}

static unsigned int xhci_get_slot(XHCIState *xhci, XHCIEvent *event, XHCITRB *trb)
{
    unsigned int slotid;
    slotid = (trb->control >> TRB_CR_SLOTID_SHIFT) & TRB_CR_SLOTID_MASK;
    if (slotid < 1 || slotid > xhci->numslots) {
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
    dma_addr_t ctx;
    uint8_t bw_ctx[xhci->numports+1];

    DPRINTF("xhci_get_port_bandwidth()\n");

    ctx = xhci_mask64(pctx);

    DPRINTF("xhci: bandwidth context at "DMA_ADDR_FMT"\n", ctx);

    /* TODO: actually implement real values here */
    bw_ctx[0] = 0;
    memset(&bw_ctx[1], 80, xhci->numports); /* 80% */
    pci_dma_write(&xhci->pci_dev, ctx, bw_ctx, sizeof(bw_ctx));

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

static void xhci_via_challenge(XHCIState *xhci, uint64_t addr)
{
    uint32_t buf[8];
    uint32_t obuf[8];
    dma_addr_t paddr = xhci_mask64(addr);

    pci_dma_read(&xhci->pci_dev, paddr, &buf, 32);

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

    pci_dma_write(&xhci->pci_dev, paddr, &obuf, 32);
}

static void xhci_process_commands(XHCIState *xhci)
{
    XHCITRB trb;
    TRBType type;
    XHCIEvent event = {ER_COMMAND_COMPLETE, CC_SUCCESS};
    dma_addr_t addr;
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
            for (i = 0; i < xhci->numslots; i++) {
                if (!xhci->slots[i].enabled) {
                    break;
                }
            }
            if (i >= xhci->numslots) {
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
            xhci_via_challenge(xhci, trb.parameter);
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
        xhci_event(xhci, &event, 0);
    }
}

static void xhci_update_port(XHCIState *xhci, XHCIPort *port, int is_detach)
{
    port->portsc = PORTSC_PP;
    if (port->uport->dev && port->uport->dev->attached && !is_detach &&
        (1 << port->uport->dev->speed) & port->speedmask) {
        port->portsc |= PORTSC_CCS;
        switch (port->uport->dev->speed) {
        case USB_SPEED_LOW:
            port->portsc |= PORTSC_SPEED_LOW;
            break;
        case USB_SPEED_FULL:
            port->portsc |= PORTSC_SPEED_FULL;
            break;
        case USB_SPEED_HIGH:
            port->portsc |= PORTSC_SPEED_HIGH;
            break;
        case USB_SPEED_SUPER:
            port->portsc |= PORTSC_SPEED_SUPER;
            break;
        }
    }

    if (xhci_running(xhci)) {
        port->portsc |= PORTSC_CSC;
        XHCIEvent ev = { ER_PORT_STATUS_CHANGE, CC_SUCCESS,
                         port->portnr << 24};
        xhci_event(xhci, &ev, 0);
        DPRINTF("xhci: port change event for port %d\n", port->portnr);
    }
}

static void xhci_reset(DeviceState *dev)
{
    XHCIState *xhci = DO_UPCAST(XHCIState, pci_dev.qdev, dev);
    int i;

    trace_usb_xhci_reset();
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

    for (i = 0; i < xhci->numslots; i++) {
        xhci_disable_slot(xhci, i+1);
    }

    for (i = 0; i < xhci->numports; i++) {
        xhci_update_port(xhci, xhci->ports + i, 0);
    }

    for (i = 0; i < xhci->numintrs; i++) {
        xhci->intr[i].iman = 0;
        xhci->intr[i].imod = 0;
        xhci->intr[i].erstsz = 0;
        xhci->intr[i].erstba_low = 0;
        xhci->intr[i].erstba_high = 0;
        xhci->intr[i].erdp_low = 0;
        xhci->intr[i].erdp_high = 0;
        xhci->intr[i].msix_used = 0;

        xhci->intr[i].er_ep_idx = 0;
        xhci->intr[i].er_pcs = 1;
        xhci->intr[i].er_full = 0;
        xhci->intr[i].ev_buffer_put = 0;
        xhci->intr[i].ev_buffer_get = 0;
    }

    xhci->mfindex_start = qemu_get_clock_ns(vm_clock);
    xhci_mfwrap_update(xhci);
}

static uint64_t xhci_cap_read(void *ptr, hwaddr reg, unsigned size)
{
    XHCIState *xhci = ptr;
    uint32_t ret;

    switch (reg) {
    case 0x00: /* HCIVERSION, CAPLENGTH */
        ret = 0x01000000 | LEN_CAP;
        break;
    case 0x04: /* HCSPARAMS 1 */
        ret = ((xhci->numports_2+xhci->numports_3)<<24)
            | (xhci->numintrs<<8) | xhci->numslots;
        break;
    case 0x08: /* HCSPARAMS 2 */
        ret = 0x0000000f;
        break;
    case 0x0c: /* HCSPARAMS 3 */
        ret = 0x00000000;
        break;
    case 0x10: /* HCCPARAMS */
        if (sizeof(dma_addr_t) == 4) {
            ret = 0x00081000;
        } else {
            ret = 0x00081001;
        }
        break;
    case 0x14: /* DBOFF */
        ret = OFF_DOORBELL;
        break;
    case 0x18: /* RTSOFF */
        ret = OFF_RUNTIME;
        break;

    /* extended capabilities */
    case 0x20: /* Supported Protocol:00 */
        ret = 0x02000402; /* USB 2.0 */
        break;
    case 0x24: /* Supported Protocol:04 */
        ret = 0x20425355; /* "USB " */
        break;
    case 0x28: /* Supported Protocol:08 */
        ret = 0x00000001 | (xhci->numports_2<<8);
        break;
    case 0x2c: /* Supported Protocol:0c */
        ret = 0x00000000; /* reserved */
        break;
    case 0x30: /* Supported Protocol:00 */
        ret = 0x03000002; /* USB 3.0 */
        break;
    case 0x34: /* Supported Protocol:04 */
        ret = 0x20425355; /* "USB " */
        break;
    case 0x38: /* Supported Protocol:08 */
        ret = 0x00000000 | (xhci->numports_2+1) | (xhci->numports_3<<8);
        break;
    case 0x3c: /* Supported Protocol:0c */
        ret = 0x00000000; /* reserved */
        break;
    default:
        fprintf(stderr, "xhci_cap_read: reg %d unimplemented\n", (int)reg);
        ret = 0;
    }

    trace_usb_xhci_cap_read(reg, ret);
    return ret;
}

static uint64_t xhci_port_read(void *ptr, hwaddr reg, unsigned size)
{
    XHCIPort *port = ptr;
    uint32_t ret;

    switch (reg) {
    case 0x00: /* PORTSC */
        ret = port->portsc;
        break;
    case 0x04: /* PORTPMSC */
    case 0x08: /* PORTLI */
        ret = 0;
        break;
    case 0x0c: /* reserved */
    default:
        fprintf(stderr, "xhci_port_read (port %d): reg 0x%x unimplemented\n",
                port->portnr, (uint32_t)reg);
        ret = 0;
    }

    trace_usb_xhci_port_read(port->portnr, reg, ret);
    return ret;
}

static void xhci_port_write(void *ptr, hwaddr reg,
                            uint64_t val, unsigned size)
{
    XHCIPort *port = ptr;
    uint32_t portsc;

    trace_usb_xhci_port_write(port->portnr, reg, val);

    switch (reg) {
    case 0x00: /* PORTSC */
        portsc = port->portsc;
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
            usb_device_reset(port->uport->dev);
            portsc |= PORTSC_PRC | PORTSC_PED;
        }
        port->portsc = portsc;
        break;
    case 0x04: /* PORTPMSC */
    case 0x08: /* PORTLI */
    default:
        fprintf(stderr, "xhci_port_write (port %d): reg 0x%x unimplemented\n",
                port->portnr, (uint32_t)reg);
    }
}

static uint64_t xhci_oper_read(void *ptr, hwaddr reg, unsigned size)
{
    XHCIState *xhci = ptr;
    uint32_t ret;

    switch (reg) {
    case 0x00: /* USBCMD */
        ret = xhci->usbcmd;
        break;
    case 0x04: /* USBSTS */
        ret = xhci->usbsts;
        break;
    case 0x08: /* PAGESIZE */
        ret = 1; /* 4KiB */
        break;
    case 0x14: /* DNCTRL */
        ret = xhci->dnctrl;
        break;
    case 0x18: /* CRCR low */
        ret = xhci->crcr_low & ~0xe;
        break;
    case 0x1c: /* CRCR high */
        ret = xhci->crcr_high;
        break;
    case 0x30: /* DCBAAP low */
        ret = xhci->dcbaap_low;
        break;
    case 0x34: /* DCBAAP high */
        ret = xhci->dcbaap_high;
        break;
    case 0x38: /* CONFIG */
        ret = xhci->config;
        break;
    default:
        fprintf(stderr, "xhci_oper_read: reg 0x%x unimplemented\n", (int)reg);
        ret = 0;
    }

    trace_usb_xhci_oper_read(reg, ret);
    return ret;
}

static void xhci_oper_write(void *ptr, hwaddr reg,
                            uint64_t val, unsigned size)
{
    XHCIState *xhci = ptr;

    trace_usb_xhci_oper_write(reg, val);

    switch (reg) {
    case 0x00: /* USBCMD */
        if ((val & USBCMD_RS) && !(xhci->usbcmd & USBCMD_RS)) {
            xhci_run(xhci);
        } else if (!(val & USBCMD_RS) && (xhci->usbcmd & USBCMD_RS)) {
            xhci_stop(xhci);
        }
        xhci->usbcmd = val & 0xc0f;
        xhci_mfwrap_update(xhci);
        if (val & USBCMD_HCRST) {
            xhci_reset(&xhci->pci_dev.qdev);
        }
        xhci_intx_update(xhci);
        break;

    case 0x04: /* USBSTS */
        /* these bits are write-1-to-clear */
        xhci->usbsts &= ~(val & (USBSTS_HSE|USBSTS_EINT|USBSTS_PCD|USBSTS_SRE));
        xhci_intx_update(xhci);
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
            xhci_event(xhci, &event, 0);
            DPRINTF("xhci: command ring stopped (CRCR=%08x)\n", xhci->crcr_low);
        } else {
            dma_addr_t base = xhci_addr64(xhci->crcr_low & ~0x3f, val);
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
        fprintf(stderr, "xhci_oper_write: reg 0x%x unimplemented\n", (int)reg);
    }
}

static uint64_t xhci_runtime_read(void *ptr, hwaddr reg,
                                  unsigned size)
{
    XHCIState *xhci = ptr;
    uint32_t ret = 0;

    if (reg < 0x20) {
        switch (reg) {
        case 0x00: /* MFINDEX */
            ret = xhci_mfindex_get(xhci) & 0x3fff;
            break;
        default:
            fprintf(stderr, "xhci_runtime_read: reg 0x%x unimplemented\n",
                    (int)reg);
            break;
        }
    } else {
        int v = (reg - 0x20) / 0x20;
        XHCIInterrupter *intr = &xhci->intr[v];
        switch (reg & 0x1f) {
        case 0x00: /* IMAN */
            ret = intr->iman;
            break;
        case 0x04: /* IMOD */
            ret = intr->imod;
            break;
        case 0x08: /* ERSTSZ */
            ret = intr->erstsz;
            break;
        case 0x10: /* ERSTBA low */
            ret = intr->erstba_low;
            break;
        case 0x14: /* ERSTBA high */
            ret = intr->erstba_high;
            break;
        case 0x18: /* ERDP low */
            ret = intr->erdp_low;
            break;
        case 0x1c: /* ERDP high */
            ret = intr->erdp_high;
            break;
        }
    }

    trace_usb_xhci_runtime_read(reg, ret);
    return ret;
}

static void xhci_runtime_write(void *ptr, hwaddr reg,
                               uint64_t val, unsigned size)
{
    XHCIState *xhci = ptr;
    int v = (reg - 0x20) / 0x20;
    XHCIInterrupter *intr = &xhci->intr[v];
    trace_usb_xhci_runtime_write(reg, val);

    if (reg < 0x20) {
        fprintf(stderr, "%s: reg 0x%x unimplemented\n", __func__, (int)reg);
        return;
    }

    switch (reg & 0x1f) {
    case 0x00: /* IMAN */
        if (val & IMAN_IP) {
            intr->iman &= ~IMAN_IP;
        }
        intr->iman &= ~IMAN_IE;
        intr->iman |= val & IMAN_IE;
        if (v == 0) {
            xhci_intx_update(xhci);
        }
        xhci_msix_update(xhci, v);
        break;
    case 0x04: /* IMOD */
        intr->imod = val;
        break;
    case 0x08: /* ERSTSZ */
        intr->erstsz = val & 0xffff;
        break;
    case 0x10: /* ERSTBA low */
        /* XXX NEC driver bug: it doesn't align this to 64 bytes
        intr->erstba_low = val & 0xffffffc0; */
        intr->erstba_low = val & 0xfffffff0;
        break;
    case 0x14: /* ERSTBA high */
        intr->erstba_high = val;
        xhci_er_reset(xhci, v);
        break;
    case 0x18: /* ERDP low */
        if (val & ERDP_EHB) {
            intr->erdp_low &= ~ERDP_EHB;
        }
        intr->erdp_low = (val & ~ERDP_EHB) | (intr->erdp_low & ERDP_EHB);
        break;
    case 0x1c: /* ERDP high */
        intr->erdp_high = val;
        xhci_events_update(xhci, v);
        break;
    default:
        fprintf(stderr, "xhci_oper_write: reg 0x%x unimplemented\n",
                (int)reg);
    }
}

static uint64_t xhci_doorbell_read(void *ptr, hwaddr reg,
                                   unsigned size)
{
    /* doorbells always read as 0 */
    trace_usb_xhci_doorbell_read(reg, 0);
    return 0;
}

static void xhci_doorbell_write(void *ptr, hwaddr reg,
                                uint64_t val, unsigned size)
{
    XHCIState *xhci = ptr;

    trace_usb_xhci_doorbell_write(reg, val);

    if (!xhci_running(xhci)) {
        fprintf(stderr, "xhci: wrote doorbell while xHC stopped or paused\n");
        return;
    }

    reg >>= 2;

    if (reg == 0) {
        if (val == 0) {
            xhci_process_commands(xhci);
        } else {
            fprintf(stderr, "xhci: bad doorbell 0 write: 0x%x\n",
                    (uint32_t)val);
        }
    } else {
        if (reg > xhci->numslots) {
            fprintf(stderr, "xhci: bad doorbell %d\n", (int)reg);
        } else if (val > 31) {
            fprintf(stderr, "xhci: bad doorbell %d write: 0x%x\n",
                    (int)reg, (uint32_t)val);
        } else {
            xhci_kick_ep(xhci, reg, val);
        }
    }
}

static const MemoryRegionOps xhci_cap_ops = {
    .read = xhci_cap_read,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static const MemoryRegionOps xhci_oper_ops = {
    .read = xhci_oper_read,
    .write = xhci_oper_write,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static const MemoryRegionOps xhci_port_ops = {
    .read = xhci_port_read,
    .write = xhci_port_write,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static const MemoryRegionOps xhci_runtime_ops = {
    .read = xhci_runtime_read,
    .write = xhci_runtime_write,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static const MemoryRegionOps xhci_doorbell_ops = {
    .read = xhci_doorbell_read,
    .write = xhci_doorbell_write,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void xhci_attach(USBPort *usbport)
{
    XHCIState *xhci = usbport->opaque;
    XHCIPort *port = xhci_lookup_port(xhci, usbport);

    xhci_update_port(xhci, port, 0);
}

static void xhci_detach(USBPort *usbport)
{
    XHCIState *xhci = usbport->opaque;
    XHCIPort *port = xhci_lookup_port(xhci, usbport);

    xhci_update_port(xhci, port, 1);
}

static void xhci_wakeup(USBPort *usbport)
{
    XHCIState *xhci = usbport->opaque;
    XHCIPort *port = xhci_lookup_port(xhci, usbport);
    XHCIEvent ev = { ER_PORT_STATUS_CHANGE, CC_SUCCESS,
                     port->portnr << 24};
    uint32_t pls;

    pls = (port->portsc >> PORTSC_PLS_SHIFT) & PORTSC_PLS_MASK;
    if (pls != 3) {
        return;
    }
    port->portsc |= 0xf << PORTSC_PLS_SHIFT;
    if (port->portsc & PORTSC_PLC) {
        return;
    }
    port->portsc |= PORTSC_PLC;
    xhci_event(xhci, &ev, 0);
}

static void xhci_complete(USBPort *port, USBPacket *packet)
{
    XHCITransfer *xfer = container_of(packet, XHCITransfer, packet);

    if (packet->result == USB_RET_REMOVE_FROM_QUEUE) {
        xhci_ep_nuke_one_xfer(xfer);
        return;
    }
    xhci_complete_packet(xfer, packet->result);
    xhci_kick_ep(xfer->xhci, xfer->slotid, xfer->epid);
}

static void xhci_child_detach(USBPort *uport, USBDevice *child)
{
    USBBus *bus = usb_bus_from_device(child);
    XHCIState *xhci = container_of(bus, XHCIState, bus);
    int i;

    for (i = 0; i < xhci->numslots; i++) {
        if (xhci->slots[i].uport == uport) {
            xhci->slots[i].uport = NULL;
        }
    }
}

static USBPortOps xhci_uport_ops = {
    .attach   = xhci_attach,
    .detach   = xhci_detach,
    .wakeup   = xhci_wakeup,
    .complete = xhci_complete,
    .child_detach = xhci_child_detach,
};

static int xhci_find_slotid(XHCIState *xhci, USBDevice *dev)
{
    XHCISlot *slot;
    int slotid;

    for (slotid = 1; slotid <= xhci->numslots; slotid++) {
        slot = &xhci->slots[slotid-1];
        if (slot->devaddr == dev->addr) {
            return slotid;
        }
    }
    return 0;
}

static int xhci_find_epid(USBEndpoint *ep)
{
    if (ep->nr == 0) {
        return 1;
    }
    if (ep->pid == USB_TOKEN_IN) {
        return ep->nr * 2 + 1;
    } else {
        return ep->nr * 2;
    }
}

static void xhci_wakeup_endpoint(USBBus *bus, USBEndpoint *ep)
{
    XHCIState *xhci = container_of(bus, XHCIState, bus);
    int slotid;

    DPRINTF("%s\n", __func__);
    slotid = xhci_find_slotid(xhci, ep->dev);
    if (slotid == 0 || !xhci->slots[slotid-1].enabled) {
        DPRINTF("%s: oops, no slot for dev %d\n", __func__, ep->dev->addr);
        return;
    }
    xhci_kick_ep(xhci, slotid, xhci_find_epid(ep));
}

static USBBusOps xhci_bus_ops = {
    .wakeup_endpoint = xhci_wakeup_endpoint,
};

static void usb_xhci_init(XHCIState *xhci, DeviceState *dev)
{
    XHCIPort *port;
    int i, usbports, speedmask;

    xhci->usbsts = USBSTS_HCH;

    if (xhci->numports_2 > MAXPORTS_2) {
        xhci->numports_2 = MAXPORTS_2;
    }
    if (xhci->numports_3 > MAXPORTS_3) {
        xhci->numports_3 = MAXPORTS_3;
    }
    usbports = MAX(xhci->numports_2, xhci->numports_3);
    xhci->numports = xhci->numports_2 + xhci->numports_3;

    usb_bus_new(&xhci->bus, &xhci_bus_ops, &xhci->pci_dev.qdev);

    for (i = 0; i < usbports; i++) {
        speedmask = 0;
        if (i < xhci->numports_2) {
            port = &xhci->ports[i];
            port->portnr = i + 1;
            port->uport = &xhci->uports[i];
            port->speedmask =
                USB_SPEED_MASK_LOW  |
                USB_SPEED_MASK_FULL |
                USB_SPEED_MASK_HIGH;
            snprintf(port->name, sizeof(port->name), "usb2 port #%d", i+1);
            speedmask |= port->speedmask;
        }
        if (i < xhci->numports_3) {
            port = &xhci->ports[i + xhci->numports_2];
            port->portnr = i + 1 + xhci->numports_2;
            port->uport = &xhci->uports[i];
            port->speedmask = USB_SPEED_MASK_SUPER;
            snprintf(port->name, sizeof(port->name), "usb3 port #%d", i+1);
            speedmask |= port->speedmask;
        }
        usb_register_port(&xhci->bus, &xhci->uports[i], xhci, i,
                          &xhci_uport_ops, speedmask);
    }
}

static int usb_xhci_initfn(struct PCIDevice *dev)
{
    int i, ret;

    XHCIState *xhci = DO_UPCAST(XHCIState, pci_dev, dev);

    xhci->pci_dev.config[PCI_CLASS_PROG] = 0x30;    /* xHCI */
    xhci->pci_dev.config[PCI_INTERRUPT_PIN] = 0x01; /* interrupt pin 1 */
    xhci->pci_dev.config[PCI_CACHE_LINE_SIZE] = 0x10;
    xhci->pci_dev.config[0x60] = 0x30; /* release number */

    usb_xhci_init(xhci, &dev->qdev);

    if (xhci->numintrs > MAXINTRS) {
        xhci->numintrs = MAXINTRS;
    }
    if (xhci->numintrs < 1) {
        xhci->numintrs = 1;
    }
    if (xhci->numslots > MAXSLOTS) {
        xhci->numslots = MAXSLOTS;
    }
    if (xhci->numslots < 1) {
        xhci->numslots = 1;
    }

    xhci->mfwrap_timer = qemu_new_timer_ns(vm_clock, xhci_mfwrap_timer, xhci);

    xhci->irq = xhci->pci_dev.irq[0];

    memory_region_init(&xhci->mem, "xhci", LEN_REGS);
    memory_region_init_io(&xhci->mem_cap, &xhci_cap_ops, xhci,
                          "capabilities", LEN_CAP);
    memory_region_init_io(&xhci->mem_oper, &xhci_oper_ops, xhci,
                          "operational", 0x400);
    memory_region_init_io(&xhci->mem_runtime, &xhci_runtime_ops, xhci,
                          "runtime", LEN_RUNTIME);
    memory_region_init_io(&xhci->mem_doorbell, &xhci_doorbell_ops, xhci,
                          "doorbell", LEN_DOORBELL);

    memory_region_add_subregion(&xhci->mem, 0,            &xhci->mem_cap);
    memory_region_add_subregion(&xhci->mem, OFF_OPER,     &xhci->mem_oper);
    memory_region_add_subregion(&xhci->mem, OFF_RUNTIME,  &xhci->mem_runtime);
    memory_region_add_subregion(&xhci->mem, OFF_DOORBELL, &xhci->mem_doorbell);

    for (i = 0; i < xhci->numports; i++) {
        XHCIPort *port = &xhci->ports[i];
        uint32_t offset = OFF_OPER + 0x400 + 0x10 * i;
        port->xhci = xhci;
        memory_region_init_io(&port->mem, &xhci_port_ops, port,
                              port->name, 0x10);
        memory_region_add_subregion(&xhci->mem, offset, &port->mem);
    }

    pci_register_bar(&xhci->pci_dev, 0,
                     PCI_BASE_ADDRESS_SPACE_MEMORY|PCI_BASE_ADDRESS_MEM_TYPE_64,
                     &xhci->mem);

    ret = pcie_cap_init(&xhci->pci_dev, 0xa0, PCI_EXP_TYPE_ENDPOINT, 0);
    assert(ret >= 0);

    if (xhci->flags & (1 << XHCI_FLAG_USE_MSI)) {
        msi_init(&xhci->pci_dev, 0x70, xhci->numintrs, true, false);
    }
    if (xhci->flags & (1 << XHCI_FLAG_USE_MSI_X)) {
        msix_init(&xhci->pci_dev, xhci->numintrs,
                  &xhci->mem, 0, OFF_MSIX_TABLE,
                  &xhci->mem, 0, OFF_MSIX_PBA,
                  0x90);
    }

    return 0;
}

static const VMStateDescription vmstate_xhci = {
    .name = "xhci",
    .unmigratable = 1,
};

static Property xhci_properties[] = {
    DEFINE_PROP_BIT("msi",      XHCIState, flags, XHCI_FLAG_USE_MSI, true),
    DEFINE_PROP_BIT("msix",     XHCIState, flags, XHCI_FLAG_USE_MSI_X, true),
    DEFINE_PROP_UINT32("intrs", XHCIState, numintrs, MAXINTRS),
    DEFINE_PROP_UINT32("slots", XHCIState, numslots, MAXSLOTS),
    DEFINE_PROP_UINT32("p2",    XHCIState, numports_2, 4),
    DEFINE_PROP_UINT32("p3",    XHCIState, numports_3, 4),
    DEFINE_PROP_END_OF_LIST(),
};

static void xhci_class_init(ObjectClass *klass, void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd    = &vmstate_xhci;
    dc->props   = xhci_properties;
    dc->reset   = xhci_reset;
    k->init         = usb_xhci_initfn;
    k->vendor_id    = PCI_VENDOR_ID_NEC;
    k->device_id    = PCI_DEVICE_ID_NEC_UPD720200;
    k->class_id     = PCI_CLASS_SERIAL_USB;
    k->revision     = 0x03;
    k->is_express   = 1;
}

static TypeInfo xhci_info = {
    .name          = "nec-usb-xhci",
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(XHCIState),
    .class_init    = xhci_class_init,
};

static void xhci_register_types(void)
{
    type_register_static(&xhci_info);
}

type_init(xhci_register_types)
