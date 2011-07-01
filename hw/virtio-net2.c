#include <linux/virtio_pci.h>
#include <linux/virtio_net.h>
#include <linux/virtio_ring.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include "pci.h"

//#define DEBUG_VIRTIO_NET_2 1

#ifdef DEBUG_VIRTIO_NET_2
#define dprintf(fmt, ...) \
    do { fprintf(stderr, fmt "\n", ## __VA_ARGS__); } while (0)
#else
#define dprintf(fmt, ...) \
    do { } while (0)
#endif

#define PAGE_SIZE 4096
#define PAGE_SHIFT 12

#define mb() asm volatile("" ::: "memory")

typedef struct VirtioQueue2
{
    uint32_t pfn;
    uint16_t num;
    void *base;

    struct vring_desc *desc;
    struct vring_avail *avail;
    struct vring_used *used;

    uint16_t last_avail_idx;
} VirtioQueue2;

typedef struct VirtioNet2
{
    PCIDevice pci_dev;
    uint32_t host_features;
    uint32_t guest_features;
    uint16_t queue_sel;
    uint8_t status;
    uint8_t isr;
    
    pcibus_t bar0;
    void *base_ptr;

    char *ifname;
    int fd;

    int event_fd;

    VirtioQueue2 vq[3];
} VirtioNet2;

#ifdef DEBUG_VIRTIO_NET2
static const char *register_names[] = {
    [VIRTIO_PCI_HOST_FEATURES] = "HOST_FEATURES",
    [VIRTIO_PCI_GUEST_FEATURES] = "GUEST_FEATURES",
    [VIRTIO_PCI_QUEUE_PFN] = "QUEUE_PFN",
    [VIRTIO_PCI_QUEUE_NUM] = "QUEUE_NUM",
    [VIRTIO_PCI_QUEUE_SEL] = "QUEUE_SEL",
    [VIRTIO_PCI_QUEUE_NOTIFY] = "QUEUE_NOTIFY",
    [VIRTIO_PCI_STATUS] = "STATUS",
    [VIRTIO_PCI_ISR] = "ISR",
    [20 ... 26] = "CONFIG::MAC",
    [26] = "CONFIG::LINK",
};
#endif

static VirtioNet2 *to_vnet(PCIDevice *pci_dev)
{
    return container_of(pci_dev, VirtioNet2, pci_dev);
}

static void virtio_net_update_irq(VirtioNet2 *n)
{
    qemu_set_irq(n->pci_dev.irq[0], !!(n->isr & 1));
}

static void virtio_net_process_eventfd(void *opaque)
{
    VirtioNet2 *n = opaque;
    uint64_t count;
    ssize_t len;

    len = read(n->event_fd, &count, sizeof(count));
    assert(len != -1);

    if (count) {
        n->isr = 1;
        virtio_net_update_irq(n);
    }
}

static void virtio_net_kick(VirtioNet2 *n, VirtioQueue2 *vq)
{
    mb();
    if (!(vq->avail->flags & VRING_AVAIL_F_NO_INTERRUPT)) {
        uint64_t count = 1;
        ssize_t len;

        len = write(n->event_fd, &count, sizeof(count));
        assert(len != -1);
    }
}

static unsigned virtio_net_next_avail(VirtioNet2 *n, VirtioQueue2 *vq,
                                      struct iovec *iov, unsigned max_iov,
                                      unsigned *pin_num, unsigned *pout_num)
{
    unsigned head, next, in_num, out_num;
    struct vring_desc *desc;

    mb();
    if (vq->last_avail_idx == vq->avail->idx) {
        return vq->num;
    }

    head = vq->avail->ring[vq->last_avail_idx % vq->num];

    next = head;
    in_num = out_num = 0;

    do {
        struct iovec *sg = &iov[in_num + out_num];

        desc = &vq->desc[next];

        sg->iov_base = n->base_ptr + desc->addr;
        sg->iov_len = desc->len;

        if ((desc->flags & VRING_DESC_F_WRITE)) {
            out_num++;
        } else {
            in_num++;
        }

        next = desc->next;
    } while ((desc->flags & VRING_DESC_F_NEXT));

    *pin_num = in_num;
    *pout_num = out_num;

    vq->last_avail_idx++;
    mb();

    return head;
}

static void virtio_net_add_used(VirtioQueue2 *vq, unsigned head, uint32_t len)
{
    struct vring_used_elem *elem;

    elem = &vq->used->ring[vq->used->idx % vq->num];
    elem->id = head;
    elem->len = len;
    vq->used->idx++;
    mb();
}

static void *virtio_net_tx_thread(void *opaque)
{
    VirtioNet2 *n = opaque;
    VirtioQueue2 *vq = &n->vq[1];

    vq->used->flags |= VRING_USED_F_NO_NOTIFY;

    while (1) {
        struct iovec sg[vq->num];
        unsigned in_num, out_num;
        unsigned head;
        ssize_t len;

        in_num = out_num = 0;

        do {
            head = virtio_net_next_avail(n, vq, sg, vq->num, &in_num, &out_num);
            if (head == vq->num) {
            }
        } while (head == vq->num);

        dprintf("got tx packet %d %d %d", head, in_num, out_num);
        len = writev(n->fd, &sg[1], in_num - 1);
        assert(len != -1);
        dprintf("wrote %ld", len);

        virtio_net_add_used(vq, head, 0);
        virtio_net_kick(n, vq);
    }

    return NULL;
}

static void *virtio_net_rx_thread(void *opaque)
{
    VirtioNet2 *n = opaque;
    VirtioQueue2 *vq = &n->vq[0];

    vq->used->flags |= VRING_USED_F_NO_NOTIFY;

    while (1) {
        struct virtio_net_hdr *hdr;
        struct iovec sg[vq->num];
        unsigned in_num, out_num;
        unsigned head;
        ssize_t len;

        in_num = out_num = 0;

        do {
            head = virtio_net_next_avail(n, vq, sg, vq->num, &in_num, &out_num);
            if (head == vq->num) {
            }
        } while (head == vq->num);

        dprintf("got rx packet %d %d %d", head, in_num, out_num);
        hdr = sg[in_num].iov_base;
        hdr->flags = 0;
        len = readv(n->fd, &sg[in_num + 1], out_num - 1);
        assert(len != -1);
        dprintf("read %ld", len);

        virtio_net_add_used(vq, head, len + 10);
        virtio_net_kick(n, vq);
    }

    return NULL;
}

static uint32_t virtio_net_config_read(void *opaque, uint32_t addr, int size)
{
    VirtioNet2 *n = opaque;
    uint32_t value = 0;

    addr -= n->bar0;

    switch (addr) {
    case VIRTIO_PCI_HOST_FEATURES:
        value = n->host_features;
        break;
    case VIRTIO_PCI_GUEST_FEATURES:
        value = n->guest_features;
        break;
    case VIRTIO_PCI_QUEUE_PFN:
        value = n->vq[n->queue_sel].pfn;
        break;
    case VIRTIO_PCI_QUEUE_NUM:
        value = n->vq[n->queue_sel].num;
        break;
    case VIRTIO_PCI_QUEUE_SEL:
        value = n->queue_sel;
        break;
    case VIRTIO_PCI_QUEUE_NOTIFY:
        value = 0;
        break;
    case VIRTIO_PCI_STATUS:
        value = n->status;
        break;
    case VIRTIO_PCI_ISR:
        value = n->isr;
        n->isr = 0;
        if (value) {
            virtio_net_update_irq(n);
        }
        break;
    default:
        break;
    }

    dprintf("config_read(%p, %s, %d) = %x", n, register_names[addr], size, value);

    return value;
}

static void virtio_net_config_write(void *opaque, uint32_t addr, int size, uint32_t value)
{
    VirtioNet2 *n = opaque;

    addr -= n->bar0;

    switch (addr) {
    case VIRTIO_PCI_HOST_FEATURES:
        break;
    case VIRTIO_PCI_GUEST_FEATURES:
        n->guest_features = value;
        break; 
    case VIRTIO_PCI_QUEUE_PFN: {
        VirtioQueue2 *vq = &n->vq[n->queue_sel];

        vq->pfn = value;
        vq->base = n->base_ptr + (value << PAGE_SHIFT);

	vq->desc = vq->base;
	vq->avail = vq->base + vq->num * sizeof(struct vring_desc);
	vq->used = (void *)(((unsigned long)&vq->avail->ring[vq->num] + PAGE_SIZE - 1)
			    & ~(PAGE_SIZE - 1));
    }   break;
    case VIRTIO_PCI_QUEUE_NUM:
        break;
    case VIRTIO_PCI_QUEUE_SEL:
        n->queue_sel = value;
        break;
    case VIRTIO_PCI_QUEUE_NOTIFY:
        break;
    case VIRTIO_PCI_STATUS:
        n->status = value;
        if ((n->status & VIRTIO_CONFIG_S_DRIVER_OK)) {
            pthread_t tid_tx, tid_rx;

            n->event_fd = eventfd(0, 0);

            qemu_set_fd_handler(n->event_fd, virtio_net_process_eventfd, NULL, n);

            pthread_create(&tid_tx, NULL, virtio_net_tx_thread, n);
            pthread_create(&tid_rx, NULL, virtio_net_rx_thread, n);
        }
        break;
    case VIRTIO_PCI_ISR:
        break;
    default:
        break;
    }

    dprintf("config_write(%p, %s, %d, 0x%x)", n, register_names[addr], size, value);
}

static void virtio_net_config_writeb(void *opaque, uint32_t addr, uint32_t value)
{
    virtio_net_config_write(opaque, addr, 1, value);
}

static void virtio_net_config_writew(void *opaque, uint32_t addr, uint32_t value)
{
    virtio_net_config_write(opaque, addr, 2, value);
}

static void virtio_net_config_writel(void *opaque, uint32_t addr, uint32_t value)
{
    virtio_net_config_write(opaque, addr, 4, value);
}

static uint32_t virtio_net_config_readb(void *opaque, uint32_t addr)
{
    return virtio_net_config_read(opaque, addr, 1);
}

static uint32_t virtio_net_config_readw(void *opaque, uint32_t addr)
{
    return virtio_net_config_read(opaque, addr, 2);
}

static uint32_t virtio_net_config_readl(void *opaque, uint32_t addr)
{
    return virtio_net_config_read(opaque, addr, 4);
}

static void virtio_net_map(PCIDevice *pci_dev, int region_num,
                           pcibus_t addr, pcibus_t size, int type)
{
    VirtioNet2 *n = to_vnet(pci_dev);
    unsigned config_len = 32;

    n->bar0 = addr;

    register_ioport_write(addr, config_len, 1, virtio_net_config_writeb, n);
    register_ioport_write(addr, config_len, 2, virtio_net_config_writew, n);
    register_ioport_write(addr, config_len, 4, virtio_net_config_writel, n);
    register_ioport_read(addr, config_len, 1, virtio_net_config_readb, n);
    register_ioport_read(addr, config_len, 2, virtio_net_config_readw, n);
    register_ioport_read(addr, config_len, 4, virtio_net_config_readl, n);
}

static int virtio_net_init(PCIDevice *pci_dev)
{
    VirtioNet2 *n = to_vnet(pci_dev);
    unsigned size;
    struct ifreq ifr;
    int ret;

    pci_set_word(pci_dev->config + 0x2c, pci_get_word(pci_dev->config + PCI_VENDOR_ID));
    pci_set_word(pci_dev->config + 0x2e, 1);
    pci_dev->config[0x3d] = 1;

    n->base_ptr = qemu_get_ram_ptr(0); // Going to hell for this...

    size = 32;
    if (size & (size - 1)) {
        size = 1 << qemu_fls(size);
    }

    n->vq[0].num = 256;
    n->vq[1].num = 256;

    pci_register_bar(&n->pci_dev, 0, size, PCI_BASE_ADDRESS_SPACE_IO, virtio_net_map);

    n->fd = open("/dev/net/tun", O_RDWR);
    assert(n->fd != -1);

    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;

    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", n->ifname);

    ret = ioctl(n->fd, TUNSETIFF, &ifr);
    assert(ret != -1);

    return 0;
}

static int virtio_net_exit(PCIDevice *pci_dev)
{
    VirtioNet2 *n = to_vnet(pci_dev);

    (void)n;

    return 0;
}

static void virtio_net_reset(DeviceState *d)
{
}

static PCIDeviceInfo virtio_info = {
    .qdev.name = "virtio-net2",
    .qdev.size = sizeof(VirtioNet2),
    .init = virtio_net_init,
    .exit = virtio_net_exit,
    .vendor_id = PCI_VENDOR_ID_REDHAT_QUMRANET,
    .device_id = PCI_DEVICE_ID_VIRTIO_NET,
    .revision  = VIRTIO_PCI_ABI_VERSION,
    .class_id   = PCI_CLASS_NETWORK_ETHERNET,
    .qdev.props = (Property[]) {
        DEFINE_PROP_STRING("ifname", VirtioNet2, ifname),
        DEFINE_PROP_END_OF_LIST(),
    },
    .qdev.reset = virtio_net_reset,
};    

static void virtio_net_register_devices(void)
{
    pci_qdev_register(&virtio_info);
}

device_init(virtio_net_register_devices);
