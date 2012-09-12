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
    int fd;
    GMutex *lock;
    size_t size;
    void *data;
    gboolean quit;
    GThread *tid;
} BlockDevice;

#define NBD_REPLY_MAGIC         0x67446698

static gboolean nbd_accept(BlockDevice *bd, int *c)
{
    struct sockaddr_un addr;
    socklen_t addrlen = sizeof(addr);
    char buf[8 + 8 + 8 + 128];
    ssize_t ret;
    int fd;

    if (bd->quit) {
        return FALSE;
    }

    fd = *c = accept(bd->fd, &addr, &addrlen);
    g_assert_cmpint(fd, !=, -1);

    memcpy(buf, "NBDMAGIC", 8);
    cpu_to_be64w((uint64_t *)(buf + 8), 0x00420281861253LL);
    cpu_to_be64w((uint64_t *)(buf + 16), bd->size);
    memset(buf + 24, 0, 128);

    ret = write(fd, buf, sizeof(buf));
    g_assert_cmpint(ret, ==, sizeof(buf));

    return TRUE;
}

static gboolean nbd_server_handle_request(BlockDevice *bd, int *c)
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

    switch (type) {
    case 0: {
        uint8_t *data;

        g_assert_cmpint(from + len, <=, bd->size);
        data = g_malloc(len);

        g_mutex_lock(bd->lock);
        memcpy(data, bd->data + from, len);
        g_mutex_unlock(bd->lock);

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
    case 1: {
        uint8_t *data;

        g_assert_cmpint(from + len, <=, bd->size);
        data = g_malloc(len);

        ret = read(fd, data, len);
        g_assert_cmpint(ret, ==, len);

        g_mutex_lock(bd->lock);
        memcpy(bd->data + from, data, len);
        g_mutex_unlock(bd->lock);

        cpu_to_be32wu((uint32_t *)(buf + 0), NBD_REPLY_MAGIC);
        cpu_to_be32wu((uint32_t *)(buf + 4), 0);
        cpu_to_be64wu((uint64_t *)(buf + 8), handle);
        ret = write(fd, buf, 16);
        g_assert_cmpint(ret, ==, 16);

        g_free(data);
        break;           
    }
    case 2: {
        close(fd);
        *c = -1;
        if (!nbd_accept(bd, c)) {
            return FALSE;
        }
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
    BlockDevice *bd = user_data;
    int c = -1;

    if (!nbd_accept(bd, &c)) {
        return NULL;
    }

    while (!bd->quit && nbd_server_handle_request(bd, &c)) {
        /* do nothing */
    }

    if (c != -1) {
        close(c);
    }

    return NULL;
}

static char *block_device_init(BlockDevice *bd, const char *name)
{
    const char *tmp_sock = "/tmp/foo.sock";
    struct sockaddr_un addr;
    int ret;

    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path),
             "%s", tmp_sock);

    bd->fd = socket(PF_UNIX, SOCK_STREAM, 0);
    g_assert_cmpint(bd->fd, !=, -1);

    unlink(tmp_sock);

    ret = bind(bd->fd, (struct sockaddr *)&addr, sizeof(addr));
    g_assert_cmpint(ret, !=, -1);

    ret = listen(bd->fd, 1);
    g_assert_cmpint(ret, !=, -1);

    bd->size = (32 << 20); /* 32MB */
    bd->data = g_malloc0(bd->size);
    bd->lock = g_mutex_new();

    bd->tid = g_thread_create(nbd_server_thread, bd, TRUE, NULL);

    return g_strdup_printf("-drive file=nbd:unix:%s,if=none,id=%s",
                           tmp_sock, name);
}

static void block_device_shutdown(BlockDevice *bd)
{
    bd->quit = true;
}

static void block_device_cleanup(BlockDevice *bd)
{
    g_thread_join(bd->tid);
    g_free(bd->data);
    g_mutex_free(bd->lock);
}

static QPCIBus *pci_bus;
static QGuestAllocator *ga;
static QPCIDevice *dev;
static void *bar0;

static QTestState *init(BlockDevice *bd)
{
    QTestState *qs;
    char *block_info;
    gchar *cmdline;

    block_info = block_device_init(bd, "hd0");

    cmdline = g_strdup_printf("-display none "
                              "-device virtio-blk-pci,drive=hd0,addr=04.0 "
                              "%s ",
                              block_info);
    g_free(block_info);

    qs = qtest_start(cmdline);
    g_free(cmdline);

    pci_bus = qpci_init_pc();
    ga = pc_alloc_init();

    dev = qpci_device_find(pci_bus, QPCI_DEVFN(4, 0));
    g_assert(dev != NULL);

    bar0 = qpci_iomap(dev, 0);
    qpci_device_enable(dev);

    return qs;
}

static void cleanup(QTestState *qs, BlockDevice *bd)
{
    block_device_shutdown(bd);

    if (qs) {
        qtest_quit(qs);
    }

    block_device_cleanup(bd);
}

static void test_virtio_blk_pci_features(void)
{
    QTestState *qs;
    BlockDevice bd = {};
    uint32_t host_features;

    qs = init(&bd);

    host_features = qpci_io_readl(dev, bar0);
    g_assert(host_features & (1 << VIRTIO_BLK_F_SEG_MAX));
    g_assert(host_features & (1 << VIRTIO_BLK_F_GEOMETRY));
    g_assert(!(host_features & (1 << VIRTIO_BLK_F_RO)));
    g_assert(host_features & (1 << VIRTIO_BLK_F_BLK_SIZE));
    g_assert(host_features & (1 << VIRTIO_BLK_F_SCSI));
    g_assert(host_features & (1 << VIRTIO_BLK_F_WCE));
    g_assert(host_features & (1 << VIRTIO_BLK_F_TOPOLOGY));
    g_assert(!(host_features & (1 << VIRTIO_BLK_F_CONFIG_WCE)));

    cleanup(qs, &bd);
}

static void test_virtio_blk_pci_enum(void)
{
    QTestState *qs;
    BlockDevice bd = {};

    qs = init(&bd);

    qpci_io_writew(dev, bar0 + 14, 0);
    g_assert_cmpint(qpci_io_readw(dev, bar0 + 12), ==, 128);

    cleanup(qs, &bd);
}

int main(int argc, char **argv)
{
    g_thread_init(NULL);

    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/virtio-blk-pci/features", test_virtio_blk_pci_features);
    g_test_add_func("/virtio-blk-pci/enum", test_virtio_blk_pci_enum);

    return g_test_run();
}
