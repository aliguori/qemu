#define NO_QEMU_PROTOS

#include "libqtest.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdio.h>

#include "libqos/pci-pc.h"
#include "libqos/malloc-pc.h"

#include <glib.h>

#include "bswap.h"

#include "hw/virtio-blk.h"

typedef struct BlockDevice
{
    GMutex *lock;
    size_t size;
    void *data;
} BlockDevice;

#define NBD_REPLY_MAGIC         0x67446698

static void block_device_init(BlockDevice *bd)
{
    bd->size = (32 << 20); /* 32MB */
    bd->data = g_malloc0(bd->size);
    bd->lock = g_mutex_new();
}

typedef struct NBDServer
{
    int fd;
    BlockDevice *bd;
} NBDServer;

static bool do_exit;

static void nbd_accept(NBDServer *srv, int *c)
{
    struct sockaddr_un addr;
    socklen_t addrlen = sizeof(addr);
    char buf[8 + 8 + 8 + 128];
    ssize_t ret;
    int fd;

    if (do_exit) {
        g_thread_exit(NULL);
    }

    fd = *c = accept(srv->fd, &addr, &addrlen);
    g_assert_cmpint(fd, !=, -1);

    printf("Got connection from QEMU!\n");

    memcpy(buf, "NBDMAGIC", 8);
    cpu_to_be64w((uint64_t *)(buf + 8), 0x00420281861253LL);
    cpu_to_be64w((uint64_t *)(buf + 16), srv->bd->size);
    memset(buf + 24, 0, 128);

    ret = write(fd, buf, sizeof(buf));
    g_assert_cmpint(ret, ==, sizeof(buf));
}

static gboolean nbd_server_handle_request(NBDServer *srv, int *c)
{
    uint8_t buf[4 + 4 + 8 + 8 + 4];
    uint32_t type;
    uint64_t handle;
    uint64_t from;
    uint32_t len;
    ssize_t ret;
    int fd = *c;

    ret = read(fd, buf, sizeof(buf));
    g_assert_cmpint(ret, ==, sizeof(buf));

    type = be32_to_cpup((uint32_t*)(buf + 4));
    handle = be64_to_cpup((uint64_t*)(buf + 8));
    from = be64_to_cpup((uint64_t*)(buf + 16));
    len = be32_to_cpup((uint32_t*)(buf + 24));

    printf("got %d %ld %ld %d\n", type, handle, from, len);

    switch (type) {
    case 0: {
        uint8_t *data;

        g_assert_cmpint(from + len, <=, srv->bd->size);
        data = g_malloc(len);

        g_mutex_lock(srv->bd->lock);
        memcpy(data, srv->bd->data + from, len);
        g_mutex_unlock(srv->bd->lock);

        cpu_to_be32wu((uint32_t *)(buf + 0), NBD_REPLY_MAGIC);
        cpu_to_be32wu((uint32_t *)(buf + 4), 0);
        cpu_to_be64wu((uint64_t *)(buf + 8), handle);
        ret = write(fd, buf, 16);
        g_assert_cmpint(ret, ==, 16);

        ret = write(fd, data, len);
        g_assert_cmpint(ret, ==, len);
        g_free(data);
        break;           
    }
    case 2: {
        close(fd);
        nbd_accept(srv, c);
        fd = *c;
        break;
    }
    default:
        g_assert_not_reached();
        break;
    }

    return TRUE;
}

static gpointer nbd_server_thread(gpointer user_data)
{
    NBDServer *srv = user_data;
    int c;

    printf("Waiting for connection from QEMU\n");

    nbd_accept(srv, &c);

    while (nbd_server_handle_request(srv, &c)) {
    }

    return NULL;
}

static QPCIBus *pci_bus;
static QGuestAllocator *ga;
static QPCIDevice *dev;
static void *bar0;

static void test_virtio_blk_pci_features(void)
{
    uint32_t host_features;

    host_features = qpci_io_readl(dev, bar0);
    g_assert(host_features & (1 << VIRTIO_BLK_F_SEG_MAX));
    g_assert(host_features & (1 << VIRTIO_BLK_F_GEOMETRY));
    g_assert(!(host_features & (1 << VIRTIO_BLK_F_RO)));
    g_assert(host_features & (1 << VIRTIO_BLK_F_BLK_SIZE));
    g_assert(host_features & (1 << VIRTIO_BLK_F_SCSI));
    g_assert(host_features & (1 << VIRTIO_BLK_F_WCE));
    g_assert(host_features & (1 << VIRTIO_BLK_F_TOPOLOGY));
    g_assert(host_features & (1 << VIRTIO_BLK_F_CONFIG_WCE));
}

static void test_virtio_blk_pci_enum(void)
{
    qpci_io_writew(dev, bar0 + 14, 0);
    g_assert_cmpint(qpci_io_readw(dev, bar0 + 12), ==, 128);
}

int main(int argc, char **argv)
{
    struct sockaddr_un addr;
    const char *tmp_sock = "/tmp/foo.sock";
    int s, ret;
    BlockDevice bd;
    NBDServer nbd;
    gchar *cmdline;
    GThread *tid;
    QTestState *qs;

    g_thread_init(NULL);

    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path),
             "%s", tmp_sock);

    s = socket(PF_UNIX, SOCK_STREAM, 0);
    g_assert_cmpint(s, !=, -1);

    unlink(tmp_sock);

    ret = bind(s, (struct sockaddr *)&addr, sizeof(addr));
    g_assert_cmpint(ret, !=, -1);

    ret = listen(s, 1);
    g_assert_cmpint(ret, !=, -1);

    g_test_init(&argc, &argv, NULL);

    block_device_init(&bd);

    nbd.fd = s;
    nbd.bd = &bd;

    tid = g_thread_create(nbd_server_thread, &nbd, TRUE, NULL);

    cmdline = g_strdup_printf("-display none "
                              "-drive file=nbd:unix:%s,if=none,id=hd0 "
                              "-device virtio-blk-pci,drive=hd0,addr=04.0 ",
                              tmp_sock);
    qs = qtest_start(cmdline);
    g_free(cmdline);

    printf("Go!\n");

    pci_bus = qpci_init_pc();
    ga = pc_alloc_init();

    dev = qpci_device_find(pci_bus, QPCI_DEVFN(4, 0));
    g_assert(dev != NULL);

    bar0 = qpci_iomap(dev, 0);
    qpci_device_enable(dev);

    g_test_add_func("/virtio-blk-pci/features", test_virtio_blk_pci_features);
    g_test_add_func("/virtio-blk-pci/enum", test_virtio_blk_pci_enum);

    ret = g_test_run();

    do_exit = true;

    if (qs) {
        qtest_quit(qs);
    }

    g_thread_join(tid);

    return 0;
}
