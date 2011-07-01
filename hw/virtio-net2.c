#include <linux/virtio_pci.h>
#include <linux/virtio_net.h>
#include <linux/virtio_ring.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include "pci.h"

static void fd_wait_read(int fd)
{
    fd_set readfds;

    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);

    select(fd + 1, &readfds, NULL, NULL, NULL);
    assert(FD_ISSET(fd, &readfds));
}

static void fd_wait_write(int fd)
{
    fd_set fds;

    FD_ZERO(&fds);
    FD_SET(fd, &fds);

    select(fd + 1, NULL, &fds, NULL, NULL);
    assert(FD_ISSET(fd, &fds));
}

/* core virtio */

//#define DEBUG_VIRTIO_2 1

#ifdef DEBUG_VIRTIO_2
#define dprintf(fmt, ...) \
    do { fprintf(stderr, fmt "\n", ## __VA_ARGS__); } while (0)
#else
#define dprintf(fmt, ...) \
    do { } while (0)
#endif

#define PAGE_SIZE 4096
#define PAGE_SHIFT 12

#define mb() asm volatile("" ::: "memory")

typedef struct VirtioDevice2 VirtioDevice2;

typedef size_t (virtio_io_handler_t)(VirtioDevice2 *, struct iovec *, int);
typedef void (virtio_io_waiter_t)(VirtioDevice2 *);

typedef struct VirtioQueue2
{
    VirtioDevice2 *vdev;

    uint32_t pfn;
    uint16_t num;
    void *base;

    struct vring_desc *desc;
    struct vring_avail *avail;
    struct vring_used *used;

    uint16_t last_avail_idx;

    virtio_io_handler_t *handler;
    virtio_io_waiter_t *waiter;
} VirtioQueue2;

struct VirtioDevice2
{
    PCIDevice pci_dev;
    uint32_t host_features;
    uint32_t guest_features;
    uint16_t queue_sel;
    uint8_t status;
    uint8_t isr;
    
    pcibus_t bar0;
    void *base_ptr;

    int event_fd;

    int max_vq;
    VirtioQueue2 vq[3];
};

static VirtioDevice2 *to_vdev(PCIDevice *pci_dev)
{
    return container_of(pci_dev, VirtioDevice2, pci_dev);
}

static void virtio_update_irq(VirtioDevice2 *vdev)
{
    qemu_set_irq(vdev->pci_dev.irq[0], !!(vdev->isr & 1));
}

static void virtio_process_eventfd(void *opaque)
{
    VirtioDevice2 *vdev = opaque;
    uint64_t count;
    ssize_t len;

    len = read(vdev->event_fd, &count, sizeof(count));
    assert(len != -1);

    if (count) {
        vdev->isr = 1;
        virtio_update_irq(vdev);
    }
}

static void virtio_kick(VirtioDevice2 *vdev, VirtioQueue2 *vq)
{
    mb();
    if (!(vq->avail->flags & VRING_AVAIL_F_NO_INTERRUPT)) {
        uint64_t count = 1;
        ssize_t len;

        len = write(vdev->event_fd, &count, sizeof(count));
        assert(len != -1);
    }
}

static unsigned virtio_next_avail(VirtioDevice2 *vdev, VirtioQueue2 *vq,
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

        sg->iov_base = vdev->base_ptr + desc->addr;
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

    return head;
}

static void virtio_add_used(VirtioQueue2 *vq, unsigned head, uint32_t len)
{
    struct vring_used_elem *elem;

    elem = &vq->used->ring[vq->used->idx % vq->num];
    elem->id = head;
    elem->len = len;
    vq->used->idx++;
    mb();
}

static void *virtio_thread(void *opaque)
{
    VirtioQueue2 *vq = opaque;
    VirtioDevice2 *vdev = vq->vdev;

    vq->used->flags |= VRING_USED_F_NO_NOTIFY;

    while (1) {
        struct iovec sg[vq->num];
        unsigned in_num, out_num;
        unsigned head;
        ssize_t len;
        bool sent = false;

        do {
            in_num = out_num = 0;
            head = virtio_next_avail(vdev, vq, sg, vq->num, &in_num, &out_num);
        } while (head == vq->num);

        while (head != vq->num) {
            len = vq->handler(vdev, &sg[in_num], out_num - 1);
            if (len == -1 && errno == EAGAIN) {
                if (sent) {
                    virtio_kick(vdev, vq);
                }

                vq->waiter(vdev);
                len = vq->handler(vdev, &sg[in_num], out_num);
            }

            assert(len != -1);

            virtio_add_used(vq, head, len + 10);

            in_num = out_num = 0;
            head = virtio_next_avail(vdev, vq, sg, vq->num, &in_num, &out_num);
            sent = true;
        }
    }
}

#ifdef DEBUG_VIRTIO_2
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

static uint32_t virtio_config_read(void *opaque, uint32_t addr, int size)
{
    VirtioDevice2 *vdev = opaque;
    uint32_t value = 0;

    addr -= vdev->bar0;

    switch (addr) {
    case VIRTIO_PCI_HOST_FEATURES:
        value = vdev->host_features;
        break;
    case VIRTIO_PCI_GUEST_FEATURES:
        value = vdev->guest_features;
        break;
    case VIRTIO_PCI_QUEUE_PFN:
        value = vdev->vq[vdev->queue_sel].pfn;
        break;
    case VIRTIO_PCI_QUEUE_NUM:
        value = vdev->vq[vdev->queue_sel].num;
        break;
    case VIRTIO_PCI_QUEUE_SEL:
        value = vdev->queue_sel;
        break;
    case VIRTIO_PCI_QUEUE_NOTIFY:
        value = 0;
        break;
    case VIRTIO_PCI_STATUS:
        value = vdev->status;
        break;
    case VIRTIO_PCI_ISR:
        value = vdev->isr;
        vdev->isr = 0;
        if (value) {
            virtio_update_irq(vdev);
        }
        break;
    default:
        break;
    }

    dprintf("config_read(%p, %s, %d) = %x", vdev, register_names[addr], size, value);

    return value;
}

static void virtio_config_write(void *opaque, uint32_t addr, int size, uint32_t value)
{
    VirtioDevice2 *vdev = opaque;

    addr -= vdev->bar0;

    switch (addr) {
    case VIRTIO_PCI_HOST_FEATURES:
        break;
    case VIRTIO_PCI_GUEST_FEATURES:
        vdev->guest_features = value;
        break; 
    case VIRTIO_PCI_QUEUE_PFN: {
        VirtioQueue2 *vq = &vdev->vq[vdev->queue_sel];

        vq->pfn = value;
        vq->base = vdev->base_ptr + (value << PAGE_SHIFT);

	vq->desc = vq->base;
	vq->avail = vq->base + vq->num * sizeof(struct vring_desc);
	vq->used = (void *)(((unsigned long)&vq->avail->ring[vq->num] + PAGE_SIZE - 1)
			    & ~(PAGE_SIZE - 1));
    }   break;
    case VIRTIO_PCI_QUEUE_NUM:
        break;
    case VIRTIO_PCI_QUEUE_SEL:
        vdev->queue_sel = MAX(value, vdev->max_vq - 1);
        break;
    case VIRTIO_PCI_QUEUE_NOTIFY:
        break;
    case VIRTIO_PCI_STATUS:
        vdev->status = value;
        if ((vdev->status & VIRTIO_CONFIG_S_DRIVER_OK)) {
            int i;

            vdev->event_fd = eventfd(0, 0);

            qemu_set_fd_handler(vdev->event_fd, virtio_process_eventfd, NULL, vdev);

            for (i = 0; i < vdev->max_vq; i++) {
                pthread_t tid;
                pthread_create(&tid, NULL, virtio_thread, &vdev->vq[i]);
            }
        }
        break;
    case VIRTIO_PCI_ISR:
        break;
    default:
        break;
    }

    dprintf("config_write(%p, %s, %d, 0x%x)", vdev, register_names[addr], size, valubbe);
}

static void virtio_config_writeb(void *opaque, uint32_t addr, uint32_t value)
{
    virtio_config_write(opaque, addr, 1, value);
}

static void virtio_config_writew(void *opaque, uint32_t addr, uint32_t value)
{
    virtio_config_write(opaque, addr, 2, value);
}

static void virtio_config_writel(void *opaque, uint32_t addr, uint32_t value)
{
    virtio_config_write(opaque, addr, 4, value);
}

static uint32_t virtio_config_readb(void *opaque, uint32_t addr)
{
    return virtio_config_read(opaque, addr, 1);
}

static uint32_t virtio_config_readw(void *opaque, uint32_t addr)
{
    return virtio_config_read(opaque, addr, 2);
}

static uint32_t virtio_config_readl(void *opaque, uint32_t addr)
{
    return virtio_config_read(opaque, addr, 4);
}

static void virtio_map(PCIDevice *pci_dev, int region_num,
                       pcibus_t addr, pcibus_t size, int type)
{
    VirtioDevice2 *vdev = to_vdev(pci_dev);
    unsigned config_len = 32;

    vdev->bar0 = addr;

    register_ioport_write(addr, config_len, 1, virtio_config_writeb, vdev);
    register_ioport_write(addr, config_len, 2, virtio_config_writew, vdev);
    register_ioport_write(addr, config_len, 4, virtio_config_writel, vdev);
    register_ioport_read(addr, config_len, 1, virtio_config_readb, vdev);
    register_ioport_read(addr, config_len, 2, virtio_config_readw, vdev);
    register_ioport_read(addr, config_len, 4, virtio_config_readl, vdev);
}

static void virtio_add_queue_sync(VirtioDevice2 *vdev, unsigned index, unsigned num,
                                  virtio_io_handler_t *handler, virtio_io_waiter_t *waiter)
{
    vdev->vq[index].num = num;
    vdev->vq[index].handler = handler;
    vdev->vq[index].waiter = waiter;
    vdev->max_vq = MAX(vdev->max_vq, index);
}

static int virtio_init(PCIDevice *pci_dev)
{
    VirtioDevice2 *vdev = to_vdev(pci_dev);
    unsigned size;

    pci_config_set_vendor_id(pci_dev->config, PCI_VENDOR_ID_REDHAT_QUMRANET);
    pci_config_set_device_id(pci_dev->config, PCI_DEVICE_ID_VIRTIO_NET);
    pci_config_set_revision(pci_dev->config, VIRTIO_PCI_ABI_VERSION);
    pci_config_set_class(pci_dev->config, PCI_CLASS_NETWORK_ETHERNET);

    pci_set_word(pci_dev->config + 0x2c, pci_get_word(pci_dev->config + PCI_VENDOR_ID));
    pci_set_word(pci_dev->config + 0x2e, 1);
    pci_dev->config[0x3d] = 1;

    vdev->base_ptr = qemu_get_ram_ptr(0); // Going to hell for this...

    size = 32;
    if (size & (size - 1)) {
        size = 1 << qemu_fls(size);
    }

    pci_register_bar(&vdev->pci_dev, 0, size, PCI_BASE_ADDRESS_SPACE_IO, virtio_map);

    return 0;
}

/* virtio-net */

typedef struct VirtioNet2
{
    VirtioDevice2 vdev;

    char *ifname;
    int fd;
} VirtioNet2;

static VirtioNet2 *to_vnet(VirtioDevice2 *vdev)
{
    return container_of(vdev, VirtioNet2, vdev);
}

static size_t virtio_net_tx_handler(VirtioDevice2 *vdev, struct iovec *sg, int iovcnt)
{
    VirtioNet2 *n = to_vnet(vdev);
    return writev(n->fd, &sg[1], iovcnt - 1);
}

static void virtio_net_tx_waiter(VirtioDevice2 *vdev)
{
    VirtioNet2 *n = to_vnet(vdev);
    fd_wait_write(n->fd);
}

static size_t virtio_net_rx_handler(VirtioDevice2 *vdev, struct iovec *sg, int iovcnt)
{
    VirtioNet2 *n = to_vnet(vdev);
    struct virtio_net_hdr *hdr;

    hdr = sg[0].iov_base;
    hdr->flags = 0;

    return readv(n->fd, &sg[1], iovcnt - 1);
}

static void virtio_net_rx_waiter(VirtioDevice2 *vdev)
{
    VirtioNet2 *n = to_vnet(vdev);
    fd_wait_read(n->fd);
}

static int virtio_net_init(VirtioDevice2 *vdev)
{
    VirtioNet2 *n = to_vnet(vdev);
    struct ifreq ifr;
    int ret;

    virtio_add_queue_sync(vdev, 0, 256, virtio_net_rx_handler, virtio_net_rx_waiter);
    virtio_add_queue_sync(vdev, 1, 256, virtio_net_tx_handler, virtio_net_tx_waiter);

    n->fd = open("/dev/net/tun", O_RDWR);
    assert(n->fd != -1);

    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;

    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", n->ifname);

    ret = ioctl(n->fd, TUNSETIFF, &ifr);
    assert(ret != -1);

    fcntl(n->fd, F_SETFL, O_NONBLOCK);

    return 0;
}

static int virtio_net_initpci(PCIDevice *pci_dev)
{
    VirtioDevice2 *vdev = to_vdev(pci_dev);
    int ret;

    ret = virtio_init(pci_dev);
    if (ret) {
        return ret;
    }

    return virtio_net_init(vdev);
}

static PCIDeviceInfo virtio_info = {
    .qdev.name = "virtio-net2",
    .qdev.size = sizeof(VirtioNet2),
    .init = virtio_net_initpci,
    .qdev.props = (Property[]) {
        DEFINE_PROP_STRING("ifname", VirtioNet2, ifname),
        DEFINE_PROP_END_OF_LIST(),
    },
};    

static void virtio_net_register_devices(void)
{
    pci_qdev_register(&virtio_info);
}

device_init(virtio_net_register_devices);
