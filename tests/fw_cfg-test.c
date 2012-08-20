#define NO_QEMU_PROTOS

#include "libqtest.h"
#include "hw/fw_cfg.h"

#include <string.h>
#include <glib.h>

typedef struct QFWCFG QFWCFG;

struct QFWCFG
{
    void (*select)(QFWCFG *fw_cfg, uint16_t key);
    void (*read)(QFWCFG *fw_cfg, void *data, size_t len);
};

/* PC specific */

static void pc_fw_cfg_select(QFWCFG *fw_cfg, uint16_t key)
{
    outw(0x510, key);
}

static void pc_fw_cfg_read(QFWCFG *fw_cfg, void *data, size_t len)
{
    uint8_t *ptr = data;
    int i;

    for (i = 0; i < len; i++) {
        ptr[i] = inb(0x511);
    }
}

static QFWCFG *pc_fw_cfg_init(void)
{
    QFWCFG *fw_cfg = g_malloc0(sizeof(*fw_cfg));

    fw_cfg->select = pc_fw_cfg_select;
    fw_cfg->read = pc_fw_cfg_read;

    return fw_cfg;
}

/* Generic code */

static void qfw_cfg_select(QFWCFG *fw_cfg, uint16_t key)
{
    fw_cfg->select(fw_cfg, key);
}

static void qfw_cfg_read_data(QFWCFG *fw_cfg, void *data, size_t len)
{
    fw_cfg->read(fw_cfg, data, len);
}

static void qfw_cfg_get(QFWCFG *fw_cfg, uint16_t key, void *data, size_t len)
{
    qfw_cfg_select(fw_cfg, key);
    qfw_cfg_read_data(fw_cfg, data, len);
}

static uint16_t qfw_cfg_get_u16(QFWCFG *fw_cfg, uint16_t key)
{
    uint16_t value;
    qfw_cfg_get(fw_cfg, key, &value, sizeof(value));
    return value;
}

static uint32_t qfw_cfg_get_u32(QFWCFG *fw_cfg, uint16_t key)
{
    uint32_t value;
    qfw_cfg_get(fw_cfg, key, &value, sizeof(value));
    return value;
}

static uint64_t qfw_cfg_get_u64(QFWCFG *fw_cfg, uint16_t key)
{
    uint64_t value;
    qfw_cfg_get(fw_cfg, key, &value, sizeof(value));
    return value;
}

static uint64_t ram_size = 128 << 20;
static uint16_t nb_cpus = 1;
static uint16_t max_cpus = 1;
static uint64_t nb_nodes = 0;
static uint16_t boot_menu = 0;
static QFWCFG *fw_cfg = NULL;

static void test_fw_cfg_signature(void)
{
    char buf[5];

    qfw_cfg_get(fw_cfg, FW_CFG_SIGNATURE, buf, 4);
    buf[4] = 0;

    g_assert_cmpstr(buf, ==, "QEMU");
}

static void test_fw_cfg_id(void)
{
    g_assert_cmpint(qfw_cfg_get_u32(fw_cfg, FW_CFG_ID), ==, 1);
}

static void test_fw_cfg_uuid(void)
{
    uint8_t buf[16];
    static const uint8_t uuid[16] = {
        0x46, 0x00, 0xcb, 0x32, 0x38, 0xec, 0x4b, 0x2f,
        0x8a, 0xcb, 0x81, 0xc6, 0xea, 0x54, 0xf2, 0xd8,
    };

    qfw_cfg_get(fw_cfg, FW_CFG_UUID, buf, 16);
    g_assert(memcmp(buf, uuid, sizeof(buf)) == 0);
}

static void test_fw_cfg_ram_size(void)
{
    g_assert_cmpint(qfw_cfg_get_u64(fw_cfg, FW_CFG_RAM_SIZE), ==, ram_size);
}

static void test_fw_cfg_nographic(void)
{
    g_assert_cmpint(qfw_cfg_get_u16(fw_cfg, FW_CFG_NOGRAPHIC), ==, 0);
}

static void test_fw_cfg_nb_cpus(void)
{
    g_assert_cmpint(qfw_cfg_get_u16(fw_cfg, FW_CFG_NB_CPUS), ==, nb_cpus);
}

static void test_fw_cfg_max_cpus(void)
{
    g_assert_cmpint(qfw_cfg_get_u16(fw_cfg, FW_CFG_MAX_CPUS), ==, max_cpus);
}

static void test_fw_cfg_numa(void)
{
    uint64_t *cpu_mask;
    uint64_t *node_mask;

    g_assert_cmpint(qfw_cfg_get_u64(fw_cfg, FW_CFG_NUMA), ==, nb_nodes);

    cpu_mask = g_malloc0(sizeof(uint64_t) * max_cpus);
    node_mask = g_malloc0(sizeof(uint64_t) * nb_nodes);

    qfw_cfg_read_data(fw_cfg, cpu_mask, sizeof(uint64_t) * max_cpus);
    qfw_cfg_read_data(fw_cfg, node_mask, sizeof(uint64_t) * nb_nodes);

    if (nb_nodes) {
        g_assert(cpu_mask[0] & 0x01);
        g_assert_cmpint(node_mask[0], ==, ram_size);
    }

    g_free(node_mask);
    g_free(cpu_mask);
}

static void test_fw_cfg_boot_menu(void)
{
    g_assert_cmpint(qfw_cfg_get_u16(fw_cfg, FW_CFG_BOOT_MENU), ==, boot_menu);
}

int main(int argc, char **argv)
{
    QTestState *s = NULL;
    char *cmdline;
    int ret;

    g_test_init(&argc, &argv, NULL);

    fw_cfg = pc_fw_cfg_init();

    g_test_add_func("/fw_cfg/signature", test_fw_cfg_signature);
    g_test_add_func("/fw_cfg/id", test_fw_cfg_id);
    g_test_add_func("/fw_cfg/uuid", test_fw_cfg_uuid);
    g_test_add_func("/fw_cfg/ram_size", test_fw_cfg_ram_size);
    g_test_add_func("/fw_cfg/nographic", test_fw_cfg_nographic);
    g_test_add_func("/fw_cfg/nb_cpus", test_fw_cfg_nb_cpus);
#if 0
    g_test_add_func("/fw_cfg/machine_id", test_fw_cfg_machine_id);
    g_test_add_func("/fw_cfg/kernel", test_fw_cfg_kernel);
    g_test_add_func("/fw_cfg/initrd", test_fw_cfg_initrd);
    g_test_add_func("/fw_cfg/boot_device", test_fw_cfg_boot_device);
#endif
    g_test_add_func("/fw_cfg/max_cpus", test_fw_cfg_max_cpus);
    g_test_add_func("/fw_cfg/numa", test_fw_cfg_numa);
    g_test_add_func("/fw_cfg/boot_menu", test_fw_cfg_boot_menu);

    cmdline = g_strdup_printf("-display none "
                              "-uuid 4600cb32-38ec-4b2f-8acb-81c6ea54f2d8 ");
    qtest_start(cmdline);
    g_free(cmdline);

    ret = g_test_run();

    if (s) {
        qtest_quit(s);
    }

    return ret;
}
